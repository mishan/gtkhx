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
 * window + 2-pane layout, empty tree.
 * root NEWSDIRLIST on open populates the top level
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
#include <libpanel.h>

#include "hx.h"
#include "hx_panel.h"
#include "panel_registry.h"
#include "toolbar.h"
#include "session.h"
#include "gtkhx_session.h"
#include "network.h"
#include "news15.h"
#include "news_browser.h"
#include "gtkutil.h"
#include "gtkhx.h"
#include "gtkhx_icon.h"
#include "gtkurl.h"
#include "hl_access.h"
#include "hl_date.h"
#include "debug.h"

/* Pure post-threading layout (hxnews-model crate, unit-tested). Fills
 * out_parent[i] with post i's parent *array index*, or -1 for a top-level
 * post — the postid→index map + the parentid==0 / self / missing rules that
 * catlist_thread_into used to do inline with a GHashTable. */
extern void hx_news_thread_parent_indices (const guint32 *postids,
                                           const guint32 *parentids, gsize n,
                                           int *out_parent);

/* Category tree builder (hxnews-model crate; replaces catlist_thread_into's
 * inline node-tree assembly). Given a category's posts, it creates one
 * NB_KIND_POST HxNewsNode per post, threads replies under their parents (using
 * the parent-index logic above), and appends the top-level posts to `dest`
 * LAST — so each has its full reply subtree before GtkTreeListModel fixes its
 * one-shot expandable verdict. The const char* fields may be NULL; defaults are
 * applied (subject "(no subject)", sender "", mime "text/plain"). Layout of
 * struct hx_news_post_data must match the crate's #[repr(C)] HxNewsPostData. */
struct hx_news_post_data {
    guint32 postid;
    guint32 parentid;
    const char *subject;
    const char *sender;
    const char *mime_type;
    struct date_time date;
};
extern void hx_news_build_category_tree (GListStore *dest,
                                         const char *category_path,
                                         const struct hx_news_post_data *posts,
                                         gsize count);

/* ---------- HxNewsNode (one GObject per tree row) ----------
 *
 * The node — one GObject per folder / category / post — moved to the
 * hxnews-model Rust crate (glib::subclass GObject; unit-tested headless). It's
 * a pure data holder, so the C browser here keeps the GtkTreeListModel /
 * factory / fetch glue and reaches the node opaquely through the
 * hx_news_node_* accessors below. `HX_NEWS_NODE(obj)` is now just a pointer
 * cast; `HX_TYPE_NEWS_NODE` still backs g_list_store_new / the tree item type. */

enum {
    NB_KIND_FOLDER = 1,
    NB_KIND_CATEGORY = 2,
    NB_KIND_POST = 3,
};

typedef struct _HxNewsNode HxNewsNode;
#define HX_TYPE_NEWS_NODE (hx_news_node_get_type ())
#define HX_NEWS_NODE(obj) ((HxNewsNode *) (obj))

extern GType hx_news_node_get_type (void);
extern HxNewsNode *hx_news_node_new (int kind, const char *name,
                                     const char *path);
extern int hx_news_node_kind (HxNewsNode *node);
extern guint32 hx_news_node_postid (HxNewsNode *node);
extern gboolean hx_news_node_loaded (HxNewsNode *node);
extern gboolean hx_news_node_body_fetching (HxNewsNode *node);
extern void hx_news_node_set_loaded (HxNewsNode *node, gboolean loaded);
extern void hx_news_node_set_body_fetching (HxNewsNode *node, gboolean fetching);
extern void hx_news_node_set_postid (HxNewsNode *node, guint32 postid);
extern const char *hx_news_node_name (HxNewsNode *node);
extern const char *hx_news_node_path (HxNewsNode *node);
extern const char *hx_news_node_sender (HxNewsNode *node);
extern const char *hx_news_node_mime_type (HxNewsNode *node);
extern const char *hx_news_node_body (HxNewsNode *node);
extern void hx_news_node_set_sender (HxNewsNode *node, const char *s);
extern void hx_news_node_set_mime_type (HxNewsNode *node, const char *s);
extern void hx_news_node_set_body (HxNewsNode *node, const char *s);
extern void hx_news_node_get_date (HxNewsNode *node, struct date_time *out);
extern void hx_news_node_set_date (HxNewsNode *node, const struct date_time *date);
extern GListStore *hx_news_node_children (HxNewsNode *node);
extern GListStore *hx_news_node_ensure_children (HxNewsNode *node);

/* Create (new folder / category) + delete-confirm dialogs — ported to the
 * gtkhx-ui Rust crate (news_dialogs.rs). C keeps the selection logic (the
 * toolbar handlers below pick the target and hand it over); Rust owns the
 * dialog, the wire send, and the post-send refresh via the bridge just
 * below. `parent` may be NULL (create at root); the delete opener takes a
 * snapshot of the target's identity by value. */
extern void gtkhx_news_create_dialog_open (GtkWidget *parent_window,
                                           HxNewsNode *parent, int kind);
extern void gtkhx_news_delete_dialog_open (GtkWidget *parent_window, int kind,
                                           const char *name, const char *path,
                                           guint32 postid);

/* Re-fetch a listing after a create / delete (the server pushes no
 * notification for these). Called from the Rust dialogs once the RPC is
 * away; `node` NULL means refresh from the root. */
void gtkhx_news_browser_refresh (HxNewsNode *node);

/* ---------- Browser ---------- */

struct _gnews_browser {
    /* window points at the HxPanel
     * widget that hosts the browser content (was a standalone
     * GtkWindow). adw_dialog_present and gtk_widget_get_root keep
     * working via the duck-typed widget walk to the toplevel
     * (toolbar_window). init_keyaccel is still attached to this
     * widget but only the Ctrl+Q / Ctrl+K / Ctrl+T accelerators
     * take effect — the Ctrl+W close path inside init_keyaccel
     * checks GTK_IS_WINDOW and bails on a PanelWidget. The one
     * site that actually needed a GtkWindow —
     * gtk_window_set_transient_for for the compose window —
     * switched to toolbar_window. */
    GtkWidget *window;
    GtkWidget *button_bar;

    /* Tree side */
    GListStore *root_store;       /* top-level nodes */
    GtkTreeListModel *tree_model; /* wraps root_store + children */
    GtkSingleSelection *selection;
    GtkWidget *list_view;

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
    GtkWidget *post_view;    /* GtkTextView, read-only */
    GtkLabel *subject_label; /* subject above the body, "heading" */
    GtkLabel *meta_label;    /* "From <sender> on <date>", "dim-label" */
    GtkWidget *header_strip; /* container for subject + meta */
    GtkLabel *breadcrumb;

    /* Selected post (weak — the GListStore owns the ref). NULL
	 * when nothing or a non-post is selected. Used by the
	 * thread-reply handler to know whether the fetch we got back
	 * is for the currently-displayed post. */
    HxNewsNode *selected_post;

    /* Header bar action buttons.
	 *
	 * Visibility is selection-driven by sync_action_buttons:
	 *   root / no selection → Refresh, New Folder, New Category
	 *   folder              → Refresh, New Folder, New Category, Delete
	 *   category            → Refresh, New Post, Delete
	 *   post                → Refresh, New Post, Reply, Delete
	 *
	 * Sensitivity is access-bit-driven by the same function:
	 * the per-button access bit must be set in htlc->access for
	 * the button to be clickable. Buttons stay visible-but-grey
	 * when the action is forbidden — the user can see the slot is
	 * there but the server has revoked permission. */
    GtkWidget *btn_refresh;
    GtkWidget *btn_new_folder;
    GtkWidget *btn_new_category;
    GtkWidget *btn_new_post;
    GtkWidget *btn_reply;
    GtkWidget *btn_delete;

    /* Disconnected-state banner. AdwBanner above the breadcrumb row
     * that reveals when the connection drops and dismisses on
     * LOGIN_READY. Hidden by default — built unconnected so the
     * first present without a server already shows the right
     * state without flashing. */
    AdwBanner *disconnected_banner;

    /* GtkhxSession::connection-state-changed handler — manages the
     * disconnected banner + auto-fetches NEWSDIRLIST on LOGIN_READY
     * when the panel is open. Disconnected on browser teardown
     * (currently never — the HxPanel is a permanent toolbar
     * resident; the handler ID is still tracked for Phase 4 layout-
     * teardown work). */
    gulong conn_state_handler;
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
static GHashTable *pending_dirlists = NULL; /* stub → HxNewsNode* or NULL */
static GHashTable *pending_catlists = NULL; /* stub → HxNewsNode* or NULL */

/* Pending HTLC_HDR_GETTHREAD fetches. Keys are throwaway news_item
 * stubs we hand to hx_news15_get_post; values are reffed HxNewsNodes
 * whose `body` cache should be populated when the reply arrives. */
static GHashTable *pending_threads = NULL; /* stub news_item* → HxNewsNode* */

static void
ensure_pending_tables (void)
{
    if (!pending_dirlists) {
        pending_dirlists = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                  NULL, g_object_unref);
    }
    if (!pending_catlists) {
        pending_catlists = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                  NULL, g_object_unref);
    }
    if (!pending_threads) {
        pending_threads = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                 NULL, g_object_unref);
    }
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
    (void)user_data;

    if (hx_news_node_kind (node) == NB_KIND_POST) {
        GListStore *ch = hx_news_node_children (node);
        if (ch && g_list_model_get_n_items (G_LIST_MODEL (ch)) > 0) {
            return G_LIST_MODEL (g_object_ref (ch));
        }
        return NULL;
    }

    /* Folders + categories: lazy-allocate an empty children store so
	 * the expander appears. The fetch only fires on first expand. */
    return G_LIST_MODEL (g_object_ref (hx_news_node_ensure_children (node)));
}

