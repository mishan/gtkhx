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

#include <string.h>

/* Storage column offsets: see header comment above. */
#define HLIST_COL_DATA          0
#define HLIST_COL_FG            1
#define HLIST_COL_FG_SET        2
#define HLIST_COL_TEXT(i)       (3 + (i))
#define HLIST_COL_PIXBUF(N, i)  (3 + (N) + (i))
#define HLIST_TOTAL_COLS(N)     (3 + 2 * (N))

typedef struct _GtkHListPrivate GtkHListPrivate;

struct _GtkHListPrivate {
	GtkListStore        *store;
	gint                 n_columns;
	gint                 freeze_count;
	GtkHListCompareFunc  compare;
	gint                 sort_column;
	GtkSortType          sort_type;
};

static GtkHListPrivate *
get_priv (GtkHList *hlist)
{
	return (GtkHListPrivate *) g_object_get_data (G_OBJECT (hlist),
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
		if (priv->store)
			g_object_unref (priv->store);
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
		"click_column",
		G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (GtkHListClass, click_column),
		NULL, NULL,
		g_cclosure_marshal_VOID__INT,
		G_TYPE_NONE, 1, G_TYPE_INT);

	/* select_row mirrors the legacy GtkCList "select_row" signature
	 * (row, column, GdkEvent*). The compat shim emits this from a
	 * GtkTreeSelection "changed" handler. */
	select_row_signal = g_signal_new (
		"select_row",
		G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (GtkHListClass, select_row),
		NULL, NULL,
		NULL,                            /* generic marshaller */
		G_TYPE_NONE, 3,
		G_TYPE_INT, G_TYPE_INT, GDK_TYPE_EVENT);
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

	if (row < 0)
		return FALSE;
	path = gtk_tree_path_new_from_indices (row, -1);
	ok = gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), iter, path);
	gtk_tree_path_free (path);
	return ok;
}

static void
refresh_row_count (GtkHList *hlist)
{
	GtkHListPrivate *priv = get_priv (hlist);
	hlist->rows = priv ?
		gtk_tree_model_iter_n_children (GTK_TREE_MODEL (priv->store), NULL)
		: 0;
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
	if (!pixmap)
		return NULL;
	return g_object_ref ((GdkPixbuf *)pixmap);
}

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

static void
on_column_clicked (GtkTreeViewColumn *col, gpointer user_data)
{
	GtkHList *hlist = GTK_HLIST (user_data);
	gint idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (col),
	                                               "hlist-col-idx"));
	g_signal_emit (hlist, click_column_signal, 0, idx);
}

static void
on_selection_changed (GtkTreeSelection *sel, gpointer user_data)
{
	GtkHList    *hlist = GTK_HLIST (user_data);
	GtkTreeModel *model;
	GtkTreeIter  iter;

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
	GtkHList         *self;
	GtkHListPrivate  *priv;
	GType            *types;
	GtkTreeSelection *sel;
	gint              i;

	g_return_val_if_fail (n_columns > 0, NULL);

	self = g_object_new (GTK_TYPE_HLIST, NULL);
	priv = g_new0 (GtkHListPrivate, 1);
	priv->n_columns   = n_columns;
	priv->sort_column = 0;
	priv->sort_type   = GTK_SORT_ASCENDING;
	g_object_set_data (G_OBJECT (self), "hlist-priv", priv);

	types = g_new0 (GType, HLIST_TOTAL_COLS (n_columns));
	types[HLIST_COL_DATA]   = G_TYPE_POINTER;
	types[HLIST_COL_FG]     = G_TYPE_STRING;
	types[HLIST_COL_FG_SET] = G_TYPE_BOOLEAN;
	for (i = 0; i < n_columns; i++) {
		types[HLIST_COL_TEXT (i)]              = G_TYPE_STRING;
		types[HLIST_COL_PIXBUF (n_columns, i)] = GDK_TYPE_PIXBUF;
	}
	priv->store = gtk_list_store_newv (HLIST_TOTAL_COLS (n_columns), types);
	g_free (types);

	gtk_tree_view_set_model (GTK_TREE_VIEW (self),
	                         GTK_TREE_MODEL (priv->store));

	for (i = 0; i < n_columns; i++) {
		GtkTreeViewColumn *col;
		GtkCellRenderer   *pixr;
		GtkCellRenderer   *txtr;
		const gchar       *title = (titles && titles[i]) ? titles[i] : "";

		col = gtk_tree_view_column_new ();
		gtk_tree_view_column_set_title (col, title);
		gtk_tree_view_column_set_resizable (col, TRUE);
		gtk_tree_view_column_set_sizing (col, GTK_TREE_VIEW_COLUMN_FIXED);
		gtk_tree_view_column_set_clickable (col, TRUE);
		g_object_set_data (G_OBJECT (col), "hlist-col-idx",
		                   GINT_TO_POINTER (i));
		g_signal_connect (col, "clicked",
		                  G_CALLBACK (on_column_clicked), self);

		pixr = gtk_cell_renderer_pixbuf_new ();
		gtk_tree_view_column_pack_start (col, pixr, FALSE);
		gtk_tree_view_column_set_attributes (col, pixr,
			"pixbuf", HLIST_COL_PIXBUF (n_columns, i),
			NULL);

		txtr = gtk_cell_renderer_text_new ();
		gtk_tree_view_column_pack_start (col, txtr, TRUE);
		gtk_tree_view_column_set_attributes (col, txtr,
			"text",           HLIST_COL_TEXT (i),
			"foreground",     HLIST_COL_FG,
			"foreground-set", HLIST_COL_FG_SET,
			NULL);

		gtk_tree_view_append_column (GTK_TREE_VIEW (self), col);
	}

	gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (self),
	                                   titles != NULL);

	sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
	gtk_tree_selection_set_mode (sel, GTK_SELECTION_SINGLE);
	g_signal_connect (sel, "changed",
	                  G_CALLBACK (on_selection_changed), self);

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
gtk_hlist_set_shadow_type (GtkHList *hlist, GtkShadowType type)
{
	(void) hlist; (void) type;
	/* The legacy widget drew its own shadow; GtkTreeView delegates
	 * framing to its enclosing GtkScrolledWindow. The consumers
	 * always wrap us in one, so this is a no-op. */
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
		gtk_tree_view_column_set_sizing (col,
		                                 GTK_TREE_VIEW_COLUMN_FIXED);
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
	if (!col) return;

	switch (justification) {
	case GTK_JUSTIFY_LEFT:   align = 0.0; break;
	case GTK_JUSTIFY_CENTER: align = 0.5; break;
	case GTK_JUSTIFY_RIGHT:  align = 1.0; break;
	default:                 align = 0.0; break;
	}
	gtk_tree_view_column_set_alignment (col, align);
	cells = gtk_cell_layout_get_cells (GTK_CELL_LAYOUT (col));
	for (l = cells; l; l = l->next)
		g_object_set (l->data, "xalign", align, NULL);
	g_list_free (cells);
}

