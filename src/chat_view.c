/* GtkHx — chat view abstraction, xtext backend (chat-view phase C0)
 *
 * Copyright (C) 2000-2003 Misha Nasledov
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * chat_view.h's dispatcher, over two backends.
 *
 *   xtext   — the vendored widget. Still the default.
 *   hxchat  — the new Rust widget (rust/crates/hxchat-view), phase C2.
 *
 * This file is the ONLY place outside xtext.c itself that is allowed
 * to name GTK_XTEXT, xtext_buffer or textentry. Everything else in the
 * tree goes through chat_view.h. That invariant is what keeps the
 * second backend a contained change rather than a tree-wide one, and it
 * is cheap to check:
 *
 *     grep -rn 'GTK_XTEXT\|xtext_buffer\|textentry' src/ \
 *         --include=*.c --include=*.h | grep -v '^src/xtext\.\|^src/chat_view\.c'
 *
 * should print nothing.
 *
 * ---- how the two coexist -------------------------------------------
 *
 * The backend is chosen once, at construction, from GTKHX_CHATVIEW.
 * Every later call dispatches on the widget's actual type rather than
 * re-reading the environment, so a single process can hold views of
 * both kinds at once — which is what makes a side-by-side A/B against a
 * live server possible, and is the whole point of the coexistence
 * period (docs/chat-view-scoping.md §5).
 *
 *     GTKHX_CHATVIEW=xtext   (default)  the vendored widget
 *     GTKHX_CHATVIEW=new                the Rust widget
 *
 * ---- marks ----------------------------------------------------------
 *
 * An HxChatMark is an opaque handle whose representation is the
 * backend's business. Under xtext it is a textentry* wearing an
 * incomplete type — no allocation, the cast is the whole
 * implementation. Under hxchat it is a MessageId encoded the way GLib
 * encodes integer handles. Callers can't dereference either, because
 * struct _HxChatMark is never defined; that is exactly the property we
 * wanted from the old raw textentry* fields, and it is why a stale mark
 * is inert rather than dangling.
 */

#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

#include "chat_view.h"
#include "xtext.h"

/* ---- the hxchat backend's C ABI ------------------------------------ *
 *
 * Hand-declared rather than cbindgen-generated, matching the pattern
 * every other Rust crate in the tree uses at its FFI boundary: a
 * signature mismatch then fails at link time rather than silently
 * miscompiling. Defined in rust/crates/hxchat-view/src/ffi.rs. */
extern GType hx_chat_view_impl_get_type (void);
extern GtkWidget *hx_chat_view_impl_new (const GdkRGBA *palette, int separator);
extern void hx_chat_view_impl_set_font (GtkWidget *w, const char *font);
extern void hx_chat_view_impl_set_palette (GtkWidget *w, const GdkRGBA *palette);
extern void hx_chat_view_impl_set_word_wrap (GtkWidget *w, int on);
extern void hx_chat_view_impl_set_max_lines (GtkWidget *w, int n);
extern void hx_chat_view_impl_set_indent (GtkWidget *w, int on);
extern void hx_chat_view_impl_set_max_indent (GtkWidget *w, int px);
extern void hx_chat_view_impl_set_time_stamp (GtkWidget *w, int on);
extern void hx_chat_view_impl_set_stamp_format (GtkWidget *w, const char *fmt);
/* Typed, not void*: casting a function pointer to a data pointer is
 * undefined behaviour in C, so the dispatcher passes it through with its
 * real type. The hxchat backend ignores it until C3 replaces it with a
 * typed link-activated signal. */
extern void hx_chat_view_impl_set_urlcheck_function (
    GtkWidget *w, int (*fn) (GtkWidget *view, char *word));
extern GtkAdjustment *hx_chat_view_impl_get_vadjustment (GtkWidget *w);
extern void hx_chat_view_impl_refresh (GtkWidget *w);
extern void hx_chat_view_impl_clear (GtkWidget *w);
extern void hx_chat_view_impl_append (GtkWidget *w, const char *text, int len,
                                      gint64 stamp);
extern void *hx_chat_view_impl_append_indent (GtkWidget *w, const char *left,
                                              int left_len, const char *right,
                                              int right_len, gint64 stamp);
extern void *hx_chat_view_impl_insert_before (GtkWidget *w, void *anchor,
                                              const char *left, int left_len,
                                              const char *right, int right_len,
                                              gint64 stamp);
