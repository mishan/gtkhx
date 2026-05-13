/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"

#include <gtk/gtk.h>
#include <adwaita.h>

#include "files_entry.h"
#include "files_provider.h"
#include "files_local_provider.h"
#include "files_remote_provider.h"
#include "files_panel.h"
#include "files_ops.h"
#include "files_browser.h"
#include "gtkhx_session.h"
#include "gtkutil.h"

/* gi18n.h after the project headers — the codebase's compat.h has
 * a placeholder _ macro that we want overridden by the proper
 * gettext expansion. */
#undef _
#include <glib/gi18n.h>

/* The browser holds two panels + the active-panel marker. The
 * active panel is the one with the column-view focus child; we
 * detect focus changes by hooking each column-view's
 * has-focus notify and update both panels' CSS class +
 * `active_panel` accordingly. */
struct browser {
	GtkWidget   *window;
	files_panel *left;
	files_panel *right;
	files_panel *active;

	/* Keep refs on the providers separately from the panels so
	 * the connection-state hook can reach the remote one even if
	 * the panel pointer ever needs to be swapped (Phase 2.5 side
	 * selector, future). */
	HxFilesProvider *left_provider;
	HxFilesProvider *right_provider;

	/* GtkhxSession::connection-state handler — fires the remote
	 * provider's "unavailable-changed" so the panel reloads on
	 * login + paints the not-connected state on disconnect. */
	gulong conn_state_handler;

	/* CSS provider that paints the .files-panel-active border.
	 * Lives for the window's lifetime; unrefed in on_close. */
	GtkCssProvider *css;

	/* AdwToastOverlay wrapping the window content — used by the
	 * Copy action to surface "no permission" / "not connected" /
	 * etc. results without an interrupting dialog. */
	AdwToastOverlay *toast;

	/* Copy button — re-tooltip'd on active-panel change so the
	 * arrow direction in the hover text reflects which side is
	 * source vs. dest. */
	GtkWidget *btn_copy;
};

static struct browser *the_browser = NULL;

/* ---- Active-panel tracking ---- */

/* Update Copy's tooltip to reflect which way the transfer goes. */
static void
sync_copy_tooltip (struct browser *br)
{
	if (!br || !br->btn_copy) return;
	if (br->active == br->left)
		gtk_widget_set_tooltip_text (br->btn_copy,
			_("Copy selection from left to right"));
	else if (br->active == br->right)
		gtk_widget_set_tooltip_text (br->btn_copy,
			_("Copy selection from right to left"));
}

static void
set_active (struct browser *br, files_panel *p)
{
	if (!br || !p || br->active == p) return;
	files_panel_set_active (br->left,  p == br->left);
	files_panel_set_active (br->right, p == br->right);
	br->active = p;
	sync_copy_tooltip (br);
}

static void
show_toast (struct browser *br, const char *text)
{
	if (!br || !br->toast || !text) return;
	adw_toast_overlay_add_toast (br->toast, adw_toast_new (text));
}

/* Wire a focus controller on a panel's root widget. The
 * controller's "enter" signal fires when focus moves into the
 * widget OR any descendant — that's the right semantic for the
 * active-panel marker. Hooking notify::has-focus on the column
 * view alone didn't work: the column view itself rarely gets
 * focus directly; its inner row widget does, and has-focus on
 * the parent doesn't reliably propagate.
 *
 * A click gesture in capture phase covers the case where the
 * user clicks somewhere that's not focusable (the path entry,
 * empty space below the rows) — we want that to flip the active
 * panel anyway so the headerbar actions follow the user's
 * pointer-driven intent. */
static void
on_panel_focus_enter (GtkEventControllerFocus *ctrl, gpointer user_data)
{
	struct browser *br = user_data;
	GtkWidget *panel_root;
	(void) ctrl;

	panel_root = gtk_event_controller_get_widget (
		GTK_EVENT_CONTROLLER (ctrl));
	if (br->left  && files_panel_get_widget (br->left)  == panel_root)
		set_active (br, br->left);
	else if (br->right && files_panel_get_widget (br->right) == panel_root)
		set_active (br, br->right);
}

