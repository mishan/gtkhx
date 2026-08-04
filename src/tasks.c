/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <adwaita.h>
#include <sys/time.h>
#include <time.h>
#include "hx.h"
#include "gtkhx_session.h"
#include "network.h"
#include "gtkutil.h"
#include "gtkhx_theme.h" /* gtkhx_theme_scale, GTKHX_SCALE_TASKS_ROW_ICON */
#include "human_readable.h"
#include "gtkhx.h"
#include "gtkhx_icon.h"
#include "xfers.h"
#include "htxf_accessors.h"
#include "sound.h"
#include "toolbar.h" /* disconnect_clicked, toolbar_show_toast */
#include "tasks.h"
#include "session_registry.h"
#include "panel_registry.h"

/* Phase 5 task-row polish: each row is now an Adwaita-shaped
 * action-row layout — icon column on the left, then a vbox with
 * three stacked elements:
 *
 *   [icon] [Title (filename)                          Queue #3]
 *          [Subtitle: bytes · speed · ETA                     ]
 *          [progress bar                                      ]
 *
 * The subtitle uses font-feature-settings: "tnum" so digit
 * widths stay fixed across updates — that's what kills the
 * "label keeps moving around" jitter the old single-line label
 * suffered from: when "1.2 MB" grew to "10.2 MB" the whole
 * trailing path shifted right by one glyph each tick.
 *
 * The struct now holds pointers to all three independently-
 * updatable widgets so file_update / task_update can refresh
 * them in place without rebuilding the row. */
struct gtask {
    struct gtask *next, *prev;
    /* The connection this row belongs to, by serial. The queue is one list
     * for the whole app — "is anything still going?" should be one place to
     * look, not a hunt across tabs — so every row has to say whose it is.
     *
     * A serial rather than a session pointer for the usual reason: a row can
     * outlive the tab that made it by the length of one main-loop turn, and
     * closing a tab frees the session.
     *
     * It is also what makes lookups correct. Transaction ids are only unique
     * within a connection, and the progress rows below use fixed pseudo-ids
     * (-127 / -128 / -129), so two connections would otherwise collide on
     * every one of them — a second server connecting would drive the first
     * server's connect row. Every search keys on the pair. */
    guint16 conn;
    guint32 trans;
    struct htxf_conn *htxf;
    GtkWidget *icon;
    GtkWidget *title;
    GtkWidget *subtitle;
    GtkWidget *pbar;
    GtkWidget *listitem;
    GtkWidget *queue; /* badge on the title row; NULL for non-xfer tasks */
    GtkWidget *tag;   /* which connection this row is on; see tags_refresh */
};

/* A row that belongs to no connection.
 *
 * Serials start at 1, so 0 is free for this. The tracker rows use it: a
 * tracker fetch is one process-wide operation, not something a connection
 * owns, so tagging its progress row with whichever connection happened to be
 * focused would name a server that has nothing to do with it — and then
 * disconnecting that server mid-refresh would delete the row out from under a
 * fetch that is still running. Rows on this connection are exempt from the
 * per-connection sweep and show no tag. */
#define CONN_NONE 0

/* The queue itself: one list, one widget, for the whole application.
 *
 * These were three per-session fields. That made the panel per-connection —
 * one page per tab, each showing only its own transfers — which turns "is
 * anything still going?" into a hunt, and makes a stalled upload on a
 * connection you aren't looking at invisible. See docs/multi-connection.md,
 * "Global but tagged". */
static struct gtask *gtask_list;
static GtkWidget *gtklist;
static GtkWidget *gtask_scroll;

/* The connection a session is on. Every public entry point below still takes
 * the session — its callers are signal handlers that have one — and turns it
 * into the serial the rows are keyed on right here.
 *
 * NULL answers CONN_NONE, which is a real key rather than a failure: it is
 * where the tracker's connection-less rows live. That makes it the wrong
 * answer for anything else, which is why the entry points reject a NULL
 * session rather than letting one through to here. A freed connection is not
 * tolerated at all — hx_conn_serial dereferences. */
static guint16
sess_conn (session *sess)
{
    return sess != NULL ? hx_conn_serial (sess->htlc) : CONN_NONE;
}

/* One-time CSS provider for the tasks rows. Loads tabular-nums on
 * the subtitle (kills digit-width jitter), dims the subtitle, and
 * gives the progress bar a sensible minimum height. Attached at
 * GTK_STYLE_PROVIDER_PRIORITY_APPLICATION so our rules sit above
 * theme defaults but below the user's gtk.css. */
static GtkCssProvider *tasks_css_provider;