/* Join a parent path and a child name to form the child's full
 * Hotline path. The root case ("/") needs special treatment to
 * avoid producing "//child". */
static char *
build_child_path (const char *parent_path, const char *child_name)
{
    if (!parent_path || g_strcmp0 (parent_path, "/") == 0) {
        return g_strdup_printf ("/%s", child_name ? child_name : "");
    }
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
    GdkPixbuf *pb;
    GdkTexture *tex;
    int w, h;

    /* Route through the icon resolver so the active theme's bundled
	 * icons (e.g. $CONFIG/themes/<theme>/icons/news_folder.png) shadow
	 * the stock pixmap. */
    pb = gtkhx_icon_load (resource);
    if (!pb) {
        return NULL;
    }

    w = (gdk_pixbuf_get_width (pb) * 3) / 2;
    h = (gdk_pixbuf_get_height (pb) * 3) / 2;
    {
        GdkPixbuf *scaled
            = gdk_pixbuf_scale_simple (pb, w, h, GDK_INTERP_NEAREST);
        g_object_unref (pb);
        pb = scaled;
        if (!pb) {
            return NULL;
        }
    }

    tex = gtkhx_texture_from_pixbuf (pb);
    g_object_unref (pb);
    return tex ? GDK_PAINTABLE (tex) : NULL;
}

static GdkPaintable *
icon_paintable_for_kind (gnews_browser *br, int kind)
{
    switch (kind) {
    case NB_KIND_FOLDER:
        return br->icon_folder;
    case NB_KIND_CATEGORY:
        return br->icon_category;
    case NB_KIND_POST:
        return br->icon_post;
    default:
        return NULL;
    }
}

/* Forward decls — selection-changed + the row-expanded handler
 * fire these; the bodies live further down so the file reads
 * lifecycle-first, then RPC, then rendering. */
static void fetch_dirlist (gnews_browser *br, HxNewsNode *target);
static void fetch_catlist (gnews_browser *br, HxNewsNode *target);
static void render_selected_post (gnews_browser *br);
static void sync_action_buttons (gnews_browser *br);

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

    if (leaf_out) {
        *leaf_out = NULL;
    }

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
        GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
        GtkTreeListRow *cur = g_object_ref (row);
        gboolean first = TRUE;

        while (cur) {
            HxNewsNode *node = gtk_tree_list_row_get_item (cur);
            if (node) {
                if (first && leaf_out) {
                    /* The first node we visit IS the selected
					 * leaf (we walk upward from there). */
                    *leaf_out = node;
                }
                g_ptr_array_insert (names, 0,
                                    g_strdup (hx_news_node_name (node)));
                g_object_unref (node);
                first = FALSE;
            }
            GtkTreeListRow *parent = gtk_tree_list_row_get_parent (cur);
            g_object_unref (cur);
            cur = parent;
        }

        crumb = g_string_new ("/");
        for (guint i = 0; i < names->len; i++) {
            if (i > 0) {
                g_string_append_c (crumb, '/');
            }
            g_string_append (crumb, (const char *)names->pdata[i]);
        }
        g_ptr_array_free (names, TRUE);
    }

    gtk_label_set_text (br->breadcrumb, crumb->str);
    g_string_free (crumb, TRUE);
    g_object_unref (row);
}

static void
on_selection_changed (GtkSingleSelection *sel, guint position, guint n_items,
                      gpointer user_data)
{
    gnews_browser *br = user_data;
    HxNewsNode *leaf = NULL;
    (void)sel;
    (void)position;
    (void)n_items;

    update_breadcrumb (br, &leaf);

    /* Track the currently-selected post (NULL for folder /
	 * category / empty). The reply handler uses this to decide
	 * whether to push a fetched body into the view. */
    br->selected_post
        = (leaf && hx_news_node_kind (leaf) == NB_KIND_POST) ? leaf : NULL;

    render_selected_post (br);
    sync_action_buttons (br);
}

/* ---------- Factory: setup + bind for each row widget ---------- */

static void
on_row_expanded (GtkTreeListRow *row, GParamSpec *pspec, gpointer user_data)
{
    gnews_browser *br = user_data;
    HxNewsNode *node;
    (void)pspec;

    if (!gtk_tree_list_row_get_expanded (row)) {
        return; /* collapse — nothing to do */
    }

    node = gtk_tree_list_row_get_item (row);
    if (!node) {
        return;
    }

    if (!hx_news_node_loaded (node)) {
        if (hx_news_node_kind (node) == NB_KIND_FOLDER) {
            fetch_dirlist (br, node);
        } else if (hx_news_node_kind (node) == NB_KIND_CATEGORY) {
            fetch_catlist (br, node);
        }
        /* Posts: children are already populated by the catlist
		 * threading walker, so no fetch needed. */
    }

    g_object_unref (node);
}

static void
on_factory_setup (GtkSignalListItemFactory *factory, GtkListItem *list_item,
                  gpointer user_data)
{
    (void)factory;
    (void)user_data;

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new ();
    GtkWidget *label = gtk_label_new (NULL);
    GtkWidget *expander = gtk_tree_expander_new ();

    /* GtkImage clamps paintable size to its `icon-size` (~16px by
	 * default in Adwaita), so the upscaled-paintable bytes are
	 * shrunk back down at draw time. Override with pixel_size to
	 * make the row actually use the 1.5x render. */
    gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);

    gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
    gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (box), icon);
    gtk_box_append (GTK_BOX (box), label);
    gtk_tree_expander_set_child (GTK_TREE_EXPANDER (expander), box);

    gtk_list_item_set_child (list_item, expander);

    /* Stash refs on the expander itself so bind() can find them
	 * without juggling another struct. */
    g_object_set_data (G_OBJECT (expander), "icon", icon);
    g_object_set_data (G_OBJECT (expander), "label", label);
}

static void
on_factory_bind (GtkSignalListItemFactory *factory, GtkListItem *list_item,
                 gpointer user_data)
{
    gnews_browser *br = user_data;
    (void)factory;

    GtkWidget *expander = gtk_list_item_get_child (list_item);
    GtkTreeListRow *row = gtk_list_item_get_item (list_item);
    HxNewsNode *node = row ? gtk_tree_list_row_get_item (row) : NULL;
    GtkImage *icon = g_object_get_data (G_OBJECT (expander), "icon");
    GtkLabel *label = g_object_get_data (G_OBJECT (expander), "label");

    gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), row);

    if (!node) {
        gtk_image_clear (icon);
        gtk_label_set_text (label, "");
        return;
    }

    GdkPaintable *paintable
        = icon_paintable_for_kind (br, hx_news_node_kind (node));
    if (paintable) {
        gtk_image_set_from_paintable (icon, paintable);
    } else {
        gtk_image_clear (icon);
    }

    gtk_label_set_text (label, hx_news_node_name (node));

    /* Lazy-fetch: connect a notify::expanded handler so the first
	 * time the row's expander flips open we fire the NEWSDIRLIST /
	 * NEWSCATLIST that populates its children. The connection
	 * lives for this row binding only — unbind disconnects. */
    {
        gulong id = g_signal_connect (row, "notify::expanded",
                                      G_CALLBACK (on_row_expanded), br);
        g_object_set_data (G_OBJECT (expander), "expanded-handler",
                           GSIZE_TO_POINTER ((gsize)id));
        g_object_set_data_full (G_OBJECT (expander), "bound-row",
                                g_object_ref (row), g_object_unref);
    }

    g_object_unref (node); /* gtk_tree_list_row_get_item returned a ref */
}

static void
on_factory_unbind (GtkSignalListItemFactory *factory, GtkListItem *list_item,
                   gpointer user_data)
{
    (void)factory;
    (void)user_data;

    GtkWidget *expander = gtk_list_item_get_child (list_item);
    gulong id;
    GtkTreeListRow *row;

    id = (gulong)GPOINTER_TO_SIZE (
        g_object_get_data (G_OBJECT (expander), "expanded-handler"));
    row = g_object_get_data (G_OBJECT (expander), "bound-row");
    if (id && row && g_signal_handler_is_connected (row, id)) {
        g_signal_handler_disconnect (row, id);
    }
    g_object_set_data (G_OBJECT (expander), "expanded-handler", NULL);
    g_object_set_data (G_OBJECT (expander), "bound-row", NULL);

    gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), NULL);
}

/* ---------- RPC dispatch ---------- */

