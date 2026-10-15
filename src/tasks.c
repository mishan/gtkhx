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
#include <stdlib.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <adwaita.h>
#include <sys/time.h>
#include "hx.h"
#include "gtkhx_session.h"
#include "network.h"
#include "gtkutil.h"
#include "gtkhx_theme.h" /* gtkhx_theme_scale, GTKHX_SCALE_TASKS_ROW_ICON */
#include "gtkhx.h"
#include "gtkhx_icon.h"
#include "xfers.h"
#include "htxf_accessors.h"
#include "hotline_proto.h"
#include "sound.h"
#include "toolbar.h" /* disconnect_clicked, toolbar_show_toast */
#include "tasks.h"
#include "session_registry.h"
#include "panel_registry.h"
#include "dock_bridge.h" /* raising the queue on a new transfer */

#undef _
#include <glib/gi18n.h>

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
 * updatable widgets so file_update can refresh
 * them in place without rebuilding the row. */
struct gtask {
    struct gtask *next, *prev;
    /* The connection this row belongs to, by serial. The queue is one list
     * for the whole app — "is anything still going?" should be one place to
     * look, not a hunt across tabs — so every row has to say whose it is.
     *
     * A serial rather than a session pointer for the usual reason: a row can
     * outlive the tab that made it by the length of one main-loop turn, and
     * closing a tab frees the session. */
    guint16 conn;
    struct htxf_conn *htxf;
    GtkWidget *icon;
    GtkWidget *title;
    GtkWidget *subtitle;
    GtkWidget *pbar;
    GtkWidget *listitem;
    GtkWidget *queue; /* badge on the title row */
    GtkWidget *tag;   /* which connection this row is on; see tags_refresh */
};

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
static GtkWidget *pausebtn, *gobtn; /* sensitive to the selection */

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

/* The row icon: the same retro pixmaps the toolbar carries, so the
 * Transfers panel keeps the period-appropriate look. The pixmaps are 16x16
 * originals; gtask_make_icon scales them to the theme's row size. */
static const char *
gtask_icon_for (struct htxf_conn *htxf)
{
    return htxf->type == XFER_GET ? "/com/nasledov/gtkhx/pixmaps/download.png"
                                  : "/com/nasledov/gtkhx/pixmaps/upload.png";
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
    const char *symbolic = gtkhx_icon_symbolic_name (resource_path);

    /* A theme that uses symbolic icons: the named icon at the same
     * size, drawn in the row's CSS color. */
    if (symbolic) {
        GtkWidget *image = gtk_image_new_from_icon_name (symbolic);
        gtk_image_set_pixel_size (GTK_IMAGE (image), px);
        return image;
    }

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

static void tasks_sync_buttons (GtkListBox *box, gpointer data);

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

    /* A GtkListBoxRow per transfer, gtsk->listitem, holding its gtsk as
     * object data. */
    gtklist = gtk_list_box_new ();
    gtk_accessible_update_property (GTK_ACCESSIBLE (gtklist),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL,
                                    _ ("Transfers"), -1);
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (gtklist),
                                     GTK_SELECTION_MULTIPLE);
    /* Activating on a single click would make a click add to the selection. */
    gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (gtklist), FALSE);
    /* Follow the active GtkHx theme's fg/bg via .gtkhx-listview. */
    gtkhx_apply_listview_style (gtklist);
    g_object_ref_sink (gtklist);
    g_signal_connect (gtklist, "selected-rows-changed",
                      G_CALLBACK (tasks_sync_buttons), NULL);

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

