/* Rotulus — a GTK4 scrollback view for text-stream chat
 *
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

/*
 * RotulusView is a GtkWidget that shows a long, fast-growing stream of
 * chat messages. Everything here is implemented in Rust; this header
 * declares the C ABI it exports.
 *
 * Rows are structured, never text with codes in it. The application
 * hands over a row kind, a speaker, and styled runs for the nick column
 * and the body; the view never parses escape codes, so nothing a remote
 * user sends can restyle the transcript.
 *
 * Configuration is GObject properties ("font", "word-wrap", "max-lines",
 * "indent", "max-indent", "separator", "show-timestamps",
 * "timestamp-format", "avatar-size", "group-gap", "markdown",
 * "link-schemes", "autocopy", "copy-timestamps", "activate-links", "zoom", and the
 * read-only "has-selection"). The setters below are conveniences over
 * them; g_object_set works equally well.
 *
 * Signals:
 *
 *   gboolean link-activated    (view, const char *href)
 *       A primary click on a link, when "activate-links" is on. Return
 *       TRUE to say it was handled; otherwise the view opens it with the
 *       desktop's handler.
 *   gboolean link-menu         (view, const char *href, double x, double y)
 *       A secondary or middle click on a link, in widget coordinates.
 *       Return TRUE to say it was handled; otherwise the view pops its
 *       own Open / Copy menu.
 *   void speaker-activated     (view, guint64 key)
 *   void speaker-menu          (view, guint64 key, double x, double y)
 *       A primary or secondary click on a speaker's nick or avatar.
 *   void load-more             (view, RotulusLoadDirection direction)
 *       A click on a ROTULUS_ROW_LOAD_OLDER / _NEWER row.
 *   void media-activated       (view, guint token)
 *       A primary click on an inline image or its placeholder.
 *   void selection-changed     (view)
 *
 * Marks are opaque handles to rows. They stay valid until the row they
 * name goes — removed, trimmed by "max-lines", or cleared — and are
 * weak: using a stale one is a safe no-op, and rotulus_view_remove
 * returning FALSE is the intended way to find out.
 */

#ifndef ROTULUS_H
#define ROTULUS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define ROTULUS_TYPE_VIEW (rotulus_view_get_type ())
GType rotulus_view_get_type (void);

typedef enum {
    ROTULUS_LOAD_OLDER,
    ROTULUS_LOAD_NEWER,
} RotulusLoadDirection;

#define ROTULUS_TYPE_LOAD_DIRECTION (rotulus_load_direction_get_type ())
GType rotulus_load_direction_get_type (void);

/* ---- palette ------------------------------------------------------ *
 *
 * A view draws from a palette of ROTULUS_PAL_COLS colors. Slots 0..31
 * are the legacy mIRC colors, so IRC formatting can address them by
 * number. The rest are roles.
 *
 * A role slot holding a fully transparent color means "follow the
 * system": text takes the widget's CSS color, and the background is
 * left to CSS. A new view starts with the mIRC colors and every role
 * following the system, apart from the selection, the muted and
 * timestamp text, the marker, and the per-nick colors, which have
 * defaults of their own.
 *
 * Keep each one a plain number: the crate's tests parse them. */
#define ROTULUS_PAL_MIRC_COLS 32
#define ROTULUS_PAL_MARK_FG 32        /* selection foreground */
#define ROTULUS_PAL_MARK_BG 33        /* selection background */
#define ROTULUS_PAL_FG 34             /* default text foreground */
#define ROTULUS_PAL_BG 35             /* default text background */
#define ROTULUS_PAL_MARKER 36         /* last-read marker line */
#define ROTULUS_PAL_MUTED 37          /* secondary text: history, captions */
#define ROTULUS_PAL_TIMESTAMP 38      /* timestamp column */
#define ROTULUS_PAL_NICK 39           /* other people's nicks */
#define ROTULUS_PAL_SELF_NICK 40      /* your own nick */
#define ROTULUS_PAL_NICK_BRACKET 41   /* < > around other people's nicks */
#define ROTULUS_PAL_SELF_BRACKET 42   /* < > around your own nick */
#define ROTULUS_PAL_SYSTEM 43         /* the tag of a status line */
#define ROTULUS_PAL_SYSTEM_BRACKET 44 /* the brackets around it */
#define ROTULUS_PAL_HIGHLIGHT 45      /* nick on a line that mentions you */
#define ROTULUS_PAL_RULE 46           /* the column divider */
#define ROTULUS_PAL_NICK_COLOR0 47    /* first of the per-nick colors */
#define ROTULUS_PAL_NICK_COLORS 8     /* how many per-nick slots follow */
#define ROTULUS_PAL_COLS 55           /* 32 mIRC + roles + nick colors */

