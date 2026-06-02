/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"

#include <string.h>
#include <ctype.h>
#include <gdk/gdk.h>
#include <glib.h>
#include <gtk/gtk.h>

/* hx.h before cicn.h: cicn.h's CIcon struct fields use the PACKED
 * macro that hx.h defines. compat.h (via hx.h) also provides
 * the _ () gettext macro the rest of this TU uses. */
#include "hx.h"
#include "cicn.h"   /* load_icon */
#include "gtkhx.h"  /* gtkhx_apply_userlist_style + icon_files */
#include "msg.h"    /* msgwin_with_uid + create_msgwin */
#include "users.h"  /* users_font_desc + user_popup_show */
#include "users_row.h"
#include "users_view.h"

/* Right-click handler — pops user_popup over the row under (x,y).
 * Installed by hx_user_list_view_new on the column view; locates
 * the row by gtk_widget_pick + walk-up to a GtkListItem (same
 * pattern tracker.c uses), then selects it and pops the menu. */
static void
on_view_secondary_press (GtkGestureClick *gesture, int n_press, double x,
                         double y, gpointer data);

/* ============================================================ */
/* HxUserCellName — custom widget for the Name column           */
/* ============================================================ */

/* Renders the Mac-classic "icon as background + name overlay at
 * fixed text-x-offset" look. Snapshots:
 *
 *   1. Icon paintable at its natural width (scaled by pixel_scale)
 *      anchored to the cell's start edge, vertically centered.
 *   2. Text Pango layout positioned at text_x_offset * pixel_scale
 *      from the start edge, vertically centered. With text_outline
 *      on, four 1-px offset copies in a contrasting color paint
 *      first, then the foreground color on top — a halo so light
 *      user-set nick colors stay readable on busy banner icons.
 *
 * Backed by a borrowed HxUserRow that the column factory's bind
 * callback hands in. The cell connects to the row's "changed"
 * signal so an in-place rename / status flip re-snapshots without
 * a model splice. */

#define HX_TYPE_USER_CELL_NAME (hx_user_cell_name_get_type ())
G_DECLARE_FINAL_TYPE (HxUserCellName, hx_user_cell_name, HX, USER_CELL_NAME,
                      GtkWidget)

/* Mac wide-banner cicn icons (Badmoon's set: jokki., SkAtE!@, the
 * Bouncer, heavy_early, &c.) follow a community convention: the
 * first ~200 px of the bitmap is reserved for the user-name area
 * (Mac-classic drew the user name immediately to the right of the
 * icon, so for a wide "banner-style" icon the designer left that
 * left portion blank — either transparent via the mask, or filled
 * with opaque black) and the actual banner art lives in the right
 * portion. For our overlay layout (name at fixed x with icon
 * BEHIND it) the icon needs to render with its left ~200 px shifted
 * off-cell so the visible banner art lands behind the name. The
 * 200 px constant is empirically right across every wide banner
 * the client has shipped. */
#define HX_USER_WIDE_ICON_THRESHOLD 48
#define HX_USER_WIDE_ICON_LEFT_PAD 200

struct _HxUserCellName {
    GtkWidget parent_instance;

    HxUserRow *row;        /* borrowed */
    gulong row_changed_id; /* notify handler on row */

    GdkPaintable *icon;    /* resolved from row->icon via load_icon */
    guint16 icon_id_cached;
    /* Cached per-icon left padding so the wide-banner shift is
	 * computed at icon-load time, not every snapshot. 0 for narrow
	 * icons (which render starting at the cell's left edge). For
	 * wide banners this is HX_USER_WIDE_ICON_LEFT_PAD, applied
	 * scaled by pixel_scale at snapshot. */
    int icon_left_pad;

    /* Style — set once at construction. */
    int text_x_offset;
    double pixel_scale;
    gboolean text_outline;
    int row_height;
};

G_DEFINE_FINAL_TYPE (HxUserCellName, hx_user_cell_name, GTK_TYPE_WIDGET)

