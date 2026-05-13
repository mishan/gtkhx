/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * news_browser.c — unified 1.5 threaded-news browser. See
 * news_browser.h for the design overview.
 *
 * Phase 1: window + 2-pane layout, empty tree.
 * Phase 2: root NEWSDIRLIST on open populates the top level
 *          (folders + categories).
 *
 * Built on the modern GTK 4 list stack:
 *
 *   HxNewsNode     — a GObject per tree row (folder / category / post)
 *   GListStore<HxNewsNode> — root + each node's children
 *   GtkTreeListModel — wraps the GListStore tree into a flat
 *                      virtual list with parent/depth info
 *   GtkSingleSelection — selection model over the tree-list
 *   GtkListView    — the view, with a GtkSignalListItemFactory
 *                    placing a GtkTreeExpander on each row
 *
 * Coexists with the legacy gnews_folder / gnews_catalog windows in
 * news15.c — both compile in. The toolbar's News (1.5+) button is
 * rerouted here. The signal-intercept hooks (gnews_browser_handle_
 * dirlist / _catlist) let the existing gtkhx.c on_news_*_signal
 * adapters route replies to the browser when the carrier
 * gnews_folder / gnews_catalog stub came from us; otherwise the
 * signal falls through to the legacy output_news_folder /
 * output_news_catalog path.
 */

#include "config.h"

#include <gtk/gtk.h>
#include <adwaita.h>

#include "hx.h"
#include "session.h"
#include "news15.h"
#include "news_browser.h"
#include "gtkutil.h"

/* ---------- HxNewsNode (one GObject per tree row) ---------- */

enum {
	NB_KIND_FOLDER   = 1,
	NB_KIND_CATEGORY = 2,
	NB_KIND_POST     = 3,
};

#define HX_TYPE_NEWS_NODE (hx_news_node_get_type ())
G_DECLARE_FINAL_TYPE (HxNewsNode, hx_news_node, HX, NEWS_NODE, GObject)

struct _HxNewsNode {
	GObject     parent_instance;
	int         kind;
	char       *name;
	char       *path;        /* full Hotline path (folders / categories);
	                          * NULL for posts */
	GListStore *children;    /* created lazily on first expansion */
	gboolean    loaded;      /* TRUE once the RPC reply has populated
	                          * children — guards against re-fetch on
	                          * collapse + re-expand */
};

G_DEFINE_FINAL_TYPE (HxNewsNode, hx_news_node, G_TYPE_OBJECT)

static void
hx_news_node_finalize (GObject *obj)
{
	HxNewsNode *n = HX_NEWS_NODE (obj);
	g_free (n->name);
	g_free (n->path);
	g_clear_object (&n->children);
	G_OBJECT_CLASS (hx_news_node_parent_class)->finalize (obj);
}

static void
hx_news_node_class_init (HxNewsNodeClass *klass)
{
	G_OBJECT_CLASS (klass)->finalize = hx_news_node_finalize;
}

static void
hx_news_node_init (HxNewsNode *self)
{
	(void) self;
}

static HxNewsNode *
hx_news_node_new (int kind, const char *name, const char *path)
{
	HxNewsNode *n = g_object_new (HX_TYPE_NEWS_NODE, NULL);
	n->kind = kind;
	n->name = g_strdup (name ? name : "");
	n->path = path ? g_strdup (path) : NULL;
	return n;
}

/* ---------- Browser ---------- */

struct _gnews_browser {
	GtkWidget    *window;

	/* Tree side */
	GListStore   *root_store;     /* top-level nodes */
	GtkTreeListModel *tree_model; /* wraps root_store + children */
	GtkSingleSelection *selection;
	GtkWidget    *list_view;

	/* Cached row icons. GdkTextures wrapped around pixbufs loaded
	 * from the XPM resources at window-construction time. One ref
	 * each, dropped on close. The factory bind callback hands the
	 * paintable to a GtkImage per row — much cheaper than re-loading
	 * the resource for every visible row, and avoids the XPM-via-
	 * gtk_image_set_from_resource path which renders blank for us
	 * (the same reason gtkhx_pixmap_button takes the long way round). */
	GdkPaintable *icon_folder;
	GdkPaintable *icon_category;
	GdkPaintable *icon_post;

