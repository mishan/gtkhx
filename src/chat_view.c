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
 * The one implementation of chat_view.h: a forwarding layer onto the
 * vendored xtext widget.
 *
 * This file is the ONLY place outside xtext.c itself that is allowed
 * to name GTK_XTEXT, xtext_buffer or textentry. Everything else in the
 * tree goes through chat_view.h. That invariant is what makes the
 * second backend (docs/chat-view-scoping.md phase C2) a contained
 * change rather than a tree-wide one, and it is cheap to check:
 *
 *     grep -rn 'GTK_XTEXT\|xtext_buffer\|textentry' src/ \
 *         --include=*.c --include=*.h | grep -v '^src/xtext\.\|^src/chat_view\.c'
 *
 * should print nothing.
 *
 * An HxChatMark is a textentry* wearing an incomplete type. No
 * allocation, no lookup table, no lifetime of its own — the cast is
 * the whole implementation. Callers can't dereference it because
 * struct _HxChatMark is never defined, which is exactly the property
 * we wanted from the old raw textentry* fields.
 */

#include <string.h>

#include <gtk/gtk.h>

#include "chat_view.h"
#include "xtext.h"

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
    return gtk_xtext_new ((GdkRGBA *) (const void *) palette,
                          separator ? 1 : 0);
}

void
hx_chat_view_set_font (GtkWidget *view, const char *font)
{
    gtk_xtext_set_font (xt (view), (char *) (const void *) font);
}

void
hx_chat_view_set_palette (GtkWidget *view, const GdkRGBA palette[])
{
    gtk_xtext_set_palette (xt (view),
                           (GdkRGBA *) (const void *) palette);
}

void
hx_chat_view_set_word_wrap (GtkWidget *view, gboolean word_wrap)
{
    gtk_xtext_set_wordwrap (xt (view), word_wrap);
}

void
hx_chat_view_set_max_lines (GtkWidget *view, int max_lines)
{
    gtk_xtext_set_max_lines (xt (view), max_lines);
}

void
hx_chat_view_set_indent (GtkWidget *view, gboolean indent)
{
    gtk_xtext_set_indent (xt (view), indent);
}

void
hx_chat_view_set_max_indent (GtkWidget *view, int max_indent_px)
{
    gtk_xtext_set_max_indent (xt (view), max_indent_px);
}

void
hx_chat_view_set_time_stamp (GtkWidget *view, gboolean time_stamp)
{
    gtk_xtext_set_time_stamp (xbuf (view), time_stamp);
}

void
hx_chat_view_set_stamp_format (GtkWidget *view, const char *format)
{
    /* NULL view is legal here and only here: prefs_read applies the
     * persisted format before any chat window exists. The format itself
     * is process-wide; the per-view work (recompute the stamp column
     * width, re-grow the indent) is what a NULL skips. */
    gtk_xtext_set_stamp_format (view ? xt (view) : NULL, format);
}

void
hx_chat_view_set_urlcheck_function (
    GtkWidget *view, int (*urlcheck_function) (GtkWidget *view, char *word))
{
    gtk_xtext_set_urlcheck_function (xt (view), urlcheck_function);
}

GtkAdjustment *
hx_chat_view_get_vadjustment (GtkWidget *view)
{
    return xt (view)->adj;
}

void
hx_chat_view_refresh (GtkWidget *view)
{
    gtk_xtext_refresh (xt (view));
}

void
hx_chat_view_clear (GtkWidget *view)
{
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
    gtk_xtext_append (xbuf (view), bytes (text), len, stamp);
}

HxChatMark *
hx_chat_view_append_indent (GtkWidget *view, const char *left, int left_len,
                            const char *right, int right_len, time_t stamp)
{
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
    return gtk_xtext_remove_entry (xbuf (view), ent_of (mark));
}

/* ---- inline media -------------------------------------------------- */

void
hx_chat_view_append_media (GtkWidget *view, GdkTexture *texture,
                           const char *alt_text, guint media_token,
                           time_t stamp)
{
    gtk_xtext_append_media (xbuf (view), texture, alt_text, media_token,
                            stamp);
}

HxChatMark *
hx_chat_view_media_mark (GtkWidget *view, guint media_token)
{
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
    gtk_xtext_media_set_texture (xbuf (view), ent_of (mark), texture);
}

void
hx_chat_view_media_set_animation (GtkWidget *view, HxChatMark *mark,
                                  GArray *frames)
{
    if (!mark) {
        return;
    }
    gtk_xtext_media_set_animation (xbuf (view), ent_of (mark), frames);
}