static void
hx_user_cell_name_dispose (GObject *object)
{
    HxUserCellName *cell = HX_USER_CELL_NAME (object);

    if (cell->row && cell->row_changed_id) {
        g_signal_handler_disconnect (cell->row, cell->row_changed_id);
        cell->row_changed_id = 0;
    }
    g_clear_object (&cell->row);
    g_clear_object (&cell->icon);

    /* GtkWidget needs explicit unparent — this widget doesn't
     * have children, but the parent class's dispose still
     * expects a clean state. */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child (GTK_WIDGET (object)))) {
        gtk_widget_unparent (child);
    }

    G_OBJECT_CLASS (hx_user_cell_name_parent_class)->dispose (object);
}

/* Resolve the row's icon-id into a GdkPaintable. Cached via
 * icon_id_cached so a re-bind to the same row doesn't re-load,
 * and a row-change with the same icon doesn't either. load_icon
 * returns a GdkPixbuf (alias to GdkPixmap*); we wrap in a
 * GdkTexture for the snapshot. NULL when no icon is known. */
static void
hx_user_cell_name_refresh_icon (HxUserCellName *cell)
{
    GdkPixbuf *pixbuf = NULL;
    GdkPixbuf *mask_unused = NULL;
    guint16 icon_id = cell->row ? hx_user_row_get_icon (cell->row) : 0;

    if (icon_id == cell->icon_id_cached && cell->icon) {
        return;
    }
    g_clear_object (&cell->icon);
    cell->icon_id_cached = icon_id;
    cell->icon_left_pad = 0;
    if (icon_id == 0) {
        return;
    }
    load_icon (GTK_WIDGET (cell), icon_id, &icon_files, 1, &pixbuf,
               &mask_unused);
    if (pixbuf) {
        /* Wide-banner detection: anything >= 48 px gets the 200 px
		 * left shift (clamped to the actual width for the rare
		 * pathologically narrow "wide" icon). */
        int pb_w = gdk_pixbuf_get_width (pixbuf);
        if (pb_w >= HX_USER_WIDE_ICON_THRESHOLD) {
            cell->icon_left_pad = MIN (HX_USER_WIDE_ICON_LEFT_PAD, pb_w);
        }
        /* gdk_texture_new_for_pixbuf is deprecated in GTK 4.12+ but
         * still the simplest way to bridge a GdkPixbuf into a
         * paintable. Other call sites in the codebase do the same;
         * the deprecation pragma keeps -Werror happy. */
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        cell->icon = GDK_PAINTABLE (gdk_texture_new_for_pixbuf (pixbuf));
        G_GNUC_END_IGNORE_DEPRECATIONS
        g_object_unref (pixbuf);
    }
}

static void
on_row_changed (HxUserRow *row, gpointer user_data)
{
    HxUserCellName *cell = HX_USER_CELL_NAME (user_data);
    (void)row;
    hx_user_cell_name_refresh_icon (cell);
    gtk_widget_queue_draw (GTK_WIDGET (cell));
}

static void
hx_user_cell_name_set_row (HxUserCellName *cell, HxUserRow *row)
{
    if (cell->row == row) {
        return;
    }
    if (cell->row && cell->row_changed_id) {
        g_signal_handler_disconnect (cell->row, cell->row_changed_id);
        cell->row_changed_id = 0;
    }
    g_clear_object (&cell->row);
    cell->icon_id_cached = 0;
    g_clear_object (&cell->icon);

    if (row) {
        cell->row = g_object_ref (row);
        cell->row_changed_id
            = g_signal_connect (row, "changed", G_CALLBACK (on_row_changed),
                                cell);
        hx_user_cell_name_refresh_icon (cell);
    }
    gtk_widget_queue_draw (GTK_WIDGET (cell));
}

/* Build the Pango layout for the row's name, applying the
 * pixel_scale to whatever font users_font_desc currently holds.
 * Caller frees with g_object_unref. */
static PangoLayout *
make_layout (HxUserCellName *cell)
{
    PangoLayout *layout;
    PangoFontDescription *fd;
    const char *text;

    text = (cell->row && hx_user_row_get_name (cell->row))
               ? hx_user_row_get_name (cell->row)
               : "";
    layout = gtk_widget_create_pango_layout (GTK_WIDGET (cell), text);

    if (users_font_desc) {
        fd = pango_font_description_copy (users_font_desc);
    } else {
        fd = pango_font_description_from_string ("Sans 10");
    }
    /* Scale font size by pixel_scale. Pango sizes are in 1024ths
     * of a point. */
    if (cell->pixel_scale != 1.0) {
        int size = pango_font_description_get_size (fd);
        if (size > 0) {
            pango_font_description_set_size (
                fd, (gint) (size * cell->pixel_scale));
        }
    }
    pango_layout_set_font_description (layout, fd);
    pango_font_description_free (fd);
    return layout;
}