static void gtask_delete (struct gtask *gtsk);

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
        gtk_label_set_text (GTK_LABEL (gtsk->queue), "");
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
gtask_new (guint16 conn, struct htxf_conn *htxf)
{
    GtkWidget *row_box;     /* outer hbox: icon | vbox */
    GtkWidget *content_box; /* inner vbox: title-row, subtitle, pbar */
    GtkWidget *title_row;   /* inner hbox: title (hexpand) | queue badge */
    GtkWidget *icon, *title, *subtitle, *pbar, *queue, *listitem, *tag;
    struct gtask *gtsk;
    gboolean transfer_is_first;

    /* Whether a transfer row already exists, before this one joins. */
    transfer_is_first = TRUE;
    for (struct gtask *t = gtask_list; t != NULL; t = t->prev) {
        if (t->htxf != NULL) {
            transfer_is_first = FALSE;
            break;
        }
    }

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
    icon = gtask_make_icon (gtask_icon_for (htxf));
    gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
    /* The title is only the file name; the icon is what tells a download
     * from an upload. */
    gtk_accessible_update_property (
        GTK_ACCESSIBLE (icon), GTK_ACCESSIBLE_PROPERTY_LABEL,
        htxf->type == XFER_GET ? C_ ("files transfer", "Download")
                               : C_ ("files transfer", "Upload"),
        -1);

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

    /* Queue badge. Starts hidden; gtask_refresh_queue_badge flips it on
     * when htxf->queue > 0. */
    queue = gtk_label_new ("");
    gtk_widget_add_css_class (queue, "gtkhx-task-queue");
    gtk_widget_set_valign (queue, GTK_ALIGN_CENTER);
    gtk_widget_set_visible (queue, FALSE);
    gtask_apply_smaller_font (GTK_LABEL (queue), PANGO_SCALE_X_SMALL);

    /* Progress bar — slim Adwaita-styled bar via CSS min-height,
     * no overlaid percent text (subtitle carries position info). */
    pbar = gtk_progress_bar_new ();
    gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (pbar), FALSE);
    gtk_widget_set_valign (pbar, GTK_ALIGN_CENTER);
    /* Named by the file, described by the speed and ETA beside it. */
    gtk_accessible_update_relation (
        GTK_ACCESSIBLE (pbar), GTK_ACCESSIBLE_RELATION_LABELLED_BY, title, NULL,
        GTK_ACCESSIBLE_RELATION_DESCRIBED_BY, subtitle, NULL, -1);

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

    /* Top-row hbox: title (hexpand) + queue badge + connection. */
    title_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtkhx_box_pack (title_row, title, 1, 1, 0);
    gtkhx_box_pack (title_row, queue, 0, 0, 0);
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
    /* By reference, so the name follows the progress without announcing it.
     * A hidden label still counts, so the badge and tag are emptied as they
     * hide. The tag reads best beside the file name, not after the badge. */
    gtk_accessible_update_relation (GTK_ACCESSIBLE (listitem),
                                    GTK_ACCESSIBLE_RELATION_LABELLED_BY, title,
                                    tag, queue, subtitle, NULL, -1);
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
    gtsk->htxf = htxf;
    gtsk->queue = queue;
    gtsk->tag = tag;
    gtsk->conn = conn;
    gtask_list = gtsk;

    /* Initial queue-badge state if the htxf came in pre-queued. */
    gtask_refresh_queue_badge (gtsk);
    gtkhx_tasks_refresh_tags ();

    /* A transfer brings the queue forward. Transfers shares a frame by
     * default (it has no leaf of its own), so without this a download
     * started from Files would progress out of sight. Only:
     *   - transfers, not previews: a preview is too brief to be worth
     *     switching tabs for;
     *   - the connection the user is looking at, not one in the
     *     background;
     *   - the first transfer of a burst: a folder download or a queue
     *     of files would otherwise pull Transfers forward again every time
     *     the user went back to what they were reading;
     *   - a panel that is docked: one the user closed stays closed. */
    if (!hx_htxf_opt_preview (htxf) && transfer_is_first
        && hx_active_session () != NULL
        && conn == hx_conn_serial (hx_active_session ()->htlc)) {
        gtkhx_dock_show_if_open (HX_PANEL_ID_TASKS);
    }

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

    for (gtsk = gtask_list; gtsk; gtsk = prev) {
        prev = gtsk->prev;
        if (gtsk->conn == conn) {
            gtask_delete (gtsk);
        }
    }
    gtkhx_tasks_refresh_tags ();
}