void
gtk_hlist_set_row_height (GtkHList *hlist, guint height)
{
	(void) hlist; (void) height;
	/* GtkTreeView sizes rows to content; a fixed minimum can be
	 * imposed by setting "height" on every cell renderer, but no
	 * in-tree consumer relies on the precise pixel value. Treat
	 * this as a hint we ignore. */
}

void
gtk_hlist_moveto (GtkHList *hlist, gint row, gint column,
                  gfloat row_align, gfloat col_align)
{
	GtkTreePath *path;
	GtkTreeViewColumn *col = NULL;

	g_return_if_fail (GTK_IS_HLIST (hlist));
	if (row < 0)
		return;
	path = gtk_tree_path_new_from_indices (row, -1);
	if (column >= 0)
		col = gtk_tree_view_get_column (GTK_TREE_VIEW (hlist), column);
	gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (hlist),
	                              path, col,
	                              TRUE, row_align, col_align);
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
	if (priv->freeze_count++ == 0)
		gtk_tree_view_set_model (GTK_TREE_VIEW (hlist), NULL);
}

void
gtk_hlist_thaw (GtkHList *hlist)
{
	GtkHListPrivate *priv;
	g_return_if_fail (GTK_IS_HLIST (hlist));
	priv = get_priv (hlist);
	if (priv->freeze_count > 0 && --priv->freeze_count == 0)
		gtk_tree_view_set_model (GTK_TREE_VIEW (hlist),
		                         GTK_TREE_MODEL (priv->store));
}

/* ------------------------------------------------------------------ */
/* Cell content                                                        */
/* ------------------------------------------------------------------ */

void
gtk_hlist_set_text (GtkHList *hlist, gint row, gint column,
                    const gchar *text)
{
	GtkHListPrivate *priv;
	GtkTreeIter iter;

	g_return_if_fail (GTK_IS_HLIST (hlist));
	priv = get_priv (hlist);
	if (column < 0 || column >= priv->n_columns)
		return;
	if (!iter_at_row (priv, row, &iter))
		return;
	gtk_list_store_set (priv->store, &iter,
	                    HLIST_COL_TEXT (column), text ? text : "",
	                    -1);
}

void
gtk_hlist_set_pixtext (GtkHList *hlist, gint row, gint column,
                       const gchar *text, guint8 spacing,
                       GdkPixmap *pixmap, GdkBitmap *mask)
{
	GtkHListPrivate *priv;
	GtkTreeIter iter;
	GdkPixbuf *pb;

	g_return_if_fail (GTK_IS_HLIST (hlist));
	(void) spacing;
	priv = get_priv (hlist);
	if (column < 0 || column >= priv->n_columns)
		return;
	if (!iter_at_row (priv, row, &iter))
		return;
	pb = pixmap_to_pixbuf (pixmap, mask);
	gtk_list_store_set (priv->store, &iter,
	                    HLIST_COL_TEXT   (column), text ? text : "",
	                    HLIST_COL_PIXBUF (priv->n_columns, column), pb,
	                    -1);
	if (pb)
		g_object_unref (pb);
}

