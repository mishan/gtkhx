/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * users_cell.c — see users_cell.h. HxUserCellName, the custom Name-column
 * cell, split verbatim out of users_view.c when HxUserListView moved to
 * Rust (Phase R5.9). Delicate custom snapshot/measure rendering stays C
 * behind the users_cell.h ABI the Rust view calls.
 */

#include "config.h"

#include <gdk/gdk.h>
#include <glib.h>
#include <gtk/gtk.h>

/* hx.h before cicn.h: cicn.h's CIcon struct fields use the PACKED macro
 * that hx.h defines. */
#include "hx.h"
#include "cicn.h"        /* load_icon */
#include "gtkhx.h"       /* icon_files */
#include "gtkutil.h"     /* gtkhx_texture_from_pixbuf */
#include "gtkhx_theme.h" /* gtkhx_theme_scale, gtkhx_theme_get_default_percent */
#include "users.h"       /* users_font_desc */
#include "users_row.h"
#include "users_cell.h"
#include "gif_avatar.h" /* gtkhx_avatar_get / is_animated / is_paused / set_paused */

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

    GdkPaintable *icon; /* resolved from row->icon via load_icon, OR
                            * the GIF avatar texture when one is cached
                            * for this row's uid (using_avatar) */
    guint16 icon_id_cached;
    /* TRUE when `icon` currently holds a GIF avatar rather than a
     * cicn sprite. Avatars take precedence over the 16-bit icon id and
     * render through the identical snapshot path (intrinsic size *
     * scale + wide-banner shift), so a GIF authored at icon / banner
     * dimensions looks just like its cicn equivalent. */
    gboolean using_avatar;
    /* Cached per-icon left padding so the wide-banner shift is
     * computed at icon-load time, not every snapshot. 0 for narrow
     * icons (which render starting at the cell's left edge). For
     * wide banners this is HX_USER_WIDE_ICON_LEFT_PAD, applied
     * scaled by pixel_scale at snapshot. */
    int icon_left_pad;

    /* Style — set once at construction. */
    int text_x_offset;
    /* When TRUE this cell follows the GTKHX_SCALE_USERLIST_* theme
     * areas (the standalone Users window); icon, text and geometry
     * read the live theme scale at measure / snapshot time so a
     * Settings change rescales without rebuilding cells. When FALSE
     * (the compact chat-sidebar list) the cell stays at its fixed
     * structural density, unthemed. */
    gboolean themed;
    gboolean text_outline;
    /* Base row height, tuned at the default-theme icon scale.
     * Themed cells scale it live with the icon area; see
     * cell_effective_row_height. */
    int row_height;

    /* Click-to-pause deferral (Phase 10.D). A single primary click on an
     * animated avatar toggles its pause, but we can't act on the first
     * press: it might be the opening press of a double-click (which opens
     * the PM via on_view_activate). So we arm a timeout for the double-
     * click interval and only toggle if no second press lands.
     * pause_click_uid is the uid captured at press time (robust against
     * cell recycling); pause_click_source is the pending timeout. */
    guint pause_click_source;
    guint16 pause_click_uid;
};

G_DEFINE_FINAL_TYPE (HxUserCellName, hx_user_cell_name, GTK_TYPE_WIDGET)

static void
hx_user_cell_name_dispose (GObject *object)
{
    HxUserCellName *cell = HX_USER_CELL_NAME (object);

    if (cell->pause_click_source) {
        g_source_remove (cell->pause_click_source);
        cell->pause_click_source = 0;
    }

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

    /* GIF avatar takes precedence over the 16-bit icon id. The cached
     * texture is the source of truth (gif_avatar.c); we route it through
     * the same cell->icon field + snapshot path as a cicn sprite so the
     * avatar is sized identically — intrinsic px * theme scale, with the
     * wide-banner left-shift for banner-width art. */
    guint16 uid = cell->row ? hx_user_row_get_uid (cell->row) : 0;
    GdkTexture *avatar
        = uid ? gtkhx_avatar_get (hx_active_session ()->htlc, uid) : NULL;
    if (avatar) {
        GdkPaintable *ap = GDK_PAINTABLE (avatar);
        if (cell->using_avatar && cell->icon == ap) {
            return; /* already showing this exact avatar */
        }
        g_clear_object (&cell->icon);
        cell->icon = g_object_ref (ap);
        cell->using_avatar = TRUE;
        /* Force a cicn re-resolve if the avatar is later cleared. */
        cell->icon_id_cached = 0;
        int w = gdk_texture_get_width (avatar);
        cell->icon_left_pad = (w >= HX_USER_WIDE_ICON_THRESHOLD)
                                  ? MIN (HX_USER_WIDE_ICON_LEFT_PAD, w)
                                  : 0;
        return;
    }

    /* No avatar — fall back to the cicn sprite for the 16-bit icon id.
     * Drop a stale avatar first so the cached-id fast path is valid. */
    if (cell->using_avatar) {
        g_clear_object (&cell->icon);
        cell->using_avatar = FALSE;
        cell->icon_id_cached = 0;
    }

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
        GdkTexture *tex;
        /* Wide-banner detection: anything >= 48 px gets the 200 px
         * left shift (clamped to the actual width for the rare
         * pathologically narrow "wide" icon). */
        int pb_w = gdk_pixbuf_get_width (pixbuf);
        if (pb_w >= HX_USER_WIDE_ICON_THRESHOLD) {
            cell->icon_left_pad = MIN (HX_USER_WIDE_ICON_LEFT_PAD, pb_w);
        }
        /* gtkhx_texture_from_pixbuf is the non-deprecated
         * GBytes / gdk_memory_texture_new wrapper; centralises
         * the conversion so the per-call-site deprecation
         * pragma is no longer needed. */
        tex = gtkhx_texture_from_pixbuf (pixbuf);
        cell->icon = tex ? GDK_PAINTABLE (tex) : NULL;
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

void
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
    cell->using_avatar = FALSE;
    g_clear_object (&cell->icon);

    if (row) {
        cell->row = g_object_ref (row);
        cell->row_changed_id = g_signal_connect (
            row, "changed", G_CALLBACK (on_row_changed), cell);
        hx_user_cell_name_refresh_icon (cell);
    }
    gtk_widget_queue_draw (GTK_WIDGET (cell));
}