/* Whether it's worth speaking the 1.5 threaded-news protocol to the
 * current server. It needs BOTH the 1.5 protocol — the server advertised
 * HTLS_DATA_VERSION >= 150 — AND read-news permission. A 1.0/1.2-class
 * server (version 0: the classic servers that don't advertise a version)
 * doesn't implement NEWSDIRLIST and rejects it with a task error, so we
 * don't auto-fire it there; the flat News window (news.c) is the news UI
 * for those. hl_access_permits keeps a permission-less legacy account
 * (empty access map) allowed — the version gate is what excludes it. */
static gboolean
threaded_news_available (void)
{
    const guint8 *access = (const guint8 *) &hx_active_session ()->htlc.access;
    return hx_active_session ()->htlc.version >= 150
           && hl_access_permits (access, HL_ACCESS_READ_NEWS);
}

/* Fire HTLC_HDR_NEWSDIRLIST. `target` is the HxNewsNode whose
 * `children` store should be populated; NULL means a root fetch
 * (populate the browser's root_store instead). */
static void
fetch_dirlist (gnews_browser *br, HxNewsNode *target)
{
    struct gnews_folder *stub;
    (void)br;

    /* Don't send NEWSDIRLIST to a server that can't answer it (1.0/1.2, or
	 * no read-news permission) — it just earns a task-error toast on login.
	 * Covers both the auto-fires (LOGIN_READY / panel-presented) and a
	 * manual root Refresh; a legacy server's tree stays empty so node
	 * expansions never reach here. */
    if (!threaded_news_available ()) {
        debug_log ("news",
                   "skipping NEWSDIRLIST — server version %u lacks 1.5 "
                   "threaded news (or account lacks read-news)",
                   (unsigned) hx_active_session ()->htlc.version);
        return;
    }

    ensure_pending_tables ();

    stub = g_malloc0 (sizeof (struct gnews_folder));
    /* path_to_hldir (called inside hx_news15_fldr_list) walks the
	 * string and dereferences it unconditionally — NULL crashes.
	 * The legacy create_gfnews_window uses "/" for the root case;
	 * mirror that. */
    stub->path = target && hx_news_node_path (target)
                     ? g_strdup (hx_news_node_path (target))
                     : g_strdup ("/");

    g_hash_table_insert (pending_dirlists, stub,
                         target ? g_object_ref (target) : NULL);

    /* Set loaded BEFORE the wire call so a quick collapse + reexpand
	 * sequence doesn't re-fire the fetch. The reply handler appends
	 * children into the existing store. */
    if (target) {
        hx_news_node_set_loaded (target, TRUE);
    }

    hx_news15_fldr_list (&hx_active_session ()->htlc, stub);
}

/* Fire HTLC_HDR_NEWSCATLIST for a category node. `target` is the
 * category HxNewsNode whose `children` store should be populated
 * with the threaded posts. */