/* ---- runs --------------------------------------------------------- *
 *
 * A run is a slice of text with a color and attributes. A row is built
 * from an array of them for the nick column and another for the body.
 * Runs are borrowed for the duration of the call; the view copies what
 * it needs, so build them on the stack.
 *
 * The first four fields are all most callers set (ROTULUS_RUN); the rest
 * take effect only when the matching attribute bit says so. */

#define ROTULUS_COLOR_DEFAULT (-1)

#define ROTULUS_ATTR_NONE 0u
#define ROTULUS_ATTR_BOLD (1u << 0)
#define ROTULUS_ATTR_ITALIC (1u << 1)
#define ROTULUS_ATTR_UNDERLINE (1u << 2)
#define ROTULUS_ATTR_STRIKETHROUGH (1u << 3)
#define ROTULUS_ATTR_MONOSPACE (1u << 4)
/* Swap the foreground and background. */
#define ROTULUS_ATTR_REVERSE (1u << 5)
/* `background` holds a palette index for the run's background. */
#define ROTULUS_ATTR_BACKGROUND (1u << 8)
/* `rgb` is the foreground, 0xRRGGBB, instead of `color`. */
#define ROTULUS_ATTR_RGB (1u << 9)
/* `background_rgb` is the background, 0xRRGGBB. */
#define ROTULUS_ATTR_BACKGROUND_RGB (1u << 10)

typedef struct {
    const char *text;
    int len;       /* bytes, or -1 for strlen */
    gint16 color;  /* palette index, or ROTULUS_COLOR_DEFAULT */
    guint16 attrs; /* ROTULUS_ATTR_* bits */
    gint16 background;
    guint32 rgb;
    guint32 background_rgb;
} RotulusRun;

/* A run with the four common fields; the rest are zero. Designated,
 * so -Wmissing-field-initializers has nothing to say about them. */
#define ROTULUS_RUN(t, l, c, a)                                                \
    ((RotulusRun){ .text = (t), .len = (l), .color = (c), .attrs = (a) })

/* The common "one unstyled run" case. */
#define ROTULUS_RUN_PLAIN(t, l)                                                \
    ROTULUS_RUN ((t), (l), ROTULUS_COLOR_DEFAULT, ROTULUS_ATTR_NONE)

/* ---- rows --------------------------------------------------------- */

typedef enum {
    /* A message someone sent. */
    ROTULUS_ROW_MESSAGE,
    /* A notice the application generated: never groups with its
     * neighbours, even when they share a tag. */
    ROTULUS_ROW_SYSTEM,
    /* A message loaded from history. Doesn't count against
     * "max-lines", and is never trimmed to make room for live rows. */
    ROTULUS_ROW_HISTORY,
    /* A rule with a caption, framing a block of history. */
    ROTULUS_ROW_DIVIDER,
    /* A row that pages when clicked; see the load-more signal. */
    ROTULUS_ROW_LOAD_OLDER,
    ROTULUS_ROW_LOAD_NEWER,
} RotulusRowKind;

/* The row originated here rather than arriving from elsewhere. It
 * breaks grouping independently of who the speaker is: in a
 * conversation with yourself, both halves have the same speaker, and
 * only direction tells your echo from the copy that came back. */
#define ROTULUS_ROW_OUTGOING (1u << 0)
/* A "/me" action: never groups. */
#define ROTULUS_ROW_ACTION (1u << 1)

/* Who said it.
 *
 * `key` is the application's identity for the person, or 0 when it has
 * none. The view only compares it, hands it back in the speaker signals,
 * and passes it to the avatar function. A wrong key is worse than none:
 * it attaches the wrong avatar and groups two people's messages. */