/* Live theme scales for a cell. Themed cells (standalone Users window)
 * track the user's GTKHX_SCALE_USERLIST_* knobs; compact cells stay at
 * 1.0. Read fresh on every measure / snapshot so a Settings change
 * takes effect on the next queue_resize / queue_draw with no per-cell
 * state to refresh. */
static double
cell_icon_scale (HxUserCellName *cell)
{
    return cell->themed ? gtkhx_theme_scale (GTKHX_SCALE_USERLIST_ICON) : 1.0;
}

static double
cell_text_scale (HxUserCellName *cell)
{
    return cell->themed ? gtkhx_theme_scale (GTKHX_SCALE_USERLIST_TEXT) : 1.0;
}

/* Effective row height. The configured row_height was tuned at the
 * default-theme icon scale (125% for the Users window), so scale it by
 * how far the current icon scale departs from that default — at the
 * default it returns the configured value unchanged. */
static int
cell_effective_row_height (HxUserCellName *cell)
{
    double def;
    int h;

    if (!cell->themed) {
        return cell->row_height;
    }
    def = gtkhx_theme_get_default_percent (GTKHX_SCALE_USERLIST_ICON) / 100.0;
    if (def <= 0.0) {
        def = 1.0;
    }
    h = (int)(cell->row_height * cell_icon_scale (cell) / def + 0.5);
    return h < 1 ? 1 : h;
}