static void
fetch_catlist (gnews_browser *br, HxNewsNode *target)
{
    struct gnews_catalog *stub;
    (void)br;

    if (!target || !hx_news_node_path (target)) {
        return;
    }

    ensure_pending_tables ();

    stub = g_malloc0 (sizeof (struct gnews_catalog));
    stub->path = g_strdup (hx_news_node_path (target));

    g_hash_table_insert (pending_catlists, stub, g_object_ref (target));

    hx_news_node_set_loaded (target, TRUE);

    hx_news15_cat_list (&hx_active_session ()->htlc, stub);
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

    if (!pending_dirlists) {
        return FALSE;
    }
    if (!g_hash_table_contains (pending_dirlists, gfnews)) {
        return FALSE;
    }

    /* Steal the target ref so the hashtable destroy notify doesn't
	 * unref it before we're done. */
    target = g_hash_table_lookup (pending_dirlists, gfnews);
    if (target) {
        g_object_ref (target);
    }
    g_hash_table_remove (pending_dirlists, gfnews);

    /* Browser still alive: pick the right destination store. */
    if (br) {
        if (!target) {
            dest = br->root_store;
        } else {
            dest = hx_news_node_children (target);
        }
    }

    if (dest && gfnews->news) {
        struct news_folder *folder = gfnews->news;
        const char *parent_path = target ? hx_news_node_path (target) : "/";
        for (i = 0; i < folder->num_entries; i++) {
            struct folder_item *item = folder->entry[i];
            int kind = (item->type == 1) ? NB_KIND_FOLDER : NB_KIND_CATEGORY;
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
    if (!group || group->post_count <= 0) {
        return;
    }
    gsize count = (gsize) group->post_count;

    /* Marshal the posts into the flat repr(C) array the Rust tree builder
     * takes. hx_news_build_category_tree (hxnews-model) owns all of it now:
     * node creation, reply-threading, and the two-pass append ordering that
     * keeps GtkTreeListModel from fixing a parent as a leaf before its reply
     * subtree exists. The const char* fields are borrowed for the call — Rust
     * copies them into the nodes and applies the defaults. */
    struct hx_news_post_data *data = g_new0 (struct hx_news_post_data, count);
    for (gsize i = 0; i < count; i++) {
        struct news_item *it = &group->posts[i];
        data[i].postid = it->postid;
        data[i].parentid = it->parentid;
        data[i].subject = it->subject;
        data[i].sender = it->sender;
        data[i].mime_type
            = (it->partcount > 0 && it->parts && it->parts[0].mime_type)
                  ? it->parts[0].mime_type
                  : NULL;
        data[i].date = it->date;
    }
    hx_news_build_category_tree (dest, category_path, data, count);
    g_free (data);
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
    if (!group) {
        return;
    }
    if (group->posts) {
        for (i = 0; i < group->post_count; i++) {
            struct news_item *it = &group->posts[i];
            g_free (it->subject);
            g_free (it->sender);
            if (it->parts) {
                int j;
                for (j = 0; j < it->partcount; j++) {
                    g_free (it->parts[j].mime_type);
                }
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

    if (!pending_catlists) {
        return FALSE;
    }
    if (!g_hash_table_contains (pending_catlists, gcnews)) {
        return FALSE;
    }

    target = g_hash_table_lookup (pending_catlists, gcnews);
    if (target) {
        g_object_ref (target);
    }
    g_hash_table_remove (pending_catlists, gcnews);

    GListStore *ch = target ? hx_news_node_children (target) : NULL;
    if (br && ch && gcnews->group) {
        catlist_thread_into (ch, gcnews->group, hx_news_node_path (target));
    }

    /* Free the parsed group + the stub. */
    news_group_free (gcnews->group);
    g_free (gcnews->path);
    g_free (gcnews);

    g_clear_object (&target);
    return TRUE;
}

/* ---------- Post body fetch + display ---------- */

/* Format the post date as a human-friendly string. Auto-detects the
 * Mac 1904 epoch vs. modern wire format via hl_date_decode — same
 * helper rcv.c uses for file get-info timestamps. See
 * Capabilities.md "Date Format Selection" for why servers serve
 * different formats per-client. */
static char *
post_date_format (const struct date_time *dt)
{
    /* Pack the struct date_time back into the 8-byte wire layout
	 * hl_date_decode expects (year:2 / pad:2 / seconds:4, big-
	 * endian). The struct was already parsed off the wire — we're
	 * round-tripping the bytes here rather than maintaining a
	 * parallel decoder. */
    guint8 buf[8];
    buf[0] = (guint8) (dt->base_year >> 8);
    buf[1] = (guint8) (dt->base_year & 0xff);
    buf[2] = (guint8) (dt->pad >> 8);
    buf[3] = (guint8) (dt->pad & 0xff);
    buf[4] = (guint8) (dt->seconds >> 24);
    buf[5] = (guint8) (dt->seconds >> 16);
    buf[6] = (guint8) (dt->seconds >> 8);
    buf[7] = (guint8) (dt->seconds & 0xff);

    time_t t;
    if (!hl_date_decode (buf, &t)) {
        return g_strdup ("");
    }

    struct tm tm_buf;
    if (!localtime_r (&t, &tm_buf)) {
        return g_strdup ("");
    }

    char out[64];
    if (strftime (out, sizeof out, "%a %b %e %H:%M:%S %Y", &tm_buf) == 0) {
        return g_strdup ("");
    }
    return g_strdup (out);
}

/* Issue HTLC_HDR_GETTHREAD for `target`. The legacy hx_news15_get_post
 * helper takes a struct news_item — build a stub one with just the
 * fields it dereferences (postid, group->path, parts[0].mime_type)
 * and register it in pending_threads so the reply routes back to us. */
static void
fetch_thread (gnews_browser *br, HxNewsNode *target)
{
    struct news_item *stub_item;
    struct news_group *stub_group;
    (void)br;

    if (!target || hx_news_node_kind (target) != NB_KIND_POST
        || !hx_news_node_path (target)) {
        return;
    }
    if (hx_news_node_body_fetching (target)) {
        return; /* already in flight */
    }

    ensure_pending_tables ();

    stub_group = g_malloc0 (sizeof (struct news_group));
    stub_group->path = g_strdup (hx_news_node_path (target));

    stub_item = g_malloc0 (sizeof (struct news_item));
    stub_item->postid = hx_news_node_postid (target);
    stub_item->group = stub_group;
    stub_item->partcount = 1;
    stub_item->parts = g_malloc0 (sizeof (struct news_parts));
    {
        const char *mt = hx_news_node_mime_type (target);
        stub_item->parts[0].mime_type = g_strdup (mt ? mt : "text/plain");
    }

    g_hash_table_insert (pending_threads, stub_item, g_object_ref (target));
    hx_news_node_set_body_fetching (target, TRUE);

    hx_news15_get_post (&hx_active_session ()->htlc, stub_item);
}

/* Render the currently-selected post into the right pane. If the
 * body hasn't been fetched yet, fires fetch_thread and shows a
 * "Loading…" placeholder until the reply lands. */
static void
render_selected_post (gnews_browser *br)
{
    HxNewsNode *node = br->selected_post;
    GtkTextBuffer *buf;

    if (!node || hx_news_node_kind (node) != NB_KIND_POST) {
        gtk_widget_set_visible (br->header_strip, FALSE);
        buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (br->post_view));
        gtk_text_buffer_set_text (
            buf, _ ("Select a post in the tree to view it here."), -1);
        gtkurl_textview_apply_tags (GTK_TEXT_VIEW (br->post_view));
        return;
    }

    /* Header strip */
    {
        const char *nm = hx_news_node_name (node);
        gtk_label_set_text (br->subject_label,
                            nm && *nm ? nm : _ ("(no subject)"));
    }
    {
        struct date_time dt;
        const char *snd = hx_news_node_sender (node);
        hx_news_node_get_date (node, &dt);
        char *date_str = post_date_format (&dt);
        char *meta = g_strdup_printf (_ ("%1$s — %2$s"),
                                      snd && *snd ? snd : "?", date_str);
        gtk_label_set_text (br->meta_label, meta);
        g_free (meta);
        g_free (date_str);
    }
    gtk_widget_set_visible (br->header_strip, TRUE);

    /* Body */
    buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (br->post_view));
    {
        const char *body = hx_news_node_body (node);
        if (body) {
            gtk_text_buffer_set_text (buf, body, -1);
        } else {
            gtk_text_buffer_set_text (buf, _ ("Loading…"), -1);
            fetch_thread (br, node);
        }
    }
    /* Tag URLs (http://, https://, hotline://, mailto:, etc.) so the
	 * hover-cursor + right-click popup wired by gtkurl_textview_install
	 * has something to anchor on. */
    gtkurl_textview_apply_tags (GTK_TEXT_VIEW (br->post_view));
}

gboolean
gnews_browser_handle_thread (gpointer post_p)
{
    struct news_post *post = post_p;
    struct news_item *stub_item;
    HxNewsNode *target = NULL;
    gnews_browser *br = the_browser;

    if (!post || !pending_threads) {
        return FALSE;
    }
    stub_item = post->item;
    if (!stub_item) {
        return FALSE;
    }
    if (!g_hash_table_contains (pending_threads, stub_item)) {
        return FALSE;
    }

    target = g_hash_table_lookup (pending_threads, stub_item);
    if (target) {
        g_object_ref (target);
    }
    g_hash_table_remove (pending_threads, stub_item);

    if (target) {
        hx_news_node_set_body_fetching (target, FALSE);
        hx_news_node_set_body (target, post->buf ? post->buf : "");

        /* If the post is still the selected one, push the body
		 * into the view. (User may have moved on while the fetch
		 * was in flight — in that case we just keep the cached
		 * body for when they come back.) */
        if (br && br->selected_post == target) {
            render_selected_post (br);
        }
    }

    /* Free the stub news_item + stub group + the news_post. */
    if (stub_item->parts) {
        int j;
        for (j = 0; j < stub_item->partcount; j++) {
            g_free (stub_item->parts[j].mime_type);
        }
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

/* ---------- Header-bar action buttons ---------- */

/* Get the leaf node of the current selection, or NULL if nothing
 * is selected. Caller does NOT own a ref. */
static HxNewsNode *
selected_node (gnews_browser *br)
{
    guint pos = gtk_single_selection_get_selected (br->selection);
    GtkTreeListRow *row;
    HxNewsNode *node;

    if (pos == GTK_INVALID_LIST_POSITION) {
        return NULL;
    }
    row = g_list_model_get_item (G_LIST_MODEL (br->tree_model), pos);
    if (!row) {
        return NULL;
    }
    node = gtk_tree_list_row_get_item (row);
    g_object_unref (row);
    if (node) {
        g_object_unref (node); /* the GListStore still holds a ref */
    }
    return node;
}

/* Clear `node`'s children store + reset loaded flag + refire the
 * appropriate fetch. Used for the Refresh button and as the
 * "settle" step after a create / delete RPC so the user sees the
 * updated listing without manually re-clicking. */
static void
refresh_node (gnews_browser *br, HxNewsNode *node)
{
    if (!node) {
        /* Root refresh. */
        g_list_store_remove_all (br->root_store);
        fetch_dirlist (br, NULL);
        return;
    }
    {
        GListStore *ch = hx_news_node_children (node);
        if (ch) {
            g_list_store_remove_all (ch);
        }
    }
    hx_news_node_set_loaded (node, FALSE);
    if (hx_news_node_kind (node) == NB_KIND_FOLDER) {
        fetch_dirlist (br, node);
    } else if (hx_news_node_kind (node) == NB_KIND_CATEGORY) {
        fetch_catlist (br, node);
    }
}

static void
on_refresh_clicked (GtkButton *btn, gpointer user_data)
{
    gnews_browser *br = user_data;
    HxNewsNode *node = selected_node (br);
    (void)btn;

    /* If a post is selected: refetch the body. Otherwise refresh
	 * the selected folder / category — or the root, if nothing
	 * (or only the root level) is selected. */
    if (node && hx_news_node_kind (node) == NB_KIND_POST) {
        hx_news_node_set_body (node, NULL);
        hx_news_node_set_body_fetching (node, FALSE);
        render_selected_post (br);
        return;
    }
    if (node
        && (hx_news_node_kind (node) == NB_KIND_FOLDER
            || hx_news_node_kind (node) == NB_KIND_CATEGORY)) {
        refresh_node (br, node);
    } else {
        refresh_node (br, NULL);
    }
}

/* New Folder / New Category — the toolbar handlers pick the target and hand
 * off to the Rust create dialog (gtkhx-ui news_dialogs.rs), which owns the
 * name prompt, the mkdir / mkcat send, and the refresh. */

void
gtkhx_news_browser_refresh (HxNewsNode *node)
{
    /* Called from the Rust create / delete dialogs once the RPC is away. */
    if (the_browser) {
        refresh_node (the_browser, node);
    }
}

static void
on_new_folder_clicked (GtkButton *btn, gpointer user_data)
{
    gnews_browser *br = user_data;
    HxNewsNode *sel = selected_node (br);
    (void)btn;
    /* If a folder is selected, create inside it. Otherwise (no
     * selection, category, or post selected) create at the root. */
    gtkhx_news_create_dialog_open (
        br->window,
        (sel && hx_news_node_kind (sel) == NB_KIND_FOLDER) ? sel : NULL,
        NB_KIND_FOLDER);
}

static void
on_new_category_clicked (GtkButton *btn, gpointer user_data)
{
    gnews_browser *br = user_data;
    HxNewsNode *sel = selected_node (br);
    (void)btn;
    gtkhx_news_create_dialog_open (
        br->window,
        (sel && hx_news_node_kind (sel) == NB_KIND_FOLDER) ? sel : NULL,
        NB_KIND_CATEGORY);
}

/* Delete — snapshot the target's identity (kind, name, path, postid) at click
 * time and hand it to the Rust confirm dialog (which copies the strings
 * synchronously before deferring). Snapshotting rather than holding an
 * HxNewsNode ref avoids a use-after-clear: a held ref keeps the node alive but
 * doesn't stop the GListStore dropping its ref during a refresh or collapse,
 * which clears the node's path pointer in flight (\u2192 NULL to path_to_hldir,
 * crash). Once the user clicks Delete the intent is fixed regardless of what
 * happens to the tree before they confirm. */
static void
on_delete_clicked (GtkButton *btn, gpointer user_data)
{
    gnews_browser *br = user_data;
    HxNewsNode *sel = selected_node (br);
    (void)btn;

    if (!sel || !hx_news_node_path (sel)) {
        return;
    }

    gtkhx_news_delete_dialog_open (br->window, hx_news_node_kind (sel),
                                   hx_news_node_name (sel),
                                   hx_news_node_path (sel),
                                   hx_news_node_postid (sel));
}

/* ---------- Compose window (shared by New Post and Reply) ----------
 *
 * The user fires either New Post (parent_postid = 0) or Reply (parent
 * postid taken from the selected post). Both open the same compose
 * window — subject entry, body text view, Post + Cancel. On Post:
 *   1. hx_news15_post_thread on the wire
 *   2. refresh the containing category so the new post appears
 *
 * The category path comes from the selected node (for a post: its
 * containing-category path, which is what HxNewsNode->path already
 * holds; for a category: the category's own path). */

struct compose_ctx {
    gnews_browser *br;
    char *category_path; /* owned */
    guint32 parent_postid;
    GtkWidget *window;
    GtkWidget *subject_entry;
    GtkWidget *body_view;
};

static void
compose_ctx_free (struct compose_ctx *ctx)
{
    if (!ctx) {
        return;
    }
    g_free (ctx->category_path);
    g_free (ctx);
}

/* Find the HxNewsNode that owns the category path, walking the
 * existing tree only — we don't fetch on miss. Returns a reffed
 * pointer (caller must g_object_unref) or NULL if the category
 * isn't currently loaded into the tree (in which case the caller
 * falls back to a root refresh, which is heavier but always
 * works). */
static HxNewsNode *
find_category_node (GListStore *store, const char *path)
{
    guint n, i;
    if (!store || !path) {
        return NULL;
    }
    n = g_list_model_get_n_items (G_LIST_MODEL (store));
    for (i = 0; i < n; i++) {
        HxNewsNode *node = g_list_model_get_item (G_LIST_MODEL (store), i);
        /* g_list_model_get_item gave us one ref. The match branch
		 * transfers that ref to the caller; the recursive branch
		 * gets its own ref from the inner call and drops the
		 * outer-folder ref here; the no-match branch just drops
		 * the ref before continuing. */
        if (hx_news_node_kind (node) == NB_KIND_CATEGORY
            && g_strcmp0 (hx_news_node_path (node), path) == 0) {
            return node;
        }
        if (hx_news_node_kind (node) == NB_KIND_FOLDER) {
            GListStore *ch = hx_news_node_children (node);
            if (ch) {
                HxNewsNode *hit = find_category_node (ch, path);
                if (hit) {
                    g_object_unref (node);
                    return hit;
                }
            }
        }
        g_object_unref (node);
    }
    return NULL;
}

static void
compose_do_post (GtkButton *btn, gpointer user_data)
{
    struct compose_ctx *ctx = user_data;
    const char *subject;
    GtkTextBuffer *buf;
    GtkTextIter a, b;
    char *body;
    HxNewsNode *cat;
    (void)btn;

    subject = gtk_editable_get_text (GTK_EDITABLE (ctx->subject_entry));
    if (!subject) {
        subject = "";
    }

    buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (ctx->body_view));
    gtk_text_buffer_get_start_iter (buf, &a);
    gtk_text_buffer_get_end_iter (buf, &b);
    body = gtk_text_buffer_get_text (buf, &a, &b, FALSE);

    hx_news15_post_thread (&hx_active_session ()->htlc, ctx->category_path, subject,
                           ctx->parent_postid, body ? body : (char *)"");

    g_free (body);

    /* Settle: refresh just the affected category if it's still in
	 * the tree, otherwise the whole root. */
    if (the_browser) {
        cat = find_category_node (the_browser->root_store, ctx->category_path);
        if (cat) {
            refresh_node (the_browser, cat);
            g_object_unref (cat);
        } else {
            refresh_node (the_browser, NULL);
        }
    }

    gtk_window_destroy (GTK_WINDOW (ctx->window));
}

static void
compose_cancel (GtkButton *btn, gpointer user_data)
{
    struct compose_ctx *ctx = user_data;
    (void)btn;
    gtk_window_destroy (GTK_WINDOW (ctx->window));
}

static void
compose_window_closed (GtkWindow *win, gpointer user_data)
{
    struct compose_ctx *ctx = user_data;
    (void)win;
    compose_ctx_free (ctx);
}

/* Build the "Replying to ..." context strip — sender + date line,
 * subject line, then a scrollable preview of the original body.
 * Returned widget is the container; caller boxes it above the
 * compose form. `reply_to` must have kind == NB_KIND_POST. */
static GtkWidget *
build_reply_context_panel (HxNewsNode *reply_to)
{
    GtkWidget *outer, *header_row, *meta_lbl, *subj_lbl;
    GtkWidget *body_scroll, *body_view;
    GtkTextBuffer *buf;
    char *meta;
    char *date_str;
    const char *body_text;

    outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class (outer, "card");
    gtk_widget_set_margin_start (outer, 12);
    gtk_widget_set_margin_end (outer, 12);
    gtk_widget_set_margin_top (outer, 10);
    gtk_widget_set_margin_bottom (outer, 4);

    /* Header row: pinned mini-label + sender/date.
	 *
	 * Compact "From <sender> on <date>" line matches the post-pane
	 * format in the main browser, so the user reads it the same
	 * way in both places. */
    header_row = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start (header_row, 8);
    gtk_widget_set_margin_end (header_row, 8);
    gtk_widget_set_margin_top (header_row, 6);

    struct date_time reply_dt;
    hx_news_node_get_date (reply_to, &reply_dt);
    date_str = post_date_format (&reply_dt);
    const char *reply_sender = hx_news_node_sender (reply_to);
    meta = g_strdup_printf (
        _ ("Replying to %1$s — %2$s"),
        reply_sender && *reply_sender ? reply_sender : "?", date_str);
    meta_lbl = gtk_label_new (meta);
    gtk_label_set_xalign (GTK_LABEL (meta_lbl), 0.0f);
    gtk_label_set_ellipsize (GTK_LABEL (meta_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class (meta_lbl, "dim-label");
    gtk_widget_add_css_class (meta_lbl, "caption");
    g_free (meta);
    g_free (date_str);

    const char *reply_name = hx_news_node_name (reply_to);
    subj_lbl = gtk_label_new (reply_name && *reply_name
                                  ? reply_name
                                  : _ ("(no subject)"));
    gtk_label_set_xalign (GTK_LABEL (subj_lbl), 0.0f);
    gtk_label_set_wrap (GTK_LABEL (subj_lbl), TRUE);
    gtk_label_set_wrap_mode (GTK_LABEL (subj_lbl), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class (subj_lbl, "heading");

    gtk_box_append (GTK_BOX (header_row), meta_lbl);
    gtk_box_append (GTK_BOX (header_row), subj_lbl);
    gtk_box_append (GTK_BOX (outer), header_row);

    /* Body preview (scrollable, capped height so the panel doesn't
	 * crowd out the user's reply box on a long original). */
    body_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (body_scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height (
        GTK_SCROLLED_WINDOW (body_scroll), 140);
    gtk_scrolled_window_set_propagate_natural_height (
        GTK_SCROLLED_WINDOW (body_scroll), TRUE);

    body_view = gtk_text_view_new ();
    gtk_text_view_set_editable (GTK_TEXT_VIEW (body_view), FALSE);
    gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (body_view), FALSE);
    gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (body_view), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_margin_start (body_view, 8);
    gtk_widget_set_margin_end (body_view, 8);
    gtk_widget_set_margin_top (body_view, 2);
    gtk_widget_set_margin_bottom (body_view, 6);
    gtk_widget_add_css_class (body_view, "dim-label");

    buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (body_view));
    /* If we don't have the body cached, the user just clicked Reply
	 * before the GETTHREAD reply landed. Rather than block, render
	 * a placeholder — the reply can still be composed; the user has
	 * the subject + sender + date for context. */
    const char *reply_body = hx_news_node_body (reply_to);
    body_text = reply_body ? reply_body
                           : _ ("(original post body not loaded — open the "
                                "post first to fetch it)");
    gtk_text_buffer_set_text (buf, body_text, -1);

    gtkhx_widget_set_child (body_scroll, body_view);
    gtk_box_append (GTK_BOX (outer), body_scroll);

    return outer;
}

/* Open a compose window. `reply_to` NULL = new post; non-NULL =
 * reply (must be kind == NB_KIND_POST; the parent_postid + reply
 * context panel come from the node). `prefill_subject` is the
 * initial subject text. */
static void
open_compose_window (gnews_browser *br, const char *category_path,
                     HxNewsNode *reply_to, const char *prefill_subject)
{
    struct compose_ctx *ctx;
    GtkWidget *window, *header, *content, *form, *body_scroll;
    GtkWidget *subject_row, *subject_lbl;
    GtkWidget *cancel_btn, *post_btn;
    guint32 parent_postid = reply_to ? hx_news_node_postid (reply_to) : 0;

    if (!category_path) {
        return;
    }

    ctx = g_new0 (struct compose_ctx, 1);
    ctx->br = br;
    ctx->category_path = g_strdup (category_path);
    ctx->parent_postid = parent_postid;

    window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (window),
                          reply_to ? _ ("Reply") : _ ("New Post"));
    gtk_widget_set_size_request (window, 560, reply_to ? 540 : 380);
    /* br->window is a PanelWidget,
     * not a GtkWindow. Transient-for the toolbar window which now
     * hosts the panel — same target every other panel uses for
     * sub-dialog parenting. */
    gtk_window_set_transient_for (GTK_WINDOW (window),
                                  GTK_WINDOW (toolbar_window));
    gtk_window_set_modal (GTK_WINDOW (window), TRUE);
    ctx->window = window;

    /* Header bar with Cancel (left) + Post (right). */
    header = adw_header_bar_new ();
    adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header),
                                                 FALSE);
    adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);

    cancel_btn = gtk_button_new_with_mnemonic (_ ("_Cancel"));
    post_btn = gtk_button_new_with_mnemonic (_ ("_Post"));
    gtk_widget_add_css_class (post_btn, "suggested-action");

    g_signal_connect (cancel_btn, "clicked", G_CALLBACK (compose_cancel), ctx);
    g_signal_connect (post_btn, "clicked", G_CALLBACK (compose_do_post), ctx);

    adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel_btn);
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), post_btn);
    gtk_window_set_titlebar (GTK_WINDOW (window), header);

    /* Layout: optional context panel + subject row + reply body. */
    content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    if (reply_to && hx_news_node_kind (reply_to) == NB_KIND_POST) {
        GtkWidget *ctx_panel = build_reply_context_panel (reply_to);
        gtkhx_box_pack (content, ctx_panel, FALSE, FALSE, 0);
    }

    form = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start (form, 12);
    gtk_widget_set_margin_end (form, 12);
    gtk_widget_set_margin_top (form, 10);
    gtk_widget_set_margin_bottom (form, 6);

    subject_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    subject_lbl = gtk_label_new (_ ("Subject:"));
    gtk_label_set_xalign (GTK_LABEL (subject_lbl), 0.0f);
    ctx->subject_entry = gtk_entry_new ();
    gtk_entry_set_activates_default (GTK_ENTRY (ctx->subject_entry), FALSE);
    gtk_widget_set_hexpand (ctx->subject_entry, TRUE);
    gtk_editable_set_text (GTK_EDITABLE (ctx->subject_entry),
                           prefill_subject ? prefill_subject : "");
    gtk_box_append (GTK_BOX (subject_row), subject_lbl);
    gtk_box_append (GTK_BOX (subject_row), ctx->subject_entry);

    gtk_box_append (GTK_BOX (form), subject_row);
    gtkhx_box_pack (content, form, FALSE, FALSE, 0);

    body_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (body_scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    ctx->body_view = gtk_text_view_new ();
    gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (ctx->body_view),
                                 GTK_WRAP_WORD_CHAR);
    gtk_widget_set_margin_start (ctx->body_view, 4);
    gtk_widget_set_margin_end (ctx->body_view, 4);
    gtk_widget_set_margin_top (ctx->body_view, 4);
    gtk_widget_set_margin_bottom (ctx->body_view, 4);
    /* Compose surface — the user types the post body here. Themed
     * via .gtkhx-input so the editor matches the chat / PM inputs. */
    gtkhx_apply_input_style (ctx->body_view);
    gtkhx_widget_set_child (body_scroll, ctx->body_view);
    gtkhx_box_pack (content, body_scroll, TRUE, TRUE, 0);

    gtkhx_widget_set_child (window, content);

    g_signal_connect (window, "destroy", G_CALLBACK (compose_window_closed),
                      ctx);

    gtk_window_present (GTK_WINDOW (window));
    /* Focus the subject for new posts, body for replies (the
	 * subject is already prefilled "Re: …" — the user usually just
	 * wants to start typing). */
    if (reply_to) {
        gtk_widget_grab_focus (ctx->body_view);
    } else {
        gtk_widget_grab_focus (ctx->subject_entry);
    }
}

