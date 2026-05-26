/*
 * gtk_hlist_compat.c — implementation notes for the shim. See
 * gtk_hlist_compat.h for the rationale and API surface.
 *
 * Storage model
 * -------------
 * Each GtkHList wraps a GtkListStore whose columns are laid out:
 *
 *   col 0          : G_TYPE_POINTER   — per-row data pointer
 *   col 1          : G_TYPE_STRING    — Pango foreground spec ("#rrggbb")
 *   col 2          : G_TYPE_BOOLEAN   — foreground-set flag
 *   col 3 .. 3+N-1 : G_TYPE_STRING    — display text per data column
 *   col 3+N..3+2N-1: GDK_TYPE_PIXBUF  — pixbuf per data column (NULL = none)
 *
 * For each of N data columns we add a GtkTreeViewColumn that packs a
 * pixbuf renderer (bound to the pixbuf store column) and a text
 * renderer (bound to the text store column with foreground+set bound
 * to cols 1/2).
 *
 * Sort
 * ----
 * gtk_hlist_set_compare_func() registers a default compare on the
 * sortable model. The wrapping callback materializes a transient
 * GtkHListRow (with cell[] populated from the model row's text
 * columns and ->data set from the data column) for each side, calls
 * the user's comparator, frees the temps. This is the only path that
 * has to satisfy the legacy struct's ABI shape — and only the cell
 * text and row data are ever inspected.
 */

#include "gtk_hlist_compat.h"
#include "debug.h"

#include <string.h>

/* Phase 4.10: GtkTreeView and friends are deprecated in GTK 4.10 in
 * favor of GtkColumnView + GListModel. The migration is a separate
 * axis of change tracked in the ROADMAP — see "Phase 4.10
 * gtk_hlist_compat deprecation containment" and the Phase 5
 * GtkColumnView/GListModel item. Until then, the entire shim file is
 * built with deprecation warnings suppressed so the rest of the
 * tree can lock in -Werror=deprecated-declarations cleanly. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

/* Storage column offsets: see header comment above. */
#define HLIST_COL_DATA 0
#define HLIST_COL_FG 1
#define HLIST_COL_FG_SET 2
#define HLIST_COL_TEXT(i) (3 + (i))
#define HLIST_COL_PIXBUF(N, i) (3 + (N) + (i))
#define HLIST_TOTAL_COLS(N) (3 + 2 * (N))

typedef struct _GtkHListPrivate GtkHListPrivate;

struct _GtkHListPrivate {
    GtkListStore *store;
    gint n_columns;
    gint freeze_count;
    GtkHListCompareFunc compare;
    gint sort_column;
    GtkSortType sort_type;
};

static GtkHListPrivate *
get_priv (GtkHList *hlist)
{
    return (GtkHListPrivate *)g_object_get_data (G_OBJECT (hlist),
                                                 "hlist-priv");
}

/* ------------------------------------------------------------------ */
/* GObject boilerplate                                                 */
/* ------------------------------------------------------------------ */

static guint click_column_signal;
static guint select_row_signal;

G_DEFINE_TYPE (GtkHList, gtk_hlist, GTK_TYPE_TREE_VIEW)

static void
gtk_hlist_finalize (GObject *object)
{
    GtkHListPrivate *priv = get_priv (GTK_HLIST (object));
    if (priv) {
        if (priv->store) {
            g_object_unref (priv->store);
        }
        g_free (priv);
        g_object_set_data (object, "hlist-priv", NULL);
    }
    G_OBJECT_CLASS (gtk_hlist_parent_class)->finalize (object);
}

static void
gtk_hlist_class_init (GtkHListClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = gtk_hlist_finalize;

    click_column_signal = g_signal_new (
        "click_column", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (GtkHListClass, click_column), NULL, NULL,
        g_cclosure_marshal_VOID__INT, G_TYPE_NONE, 1, G_TYPE_INT);

    /* select_row mirrors the legacy GtkCList "select_row" signature
	 * (row, column, GdkEvent*). The compat shim emits this from a
	 * GtkTreeSelection "changed" handler.
	 *
	 * The third arg is typed as G_TYPE_POINTER instead of
	 * GDK_TYPE_EVENT because the va-args FFI machinery GLib uses for
	 * the generic marshaller can't bridge GdkEvent through va_list:
	 *   "va_to_ffi_type: Unsupported fundamental type: GdkEvent"
	 * fires every time a handler runs. We always emit NULL for the
	 * event parameter (no caller ever reads it) so a typeless pointer
	 * is identical at the call boundary and dodges the warning. */
    select_row_signal = g_signal_new (
        "select_row", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (GtkHListClass, select_row), NULL, NULL,
        NULL, /* generic marshaller */
        G_TYPE_NONE, 3, G_TYPE_INT, G_TYPE_INT, G_TYPE_POINTER);
}

static void
gtk_hlist_init (GtkHList *self)
{
    self->rows = 0;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static gboolean
iter_at_row (GtkHListPrivate *priv, gint row, GtkTreeIter *iter)
{
    GtkTreePath *path;
    gboolean ok;

    if (row < 0) {
        return FALSE;
    }
    path = gtk_tree_path_new_from_indices (row, -1);
    ok = gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), iter, path);
    gtk_tree_path_free (path);
    return ok;
}

static void
refresh_row_count (GtkHList *hlist)
{
    GtkHListPrivate *priv = get_priv (hlist);
    hlist->rows = priv ? gtk_tree_model_iter_n_children (
                             GTK_TREE_MODEL (priv->store), NULL)
                       : 0;
}

/* ------------------------------------------------------------------ */
/* Custom overlay cell renderer                                        */
/*                                                                     */
/* GtkCellRenderer subclass that paints the pixbuf at the column's    */
/* left edge at its NATURAL width and draws the text on top at a       */
/* fixed x offset. Used by the user list to keep names aligned         */
/* regardless of icon width — wide icons (Hotline banner icons) act    */
/* as a row background instead of pushing names rightward.             */
/*                                                                     */
/* GtkTreeView's standard pack_start (pixbuf FALSE) + pack_start       */
/* (text TRUE) layout can't do this — the text renderer's allocation   */
/* starts where the pixbuf renderer's ends, and pixbufs render only    */
/* within their allocated cell area. We replace both renderers with    */
/* a single instance of this class for the affected column.            */
/* ------------------------------------------------------------------ */

#define GTK_TYPE_HLIST_OVERLAY_CELL (gtk_hlist_overlay_cell_get_type ())
G_DECLARE_FINAL_TYPE (GtkHListOverlayCell, gtk_hlist_overlay_cell, GTK,
                      HLIST_OVERLAY_CELL, GtkCellRenderer)

