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
	                          * for posts: the containing category's path */
	GListStore *children;    /* created lazily on first expansion */
	gboolean    loaded;      /* TRUE once the RPC reply has populated
	                          * children — guards against re-fetch on
	                          * collapse + re-expand */

	/* Post-specific (kind == NB_KIND_POST). Filled at the
	 * NEWSCATLIST reply when the post tree gets built. */
	guint32           postid;
	char             *sender;
	char             *mime_type;
	struct date_time  date;

	/* Cached post body (NULL = not fetched yet; "" = empty
	 * body the server returned). Populated by HTLC_HDR_GETTHREAD
	 * reply via gnews_browser_handle_thread. */
	char             *body;
	gboolean          body_fetching;
};

G_DEFINE_FINAL_TYPE (HxNewsNode, hx_news_node, G_TYPE_OBJECT)

static void
hx_news_node_finalize (GObject *obj)
{
	HxNewsNode *n = HX_NEWS_NODE (obj);
	g_free (n->name);
	g_free (n->path);
	g_free (n->sender);
	g_free (n->mime_type);
	g_free (n->body);
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
	GtkLabel     *subject_label;  /* subject above the body, "heading" */
	GtkLabel     *meta_label;     /* "From <sender> on <date>", "dim-label" */
	GtkWidget    *header_strip;   /* container for subject + meta */
	GtkLabel     *breadcrumb;

	/* Selected post (weak — the GListStore owns the ref). NULL
	 * when nothing or a non-post is selected. Used by the
	 * thread-reply handler to know whether the fetch we got back
	 * is for the currently-displayed post. */
	HxNewsNode   *selected_post;
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

/* Pending HTLC_HDR_GETTHREAD fetches. Keys are throwaway news_item
 * stubs we hand to hx_news15_get_post; values are reffed HxNewsNodes
 * whose `body` cache should be populated when the reply arrives. */
static GHashTable *pending_threads = NULL;    /* stub news_item* → HxNewsNode* */

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
	if (!pending_threads)
		pending_threads = g_hash_table_new_full (
			g_direct_hash, g_direct_equal,
			NULL, g_object_unref);
}

/* ---------- create_child_model: builds the tree's child stores --------
 *
 * Called by GtkTreeListModel for each row to decide whether it's a
 * leaf or expandable. Returning NULL marks the row as a leaf.
 * Returning a GListModel (even empty) marks it as expandable; the
 * tree expander will appear next to the row.
 *
 *   FOLDER, CATEGORY → always expandable. Children get appended
 *                      when the NEWSDIRLIST / NEWSCATLIST reply
 *                      arrives (lazy fetch, see on_row_expanded).
 *   POST             → expandable only if the post has replies,
 *                      i.e. node->children is already populated by
 *                      the NEWSCATLIST threading walker. Post
 *                      replies don't need a separate fetch — they
 *                      came in the same task reply as the parent.
 */
static GListModel *
news_node_create_child_model (gpointer item, gpointer user_data)
{
	HxNewsNode *node = item;
	(void) user_data;

	if (node->kind == NB_KIND_POST) {
		if (node->children
		    && g_list_model_get_n_items (G_LIST_MODEL (node->children)) > 0)
			return G_LIST_MODEL (g_object_ref (node->children));
		return NULL;
	}

	/* Folders + categories: lazy-allocate an empty children store so
	 * the expander appears. The fetch only fires on first expand. */
	if (!node->children)
		node->children = g_list_store_new (HX_TYPE_NEWS_NODE);
	return G_LIST_MODEL (g_object_ref (node->children));
}

/* Join a parent path and a child name to form the child's full
 * Hotline path. The root case ("/") needs special treatment to
 * avoid producing "//child". */
static char *
build_child_path (const char *parent_path, const char *child_name)
{
	if (!parent_path || g_strcmp0 (parent_path, "/") == 0)
		return g_strdup_printf ("/%s", child_name ? child_name : "");
	return g_strdup_printf ("%s/%s", parent_path, child_name ? child_name : "");
}

