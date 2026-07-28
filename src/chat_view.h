/* GtkHx — chat view: the C-facing declaration of the chat output widget
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
 * The chat output surface. Every chat / private-chat / private-message
 * window renders through this header.
 *
 * There is exactly one implementation and it is Rust: the symbols below
 * are exported from `rust/crates/hxchat-view/src/ffi.rs`, and C links
 * against them directly. This file is a *declaration* header, not a
 * forwarding layer — there is no chat_view.c.
 *
 * It was one, from C0 to C5. The dispatcher existed so the new backend
 * could coexist with the vendored xtext widget behind a runtime switch
 * while it was brought to parity; with xtext deleted it had one branch
 * left and became pure indirection, so it went too. The seam did its
 * job: by the end, xtext was referenced from that one file and nowhere
 * else in the tree, which is what made deleting 6,721 lines a
 * mechanical change rather than an archaeology project.
 *
 * What the seam bought, and what is worth not giving back:
 *
 *   1. No struct-field access. chat.c / msg.c / options.c once wrote
 *      GTK_XTEXT(w)->wordwrap, ->max_lines, ->urlcheck_function and read
 *      ->buffer and ->adj. Everything goes through hx_chat_view_set_* /
 *      _get_* calls, so the implementation owes callers behaviour rather
 *      than layout.
 *
 *   2. No raw entry pointers. The chat-history render cursors in
 *      struct hx_chat_history_render used to be live textentry* into
 *      xtext's internal linked list. They are opaque HxChatMark handles,
 *      which the Rust side backs with a message id — so a stale one is
 *      inert rather than dangling.
 *
 * Marks stay valid until the row they name is removed — explicitly via
 * hx_chat_view_remove, or implicitly by the scrollback trim
 * (hx_chat_view_set_max_lines) or a clear. Treat them as weak
 * references: hx_chat_view_remove on a stale mark is a safe no-op
 * returning FALSE, and that is the intended way to find out. Do not hold
 * a mark across a clear.
 *
 * Still xtext-shaped, and known to be: the "word_click" signal and its
 * urlcheck-function companion. A click yields a whitespace-delimited
 * word and callers demux by string prefix. Replacing them with typed
 * signals is a semantic change to three C handlers, not a mechanical
 * one — see docs/chat-view-scoping.md §3.6.
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
 * This is now the sole definition. It used to mirror xtext.h's XTEXT_*
 * constants, with G_STATIC_ASSERTs in chat_view.c checking the two
 * agreed; both are gone with xtext. The Rust side asserts against these
 * values in hxchat-view (PALETTE_COLS and the PAL_* constants), so the
 * agreement is still checked — just from the other end. */
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
/* Classifier that the view calls to decide whether a word under the
 * pointer is a link (drives the hand cursor and the word_click
 * routing). */
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

/* ---- styled runs (C6) --------------------------------------------- *
 *
 * A run is a slice of text with a colour and some attributes. A row is
 * built from an array of them: the gutter (nick column) is one array,
 * the body another.
 *
 * This replaces the in-band "\003NN" escape vocabulary that C0–C5 used
 * to get style from chat.c to the view. The escapes came from the XChat
 * xtext fork in 2000 and were never protocol — Hotline chat is plain
 * text — so they were GtkHx talking to itself in a format neither end
 * needed. The reason to be rid of them is not tidiness: two sites
 * (chat.c's info-prefix branch, msg.c's broadcast prefix) used to
 * *re-parse GtkHx's own escape output* to find where a name ended,
 * because the structure had been flattened into presentation and the
 * only way to get it back was to read it out again. Runs keep the
 * structure, so nothing has to reconstruct it.
 *
 * Runs are borrowed for the duration of the call; the view copies what
 * it needs. Build them on the stack.
 */

/* Palette index for a run: 0..31 mIRC-legacy slots, 32..37 UI roles
 * (see the HX_CHAT_PAL_* block above), or DEFAULT for the theme's
 * normal foreground. */
#define HX_CHAT_COLOR_DEFAULT (-1)