typedef struct {
    guint64 key;
    const char *nick; /* borrowed for the call; may be NULL */
    int nick_len;     /* bytes, or -1 when NUL-terminated */
} RotulusSpeaker;

#define ROTULUS_SPEAKER_NONE ((RotulusSpeaker){ 0, NULL, -1 })

typedef struct {
    RotulusRowKind kind;
    guint flags; /* ROTULUS_ROW_OUTGOING, ROTULUS_ROW_ACTION */
    gint64 stamp; /* Unix seconds for the timestamp column; 0 is now */
    RotulusSpeaker speaker;
    const RotulusRun *gutter; /* the nick column; NULL for none */
    int n_gutter;
    const RotulusRun *body;
    int n_body;
} RotulusRow;

typedef struct _RotulusMark RotulusMark;

/* ---- construction and configuration ------------------------------- */

GtkWidget *rotulus_view_new (void);

/* `palette` holds ROTULUS_PAL_COLS colors. */
void rotulus_view_set_palette (GtkWidget *view, const GdkRGBA palette[]);

void rotulus_view_set_font (GtkWidget *view, const char *font);
void rotulus_view_set_word_wrap (GtkWidget *view, gboolean word_wrap);
/* Rows kept before the oldest are dropped; 0 keeps everything. */
void rotulus_view_set_max_lines (GtkWidget *view, int max_lines);
/* Two columns: timestamps and nicks in a column of their own, bodies
 * indented past it. Off, a row is one column, with the nick at the
 * start of the body's first line. */
void rotulus_view_set_indent (GtkWidget *view, gboolean indent);
void rotulus_view_set_max_indent (GtkWidget *view, int max_indent_px);
void rotulus_view_set_separator (GtkWidget *view, gboolean separator);
void rotulus_view_set_show_timestamps (GtkWidget *view, gboolean show);
/* strftime(3); NULL or "" restores the default, "[%H:%M:%S] ". */
void rotulus_view_set_timestamp_format (GtkWidget *view, const char *format);
/* Edge of the avatar beside a speaker, in px; 0 hides avatars. Only the
 * first row of a group draws one. */
void rotulus_view_set_avatar_size (GtkWidget *view, int px);
#define ROTULUS_AVATAR_SIZE_DEFAULT 32
/* Seconds between one speaker's messages that start a new group; 0
 * turns grouping off. */
void rotulus_view_set_group_gap (GtkWidget *view, int secs);
#define ROTULUS_GROUP_GAP_DEFAULT 300
/* Render markdown in bodies appended from now on: **bold**, *italic*,
 * `code`, fenced code blocks, > quotes, and [label](url) links to the
 * view's link schemes. Rows already appended keep their rendering. */
void rotulus_view_set_markdown (GtkWidget *view, gboolean markdown);
/* The URL scheme prefixes that become links, NULL-terminated:
 * { "https://", "mailto:", NULL }. NULL restores the default set. */
void rotulus_view_set_link_schemes (GtkWidget *view,
                                    const char *const *schemes);
/* Copy to the clipboards when a drag-select ends. */
void rotulus_view_set_autocopy (GtkWidget *view, gboolean autocopy);
/* Prefix each copied row with its timestamp. */
void rotulus_view_set_copy_timestamps (GtkWidget *view, gboolean copy);
/* Open a link on a primary click (the default). Off, a primary click on a
 * link does nothing special; the link menu works either way. */
void rotulus_view_set_activate_links (GtkWidget *view, gboolean activate);
/* Text scale, where 1.0 is the font's own size. */
void rotulus_view_set_zoom (GtkWidget *view, double zoom);

/* Resolves a speaker's key to the image in their avatar slot. Called on
 * every draw, so an animated avatar animates; cache anything expensive.
 * The result is borrowed until the draw finishes. The function must not
 * change the view. */
typedef GdkPaintable *(*RotulusAvatarFunc) (GtkWidget *view, guint64 key,
                                            gpointer user_data);
void rotulus_view_set_avatar_func (GtkWidget *view, RotulusAvatarFunc func,
                                   gpointer user_data, GDestroyNotify destroy);