struct _GtkHListOverlayCell {
    GtkCellRenderer parent_instance;
    GdkPixbuf *pixbuf;
    gchar *text;
    gchar *foreground;
    gboolean foreground_set;
    gint text_x_offset;
    /* Number of fully-transparent columns at the pixbuf's left edge.
	 * Mac wide-banner cicns are authored for icon-at-left + name-to-
	 * right layout, with the visible banner art packed in the right
	 * half of the image and the left half left transparent for the
	 * name area. For our overlay layout we shift the pixbuf left by
	 * this amount so the visible art lines up with the cell's left
	 * edge (where the name overlays it). Computed when the pixbuf
	 * property is set. */
    gint left_pad;
    /* Multiplier applied to both the pixbuf rendering and the
	 * font size. Default 1.0 (native). Bumped above 1.0 to scale
	 * up the visual without changing the underlying icon source
	 * or default font. Used by the Users window for a 1.25x
	 * larger row. */
    gdouble pixel_scale;
    /* When TRUE, draw a 1 px contrasting outline around the text
	 * before drawing it in the foreground colour. Improves
	 * readability for light names rendered on top of busy banner-
	 * style icon backgrounds (where, e.g., a pink user name can
	 * disappear into the banner art). The outline colour is
	 * computed from the foreground's luminance — dark fg gets a
	 * white outline, light fg gets a black one. */
    gboolean text_outline;
};

G_DEFINE_TYPE (GtkHListOverlayCell, gtk_hlist_overlay_cell,
               GTK_TYPE_CELL_RENDERER)

enum {
    OV_PROP_0,
    OV_PROP_PIXBUF,
    OV_PROP_TEXT,
    OV_PROP_FOREGROUND,
    OV_PROP_FOREGROUND_SET,
    OV_PROP_TEXT_X_OFFSET,
    OV_PROP_PIXEL_SCALE,
    OV_PROP_TEXT_OUTLINE,
    OV_N_PROPS
};

static GParamSpec *ov_props[OV_N_PROPS];

static void
gtk_hlist_overlay_cell_init (GtkHListOverlayCell *self)
{
    self->pixbuf = NULL;
    self->text = NULL;
    self->foreground = NULL;
    self->foreground_set = FALSE;
    self->text_x_offset = 4;
    self->left_pad = 0;
    self->pixel_scale = 1.0;
    self->text_outline = FALSE;
}

/* Mac wide-banner cicn icons follow a community convention: the
 * first ~200 pixels of the bitmap are reserved for the user-name
 * area (Mac classic drew the user name immediately to the right
 * of the icon, so for a wide "banner-style" icon the designer
 * left that left portion blank — either by leaving it
 * transparent via the mask, or filling it with opaque black) and
 * the actual banner art lives in the right portion of the
 * bitmap. The offset is consistent across icon sets because
 * everyone authored against the same Mac-Hotline reference
 * layout.
 *
 * For our overlay layout (name at fixed x with icon BEHIND it)
 * we want the banner art to sit behind the name, not floating
 * off the right edge of the column. Cropping the bitmap's left
 * ~200 px and drawing the rest from the cell's left edge does
 * exactly that.
 *
 * Empirically a fixed crop matches all the wide-banner icons we
 * have seen (Badmoon's set: jokki.-style mask-based and
 * SkAtE!@/Bouncer/heavy_early-style no-mask black-filled both
 * use the same convention). No-op for narrow icons. */
#define WIDE_BANNER_THRESHOLD 48 /* px — anything narrower is a normal icon */
#define WIDE_BANNER_LEFT_PAD 200 /* px to crop off the left of wide banners */

static gint
compute_left_padding (GdkPixbuf *pb)
{
    int w;
    if (!pb) {
        return 0;
    }
    w = gdk_pixbuf_get_width (pb);
    if (w < WIDE_BANNER_THRESHOLD) {
        return 0;
    }
    return MIN (WIDE_BANNER_LEFT_PAD, w);
}

static void
gtk_hlist_overlay_cell_finalize (GObject *object)
{
    GtkHListOverlayCell *self = (GtkHListOverlayCell *)object;
    g_clear_object (&self->pixbuf);
    g_clear_pointer (&self->text, g_free);
    g_clear_pointer (&self->foreground, g_free);
    G_OBJECT_CLASS (gtk_hlist_overlay_cell_parent_class)->finalize (object);
}