void
gtk_hlist_set_foreground (GtkHList *hlist, gint row, GdkRGBA *color)
{
	GtkHListPrivate *priv;
	GtkTreeIter iter;
	gchar buf[16];

	g_return_if_fail (GTK_IS_HLIST (hlist));
	priv = get_priv (hlist);
	if (!iter_at_row (priv, row, &iter))
		return;
	if (color) {
		/* Phase 3.10: GdkRGBA channels are doubles 0..1 — scale to
		 * 16-bit to keep emitting the same #RRRRGGGGBBBB hex string
		 * the GtkCellRendererText 'foreground' attribute expects. */
		g_snprintf (buf, sizeof buf, "#%04x%04x%04x",
		            (unsigned) (color->red   * 65535),
		            (unsigned) (color->green * 65535),
		            (unsigned) (color->blue  * 65535));
		gtk_list_store_set (priv->store, &iter,
		                    HLIST_COL_FG,     buf,
		                    HLIST_COL_FG_SET, TRUE,
		                    -1);
	} else {
		gtk_list_store_set (priv->store, &iter,
		                    HLIST_COL_FG,     NULL,
		                    HLIST_COL_FG_SET, FALSE,
		                    -1);
	}
}

/* ------------------------------------------------------------------ */
/* Row append / insert / remove / clear                                */
/* ------------------------------------------------------------------ */

static void
fill_row_text (GtkHListPrivate *priv, GtkTreeIter *iter, gchar **text)
{
	gint i;
	if (!text)
		return;
	for (i = 0; i < priv->n_columns; i++) {
		gtk_list_store_set (priv->store, iter,
		                    HLIST_COL_TEXT (i),
		                    text[i] ? text[i] : "",
		                    -1);
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
	row = gtk_tree_model_iter_n_children (GTK_TREE_MODEL (priv->store),
	                                      NULL) - 1;
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
	n = gtk_tree_model_iter_n_children (GTK_TREE_MODEL (priv->store),
	                                    NULL);
	if (row < 0 || row > n)
		row = n;
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
	if (!iter_at_row (priv, row, &iter))
		return;
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
	if (!iter_at_row (priv, row, &iter))
		return;
	gtk_list_store_set (priv->store, &iter,
	                    HLIST_COL_DATA, data,
	                    -1);
}

gpointer
gtk_hlist_get_row_data (GtkHList *hlist, gint row)
{
	GtkHListPrivate *priv;
	GtkTreeIter iter;
	gpointer data = NULL;

	g_return_val_if_fail (GTK_IS_HLIST (hlist), NULL);
	priv = get_priv (hlist);
	if (!iter_at_row (priv, row, &iter))
		return NULL;
	gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter,
	                    HLIST_COL_DATA, &data,
	                    -1);
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
		gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter,
		                    HLIST_COL_DATA, &cur, -1);
		if (cur == data)
			return row;
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
	(void) column;
	priv = get_priv (hlist);
	if (!iter_at_row (priv, row, &iter))
		return;
	sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (hlist));
	gtk_tree_selection_select_iter (sel, &iter);
}

gint
gtk_hlist_get_selection_info (GtkHList *hlist, gint x, gint y,
                              gint *row, gint *column)
{
	GtkTreePath       *path = NULL;
	GtkTreeViewColumn *col  = NULL;

	if (row)    *row    = -1;
	if (column) *column = -1;

	g_return_val_if_fail (GTK_IS_HLIST (hlist), 0);

	if (!gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (hlist),
	                                    x, y, &path, &col, NULL, NULL))
		return 0;

	if (path) {
		if (row)
			*row = gtk_tree_path_get_indices (path)[0];
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
sort_iter_compare (GtkTreeModel *model,
                   GtkTreeIter  *a,
                   GtkTreeIter  *b,
                   gpointer      user_data)
{
	GtkHList        *hlist = GTK_HLIST (user_data);
	GtkHListPrivate *priv  = get_priv (hlist);
	GtkHListRow      ra, rb;
	GtkHellText     *cells_a, *cells_b;
	GtkHellText    **cellp_a, **cellp_b;
	gchar          **owned_a, **owned_b;
	gint i, n = priv->n_columns;
	gpointer da = NULL, db = NULL;
	gint result;

	if (!priv->compare)
		return 0;

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
		cells_a[i].text = owned_a[i] ? owned_a[i] : (gchar *) "";
		cells_b[i].text = owned_b[i] ? owned_b[i] : (gchar *) "";
		cellp_a[i] = &cells_a[i];
		cellp_b[i] = &cells_b[i];
	}
	gtk_tree_model_get (model, a, HLIST_COL_DATA, &da, -1);
	gtk_tree_model_get (model, b, HLIST_COL_DATA, &db, -1);

	memset (&ra, 0, sizeof ra);
	memset (&rb, 0, sizeof rb);
	ra.cell = cellp_a; ra.data = da;
	rb.cell = cellp_b; rb.data = db;

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
		gtk_tree_sortable_set_default_sort_func (sortable,
		                                         sort_iter_compare,
		                                         hlist, NULL);
	} else {
		gtk_tree_sortable_set_default_sort_func (sortable, NULL,
		                                         NULL, NULL);
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
	gtk_tree_sortable_set_sort_column_id (sortable,
		GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID,
		priv->sort_type);
}