static void
on_new_post_clicked (GtkButton *btn, gpointer user_data)
{
    gnews_browser *br = user_data;
    HxNewsNode *sel = selected_node (br);
    const char *cat_path = NULL;
    (void)btn;

    /* Need a category to post into. Selected category → its path.
	 * Selected post → its containing-category path (already what
	 * HxNewsNode->path stores for posts). Anything else (folder,
	 * nothing) → can't post; the button shouldn't have been
	 * visible, but be defensive. */
    if (!sel) {
        return;
    }
    int sel_kind = hx_news_node_kind (sel);
    if (sel_kind == NB_KIND_CATEGORY || sel_kind == NB_KIND_POST) {
        cat_path = hx_news_node_path (sel);
    }
    if (!cat_path) {
        return;
    }

    open_compose_window (br, cat_path, NULL, "");
}

static void
on_reply_clicked (GtkButton *btn, gpointer user_data)
{
    gnews_browser *br = user_data;
    HxNewsNode *sel = selected_node (br);
    char *subj;
    (void)btn;

    if (!sel || hx_news_node_kind (sel) != NB_KIND_POST
        || !hx_news_node_path (sel)) {
        return;
    }

    /* Prefill with "Re: <original>", but only if the original
	 * doesn't already start with "Re:" — avoid Re: Re: Re: chains
	 * the way every mail client has for decades. */
    const char *sel_name = hx_news_node_name (sel);
    if (sel_name && g_ascii_strncasecmp (sel_name, "Re:", 3) == 0) {
        subj = g_strdup (sel_name);
    } else {
        subj = g_strdup_printf ("Re: %s", sel_name ? sel_name : "");
    }

    /* Make sure the original body is loaded before opening compose —
	 * if the user clicked Reply before the GETTHREAD reply landed,
	 * the context panel renders a "(not loaded)" placeholder, which
	 * is fine but ugly. Firing the fetch here costs nothing (the
	 * helper no-ops if already in flight or cached). */
    if (!hx_news_node_body (sel) && !hx_news_node_body_fetching (sel)) {
        fetch_thread (br, sel);
    }

    open_compose_window (br, hx_news_node_path (sel), sel, subj);
    g_free (subj);
}