static void
hx_user_cell_name_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
    HxUserCellName *cell = HX_USER_CELL_NAME (widget);
    int width = gtk_widget_get_width (widget);
    int height = gtk_widget_get_height (widget);
    int text_x;
    PangoLayout *layout;
    PangoRectangle ink, log;
    int text_y;
    GdkRGBA fg_color;
    const GdkRGBA *row_fg;

    if (!cell->row) {
        return;
    }

    /* Clip everything to the cell's bounds so wide-banner icons
	 * don't spill into adjacent rows / columns when shifted left
	 * by HX_USER_WIDE_ICON_LEFT_PAD. The clip also covers the
	 * text layer below, harmless because the text never exceeds
	 * the cell width by design (text_x_offset clears the icon
	 * region; very long names get visually truncated at the
	 * column edge rather than overflowing). */
    gtk_snapshot_push_clip (
        snapshot, &GRAPHENE_RECT_INIT (0, 0, (float) width, (float) height));

    /* Paint icon first so the name renders on top. The paintable
     * is sized to its natural dimensions scaled by pixel_scale;
     * vertically centered in the cell. Wide banner icons (Mac
     * convention: blank/transparent left ~200 px reserved for the
     * name area, art in the right portion) are shifted LEFT by
     * scaled_lpad so the visible art lines up with the cell's
     * left edge. */
    if (cell->icon) {
        double iw = gdk_paintable_get_intrinsic_width (cell->icon)
                    * cell->pixel_scale;
        double ih = gdk_paintable_get_intrinsic_height (cell->icon)
                    * cell->pixel_scale;
        double iy = (height - ih) / 2.0;
        double ix = -(cell->icon_left_pad * cell->pixel_scale);
        gtk_snapshot_save (snapshot);
        gtk_snapshot_translate (
            snapshot, &GRAPHENE_POINT_INIT ((float) ix, (float) iy));
        gdk_paintable_snapshot (cell->icon, snapshot, iw, ih);
        gtk_snapshot_restore (snapshot);
    }

    /* Name layout. Pull foreground from the row (per-user nick
     * color or status palette); fall back to the widget's GTK
     * foreground so light/dark theme tracking keeps working. */
    layout = make_layout (cell);
    pango_layout_get_pixel_extents (layout, &ink, &log);
    text_x = (int) (cell->text_x_offset * cell->pixel_scale);
    text_y = (height - log.height) / 2;

    row_fg = hx_user_row_get_foreground (cell->row);
    if (row_fg) {
        fg_color = *row_fg;
    } else {
        gtk_widget_get_color (widget, &fg_color);
    }

    /* Text outline (Users window only) — paint four offset copies
     * in a contrast color before the foreground. Contrast = inverse
     * of fg luminance so the halo stays visible regardless of the
     * row's color. Reasonable approximation: pick black when fg is
     * light, white when fg is dark. */
    if (cell->text_outline) {
        GdkRGBA halo;
        double lum = 0.299 * fg_color.red + 0.587 * fg_color.green
                     + 0.114 * fg_color.blue;
        halo.red = halo.green = halo.blue = (lum > 0.5 ? 0.0 : 1.0);
        halo.alpha = 1.0;
        static const int dx[] = { -1, 1, 0, 0 };
        static const int dy[] = { 0, 0, -1, 1 };
        for (int i = 0; i < 4; i++) {
            gtk_snapshot_save (snapshot);
            gtk_snapshot_translate (
                snapshot, &GRAPHENE_POINT_INIT ((float) (text_x + dx[i]),
                                                (float) (text_y + dy[i])));
            gtk_snapshot_append_layout (snapshot, layout, &halo);
            gtk_snapshot_restore (snapshot);
        }
    }

    gtk_snapshot_save (snapshot);
    gtk_snapshot_translate (snapshot,
                            &GRAPHENE_POINT_INIT ((float) text_x,
                                                  (float) text_y));
    gtk_snapshot_append_layout (snapshot, layout, &fg_color);
    gtk_snapshot_restore (snapshot);

    g_object_unref (layout);

    /* Matches gtk_snapshot_push_clip at the top of this function. */
    gtk_snapshot_pop (snapshot);
}