/* Named palette slots the chat code actually reaches for. These were
 * bare numbers inside printf format strings ("\00310[", "\003" "37"),
 * which is how a colour choice ends up undocumented and unsearchable.
 *
 * The values are the historical mIRC indices, kept so themes that
 * already set slots 0..31 keep rendering the same. */
#define HX_CHAT_INFO_COLOR      3  /* "[hx]" and broadcast sender names */
#define HX_CHAT_INFO_BRACKET_COLOR 10 /* the [ ] around them */
#define HX_CHAT_HIGHLIGHT_COLOR 4  /* light red: a line that mentions you */
#define HX_CHAT_PLACEHOLDER_COLOR 14 /* dark grey: inline-media alt text */

#define HX_CHAT_ATTR_NONE      0u
#define HX_CHAT_ATTR_BOLD      (1u << 0)
#define HX_CHAT_ATTR_ITALIC    (1u << 1)
#define HX_CHAT_ATTR_UNDERLINE (1u << 2)

typedef struct {
    const char *text;
    int len;       /* bytes, or -1 for strlen */
    gint16 color;  /* palette index, or HX_CHAT_COLOR_DEFAULT */
    guint16 attrs; /* HX_CHAT_ATTR_* bits */
} HxChatRun;

/* Convenience for the common "one unstyled run" case. */
#define HX_CHAT_RUN_PLAIN(t, l)                                               \
    ((HxChatRun){ (t), (l), HX_CHAT_COLOR_DEFAULT, HX_CHAT_ATTR_NONE })

/* Append a row built from runs. `gutter` may be NULL/0 for a row with
 * no nick column. Returns a mark naming the appended row. */
HxChatMark *hx_chat_view_append_runs (GtkWidget *view,
                                      const HxChatRun *gutter, int n_gutter,
                                      const HxChatRun *body, int n_body,
                                      time_t stamp);

/* As above, but inserted immediately BEFORE `anchor` (NULL prepends at
 * the head). Scroll position is preserved across the insert — see
 * hx_chat_view_insert_before below for the reasoning. */
HxChatMark *hx_chat_view_insert_runs_before (GtkWidget *view,
                                             HxChatMark *anchor,
                                             const HxChatRun *gutter,
                                             int n_gutter,
                                             const HxChatRun *body, int n_body,
                                             time_t stamp);

/* ---- appending (plain text) --------------------------------------- *
 *
 * `stamp` is a time_t for the row's timestamp column; 0 means "now".
 * Text is plain: no escape vocabulary, no styling. Use the run API
 * above when a row needs colour or attributes. */

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

/* ---- in-buffer search ----------------------------------------------- *
 *
 * Drives hxchat-layout's search engine. The old `hx_chat_view_can_search`
 * predicate is gone with xtext: it existed only so the find bar could
 * hide itself on a backend that couldn't search, and there is no such
 * backend now.
 *
 * (xtext did carry a `gtk_xtext_search` — a GRegex engine plus a
 * `search_found` list threaded through the entry chain — that nothing in
 * GtkHx ever called. It arrived with the HexChat vendoring and never ran
 * under GTK 4. It was deleted unexercised.)
 *
 * Run `needle` over the whole scrollback and select the first hit at or
 * below the viewport. An empty or NULL needle clears the search.
 *
 * `n_matches` and `current` are out-parameters for the find bar's
 * readout; `current` is 1-based, or 0 when nothing is current. Either
 * may be NULL. */
void hx_chat_view_search (GtkWidget *view, const char *needle,
                          gboolean case_sensitive,
                          guint *n_matches, guint *current);

/* Step to the next (dir > 0) or previous (dir < 0) match, wrapping at
 * both ends, and scroll it into view. */
void hx_chat_view_search_step (GtkWidget *view, int dir,
                               guint *n_matches, guint *current);

/* Drop the query and its highlights. */
void hx_chat_view_search_clear (GtkWidget *view);

G_END_DECLS

#endif /* GTKHX_CHAT_VIEW_H */