static void
ensure_tasks_css (void)
{
    /* Subtitle size + queue size now come from Pango attributes
     * set directly on the labels (see gtask_apply_smaller_font),
     * not from CSS font-size. CSS font-size resolves AFTER the
     * label asks Pango for its natural height, and rounding from
     * em -> px occasionally leaves the rendered glyphs 1 px
     * taller than the allocated height — which clipped the top of
     * the subtitle line. Pango attributes are consulted during
     * layout's get_extents call, so allocation matches rendering
     * exactly. CSS still handles opacity + tabular-nums + the
     * progress-bar height. */
    static const char tasks_css[]
        = ".gtkhx-task-row {"
          "  padding: 8px 12px;"
          "}"
          ".gtkhx-task-title {"
          "  font-weight: 600;"
          "}"
          ".gtkhx-task-subtitle {"
          "  opacity: 0.65;"
          "  font-feature-settings: \"tnum\";"
          "}"
          ".gtkhx-task-queue {"
          "  opacity: 0.65;"
          "  font-feature-settings: \"tnum\";"
          "}"
          ".gtkhx-task-row progressbar > trough {"
          "  min-height: 6px;"
          "}"
          ".gtkhx-task-row progressbar > trough > progress {"
          "  min-height: 6px;"
          "}";

    GdkDisplay *display;

    if (tasks_css_provider) {
        return;
    }
    /* No display means we're running before GTK is initialised or
     * in a non-GUI context (the unit tests don't currently exercise
     * tasks.c, but be defensive — attach-with-NULL is a hard crash
     * in GTK 4 rather than a graceful no-op). The provider stays
     * NULL so the next call retries once a display exists. */
    display = gdk_display_get_default ();
    if (!display) {
        return;
    }
    tasks_css_provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (tasks_css_provider, tasks_css);
    gtk_style_context_add_provider_for_display (
        display, GTK_STYLE_PROVIDER (tasks_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* Apply a "smaller than default" font size as a Pango attribute on
 * the label. Pango consults its attribute list during get_extents,
 * which is the same call the GtkLabel measure path goes through,
 * so the label's natural-size matches what the glyphs need. CSS
 * font-size doesn't have that property — it resolves after the
 * em-to-px conversion, which can leave 1 px of clip at the top
 * of the line when the conversion rounds down. */
static void
gtask_apply_smaller_font (GtkLabel *label, double scale)
{
    PangoAttrList *attrs = pango_attr_list_new ();
    PangoAttribute *a = pango_attr_scale_new (scale);
    pango_attr_list_insert (attrs, a);
    gtk_label_set_attributes (label, attrs);
    pango_attr_list_unref (attrs);
}

/* Resolve a gresource path for the row icon column. We use the
 * same retro pixmaps the toolbar carries (download/upload/tracker/
 * connect/tasks) so the Tasks window keeps the period-appropriate look
 * instead of jumping into Adwaita's symbolic icon set. The pixmaps
 * are 16x16 originals; gtask_make_icon scales 2x to 32px to match
 * the toolbar treatment. */
static const char *
gtask_icon_for (guint32 trans, struct htxf_conn *htxf)
{
    if (htxf) {
        return htxf->type == XFER_GET
                   ? "/com/nasledov/gtkhx/pixmaps/download.png"
                   : "/com/nasledov/gtkhx/pixmaps/upload.png";
    }
    switch ((gint32)trans) {
    case -127: /* tracker list */
    case -129: /* tracker connect */
        return "/com/nasledov/gtkhx/pixmaps/tracker.png";
    case -128: /* main control-channel connect */
        return "/com/nasledov/gtkhx/pixmaps/connect.png";
    default:
        return "/com/nasledov/gtkhx/pixmaps/tasks.png";
    }
}

/* Build a GtkPicture from a gresource pixmap path, scaled
 * nearest-neighbour to preserve the chunky pixel-art look. Mirrors
 * the icon-loading half of gtkhx_pixmap_button without the
 * surrounding GtkButton wrap. The icons we use are all 16x16
 * originals; the 2x scale gives 32x32 to match the toolbar
 * treatment. On a missing resource (shouldn't happen in a normal
 * build — the gresource bundle is compiled in) we hand back a
 * GtkPicture with the same size request so the row's icon column
 * stays aligned with sibling rows that did get their icon. */
#define GTASK_ICON_SRC_SIZE 16
static GtkWidget *
gtask_make_icon (const char *resource_path)
{
    GdkPixbuf *src;
    GdkPixbuf *use_pb;
    GdkTexture *tex;
    GtkWidget *picture;
    /* Theme-driven render size. The source art is 16×16; the active
     * theme's GTKHX_SCALE_TASKS_ROW_ICON factor (default 200% — the
     * historical 2× scale) lands the row icon at the user-tunable
     * size. Read at construction time; existing in-flight task rows
     * keep their construction-size on a Settings change, new tasks
     * pick up the new factor. Could subscribe per-row to the theme
     * `changed` signal for live rescale, but task rows are usually
     * short-lived (xfers complete) so it's not worth the bookkeeping. */
    double scale = gtkhx_theme_scale (GTKHX_SCALE_TASKS_ROW_ICON);
    int px = (int)(GTASK_ICON_SRC_SIZE * scale + 0.5);

    /* Route through the icon resolver so the active theme's bundled
     * icons (e.g. $CONFIG/themes/<theme>/icons/download.png) shadow the
     * stock pixmap. */
    src = gtkhx_icon_load (resource_path);
    if (!src) {
        picture = gtk_picture_new ();
        gtk_widget_set_size_request (picture, px, px);
        return picture;
    }
    if (scale != 1.0) {
        int w = (int)(gdk_pixbuf_get_width (src) * scale + 0.5);
        int h = (int)(gdk_pixbuf_get_height (src) * scale + 0.5);
        use_pb = gdk_pixbuf_scale_simple (src, w, h, GDK_INTERP_NEAREST);
        g_object_unref (src);
    } else {
        use_pb = src;
    }
    /* gdk_pixbuf_scale_simple can return NULL under OOM (or if the
     * scaled dimensions overflow). Treat it the same as a missing
     * resource: empty GtkPicture with the expected size request so
     * the icon column stays aligned with sibling rows. Matches
     * gtkhx_pixmap_button's defensive shape. */
    if (!use_pb) {
        picture = gtk_picture_new ();
        gtk_widget_set_size_request (picture, px, px);
        return picture;
    }
    tex = gtkhx_texture_from_pixbuf (use_pb);
    if (!tex) {
        g_object_unref (use_pb);
        picture = gtk_picture_new ();
        gtk_widget_set_size_request (picture, px, px);
        return picture;
    }
    picture = gtk_picture_new_for_paintable (GDK_PAINTABLE (tex));
    gtk_picture_set_can_shrink (GTK_PICTURE (picture), FALSE);
    g_object_unref (tex);
    g_object_unref (use_pb);
    return picture;
}

void
create_tasks (void)
{
    /* Once for the application, not once per connection. Idempotent so the
     * historic per-session call sites can keep calling it. */
    if (gtklist != NULL) {
        return;
    }

    /* CSS-loading happens once at first session bring-up — the
     * provider is attached to the display, so all sessions share
     * it. Idempotent if already loaded. */
    ensure_tasks_css ();

    /* ported from GtkList (removed in GTK 3) to GtkListBox.
     * Each transfer/task is a GtkListBoxRow holding the icon + title +
     * subtitle + progress-bar layout; gtsk->listitem points at the row,
     * with the gtsk pointer stashed via g_object_set_data on the row
     * itself. */
    gtklist = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (gtklist),
                                     GTK_SELECTION_MULTIPLE);
    /* Follow the active GtkHx theme's fg/bg via .gtkhx-listview. */
    gtkhx_apply_listview_style (gtklist);
    g_object_ref_sink (gtklist);

    gtask_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (gtask_scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    /* grow vertically inside the
     * panel's content vbox so the scrolled area fills whichever
     * frame the panel resides in. Without this the scroll widget
     * shrinks to the list's natural height and the panel leaves
     * empty space below the rows. Matches users.c's pattern. */
    gtk_widget_set_vexpand (gtask_scroll, TRUE);
    gtkhx_widget_set_child (gtask_scroll, gtklist);
    g_object_ref_sink (gtask_scroll);
}

/* Keyed on the pair: a transaction id means nothing without the connection
 * that issued it. */
static void gtask_delete (struct gtask *gtsk);

static struct gtask *
gtask_with_trans (guint16 conn, guint32 trans)
{
    struct gtask *gtsk;

    for (gtsk = gtask_list; gtsk; gtsk = gtsk->prev) {
        if (gtsk->conn == conn && gtsk->trans == trans) {
            return gtsk;
        }
    }

    return 0;
}

/* No connection needed: a transfer handle is unique across the process, and
 * the row that holds it is the one that owns it. */
static struct gtask *
gtask_with_htxf (struct htxf_conn *htxf)
{
    struct gtask *gtsk;

    for (gtsk = gtask_list; gtsk; gtsk = gtsk->prev) {
        if (gtsk->htxf == htxf) {
            return gtsk;
        }
    }

    return 0;
}

/* Refresh the queue badge text + visibility from htxf->queue. The
 * server sends a position-in-queue when too many transfers are
 * pending; 0 means "running now". We hide the badge when the
 * value isn't useful so the title row doesn't carry dead chrome. */
static void
gtask_refresh_queue_badge (struct gtask *gtsk)
{
    if (!gtsk->queue) {
        return;
    }
    if (gtsk->htxf && gtsk->htxf->queue > 0) {
        /* g_strdup_printf sizes itself so neither a translated
         * "Queued #%u" form nor a large queue number can truncate.
         * %u matches htxf->queue's guint32 type. */
        g_autofree char *qid
            = g_strdup_printf (_ ("Queued #%u"), gtsk->htxf->queue);
        gtk_label_set_text (GTK_LABEL (gtsk->queue), qid);
        gtk_widget_set_visible (gtsk->queue, TRUE);
    } else {
        gtk_widget_set_visible (gtsk->queue, FALSE);
    }
}

void
output_xfer_queue (session *sess, struct htxf_conn *htxf)
{
    struct gtask *gtsk = gtask_with_htxf (htxf);

    /* No session needed, and so no NULL guard: a transfer handle is unique
     * across the process, and the row that holds it is the one that owns it.
     * The parameter stays because the signal handlers that call in have one. */
    (void)sess;
    if (!gtsk) {
        return;
    }
    gtask_refresh_queue_badge (gtsk);
}

static struct gtask *
gtask_new (guint16 conn, guint32 trans, struct htxf_conn *htxf)
{
    GtkWidget *row_box;     /* outer hbox: icon | vbox */
    GtkWidget *content_box; /* inner vbox: title-row, subtitle, pbar */
    GtkWidget *title_row;   /* inner hbox: title (hexpand) | queue badge */
    GtkWidget *icon, *title, *subtitle, *pbar, *queue, *listitem, *tag;
    struct gtask *gtsk;

    /* `conn` must be a real connection serial (>= 1) unless the caller is
     * building one of the tracker's rows, which ask for CONN_NONE by name.
     * Anything else arriving with CONN_NONE is building a row that no
     * disconnect will ever sweep, because the sweep is per connection and
     * this one belongs to none. */
    gtsk = g_malloc (sizeof (struct gtask));
    gtsk->next = 0;
    gtsk->prev = gtask_list;
    if (gtask_list) {
        gtask_list->next = gtsk;
    }

    /* Icon column. Retro Hotline-era pixmaps from gresource —
     * gtask_make_icon scales nearest-neighbour so the pixel art
     * stays crisp at the 2x size. Vcentered against the title +
     * subtitle block to its right. */
    icon = gtask_make_icon (gtask_icon_for (trans, htxf));
    gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);

    /* Title — bold via CSS class, ellipsize keeps it on one line,
     * expands horizontally inside the title row so the queue badge
     * sits flush-right. xalign=0 keeps the text left-aligned.
     *
     * NOTE: we deliberately do NOT call gtk_label_set_single_line_mode
     * here. That API pins the label's height to the *default* font's
     * ascent+descent. The subtitle below uses Pango attributes to
     * scale its font down (see gtask_apply_smaller_font); single_line_mode
     * uses the unscaled metric for its height query, which over-reserves
     * vertical space and earlier interactions even managed to clip the
     * top of the smaller subtitle line. Skip set_single_line_mode and
     * rely on ellipsize alone to enforce one-line behaviour — both
     * labels are always populated in gtask_new so the empty/non-empty
     * height jump set_single_line_mode was designed to suppress is a
     * non-issue here. */
    title = gtk_label_new ("");
    gtk_widget_add_css_class (title, "gtkhx-task-title");
    gtk_label_set_xalign (GTK_LABEL (title), 0.0f);
    gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (title, TRUE);

    /* Subtitle — the line that updates every tick. CSS pins it to
     * tabular-nums + dim opacity, which is what stops the digits
     * shifting siblings around as the speed / ETA churn. Font-size
     * shrinkage goes through a Pango scale attribute (not CSS) so
     * the label's natural-height query matches the rendered glyph
     * height exactly — see gtask_apply_smaller_font's comment. */
    subtitle = gtk_label_new ("");
    gtk_widget_add_css_class (subtitle, "gtkhx-task-subtitle");
    gtk_label_set_xalign (GTK_LABEL (subtitle), 0.0f);
    gtk_label_set_ellipsize (GTK_LABEL (subtitle), PANGO_ELLIPSIZE_END);
    gtask_apply_smaller_font (GTK_LABEL (subtitle), PANGO_SCALE_SMALL);

    /* Queue badge — only allocated for xfer rows (HTLC tasks never
     * carry a queue position). Starts hidden; gtask_refresh_queue_badge
     * flips it on when htxf->queue > 0. */
    queue = NULL;
    if (htxf) {
        queue = gtk_label_new ("");
        gtk_widget_add_css_class (queue, "gtkhx-task-queue");
        gtk_widget_set_valign (queue, GTK_ALIGN_CENTER);
        gtk_widget_set_visible (queue, FALSE);
        gtask_apply_smaller_font (GTK_LABEL (queue), PANGO_SCALE_X_SMALL);
    }

    /* Progress bar — slim Adwaita-styled bar via CSS min-height,
     * no overlaid percent text (subtitle carries position info). */
    pbar = gtk_progress_bar_new ();
    gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (pbar), FALSE);
    gtk_widget_set_valign (pbar, GTK_ALIGN_CENTER);

    /* Which connection this row is on. Dim and to the right of the queue
     * badge, and hidden outright while only one connection is open — so a
     * single-connection session looks exactly as it did before the queue
     * became shared, and the tag appears when a second tab does. Text is
     * filled in by tags_refresh below rather than here, because the server's
     * name arrives at login, after the first rows already exist. */
    tag = gtk_label_new ("");
    gtk_widget_add_css_class (tag, "gtkhx-task-queue");
    gtk_widget_set_valign (tag, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize (GTK_LABEL (tag), PANGO_ELLIPSIZE_END);
    /* Capped, because the text is whatever the server called itself in its
     * SERVERNAME chunk — often a whole banner line. Uncapped, its natural
     * width is that entire string and the filename beside it, which is the
     * part the user is actually reading, gets crushed instead. */
    gtk_label_set_max_width_chars (GTK_LABEL (tag), 16);
    gtk_widget_set_visible (tag, FALSE);
    gtask_apply_smaller_font (GTK_LABEL (tag), PANGO_SCALE_X_SMALL);

    /* Top-row hbox: title (hexpand) + optional queue badge + connection. */
    title_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtkhx_box_pack (title_row, title, 1, 1, 0);
    if (queue) {
        gtkhx_box_pack (title_row, queue, 0, 0, 0);
    }
    gtkhx_box_pack (title_row, tag, 0, 0, 0);

    /* Content vbox stacks title-row, subtitle, progress bar. */
    content_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand (content_box, TRUE);
    gtkhx_box_pack (content_box, title_row, 0, 0, 0);
    gtkhx_box_pack (content_box, subtitle, 0, 0, 0);
    gtkhx_box_pack (content_box, pbar, 0, 0, 0);

    /* Outer row hbox: icon + content. Min width keeps narrow
     * tasks windows from squashing the bar to nothing. */
    row_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class (row_box, "gtkhx-task-row");
    gtk_widget_set_size_request (row_box, 280, -1);
    gtkhx_box_pack (row_box, icon, 0, 0, 0);
    gtkhx_box_pack (row_box, content_box, 1, 1, 0);

    listitem = gtk_list_box_row_new ();
    g_object_set_data (G_OBJECT (listitem), "gtsk", gtsk);
    gtkhx_widget_set_child (listitem, row_box);

    if (gtklist) {
        gtk_list_box_insert (GTK_LIST_BOX (gtklist), listitem, -1);
    }

    gtsk->icon = icon;
    gtsk->title = title;
    gtsk->subtitle = subtitle;
    gtsk->pbar = pbar;
    gtsk->listitem = listitem;
    gtsk->trans = trans;
    gtsk->htxf = htxf;
    gtsk->queue = queue;
    gtsk->tag = tag;
    gtsk->conn = conn;
    gtask_list = gtsk;

    /* Initial queue-badge state if the htxf came in pre-queued. */
    gtask_refresh_queue_badge (gtsk);
    gtkhx_tasks_refresh_tags ();

    return gtsk;
}

/* Drop every row belonging to `htlc`.
 *
 * The queue is shared, so a connection going away no longer takes its rows
 * with it — nothing destroys a per-connection page any more, because there
 * isn't one. Its transfers are cancelled separately (xfers_delete_on_conn),
 * and leaving their rows behind would mean dead progress bars for a server
 * that is gone, with a reconnect building fresh ones alongside.
 *
 * Walks forward off `prev` like every other traversal here, taking the next
 * pointer before the delete unlinks the node. */
void
gtasks_delete_on_conn (struct htlc_conn *htlc)
{
    guint16 conn = hx_conn_serial (htlc);
    struct gtask *gtsk, *prev;

    /* Never sweep the connection-less rows: that is where the tracker's live,
     * and a NULL htlc reads as CONN_NONE. A *freed* connection is a different
     * matter and is not tolerated here — hx_conn_serial dereferences, so the
     * caller must still hold a live connection or NULL. */
    if (conn == CONN_NONE) {
        return;
    }

    for (gtsk = gtask_list; gtsk; gtsk = prev) {
        prev = gtsk->prev;
        if (gtsk->conn == conn) {
            gtask_delete (gtsk);
        }
    }
    gtkhx_tasks_refresh_tags ();
}

/* Drop the tracker's progress rows.
 *
 * They belong to no connection, so nothing sweeps them; and the only path
 * that removed them was reaching the last server of a walk. A tracker that
 * errors out never gets there, which left a stale "Listing tracker" row that
 * the next refresh would sit beside rather than replace. Called from
 * tracker_kill_threads, which is every way a fetch ends early. */
void
gtasks_delete_tracker_rows (void)
{
    struct gtask *gtsk, *prev;

    for (gtsk = gtask_list; gtsk; gtsk = prev) {
        prev = gtsk->prev;
        if (gtsk->conn == CONN_NONE
            && (gtsk->trans == (guint32)-127 || gtsk->trans == (guint32)-129)) {
            gtask_delete (gtsk);
        }
    }
}

/* How many rows the queue is holding for `conn`. Debug-only: the sweep on
 * disconnect has no other visible effect in a headless run, and "the shared
 * queue kept a dead connection's rows" is the failure this port could
 * plausibly introduce. */
guint
gtkhx_tasks_rows_for_conn (guint16 conn)
{
    struct gtask *gtsk;
    guint n = 0;

    for (gtsk = gtask_list; gtsk; gtsk = gtsk->prev) {
        if (gtsk->conn == conn) {
            n++;
        }
    }
    return n;
}

/* Re-label every row with its connection, and show or hide the labels.
 *
 * Called whenever the set of connections changes (open, close, and login,
 * where a server finally says what it is called) and whenever a row is added.
 * Cheap enough to do wholesale: the list is short and this only runs on
 * events the user caused.
 *
 * Below two connections there is nothing to disambiguate, so the labels go
 * away entirely rather than sitting there repeating one name. */
void
gtkhx_tasks_refresh_tags (void)
{
    gboolean show = hx_session_count () > 1;
    struct gtask *gtsk;

    for (gtsk = gtask_list; gtsk; gtsk = gtsk->prev) {
        session *owner;
        char *label;

        if (!gtsk->tag) {
            continue;
        }
        /* A row that belongs to no connection has nothing to say here. */
        gtk_widget_set_visible (gtsk->tag, show && gtsk->conn != CONN_NONE);
        if (!show || gtsk->conn == CONN_NONE) {
            continue;
        }
        /* NULL for a row whose connection is on its way out — it will be
         * swept in a moment, so leave whatever it last said rather than
         * blanking it for one frame. */
        owner = hx_session_with_serial (gtsk->conn);
        if (owner == NULL) {
            continue;
        }
        label = hx_session_label (owner);
        gtk_label_set_text (GTK_LABEL (gtsk->tag), label);
        g_free (label);
    }
}

/* Drop one row: unparent its widget and unlink it from the queue. The list is
 * hand-rolled and separate from the per-connection task hash tables, so
 * destroying one of those does not free these. Internal now — the session
 * teardown used to walk the per-session list through this, and there is no
 * per-session list any more. */
static void
gtask_delete (struct gtask *gtsk)
{
    if (gtklist) {
        gtkhx_widget_remove_child (gtklist, gtsk->listitem);
    }
    if (gtsk->next) {
        gtsk->next->prev = gtsk->prev;
    }
    if (gtsk->prev) {
        gtsk->prev->next = gtsk->next;
    }
    if (gtsk == gtask_list) {
        gtask_list = gtsk->prev;
    }
    g_free (gtsk);
}

void
gtask_delete_htxf (session *sess, struct htxf_conn *htxf)
{
    struct gtask *gtsk = gtask_with_htxf (htxf);
    /* Keyed on the transfer, which is unique process-wide — see
     * output_xfer_queue on why there is no session guard here. */
    (void)sess;
    if (!gtsk) {
        return;
    }
    gtask_delete (gtsk);
}

/* Disconnect any gtask still referencing this htxf, leaving the
 * UI row visible but with a NULL htxf pointer. Called from
 * gtkhx.c's xfer-destroyed signal handler at the moment the htxf
 * leaves the live xfers[] list and may be freed shortly after by
 * a cleanup_dispatch unref. Subsequent clicks on the row's Cancel
 * button see the NULL pointer and skip the xfer_delete call.
 *
 * NOT the same as gtask_delete_htxf — that one removes the row
 * from the UI as well. This one just severs the dangling-pointer
 * risk while leaving the row in place. For normal-success folder
 * transfers the row gets removed by file_update's pos>=size path
 * anyway; this clear runs after that (idempotent — gtask_with_htxf
 * returns NULL if the row is already gone). */
void
gtask_clear_htxf (session *sess, struct htxf_conn *htxf)
{
    struct gtask *gtsk = gtask_with_htxf (htxf);
    /* Keyed on the transfer, which is unique process-wide — see
     * output_xfer_queue on why there is no session guard here. */
    (void)sess;
    if (!gtsk) {
        return;
    }
    gtsk->htxf = NULL;
}

void
gtask_delete_tsk (session *sess, guint32 trans)
{
    struct gtask *gtsk;

    /* A NULL session would key the lookup on CONN_NONE, which is where the
     * tracker's rows live — so a pseudo-id colliding with one of theirs would
     * delete a tracker row, and any other id would silently find nothing. */
    g_return_if_fail (sess != NULL);

    gtsk = gtask_with_trans (sess_conn (sess), trans);
    if (!gtsk) {
        return;
    }
    gtask_delete (gtsk);
}

/* Set the progress bar fraction to num/total, guarded against zero
 * total and clamped to [0.0, 1.0]. Earlier task_update / progress
 * call sites used num / (num + total) which never reaches 1.0
 * (tops out at 0.5 when num == total). Caller semantics here are
 * the natural "X of Y" — num is current count, total is the
 * denominator. */
static void
gtask_set_fraction (GtkProgressBar *pbar, guint32 num, guint32 total)
{
    gdouble frac;

    if (total == 0) {
        gtk_progress_bar_set_fraction (pbar, 0.0);
        return;
    }
    frac = (gdouble)num / (gdouble)total;
    if (frac < 0.0) {
        frac = 0.0;
    }
    if (frac > 1.0) {
        frac = 1.0;
    }
    gtk_progress_bar_set_fraction (pbar, frac);
}

void
track_prog_update (session *sess, char *str, int num, int total)
{
    struct gtask *gtsk;
    guint32 pos = (guint32)(num < 0 ? 0 : num);
    guint32 tot = (guint32)(total < 0 ? 0 : total);
    g_autofree char *sub = NULL;

    /* Deliberately ignored: the tracker's rows belong to no connection, so
     * this asks for CONN_NONE by name rather than deriving a serial from
     * whichever session happened to start the fetch. */
    (void)sess;
    gtsk = gtask_with_trans (CONN_NONE, -127);
    if (!gtsk) {
        gtsk = gtask_new (CONN_NONE, -127, 0);
    }

    gtk_label_set_text (GTK_LABEL (gtsk->title), _ ("Listing tracker"));
    /* g_strdup_printf sizes itself so a long tracker hostname (str)
     * or a translated form with extra glyphs can't truncate. Cast
     * the int args explicitly so the %u format matches the type
     * actually passed through varargs. */
    sub = g_strdup_printf (_ ("%1$s \xc2\xb7 %2$u of %3$u servers"),
                           str ? str : "", pos, tot);
    gtk_label_set_text (GTK_LABEL (gtsk->subtitle), sub);

    gtask_set_fraction (GTK_PROGRESS_BAR (gtsk->pbar), pos, tot);

    if (num >= total) {
        gtask_delete (gtsk);
    }
}

void
trackconn_prog_update (session *sess, char *str, int num, int total)
{
    struct gtask *gtsk;
    guint32 pos = (guint32)(num < 0 ? 0 : num);
    guint32 tot = (guint32)(total < 0 ? 0 : total);
    g_autofree char *sub = NULL;

    /* Deliberately ignored: the tracker's rows belong to no connection, so
     * this asks for CONN_NONE by name rather than deriving a serial from
     * whichever session happened to start the fetch. */
    (void)sess;
    gtsk = gtask_with_trans (CONN_NONE, -129);
    if (!gtsk) {
        gtsk = gtask_new (CONN_NONE, -129, 0);
    }

    gtk_label_set_text (GTK_LABEL (gtsk->title), _ ("Connecting to tracker"));
    sub = g_strdup_printf (_ ("%1$s \xc2\xb7 %2$u of %3$u"), str ? str : "",
                           pos, tot);
    gtk_label_set_text (GTK_LABEL (gtsk->subtitle), sub);

    gtask_set_fraction (GTK_PROGRESS_BAR (gtsk->pbar), pos, tot);

    if (num >= total) {
        gtask_delete (gtsk);
    }
}

void
conn_task_update (session *sess, int stat)
{
    g_return_if_fail (sess != NULL);

    char sub[64];
    struct gtask *gtsk;
    /* Callers (toolbar.c / gtkhx.c) pass stat in {0, 1, 2}
     * representing the connection-phase step. The old code split
     * that into pos=stat/2 + len=2 which gave nonsense progress
     * (e.g. stat==2 -> "Step 1 of 3" at ~33%). Treat stat as the
     * step directly: "Step 0..2 of 2", fraction = stat/2. */
    guint32 pos = (guint32)(stat < 0 ? 0 : stat);
    const guint32 len = 2;
    if (pos > len) {
        pos = len;
    }

    gtsk = gtask_with_trans (sess_conn (sess), -128);
    if (!gtsk) {
        gtsk = gtask_new (sess_conn (sess), -128, 0);
    }

    gtk_label_set_text (GTK_LABEL (gtsk->title), _ ("Connecting"));
    g_snprintf (sub, sizeof (sub), _ ("Step %1$u of %2$u"), pos, len);
    gtk_label_set_text (GTK_LABEL (gtsk->subtitle), sub);

    gtask_set_fraction (GTK_PROGRESS_BAR (gtsk->pbar), pos, len);

    if (pos >= len) {
        gtask_delete (gtsk);
    }
}

void
task_update (session *sess, struct task *tsk)
{
    g_return_if_fail (sess != NULL);

    struct gtask *gtsk;
    /* tsk->pos / tsk->len are byte counts on the inbound TASK
     * reply: pos is bytes received so far, len is bytes still
     * to read, so pos + len is the announced total. */
    guint32 pos = tsk->pos;
    guint32 len = tsk->len;
    guint32 tot = pos + len;
    char posbuf[LONGEST_HUMAN_READABLE + 1];
    char totbuf[LONGEST_HUMAN_READABLE + 1];
    g_autofree char *posstr = NULL;
    g_autofree char *totstr = NULL;
    g_autofree char *sub = NULL;

    gtsk = gtask_with_trans (sess_conn (sess), tsk->trans);
    if (!gtsk) {
        gtsk = gtask_new (sess_conn (sess), tsk->trans, 0);
    }

    /* tsk->str is the human-friendly task description from task_new
     * (e.g. "Login", "Get file list", "Send chat"). Falls back to
     * the trans id when missing so the row isn't a mystery. */
    if (tsk->str && *tsk->str) {
        gtk_label_set_text (GTK_LABEL (gtsk->title), tsk->str);
    } else {
        g_autofree char *title = g_strdup_printf (_ ("Task 0x%x"), tsk->trans);
        gtk_label_set_text (GTK_LABEL (gtsk->title), title);
    }

    /* Subtitle: bytes received / total announced. The earlier
     * "Step %u of %u" labelling was misleading — these are not
     * discrete steps. human_size keeps the digits compact for
     * the typical KB-sized TASK replies (and stays readable on
     * the rare large ones). */
    posstr = g_strdup (human_size (posbuf, pos));
    totstr = g_strdup (human_size (totbuf, tot));
    sub = g_strdup_printf (_ ("%1$s of %2$s"), posstr, totstr);
    gtk_label_set_text (GTK_LABEL (gtsk->subtitle), sub);

    gtask_set_fraction (GTK_PROGRESS_BAR (gtsk->pbar), pos, tot);

    if (len == 0) {
        gtask_delete (gtsk);
    }
}

/* tasks_destroy retired. The Tasks
 * panel is a permanent resident of the toolbar's sidebar
 * PanelFrame; the standalone GtkWindow it used to hang under is
 * gone, so there's nothing to unparent on close. The panel still
 * marks itself constructed at creation time, so the rest of tasks.c
 * continues to gate worker-thread updates on that. */

extern void tracker_kill_threads (void);
static void
task_stop (GtkWidget *widget, gpointer data)
{
    struct gtask *gtsk;
    GList *sel, *lp, *next;
    GtkWidget *listitem;

    (void)data;
    if (!hx_panel_was_constructed (HX_PANEL_ID_TASKS)) {
        return;
    }

    /* gtk_list_box_get_selected_rows returns a GList* of
     * GtkListBoxRow* (the rows themselves, not their children).
     * Caller owns the GList and must g_list_free() it; the rows
     * themselves are owned by the list box. */
    sel = gtk_list_box_get_selected_rows (GTK_LIST_BOX (gtklist));
    for (lp = sel; lp; lp = next) {
        next = lp->next;
        listitem = (GtkWidget *)lp->data;
        gtsk = (struct gtask *)g_object_get_data (G_OBJECT (listitem), "gtsk");

        if (gtsk->htxf) {
            /* gtsk->htxf is guaranteed live here — the
             * GtkhxSession::xfer-destroyed handler
             * (gtask_clear_htxf, wired in gtkhx.c) sets this
             * pointer to NULL the moment the htxf leaves the
             * live xfers[] list, before any unref that might
             * free the slab. A pre-signal cancel-after-hang
             * crash (cancelling a worker that had already
             * exited) lives in the git history as a defensive
             * xfers[]-scan in this spot; the signal-based clear
             * obsoletes it. */
            xfer_delete (gtsk->htxf);
            gtask_delete (gtsk);
        } else if (gtsk->trans == (guint32)-127
                   || gtsk->trans == (guint32)-129) {
            /* Tracker cancel (-127) and tracker-quit (-129) both tear down
             * the tracker worker pool, and that removes both rows — there is
             * only ever one fetch, so there is nothing to be selective
             * about. */
            tracker_kill_threads ();
        } else if (gtsk->trans == (guint32)-128) {
            /* The connect row's Stop is a disconnect — of the connection the
             * row names. It used to be `disconnect_clicked`, which acts on
             * whichever connection is focused; from a shared queue that would
             * disconnect the server you are looking at because you asked to
             * stop a different one. Closing the connection drives
             * conn_task_update, which removes the row. */
            session *owner = hx_session_with_serial (gtsk->conn);

            if (owner != NULL && hx_conn_fd (owner->htlc)) {
                hx_htlc_close (owner->htlc, 1);
            } else {
                gtask_delete (gtsk);
            }
        } else {
            /* The row's own connection, not the focused one. The queue is
             * shared now, so the selection can name a task on a server the
             * user isn't looking at — cancelling it against `sess` would
             * have hunted for that transaction id in the wrong connection's
             * table and, on a collision, cancelled an unrelated task. NULL
             * once that connection has gone, in which case the row is stale
             * and only the row needs removing. */
            session *owner = hx_session_with_serial (gtsk->conn);

            struct task *tsk
                = owner ? task_with_trans (owner, gtsk->trans) : NULL;

            if (tsk != NULL) {
                /* task_delete removes the row on its way through. */
                task_delete (owner, tsk);
            } else {
                /* No model task behind it: the connection has gone, or the
                 * task finished without its row being cleared. task_delete
                 * returns early on a NULL task and would leave the row
                 * standing — and nothing else removes it now that closing a
                 * tab no longer destroys a page full of rows. */
                gtask_delete (gtsk);
            }
            /*			gtask_delete(sess, gtsk); */
        }
    }
    g_list_free (sel);
}

/* The index of the nearest row above or below `from` that is a transfer.
 *
 * `dir` is -1 for above, +1 for below. -1 when there is none.
 *
 * Reordering moves a transfer within the transfer queue, but the list holds
 * more than transfers: protocol tasks, each connection's connect row, the
 * tracker's progress. Moving the widget by one *visual* position would step it
 * past whichever of those happened to be adjacent while the queue swapped it
 * with a different transfer entirely, and the two orders would drift further
 * apart with every press. Interleaving connections made that common rather
 * than occasional. */
static int
gtklist_adjacent_xfer_index (GtkListBox *box, GtkWidget *from, int dir)
{
    int i = gtk_list_box_row_get_index (GTK_LIST_BOX_ROW (from));

    for (i += dir; i >= 0; i += dir) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index (box, i);
        struct gtask *other;

        if (row == NULL) {
            return -1;
        }
        other = g_object_get_data (G_OBJECT (row), "gtsk");
        if (other != NULL && other->htxf != NULL) {
            return i;
        }
    }
    return -1;
}

/* Move a GtkListBoxRow to a new index by ref'ing it,
 * removing it from the container, and re-inserting at the new index.
 * The list box re-acquires its ref on insert. */
static void
gtklist_row_move (GtkListBox *box, GtkWidget *row, int new_index)
{
    g_object_ref (row);
    gtkhx_widget_remove_child (GTK_WIDGET (box), row);
    gtk_list_box_insert (box, row, new_index);
    g_object_unref (row);
    gtk_list_box_select_row (box, GTK_LIST_BOX_ROW (row));
}

static void
task_up (GtkWidget *widget, gpointer data)
{
    struct gtask *gtsk;
    GList *sel;
    GtkWidget *listitem;
    int num, gtkpos;

    (void)data;

    if (!gtkhx_prefs.queuedl || !hx_panel_was_constructed (HX_PANEL_ID_TASKS)) {
        return;
    }

    sel = gtk_list_box_get_selected_rows (GTK_LIST_BOX (gtklist));
    if (!sel) {
        return;
    }
    listitem = sel->data;
    g_list_free (sel);
    gtsk = g_object_get_data (G_OBJECT (listitem), "gtsk");

    if (!gtsk->htxf) {
        return;
    }

    num = xfer_num (gtsk->htxf);

    /* xfer_up swaps `num` with `num - 1`, so index 1 — the second transfer in
     * the queue — is a legitimate move to the top. Only index 0 and the
     * not-in-the-queue -1 have nowhere to go. This used to reject 1 as well,
     * which pinned whatever was second in the queue. */
    if (num < 1) {
        return;
    }

    /* Where the transfer above it sits, before the queue swap moves either. */
    gtkpos = gtklist_adjacent_xfer_index (GTK_LIST_BOX (gtklist), listitem, -1);
    if (gtkpos < 0) {
        return;
    }

    xfer_up (num);
    gtklist_row_move (GTK_LIST_BOX (gtklist), listitem, gtkpos);
}

static void
task_dn (GtkWidget *widget, gpointer data)
{
    struct gtask *gtsk;
    GList *sel;
    GtkWidget *listitem;
    int num, gtkpos;

    (void)data;

    if (!gtkhx_prefs.queuedl || !hx_panel_was_constructed (HX_PANEL_ID_TASKS)) {
        return;
    }
    sel = gtk_list_box_get_selected_rows (GTK_LIST_BOX (gtklist));
    if (!sel) {
        return;
    }
    listitem = sel->data;
    g_list_free (sel);
    gtsk = g_object_get_data (G_OBJECT (listitem), "gtsk");

    if (!gtsk->htxf) {
        return;
    }

    num = xfer_num (gtsk->htxf);

    /* xfer_num answers -1 for "not in the queue" and a 0-based index
     * otherwise, so index 0 — the transfer at the top — is a legitimate move
     * down. This used to reject it. */
    if (num < 0) {
        return;
    }

    gtkpos = gtklist_adjacent_xfer_index (GTK_LIST_BOX (gtklist), listitem, 1);
    if (gtkpos < 0) {
        return;
    }

    if (xfer_down (num)) {
        return;
    }
    gtklist_row_move (GTK_LIST_BOX (gtklist), listitem, gtkpos);
}

static void
task_go (GtkWidget *widget, gpointer data)
{
    struct gtask *gtsk;
    GList *sel;
    GtkWidget *listitem;

    (void)data;
    if (!hx_panel_was_constructed (HX_PANEL_ID_TASKS)) {
        return;
    }

    sel = gtk_list_box_get_selected_rows (GTK_LIST_BOX (gtklist));
    if (!sel) {
        return;
    }
    listitem = sel->data;
    g_list_free (sel);
    gtsk = (struct gtask *)g_object_get_data (G_OBJECT (listitem), "gtsk");
    if (gtsk->htxf) {
        xfer_go (gtsk->htxf);
    }
}

static void
task_tasks_update (session *sess)
{
    GHashTableIter iter;
    gpointer val;

    if (!sess->tasks) {
        return;
    }
    g_hash_table_iter_init (&iter, sess->tasks);
    while (g_hash_table_iter_next (&iter, NULL, &val)) {
        gtkhx_session_emit_task_update (gtkhx_session_get_default (), sess,
                                        (struct task *)val);
    }
}

/* Tasks-headerbar pixmap buttons share the GTKHX_SCALE_WINDOW_BUTTONS
 * theme area with the other secondary windows (Users / Files / News /
 * Tracker). The default theme renders that area at 200% — the old
 * hard-coded 2x — and the user can retune it in Settings → Appearance.
 * gtkhx_pixmap_button in gtkutil.c handles the upscale + button
 * construction. */
static GtkWidget *
tasks_pixmap_button (const char *resource_name, const char *tooltip,
                     GCallback cb, gpointer user_data)
{
    return gtkhx_pixmap_button (resource_name, tooltip,
                                GTKHX_SCALE_WINDOW_BUTTONS, cb, user_data);
}

/* Content build for the Rust Tasks window shell (gtkhx-ui `tasks`). The
 * dock registration moved to Rust via dock_bridge; the C content leaves —
 * the Stop/Start/Up/Down action buttons wired to the static task_* handlers
 * plus the task-list scroller built in create_tasks — are assembled here
 * and handed back as one still-floating container. Mirrors
 * users_bridge.c::gtkhx_users_bridge_build_content. */
GtkWidget *
gtkhx_tasks_build_content (session *sess)
{
    GtkWidget *stopbtn, *gobtn, *upbtn, *dnbtn;
    GtkWidget *content_vbox;
    GtkWidget *button_bar;

    g_return_val_if_fail (sess != NULL, NULL);

    stopbtn
        = tasks_pixmap_button ("/com/nasledov/gtkhx/pixmaps/kick.png",
                               _ ("Stop Task"), G_CALLBACK (task_stop), sess);
    gobtn = tasks_pixmap_button ("/com/nasledov/gtkhx/pixmaps/start.png",
                                 _ ("Start Task"), G_CALLBACK (task_go), sess);
    upbtn = tasks_pixmap_button ("/com/nasledov/gtkhx/pixmaps/up.png",
                                 _ ("Move Xfer Up in Queue"),
                                 G_CALLBACK (task_up), sess);
    dnbtn = tasks_pixmap_button ("/com/nasledov/gtkhx/pixmaps/down.png",
                                 _ ("Move Xfer Down in Queue"),
                                 G_CALLBACK (task_dn), sess);

    /* The four action buttons (Stop/Start on start, Up/Down on end)
     * relocate to a slim top-of-content GtkBox with an hexpand spacer
     * keeping the start/end grouping the old headerbar implied. */
    button_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start (button_bar, 6);
    gtk_widget_set_margin_end (button_bar, 6);
    gtk_widget_set_margin_top (button_bar, 6);
    gtk_widget_set_margin_bottom (button_bar, 4);
    gtk_box_append (GTK_BOX (button_bar), stopbtn);
    gtk_box_append (GTK_BOX (button_bar), gobtn);
    {
        GtkWidget *spacer = gtk_label_new (NULL);
        gtk_widget_set_hexpand (spacer, TRUE);
        gtk_box_append (GTK_BOX (button_bar), spacer);
    }
    gtk_box_append (GTK_BOX (button_bar), upbtn);
    gtk_box_append (GTK_BOX (button_bar), dnbtn);

    content_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (content_vbox), button_bar);
    gtk_box_append (GTK_BOX (content_vbox), gtask_scroll);
    return content_vbox;
}