static void
gtk_hlist_overlay_cell_get_property (GObject *object, guint prop_id,
                                     GValue *value, GParamSpec *pspec)
{
    GtkHListOverlayCell *self = (GtkHListOverlayCell *)object;
    switch (prop_id) {
    case OV_PROP_PIXBUF:
        g_value_set_object (value, self->pixbuf);
        break;
    case OV_PROP_TEXT:
        g_value_set_string (value, self->text);
        break;
    case OV_PROP_FOREGROUND:
        g_value_set_string (value, self->foreground);
        break;
    case OV_PROP_FOREGROUND_SET:
        g_value_set_boolean (value, self->foreground_set);
        break;
    case OV_PROP_TEXT_X_OFFSET:
        g_value_set_int (value, self->text_x_offset);
        break;
    case OV_PROP_PIXEL_SCALE:
        g_value_set_double (value, self->pixel_scale);
        break;
    case OV_PROP_TEXT_OUTLINE:
        g_value_set_boolean (value, self->text_outline);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_hlist_overlay_cell_set_property (GObject *object, guint prop_id,
                                     const GValue *value, GParamSpec *pspec)
{
    GtkHListOverlayCell *self = (GtkHListOverlayCell *)object;
    switch (prop_id) {
    case OV_PROP_PIXBUF:
        g_clear_object (&self->pixbuf);
        self->pixbuf = (GdkPixbuf *)g_value_dup_object (value);
        self->left_pad = compute_left_padding (self->pixbuf);
        break;
    case OV_PROP_TEXT:
        g_free (self->text);
        self->text = g_value_dup_string (value);
        break;
    case OV_PROP_FOREGROUND:
        g_free (self->foreground);
        self->foreground = g_value_dup_string (value);
        break;
    case OV_PROP_FOREGROUND_SET:
        self->foreground_set = g_value_get_boolean (value);
        break;
    case OV_PROP_TEXT_X_OFFSET:
        self->text_x_offset = g_value_get_int (value);
        break;
    case OV_PROP_PIXEL_SCALE:
        self->pixel_scale = g_value_get_double (value);
        if (self->pixel_scale < 0.1) {
            self->pixel_scale = 1.0;
        }
        break;
    case OV_PROP_TEXT_OUTLINE:
        self->text_outline = g_value_get_boolean (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static PangoLayout *
gtk_hlist_overlay_cell_make_layout (GtkHListOverlayCell *self,
                                    GtkWidget *widget)
{
    PangoLayout *layout
        = gtk_widget_create_pango_layout (widget, self->text ? self->text : "");
    pango_layout_set_single_paragraph_mode (layout, TRUE);
    pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
    /* Apply per-cell font scale via a PangoAttribute. Cheaper than
	 * cloning the widget's font description and tweaking its size:
	 * Pango multiplies the effective size by `scale` at layout time. */
    if (self->pixel_scale != 1.0) {
        PangoAttrList *attrs = pango_attr_list_new ();
        pango_attr_list_insert (attrs, pango_attr_scale_new (self->pixel_scale));
        pango_layout_set_attributes (layout, attrs);
        pango_attr_list_unref (attrs);
    }
    return layout;
}

static void
gtk_hlist_overlay_cell_get_preferred_width (GtkCellRenderer *cell,
                                            GtkWidget *widget, gint *minimum,
                                            gint *natural)
{
    GtkHListOverlayCell *self = (GtkHListOverlayCell *)cell;
    int txt_w = 0;
    if (self->text && *self->text) {
        PangoLayout *layout = gtk_hlist_overlay_cell_make_layout (self, widget);
        pango_layout_get_pixel_size (layout, &txt_w, NULL);
        g_object_unref (layout);
    }
    /* Deliberately *do not* report the pixbuf width as natural: this
	 * is an overlay cell, the pixbuf renders inside whatever
	 * cell_area we are given (and is clipped to it in snapshot).
	 * If we reported pb_w here, GtkCellArea would expand cell_area
	 * to the pixbuf's full width — overriding the column's fixed
	 * width — and the snapshot's push_clip would then be a 400+px
	 * rectangle that doesn't actually clip the wide-banner pixbuf
	 * back inside the column. */
    double scale = self->pixel_scale > 0 ? self->pixel_scale : 1.0;
    int x_off = (int)(self->text_x_offset * scale + 0.5);
    int natural_w = x_off + txt_w;
    if (minimum) {
        *minimum = x_off + 16; /* a reasonable floor */
    }
    if (natural) {
        *natural = natural_w;
    }
}

static void
gtk_hlist_overlay_cell_get_preferred_height (GtkCellRenderer *cell,
                                             GtkWidget *widget, gint *minimum,
                                             gint *natural)
{
    GtkHListOverlayCell *self = (GtkHListOverlayCell *)cell;
    int pb_h = self->pixbuf ? gdk_pixbuf_get_height (self->pixbuf) : 0;
    int txt_h = 0;
    if (self->text && *self->text) {
        PangoLayout *layout = gtk_hlist_overlay_cell_make_layout (self, widget);
        pango_layout_get_pixel_size (layout, NULL, &txt_h);
        g_object_unref (layout);
    }
    int h = MAX (pb_h, txt_h);
    if (minimum) {
        *minimum = h;
    }
    if (natural) {
        *natural = h;
    }
}

static void
gtk_hlist_overlay_cell_snapshot (GtkCellRenderer *cell, GtkSnapshot *snapshot,
                                 GtkWidget *widget,
                                 const GdkRectangle *background_area,
                                 const GdkRectangle *cell_area,
                                 GtkCellRendererState flags)
{
    GtkHListOverlayCell *self = (GtkHListOverlayCell *)cell;
    GtkTreeViewColumn *col;
    int col_width;
    int clip_x, clip_w;
    (void)flags;

    /* GtkTreeView in GTK 4 passes a cell_area / background_area that
	 * starts at the column's left edge but extends to the *end of
	 * the row*, not to the column's right edge. We need the column's
	 * actual fixed width to clip the wide-banner pixbuf back inside
	 * the column boundary. Look up the column from the data we
	 * stashed on the renderer at install time. */
    col = (GtkTreeViewColumn *)g_object_get_data (G_OBJECT (cell),
                                                  "hlist-overlay-col");
    col_width = col ? gtk_tree_view_column_get_width (col) : cell_area->width;
    clip_x = cell_area->x;
    clip_w = MIN (cell_area->width, col_width);

    debug_log ("overlay",
               "snapshot: cell=(%d,%d %dx%d) bg=(%d,%d %dx%d) "
               "col_w=%d clip=(%d,%d) pb=%s pb_w=%d lpad=%d "
               "text=\"%s\" xoff=%d",
               cell_area->x, cell_area->y, cell_area->width, cell_area->height,
               background_area->x, background_area->y, background_area->width,
               background_area->height, col_width, clip_x, clip_w,
               self->pixbuf ? "yes" : "no",
               self->pixbuf ? gdk_pixbuf_get_width (self->pixbuf) : 0,
               self->left_pad, self->text ? self->text : "(null)",
               self->text_x_offset);

    gtk_snapshot_push_clip (
        snapshot,
        &GRAPHENE_RECT_INIT ((float)clip_x, (float)cell_area->y, (float)clip_w,
                             (float)cell_area->height));

    if (self->pixbuf) {
        int pb_w = gdk_pixbuf_get_width (self->pixbuf);
        int pb_h = gdk_pixbuf_get_height (self->pixbuf);
        double scale = self->pixel_scale > 0 ? self->pixel_scale : 1.0;
        int draw_w = (int)(pb_w * scale + 0.5);
        int draw_h = (int)(pb_h * scale + 0.5);
        int scaled_lpad = (int)(self->left_pad * scale + 0.5);
        int draw_x, draw_y;
        GdkTexture *tex;

        /* Shift the pixbuf left by its (scaled) transparent left-
		 * padding so the first column with visible content lands at
		 * the cell's left edge. Mac wide-banner cicns are authored
		 * with the art packed in the right half of the bitmap and
		 * the left half left transparent for the user name (Mac-
		 * classic icon-then-name layout). Cropping that padding off
		 * the left and letting the push_clip above trim anything
		 * that still spills off the right gives us the banner
		 * sitting behind the name with the name at its fixed
		 * offset. The destination rectangle's width/height scale
		 * the texture up — GSK does the upscaling for us. */
        draw_x = clip_x - scaled_lpad;
        draw_y = cell_area->y + (cell_area->height - draw_h) / 2;

        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        tex = gdk_texture_new_for_pixbuf (self->pixbuf);
        G_GNUC_END_IGNORE_DEPRECATIONS

        gtk_snapshot_save (snapshot);
        gtk_snapshot_translate (
            snapshot, &GRAPHENE_POINT_INIT ((float)draw_x, (float)draw_y));
        gtk_snapshot_append_texture (
            snapshot, tex,
            &GRAPHENE_RECT_INIT (0, 0, (float)draw_w, (float)draw_h));
        gtk_snapshot_restore (snapshot);
        g_object_unref (tex);
    }

    /* Text at fixed x offset on top of the pixbuf. */
    if (self->text && *self->text) {
        PangoLayout *layout = gtk_hlist_overlay_cell_make_layout (self, widget);
        int tw, th;
        GdkRGBA rgba = { 0, 0, 0, 1 };
        gboolean have_rgba = FALSE;
        float tx, ty;
        /* Scale the icon-side spacing alongside the pixel-scale so
		 * the offset stays proportional to the (also scaled) icon
		 * width. Configured value (e.g. 36 px) is "px at 1.0×" —
		 * at 1.25× the larger icon needs ~45 px of clearance. */
        double scale = self->pixel_scale > 0 ? self->pixel_scale : 1.0;
        int x_off = (int)(self->text_x_offset * scale + 0.5);

        pango_layout_get_pixel_size (layout, &tw, &th);
        /* Clamp ellipsize width so we don't overflow the column. */
        pango_layout_set_width (layout, (clip_w - x_off) * PANGO_SCALE);

        if (self->foreground_set && self->foreground) {
            have_rgba = gdk_rgba_parse (&rgba, self->foreground);
        }
        if (!have_rgba) {
            GtkStyleContext *ctx = gtk_widget_get_style_context (widget);
            G_GNUC_BEGIN_IGNORE_DEPRECATIONS
            gtk_style_context_get_color (ctx, &rgba);
            G_GNUC_END_IGNORE_DEPRECATIONS
        }

        tx = (float)(cell_area->x + x_off);
        ty = (float)(cell_area->y + (cell_area->height - th) / 2);

        /* Optional 4-direction outline. Draws the same layout four
		 * times at ±1 px offsets in pure black before the fg draw
		 * — black halos add depth without tinting bright nicks.
		 * Early attempts at "smart" white-on-dark / black-on-light
		 * outlines made red names look pink (the white halo bled
		 * into the red fill perceptually); plain black behind every
		 * colour reads as a uniform pseudo-stroke and keeps the
		 * fg's hue intact. The Users window's nicks are all bright
		 * by convention, so the loss of "white-halo-on-dark-text"
		 * doesn't cost us anything in practice. */
        if (self->text_outline) {
            const GdkRGBA outline = { 0.0, 0.0, 0.0, 1.0 };
            const float offsets[4][2] = {
                { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }
            };
            for (int i = 0; i < 4; i++) {
                gtk_snapshot_save (snapshot);
                gtk_snapshot_translate (
                    snapshot,
                    &GRAPHENE_POINT_INIT (tx + offsets[i][0],
                                          ty + offsets[i][1]));
                gtk_snapshot_append_layout (snapshot, layout, &outline);
                gtk_snapshot_restore (snapshot);
            }
        }

        gtk_snapshot_save (snapshot);
        gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (tx, ty));
        gtk_snapshot_append_layout (snapshot, layout, &rgba);
        gtk_snapshot_restore (snapshot);
        g_object_unref (layout);
    }

    gtk_snapshot_pop (snapshot);
}

static void
gtk_hlist_overlay_cell_class_init (GtkHListOverlayCellClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkCellRendererClass *cell_class = GTK_CELL_RENDERER_CLASS (klass);

    object_class->finalize = gtk_hlist_overlay_cell_finalize;
    object_class->get_property = gtk_hlist_overlay_cell_get_property;
    object_class->set_property = gtk_hlist_overlay_cell_set_property;

    cell_class->snapshot = gtk_hlist_overlay_cell_snapshot;
    cell_class->get_preferred_width
        = gtk_hlist_overlay_cell_get_preferred_width;
    cell_class->get_preferred_height
        = gtk_hlist_overlay_cell_get_preferred_height;

    ov_props[OV_PROP_PIXBUF] = g_param_spec_object (
        "pixbuf", NULL, NULL, GDK_TYPE_PIXBUF, G_PARAM_READWRITE);
    ov_props[OV_PROP_TEXT]
        = g_param_spec_string ("text", NULL, NULL, NULL, G_PARAM_READWRITE);
    ov_props[OV_PROP_FOREGROUND] = g_param_spec_string (
        "foreground", NULL, NULL, NULL, G_PARAM_READWRITE);
    ov_props[OV_PROP_FOREGROUND_SET] = g_param_spec_boolean (
        "foreground-set", NULL, NULL, FALSE, G_PARAM_READWRITE);
    ov_props[OV_PROP_TEXT_X_OFFSET] = g_param_spec_int (
        "text-x-offset", NULL, NULL, 0, G_MAXINT, 4, G_PARAM_READWRITE);
    ov_props[OV_PROP_PIXEL_SCALE] = g_param_spec_double (
        "pixel-scale", NULL, NULL, 0.1, 8.0, 1.0, G_PARAM_READWRITE);
    ov_props[OV_PROP_TEXT_OUTLINE] = g_param_spec_boolean (
        "text-outline", NULL, NULL, FALSE, G_PARAM_READWRITE);

    g_object_class_install_properties (object_class, OV_N_PROPS, ov_props);
}

static GdkPixbuf *
pixmap_to_pixbuf (GdkPixmap *pixmap, GdkBitmap *mask)
{
    /* Phase 3.2: GdkPixmap and GdkBitmap are aliased to GdkPixbuf in
	 * session.h for the GTK 3 transition. The legacy implementation here
	 * called gdk_pixbuf_get_from_drawable + gdk_drawable_get_image and
	 * applied the bitmap as alpha by hand — none of which exists on
	 * GTK 3. Now: take a strong ref to the input so the caller's free
	 * pattern (caller drops its own ref afterwards) doesn't double-free,
	 * and ignore the bitmap mask: pixbufs already carry alpha. */
    (void)mask;
    if (!pixmap) {
        return NULL;
    }
    return g_object_ref ((GdkPixbuf *)pixmap);
}

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

static void
on_column_clicked (GtkTreeViewColumn *col, gpointer user_data)
{
    GtkHList *hlist = GTK_HLIST (user_data);
    gint idx
        = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (col), "hlist-col-idx"));
    g_signal_emit (hlist, click_column_signal, 0, idx);
}

/*
 * Per-column sort function used by every hlist column so clicking the
 * header sorts the rows by that column. Uses g_utf8_collate to do a
 * locale-aware string compare; numeric columns stored as decimal text
 * (for example "10" vs "9") sort lexically rather than numerically,
 * which is good enough for now — consumers that want a smarter compare
 * can override per-column with gtk_tree_sortable_set_sort_func().
 *
 * user_data is the model column index (HLIST_COL_TEXT(i)) wrapped in
 * a GINT pointer.
 */
static gint
hlist_text_column_compare (GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
                           gpointer user_data)
{
    gint col = GPOINTER_TO_INT (user_data);
    gchar *sa = NULL;
    gchar *sb = NULL;
    gint result;

    gtk_tree_model_get (model, a, col, &sa, -1);
    gtk_tree_model_get (model, b, col, &sb, -1);

    /* Numeric-looking strings sort numerically when both sides parse
	 * cleanly — keeps "9" before "10" without consumers having to
	 * register a custom compare. Falls through to a locale-aware
	 * string compare otherwise. */
    if (sa && sb) {
        gchar *ea = NULL, *eb = NULL;
        gint64 na, nb;

        na = g_ascii_strtoll (sa, &ea, 10);
        nb = g_ascii_strtoll (sb, &eb, 10);
        if (ea && eb && ea != sa && eb != sb && *ea == '\0' && *eb == '\0') {
            result = (na < nb) ? -1 : (na > nb) ? 1 : 0;
        } else {
            result = g_utf8_collate (sa, sb);
        }
    } else if (sa) {
        result = 1;
    } else if (sb) {
        result = -1;
    } else {
        result = 0;
    }

    g_free (sa);
    g_free (sb);
    return result;
}

static void
on_selection_changed (GtkTreeSelection *sel, gpointer user_data)
{
    GtkHList *hlist = GTK_HLIST (user_data);
    GtkTreeModel *model;
    GtkTreeIter iter;

    if (gtk_tree_selection_get_selected (sel, &model, &iter)) {
        GtkTreePath *path = gtk_tree_model_get_path (model, &iter);
        gint row = gtk_tree_path_get_indices (path)[0];
        gtk_tree_path_free (path);
        g_signal_emit (hlist, select_row_signal, 0, row, 0, NULL);
    }
}

static GtkWidget *
gtk_hlist_construct (gint n_columns, gchar **titles)
{
    GtkHList *self;
    GtkHListPrivate *priv;
    GType *types;
    GtkTreeSelection *sel;
    gint i;

    g_return_val_if_fail (n_columns > 0, NULL);

    self = g_object_new (GTK_TYPE_HLIST, NULL);
    priv = g_new0 (GtkHListPrivate, 1);
    priv->n_columns = n_columns;
    priv->sort_column = 0;
    priv->sort_type = GTK_SORT_ASCENDING;
    g_object_set_data (G_OBJECT (self), "hlist-priv", priv);

    types = g_new0 (GType, HLIST_TOTAL_COLS (n_columns));
    types[HLIST_COL_DATA] = G_TYPE_POINTER;
    types[HLIST_COL_FG] = G_TYPE_STRING;
    types[HLIST_COL_FG_SET] = G_TYPE_BOOLEAN;
    for (i = 0; i < n_columns; i++) {
        types[HLIST_COL_TEXT (i)] = G_TYPE_STRING;
        types[HLIST_COL_PIXBUF (n_columns, i)] = GDK_TYPE_PIXBUF;
    }
    priv->store = gtk_list_store_newv (HLIST_TOTAL_COLS (n_columns), types);
    g_free (types);

    gtk_tree_view_set_model (GTK_TREE_VIEW (self),
                             GTK_TREE_MODEL (priv->store));

    /* GtkTreeView's "interactive search" feature is on by default —
	 * any keystroke while the tree has focus pops up a small search
	 * entry. None of our lists (users, tracker, tasks, files,
	 * options icon list) want that UX, but more importantly the
	 * lazy popover construction inside
	 * gtk_tree_view_ensure_interactive_directory has a GTK 4 bug:
	 * it fires
	 *   Gtk-CRITICAL: gtk_css_node_insert_after: assertion
	 *   'previous_sibling == NULL || previous_sibling->parent == parent'
	 * the first time a key (including modifier keys like Alt) arrives
	 * at the tree view. The popover is parented under the tree view
	 * and the CSS node hierarchy is set up in the wrong order
	 * internally. Disabling search side-steps the whole code path. */
    gtk_tree_view_set_enable_search (GTK_TREE_VIEW (self), FALSE);

    for (i = 0; i < n_columns; i++) {
        GtkTreeViewColumn *col;
        GtkCellRenderer *pixr;
        GtkCellRenderer *txtr;
        const gchar *title = (titles && titles[i]) ? titles[i] : "";

        col = gtk_tree_view_column_new ();
        gtk_tree_view_column_set_title (col, title);
        gtk_tree_view_column_set_resizable (col, TRUE);
        gtk_tree_view_column_set_sizing (col, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_clickable (col, TRUE);
        g_object_set_data (G_OBJECT (col), "hlist-col-idx",
                           GINT_TO_POINTER (i));
        g_signal_connect (col, "clicked", G_CALLBACK (on_column_clicked), self);

        pixr = gtk_cell_renderer_pixbuf_new ();
        gtk_tree_view_column_pack_start (col, pixr, FALSE);
        gtk_tree_view_column_set_attributes (
            col, pixr, "pixbuf", HLIST_COL_PIXBUF (n_columns, i), NULL);

        txtr = gtk_cell_renderer_text_new ();
        gtk_tree_view_column_pack_start (col, txtr, TRUE);
        gtk_tree_view_column_set_attributes (
            col, txtr, "text", HLIST_COL_TEXT (i), "foreground", HLIST_COL_FG,
            "foreground-set", HLIST_COL_FG_SET, NULL);

        gtk_tree_view_append_column (GTK_TREE_VIEW (self), col);

        /* Phase 5: register a sort function for this text column on
		 * the model and link the column header to it via
		 * sort_column_id, but only when the list will actually have
		 * a visible header to click — set_sort_column_id installs an
		 * indicator-arrow widget hierarchy that asserts during
		 * mapping if the column header isn't present (the
		 * gtk_css_node_insert_after assertion). gtk_hlist_new() lists
		 * (no titles) keep the legacy gtk_hlist_set_compare_func +
		 * gtk_hlist_sort path and don't get auto-sort.
		 *
		 * MUST be called AFTER append_column, not before. Otherwise
		 * the indicator-arrow's CSS node is inserted into a column
		 * header whose own CSS node hasn't been parented to the
		 * tree view yet, and GTK 4 fires
		 * "gtk_css_node_insert_after: assertion
		 *  'previous_sibling == NULL ||
		 *   previous_sibling->parent == parent' failed"
		 * once per titled column on every list construction. (Showed
		 * up as two CRITICALs on every Users window open, two on every
		 * Tracker / Tasks / Files list, etc.) */
        if (titles != NULL) {
            gtk_tree_sortable_set_sort_func (
                GTK_TREE_SORTABLE (priv->store), HLIST_COL_TEXT (i),
                hlist_text_column_compare, GINT_TO_POINTER (HLIST_COL_TEXT (i)),
                NULL);
            gtk_tree_view_column_set_sort_column_id (col, HLIST_COL_TEXT (i));
        }
    }

    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (self), titles != NULL);

    sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
    gtk_tree_selection_set_mode (sel, GTK_SELECTION_SINGLE);
    g_signal_connect (sel, "changed", G_CALLBACK (on_selection_changed), self);

    return GTK_WIDGET (self);
}

GtkWidget *
gtk_hlist_new (gint columns)
{
    return gtk_hlist_construct (columns, NULL);
}

GtkWidget *
gtk_hlist_new_with_titles (gint columns, gchar *titles[])
{
    return gtk_hlist_construct (columns, titles);
}

/* ------------------------------------------------------------------ */
/* Visual / column properties                                          */
/* ------------------------------------------------------------------ */

void
gtk_hlist_set_shadow_type (GtkHList *hlist, int type)
{
    (void)hlist;
    (void)type;
    /* The legacy widget drew its own shadow; GtkTreeView delegates
	 * framing to its enclosing GtkScrolledWindow. The consumers
	 * always wrap us in one, so this is a no-op. Parameter is `int'
	 * (was GtkShadowType in pre-GTK4) so the GTK_SHADOW_NONE call
	 * sites still pass — see compat values in gtk_hlist_compat.h. */
}

void
gtk_hlist_set_selection_mode (GtkHList *hlist, GtkSelectionMode mode)
{
    GtkTreeSelection *sel;
    g_return_if_fail (GTK_IS_HLIST (hlist));
    sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (hlist));
    gtk_tree_selection_set_mode (sel, mode);
}

void
gtk_hlist_set_column_width (GtkHList *hlist, gint column, gint width)
{
    GtkTreeViewColumn *col;
    g_return_if_fail (GTK_IS_HLIST (hlist));
    col = gtk_tree_view_get_column (GTK_TREE_VIEW (hlist), column);
    if (col) {
        gtk_tree_view_column_set_sizing (col, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width (col, MAX (1, width));
    }
}

void
gtk_hlist_set_column_justification (GtkHList *hlist, gint column,
                                    GtkJustification justification)
{
    GtkTreeViewColumn *col;
    gfloat align = 0.0;
    GList *cells, *l;

    g_return_if_fail (GTK_IS_HLIST (hlist));
    col = gtk_tree_view_get_column (GTK_TREE_VIEW (hlist), column);
    if (!col) {
        return;
    }

    switch (justification) {
    case GTK_JUSTIFY_LEFT:
        align = 0.0;
        break;
    case GTK_JUSTIFY_CENTER:
        align = 0.5;
        break;
    case GTK_JUSTIFY_RIGHT:
        align = 1.0;
        break;
    default:
        align = 0.0;
        break;
    }
    gtk_tree_view_column_set_alignment (col, align);
    cells = gtk_cell_layout_get_cells (GTK_CELL_LAYOUT (col));
    for (l = cells; l; l = l->next) {
        g_object_set (l->data, "xalign", align, NULL);
    }
    g_list_free (cells);
}

void
gtk_hlist_set_row_height (GtkHList *hlist, guint height)
{
    (void)hlist;
    (void)height;
    /* GtkTreeView sizes rows to content; a fixed minimum can be
	 * imposed by setting "height" on every cell renderer, but no
	 * in-tree consumer relies on the precise pixel value. Treat
	 * this as a hint we ignore. */
}

void
gtk_hlist_moveto (GtkHList *hlist, gint row, gint column, gfloat row_align,
                  gfloat col_align)
{
    GtkTreePath *path;
    GtkTreeViewColumn *col = NULL;

    g_return_if_fail (GTK_IS_HLIST (hlist));
    if (row < 0) {
        return;
    }
    path = gtk_tree_path_new_from_indices (row, -1);
    if (column >= 0) {
        col = gtk_tree_view_get_column (GTK_TREE_VIEW (hlist), column);
    }
    gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (hlist), path, col, TRUE,
                                  row_align, col_align);
    gtk_tree_path_free (path);
}

/* ------------------------------------------------------------------ */
/* Freeze / thaw                                                       */
/* ------------------------------------------------------------------ */

void
gtk_hlist_freeze (GtkHList *hlist)
{
    GtkHListPrivate *priv;
    g_return_if_fail (GTK_IS_HLIST (hlist));
    priv = get_priv (hlist);
    if (priv->freeze_count++ == 0) {
        gtk_tree_view_set_model (GTK_TREE_VIEW (hlist), NULL);
    }
}

void
gtk_hlist_thaw (GtkHList *hlist)
{
    GtkHListPrivate *priv;
    g_return_if_fail (GTK_IS_HLIST (hlist));
    priv = get_priv (hlist);
    if (priv->freeze_count > 0 && --priv->freeze_count == 0) {
        gtk_tree_view_set_model (GTK_TREE_VIEW (hlist),
                                 GTK_TREE_MODEL (priv->store));
    }
}

/* ------------------------------------------------------------------ */
/* Cell content                                                        */
/* ------------------------------------------------------------------ */

void
gtk_hlist_set_text (GtkHList *hlist, gint row, gint column, const gchar *text)
{
    GtkHListPrivate *priv;
    GtkTreeIter iter;

    g_return_if_fail (GTK_IS_HLIST (hlist));
    priv = get_priv (hlist);
    if (column < 0 || column >= priv->n_columns) {
        return;
    }
    if (!iter_at_row (priv, row, &iter)) {
        return;
    }
    gtk_list_store_set (priv->store, &iter, HLIST_COL_TEXT (column),
                        text ? text : "", -1);
}

void
gtk_hlist_set_pixtext (GtkHList *hlist, gint row, gint column,
                       const gchar *text, guint8 spacing, GdkPixmap *pixmap,
                       GdkBitmap *mask)
{
    GtkHListPrivate *priv;
    GtkTreeIter iter;
    GdkPixbuf *pb;

    g_return_if_fail (GTK_IS_HLIST (hlist));
    (void)spacing;
    priv = get_priv (hlist);
    if (column < 0 || column >= priv->n_columns) {
        return;
    }
    if (!iter_at_row (priv, row, &iter)) {
        return;
    }
    pb = pixmap_to_pixbuf (pixmap, mask);
    gtk_list_store_set (priv->store, &iter, HLIST_COL_TEXT (column),
                        text ? text : "",
                        HLIST_COL_PIXBUF (priv->n_columns, column), pb, -1);
    if (pb) {
        g_object_unref (pb);
    }
}

void
gtk_hlist_column_set_overlay_pixtext (GtkHList *hlist, gint column,
                                      gint text_x_offset)
{
    GtkHListPrivate *priv;
    GtkTreeViewColumn *col;
    GtkCellRenderer *cell;

    g_return_if_fail (GTK_IS_HLIST (hlist));
    priv = get_priv (hlist);
    if (column < 0 || column >= priv->n_columns) {
        return;
    }
    col = gtk_tree_view_get_column (GTK_TREE_VIEW (hlist), column);
    if (!col) {
        return;
    }

    /* Clear the column's existing renderers (pack_start pixbuf +
	 * pack_start text installed by gtk_hlist constructor) so we can
	 * replace them with the overlay cell. */
    gtk_cell_layout_clear (GTK_CELL_LAYOUT (col));

    cell = g_object_new (GTK_TYPE_HLIST_OVERLAY_CELL, "text-x-offset",
                         text_x_offset, NULL);
    /* Stash the column so the snapshot vfunc can read its width —
	 * GtkTreeView's cell_area in GTK 4 extends to the row's right
	 * edge, not the column's, so we can't derive the column width
	 * from anything the framework hands the renderer. */
    g_object_set_data (G_OBJECT (cell), "hlist-overlay-col", col);
    gtk_tree_view_column_pack_start (col, cell, TRUE);
    gtk_tree_view_column_set_attributes (
        col, cell, "pixbuf", HLIST_COL_PIXBUF (priv->n_columns, column), "text",
        HLIST_COL_TEXT (column), "foreground", HLIST_COL_FG, "foreground-set",
        HLIST_COL_FG_SET, NULL);
}

void
gtk_hlist_column_set_overlay_decoration (GtkHList *hlist, gint column,
                                         gdouble pixel_scale,
                                         gboolean text_outline)
{
    GtkHListPrivate *priv;
    GtkTreeViewColumn *col;
    GList *cells, *l;

    g_return_if_fail (GTK_IS_HLIST (hlist));
    priv = get_priv (hlist);
    if (column < 0 || column >= priv->n_columns) {
        return;
    }
    /* File-level G_GNUC_BEGIN_IGNORE_DEPRECATIONS at the top of
	 * this TU already covers GtkTreeView / GtkCellLayout calls. */
    col = gtk_tree_view_get_column (GTK_TREE_VIEW (hlist), column);
    if (!col) {
        return;
    }
    cells = gtk_cell_layout_get_cells (GTK_CELL_LAYOUT (col));
    for (l = cells; l; l = l->next) {
        if (GTK_IS_HLIST_OVERLAY_CELL (l->data)) {
            g_object_set (l->data, "pixel-scale", pixel_scale, "text-outline",
                          text_outline, NULL);
            break;
        }
    }
    g_list_free (cells);
}

void
gtk_hlist_set_foreground (GtkHList *hlist, gint row, GdkRGBA *color)
{
    GtkHListPrivate *priv;
    GtkTreeIter iter;
    gchar buf[16];

    g_return_if_fail (GTK_IS_HLIST (hlist));
    priv = get_priv (hlist);
    if (!iter_at_row (priv, row, &iter)) {
        return;
    }
    if (color) {
        /* Phase 3.10: GdkRGBA channels are doubles 0..1 — scale to
		 * 16-bit to keep emitting the same #RRRRGGGGBBBB hex string
		 * the GtkCellRendererText 'foreground' attribute expects. */
        g_snprintf (
            buf, sizeof buf, "#%04x%04x%04x", (unsigned)(color->red * 65535),
            (unsigned)(color->green * 65535), (unsigned)(color->blue * 65535));
        gtk_list_store_set (priv->store, &iter, HLIST_COL_FG, buf,
                            HLIST_COL_FG_SET, TRUE, -1);
    } else {
        gtk_list_store_set (priv->store, &iter, HLIST_COL_FG, NULL,
                            HLIST_COL_FG_SET, FALSE, -1);
    }
}

/* ------------------------------------------------------------------ */
/* Row append / insert / remove / clear                                */
/* ------------------------------------------------------------------ */

static void
fill_row_text (GtkHListPrivate *priv, GtkTreeIter *iter, gchar **text)
{
    gint i;
    if (!text) {
        return;
    }
    for (i = 0; i < priv->n_columns; i++) {
        gtk_list_store_set (priv->store, iter, HLIST_COL_TEXT (i),
                            text[i] ? text[i] : "", -1);
    }
}

gint
gtk_hlist_append (GtkHList *hlist, gchar *text[])
{
    GtkHListPrivate *priv;
    GtkTreeIter iter;
    gint row;

    g_return_val_if_fail (GTK_IS_HLIST (hlist), -1);
    priv = get_priv (hlist);
    gtk_list_store_append (priv->store, &iter);
    fill_row_text (priv, &iter, text);
    row = gtk_tree_model_iter_n_children (GTK_TREE_MODEL (priv->store), NULL)
          - 1;
    refresh_row_count (hlist);
    return row;
}

gint
gtk_hlist_insert (GtkHList *hlist, gint row, gchar *text[])
{
    GtkHListPrivate *priv;
    GtkTreeIter iter;
    gint n;

    g_return_val_if_fail (GTK_IS_HLIST (hlist), -1);
    priv = get_priv (hlist);
    n = gtk_tree_model_iter_n_children (GTK_TREE_MODEL (priv->store), NULL);
    if (row < 0 || row > n) {
        row = n;
    }
    gtk_list_store_insert (priv->store, &iter, row);
    fill_row_text (priv, &iter, text);
    refresh_row_count (hlist);
    return row;
}

void
gtk_hlist_remove (GtkHList *hlist, gint row)
{
    GtkHListPrivate *priv;
    GtkTreeIter iter;

    g_return_if_fail (GTK_IS_HLIST (hlist));
    priv = get_priv (hlist);
    if (!iter_at_row (priv, row, &iter)) {
        return;
    }
    gtk_list_store_remove (priv->store, &iter);
    refresh_row_count (hlist);
}

void
gtk_hlist_clear (GtkHList *hlist)
{
    GtkHListPrivate *priv;
    g_return_if_fail (GTK_IS_HLIST (hlist));
    priv = get_priv (hlist);
    gtk_list_store_clear (priv->store);
    refresh_row_count (hlist);
}

/* ------------------------------------------------------------------ */
/* Row data                                                            */
/* ------------------------------------------------------------------ */

void
gtk_hlist_set_row_data (GtkHList *hlist, gint row, gpointer data)
{
    GtkHListPrivate *priv;
    GtkTreeIter iter;

    g_return_if_fail (GTK_IS_HLIST (hlist));
    priv = get_priv (hlist);
    if (!iter_at_row (priv, row, &iter)) {
        return;
    }
    gtk_list_store_set (priv->store, &iter, HLIST_COL_DATA, data, -1);
}

gpointer
gtk_hlist_get_row_data (GtkHList *hlist, gint row)
{
    GtkHListPrivate *priv;
    GtkTreeIter iter;
    gpointer data = NULL;

    g_return_val_if_fail (GTK_IS_HLIST (hlist), NULL);
    priv = get_priv (hlist);
    if (!iter_at_row (priv, row, &iter)) {
        return NULL;
    }
    gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter, HLIST_COL_DATA,
                        &data, -1);
    return data;
}