static void
hx_user_cell_name_measure (GtkWidget *widget, GtkOrientation orientation,
                           int for_size, int *minimum, int *natural,
                           int *minimum_baseline, int *natural_baseline)
{
    HxUserCellName *cell = HX_USER_CELL_NAME (widget);
    (void)for_size;
    if (minimum_baseline) {
        *minimum_baseline = -1;
    }
    if (natural_baseline) {
        *natural_baseline = -1;
    }
    if (orientation == GTK_ORIENTATION_VERTICAL) {
        *minimum = *natural = cell->row_height;
    } else {
        /* Natural width = text offset + room for a typical 16-char
         * name at the current font size. GtkColumnView gives us
         * whatever the column's set_fixed_width says, so this is
         * only a hint to the layout machinery. */
        *minimum = (int) (cell->text_x_offset * cell->pixel_scale);
        *natural = *minimum + (int) (140 * cell->pixel_scale);
    }
}

static void
hx_user_cell_name_class_init (HxUserCellNameClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->dispose = hx_user_cell_name_dispose;
    widget_class->snapshot = hx_user_cell_name_snapshot;
    widget_class->measure = hx_user_cell_name_measure;
}

static void
hx_user_cell_name_init (HxUserCellName *cell)
{
    cell->text_x_offset = 22;
    cell->pixel_scale = 1.0;
    cell->text_outline = FALSE;
    cell->row_height = 18;
}

static GtkWidget *
hx_user_cell_name_new (int text_x_offset, double pixel_scale,
                       gboolean text_outline, int row_height)
{
    HxUserCellName *cell = g_object_new (HX_TYPE_USER_CELL_NAME, NULL);
    cell->text_x_offset = text_x_offset;
    cell->pixel_scale = pixel_scale;
    cell->text_outline = text_outline;
    cell->row_height = row_height;
    return GTK_WIDGET (cell);
}

/* ============================================================ */
/* HxUserListView — public GObject                              */
/* ============================================================ */

struct _HxUserListView {
    GObject parent_instance;

    session *sess; /* borrowed */
    HxUserListStyle style;

    /* Style parameters derived from `style` at construction. */
    int row_height;
    double pixel_scale;
    gboolean text_outline;
    int text_x_offset;
    int col_uid_width;
    int col_name_width;
    gboolean show_titles;

    /* Model chain. The store is the truth; the sort model + the
     * selection wrap it. */
    GListStore *store;
    GtkSortListModel *sort_model;
    GtkSingleSelection *selection;

    /* O(1) lookup from a borrowed hx_user pointer back to its
     * row object — needed by update / remove since the row's
     * position in the sort model is whatever the sorter decided.
     * Keys are gpointer (the user *), values are HxUserRow * with
     * NO ref (the GListStore holds the strong ref). On remove we
     * just drop the hashtable entry; finalize is empty. */
    GHashTable *by_user;

    /* Widgets. column_view is what hx_user_list_view_get_widget
     * returns. */
    GtkWidget *column_view;
};

G_DEFINE_FINAL_TYPE (HxUserListView, hx_user_list_view, G_TYPE_OBJECT)

static void
hx_user_list_view_dispose (GObject *object)
{
    HxUserListView *v = HX_USER_LIST_VIEW (object);
    g_clear_pointer (&v->by_user, g_hash_table_unref);
    g_clear_object (&v->selection);
    g_clear_object (&v->sort_model);
    g_clear_object (&v->store);
    /* column_view is owned by whoever packed it; we don't unparent
     * here. */
    v->column_view = NULL;
    G_OBJECT_CLASS (hx_user_list_view_parent_class)->dispose (object);
}

static void
hx_user_list_view_class_init (HxUserListViewClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = hx_user_list_view_dispose;
}

static void
hx_user_list_view_init (HxUserListView *v)
{
    (void)v;
}

/* ---- Sort comparators ----------------------------------------- */

/* GtkCustomSorter callbacks return < 0, 0, > 0 (GCompareDataFunc
 * shape). a / b are HxUserRow * casts. */