/* ---------- Icon helpers ---------- */

/* Load one XPM resource and return it as a GdkPaintable (a
 * GdkTexture wrapping a GdkPixbuf). Returns NULL silently on a
 * missing resource — callers null-check.
 *
 * Goes pixbuf → texture rather than calling gtk_image_set_from_
 * resource: the latter renders blank for our XPMs (probably an
 * Adwaita CSS sizing interaction). The pixbuf path is the same
 * one gtkhx_pixmap_button uses for the toolbar icons.
 *
 * Scales the pixbuf by 1.5x with nearest-neighbor interpolation —
 * keeps the blocky pixel-art XPMs crisp without going so big that
 * the rows feel cramped. 2x was too chunky for tree-row use. */
static GdkPaintable *
load_icon_paintable (const char *resource)
{
	GdkPixbuf  *pb;
	GdkTexture *tex;
	int w, h;

	pb = gdk_pixbuf_new_from_resource (resource, NULL);
	if (!pb)
		return NULL;

	w = (gdk_pixbuf_get_width  (pb) * 3) / 2;
	h = (gdk_pixbuf_get_height (pb) * 3) / 2;
	{
		GdkPixbuf *scaled = gdk_pixbuf_scale_simple (pb, w, h, GDK_INTERP_NEAREST);
		g_object_unref (pb);
		pb = scaled;
		if (!pb)
			return NULL;
	}

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

/* Forward decls — selection-changed + the row-expanded handler
 * fire these; the bodies live further down so the file reads
 * lifecycle-first, then RPC, then rendering. */
static void fetch_dirlist        (gnews_browser *br, HxNewsNode *target);
static void fetch_catlist        (gnews_browser *br, HxNewsNode *target);
static void render_selected_post (gnews_browser *br);

/* ---------- Selection → breadcrumb ---------- */

/* Walk up the tree from the currently-selected row collecting names
 * for the breadcrumb. Also returns the leaf HxNewsNode of the
 * selection via `*leaf_out` (caller does not own a ref). */
static void
update_breadcrumb (gnews_browser *br, HxNewsNode **leaf_out)
{
	guint pos = gtk_single_selection_get_selected (br->selection);
	GtkTreeListRow *row;
	GString *crumb;

	if (leaf_out) *leaf_out = NULL;

	if (pos == GTK_INVALID_LIST_POSITION) {
		gtk_label_set_text (br->breadcrumb, "/");
		return;
	}

	row = g_list_model_get_item (G_LIST_MODEL (br->tree_model), pos);
	if (!row) {
		gtk_label_set_text (br->breadcrumb, "/");
		return;
	}

	{
		GPtrArray      *names = g_ptr_array_new_with_free_func (g_free);
		GtkTreeListRow *cur   = g_object_ref (row);
		gboolean        first = TRUE;

		while (cur) {
			HxNewsNode *node = gtk_tree_list_row_get_item (cur);
			if (node) {
				if (first && leaf_out) {
					/* The first node we visit IS the selected
					 * leaf (we walk upward from there). */
					*leaf_out = node;
				}
				g_ptr_array_insert (names, 0, g_strdup (node->name));
				g_object_unref (node);
				first = FALSE;
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
	gnews_browser *br = user_data;
	HxNewsNode    *leaf = NULL;
	(void) sel; (void) position; (void) n_items;

	update_breadcrumb (br, &leaf);

	/* Track the currently-selected post (NULL for folder /
	 * category / empty). The reply handler uses this to decide
	 * whether to push a fetched body into the view. */
	br->selected_post = (leaf && leaf->kind == NB_KIND_POST) ? leaf : NULL;

	render_selected_post (br);
}

/* ---------- Factory: setup + bind for each row widget ---------- */

static void
on_row_expanded (GtkTreeListRow *row, GParamSpec *pspec, gpointer user_data)
{
	gnews_browser *br   = user_data;
	HxNewsNode    *node;
	(void) pspec;

	if (!gtk_tree_list_row_get_expanded (row))
		return;                /* collapse — nothing to do */

	node = gtk_tree_list_row_get_item (row);
	if (!node) return;

	if (!node->loaded) {
		if (node->kind == NB_KIND_FOLDER)
			fetch_dirlist (br, node);
		else if (node->kind == NB_KIND_CATEGORY)
			fetch_catlist (br, node);
		/* Posts: children are already populated by the catlist
		 * threading walker, so no fetch needed. */
	}

	g_object_unref (node);
}

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

	/* Lazy-fetch: connect a notify::expanded handler so the first
	 * time the row's expander flips open we fire the NEWSDIRLIST /
	 * NEWSCATLIST that populates its children. The connection
	 * lives for this row binding only — unbind disconnects. */
	{
		gulong id = g_signal_connect (row, "notify::expanded",
		                              G_CALLBACK (on_row_expanded), br);
		g_object_set_data (G_OBJECT (expander), "expanded-handler",
		                   GSIZE_TO_POINTER ((gsize) id));
		g_object_set_data_full (G_OBJECT (expander), "bound-row",
		                        g_object_ref (row), g_object_unref);
	}

	g_object_unref (node);   /* gtk_tree_list_row_get_item returned a ref */
}

static void
on_factory_unbind (GtkSignalListItemFactory *factory,
                   GtkListItem *list_item, gpointer user_data)
{
	(void) factory; (void) user_data;

	GtkWidget      *expander = gtk_list_item_get_child (list_item);
	gulong          id;
	GtkTreeListRow *row;

	id  = (gulong) GPOINTER_TO_SIZE (
		g_object_get_data (G_OBJECT (expander), "expanded-handler"));
	row = g_object_get_data (G_OBJECT (expander), "bound-row");
	if (id && row && g_signal_handler_is_connected (row, id))
		g_signal_handler_disconnect (row, id);
	g_object_set_data (G_OBJECT (expander), "expanded-handler", NULL);
	g_object_set_data (G_OBJECT (expander), "bound-row", NULL);

	gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), NULL);
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

	/* Set loaded BEFORE the wire call so a quick collapse + reexpand
	 * sequence doesn't re-fire the fetch. The reply handler appends
	 * children into the existing store. */
	if (target)
		target->loaded = TRUE;

	hx_news15_fldr_list (&the_session.htlc, stub);
}

/* Fire HTLC_HDR_NEWSCATLIST for a category node. `target` is the
 * category HxNewsNode whose `children` store should be populated
 * with the threaded posts. */
static void
fetch_catlist (gnews_browser *br, HxNewsNode *target)
{
	struct gnews_catalog *stub;
	(void) br;

	if (!target || !target->path)
		return;

	ensure_pending_tables ();

	stub = g_malloc0 (sizeof (struct gnews_catalog));
	stub->path = g_strdup (target->path);

	g_hash_table_insert (pending_catlists, stub, g_object_ref (target));

	target->loaded = TRUE;

	hx_news15_cat_list (&the_session.htlc, stub);
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
		const char *parent_path = target ? target->path : "/";
		for (i = 0; i < folder->num_entries; i++) {
			struct folder_item *item = folder->entry[i];
			int kind = (item->type == 1) ? NB_KIND_FOLDER
			                              : NB_KIND_CATEGORY;
			char *child_path = build_child_path (parent_path, item->name);
			HxNewsNode *node = hx_news_node_new (kind, item->name, child_path);
			g_list_store_append (dest, node);
			g_object_unref (node);
			g_free (child_path);
		}
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

/* Build the thread tree for one news_group. Walks the flat
 * posts[] array, links posts by parentid, and appends top-level
 * posts into `dest` (with replies as their children, recursively
 * via HxNewsNode->children).
 *
 * Posts are visited in array order (server-given chronological).
 * Parents typically appear before their replies, but we don't
 * rely on that — a two-pass build (build map, then attach) makes
 * the order irrelevant. */
static void
catlist_thread_into (GListStore *dest, struct news_group *group,
                     const char *category_path)
{
	GHashTable *by_postid;
	HxNewsNode **nodes;
	int i;

	if (!group || group->post_count <= 0)
		return;

	/* Pass 1: build a postid → HxNewsNode map. Each node steals
	 * the subject / sender / mime_type strings from the news_item
	 * (we'll NULL them in the source so the freer skips them). */
	by_postid = g_hash_table_new (g_direct_hash, g_direct_equal);
	nodes = g_new0 (HxNewsNode *, group->post_count);

	for (i = 0; i < group->post_count; i++) {
		struct news_item *it = &group->posts[i];
		HxNewsNode *n = hx_news_node_new (
			NB_KIND_POST,
			it->subject && *it->subject ? it->subject : "(no subject)",
			category_path);
		n->postid    = it->postid;
		n->sender    = g_strdup (it->sender ? it->sender : "");
		n->date      = it->date;
		n->mime_type = (it->partcount > 0 && it->parts && it->parts[0].mime_type)
			? g_strdup (it->parts[0].mime_type)
			: g_strdup ("text/plain");
		nodes[i] = n;
		g_hash_table_insert (by_postid,
		                     GUINT_TO_POINTER (it->postid), n);
	}

	/* Pass 2: attach each node to its parent (or to dest if
	 * top-level / dangling parent). We append by ref since the
	 * GListStore takes its own ref. */
	for (i = 0; i < group->post_count; i++) {
		struct news_item *it = &group->posts[i];
		HxNewsNode       *n  = nodes[i];
		HxNewsNode       *parent = NULL;

		if (it->parentid != 0)
			parent = g_hash_table_lookup (
				by_postid, GUINT_TO_POINTER (it->parentid));

		if (parent && parent != n) {
			if (!parent->children)
				parent->children = g_list_store_new (HX_TYPE_NEWS_NODE);
			g_list_store_append (parent->children, n);
		} else {
			g_list_store_append (dest, n);
		}
		g_object_unref (n);
	}

	g_free (nodes);
	g_hash_table_destroy (by_postid);
}

/* Free a news_group + its child news_items + their owned strings.
 * Mirrors the partial-cleanup the existing rcv-task path leaves us
 * with (the rcv path g_strdup-s subject, sender, mime_type into the
 * news_item and we never free them anywhere — the legacy
 * output_news_catalog just holds onto the group for the window's
 * lifetime). */
static void
news_group_free (struct news_group *group)
{
	int i;
	if (!group)
		return;
	if (group->posts) {
		for (i = 0; i < group->post_count; i++) {
			struct news_item *it = &group->posts[i];
			g_free (it->subject);
			g_free (it->sender);
			if (it->parts) {
				int j;
				for (j = 0; j < it->partcount; j++)
					g_free (it->parts[j].mime_type);
				g_free (it->parts);
			}
		}
		g_free (group->posts);
	}
	g_free (group);
}

gboolean
gnews_browser_handle_catlist (gpointer gcnews_p)
{
	struct gnews_catalog *gcnews = gcnews_p;
	HxNewsNode *target = NULL;
	gnews_browser *br = the_browser;

	if (!pending_catlists)
		return FALSE;
	if (!g_hash_table_contains (pending_catlists, gcnews))
		return FALSE;

	target = g_hash_table_lookup (pending_catlists, gcnews);
	if (target)
		g_object_ref (target);
	g_hash_table_remove (pending_catlists, gcnews);

	if (br && target && target->children && gcnews->group) {
		catlist_thread_into (target->children, gcnews->group, target->path);
	}

	/* Free the parsed group + the stub. */
	news_group_free (gcnews->group);
	g_free (gcnews->path);
	g_free (gcnews);

	g_clear_object (&target);
	return TRUE;
}

/* ---------- Post body fetch + display ---------- */

/* Format the post date as a human-friendly string. Mirrors the
 * Mac-classic-or-Unix branch news15.c::date_to_unix uses. */
static char *
post_date_format (const struct date_time *dt)
{
	time_t t;
	struct tm tm_buf;
	char buf[64];

	if (dt->base_year >= 1970) {
		struct tm timetm;
		memset (&timetm, 0, sizeof timetm);
		timetm.tm_sec  = dt->seconds + (24 * 3600);
		timetm.tm_year = dt->base_year - 1900;
		if (timetm.tm_year < 0)
			timetm.tm_year = 1970;
		t = mktime (&timetm);
	} else if (dt->base_year == 1904) {
		t = dt->seconds - 2082844800U;
	} else {
		return g_strdup ("");
	}

	if (!localtime_r (&t, &tm_buf))
		return g_strdup ("");

	if (strftime (buf, sizeof buf, "%a %b %e %H:%M:%S %Y", &tm_buf) == 0)
		return g_strdup ("");

	return g_strdup (buf);
}

/* Issue HTLC_HDR_GETTHREAD for `target`. The legacy hx_news15_get_post
 * helper takes a struct news_item — build a stub one with just the
 * fields it dereferences (postid, group->path, parts[0].mime_type)
 * and register it in pending_threads so the reply routes back to us. */
static void
fetch_thread (gnews_browser *br, HxNewsNode *target)
{
	struct news_item  *stub_item;
	struct news_group *stub_group;
	(void) br;

	if (!target || target->kind != NB_KIND_POST || !target->path)
		return;
	if (target->body_fetching)
		return;             /* already in flight */

	ensure_pending_tables ();

	stub_group = g_malloc0 (sizeof (struct news_group));
	stub_group->path = g_strdup (target->path);

	stub_item = g_malloc0 (sizeof (struct news_item));
	stub_item->postid     = target->postid;
	stub_item->group      = stub_group;
	stub_item->partcount  = 1;
	stub_item->parts      = g_malloc0 (sizeof (struct news_parts));
	stub_item->parts[0].mime_type = g_strdup (
		target->mime_type ? target->mime_type : "text/plain");

	g_hash_table_insert (pending_threads, stub_item, g_object_ref (target));
	target->body_fetching = TRUE;

	hx_news15_get_post (&the_session.htlc, stub_item);
}

/* Render the currently-selected post into the right pane. If the
 * body hasn't been fetched yet, fires fetch_thread and shows a
 * "Loading…" placeholder until the reply lands. */
static void
render_selected_post (gnews_browser *br)
{
	HxNewsNode *node = br->selected_post;
	GtkTextBuffer *buf;

	if (!node || node->kind != NB_KIND_POST) {
		gtk_widget_set_visible (br->header_strip, FALSE);
		buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (br->post_view));
		gtk_text_buffer_set_text (buf,
			_("Select a post in the tree to view it here."),
			-1);
		return;
	}

	/* Header strip */
	gtk_label_set_text (br->subject_label,
	                    node->name && *node->name ? node->name : _("(no subject)"));
	{
		char *date_str = post_date_format (&node->date);
		char *meta = g_strdup_printf (_("%s — %s"),
			node->sender && *node->sender ? node->sender : "?",
			date_str);
		gtk_label_set_text (br->meta_label, meta);
		g_free (meta);
		g_free (date_str);
	}
	gtk_widget_set_visible (br->header_strip, TRUE);

	/* Body */
	buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (br->post_view));
	if (node->body) {
		gtk_text_buffer_set_text (buf, node->body, -1);
	} else {
		gtk_text_buffer_set_text (buf, _("Loading…"), -1);
		fetch_thread (br, node);
	}
}