extern int hx_chat_view_impl_remove (GtkWidget *w, void *mark);
extern void hx_chat_view_impl_append_media (GtkWidget *w, void *texture,
                                            const char *alt, guint token,
                                            gint64 stamp);
extern void *hx_chat_view_impl_media_mark (GtkWidget *w, guint token);
extern void hx_chat_view_impl_media_set_texture (GtkWidget *w, void *mark,
                                                 void *texture);
extern void hx_chat_view_impl_media_set_animation (GtkWidget *w, void *mark,
                                                   void *frames);

/* TRUE when `view` is the new backend. The single branch every entry
 * point below takes; NULL is treated as xtext so the one legal NULL
 * caller (set_stamp_format from prefs_read) keeps working. */
static inline gboolean
is_hxchat (GtkWidget *view)
{
    /* Cached: this runs on every call into every entry point, and
     * hx_chat_view_impl_get_type crosses the FFI to reach glib's type
     * registry. The GType never changes once registered. */
    static GType t = 0;
    if (G_UNLIKELY (t == 0)) {
        t = hx_chat_view_impl_get_type ();
    }
    return view != NULL && G_TYPE_CHECK_INSTANCE_TYPE (view, t);
}

/* Which backend a newly-created view gets.
 *
 * Read once and cached: flipping this mid-session would give us two
 * kinds of view whose *construction* differed, which is a confusing
 * thing to debug for no benefit. Existing views are unaffected either
 * way, since dispatch is by type. */
static gboolean
want_hxchat (void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = g_getenv ("GTKHX_CHATVIEW");
        cached = (v && (g_ascii_strcasecmp (v, "new") == 0
                        || g_ascii_strcasecmp (v, "hxchat") == 0))
                     ? 1
                     : 0;
        if (cached) {
            g_message ("chat view: using the hxchat backend "
                       "(GTKHX_CHATVIEW=%s)",
                       v);
        }
    }
    return cached != 0;
}

/* The palette vocabulary in chat_view.h has to agree with xtext's, since
 * callers now fill the array using the HX_CHAT_PAL_* names and xtext
 * indexes it with the XTEXT_* ones. When xtext goes away these go with
 * it. */
G_STATIC_ASSERT (HX_CHAT_PAL_COLS == XTEXT_COLS);
G_STATIC_ASSERT (HX_CHAT_PAL_MIRC_COLS == XTEXT_MIRC_COLS);
G_STATIC_ASSERT (HX_CHAT_PAL_MARK_FG == XTEXT_MARK_FG);
G_STATIC_ASSERT (HX_CHAT_PAL_MARK_BG == XTEXT_MARK_BG);
G_STATIC_ASSERT (HX_CHAT_PAL_FG == XTEXT_FG);
G_STATIC_ASSERT (HX_CHAT_PAL_BG == XTEXT_BG);
G_STATIC_ASSERT (HX_CHAT_PAL_MARKER == XTEXT_MARKER);
G_STATIC_ASSERT (HX_CHAT_PAL_HISTORY_MUTED == XTEXT_HISTORY_MUTED);

/* ---- backend plumbing --------------------------------------------- */

static inline GtkXText *
xt (GtkWidget *view)
{
    return GTK_XTEXT (view);
}

static inline xtext_buffer *
xbuf (GtkWidget *view)
{
    return GTK_XTEXT (view)->buffer;
}

static inline HxChatMark *
mark_of (textentry *ent)
{
    return (HxChatMark *) ent;
}

static inline textentry *
ent_of (HxChatMark *mark)
{
    return (textentry *) mark;
}

/* xtext's append/insert entry points take non-const `unsigned char *`
 * and never write through them. Dropping const at this one boundary
 * keeps the public header honest for every caller.
 *
 * A single cast, not a pointer→integer→pointer round trip: going via
 * uintptr_t would launder away the pointer's provenance for no benefit
 * (and would need <stdint.h>, which this file only gets transitively
 * through glib). */
static inline unsigned char *
bytes (const char *s)
{
    return (unsigned char *) (const void *) s;
}

/* ---- construction / configuration --------------------------------- */