/* Toggle action-button visibility + sensitivity.
 *
 * Visibility is selection-driven:
 *   no selection / root → New Folder, New Category
 *   folder              → New Folder, New Category, Delete
 *   category            → New Post, Delete
 *   post                → New Post, Reply, Delete
 *   (Refresh is always visible.)
 *
 * Sensitivity is access-bit-driven from htlc->access. Buttons stay
 * visible when the action is forbidden by the server but become
 * grey + unclickable. This gives the user a clearer signal than
 * silently hiding ("I know this exists; the server says no") and
 * avoids the toolbar reshuffling every time selection changes a
 * permission boundary. */
static void
sync_action_buttons (gnews_browser *br)
{
    HxNewsNode *node = selected_node (br);
    int kind = node ? hx_news_node_kind (node) : 0;
    const guint8 *access = (const guint8 *)&hx_active_session ()->htlc.access;
    int delete_bit;

    /* Visibility */
    gtk_widget_set_visible (br->btn_new_folder,
                            kind == 0 || kind == NB_KIND_FOLDER);
    gtk_widget_set_visible (br->btn_new_category,
                            kind == 0 || kind == NB_KIND_FOLDER);
    gtk_widget_set_visible (br->btn_new_post,
                            kind == NB_KIND_CATEGORY || kind == NB_KIND_POST);
    gtk_widget_set_visible (br->btn_reply, kind == NB_KIND_POST);
    gtk_widget_set_visible (br->btn_delete, kind == NB_KIND_FOLDER
                                                || kind == NB_KIND_CATEGORY
                                                || kind == NB_KIND_POST);

    /* Sensitivity: per-action access bit. The delete bit depends
	 * on what kind of node is selected — folder vs. category vs.
	 * post each carry separate bits in the bitmap. */
    gtk_widget_set_sensitive (
        br->btn_new_folder,
        hl_access_has (access, HL_ACCESS_CREATE_NEWS_BUNDLES));
    gtk_widget_set_sensitive (
        br->btn_new_category,
        hl_access_has (access, HL_ACCESS_CREATE_CATEGORIES));
    gtk_widget_set_sensitive (br->btn_new_post,
                              hl_access_has (access, HL_ACCESS_POST_NEWS));
    gtk_widget_set_sensitive (br->btn_reply,
                              hl_access_has (access, HL_ACCESS_POST_NEWS));

    switch (kind) {
    case NB_KIND_FOLDER:
        delete_bit = HL_ACCESS_DELETE_NEWS_BUNDLES;
        break;
    case NB_KIND_CATEGORY:
        delete_bit = HL_ACCESS_DELETE_CATEGORIES;
        break;
    case NB_KIND_POST:
        delete_bit = HL_ACCESS_DELETE_ARTICLES;
        break;
    default:
        delete_bit = -1;
        break;
    }
    gtk_widget_set_sensitive (
        br->btn_delete, delete_bit >= 0 && hl_access_has (access, delete_bit));
}

/* ---------- Connection-state handling ---------- */

/* Reset the browser back to a freshly-built state. Used on
 * DISCONNECTED so the next connection starts from a clean slate
 * instead of inheriting the previous server's tree, and avoids
 * showing stale content when the user is no longer logged in. */
static void
reset_browser_state (gnews_browser *br)
{
    GtkTextBuffer *buf;

    if (!br) {
        return;
    }

    /* Drop every node — both the top-level folders and (by
     * GListStore ownership) every descendant store cached on
     * those nodes. Pending fetches that come back later for
     * stubs we kept hashes on are no-ops: the carrier struct
     * is freed by the matching code and the ref drops. */
    if (br->root_store) {
        g_list_store_remove_all (br->root_store);
    }
    br->selected_post = NULL;

    /* Reset the right pane to the empty-selection prompt. */
    if (br->post_view) {
        buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (br->post_view));
        gtk_text_buffer_set_text (
            buf, _ ("Select a post in the tree to view it here."), -1);
        gtkurl_textview_apply_tags (GTK_TEXT_VIEW (br->post_view));
    }
    if (br->header_strip) {
        gtk_widget_set_visible (br->header_strip, FALSE);
    }

    /* Breadcrumb back to the root marker. */
    if (br->breadcrumb) {
        gtk_label_set_text (br->breadcrumb, "/");
    }

    /* sync_action_buttons keys off htlc->access. Calling it here
     * with the disconnected (zero) access bitmap leaves every
     * button greyed but visible-where-appropriate, matching the
     * "logged out" rendering. */
    sync_action_buttons (br);
}