gint
gtk_hlist_find_row_from_data (GtkHList *hlist, gpointer data)
{
    GtkHListPrivate *priv;
    GtkTreeIter iter;
    gint row = 0;
    gboolean ok;

    g_return_val_if_fail (GTK_IS_HLIST (hlist), -1);
    priv = get_priv (hlist);
    ok = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (priv->store), &iter);
    while (ok) {
        gpointer cur = NULL;
        gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter, HLIST_COL_DATA,
                            &cur, -1);
        if (cur == data) {
            return row;
        }
        row++;
        ok = gtk_tree_model_iter_next (GTK_TREE_MODEL (priv->store), &iter);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Selection                                                           */
/* ------------------------------------------------------------------ */

void
gtk_hlist_select_row (GtkHList *hlist, gint row, gint column)
{
    GtkHListPrivate *priv;
    GtkTreeIter iter;
    GtkTreeSelection *sel;

    g_return_if_fail (GTK_IS_HLIST (hlist));
    (void)column;
    priv = get_priv (hlist);
    if (!iter_at_row (priv, row, &iter)) {
        return;
    }
    sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (hlist));
    gtk_tree_selection_select_iter (sel, &iter);
}

gint
gtk_hlist_get_selection_info (GtkHList *hlist, gint x, gint y, gint *row,
                              gint *column)
{
    GtkTreePath *path = NULL;
    GtkTreeViewColumn *col = NULL;
    gint bx = 0;
    gint by = 0;

    if (row) {
        *row = -1;
    }
    if (column) {
        *column = -1;
    }

    g_return_val_if_fail (GTK_IS_HLIST (hlist), 0);

    /* Callers (user_pressed, file_pressed, tracker_pressed, …) hand us
	 * widget-relative coordinates straight from the GtkGestureClick. But
	 * gtk_tree_view_get_path_at_pos wants bin-window coordinates — i.e.
	 * relative to the area below the column header, not the widget. With
	 * headers visible (gtk_hlist_new_with_titles flips them on) the two
	 * differ by the header height, which is enough to push every click
	 * one row too far down. Convert before looking up. */
    gtk_tree_view_convert_widget_to_bin_window_coords (GTK_TREE_VIEW (hlist), x,
                                                       y, &bx, &by);

    if (!gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (hlist), bx, by, &path,
                                        &col, NULL, NULL)) {
        return 0;
    }

    if (path) {
        if (row) {
            *row = gtk_tree_path_get_indices (path)[0];
        }
        gtk_tree_path_free (path);
    }
    if (col && column) {
        gint i = 0;
        GList *cols = gtk_tree_view_get_columns (GTK_TREE_VIEW (hlist));
        GList *l;
        for (l = cols; l; l = l->next, i++) {
            if (l->data == col) {
                *column = i;
                break;
            }
        }
        g_list_free (cols);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Sort                                                                */
/* ------------------------------------------------------------------ */

/*
 * The legacy sort callback receives `GtkHListRow *` whose ->cell[i]
 * yields a GtkHellText whose ->text is the cell text, and whose
 * ->data is the row's user data pointer. We materialize that view
 * on the fly: per-call we read the text columns + data column out
 * of the model and stash them in transient ABI-shaped structs.
 */

static gint
sort_iter_compare (GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
                   gpointer user_data)
{
    GtkHList *hlist = GTK_HLIST (user_data);
    GtkHListPrivate *priv = get_priv (hlist);
    GtkHListRow ra, rb;
    GtkHellText *cells_a, *cells_b;
    GtkHellText **cellp_a, **cellp_b;
    gchar **owned_a, **owned_b;
    gint i, n = priv->n_columns;
    gpointer da = NULL, db = NULL;
    gint result;

    if (!priv->compare) {
        return 0;
    }

    cells_a = g_alloca (sizeof (GtkHellText) * n);
    cells_b = g_alloca (sizeof (GtkHellText) * n);
    cellp_a = g_alloca (sizeof (GtkHellText *) * n);
    cellp_b = g_alloca (sizeof (GtkHellText *) * n);
    owned_a = g_alloca (sizeof (gchar *) * n);
    owned_b = g_alloca (sizeof (gchar *) * n);

    for (i = 0; i < n; i++) {
        gtk_tree_model_get (model, a, HLIST_COL_TEXT (i), &owned_a[i], -1);
        gtk_tree_model_get (model, b, HLIST_COL_TEXT (i), &owned_b[i], -1);
        memset (&cells_a[i], 0, sizeof (GtkHellText));
        memset (&cells_b[i], 0, sizeof (GtkHellText));
        cells_a[i].type = GTK_HELL_TEXT;
        cells_b[i].type = GTK_HELL_TEXT;
        cells_a[i].text = owned_a[i] ? owned_a[i] : (gchar *)"";
        cells_b[i].text = owned_b[i] ? owned_b[i] : (gchar *)"";
        cellp_a[i] = &cells_a[i];
        cellp_b[i] = &cells_b[i];
    }
    gtk_tree_model_get (model, a, HLIST_COL_DATA, &da, -1);
    gtk_tree_model_get (model, b, HLIST_COL_DATA, &db, -1);

    memset (&ra, 0, sizeof ra);
    memset (&rb, 0, sizeof rb);
    ra.cell = cellp_a;
    ra.data = da;
    rb.cell = cellp_b;
    rb.data = db;

    result = priv->compare (hlist, &ra, &rb);

    for (i = 0; i < n; i++) {
        g_free (owned_a[i]);
        g_free (owned_b[i]);
    }
    return result;
}

void
gtk_hlist_set_compare_func (GtkHList *hlist, GtkHListCompareFunc cmp)
{
    GtkHListPrivate *priv;
    GtkTreeSortable *sortable;

    g_return_if_fail (GTK_IS_HLIST (hlist));
    priv = get_priv (hlist);
    priv->compare = cmp;
    sortable = GTK_TREE_SORTABLE (priv->store);
    if (cmp) {
        gtk_tree_sortable_set_default_sort_func (sortable, sort_iter_compare,
                                                 hlist, NULL);
    } else {
        gtk_tree_sortable_set_default_sort_func (sortable, NULL, NULL, NULL);
    }
}

void
gtk_hlist_sort (GtkHList *hlist)
{
    GtkHListPrivate *priv;
    GtkTreeSortable *sortable;

    g_return_if_fail (GTK_IS_HLIST (hlist));
    priv = get_priv (hlist);
    sortable = GTK_TREE_SORTABLE (priv->store);
    gtk_tree_sortable_set_sort_column_id (
        sortable, GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID, priv->sort_type);
}

G_GNUC_END_IGNORE_DEPRECATIONS
/* Phase 4.10: end of deprecation suppression — see top of file. */