GtkWidget *
hx_chat_view_new (const GdkRGBA palette[], gboolean separator)
{
    /* gtk_xtext_new copies the palette into the widget but takes it as
     * a non-const array. */
    if (want_hxchat ()) {
        GtkWidget *v = hx_chat_view_impl_new (palette, separator ? 1 : 0);
        /* Fail loudly rather than limping on.
         *
         * If the backend hands back something that isn't a usable
         * widget, every later call silently takes the xtext branch —
         * because is_hxchat() can only answer "no" — and the first one
         * to dereference it segfaults somewhere in xtext.c, which is a
         * spectacularly misleading place to land. That happened twice
         * during C2 bring-up: once from a use-after-free, once from a
         * botched GtkScrollable property override. Both wasted the
         * backtrace. Check here, where the answer is unambiguous. */
        if (!GTK_IS_WIDGET (v) || !is_hxchat (v)) {
            g_error ("hx_chat_view_impl_new returned %p, which is not an "
                     "HxChatView — the hxchat backend is broken; rerun "
                     "without GTKHX_CHATVIEW=new",
                     (void *) v);
        }
        return v;
    }
    return gtk_xtext_new ((GdkRGBA *) (const void *) palette,
                          separator ? 1 : 0);
}

void
hx_chat_view_set_font (GtkWidget *view, const char *font)
{
    if (is_hxchat (view)) {
        hx_chat_view_impl_set_font (view, font);
        return;
    }
    gtk_xtext_set_font (xt (view), (char *) (const void *) font);
}

void
hx_chat_view_set_palette (GtkWidget *view, const GdkRGBA palette[])
{
    if (is_hxchat (view)) {
        hx_chat_view_impl_set_palette (view, palette);
        return;
    }
    gtk_xtext_set_palette (xt (view),
                           (GdkRGBA *) (const void *) palette);
}

void
hx_chat_view_set_word_wrap (GtkWidget *view, gboolean word_wrap)
{
    if (is_hxchat (view)) {
        hx_chat_view_impl_set_word_wrap (view, word_wrap);
        return;
    }
    gtk_xtext_set_wordwrap (xt (view), word_wrap);
}

void
hx_chat_view_set_max_lines (GtkWidget *view, int max_lines)
{
    if (is_hxchat (view)) {
        hx_chat_view_impl_set_max_lines (view, max_lines);
        return;
    }
    gtk_xtext_set_max_lines (xt (view), max_lines);
}

void
hx_chat_view_set_indent (GtkWidget *view, gboolean indent)
{
    if (is_hxchat (view)) {
        hx_chat_view_impl_set_indent (view, indent);
        return;
    }
    gtk_xtext_set_indent (xt (view), indent);
}

void
hx_chat_view_set_max_indent (GtkWidget *view, int max_indent_px)
{
    if (is_hxchat (view)) {
        hx_chat_view_impl_set_max_indent (view, max_indent_px);
        return;
    }
    gtk_xtext_set_max_indent (xt (view), max_indent_px);
}

void
hx_chat_view_set_time_stamp (GtkWidget *view, gboolean time_stamp)
{
    if (is_hxchat (view)) {
        hx_chat_view_impl_set_time_stamp (view, time_stamp);
        return;
    }
    gtk_xtext_set_time_stamp (xbuf (view), time_stamp);
}

void
hx_chat_view_set_stamp_format (GtkWidget *view, const char *format)
{
    /* NULL view is legal here and only here: prefs_read applies the
     * persisted format before any chat window exists. The format itself
     * is process-wide; the per-view work (recompute the stamp column
     * width, re-grow the indent) is what a NULL skips. */
    if (is_hxchat (view)) {
        hx_chat_view_impl_set_stamp_format (view, format);
        return;
    }
    gtk_xtext_set_stamp_format (view ? xt (view) : NULL, format);
}

void
hx_chat_view_set_urlcheck_function (
    GtkWidget *view, int (*urlcheck_function) (GtkWidget *view, char *word))
{
    if (is_hxchat (view)) {
        hx_chat_view_impl_set_urlcheck_function (view, urlcheck_function);
        return;
    }
    gtk_xtext_set_urlcheck_function (xt (view), urlcheck_function);
}

GtkAdjustment *
hx_chat_view_get_vadjustment (GtkWidget *view)
{
    if (is_hxchat (view)) {
        return hx_chat_view_impl_get_vadjustment (view);
    }
    return xt (view)->adj;
}

void
hx_chat_view_refresh (GtkWidget *view)
{
    if (is_hxchat (view)) {
        hx_chat_view_impl_refresh (view);
        return;
    }
    gtk_xtext_refresh (xt (view));
}

