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
#include <netinet/in.h>
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
#include "sound.h"
#include "toolbar.h" /* disconnect_clicked, toolbar_show_toast */
#include "tasks.h"

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
    guint32 trans;
    struct htxf_conn *htxf;
    GtkWidget *icon;
    GtkWidget *title;
    GtkWidget *subtitle;
    GtkWidget *pbar;
    GtkWidget *listitem;
    GtkWidget *queue; /* badge on the title row; NULL for non-xfer tasks */
};

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
    static const char tasks_css[] =
        ".gtkhx-task-row {"
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
    switch ((gint32) trans) {
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
    int px = (int) (GTASK_ICON_SRC_SIZE * scale + 0.5);

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
        int w = (int) (gdk_pixbuf_get_width (src)  * scale + 0.5);
        int h = (int) (gdk_pixbuf_get_height (src) * scale + 0.5);
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
create_tasks (session *sess)
{
    GtkWidget *gtklist, *gtask_scroll;

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

    sess->gtklist = gtklist;
    sess->gtask_scroll = gtask_scroll;
}

static struct gtask *
gtask_with_trans (session *sess, guint32 trans)
{
    struct gtask *gtsk;

    for (gtsk = sess->gtask_list; gtsk; gtsk = gtsk->prev) {
        if (gtsk->trans == trans) {
            return gtsk;
        }
    }

    return 0;
}

static struct gtask *
gtask_with_htxf (session *sess, struct htxf_conn *htxf)
{
    struct gtask *gtsk;

    for (gtsk = sess->gtask_list; gtsk; gtsk = gtsk->prev) {
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
    struct gtask *gtsk = gtask_with_htxf (sess, htxf);

    if (!gtsk) {
        return;
    }
    gtask_refresh_queue_badge (gtsk);
}

static struct gtask *
gtask_new (session *sess, guint32 trans, struct htxf_conn *htxf)
{
    GtkWidget *row_box;        /* outer hbox: icon | vbox */
    GtkWidget *content_box;    /* inner vbox: title-row, subtitle, pbar */
    GtkWidget *title_row;      /* inner hbox: title (hexpand) | queue badge */
    GtkWidget *icon, *title, *subtitle, *pbar, *queue, *listitem;
    struct gtask *gtsk;

    gtsk = g_malloc (sizeof (struct gtask));
    gtsk->next = 0;
    gtsk->prev = sess->gtask_list;
    if (sess->gtask_list) {
        sess->gtask_list->next = gtsk;
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

    /* Top-row hbox: title (hexpand) + optional queue badge. */
    title_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtkhx_box_pack (title_row, title, 1, 1, 0);
    if (queue) {
        gtkhx_box_pack (title_row, queue, 0, 0, 0);
    }

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

    if (sess->gtklist) {
        gtk_list_box_insert (GTK_LIST_BOX (sess->gtklist), listitem, -1);
    }

    gtsk->icon = icon;
    gtsk->title = title;
    gtsk->subtitle = subtitle;
    gtsk->pbar = pbar;
    gtsk->listitem = listitem;
    gtsk->trans = trans;
    gtsk->htxf = htxf;
    gtsk->queue = queue;
    sess->gtask_list = gtsk;

    /* Initial queue-badge state if the htxf came in pre-queued. */
    gtask_refresh_queue_badge (gtsk);

    return gtsk;
}

static void
gtask_delete (session *sess, struct gtask *gtsk)
{
    if (sess->gtklist) {
        gtkhx_widget_remove_child (sess->gtklist, gtsk->listitem);
    }
    if (gtsk->next) {
        gtsk->next->prev = gtsk->prev;
    }
    if (gtsk->prev) {
        gtsk->prev->next = gtsk->next;
    }
    if (gtsk == sess->gtask_list) {
        sess->gtask_list = gtsk->prev;
    }
    g_free (gtsk);
}

void
gtask_delete_htxf (session *sess, struct htxf_conn *htxf)
{
    struct gtask *gtsk = gtask_with_htxf (sess, htxf);
    if (!gtsk) {
        return;
    }
    gtask_delete (sess, gtsk);
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
    struct gtask *gtsk = gtask_with_htxf (sess, htxf);
    if (!gtsk) {
        return;
    }
    gtsk->htxf = NULL;
}

void
gtask_delete_tsk (session *sess, guint32 trans)
{
    struct gtask *gtsk = gtask_with_trans (sess, trans);
    if (!gtsk) {
        return;
    }
    gtask_delete (sess, gtsk);
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
    frac = (gdouble) num / (gdouble) total;
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
    guint32 pos = (guint32) (num < 0 ? 0 : num);
    guint32 tot = (guint32) (total < 0 ? 0 : total);
    g_autofree char *sub = NULL;

    gtsk = gtask_with_trans (sess, -127);
    if (!gtsk) {
        gtsk = gtask_new (sess, -127, 0);
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
        gtask_delete (sess, gtsk);
    }
}

void
trackconn_prog_update (session *sess, char *str, int num, int total)
{
    struct gtask *gtsk;
    guint32 pos = (guint32) (num < 0 ? 0 : num);
    guint32 tot = (guint32) (total < 0 ? 0 : total);
    g_autofree char *sub = NULL;

    gtsk = gtask_with_trans (sess, -129);
    if (!gtsk) {
        gtsk = gtask_new (sess, -129, 0);
    }

    gtk_label_set_text (GTK_LABEL (gtsk->title), _ ("Connecting to tracker"));
    sub = g_strdup_printf (_ ("%1$s \xc2\xb7 %2$u of %3$u"),
                           str ? str : "", pos, tot);
    gtk_label_set_text (GTK_LABEL (gtsk->subtitle), sub);

    gtask_set_fraction (GTK_PROGRESS_BAR (gtsk->pbar), pos, tot);

    if (num >= total) {
        gtask_delete (sess, gtsk);
    }
}

void
conn_task_update (session *sess, int stat)
{
    char sub[64];
    struct gtask *gtsk;
    /* Callers (toolbar.c / gtkhx.c) pass stat in {0, 1, 2}
	 * representing the connection-phase step. The old code split
	 * that into pos=stat/2 + len=2 which gave nonsense progress
	 * (e.g. stat==2 -> "Step 1 of 3" at ~33%). Treat stat as the
	 * step directly: "Step 0..2 of 2", fraction = stat/2. */
    guint32 pos = (guint32) (stat < 0 ? 0 : stat);
    const guint32 len = 2;
    if (pos > len) {
        pos = len;
    }

    gtsk = gtask_with_trans (sess, -128);
    if (!gtsk) {
        gtsk = gtask_new (sess, -128, 0);
    }

    gtk_label_set_text (GTK_LABEL (gtsk->title), _ ("Connecting"));
    g_snprintf (sub, sizeof (sub), _ ("Step %1$u of %2$u"), pos, len);
    gtk_label_set_text (GTK_LABEL (gtsk->subtitle), sub);

    gtask_set_fraction (GTK_PROGRESS_BAR (gtsk->pbar), pos, len);

    if (pos >= len) {
        gtask_delete (sess, gtsk);
    }
}

void
task_update (session *sess, struct task *tsk)
{
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

    gtsk = gtask_with_trans (sess, tsk->trans);
    if (!gtsk) {
        gtsk = gtask_new (sess, tsk->trans, 0);
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
        gtask_delete (sess, gtsk);
    }
}

/* tasks_destroy retired. The Tasks
 * panel is a permanent resident of the toolbar's sidebar
 * PanelFrame; the standalone GtkWindow it used to hang under is
 * gone, so there's nothing to unparent on close. The
 * gtkhx_prefs.geo.tasks.open flag still flips at panel creation
 * time so the rest of tasks.c continues to gate worker-thread
 * updates on it. */

extern void tracker_kill_threads (void);
static void
task_stop (GtkWidget *widget, gpointer data)
{
    struct gtask *gtsk;
    GList *sel, *lp, *next;
    GtkWidget *listitem;
    session *sess = data;

    if (!gtkhx_prefs.geo.tasks.open) {
        return;
    }

    /* gtk_list_box_get_selected_rows returns a GList* of
	 * GtkListBoxRow* (the rows themselves, not their children).
	 * Caller owns the GList and must g_list_free() it; the rows
	 * themselves are owned by the list box. */
    sel = gtk_list_box_get_selected_rows (GTK_LIST_BOX (sess->gtklist));
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
            gtask_delete (sess, gtsk);
        } else if (gtsk->trans == (guint32)-127
                   || gtsk->trans == (guint32)-129) {
            /* Tracker cancel (-127) and tracker-quit (-129) both
             * tear down the tracker worker pool. */
            tracker_kill_threads ();
            gtask_delete (sess, gtsk);
        } else if (gtsk->trans == (guint32)-128) {
            disconnect_clicked ();
            /* disconnect_clicked updates connection task, so it should already
			   handle deleting the task */
            /*			gtask_delete(sess, gtsk); */
        } else {
            /* task_delete should handle deleting the gtask */
            task_delete (sess, task_with_trans (sess, gtsk->trans));
            /*			gtask_delete(sess, gtsk); */
        }
    }
    g_list_free (sel);
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
    session *sess = data;

    if (!gtkhx_prefs.queuedl) {
        return;
    }

    sel = gtk_list_box_get_selected_rows (GTK_LIST_BOX (sess->gtklist));
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

    if (num <= 1) {
        return;
    }

    xfer_up (num);

    gtkpos = gtk_list_box_row_get_index (GTK_LIST_BOX_ROW (listitem));
    if (gtkpos <= 0) {
        return;
    }
    gtklist_row_move (GTK_LIST_BOX (sess->gtklist), listitem, gtkpos - 1);
}

static void
task_dn (GtkWidget *widget, gpointer data)
{
    struct gtask *gtsk;
    GList *sel;
    GtkWidget *listitem;
    int num, gtkpos;
    session *sess = data;

    if (!gtkhx_prefs.queuedl) {
        return;
    }
    sel = gtk_list_box_get_selected_rows (GTK_LIST_BOX (sess->gtklist));
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

    if (num <= 0) {
        return;
    }

    if (xfer_down (num)) {
        return;
    }

    gtkpos = gtk_list_box_row_get_index (GTK_LIST_BOX_ROW (listitem));
    gtklist_row_move (GTK_LIST_BOX (sess->gtklist), listitem, gtkpos + 1);
}

static void
task_go (GtkWidget *widget, gpointer data)
{
    struct gtask *gtsk;
    GList *sel;
    GtkWidget *listitem;
    session *sess = data;

    if (!gtkhx_prefs.geo.tasks.open) {
        return;
    }

    sel = gtk_list_box_get_selected_rows (GTK_LIST_BOX (sess->gtklist));
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

/* see users.c users_move() for rationale — size on
 * configure, position deferred to quit.
 * gone — GTK 4 widgets don't fire configure-event. Tasks
 * window size is captured at hx_quit() in gtkhx.c. */

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
    gtk_widget_set_margin_start (button_bar,  6);
    gtk_widget_set_margin_end   (button_bar,  6);
    gtk_widget_set_margin_top   (button_bar,  6);
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
    gtk_box_append (GTK_BOX (content_vbox), sess->gtask_scroll);
    return content_vbox;
}

void
gtkhx_tasks_after_embed (session *sess)
{
    g_return_if_fail (sess != NULL);

    gtkhx_prefs.geo.tasks.open = 1;
    gtkhx_prefs.geo.tasks.init = 1;

    task_tasks_update (sess);
    xfer_tasks_update (sess->htlc);
}

/* LONGEST_HUMAN_READABLE + human_size come in via human_readable.h
 * now (transitively via files.h's include chain). */

void
file_update (session *sess, struct htxf_conn *htxf)
{
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

    gtsk = gtask_with_htxf (sess, htxf);
    if (!gtsk) {
        gtsk = gtask_new (sess, 0, htxf);
    }

    pos = htxf->total_pos;
    size = htxf->total_size;

    gettimeofday (&now, 0);
    sdiff = now.tv_sec - htxf->start.tv_sec;
    if (sdiff < 1) {
        sdiff = 1;
    }
    Bps = pos / (guint64) sdiff;
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

    hrs = (int) (eta / 3600);
    eta %= 3600;
    mins = (int) (eta / 60);
    secs = (int) (eta % 60);

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
            _ ("%1$s of %2$s \xc2\xb7 %3$s/s \xc2\xb7 ETA %4$d:%5$02d"),
            posstr, sizestr, bpsstr, mins, secs);
    }
    gtk_label_set_text (GTK_LABEL (gtsk->subtitle), subtitle);

    if (size > 0 && pos <= size) {
        /* gdouble (not gfloat) so multi-GB transfers don't lose
		 * precision in the divide — gfloat only has ~7 decimal
		 * digits of mantissa, which starts to dither at the
		 * gigabyte scale. */
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (gtsk->pbar),
                                       (gdouble) pos / (gdouble) size);
    }

    if (pos >= size) {
        gtask_delete (sess, gtsk);
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

    (void) htlc;
    if (!task_error_extract (frame, frame_len, errormsg, sizeof (errormsg), &len)) {
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