static void
on_panel_clicked (GtkGestureClick *gesture, int n_press,
                   double x, double y, gpointer user_data)
{
	struct browser *br = user_data;
	GtkWidget *panel_root;
	(void) n_press; (void) x; (void) y;

	panel_root = gtk_event_controller_get_widget (
		GTK_EVENT_CONTROLLER (gesture));
	if (br->left  && files_panel_get_widget (br->left)  == panel_root)
		set_active (br, br->left);
	else if (br->right && files_panel_get_widget (br->right) == panel_root)
		set_active (br, br->right);
}

static void
attach_panel_focus_tracking (struct browser *br, files_panel *p)
{
	GtkEventController *focus_ctrl;
	GtkGesture         *click;
	GtkWidget          *root = files_panel_get_widget (p);

	focus_ctrl = gtk_event_controller_focus_new ();
	g_signal_connect (focus_ctrl, "enter",
		G_CALLBACK (on_panel_focus_enter), br);
	gtk_widget_add_controller (root, focus_ctrl);

	/* BUBBLE phase: column view sees the click first and runs
	 * its built-in click-counting (selection on first press,
	 * activate on second press of a double-click). We observe
	 * on the way back up to flip the active panel. The earlier
	 * CAPTURE-phase version of this gesture broke double-click
	 * activation on the non-active panel — the column view saw
	 * the first click of a double-click pair as just a
	 * selection-with-focus-shift and waited for another pair
	 * before treating it as a double. Symptom: first double-
	 * click in the remote panel did nothing, second double-
	 * click descended. BUBBLE phase keeps the active-flip
	 * working for all cases except clicks that the column view
	 * fully consumes — and even then the focus controller
	 * above catches focus-enter and flips active. */
	click = gtk_gesture_click_new ();
	gtk_event_controller_set_propagation_phase (
		GTK_EVENT_CONTROLLER (click), GTK_PHASE_BUBBLE);
	g_signal_connect (click, "pressed",
		G_CALLBACK (on_panel_clicked), br);
	gtk_widget_add_controller (root, GTK_EVENT_CONTROLLER (click));
}

/* ---- Actions (scoped to active panel) ---- */

static void
on_refresh_clicked (GtkButton *btn, gpointer user_data)
{
	struct browser *br = user_data;
	(void) btn;
	if (br->active)
		hx_files_provider_reload (
			files_panel_get_provider (br->active));
}

/* Copy — issue hx_files_ops_copy for each entry in the active
 * panel's selection. The whole batch shares a source-and-dest
 * pair; per-entry errors are tallied and surfaced as one
 * summary toast. */
static void
on_copy_clicked (GtkButton *btn, gpointer user_data)
{
	struct browser *br = user_data;
	files_panel *src, *dst;
	GPtrArray *entries;
	guint i, queued = 0, failed = 0;
	HxOpsResult last_err = HX_OPS_OK;
	(void) btn;

	if (!br->active) return;
	src = br->active;
	dst = (src == br->left) ? br->right : br->left;
	if (!dst) return;

	entries = files_panel_get_selected_entries (src);
	if (!entries || entries->len == 0) {
		if (entries) g_ptr_array_free (entries, TRUE);
		show_toast (br, _("Select a file to copy first."));
		return;
	}

	for (i = 0; i < entries->len; i++) {
		HxFileEntry *e = g_ptr_array_index (entries, i);
		HxOpsResult r = hx_files_ops_copy (
			files_panel_get_provider (src),
			files_panel_get_provider (dst),
			e);
		if (r == HX_OPS_OK) queued++;
		else { failed++; last_err = r; }
	}
	g_ptr_array_free (entries, TRUE);

	/* Surface a one-shot summary: if everything queued, just say
	 * how many; if anything failed, lead with the failure reason
	 * since that's the actionable bit. last_err alone covers the
	 * common case where every failure had the same cause (most
	 * fail-modes are global — no permission, not connected, etc.). */
	if (failed == 0) {
		char *msg = g_strdup_printf (
			g_dngettext (NULL,
				"Transfer queued (%u item).",
				"Transfers queued (%u items).",
				queued),
			queued);
		show_toast (br, msg);
		g_free (msg);
	} else if (queued == 0) {
		show_toast (br, hx_files_ops_result_message (last_err));
	} else {
		char *msg = g_strdup_printf (
			_("%u queued, %u failed (%s)."),
			queued, failed,
			hx_files_ops_result_message (last_err));
		show_toast (br, msg);
		g_free (msg);
	}
}