void
hx_chat_view_clear (GtkWidget *view)
{
    if (is_hxchat (view)) {
        hx_chat_view_impl_clear (view);
        return;
    }
    gtk_xtext_clear (xbuf (view), 0);
}

void
hx_chat_view_set_autocopy_text (gboolean enabled)
{
    gtk_xtext_set_autocopy_text (enabled);
}

void
hx_chat_view_set_autocopy_stamp (gboolean enabled)
{
    gtk_xtext_set_autocopy_stamp (enabled);
}

void
hx_chat_view_set_autocopy_color (gboolean enabled)
{
    gtk_xtext_set_autocopy_color (enabled);
}

/* ---- appending ---------------------------------------------------- */

void
hx_chat_view_append (GtkWidget *view, const char *text, int len, time_t stamp)
{
    if (is_hxchat (view)) {
        hx_chat_view_impl_append (view, text, len, (gint64) stamp);
        return;
    }
    gtk_xtext_append (xbuf (view), bytes (text), len, stamp);
}

HxChatMark *
hx_chat_view_append_indent (GtkWidget *view, const char *left, int left_len,
                            const char *right, int right_len, time_t stamp)
{
    if (is_hxchat (view)) {
        return hx_chat_view_impl_append_indent (view, left, left_len, right,
                                                right_len, (gint64) stamp);
    }

    xtext_buffer *buf = xbuf (view);

    gtk_xtext_append_indent (buf, bytes (left), left_len, bytes (right),
                             right_len, stamp);
    /* gtk_xtext_append_indent returns void; the row it just appended is
     * the tail, so the mark is one accessor away. Doing the lookup here
     * rather than making callers pair an append with a separate
     * get_last_entry is the point of the return value. */
    return mark_of (gtk_xtext_get_last_entry (buf));
}

HxChatMark *
hx_chat_view_insert_before (GtkWidget *view, HxChatMark *anchor,
                            const char *left, int left_len, const char *right,
                            int right_len, time_t stamp)
{
    if (is_hxchat (view)) {
        return hx_chat_view_impl_insert_before (view, anchor, left, left_len,
                                                right, right_len,
                                                (gint64) stamp);
    }
    return mark_of (gtk_xtext_insert_indent_before (
        xbuf (view), ent_of (anchor), bytes (left), left_len, bytes (right),
        right_len, stamp));
}

gboolean
hx_chat_view_remove (GtkWidget *view, HxChatMark *mark)
{
    if (!mark) {
        return FALSE;
    }
    if (is_hxchat (view)) {
        return hx_chat_view_impl_remove (view, mark) != 0;
    }
    return gtk_xtext_remove_entry (xbuf (view), ent_of (mark));
}

/* ---- inline media -------------------------------------------------- */

void
hx_chat_view_append_media (GtkWidget *view, GdkTexture *texture,
                           const char *alt_text, guint media_token,
                           time_t stamp)
{
    if (is_hxchat (view)) {
        hx_chat_view_impl_append_media (view, texture, alt_text, media_token,
                                        (gint64) stamp);
        return;
    }
    gtk_xtext_append_media (xbuf (view), texture, alt_text, media_token,
                            stamp);
}

HxChatMark *
hx_chat_view_media_mark (GtkWidget *view, guint media_token)
{
    if (is_hxchat (view)) {
        return hx_chat_view_impl_media_mark (view, media_token);
    }
    return mark_of (
        gtk_xtext_find_media_entry_by_token (xbuf (view), media_token));
}

void
hx_chat_view_media_set_texture (GtkWidget *view, HxChatMark *mark,
                                GdkTexture *texture)
{
    if (!mark) {
        return;
    }
    if (is_hxchat (view)) {
        hx_chat_view_impl_media_set_texture (view, mark, texture);
        return;
    }
    gtk_xtext_media_set_texture (xbuf (view), ent_of (mark), texture);
}

void
hx_chat_view_media_set_animation (GtkWidget *view, HxChatMark *mark,
                                  GArray *frames)
{
    if (!mark) {
        return;
    }
    if (is_hxchat (view)) {
        hx_chat_view_impl_media_set_animation (view, mark, frames);
        return;
    }
    gtk_xtext_media_set_animation (xbuf (view), ent_of (mark), frames);
}