	/* Content side */
	GtkWidget    *post_view;      /* GtkTextView, read-only */
	GtkLabel     *breadcrumb;
};

/* Single open browser. */
static gnews_browser *the_browser = NULL;

/* In-flight fetches. Keys are the throwaway gnews_folder / gnews_catalog
 * stubs we hand to hx_news15_fldr_list / hx_news15_cat_list; values are
 * (reffed) HxNewsNode* whose children store should receive the parsed
 * entries. A NULL value means "this is a root fetch — populate the
 * root_store instead".
 *
 * These tables persist across browser opens. If the browser closes
 * while a fetch is in flight, the reply still arrives here; we free
 * the stub + drop the data on the floor (the_browser is NULL by then,
 * and the HxNewsNode ref keeps the target alive long enough for the
 * lookup to succeed even though the tree it belongs to is gone). */
static GHashTable *pending_dirlists = NULL;   /* stub → HxNewsNode* or NULL */
static GHashTable *pending_catlists = NULL;   /* stub → HxNewsNode* or NULL */

static void
ensure_pending_tables (void)
{
	if (!pending_dirlists)
		pending_dirlists = g_hash_table_new_full (
			g_direct_hash, g_direct_equal,
			NULL, g_object_unref);
	if (!pending_catlists)
		pending_catlists = g_hash_table_new_full (
			g_direct_hash, g_direct_equal,
			NULL, g_object_unref);
}

/* ---------- create_child_model: builds the tree's child stores --------
 *
 * Called by GtkTreeListModel for each row to determine whether it's
 * a leaf or expandable. Returning NULL marks the row as a leaf.
 * Returning a GListModel (even empty) marks it as expandable; the
 * tree expander will appear next to the row.
 *
 * Phase 2: every node is a leaf. Phase 3 will return a GListStore
 * for folder / category nodes and wire lazy fetch on expand. */
static GListModel *
news_node_create_child_model (gpointer item, gpointer user_data)
{
	(void) item; (void) user_data;
	return NULL;
}

/* ---------- Icon helpers ---------- */

/* Load one XPM resource and return it as a GdkPaintable (a
 * GdkTexture wrapping a GdkPixbuf). Returns NULL silently on a
 * missing resource — callers null-check.
 *
 * Goes pixbuf → texture rather than calling gtk_image_set_from_
 * resource: the latter renders blank for our XPMs (probably an
 * Adwaita CSS sizing interaction). The pixbuf path is the same
 * one gtkhx_pixmap_button uses for the toolbar icons. */
static GdkPaintable *
load_icon_paintable (const char *resource)
{
	GdkPixbuf  *pb;
	GdkTexture *tex;

	pb = gdk_pixbuf_new_from_resource (resource, NULL);
	if (!pb)
		return NULL;
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	tex = gdk_texture_new_for_pixbuf (pb);
	G_GNUC_END_IGNORE_DEPRECATIONS
	g_object_unref (pb);
	return GDK_PAINTABLE (tex);
}

static GdkPaintable *
icon_paintable_for_kind (gnews_browser *br, int kind)
{
	switch (kind) {
	case NB_KIND_FOLDER:   return br->icon_folder;
	case NB_KIND_CATEGORY: return br->icon_category;
	case NB_KIND_POST:     return br->icon_post;
	default:               return NULL;
	}
}

/* ---------- Selection → breadcrumb ---------- */