/* Build the Pango layout for the row's name, applying the live text
 * scale to whatever font users_font_desc currently holds.
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
    /* Scale font size by the live text scale. Pango sizes are in
     * 1024ths of a point. */
    {
        double tscale = cell_text_scale (cell);
        if (tscale != 1.0) {
            int size = pango_font_description_get_size (fd);
            if (size > 0) {
                pango_font_description_set_size (fd, (gint)(size * tscale));
            }
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
        snapshot, &GRAPHENE_RECT_INIT (0, 0, (float)width, (float)height));

    /* Paint icon first so the name renders on top. The paintable
     * is sized to its natural dimensions scaled by pixel_scale;
     * vertically centered in the cell. Wide banner icons (Mac
     * convention: blank/transparent left ~200 px reserved for the
     * name area, art in the right portion) are shifted LEFT by
     * scaled_lpad so the visible art lines up with the cell's
     * left edge. */
    if (cell->icon) {
        double iscale = cell_icon_scale (cell);
        double iw = gdk_paintable_get_intrinsic_width (cell->icon) * iscale;
        double ih = gdk_paintable_get_intrinsic_height (cell->icon) * iscale;
        double iy = (height - ih) / 2.0;
        double ix = -(cell->icon_left_pad * iscale);
        gtk_snapshot_save (snapshot);
        gtk_snapshot_translate (snapshot,
                                &GRAPHENE_POINT_INIT ((float)ix, (float)iy));
        gdk_paintable_snapshot (cell->icon, snapshot, iw, ih);
        gtk_snapshot_restore (snapshot);
    }

    /* Name layout. Pull foreground from the row (per-user nick
     * color or status palette); fall back to the widget's GTK
     * foreground so light/dark theme tracking keeps working. */
    layout = make_layout (cell);
    pango_layout_get_pixel_extents (layout, &ink, &log);
    /* Text starts past the icon region, so its x-offset follows the
     * icon scale (keeps the name clear of a larger / smaller icon). */
    text_x = (int)(cell->text_x_offset * cell_icon_scale (cell));
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
                snapshot, &GRAPHENE_POINT_INIT ((float)(text_x + dx[i]),
                                                (float)(text_y + dy[i])));
            gtk_snapshot_append_layout (snapshot, layout, &halo);
            gtk_snapshot_restore (snapshot);
        }
    }

    gtk_snapshot_save (snapshot);
    gtk_snapshot_translate (
        snapshot, &GRAPHENE_POINT_INIT ((float)text_x, (float)text_y));
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
        *minimum = *natural = cell_effective_row_height (cell);
    } else {
        /* Natural width = text offset (icon-scaled, clears the icon)
         * + room for a typical name at the current text scale.
         * GtkColumnView gives us whatever the column's
         * set_fixed_width says, so this is only a hint to the layout
         * machinery. */
        *minimum = (int)(cell->text_x_offset * cell_icon_scale (cell));
        *natural = *minimum + (int)(140 * cell_text_scale (cell));
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

/* Deferred single-click fired: no second press arrived within the
 * double-click window, so this really was a single click on the avatar —
 * toggle the captured uid's pause (re-checking it's still animated). */
static gboolean
pause_click_fire (gpointer user_data)
{
    HxUserCellName *cell = HX_USER_CELL_NAME (user_data);
    guint16 uid = cell->pause_click_uid;

    cell->pause_click_source = 0;
    cell->pause_click_uid = 0;
    if (uid != 0
        && gtkhx_avatar_is_animated (hx_active_session ()->htlc, uid)) {
        gtkhx_avatar_set_paused (
            hx_active_session ()->htlc, uid,
            !gtkhx_avatar_is_paused (hx_active_session ()->htlc, uid));
    }
    return G_SOURCE_REMOVE;
}

/* The system double-click interval in ms (default 250 if unavailable). */
static guint
cell_double_click_ms (HxUserCellName *cell)
{
    GtkSettings *settings = gtk_widget_get_settings (GTK_WIDGET (cell));
    int t = 250;
    if (settings) {
        g_object_get (settings, "gtk-double-click-time", &t, NULL);
    }
    return t > 0 ? (guint)t : 250;
}

/* Click-to-pause (Phase 10.D): a primary click that lands on an *animated*
 * avatar toggles that user's animation pause. We deliberately do NOT claim
 * the sequence and the toggle is deferred by the double-click interval: a
 * second press cancels it so double-click-to-open (on_view_activate) keeps
 * working on the avatar. (Claiming the first press, or running in CAPTURE
 * phase to suppress, would deny the column view the presses it needs to
 * detect the double-click — so we let selection proceed and only act on a
 * confirmed single click.) Clicks on the name or a still icon are ignored. */
static void
on_cell_icon_pressed (GtkGestureClick *gesture, int n_press, double x, double y,
                      gpointer user_data)
{
    HxUserCellName *cell = HX_USER_CELL_NAME (user_data);
    (void)y;
    (void)gesture;

    /* Second (or later) press → a double-click is forming. Cancel the
     * pending single-click toggle and let on_view_activate open the PM. */
    if (n_press != 1) {
        if (cell->pause_click_source) {
            g_source_remove (cell->pause_click_source);
            cell->pause_click_source = 0;
            cell->pause_click_uid = 0;
        }
        return;
    }
    if (!cell->row) {
        return;
    }
    guint16 uid = hx_user_row_get_uid (cell->row);
    if (uid == 0
        || !gtkhx_avatar_is_animated (hx_active_session ()->htlc, uid)) {
        return;
    }
    /* Hit-test the icon column (icon renders from the start edge up to
     * the text offset). Outside it → leave it for normal selection. */
    double icon_w = cell->text_x_offset * cell_icon_scale (cell);
    if (x >= icon_w) {
        return;
    }
    /* Arm the deferred toggle; supersede any stale pending one. */
    if (cell->pause_click_source) {
        g_source_remove (cell->pause_click_source);
    }
    cell->pause_click_uid = uid;
    cell->pause_click_source
        = g_timeout_add (cell_double_click_ms (cell), pause_click_fire, cell);
}

static void
hx_user_cell_name_init (HxUserCellName *cell)
{
    cell->text_x_offset = 22;
    cell->themed = FALSE;
    cell->text_outline = FALSE;
    cell->row_height = 18;

    GtkGesture *click = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click),
                                   GDK_BUTTON_PRIMARY);
    g_signal_connect (click, "pressed", G_CALLBACK (on_cell_icon_pressed),
                      cell);
    gtk_widget_add_controller (GTK_WIDGET (cell), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
hx_user_cell_name_new (int text_x_offset, gboolean themed,
                       gboolean text_outline, int row_height)
{
    HxUserCellName *cell = g_object_new (HX_TYPE_USER_CELL_NAME, NULL);
    cell->text_x_offset = text_x_offset;
    cell->themed = themed;
    cell->text_outline = text_outline;
    cell->row_height = row_height;
    return GTK_WIDGET (cell);
}
