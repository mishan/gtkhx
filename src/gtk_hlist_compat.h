/*
 * gtk_hlist_compat.h — drop-in shim that preserves the gtk_hlist_* API
 * surface used by GtkHx consumers, but is implemented over a modern
 * GtkTreeView + GtkListStore.
 *
 * Phase 2.7 (per ROADMAP): the in-tree GtkCList fork (gtk_hlist.[ch]) is
 * ~9k lines of GTK-1.x-private API that does not compile under GTK 2.
 * Rather than port it, we replace it with this shim. Consumers
 * (chat.c, files.c, news15.c, options.c, tracker.c, users.c) use only
 * a narrow subset of the GtkCList API; this header preserves *that*
 * subset and nothing more. Functions on the original public API that
 * had no in-tree consumer are intentionally absent.
 *
 * GTK 2 is the immediate target. The header avoids GTK-3-only types so
 * the shim ports forward without churn. GdkPixmap inputs are converted
 * to GdkPixbuf internally; Phase 2.5 will move callers off pixmaps and
 * make the shim's signatures pixbuf-native.
 *
 * License: GPL-2.0-or-later, matching the rest of GtkHx.
 */

#ifndef __GTK_HLIST_COMPAT_H
#define __GTK_HLIST_COMPAT_H 1

#include <gdk/gdk.h>
#include <gtk/gtk.h>

/* Phase 3.2: GdkPixmap and GdkBitmap were removed in GTK 3. Existing
 * callers still pass these types; alias them to GdkPixbuf so the shim
 * signatures keep their old names while pixbuf-pointers flow through. */
#if GTK_CHECK_VERSION(3, 0, 0)
typedef GdkPixbuf GdkPixmap;
typedef GdkPixbuf GdkBitmap;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Cell types — kept only for API parity with the legacy header. The
 * shim's sort callback always presents text cells; pixmap cells are
 * tracked internally as pixbufs. */
typedef enum {
    GTK_HELL_EMPTY,
    GTK_HELL_TEXT,
    GTK_HELL_PIXMAP,
    GTK_HELL_PIXTEXT,
    GTK_HELL_WIDGET
} GtkHellType;

/* Forward decls. */
typedef struct _GtkHList GtkHList;
typedef struct _GtkHListClass GtkHListClass;
typedef struct _GtkHellText GtkHellText;
typedef struct _GtkHListRow GtkHListRow;

/* Sort comparator. The shim populates a transient GtkHListRow per
 * row that exposes ->data and ->cell[]; cell[i] is a GtkHellText*
 * whose ->text member is the cell's display text. That is the only
 * subset of the legacy struct any in-tree comparator inspects. */
typedef gint (*GtkHListCompareFunc) (GtkHList *hlist, gconstpointer ptr1,
                                     gconstpointer ptr2);

/* ABI-shim versions of the cell/row structs. Layouts match the
 * legacy header at the offsets actually accessed by consumers
 * (->data on the row, ->text on a cell after GTK_HELL_TEXT cast).
 * Other fields are preserved positionally so the casts compile but
 * are never populated by the shim. */
struct _GtkHellText {
    GtkHellType type;
    gint16 vertical;
    gint16 horizontal;
    gpointer style; /* unused — was GtkStyle * */
    gchar *text;
};

struct _GtkHListRow {
    GtkHellText **cell; /* cell[col] — text only; pixmaps NULL */
    int state;          /* Phase 4.2: was GtkStateType (gone in GTK 4) */
    GdkRGBA foreground; /* Phase 3.10 */
    GdkRGBA background; /* Phase 3.10 */
    gpointer style;     /* unused — was GtkStyle * */
    gpointer data;
    GDestroyNotify destroy; /* unused by consumers */
    guint fg_set : 1;
    guint bg_set : 1;
    guint selectable : 1;
    guint draw_me : 1;
};

/* Cast macros — preserved so existing GTK_HLIST(w) and GTK_HELL_TEXT(c)
 * call sites compile unchanged. */
#define GTK_TYPE_HLIST (gtk_hlist_get_type ())
#define GTK_HLIST(obj)                                                         \
    (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_HLIST, GtkHList))
#define GTK_HLIST_CLASS(klass)                                                 \
    (G_TYPE_CHECK_CLASS_CAST ((klass), GTK_TYPE_HLIST, GtkHListClass))
#define GTK_IS_HLIST(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GTK_TYPE_HLIST))
#define GTK_IS_HLIST_CLASS(klass)                                              \
    (G_TYPE_CHECK_CLASS_TYPE ((klass), GTK_TYPE_HLIST))

#define GTK_HELL_TEXT(cell) (((GtkHellText *)&(cell)))

/* GtkHList instance struct — only `rows` is exposed publicly because
 * options.c reads GTK_HLIST(w)->rows directly. The rest of the state
 * lives in a private struct hung off the GObject. */
struct _GtkHList {
    GtkTreeView parent_instance;
    gint rows; /* refreshed on every model mutation */
               /* private state in g_object_get_data(self, "hlist-priv") */
};

struct _GtkHListClass {
    GtkTreeViewClass parent_class;
    void (*click_column) (GtkHList *hlist, gint column);
    void (*select_row) (GtkHList *hlist, gint row, gint column,
                        GdkEvent *event);
};

