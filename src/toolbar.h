#ifndef HX_TOOL_H
#define HX_TOOL_H

extern GtkWidget *toolbar_window;
extern GtkWidget *files_btn;
extern GtkWidget *connect_btn;
extern GtkWidget *disconnect_btn;
extern GtkWidget *news15_btn;
extern GtkWidget *news_btn;
extern GtkWidget *broadcast_btn;

/* status_bar is a GtkLabel now (was GtkStatusbar — deprecated
 * in GTK 4.10 and we never used its message-stack model). The
 * status_msg / context_status globals are gone with it; set_status_bar()
 * just calls gtk_label_set_text() on this. */
extern GtkWidget *status_bar;

extern void create_toolbar_window (session *sess);
extern void disconnect_clicked (void);

/* the dock is ONE recursive HxSplit tree.
 * The five globals below are the handles every other module
 * needs. NULL before create_toolbar_window has run.
 *
 *   toolbar_dock          A libpanel PanelDock acting as a
 *                         thin wrapper around the HxSplit tree.
 *                         The dock has exactly one child — the
 *                         HxSplit root — added as its center
 *                         child via Buildable add_child. The
 *                         wrapper exists for one reason:
 *                         libpanel's PanelFrame template
 *                         instantiates PanelDropControls as a
 *                         private child whose root vfunc
 *                         asserts a PANEL_TYPE_DOCK ancestor at
 *                         root time. We don't use libpanel's
 *                         drop controls — we have our own
 *                         dock-level GtkDropTarget — but the
 *                         warning still fires without the
 *                         wrapper. There's NO sidebar / area
 *                         reveal in play; do not call
 *                         panel_dock_set_reveal_* on this
 *                         pointer. See docs/docking-splits.md.
 *
 *   toolbar_*_frame       The default-leaf PanelFrame for each
 *                         of the four default placement slots
 *                         (left sidebar = News, right sidebar =
 *                         Users, bottom = Tasks, center = Chat +
 *                         Files + News 1.5). Per-window panel
 *                         factories use these as their
 *                         panel_frame_add target.
 *
 *                         Stable across user splits: when the
 *                         user splits a default leaf, the
 *                         original PanelFrame stays in place
 *                         (it becomes child_a of the new
 *                         internal split), and a fresh sibling
 *                         leaf appears as child_b. The pointer
 *                         still references the original frame.
 *
 *                         Updated when the user closes a
 *                         default leaf — on_frame_close in
 *                         hx_split.c reseats the relevant
 *                         pointer onto the surviving sibling's
 *                         PanelFrame before the leaf is
 *                         destroyed.
 *
 * Factory call shape:
 *   panel_frame_add (PANEL_FRAME (toolbar_sidebar_frame), panel);
 *   hx_panel_registry_register (panel);
 */
extern GtkWidget *toolbar_dock;
extern GtkWidget *toolbar_sidebar_frame; /* News default */
extern GtkWidget *toolbar_end_frame;     /* Users default */
extern GtkWidget *toolbar_bottom_frame;  /* Tasks default */
extern GtkWidget *toolbar_center_frame;  /* Chat + Files + News 1.5 default */

/* register the hamburger-menu's GActions on the application.
 * Call from gtkhx_activate after the AdwApplication is constructed —
 * fe_init() runs create_toolbar_window earlier (before
 * g_application_run), so the actions can't be added at toolbar
 * construction time. Idempotent: GActionMap silently overwrites a
 * second registration with a g_critical (we want to see that, so
 * caller should only call this once). */
extern void toolbar_register_actions (GApplication *app, session *sess);

/* install the per-frame plumbing every leaf
 * PanelFrame in the dock needs (close-dispatcher, drag-out hook,
 * defanged drop-controls). Called once per area at dock build
 * time, and again whenever a user splits a leaf (the new sibling
 * leaf's PanelFrame needs the same hooks). */
extern void toolbar_install_panel_hooks_on_frame (GtkWidget *frame);

/* Hard horizontal minimum every leaf in the dock applies via
 * gtk_widget_set_size_request. Lives in the header so the
 * saved-layout loader (src/dock_layout.c) and the default-build
 * path (src/toolbar.c) share the same value — drift between them
 * would silently restore custom layouts at a different minimum
 * than the default layout uses. 300 px covers the widest of the
 * default panels' button rows (Users: 6 icon-buttons + spacing
 * + margins ≈ 280 px) with a small margin. */
#define DEFAULT_LEAF_MIN_WIDTH 300

/* push a transient AdwToast onto the toolbar window's
 * AdwToastOverlay. Safe to call before the toolbar is built (no-op).
 * The toast auto-dismisses after libadwaita's default timeout. */
extern void toolbar_show_toast (const char *text);

/* Dismiss every toast we've pushed onto the toolbar's overlay that
 * hasn't auto-timed-out yet. Wired into the connection-state hook so
 * starting a new connect (Connect button, bookmark, Reconnect)
 * clears toasts from the previous server — task errors, broadcasts,
 * "Logged in" — instead of letting them hang over a new session.
 * Safe to call before the toolbar is built (no-op). */
extern void toolbar_clear_toasts (void);

/* reveal / hide the toolbar's AdwBanner. show_connection_lost
 * sets the banner text to "Lost connection to <server>" and reveals
 * the banner with a Reconnect button; hide_banner just sets revealed
 * to FALSE. Safe to call before the toolbar is built (no-op). */
extern void toolbar_show_connection_lost (const char *server);
extern void toolbar_hide_banner (void);

/* rescan the bookmarks directory and refresh the Connect
 * SplitButton's dropdown menu. Called from connect.c after a
 * successful Save Bookmark so newly-added entries show up without
 * restarting the app. */
extern void toolbar_refresh_bookmarks (void);

#endif