struct mkdir_ctx {
	struct browser *br;
	files_panel    *panel;
	GtkWidget      *entry;
};

static void
on_mkdir_response (AdwAlertDialog *dialog, const char *response, gpointer user_data)
{
	struct mkdir_ctx *ctx = user_data;
	const char *name;
	GError *err = NULL;
	(void) dialog;

	if (g_strcmp0 (response, "create") != 0) return;
	if (!ctx->panel) return;
	name = gtk_editable_get_text (GTK_EDITABLE (ctx->entry));
	if (!name || !*name) return;

	if (!hx_files_provider_mkdir (
			files_panel_get_provider (ctx->panel), name, &err)) {
		g_warning ("mkdir failed: %s", err ? err->message : "unknown");
		g_clear_error (&err);
	}
}

static void
on_mkdir_closed (AdwAlertDialog *dialog, gpointer user_data)
{
	(void) dialog;
	g_free (user_data);
}

static void
on_mkdir_clicked (GtkButton *btn, gpointer user_data)
{
	struct browser *br = user_data;
	AdwDialog *dialog;
	GtkWidget *entry;
	struct mkdir_ctx *ctx;
	(void) btn;

	if (!br->active) return;

	dialog = ADW_DIALOG (adw_alert_dialog_new (
		_("New Folder"),
		_("Enter a name for the new folder.")));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
		"cancel", _("_Cancel"));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
		"create", _("C_reate"));
	adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
		"create", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "create");
	adw_alert_dialog_set_close_response   (ADW_ALERT_DIALOG (dialog), "cancel");

	entry = gtk_entry_new ();
	gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
	adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), entry);

	ctx = g_new0 (struct mkdir_ctx, 1);
	ctx->br    = br;
	ctx->panel = br->active;
	ctx->entry = entry;

	g_signal_connect (dialog, "response", G_CALLBACK (on_mkdir_response), ctx);
	g_signal_connect (dialog, "closed",   G_CALLBACK (on_mkdir_closed),   ctx);

	adw_dialog_present (dialog, br->window);
	/* Focus the entry so the user can type immediately + hit
	 * Enter. activates-default = TRUE on the entry routes that
	 * Enter to AdwAlertDialog's default response ("create").
	 * Has to happen AFTER adw_dialog_present — the dialog isn't
	 * realized before that and grab_focus is a no-op on an
	 * unmapped widget. */
	gtk_widget_grab_focus (entry);
}

struct delete_ctx {
	struct browser *br;
	files_panel    *panel;
	GPtrArray      *names;    /* owned — array of g_strdup'd names */
};

static void
on_delete_response (AdwAlertDialog *dialog, const char *response,
                     gpointer user_data)
{
	struct delete_ctx *ctx = user_data;
	HxFilesProvider *prov;
	guint i;
	(void) dialog;

	if (g_strcmp0 (response, "delete") != 0) return;
	if (!ctx->panel || !ctx->names) return;
	prov = files_panel_get_provider (ctx->panel);

	for (i = 0; i < ctx->names->len; i++) {
		const char *name = g_ptr_array_index (ctx->names, i);
		GError *err = NULL;
		if (!hx_files_provider_delete (prov, name, &err)) {
			g_warning ("delete %s: %s", name,
				err ? err->message : "unknown");
			g_clear_error (&err);
		}
	}
}

static void
on_delete_closed (AdwAlertDialog *dialog, gpointer user_data)
{
	struct delete_ctx *ctx = user_data;
	(void) dialog;
	if (ctx->names) g_ptr_array_free (ctx->names, TRUE);
	g_free (ctx);
}

/* Build the delete-confirmation body text. Singular for one
 * entry (with the actual name so the user can sanity-check),
 * plural with a count for multi-select since fitting N names
 * into one toast line gets unwieldy. */
static char *
delete_body_text (GPtrArray *entries)
{
	HxFileEntry *e;
	if (!entries || entries->len == 0)
		return g_strdup ("");
	if (entries->len == 1) {
		e = g_ptr_array_index (entries, 0);
		return g_strdup_printf (
			_("Delete “%s”? This cannot be undone."),
			hx_file_entry_get_name (e));
	}
	return g_strdup_printf (
		g_dngettext (NULL,
			"Delete %u item? This cannot be undone.",
			"Delete %u items? This cannot be undone.",
			entries->len),
		entries->len);
}