GType gtk_hlist_get_type (void) G_GNUC_CONST;

GtkWidget *gtk_hlist_new (gint columns);
GtkWidget *gtk_hlist_new_with_titles (gint columns, gchar *titles[]);

/* Phase 4.2: was GtkShadowType — gone in GTK 4. The implementation
 * is a no-op anyway (the framing belongs to the enclosing
 * GtkScrolledWindow), so the parameter is just `int' now and the
 * GTK_SHADOW_* compat values below let call sites stay unchanged. */
void gtk_hlist_set_shadow_type (GtkHList *hlist, int type);

/* Phase 4.2: GTK_SHADOW_* enum is gone in GTK 4. The values are
 * never read (set_shadow_type is a no-op) — the defines exist so
 * gtk_hlist_set_shadow_type(GTK_HLIST(w), GTK_SHADOW_NONE) compiles
 * unchanged. */
#ifndef GTK_SHADOW_NONE
typedef enum {
    GTK_SHADOW_NONE = 0,
    GTK_SHADOW_IN = 1,
    GTK_SHADOW_OUT = 2,
    GTK_SHADOW_ETCHED_IN = 3,
    GTK_SHADOW_ETCHED_OUT = 4
} GtkShadowType;
#define GTK_SHADOW_NONE GTK_SHADOW_NONE
#endif
void gtk_hlist_set_selection_mode (GtkHList *hlist, GtkSelectionMode mode);

void gtk_hlist_freeze (GtkHList *hlist);
void gtk_hlist_thaw (GtkHList *hlist);

void gtk_hlist_set_column_width (GtkHList *hlist, gint column, gint width);
void gtk_hlist_set_column_justification (GtkHList *hlist, gint column,
                                         GtkJustification justification);

void gtk_hlist_set_row_height (GtkHList *hlist, guint height);

void gtk_hlist_moveto (GtkHList *hlist, gint row, gint column, gfloat row_align,
                       gfloat col_align);

void gtk_hlist_set_text (GtkHList *hlist, gint row, gint column,
                         const gchar *text);

void gtk_hlist_set_pixtext (GtkHList *hlist, gint row, gint column,
                            const gchar *text, guint8 spacing,
                            GdkPixmap *pixmap, GdkBitmap *mask);

void gtk_hlist_set_foreground (GtkHList *hlist, gint row, GdkRGBA *color);

/* Phase 5: replace a column's default (pack_start pixbuf + pack_start
 * text) renderers with a single custom renderer that draws the
 * pixbuf at the column's left edge at its NATURAL width and draws
 * the text on top at a fixed `text_x_offset` pixels from the cell
 * edge. The pixbuf serves as a row background that text overlays —
 * the Mac-classic Hotline user-list layout where wide banner-style
 * icons sit behind the user's name without pushing it to the right.
 *
 * Without this, the stock GtkTreeView layout sizes the pixbuf
 * renderer to its natural width and the text renderer starts after
 * it, so wider icons offset the names. Hotline icon-art sets stored
 * in icons.rsrc commonly include 100+-px-wide entries, especially
 * on community / fan-art servers (Badmoon, etc.); without overlay
 * rendering, those servers render with names spilling off the right
 * edge of the user-list column. */
void gtk_hlist_column_set_overlay_pixtext (GtkHList *hlist, gint column,
                                           gint text_x_offset);

/* Tune the overlay cell installed by gtk_hlist_column_set_overlay_pixtext.
 *
 *   pixel_scale > 1.0  scales both the pixbuf rendering and the
 *                      text font on this column. Useful for making
 *                      a single table read larger without affecting
 *                      others. 1.0 means native (no scaling).
 *
 *   text_outline       when TRUE, the renderer draws a 1 px
 *                      contrasting outline (black for light fg,
 *                      white for dark fg) behind the text before
 *                      drawing the foreground colour on top. Helps
 *                      readability of light user names rendered on
 *                      busy banner-icon backgrounds.
 *
 * No-op when the column has not had an overlay cell installed yet —
 * gtk_hlist_column_set_overlay_pixtext must be called first. */
void gtk_hlist_column_set_overlay_decoration (GtkHList *hlist, gint column,
                                              gdouble pixel_scale,
                                              gboolean text_outline);

gint gtk_hlist_append (GtkHList *hlist, gchar *text[]);
gint gtk_hlist_insert (GtkHList *hlist, gint row, gchar *text[]);
void gtk_hlist_remove (GtkHList *hlist, gint row);

void gtk_hlist_set_row_data (GtkHList *hlist, gint row, gpointer data);
gpointer gtk_hlist_get_row_data (GtkHList *hlist, gint row);
gint gtk_hlist_find_row_from_data (GtkHList *hlist, gpointer data);

void gtk_hlist_select_row (GtkHList *hlist, gint row, gint column);

void gtk_hlist_clear (GtkHList *hlist);

gint gtk_hlist_get_selection_info (GtkHList *hlist, gint x, gint y, gint *row,
                                   gint *column);

void gtk_hlist_set_compare_func (GtkHList *hlist, GtkHListCompareFunc cmp);
void gtk_hlist_sort (GtkHList *hlist);

#ifdef __cplusplus
}
#endif

#endif /* __GTK_HLIST_COMPAT_H */