static void
update_breadcrumb (gnews_browser *br)
{
	guint pos = gtk_single_selection_get_selected (br->selection);
	GtkTreeListRow *row;
	GString *crumb;

	if (pos == GTK_INVALID_LIST_POSITION) {
		gtk_label_set_text (br->breadcrumb, "/");
		return;
	}

	row = g_list_model_get_item (G_LIST_MODEL (br->tree_model), pos);
	if (!row) {
		gtk_label_set_text (br->breadcrumb, "/");
		return;
	}

	/* Walk up the tree collecting names. */
	{
		GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
		GtkTreeListRow *cur = g_object_ref (row);
		while (cur) {
			HxNewsNode *node = gtk_tree_list_row_get_item (cur);
			if (node) {
				g_ptr_array_insert (names, 0, g_strdup (node->name));
				g_object_unref (node);
			}
			GtkTreeListRow *parent = gtk_tree_list_row_get_parent (cur);
			g_object_unref (cur);
			cur = parent;
		}

		crumb = g_string_new ("/");
		for (guint i = 0; i < names->len; i++) {
			if (i > 0)
				g_string_append_c (crumb, '/');
			g_string_append (crumb, (const char *) names->pdata[i]);
		}
		g_ptr_array_free (names, TRUE);
	}

	gtk_label_set_text (br->breadcrumb, crumb->str);
	g_string_free (crumb, TRUE);
	g_object_unref (row);
}

static void
on_selection_changed (GtkSingleSelection *sel,
                       guint position, guint n_items,
                       gpointer user_data)
{
	(void) sel; (void) position; (void) n_items;
	update_breadcrumb ((gnews_browser *) user_data);
}

/* ---------- Factory: setup + bind for each row widget ---------- */

static void
on_factory_setup (GtkSignalListItemFactory *factory,
                  GtkListItem *list_item, gpointer user_data)
{
	(void) factory; (void) user_data;

	GtkWidget *box      = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *icon     = gtk_image_new ();
	GtkWidget *label    = gtk_label_new (NULL);
	GtkWidget *expander = gtk_tree_expander_new ();

	gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
	gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
	gtk_box_append (GTK_BOX (box), icon);
	gtk_box_append (GTK_BOX (box), label);
	gtk_tree_expander_set_child (GTK_TREE_EXPANDER (expander), box);

	gtk_list_item_set_child (list_item, expander);

	/* Stash refs on the expander itself so bind() can find them
	 * without juggling another struct. */
	g_object_set_data (G_OBJECT (expander), "icon",  icon);
	g_object_set_data (G_OBJECT (expander), "label", label);
}

static void
on_factory_bind (GtkSignalListItemFactory *factory,
                 GtkListItem *list_item, gpointer user_data)
{
	gnews_browser   *br       = user_data;
	(void) factory;

	GtkWidget       *expander = gtk_list_item_get_child (list_item);
	GtkTreeListRow  *row      = gtk_list_item_get_item (list_item);
	HxNewsNode      *node     = row ? gtk_tree_list_row_get_item (row) : NULL;
	GtkImage        *icon     = g_object_get_data (G_OBJECT (expander), "icon");
	GtkLabel        *label    = g_object_get_data (G_OBJECT (expander), "label");

	gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), row);

	if (!node) {
		gtk_image_clear (icon);
		gtk_label_set_text (label, "");
		return;
	}

	GdkPaintable *paintable = icon_paintable_for_kind (br, node->kind);
	if (paintable)
		gtk_image_set_from_paintable (icon, paintable);
	else
		gtk_image_clear (icon);

	gtk_label_set_text (label, node->name ? node->name : "");

	g_object_unref (node);   /* gtk_tree_list_row_get_item returned a ref */
}

/* ---------- RPC dispatch ---------- */

/* Fire HTLC_HDR_NEWSDIRLIST. `target` is the HxNewsNode whose
 * `children` store should be populated; NULL means a root fetch
 * (populate the browser's root_store instead). */
static void
fetch_dirlist (gnews_browser *br, HxNewsNode *target)
{
	struct gnews_folder *stub;
	(void) br;

	ensure_pending_tables ();

	stub = g_malloc0 (sizeof (struct gnews_folder));
	/* path_to_hldir (called inside hx_news15_fldr_list) walks the
	 * string and dereferences it unconditionally — NULL crashes.
	 * The legacy create_gfnews_window uses "/" for the root case;
	 * mirror that. */
	stub->path = target && target->path
		? g_strdup (target->path)
		: g_strdup ("/");

	g_hash_table_insert (pending_dirlists, stub,
	                     target ? g_object_ref (target) : NULL);

	hx_news15_fldr_list (&the_session.htlc, stub);
}