/* Connection state changed on the GtkhxSession singleton. */
static void
on_connection_state (GtkhxSession *sess, guint state, gpointer user_data)
{
    gnews_browser *br = user_data;
    (void)sess;

    switch (state) {
    case GTKHX_CONNECTION_DISCONNECTED:
        reset_browser_state (br);
        if (br->disconnected_banner) {
            adw_banner_set_revealed (br->disconnected_banner, TRUE);
        }
        break;

    case GTKHX_CONNECTION_LOGIN_READY:
        if (br->disconnected_banner) {
            adw_banner_set_revealed (br->disconnected_banner, FALSE);
        }
        /* Fire the initial NEWSDIRLIST so the user sees content
         * without having to switch to the tab or hit Refresh.
         * Skipping when the tree is non-empty avoids clobbering
         * an in-progress fetch race (the LOGIN_READY → fetch
         * here vs. the panel-presented → fetch below firing back-
         * to-back when the user logs in with News already
         * focused). */
        if (br->root_store
            && g_list_model_get_n_items (G_LIST_MODEL (br->root_store)) == 0) {
            fetch_dirlist (br, NULL);
        }
        break;

    default:
        break;
    }
}

/* PanelWidget::presented — fires every time the panel is brought
 * to the foreground (tab clicked, panel_widget_raise, undock /
 * redock). Mirrors the open_news_browser-on-toolbar-click path so
 * a user who clicks the news tab while connected and empty also
 * gets an auto-fetch. */
static void
on_panel_presented (PanelWidget *panel, gpointer user_data)
{
    gnews_browser *br = user_data;
    (void)panel;

    if (!connected || !br || !br->root_store) {
        return;
    }
    if (g_list_model_get_n_items (G_LIST_MODEL (br->root_store)) == 0) {
        fetch_dirlist (br, NULL);
    }
}

/* ---------- Window lifecycle ---------- */

/* kept defined for the void-cast at
 * the bottom of build_browser_window. Was wired to close-request
 * on the standalone GtkWindow; the HxPanel persists across opens
 * and uses libpanel's own close-page machinery instead. Phase 4
 * layout teardown may grow a real destroy path here. */
