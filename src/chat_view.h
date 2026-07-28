/* GtkHx — chat view abstraction (chat-view phase C0)
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
 * The chat output surface, as an interface rather than a widget.
 *
 * Every chat / private-chat / private-message window renders its
 * output through this header. Today the one and only implementation is
 * the vendored xtext widget (chat_view.c is a thin forwarding layer);
 * the point of the indirection is that xtext's internals stop being
 * part of the call sites' vocabulary.
 *
 * Two things this closes off, both of which were blocking further work
 * (see docs/chat-view-scoping.md §1.3):
 *
 *   1. Direct struct-field access. chat.c / msg.c / options.c used to
 *      write GTK_XTEXT(w)->wordwrap, ->max_lines, ->urlcheck_function
 *      and read ->buffer and ->adj. Those are hx_chat_view_set_* /
 *      _get_* calls now, so a different backend can honour them
 *      however it likes.
 *
 *   2. Raw textentry pointers. The chat-history render cursors in
 *      struct hx_chat_history_render were live textentry* into xtext's
 *      internal linked list — which is precisely why that struct
 *      "stays C" in the Phase R5 chat-model re-think. They are opaque
 *      HxChatMark handles here. Under xtext a mark IS the textentry
 *      pointer (no allocation, just a type the caller can't
 *      dereference); a future backend can make it a message id.
 *
 * Marks stay valid until the entry they name is removed — either
 * explicitly via hx_chat_view_remove, or implicitly by the scrollback
 * trim (hx_chat_view_set_max_lines) or a clear. Callers must treat
 * them as weak references: hx_chat_view_remove on a stale mark is a
 * safe no-op that returns FALSE, and that is the intended way to find
 * out. Do not hold a mark across a clear.
 *
 * Not abstracted in C0, deliberately: the "word_click" signal and its
 * urlcheck-function companion. Both are xtext-shaped (a click yields a
 * whitespace-delimited word, and callers demux by string prefix), and
 * replacing them with typed signals is a semantic change, not a
 * mechanical one — it belongs with the new widget in C2/C3, not here.
 */

#ifndef GTKHX_CHAT_VIEW_H
#define GTKHX_CHAT_VIEW_H

#include <gtk/gtk.h>
#include <time.h>

G_BEGIN_DECLS

/* ---- palette vocabulary ------------------------------------------ *
 *
 * Slots 0..31 are the mIRC colour indices addressed by an in-band
 * "\003NN" escape. Slots 32..37 are UI roles the GtkhxTheme palette
 * fills (see gtkhx_theme.h's matching GTKHX_PAL_* enum and
 * chat.c::gtkhx_apply_theme_palette).
 *
 * Worth being precise about, because the tree has claimed otherwise in
 * a few comments: the mIRC slots are NOT protocol-shaped. The Hotline
 * wire format has no text-styling concept at all — no colour field, no
 * style field, chat / message / news bodies are plain text. The escape
 * vocabulary arrived with the XChat xtext fork in 2000 and every
 * "\003NN" byte in a GtkHx buffer was written by GtkHx itself (nick
 * brackets, highlight, the info prefix, history-muted rows, media
 * placeholders). Servers never send them, and hotline-proto's
 * strip_ansi would fold most of them anyway. Hotline's actual per-user
 * colour is a separate u32 RGB attribute on the user record, not
 * in-band markup.
 *
 * That makes the whole vocabulary ours to retire — see
 * docs/chat-view-scoping.md §3.8.
 *
 * These mirror xtext.h's XTEXT_* constants and chat_view.c static-
 * asserts that they agree. When xtext goes away the assertions go with
 * it and these become the sole definition. */
#define HX_CHAT_PAL_MIRC_COLS    32
#define HX_CHAT_PAL_MARK_FG      32 /* selection foreground */
#define HX_CHAT_PAL_MARK_BG      33 /* selection background */
#define HX_CHAT_PAL_FG           34 /* default text foreground */
#define HX_CHAT_PAL_BG           35 /* default text background */
#define HX_CHAT_PAL_MARKER       36 /* marker line */
#define HX_CHAT_PAL_HISTORY_MUTED 37 /* rendered chat-history secondary text */
#define HX_CHAT_PAL_COLS         38 /* 32 mIRC + 6 UI roles */

/* ---- marks -------------------------------------------------------- *
 *
 * An opaque handle to one appended row. Never dereferenced by callers;
 * the incomplete type makes that a compile error rather than a
 * convention. */
typedef struct _HxChatMark HxChatMark;

/* ---- construction / configuration --------------------------------- */

/* Create a chat output view seeded with `palette` (HX_CHAT_PAL_COLS
 * entries). `separator` draws the indent-column separator line. */
GtkWidget *hx_chat_view_new (const GdkRGBA palette[], gboolean separator);

void hx_chat_view_set_font (GtkWidget *view, const char *font);
void hx_chat_view_set_palette (GtkWidget *view, const GdkRGBA palette[]);
void hx_chat_view_set_word_wrap (GtkWidget *view, gboolean word_wrap);
void hx_chat_view_set_max_lines (GtkWidget *view, int max_lines);
/* Two-column layout: a left gutter (timestamp) + nick column, with the
 * body indented past it. The gutter grows to fit the widest nick seen,
 * capped by hx_chat_view_set_max_indent. */