void
gtkhx_tasks_after_embed (session *sess)
{
    g_return_if_fail (sess != NULL);

    hx_panel_mark_constructed (HX_PANEL_ID_TASKS);
    gtkhx_tasks_sync_conn (sess);
}

/* Put one connection's tasks and transfers into the queue.
 *
 * Split out of after_embed because the queue is shared: only the first
 * connection builds the panel, so every connection after it has state that
 * would otherwise never reach the list. Re-emitting is safe — both update
 * paths find an existing row or make one. */
void
gtkhx_tasks_sync_conn (session *sess)
{
    g_return_if_fail (sess != NULL);

    task_tasks_update (sess);
    xfer_tasks_update (sess->htlc);
    gtkhx_tasks_refresh_tags ();
}

/* LONGEST_HUMAN_READABLE + human_size come in via human_readable.h
 * now (transitively via files.h's include chain). */

void
file_update (session *sess, struct htxf_conn *htxf)
{
    g_return_if_fail (sess != NULL);

    struct gtask *gtsk;
    char humanbuf[LONGEST_HUMAN_READABLE + 1];
    g_autofree char *posstr = NULL;
    g_autofree char *sizestr = NULL;
    g_autofree char *bpsstr = NULL;
    g_autofree char *basename = NULL;
    g_autofree char *subtitle = NULL;
    /* htxf->total_pos / total_size are guint64 to support the
     * Large-File extension (>4 GiB transfers). Earlier code stuffed
     * them into guint32 and quietly broke past the 32-bit cap. */
    guint64 pos, size, Bps, remaining, eta;
    struct timeval now;
    time_t sdiff;
    int hrs, mins, secs;
    gboolean title_set;

    gtsk = gtask_with_htxf (htxf);
    if (!gtsk) {
        gtsk = gtask_new (sess_conn (sess), 0, htxf);
    }

    pos = hx_htxf_total_pos (htxf);
    size = htxf->total_size;

    gettimeofday (&now, 0);
    sdiff = now.tv_sec - htxf->start.tv_sec;
    if (sdiff < 1) {
        sdiff = 1;
    }
    Bps = pos / (guint64)sdiff;
    if (Bps == 0) {
        Bps = 1;
    }

    /* ETA = remaining bytes / Bps, rounded UP so the displayed
     * countdown ticks down to 0 instead of clipping the last
     * fractional second. The earlier code wrote
     *   (size-pos)/Bps + ((size-pos)%Bps)/Bps
     * which is dead-on-arrival in integer math: the second term
     * is (a value < Bps) / Bps == 0.
     *
     * Guard against pos > size first — that case shows up when
     * total_size is a placeholder (1 for unknown-size folder
     * transfers, 0 for not-yet-stamped fresh htxf rows) and the
     * counter creeps past it. Unsigned subtraction would wrap to
     * an absurd remaining value otherwise. */
    if (size > pos) {
        remaining = size - pos;
        eta = (remaining + Bps - 1) / Bps;
    } else {
        eta = 0;
    }

    hrs = (int)(eta / 3600);
    eta %= 3600;
    mins = (int)(eta / 60);
    secs = (int)(eta % 60);

    posstr = g_strdup (human_size (humanbuf, pos));
    memset (&humanbuf, 0, sizeof (humanbuf));
    sizestr = g_strdup (human_size (humanbuf, size));
    memset (&humanbuf, 0, sizeof (humanbuf));
    bpsstr = g_strdup (human_size (humanbuf, Bps));

    /* Title: just the filename. htxf->path is the local-filesystem
     * path (g_path_get_basename uses POSIX '/' which is what xfers.c
     * builds), so the basename is the bare filename. Tooltip carries
     * the full path for the curious. We only set the title once per
     * row — htxf->path doesn't change after xfer_new. */
    title_set = (gtk_label_get_text (GTK_LABEL (gtsk->title))[0] != '\0');
    if (!title_set) {
        basename = g_path_get_basename (htxf->path);
        gtk_label_set_text (GTK_LABEL (gtsk->title), basename);
        gtk_widget_set_tooltip_text (gtsk->title, htxf->path);
    }

    /* Subtitle: structured stable-width metrics. Middle dots
     * (U+00B7) separate the segments — easier to scan than a string
     * of spaces, and the dim-label CSS makes the whole line read as
     * supporting text below the title. Tabular-nums (CSS feature)
     * keeps the digits from shifting siblings as values churn.
     *
     * Format:  3.2 MB of 12.5 MB \xb7 150 KB/s \xb7 ETA 1:15
     *
     * The "ETA" prefix matches Adwaita / GNOME Files conventions
     * and avoids the trailing path that the old format glued on. */
    if (hrs > 0) {
        subtitle = g_strdup_printf (
            _ ("%1$s of %2$s \xc2\xb7 %3$s/s \xc2\xb7 ETA %4$d:%5$02d:%6$02d"),
            posstr, sizestr, bpsstr, hrs, mins, secs);
    } else {
        subtitle = g_strdup_printf (
            _ ("%1$s of %2$s \xc2\xb7 %3$s/s \xc2\xb7 ETA %4$d:%5$02d"), posstr,
            sizestr, bpsstr, mins, secs);
    }
    gtk_label_set_text (GTK_LABEL (gtsk->subtitle), subtitle);

    if (size > 0 && pos <= size) {
        /* gdouble (not gfloat) so multi-GB transfers don't lose
         * precision in the divide — gfloat only has ~7 decimal
         * digits of mantissa, which starts to dither at the
         * gigabyte scale. */
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (gtsk->pbar),
                                       (gdouble)pos / (gdouble)size);
    }

    if (pos >= size) {
        gtask_delete (gtsk);
    }
}