static int
cmp_uid (gconstpointer a, gconstpointer b, gpointer user_data)
{
    (void)user_data;
    guint16 ua = hx_user_row_get_uid ((HxUserRow *) a);
    guint16 ub = hx_user_row_get_uid ((HxUserRow *) b);
    if (ua < ub) {
        return -1;
    }
    if (ua > ub) {
        return 1;
    }
    return 0;
}

static int
cmp_name (gconstpointer a, gconstpointer b, gpointer user_data)
{
    const char *na = hx_user_row_get_name ((HxUserRow *) a);
    const char *nb = hx_user_row_get_name ((HxUserRow *) b);
    int len_a = (int) strlen (na);
    int len_b = (int) strlen (nb);
    int len = len_a < len_b ? len_a : len_b;
    int i;
    (void)user_data;

    /* Same case-insensitive byte-by-byte compare the old
     * users_sort used — preserves ordering on existing servers
     * even if it's not strictly Unicode-aware. */
    for (i = 0; i < len; i++) {
        int ca = tolower ((unsigned char) na[i]);
        int cb = tolower ((unsigned char) nb[i]);
        if (ca < cb) {
            return -1;
        }
        if (ca > cb) {
            return 1;
        }
    }
    if (len_a < len_b) {
        return -1;
    }
    if (len_a > len_b) {
        return 1;
    }
    return 0;
}

/* ---- Column factories ----------------------------------------- */

static void
uid_setup (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkWidget *lbl = gtk_label_new (NULL);
    (void)f;
    (void)d;
    gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
    gtk_widget_set_margin_start (lbl, 6);
    gtk_widget_set_margin_end (lbl, 6);
    gtk_list_item_set_child (item, lbl);
}

/* Stash the GtkListItem on the cell so the right-click handler
 * can recover the row position via walk-up from gtk_widget_pick.
 * Mirrors tracker.c's "tracker-list-item" qdata. Set on every
 * bind; both columns stash to the same key on their respective
 * cell widgets so a right-click on either cell finds the row. */
static void
stash_list_item (GtkWidget *cell, GtkListItem *item)
{
    GtkWidget *cell_w, *row_w;
    g_object_set_data (G_OBJECT (cell), "user-list-item", item);
    cell_w = gtk_widget_get_parent (cell);
    row_w = cell_w ? gtk_widget_get_parent (cell_w) : NULL;
    if (row_w) {
        g_object_set_data (G_OBJECT (row_w), "user-list-item", item);
    }
}

static void
uid_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkLabel *lbl = GTK_LABEL (gtk_list_item_get_child (item));
    HxUserRow *row = gtk_list_item_get_item (item);
    char buf[16];
    (void)f;
    (void)d;
    stash_list_item (GTK_WIDGET (lbl), item);
    if (!row) {
        gtk_label_set_text (lbl, "");
        return;
    }
    g_snprintf (buf, sizeof (buf), "%u", hx_user_row_get_uid (row));
    gtk_label_set_text (lbl, buf);
}

static void
name_setup (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    HxUserListView *v = d;
    GtkWidget *cell = hx_user_cell_name_new (v->text_x_offset, v->pixel_scale,
                                             v->text_outline, v->row_height);
    (void)f;
    gtk_list_item_set_child (item, cell);
}

static void
name_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    HxUserCellName *cell = HX_USER_CELL_NAME (gtk_list_item_get_child (item));
    HxUserRow *row = gtk_list_item_get_item (item);
    (void)f;
    (void)d;
    stash_list_item (GTK_WIDGET (cell), item);
    hx_user_cell_name_set_row (cell, row);
}

static void
name_unbind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    HxUserCellName *cell = HX_USER_CELL_NAME (gtk_list_item_get_child (item));
    (void)f;
    (void)d;
    hx_user_cell_name_set_row (cell, NULL);
}

/* ---- Activate (double-click / Enter) — open msgwin ---------- */

static void
on_view_activate (GtkColumnView *cv, guint pos, gpointer user_data)
{
    HxUserListView *v = user_data;
    HxUserRow *row;
    struct hx_user *user;
    struct msgwin *mw;
    (void)cv;

    row = g_list_model_get_item (G_LIST_MODEL (v->selection), pos);
    if (!row) {
        return;
    }
    user = hx_user_row_get_user (row);
    g_object_unref (row);
    if (!user) {
        return;
    }
    mw = msgwin_with_uid (user->uid);
    if (mw) {
        gtk_window_present (GTK_WINDOW (mw->window));
    } else {
        create_msgwin (user->uid, user->name);
    }
}