/* How many rows the queue is holding for `conn`. Debug-only: the sweep on
 * disconnect has no other visible effect in a headless run, and "the shared
 * queue kept a dead connection's rows" is the failure it guards against. */
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

        gtk_widget_set_visible (gtsk->tag, show);
        if (!show) {
            gtk_label_set_text (GTK_LABEL (gtsk->tag), "");
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
        }
        gtask_delete (gtsk);
    }
    g_list_free (sel);
}

/* The index of the nearest row above or below `from` that is still a live
 * transfer.
 *
 * `dir` is -1 for above, +1 for below. -1 when there is none.
 *
 * A row outlives its transfer by a moment (gtask_clear_htxf), and stepping
 * the widget past one of those while the queue swapped it with a different
 * transfer would drift the two orders apart. */
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

/* The first selected row's live transfer, or NULL; `row` gets the row. */
static struct htxf_conn *
selected_xfer (GtkWidget **row)
{
    GList *sel;
    struct gtask *gtsk = NULL;

    if (!hx_panel_was_constructed (HX_PANEL_ID_TASKS)) {
        return NULL;
    }
    sel = gtk_list_box_get_selected_rows (GTK_LIST_BOX (gtklist));
    if (sel) {
        *row = sel->data;
        gtsk = g_object_get_data (G_OBJECT (sel->data), "gtsk");
    }
    g_list_free (sel);
    return gtsk ? gtsk->htxf : NULL;
}

static void
task_up (GtkWidget *widget, gpointer data)
{
    GtkWidget *listitem;
    struct htxf_conn *htxf = selected_xfer (&listitem);
    int num, gtkpos;

    (void)data;
    if (!gtkhx_prefs.queuedl || !htxf) {
        return;
    }

    num = xfer_num (htxf);

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
    GtkWidget *listitem;
    struct htxf_conn *htxf = selected_xfer (&listitem);
    int num, gtkpos;

    (void)data;
    if (!gtkhx_prefs.queuedl || !htxf) {
        return;
    }

    num = xfer_num (htxf);

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
    GtkWidget *row;
    struct htxf_conn *htxf = selected_xfer (&row);

    (void)data;
    if (htxf) {
        xfer_go (htxf);
    }
}

static void
task_pause (GtkWidget *widget, gpointer data)
{
    GtkWidget *row;
    struct htxf_conn *htxf = selected_xfer (&row);

    (void)data;
    if (htxf) {
        xfer_pause (htxf);
    }
}