gboolean
gnews_browser_handle_thread (gpointer post_p)
{
	struct news_post *post = post_p;
	struct news_item *stub_item;
	HxNewsNode       *target = NULL;
	gnews_browser    *br = the_browser;

	if (!post || !pending_threads)
		return FALSE;
	stub_item = post->item;
	if (!stub_item)
		return FALSE;
	if (!g_hash_table_contains (pending_threads, stub_item))
		return FALSE;

	target = g_hash_table_lookup (pending_threads, stub_item);
	if (target)
		g_object_ref (target);
	g_hash_table_remove (pending_threads, stub_item);

	if (target) {
		target->body_fetching = FALSE;
		g_free (target->body);
		target->body = g_strdup (post->buf ? post->buf : "");

		/* If the post is still the selected one, push the body
		 * into the view. (User may have moved on while the fetch
		 * was in flight — in that case we just keep the cached
		 * body for when they come back.) */
		if (br && br->selected_post == target)
			render_selected_post (br);
	}

	/* Free the stub news_item + stub group + the news_post. */
	if (stub_item->parts) {
		int j;
		for (j = 0; j < stub_item->partcount; j++)
			g_free (stub_item->parts[j].mime_type);
		g_free (stub_item->parts);
	}
	g_free (stub_item->subject);
	g_free (stub_item->sender);
	if (stub_item->group) {
		g_free (stub_item->group->path);
		g_free (stub_item->group);
	}
	g_free (stub_item);

	g_free (post->buf);
	g_free (post);

	g_clear_object (&target);
	return TRUE;
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
	br->icon_post     = load_icon_paintable ("/com/nasledov/gtkhx/pixmaps/newspost.xpm");

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
	g_signal_connect (factory, "setup",  G_CALLBACK (on_factory_setup),  br);
	g_signal_connect (factory, "bind",   G_CALLBACK (on_factory_bind),   br);
	g_signal_connect (factory, "unbind", G_CALLBACK (on_factory_unbind), br);

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

	/* ---- Right: post header strip + body ----
	 *
	 *   header_strip (hidden until a post is selected)
	 *     subject_label  — "heading" CSS class
	 *     meta_label     — "<sender> — <date>", "dim-label" class
	 *   separator
	 *   body GtkTextView (scrolled, read-only) */
	right_box        = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	br->header_strip = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_margin_start  (br->header_strip, 12);
	gtk_widget_set_margin_end    (br->header_strip, 12);
	gtk_widget_set_margin_top    (br->header_strip, 10);
	gtk_widget_set_margin_bottom (br->header_strip, 8);

	{
		GtkWidget *subj = gtk_label_new (NULL);
		GtkWidget *meta = gtk_label_new (NULL);

		gtk_label_set_xalign     (GTK_LABEL (subj), 0.0f);
		gtk_label_set_wrap       (GTK_LABEL (subj), TRUE);
		gtk_label_set_wrap_mode  (GTK_LABEL (subj), PANGO_WRAP_WORD_CHAR);
		gtk_widget_add_css_class (subj, "heading");

		gtk_label_set_xalign     (GTK_LABEL (meta), 0.0f);
		gtk_label_set_ellipsize  (GTK_LABEL (meta), PANGO_ELLIPSIZE_END);
		gtk_widget_add_css_class (meta, "dim-label");

		gtk_box_append (GTK_BOX (br->header_strip), subj);
		gtk_box_append (GTK_BOX (br->header_strip), meta);

		br->subject_label = GTK_LABEL (subj);
		br->meta_label    = GTK_LABEL (meta);
	}
	gtk_widget_set_visible (br->header_strip, FALSE);
	gtk_box_append (GTK_BOX (right_box), br->header_strip);
	gtk_box_append (GTK_BOX (right_box),
	                gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

	right_scroll = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (right_scroll),
	                                GTK_POLICY_AUTOMATIC,
	                                GTK_POLICY_AUTOMATIC);
	br->post_view = gtk_text_view_new ();
	gtk_text_view_set_editable (GTK_TEXT_VIEW (br->post_view), FALSE);
	gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (br->post_view), FALSE);
	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (br->post_view), GTK_WRAP_WORD_CHAR);
	gtk_widget_set_margin_start  (br->post_view, 12);
	gtk_widget_set_margin_end    (br->post_view, 12);
	gtk_widget_set_margin_top    (br->post_view, 10);
	gtk_widget_set_margin_bottom (br->post_view, 10);
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