/* ---- Right-click → user_popup_show -------------------------- */

static void
on_view_secondary_press (GtkGestureClick *gesture, int n_press, double x,
                         double y, gpointer data)
{
    GtkWidget *cv_widget
        = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
    HxUserListView *v
        = g_object_get_data (G_OBJECT (cv_widget), "user-list-view");
    GtkWidget *picked, *walker;
    GtkListItem *item = NULL;
    HxUserRow *row;
    struct hx_user *user;
    guint pos;
    (void)n_press;
    (void)data;

    if (!v || !v->sess) {
        return;
    }

    /* Find which row was right-clicked. Same gtk_widget_pick + walk-up
     * pattern tracker.c uses for its row context menu — checks the
     * "user-list-item" qdata that stash_list_item plants on both the
     * cell widget and the row widget during bind. */
    picked = gtk_widget_pick (cv_widget, x, y, GTK_PICK_DEFAULT);
    for (walker = picked; walker && walker != cv_widget;
         walker = gtk_widget_get_parent (walker)) {
        item = g_object_get_data (G_OBJECT (walker), "user-list-item");
        if (item) {
            break;
        }
    }
    if (!item) {
        return;
    }
    pos = gtk_list_item_get_position (item);
    if (pos == GTK_INVALID_LIST_POSITION) {
        return;
    }

    /* Select first so toolbar buttons and the menu actions both see
     * the right row. notify::selected fires synchronously. */
    gtk_single_selection_set_selected (v->selection, pos);

    row = g_list_model_get_item (G_LIST_MODEL (v->selection), pos);
    if (!row) {
        return;
    }
    user = hx_user_row_get_user (row);
    g_object_unref (row);
    if (!user) {
        return;
    }

    /* Hand off to users.c's existing popover builder. Anchor on the
     * column view; coords are widget-local. */
    user_popup_show (cv_widget, user, v->sess, x, y);
}

/* ============================================================ */
/* Public constructors and mutators                              */
/* ============================================================ */