static void
on_delete_clicked (GtkButton *btn, gpointer user_data)
{
	struct browser *br = user_data;
	GPtrArray *entries;
	AdwDialog *dialog;
	struct delete_ctx *ctx;
	char *body;
	guint i;
	(void) btn;

	if (!br->active) return;
	entries = files_panel_get_selected_entries (br->active);
	if (!entries || entries->len == 0) {
		if (entries) g_ptr_array_free (entries, TRUE);
		return;
	}

	body = delete_body_text (entries);
	dialog = ADW_DIALOG (adw_alert_dialog_new (_("Delete"), body));
	g_free (body);

	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
		"cancel", _("_Cancel"));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
		"delete", _("_Delete"));
	adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
		"delete", ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
	adw_alert_dialog_set_close_response   (ADW_ALERT_DIALOG (dialog), "cancel");

	/* Snapshot just the names — the dialog runs async and the
	 * selection set could shift in between. Same defensive
	 * pattern the news_browser delete uses. */
	ctx = g_new0 (struct delete_ctx, 1);
	ctx->br    = br;
	ctx->panel = br->active;
	ctx->names = g_ptr_array_new_with_free_func (g_free);
	for (i = 0; i < entries->len; i++) {
		HxFileEntry *e = g_ptr_array_index (entries, i);
		g_ptr_array_add (ctx->names,
			g_strdup (hx_file_entry_get_name (e)));
	}
	g_ptr_array_free (entries, TRUE);

	g_signal_connect (dialog, "response", G_CALLBACK (on_delete_response), ctx);
	g_signal_connect (dialog, "closed",   G_CALLBACK (on_delete_closed),   ctx);

	adw_dialog_present (dialog, br->window);
}

/* ---- Keyboard shortcut: Tab switches active panel ---- */

static gboolean
on_tab_shortcut (GtkWidget *widget, GVariant *args, gpointer user_data)
{
	struct browser *br = user_data;
	files_panel *other;
	(void) widget; (void) args;

	if (!br->active) return FALSE;
	other = (br->active == br->left) ? br->right : br->left;
	if (other)
		gtk_widget_grab_focus (files_panel_get_column_view (other));
	return TRUE;
}

static gboolean
on_backspace_shortcut (GtkWidget *widget, GVariant *args, gpointer user_data)
{
	struct browser *br = user_data;
	(void) widget; (void) args;

	if (br->active)
		hx_files_provider_navigate_up (
			files_panel_get_provider (br->active));
	return TRUE;
}

/* ---- CSS for the active-panel highlight ---- */

/* Active-panel marker via inset box-shadow rather than a real
 * 2px border. The border version reflowed the frame's inner
 * scrolled-window-and-column-view by 4px when the active class
 * was toggled, which is enough of a layout invalidation to
 * make GtkColumnView throw away in-progress click sequences.
 * box-shadow paints over existing pixels without taking
 * layout space, so the column view is the same size before
 * and after the active flip.
 *
 * Hardcoded hex rather than @accent_color so the rule resolves
 * unambiguously across libadwaita color-scheme + accent
 * settings — gtkurl.c does the same thing for its URL tag.
 * #1c71d8 is libadwaita's default light-theme accent. */
static const char *active_css =
	".files-panel-active {\n"
	"  box-shadow: inset 0 0 0 2px #1c71d8;\n"
	"  border-radius: 8px;\n"
	"}\n";