/* Pause and Start are offered only where they would do something. */
static void
tasks_sync_buttons (GtkListBox *box, gpointer data)
{
    GtkWidget *row;
    struct htxf_conn *htxf = selected_xfer (&row);

    (void)box;
    (void)data;
    if (pausebtn && gobtn) {
        gtk_widget_set_sensitive (pausebtn, htxf && xfer_can_pause (htxf));
        gtk_widget_set_sensitive (gobtn, htxf && xfer_can_start (htxf));
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
 * the Stop/Pause/Start/Up/Down action buttons wired to the static task_*
 * handlers plus the task-list scroller built in create_tasks — are assembled
 * here and handed back as one still-floating container. Mirrors
 * users_bridge.c::gtkhx_users_bridge_build_content. */
GtkWidget *
gtkhx_tasks_build_content (session *sess)
{
    GtkWidget *stopbtn, *upbtn, *dnbtn;
    GtkWidget *content_vbox;
    GtkWidget *button_bar;

    g_return_val_if_fail (sess != NULL, NULL);

    stopbtn = tasks_pixmap_button ("/com/nasledov/gtkhx/pixmaps/stop.png",
                                   _ ("Stop Transfer"), G_CALLBACK (task_stop),
                                   sess);
    pausebtn = tasks_pixmap_button ("/com/nasledov/gtkhx/pixmaps/pause.png",
                                    _ ("Pause Transfer"),
                                    G_CALLBACK (task_pause), sess);
    g_object_add_weak_pointer (G_OBJECT (pausebtn), (gpointer *)&pausebtn);
    gobtn = tasks_pixmap_button ("/com/nasledov/gtkhx/pixmaps/start.png",
                                 _ ("Start Transfer"), G_CALLBACK (task_go),
                                 sess);
    g_object_add_weak_pointer (G_OBJECT (gobtn), (gpointer *)&gobtn);
    tasks_sync_buttons (NULL, NULL);
    upbtn = tasks_pixmap_button ("/com/nasledov/gtkhx/pixmaps/up.png",
                                 _ ("Move Transfer Up in Queue"),
                                 G_CALLBACK (task_up), sess);
    dnbtn = tasks_pixmap_button ("/com/nasledov/gtkhx/pixmaps/down.png",
                                 _ ("Move Transfer Down in Queue"),
                                 G_CALLBACK (task_dn), sess);

    /* The action buttons (Stop/Pause/Start on start, Up/Down on end)
     * relocate to a slim top-of-content GtkBox with an hexpand spacer
     * keeping the start/end grouping the old headerbar implied. */
    button_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class (button_bar, "gtkhx-panel-actions");
    gtk_box_append (GTK_BOX (button_bar), stopbtn);
    gtk_box_append (GTK_BOX (button_bar), pausebtn);
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

/* Put one connection's transfers into the queue.
 *
 * Split out of after_embed because the queue is shared: only the first
 * connection builds the panel, so every connection after it has state that
 * would otherwise never reach the list. Re-emitting is safe — the update
 * finds an existing row or makes one. */
void
gtkhx_tasks_sync_conn (session *sess)
{
    g_return_if_fail (sess != NULL);

    xfer_tasks_update (sess->htlc);
    gtkhx_tasks_refresh_tags ();
}

void
file_update (session *sess, struct htxf_conn *htxf)
{
    g_return_if_fail (sess != NULL);

    struct gtask *gtsk;
    g_autofree char *basename = NULL;
    g_autofree char *subtitle = NULL;
    /* A resumed download's progress counts what it already had. */
    guint64 run = hx_htxf_total_pos (htxf), had = xfer_resumed_bytes (htxf);
    guint64 pos = run + had, size = htxf->total_size + had;
    struct timeval now;

    gtsk = gtask_with_htxf (htxf);
    if (!gtsk) {
        gtsk = gtask_new (hx_conn_serial (sess->htlc), htxf);
    }

    /* Title: just the filename. htxf->path is the local-filesystem
     * path (g_path_get_basename uses POSIX '/' which is what xfers.c
     * builds), so the basename is the bare filename. Tooltip carries
     * the full path for the curious. We only set the title once per
     * row — htxf->path doesn't change after xfer_new. */
    if (gtk_label_get_text (GTK_LABEL (gtsk->title))[0] == '\0') {
        basename = g_path_get_basename (htxf->path);
        gtk_label_set_text (GTK_LABEL (gtsk->title), basename);
        gtk_widget_set_tooltip_text (gtsk->title, htxf->path);
    }

    gettimeofday (&now, 0);
    subtitle = gtkhx_tasks_subtitle (
        pos, size, run, now.tv_sec - htxf->start.tv_sec, xfer_paused (htxf));
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
    tasks_sync_buttons (NULL, NULL);
}

/* task_error_extract lives in proto_helpers.c so the Tier 2 unit
 * tests can drive it without a GTK build. The prototype is in
 * tasks.h via #include "proto_helpers.h". */

void
task_error (struct htlc_conn *htlc, const guint8 *frame, gsize frame_len)
{
    char errormsg[8192 + 1];
    gsize len = 0;

    (void)htlc;
    if (!gtkhx_proto_header_in_error (frame, frame_len)
        || !task_error_extract (frame, frame_len, errormsg, sizeof (errormsg),
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