HxUserListView *
hx_user_list_view_new (session *sess, HxUserListStyle style)
{
    HxUserListView *v = g_object_new (HX_TYPE_USER_LIST_VIEW, NULL);
    GtkColumnView *cv;
    GtkColumnViewColumn *col_uid, *col_name;
    GtkListItemFactory *factory_uid, *factory_name;
    GtkSorter *sorter_uid, *sorter_name;

    v->sess = sess;
    v->style = style;
    if (style == HX_USER_LIST_STYLE_USERS) {
        /* Row height 26. Pairs with the CSS rule below that strips
		 * Adwaita's columnview row + cell padding — without that
		 * strip the rows reach 30+ px once the theme padding adds
		 * onto our value. 26 gives the 1.25×-scaled 16-18 px icons
		 * a few px of breathing room above and below. */
        v->row_height = 26;
        v->pixel_scale = 1.25;
        v->text_outline = TRUE;
        v->text_x_offset = 36;
        v->col_uid_width = 35;
        v->col_name_width = 240;
        v->show_titles = TRUE;
    } else {
        v->row_height = 18;
        v->pixel_scale = 1.0;
        v->text_outline = FALSE;
        /* text_x_offset bumped from 22 → 36. The Users window
		 * applied the same bump for the same reason: 22 cleared
		 * the 16-18 px stock-icon width, but medium-wide non-
		 * banner tiles (UNIX = icon 500 and friends are wider
		 * than that) overlapped the name. 36 px gives those room
		 * and still keeps full-width banner icons (60+ px) rendering
		 * as backgrounds with the name overlaid on top — the
		 * Mac-classic look. Same constant the Users window uses
		 * (45 px effective there after the 1.25× scale); chat
		 * sidebars run at 1.0× so 36 is 36. */
        v->text_x_offset = 36;
        v->col_uid_width = 35;
        v->col_name_width = 240;
        v->show_titles = TRUE;
    }

    v->store = g_list_store_new (HX_TYPE_USER_ROW);
    v->sort_model = gtk_sort_list_model_new (
        G_LIST_MODEL (g_object_ref (v->store)), NULL);
    gtk_sort_list_model_set_incremental (v->sort_model, FALSE);
    v->selection = gtk_single_selection_new (
        G_LIST_MODEL (g_object_ref (v->sort_model)));
    gtk_single_selection_set_autoselect (v->selection, FALSE);
    gtk_single_selection_set_can_unselect (v->selection, TRUE);
    gtk_single_selection_set_selected (v->selection,
                                       GTK_INVALID_LIST_POSITION);

    v->by_user = g_hash_table_new (g_direct_hash, g_direct_equal);

    v->column_view = gtk_column_view_new (
        GTK_SELECTION_MODEL (g_object_ref (v->selection)));
    cv = GTK_COLUMN_VIEW (v->column_view);
    gtk_column_view_set_show_column_separators (cv, FALSE);
    gtk_column_view_set_show_row_separators (cv, FALSE);

    /* UID column. */
    factory_uid = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory_uid, "setup", G_CALLBACK (uid_setup), v);
    g_signal_connect (factory_uid, "bind", G_CALLBACK (uid_bind), v);
    col_uid = gtk_column_view_column_new (_ ("UID"), factory_uid);
    gtk_column_view_column_set_fixed_width (col_uid, v->col_uid_width);
    gtk_column_view_column_set_resizable (col_uid, TRUE);
    sorter_uid = GTK_SORTER (gtk_custom_sorter_new (cmp_uid, NULL, NULL));
    gtk_column_view_column_set_sorter (col_uid, sorter_uid);
    g_object_unref (sorter_uid);
    gtk_column_view_append_column (cv, col_uid);
    g_object_unref (col_uid);

    /* Name column — uses the custom HxUserCellName widget. */
    factory_name = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory_name, "setup", G_CALLBACK (name_setup), v);
    g_signal_connect (factory_name, "bind", G_CALLBACK (name_bind), v);
    g_signal_connect (factory_name, "unbind", G_CALLBACK (name_unbind), v);
    col_name = gtk_column_view_column_new (_ ("Name"), factory_name);
    gtk_column_view_column_set_expand (col_name, TRUE);
    gtk_column_view_column_set_resizable (col_name, TRUE);
    sorter_name = GTK_SORTER (gtk_custom_sorter_new (cmp_name, NULL, NULL));
    gtk_column_view_column_set_sorter (col_name, sorter_name);
    g_object_unref (sorter_name);
    gtk_column_view_append_column (cv, col_name);
    g_object_unref (col_name);

    /* Plumb the column view's header sorter into the sort model
     * so header clicks re-order rows. */
    gtk_sort_list_model_set_sorter (v->sort_model,
                                    gtk_column_view_get_sorter (cv));

    /* Default sort by UID ascending. Name-column header click is
	 * one tap away for anyone who prefers alphabetical. */
    gtk_column_view_sort_by_column (cv, col_uid, GTK_SORT_ASCENDING);

    /* Double-click / Enter → open msgwin. */
    g_signal_connect (cv, "activate", G_CALLBACK (on_view_activate), v);

    /* Apply the userlist CSS class so the global font from
     * users_font_desc takes effect (Settings → Misc → User-list
     * font). */
    gtkhx_apply_userlist_style (v->column_view);

    /* Tighten row + cell padding. Adwaita's default columnview row
	 * gets ~10 px combined vertical padding from .listview / .cell
	 * rules, which on top of the 1.25× scaled icons reads as way
	 * too airy. Override on a one-shot global provider scoped via
	 * the .gtkhx-userlist class so we only touch our two userlist
	 * call sites (Users window + pchat sidebars). min-height: 0
	 * also lets our measure-returned row_height actually take
	 * effect. */
    {
        static GtkCssProvider *p = NULL;
        if (!p) {
            p = gtk_css_provider_new ();
            gtk_css_provider_load_from_string (
                p, ".gtkhx-userlist > listview > row,"
                   ".gtkhx-userlist > listview > row > cell {"
                   "  min-height: 0;"
                   "  padding-top: 0;"
                   "  padding-bottom: 0;"
                   "}");
            gtk_style_context_add_provider_for_display (
                gdk_display_get_default (), GTK_STYLE_PROVIDER (p),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
    }

    /* Right-click context menu. Capture-phase secondary-button
     * gesture on the column view; the handler picks the deepest
     * widget under (x,y), walks up to the GtkListItem stashed
     * during bind, and pops the user_popup over the clicked row.
     * Same pattern tracker.c uses for its row context menu. */
    if (sess) {
        GtkGesture *rclick = gtk_gesture_click_new ();
        gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (rclick),
                                       GDK_BUTTON_SECONDARY);
        gtk_event_controller_set_propagation_phase (
            GTK_EVENT_CONTROLLER (rclick), GTK_PHASE_CAPTURE);
        g_object_set_data (G_OBJECT (v->column_view), "user-list-view", v);
        g_signal_connect (rclick, "pressed",
                          G_CALLBACK (on_view_secondary_press), NULL);
        gtk_widget_add_controller (v->column_view,
                                   GTK_EVENT_CONTROLLER (rclick));
    }

    return v;
}