static gboolean
on_window_close (GtkWindow *window, gpointer user_data)
{
    gnews_browser *br = user_data;
    (void)window;

    if (the_browser == br) {
        the_browser = NULL;
    }

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

/* Teardown when the content box is destroyed. The normal case is app exit;
 * the important case is a failed dock embed — dock_bridge destroys the content
 * it was handed, and without this the session-level connection-state handler
 * would keep firing on_connection_state into a dead browser (a UAF), and the
 * process singleton the_browser would dangle at a freed widget tree. Disconnect
 * the handler and drop the singleton. br itself is deliberately NOT freed here:
 * this runs at the *start* of the content box's destruction, so the child
 * teardown still to come can run factory callbacks that read br. That leaves br
 * leaked only on the should-never-happen "toolbar dock not built" embed
 * failure — an acceptable trade for not risking a destroy-order UAF. */
static void
news_browser_content_destroyed (GtkWidget *w, gpointer user_data)
{
    gnews_browser *br = user_data;
    (void)w;
    if (br->conn_state_handler) {
        g_signal_handler_disconnect (gtkhx_session_get_default (),
                                     br->conn_state_handler);
        br->conn_state_handler = 0;
    }
    if (the_browser == br) {
        the_browser = NULL;
    }
}

/* Content build for the Rust News-browser shell (gtkhx-ui `news_browser`).
 * The dock registration moved to Rust via dock_bridge; this builds the whole
 * gnews_browser + its content tree and returns the content box. Unlike the
 * other docked windows the browser integrates the panel as its window
 * object, so br->window points at content_vbox (a widget in the panel's tree
 * once embedded — enough for dialog parenting / root-walking / keyaccel) and
 * the one genuinely panel-level hook (PanelWidget::presented) is wired in
 * gtkhx_news_browser_after_embed once the panel exists. */
GtkWidget *
gtkhx_news_browser_build_content (void)
{
    gnews_browser *br = g_new0 (gnews_browser, 1);
    GtkWidget *paned, *left_scroll, *right_box, *right_scroll;
    GtkWidget *header_title;
    GtkWidget *content_vbox;
    GtkListItemFactory *factory;
    GtkTextBuffer *buf;

    the_browser = br;

    /* ---- Icons (cached for the lifetime of the window) ---- */
    br->icon_folder
        = load_icon_paintable ("/com/nasledov/gtkhx/pixmaps/news_folder.png");
    br->icon_category
        = load_icon_paintable ("/com/nasledov/gtkhx/pixmaps/news_category.png");
    br->icon_post
        = load_icon_paintable ("/com/nasledov/gtkhx/pixmaps/news_post.png");

    /* no standalone GtkWindow. The
     * browser content lives inside an HxPanel resident of the
     * toolbar's center PanelGrid. br->window points at the panel
     * widget so the dialog-parent calls (adw_dialog_present) keep
     * compiling; the one gtk_window_set_transient_for site is
     * fixed up below to use toolbar_window instead.
     *
     * Breadcrumb relocates to its own row in the panel content
     * since the panel header strip is taken by the tab title. */
    header_title = gtk_label_new ("/");
    gtk_widget_add_css_class (header_title, "heading");
    gtk_label_set_xalign (GTK_LABEL (header_title), 0.0f);
    gtk_widget_set_margin_start  (header_title, 12);
    gtk_widget_set_margin_end    (header_title, 12);
    gtk_widget_set_margin_top    (header_title, 4);
    gtk_widget_set_margin_bottom (header_title, 4);
    br->breadcrumb = GTK_LABEL (header_title);

    /* Header action buttons.
	 *
	 * pack_start: Refresh (always), New Folder + New Category
	 *             (visible when nothing or a folder is selected).
	 * pack_end:   Reply (post-only), Delete (folder/category/post).
	 *
	 * Visibility is driven by sync_action_buttons on every
	 * selection-changed; we mark each button gtk_widget_set_visible
	 * (FALSE) up front so the initial empty-tree state isn't
	 * cluttered. The buttons render their icons via the same
	 * gtkhx_pixmap_button helper the toolbar uses. */
    br->btn_refresh = gtkhx_pixmap_button (
        "/com/nasledov/gtkhx/pixmaps/refresh.png", _ ("Refresh"), GTKHX_SCALE_WINDOW_BUTTONS,
        G_CALLBACK (on_refresh_clicked), br);
    br->btn_new_folder = gtkhx_pixmap_button (
        "/com/nasledov/gtkhx/pixmaps/news_folder.png", _ ("New Folder"), GTKHX_SCALE_WINDOW_BUTTONS,
        G_CALLBACK (on_new_folder_clicked), br);
    br->btn_new_category = gtkhx_pixmap_button (
        "/com/nasledov/gtkhx/pixmaps/news_category.png", _ ("New Category"), GTKHX_SCALE_WINDOW_BUTTONS,
        G_CALLBACK (on_new_category_clicked), br);
    br->btn_new_post = gtkhx_pixmap_button (
        "/com/nasledov/gtkhx/pixmaps/pencil.png", _ ("New Post"), GTKHX_SCALE_WINDOW_BUTTONS,
        G_CALLBACK (on_new_post_clicked), br);
    br->btn_reply = gtkhx_pixmap_button (
        "/com/nasledov/gtkhx/pixmaps/post_news.png", _ ("Reply"), GTKHX_SCALE_WINDOW_BUTTONS,
        G_CALLBACK (on_reply_clicked), br);
    br->btn_delete = gtkhx_pixmap_button (
        "/com/nasledov/gtkhx/pixmaps/trash.png", _ ("Delete"), GTKHX_SCALE_WINDOW_BUTTONS,
        G_CALLBACK (on_delete_clicked), br);

    /* the AdwHeaderBar with Refresh /
     * NewFolder / NewCategory / NewPost on start and Reply /
     * Delete on end relocates to a slim top-of-content GtkBox,
     * matching the other Phase 2 migrations. */
    {
        GtkWidget *button_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_margin_start  (button_bar, 6);
        gtk_widget_set_margin_end    (button_bar, 6);
        gtk_widget_set_margin_top    (button_bar, 6);
        gtk_widget_set_margin_bottom (button_bar, 4);
        gtk_box_append (GTK_BOX (button_bar), br->btn_refresh);
        gtk_box_append (GTK_BOX (button_bar), br->btn_new_folder);
        gtk_box_append (GTK_BOX (button_bar), br->btn_new_category);
        gtk_box_append (GTK_BOX (button_bar), br->btn_new_post);
        {
            GtkWidget *spacer = gtk_label_new (NULL);
            gtk_widget_set_hexpand (spacer, TRUE);
            gtk_box_append (GTK_BOX (button_bar), spacer);
        }
        gtk_box_append (GTK_BOX (button_bar), br->btn_reply);
        gtk_box_append (GTK_BOX (button_bar), br->btn_delete);
        br->button_bar = button_bar;
    }

    /* ---- Two-pane body ---- */
    paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position (GTK_PANED (paned), 280);
    gtk_paned_set_resize_start_child (GTK_PANED (paned), FALSE);
    gtk_widget_set_hexpand (paned, TRUE);
    gtk_widget_set_vexpand (paned, TRUE);

    /* ---- Left: GtkListView over a GtkTreeListModel ---- */
    br->root_store = g_list_store_new (HX_TYPE_NEWS_NODE);
    br->tree_model = gtk_tree_list_model_new (
        G_LIST_MODEL (br->root_store), /* takes ownership of one ref */
        FALSE,                         /* passthrough — FALSE means
		                                    * the model items are
		                                    * GtkTreeListRow wrappers */
        FALSE,                         /* autoexpand */
        news_node_create_child_model, br, NULL);
    br->selection = gtk_single_selection_new (G_LIST_MODEL (br->tree_model));
    gtk_single_selection_set_autoselect (br->selection, FALSE);
    gtk_single_selection_set_can_unselect (br->selection, TRUE);

    factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (on_factory_setup), br);
    g_signal_connect (factory, "bind", G_CALLBACK (on_factory_bind), br);
    g_signal_connect (factory, "unbind", G_CALLBACK (on_factory_unbind), br);

    br->list_view
        = gtk_list_view_new (GTK_SELECTION_MODEL (br->selection), factory);
    gtk_list_view_set_show_separators (GTK_LIST_VIEW (br->list_view), FALSE);
    /* Follow the active GtkHx theme's fg/bg via .gtkhx-listview. */
    gtkhx_apply_listview_style (br->list_view);

    g_signal_connect (br->selection, "selection-changed",
                      G_CALLBACK (on_selection_changed), br);

    left_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (left_scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtkhx_widget_set_child (left_scroll, br->list_view);
    gtk_paned_set_start_child (GTK_PANED (paned), left_scroll);

    /* ---- Right: post header strip + body ----
	 *
	 *   header_strip (hidden until a post is selected)
	 *     subject_label  — "heading" CSS class
	 *     meta_label     — "<sender> — <date>", "dim-label" class
	 *   separator
	 *   body GtkTextView (scrolled, read-only) */
    right_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    br->header_strip = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start (br->header_strip, 12);
    gtk_widget_set_margin_end (br->header_strip, 12);
    gtk_widget_set_margin_top (br->header_strip, 10);
    gtk_widget_set_margin_bottom (br->header_strip, 8);

    {
        GtkWidget *subj = gtk_label_new (NULL);
        GtkWidget *meta = gtk_label_new (NULL);

        gtk_label_set_xalign (GTK_LABEL (subj), 0.0f);
        gtk_label_set_wrap (GTK_LABEL (subj), TRUE);
        gtk_label_set_wrap_mode (GTK_LABEL (subj), PANGO_WRAP_WORD_CHAR);
        gtk_widget_add_css_class (subj, "heading");

        gtk_label_set_xalign (GTK_LABEL (meta), 0.0f);
        gtk_label_set_ellipsize (GTK_LABEL (meta), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class (meta, "dim-label");

        gtk_box_append (GTK_BOX (br->header_strip), subj);
        gtk_box_append (GTK_BOX (br->header_strip), meta);

        br->subject_label = GTK_LABEL (subj);
        br->meta_label = GTK_LABEL (meta);
    }
    gtk_widget_set_visible (br->header_strip, FALSE);
    gtk_box_append (GTK_BOX (right_box), br->header_strip);
    gtk_box_append (GTK_BOX (right_box),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    right_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (right_scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    br->post_view = gtk_text_view_new ();
    gtk_text_view_set_editable (GTK_TEXT_VIEW (br->post_view), FALSE);
    gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (br->post_view), FALSE);
    gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (br->post_view),
                                 GTK_WRAP_WORD_CHAR);
    gtk_widget_set_margin_start (br->post_view, 12);
    gtk_widget_set_margin_end (br->post_view, 12);
    gtk_widget_set_margin_top (br->post_view, 10);
    gtk_widget_set_margin_bottom (br->post_view, 10);
    /* The selected post's body — primary read surface in the
     * threaded-news browser. Themed via .gtkhx-text so it matches
     * the news viewer and chat output. */
    gtkhx_apply_text_style (br->post_view);
    buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (br->post_view));
    gtk_text_buffer_set_text (
        buf, _ ("Select a post in the tree to view it here."), -1);
    /* Wire URL detection: hover/cursor handling + right-click popup,
	 * same treatment news.c gives its threaded-news pane. The
	 * gtkurl_textview_apply_tags calls in render_selected_post +
	 * gnews_browser_set_disconnected re-tag after each
	 * gtk_text_buffer_set_text. */
    gtkurl_textview_install (GTK_TEXT_VIEW (br->post_view));
    gtkhx_widget_set_child (right_scroll, br->post_view);
    gtkhx_box_pack (right_box, right_scroll, TRUE, TRUE, 0);
    gtk_paned_set_end_child (GTK_PANED (paned), right_box);

    /* assemble the panel content
     * vbox (button bar + breadcrumb + paned) and wrap it in an
     * HxPanel. on_window_close stays defined (see comment on the
     * function) but is no longer wired — the panel persists and
     * uses libpanel's own close-page machinery. */
    (void)on_window_close;

    /* Disconnected-state AdwBanner — sits above the button bar so
     * the action buttons stay below it (a banner that hides the
     * Refresh button would be confusing). Defaults to "not
     * connected" since the panel eager-constructs before any
     * connection; on_connection_state flips it off on LOGIN_READY
     * and back on at DISCONNECTED. */
    br->disconnected_banner = ADW_BANNER (
        adw_banner_new (_ ("Not connected to a server.")));
    adw_banner_set_revealed (br->disconnected_banner, !connected);

    content_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (content_vbox),
                    GTK_WIDGET (br->disconnected_banner));
    gtk_box_append (GTK_BOX (content_vbox), br->button_bar);
    gtk_box_append (GTK_BOX (content_vbox), header_title);
    gtk_box_append (GTK_BOX (content_vbox),
                    gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append (GTK_BOX (content_vbox), paned);

    /* Connection-state changes from any source drive the disconnected
     * banner + the auto-fetch on LOGIN_READY. */
    br->conn_state_handler = g_signal_connect (
        gtkhx_session_get_default (), "connection-state-changed",
        G_CALLBACK (on_connection_state), br);

    /* br->window points at the content box, not the dock panel (which the
     * Rust shell now owns). It only needs to be a widget in the panel's
     * window tree so adw_dialog_present parents, gtk_widget_get_root walks,
     * and the keyaccel controllers route — content_vbox is exactly that once
     * embedded. The one site that wanted a real GtkWindow (compose-window
     * transient-for) uses toolbar_window instead. */
    br->window = content_vbox;

    /* Initial state: no selection → New Folder + New Category visible
     * (operating at the root); Reply + Delete hidden. */
    sync_action_buttons (br);

    /* Clean up the session handler + singleton if this content is ever
     * destroyed (embed failure, or app exit) — see the handler above. */
    g_signal_connect (content_vbox, "destroy",
                      G_CALLBACK (news_browser_content_destroyed), br);

    return content_vbox;
}

void
gtkhx_news_browser_after_embed (void)
{
    /* The one panel-level hook: PanelWidget::presented, so a tab switch onto
     * News while connected + empty fires the initial NEWSDIRLIST without a
     * manual Refresh (the toolbar-button entry point covers its own path).
     * The panel is available from the registry once dock_bridge embedded us. */
    HxPanel *panel = hx_panel_registry_lookup (HX_PANEL_ID_NEWS15);
    if (panel != NULL && the_browser != NULL) {
        g_signal_connect (panel, "presented",
                          G_CALLBACK (on_panel_presented), the_browser);
    }
}

/* ---------- Entry point ---------- */

void
open_news_browser (GtkWidget *widget, struct _session *sess)
{
    /* Was the panel already up before this open? create_news_browser_window
     * (the Rust shell) raises it if so and otherwise builds + docks it, so we
     * snapshot the state first to decide the fetch behaviour below. */
    gboolean was_open
        = (hx_panel_registry_lookup (HX_PANEL_ID_NEWS15) != NULL);

    create_news_browser_window (widget, sess);

    if (!was_open && the_browser != NULL) {
        /* Freshly built: wire Ctrl+Q / Ctrl+K / Ctrl+T via init_keyaccel on
         * br->window (the content box) in capture phase so the column view's
         * focus chain doesn't swallow the accelerators. */
        init_keyaccel (the_browser->window);
    }

    /* Fetch-on-open matches News (1.0): each explicit open while connected
     * pulls a fresh tree (a re-open also clears the old one first) so the
     * user needn't hit Refresh after a quiet period. Eager-construct before
     * any connection just leaves the tree empty. */
    if (connected && the_browser != NULL) {
        if (was_open) {
            g_list_store_remove_all (the_browser->root_store);
        }
        fetch_dirlist (the_browser, NULL);
    }
}