/* The transaction table model — tasks_table_new / task_free / tasks_init /
 * task_new / task_with_trans / task_delete — is the Rust `hxtask` crate
 * (rust/crates/hxtask). It keeps sess->tasks a real GHashTable and preserves the
 * exact C ABI these callers link against; gtask_delete_tsk (above) is the view
 * hook task_delete calls before removing the model entry. The field accessors
 * the crate needs (hx_session_tasks / hx_session_set_tasks / hx_htlc_trans) live
 * in tasks_bridge.c. */

/* task_error_extract lives in proto_helpers.c so the Tier 2 unit
 * tests can drive it without a GTK build. The prototype is in
 * tasks.h via #include "proto_helpers.h". */

void
task_error (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    char errormsg[8192 + 1];
    gsize len = 0;

    (void)htlc;
    if (!task_error_extract (frame, frame_len, errormsg, sizeof (errormsg),
                             &len)) {
        return;
    }

    (void)len;
    /* server task errors used to pop a modal
     * error_dialog. That's too heavy for the common case —
     * the server rejecting one of our auto-fired bootstrap
     * requests (news fetch on a no-news-permission account,
     * user list on a server that rate-limits, etc.) made
     * every login on certain servers (hlserver.com is the
     * known-bad example) yield a dialog the user has to
     * click through before they can do anything else.
     *
     * Toast instead: AdwToast slides in over the toolbar
     * window's overlay, auto-dismisses after a few seconds,
     * doesn't steal focus. The user still sees the message;
     * they're just not blocked by it. The ERROR sound still
     * fires so the alert isn't fully silent.
     *
     * errormsg may contain MacRoman bytes (Mac servers
     * commonly hand us curly quotes \xd2/\xd3 around filenames).
     * toolbar_show_toast sanitises to UTF-8 internally — the
     * accessibility announcement layer behind AdwToast aborts on
     * non-UTF-8, so the defence has to be at that choke point. */
    toolbar_show_toast (errormsg);
    play_sound (ERROR);
}