static void
install_css (struct browser *br)
{
	GdkDisplay *display;

	br->css = gtk_css_provider_new ();
	gtk_css_provider_load_from_string (br->css, active_css);
	display = gdk_display_get_default ();
	if (display)
		gtk_style_context_add_provider_for_display (
			display, GTK_STYLE_PROVIDER (br->css),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* ---- Lifecycle ---- */

static gboolean
on_close (GtkWindow *window, gpointer user_data)
{
	struct browser *br = user_data;
	GdkDisplay *display;
	(void) window;

	if (the_browser == br) the_browser = NULL;

	if (br->conn_state_handler) {
		g_signal_handler_disconnect (
			gtkhx_session_get_default (),
			br->conn_state_handler);
		br->conn_state_handler = 0;
	}

	if (br->css) {
		display = gdk_display_get_default ();
		if (display)
			gtk_style_context_remove_provider_for_display (
				display, GTK_STYLE_PROVIDER (br->css));
		g_clear_object (&br->css);
	}

	files_panel_free (br->left);
	files_panel_free (br->right);
	g_clear_object (&br->left_provider);
	g_clear_object (&br->right_provider);
	g_free (br);
	return FALSE;
}

/* GtkhxSession fires this when the connection state pivots.
 * State is a GtkhxConnectionState — we don't differentiate
 * here; any state change might flip get_unavailable_reason()
 * from / to NULL, so we just nudge the remote provider. */
static void
on_connection_state (GtkhxSession *sess, guint state, gpointer user_data)
{
	struct browser *br = user_data;
	(void) sess; (void) state;
	if (br->right_provider)
		g_signal_emit_by_name (br->right_provider, "unavailable-changed");
}

void
open_files_browser (void)
{
	struct browser *br;
	GtkWidget *header, *paned, *refresh_btn, *mkdir_btn, *delete_btn;
	GtkEventController *shortcuts;
	GtkShortcut *sh;

	if (the_browser) {
		gtk_window_present (GTK_WINDOW (the_browser->window));
		return;
	}

	br = g_new0 (struct browser, 1);

	br->window = gtk_window_new ();
	gtk_window_set_title (GTK_WINDOW (br->window), _("Files"));
	gtk_widget_set_size_request (br->window, 980, 560);

	install_css (br);

	/* Headerbar:
	 *   pack_start: Refresh, New Folder, Copy →
	 *   pack_end:   Delete
	 *
	 * Copy moves the active panel's selection to the inactive
	 * panel's current path. The arrow direction in the tooltip
	 * gets re-set on active-panel changes via sync_copy_tooltip;
	 * the icon stays generic (edit-copy-symbolic) since flipping
	 * it on every selection change would just be visual noise.
	 *
	 * Phase 3 wires copy + permission gating. Move and folder
	 * recursion are Phase 4. */
	header      = adw_header_bar_new ();
	refresh_btn = gtk_button_new_from_icon_name ("view-refresh-symbolic");
	mkdir_btn   = gtk_button_new_from_icon_name ("folder-new-symbolic");
	br->btn_copy = gtk_button_new_from_icon_name ("edit-copy-symbolic");
	delete_btn  = gtk_button_new_from_icon_name ("user-trash-symbolic");

	gtk_widget_set_tooltip_text (refresh_btn,  _("Reload active panel (Ctrl+R)"));
	gtk_widget_set_tooltip_text (mkdir_btn,    _("New folder in active panel (Ctrl+N)"));
	gtk_widget_set_tooltip_text (br->btn_copy, _("Copy selection to the other panel (F5)"));
	gtk_widget_set_tooltip_text (delete_btn,   _("Delete selection in active panel (Ctrl+D)"));

	g_signal_connect (refresh_btn,  "clicked", G_CALLBACK (on_refresh_clicked), br);
	g_signal_connect (mkdir_btn,    "clicked", G_CALLBACK (on_mkdir_clicked),   br);
	g_signal_connect (br->btn_copy, "clicked", G_CALLBACK (on_copy_clicked),    br);
	g_signal_connect (delete_btn,   "clicked", G_CALLBACK (on_delete_clicked),  br);

	adw_header_bar_pack_start (ADW_HEADER_BAR (header), refresh_btn);
	adw_header_bar_pack_start (ADW_HEADER_BAR (header), mkdir_btn);
	adw_header_bar_pack_start (ADW_HEADER_BAR (header), br->btn_copy);
	adw_header_bar_pack_end   (ADW_HEADER_BAR (header), delete_btn);
	gtk_window_set_titlebar (GTK_WINDOW (br->window), header);

	/* Two panels in a horizontal GtkPaned. Phase 1: both local;
	 * Phase 2 swaps the right panel's provider for a remote one
	 * (and adds the L/R side selectors). Default split position
	 * is centered; the user can drag the divider. */
	paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_set_resize_start_child (GTK_PANED (paned), TRUE);
	gtk_paned_set_resize_end_child   (GTK_PANED (paned), TRUE);
	gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);
	gtk_paned_set_shrink_end_child   (GTK_PANED (paned), FALSE);

	/* L = local FS (XDG_DOWNLOAD_DIR by default).
	 * R = remote Hotline server. The remote provider sits idle
	 * until the connection is up — the panel paints a
	 * "Not connected" state until then. */
	{
		HxLocalFilesProvider  *local;
		HxRemoteFilesProvider *remote;
		local  = hx_local_files_provider_new (NULL);
		remote = hx_remote_files_provider_new ();
		br->left_provider  = HX_FILES_PROVIDER (local);
		br->right_provider = HX_FILES_PROVIDER (remote);
	}
	br->left  = files_panel_new (br->left_provider);
	br->right = files_panel_new (br->right_provider);

	br->conn_state_handler = g_signal_connect (
		gtkhx_session_get_default (), "connection-state-changed",
		G_CALLBACK (on_connection_state), br);

	gtk_paned_set_start_child (GTK_PANED (paned),
		files_panel_get_widget (br->left));
	gtk_paned_set_end_child (GTK_PANED (paned),
		files_panel_get_widget (br->right));

	/* Wrap in a toast overlay so the Copy action (and future
	 * polish-phase actions) have somewhere to surface transient
	 * feedback ("Transfer queued.", "You don't have permission
	 * for that.", etc.) without an interrupting dialog. */
	br->toast = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
	adw_toast_overlay_set_child (br->toast, paned);
	gtk_window_set_child (GTK_WINDOW (br->window), GTK_WIDGET (br->toast));

	/* Track which panel has focus / was clicked so the headerbar
	 * actions know who to operate on. Wired AFTER both panels
	 * exist so attach_panel_focus_tracking can reach them via
	 * files_panel_get_widget. */
	attach_panel_focus_tracking (br, br->left);
	attach_panel_focus_tracking (br, br->right);

	/* Window-level keyboard shortcuts.
	 *
	 *   Tab        — switch active panel
	 *   Backspace  — up one directory in active panel
	 *
	 * Capture phase is the only way to intercept Tab — without it,
	 * GtkColumnView's built-in focus chain consumes the keystroke
	 * for column-to-column navigation before the window-level
	 * shortcut sees it. Same logic for Backspace though that one
	 * isn't normally claimed by descendants. */
	shortcuts = gtk_shortcut_controller_new ();
	gtk_event_controller_set_propagation_phase (shortcuts,
		GTK_PHASE_CAPTURE);
	gtk_shortcut_controller_set_scope (
		GTK_SHORTCUT_CONTROLLER (shortcuts), GTK_SHORTCUT_SCOPE_GLOBAL);
	gtk_widget_add_controller (br->window, shortcuts);

	sh = gtk_shortcut_new (
		gtk_keyval_trigger_new (GDK_KEY_Tab, 0),
		gtk_callback_action_new (on_tab_shortcut, br, NULL));
	gtk_shortcut_controller_add_shortcut (
		GTK_SHORTCUT_CONTROLLER (shortcuts), sh);

	sh = gtk_shortcut_new (
		gtk_keyval_trigger_new (GDK_KEY_BackSpace, 0),
		gtk_callback_action_new (on_backspace_shortcut, br, NULL));
	gtk_shortcut_controller_add_shortcut (
		GTK_SHORTCUT_CONTROLLER (shortcuts), sh);

	/* TODO Phase 4: Ctrl-R / Ctrl-N / Ctrl-D bindings. Skipped in
	 * Phase 1 since the headerbar buttons are easy to reach and
	 * the shortcuts are bikeshed-able (some users want F-key parity,
	 * others want emacs-style Ctrl-X). */

	g_signal_connect (br->window, "close-request",
		G_CALLBACK (on_close), br);

	the_browser = br;

	/* Standard window accelerators — Ctrl+W close, Ctrl+Q quit,
	 * Ctrl+K connect, Ctrl+T tracker. Same set every other
	 * window in the app picks up via init_keyaccel. Capture
	 * phase means the column views' internal focus chain
	 * doesn't swallow them. */
	init_keyaccel (br->window);

	/* Initial focus on the left panel so the user has a working
	 * active selection right away. */
	set_active (br, br->left);
	sync_copy_tooltip (br);
	gtk_widget_grab_focus (files_panel_get_column_view (br->left));

	gtk_window_present (GTK_WINDOW (br->window));
}