/* ---------- Reply handlers (called from gtkhx.c signal adapters) ---------- */

gboolean
gnews_browser_handle_dirlist (gpointer gfnews_p)
{
	struct gnews_folder *gfnews = gfnews_p;
	HxNewsNode *target = NULL;
	GListStore *dest = NULL;
	gnews_browser *br = the_browser;
	guint32 i;

	if (!pending_dirlists)
		return FALSE;
	if (!g_hash_table_contains (pending_dirlists, gfnews))
		return FALSE;

	/* Steal the target ref so the hashtable destroy notify doesn't
	 * unref it before we're done. */
	target = g_hash_table_lookup (pending_dirlists, gfnews);
	if (target)
		g_object_ref (target);
	g_hash_table_remove (pending_dirlists, gfnews);

	/* Browser still alive: pick the right destination store. */
	if (br) {
		if (!target)
			dest = br->root_store;
		else if (target->children)
			dest = target->children;
	}

	if (dest && gfnews->news) {
		struct news_folder *folder = gfnews->news;
		for (i = 0; i < folder->num_entries; i++) {
			struct folder_item *item = folder->entry[i];
			int kind = (item->type == 1) ? NB_KIND_FOLDER
			                              : NB_KIND_CATEGORY;
			HxNewsNode *node = hx_news_node_new (kind, item->name, NULL);
			g_list_store_append (dest, node);
			g_object_unref (node);
		}
		if (target)
			target->loaded = TRUE;
	}

	/* Free the stub + its parsed news_folder. */
	if (gfnews->news) {
		guint32 j;
		for (j = 0; j < gfnews->news->num_entries; j++) {
			g_free (gfnews->news->entry[j]->name);
			g_free (gfnews->news->entry[j]);
		}
		g_free (gfnews->news->entry);
		g_free (gfnews->news);
	}
	g_free (gfnews->path);
	g_free (gfnews);

	g_clear_object (&target);
	return TRUE;
}

gboolean
gnews_browser_handle_catlist (gpointer gcnews_p)
{
	/* Phase 3 wires this up. */
	(void) gcnews_p;
	return FALSE;
}

/* ---------- Window lifecycle ---------- */

static gboolean
on_window_close (GtkWindow *window, gpointer user_data)
{
	gnews_browser *br = user_data;
	(void) window;

	if (the_browser == br)
		the_browser = NULL;

	g_clear_object (&br->icon_folder);
	g_clear_object (&br->icon_category);
	g_clear_object (&br->icon_post);

	/* root_store / tree_model / selection / list_view are widget-
	 * owned and get freed when the GtkWindow tears down its child
	 * tree. We only own the gnews_browser struct + the icon
	 * paintables. */
	g_free (br);
	return FALSE;
}

/* ---------- Window construction ---------- */