/* The vertical adjustment, for a GtkScrollbar beside the view. The view
 * is also a GtkScrollable, so a GtkScrolledWindow works too. */
GtkAdjustment *rotulus_view_get_vadjustment (GtkWidget *view);

/* ---- content ------------------------------------------------------ */

RotulusMark *rotulus_view_append (GtkWidget *view, const RotulusRow *row);

/* Insert immediately before `anchor`; NULL inserts at the top. What is
 * on screen stays where it is, so backfilling history doesn't move what
 * the user is reading. Insert rows in chronological order: each lands
 * directly before the anchor. */
RotulusMark *rotulus_view_insert_before (GtkWidget *view, RotulusMark *anchor,
                                         const RotulusRow *row);

/* Swap the content of the row `mark` names, keeping its place and the
 * mark: an edit, a redaction, a streamed reply growing. FALSE if the
 * row is gone. */
gboolean rotulus_view_replace (GtkWidget *view, RotulusMark *mark,
                               const RotulusRow *row);

/* FALSE if the row was already gone, which is not an error. */
gboolean rotulus_view_remove (GtkWidget *view, RotulusMark *mark);

/* A plain row with no nick column. */
RotulusMark *rotulus_view_append_text (GtkWidget *view, const char *text,
                                       int len, gint64 stamp);

/* Drop every row. Every mark goes stale. */
void rotulus_view_clear (GtkWidget *view);

/* The newest row, or NULL when the view is empty. */
RotulusMark *rotulus_view_get_last (GtkWidget *view);

/* Draw the last-read marker under the row `mark` names; NULL removes
 * it. The marker goes with its row rather than moving to a neighbour. */
void rotulus_view_set_marker (GtkWidget *view, RotulusMark *mark);

/* ---- inline media ------------------------------------------------- *
 *
 * A media row shows `alt` as text until an image arrives, then the
 * image. `token` identifies it to the application: media-activated
 * reports it, and rotulus_view_media_mark finds the row by it, which is
 * what an asynchronous decode should hold — the row may be trimmed
 * while it runs. */

typedef struct {
    GdkTexture *texture;
    guint32 delay_ms; /* how long this frame shows */
} RotulusFrame;

RotulusMark *rotulus_view_append_media (GtkWidget *view, GdkTexture *texture,
                                        const char *alt, guint token,
                                        gint64 stamp);
RotulusMark *rotulus_view_media_mark (GtkWidget *view, guint token);
/* NULL reverts the row to its alt text. */
void rotulus_view_media_set_texture (GtkWidget *view, RotulusMark *mark,
                                     GdkTexture *texture);
/* An animation. The view takes its own references; n_frames 0 reverts
 * the row to its alt text. */
void rotulus_view_media_set_frames (GtkWidget *view, RotulusMark *mark,
                                    const RotulusFrame *frames,
                                    guint n_frames);

/* ---- search ------------------------------------------------------- *
 *
 * `n_matches` and `current` are out-parameters for a find bar's
 * readout; `current` is 1-based, 0 when nothing is current. Either may
 * be NULL. */

/* Select the first match at or below the viewport. NULL or "" clears. */
void rotulus_view_search (GtkWidget *view, const char *needle,
                          gboolean case_sensitive, guint *n_matches,
                          guint *current);
/* Step forward (dir > 0) or back (dir < 0), wrapping. */
void rotulus_view_search_step (GtkWidget *view, int dir, guint *n_matches,
                               guint *current);
void rotulus_view_search_clear (GtkWidget *view);

/* ---- IRC formatting ----------------------------------------------- *
 *
 * Split text carrying mIRC formatting codes (bold, italic, underline,
 * strikethrough, monospace, reverse, reset, and colors by number or
 * hex) into runs, with the codes removed. The runs point into `text`,
 * which must outlive them. Free the array with g_free. Colors 0..15
 * address the palette's mIRC slots; the extended colors and hex colors
 * are RGB. */
RotulusRun *rotulus_mirc_parse (const char *text, int len, int *n_runs);

G_END_DECLS

#endif /* ROTULUS_H */