GtkWidget *
hx_user_list_view_get_widget (HxUserListView *v)
{
    return v ? v->column_view : NULL;
}

void
hx_user_list_view_add (HxUserListView *v, struct hx_user *user, const char *nam,
                       guint16 icon, guint16 color)
{
    HxUserRow *row;

    if (!v || !user) {
        return;
    }
    /* Defensive: an "add" for a user that's already in the view
     * is bug-shaped — most likely a missed delete on the model
     * side. Treat as in-place state refresh rather than appending
     * a duplicate row. */
    row = g_hash_table_lookup (v->by_user, user);
    if (row) {
        hx_user_row_set_state (row, nam, icon, color);
        return;
    }
    row = hx_user_row_new (user, nam, icon, color);
    g_hash_table_insert (v->by_user, user, row);
    g_list_store_append (v->store, row);
    g_object_unref (row);
}

void
hx_user_list_view_remove (HxUserListView *v, struct hx_user *user)
{
    HxUserRow *row;
    guint pos;

    if (!v || !user) {
        return;
    }
    row = g_hash_table_lookup (v->by_user, user);
    if (!row) {
        return;
    }
    /* Resolve position via g_list_store_find — keyed on the row
     * GObject identity which we have via the by_user lookup. */
    if (g_list_store_find (v->store, row, &pos)) {
        g_list_store_remove (v->store, pos);
    }
    g_hash_table_remove (v->by_user, user);
}

void
hx_user_list_view_update (HxUserListView *v, struct hx_user *user,
                          const char *nam, guint16 icon, guint16 color)
{
    HxUserRow *row;

    if (!v || !user) {
        return;
    }
    row = g_hash_table_lookup (v->by_user, user);
    if (!row) {
        /* Update for a user we don't have a row for — most likely
         * a race with a missed user_create. Just add. */
        hx_user_list_view_add (v, user, nam, icon, color);
        return;
    }
    hx_user_row_set_state (row, nam, icon, color);
    /* The "changed" signal the row emits triggers the sort model
     * to re-evaluate this row's position. Selection stays on the
     * row identity, so an arrow-key cursor doesn't jump on
     * rename. */
}

void
hx_user_list_view_clear (HxUserListView *v)
{
    if (!v) {
        return;
    }
    g_list_store_remove_all (v->store);
    g_hash_table_remove_all (v->by_user);
}

struct hx_user *
hx_user_list_view_get_selected_user (HxUserListView *v)
{
    HxUserRow *row;
    guint pos;
    struct hx_user *user;

    if (!v || !v->selection) {
        return NULL;
    }
    pos = gtk_single_selection_get_selected (v->selection);
    if (pos == GTK_INVALID_LIST_POSITION) {
        return NULL;
    }
    row = g_list_model_get_item (G_LIST_MODEL (v->selection), pos);
    if (!row) {
        return NULL;
    }
    user = hx_user_row_get_user (row);
    g_object_unref (row);
    return user;
}

session *
hx_user_list_view_get_session (HxUserListView *v)
{
    return v ? v->sess : NULL;
}

void
hx_user_list_view_refresh_font (HxUserListView *v)
{
    if (!v || !v->column_view) {
        return;
    }
    /* The font is applied via the gtkhx-userlist CSS class on the
     * widget; gtkhx_refresh_userlist_css elsewhere updates the
     * provider when the pref changes. We just need to queue a
     * draw so cell snapshots re-run with the new font. */
    gtk_widget_queue_draw (v->column_view);
}