static gnews_browser *
build_browser_window (void)
{
	gnews_browser *br = g_new0 (gnews_browser, 1);
	GtkWidget *header, *paned, *left_scroll, *right_box, *right_scroll;
	GtkWidget *header_title;
	GtkListItemFactory *factory;
	GtkTextBuffer *buf;

	/* ---- Icons (cached for the lifetime of the window) ---- */
	br->icon_folder   = load_icon_paintable ("/com/nasledov/gtkhx/pixmaps/newsfld.xpm");
	br->icon_category = load_icon_paintable ("/com/nasledov/gtkhx/pixmaps/newscat.xpm");
	br->icon_post     = load_icon_paintable ("/com/nasledov/gtkhx/pixmaps/postnews.xpm");

	/* ---- Window ---- */
	br->window = gtk_window_new ();
	gtk_window_set_title (GTK_WINDOW (br->window), _("Threaded News"));
	gtk_widget_set_size_request (br->window, 720, 480);

	/* ---- AdwHeaderBar with breadcrumb as the title widget ----
	 *
	 * Phase 5 will pack context-sensitive action buttons here
	 * (New Folder / New Category / Delete / Reply / Refresh). */
	header       = adw_header_bar_new ();
	header_title = gtk_label_new ("/");
	gtk_widget_add_css_class (header_title, "heading");
	br->breadcrumb = GTK_LABEL (header_title);
	adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), header_title);
	gtk_window_set_titlebar (GTK_WINDOW (br->window), header);

	/* ---- Two-pane body ---- */
	paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_set_position (GTK_PANED (paned), 280);
	gtk_paned_set_resize_start_child (GTK_PANED (paned), FALSE);
	gtkhx_widget_set_child (br->window, paned);

	/* ---- Left: GtkListView over a GtkTreeListModel ---- */
	br->root_store = g_list_store_new (HX_TYPE_NEWS_NODE);
	br->tree_model = gtk_tree_list_model_new (
		G_LIST_MODEL (br->root_store),    /* takes ownership of one ref */
		FALSE,                             /* passthrough — FALSE means
		                                    * the model items are
		                                    * GtkTreeListRow wrappers */
		FALSE,                             /* autoexpand */
		news_node_create_child_model, br, NULL);
	br->selection = gtk_single_selection_new (G_LIST_MODEL (br->tree_model));
	gtk_single_selection_set_autoselect (br->selection, FALSE);
	gtk_single_selection_set_can_unselect (br->selection, TRUE);

	factory = gtk_signal_list_item_factory_new ();
	g_signal_connect (factory, "setup", G_CALLBACK (on_factory_setup), br);
	g_signal_connect (factory, "bind",  G_CALLBACK (on_factory_bind),  br);

	br->list_view = gtk_list_view_new (
		GTK_SELECTION_MODEL (br->selection),
		factory);
	gtk_list_view_set_show_separators (GTK_LIST_VIEW (br->list_view), FALSE);

	g_signal_connect (br->selection, "selection-changed",
	                  G_CALLBACK (on_selection_changed), br);

	left_scroll = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (left_scroll),
	                                GTK_POLICY_AUTOMATIC,
	                                GTK_POLICY_AUTOMATIC);
	gtkhx_widget_set_child (left_scroll, br->list_view);
	gtk_paned_set_start_child (GTK_PANED (paned), left_scroll);

	/* ---- Right: post body placeholder ----
	 *
	 * Phase 4 routes HTLC_HDR_GETTHREAD replies into this buffer
	 * and adds an author / date / subject header strip above it. */
	right_box    = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	right_scroll = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (right_scroll),
	                                GTK_POLICY_AUTOMATIC,
	                                GTK_POLICY_AUTOMATIC);
	br->post_view = gtk_text_view_new ();
	gtk_text_view_set_editable (GTK_TEXT_VIEW (br->post_view), FALSE);
	gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (br->post_view), FALSE);
	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (br->post_view), GTK_WRAP_WORD_CHAR);
	gtk_widget_set_margin_start  (br->post_view, 8);
	gtk_widget_set_margin_end    (br->post_view, 8);
	gtk_widget_set_margin_top    (br->post_view, 8);
	gtk_widget_set_margin_bottom (br->post_view, 8);
	buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (br->post_view));
	gtk_text_buffer_set_text (buf,
		_("Select a post in the tree to view it here."),
		-1);
	gtkhx_widget_set_child (right_scroll, br->post_view);
	gtkhx_box_pack (right_box, right_scroll, TRUE, TRUE, 0);
	gtk_paned_set_end_child (GTK_PANED (paned), right_box);

	g_signal_connect (br->window, "close-request",
	                  G_CALLBACK (on_window_close), br);

	return br;
}

/* ---------- Entry point ---------- */

void
open_news_browser (GtkWidget *widget, struct _session *sess)
{
	(void) widget; (void) sess;

	if (the_browser) {
		gtk_window_present (GTK_WINDOW (the_browser->window));
		return;
	}

	the_browser = build_browser_window ();
	gtk_window_present (GTK_WINDOW (the_browser->window));

	/* Phase 2: kick off the root NEWSDIRLIST so the top level
	 * populates as soon as the window appears. */
	fetch_dirlist (the_browser, NULL);
}