void hx_chat_view_set_indent (GtkWidget *view, gboolean indent);
void hx_chat_view_set_max_indent (GtkWidget *view, int max_indent_px);
void hx_chat_view_set_time_stamp (GtkWidget *view, gboolean time_stamp);
/* strftime(3) format for the per-line timestamp column. NULL or empty
 * `format` restores the built-in default; the implementation copies the
 * string. The format is process-wide, so a NULL `view` is legal and
 * means "set the format, skip the per-view column-width recompute" —
 * which is what prefs_read needs, since it runs before any chat window
 * exists. */
void hx_chat_view_set_stamp_format (GtkWidget *view, const char *format);
/* Classifier the view calls to decide whether a word under the pointer
 * is a link (drives the hand cursor and the word_click routing). */
void hx_chat_view_set_urlcheck_function (
    GtkWidget *view, int (*urlcheck_function) (GtkWidget *view, char *word));

/* The vertical scroll adjustment, for wiring up a GtkScrollbar. */
GtkAdjustment *hx_chat_view_get_vadjustment (GtkWidget *view);

/* Force a full re-render (after a palette, font or pref change). */
void hx_chat_view_refresh (GtkWidget *view);
/* Drop every row. Invalidates every outstanding mark. */
void hx_chat_view_clear (GtkWidget *view);

/* Drag-end auto-clipboard behaviour. Process-wide, not per-view —
 * these mirror three BOOLEAN cfgvars in Settings.
 *   text  — copy to the clipboard on drag-end at all
 *   stamp — include the per-line timestamp in the copied text
 *   color — retain in-band colour codes in the copied text */
void hx_chat_view_set_autocopy_text (gboolean enabled);
void hx_chat_view_set_autocopy_stamp (gboolean enabled);
void hx_chat_view_set_autocopy_color (gboolean enabled);

/* ---- appending ---------------------------------------------------- *
 *
 * `stamp` is a time_t for the row's timestamp column; 0 means "now".
 * All text is in the in-band escape vocabulary (mIRC "\003NN" colours
 * plus the ATTR_* attribute bytes) — that stays the wire format
 * between chat.c's formatting code and the view for now; C6 replaces
 * it with a structured message. */

/* Append a plain row with no nick column (server prose, continuation
 * lines of a multi-line message). */
void hx_chat_view_append (GtkWidget *view, const char *text, int len,
                          time_t stamp);

/* Append a two-column row: `left` in the nick column, `right` as the
 * body. Returns a mark naming the appended row. */
HxChatMark *hx_chat_view_append_indent (GtkWidget *view, const char *left,
                                        int left_len, const char *right,
                                        int right_len, time_t stamp);

/* Insert a two-column row immediately BEFORE `anchor`. A NULL anchor
 * prepends at the head. Returns a mark naming the inserted row.
 *
 * The scroll position is preserved across the insert: if the new row
 * lands above the viewport the view compensates, so backfilling older
 * content (the chat-history Load-Older path) doesn't move what the
 * user is reading. Call once per row in chronological order — each
 * insert lands directly before the anchor, so the last row inserted
 * ends up closest to it. */
HxChatMark *hx_chat_view_insert_before (GtkWidget *view, HxChatMark *anchor,
                                        const char *left, int left_len,
                                        const char *right, int right_len,
                                        time_t stamp);

/* Remove the row `mark` names. Returns TRUE if it was found and
 * removed, FALSE if the mark was already stale (trimmed or cleared) —
 * which is not an error. The caller should drop the mark either way. */
gboolean hx_chat_view_remove (GtkWidget *view, HxChatMark *mark);

/* ---- inline media -------------------------------------------------- *
 *
 * A media row renders `alt_text` as ordinary styled text until a
 * texture lands, then paints the image in its place. `media_token` is
 * the per-conversation token the click handler uses to route back to
 * the conversation's media table; pass 0 for an untracked row. */
void hx_chat_view_append_media (GtkWidget *view, GdkTexture *texture,
                                const char *alt_text, guint media_token,
                                time_t stamp);

/* Look a media row up by its token. Returns NULL if the row is gone
 * (trimmed or cleared mid-fetch) — which is why the async decode path
 * holds a token rather than a mark. */
HxChatMark *hx_chat_view_media_mark (GtkWidget *view, guint media_token);

/* Swap in (or, with NULL, clear back to alt text) the texture on a
 * media row. */
void hx_chat_view_media_set_texture (GtkWidget *view, HxChatMark *mark,
                                     GdkTexture *texture);

/* Install an animation on a media row. `frames` is a GArray of
 * HxInlineMediaFrame (see inline_media_decode.h); the view takes a ref
 * and drives the per-frame tick. NULL or empty clears the animation. */
void hx_chat_view_media_set_animation (GtkWidget *view, HxChatMark *mark,
                                       GArray *frames);

G_END_DECLS

#endif /* GTKHX_CHAT_VIEW_H */
