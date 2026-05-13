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
#include <glib/gi18n.h>

#include "files_entry.h"
#include "files_local_provider.h"
#include "files_panel.h"
#include "files_browser.h"

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

	/* CSS provider that paints the .files-panel-active border.
	 * Lives for the window's lifetime; unrefed in on_close. */
	GtkCssProvider *css;
};

static struct browser *the_browser = NULL;

/* ---- Active-panel tracking ---- */

static void
set_active (struct browser *br, files_panel *p)
{
	if (!br || !p || br->active == p) return;
	files_panel_set_active (br->left,  p == br->left);
	files_panel_set_active (br->right, p == br->right);
	br->active = p;
}

static void
on_focus_notify (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
	struct browser *br = user_data;
	GtkWidget *view = GTK_WIDGET (obj);
	(void) pspec;

	/* Only flip on focus-IN events. focus-OUT during a Tab cycle
	 * fires before the new panel's focus-IN; if we cleared the
	 * active state on focus-OUT we'd briefly have no active panel. */
	if (!gtk_widget_has_focus (view)) return;

	if (br->left  && files_panel_get_column_view (br->left)  == view)
		set_active (br, br->left);
	else if (br->right && files_panel_get_column_view (br->right) == view)
		set_active (br, br->right);
}

/* ---- Actions (scoped to active panel) ---- */

static void
on_refresh_clicked (GtkButton *btn, gpointer user_data)
{
	struct browser *br = user_data;
	(void) btn;
	if (br->active)
		hx_local_files_provider_reload (
			files_panel_get_provider (br->active));
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

	if (!hx_local_files_provider_mkdir (
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
}

struct delete_ctx {
	struct browser *br;
	files_panel    *panel;
	char           *name;     /* owned */
};

static void
on_delete_response (AdwAlertDialog *dialog, const char *response,
                     gpointer user_data)
{
	struct delete_ctx *ctx = user_data;
	GError *err = NULL;
	(void) dialog;

	if (g_strcmp0 (response, "delete") != 0) return;
	if (!ctx->panel || !ctx->name) return;

	if (!hx_local_files_provider_delete (
			files_panel_get_provider (ctx->panel), ctx->name, &err)) {
		g_warning ("delete failed: %s", err ? err->message : "unknown");
		g_clear_error (&err);
	}
}

static void
on_delete_closed (AdwAlertDialog *dialog, gpointer user_data)
{
	struct delete_ctx *ctx = user_data;
	(void) dialog;
	g_free (ctx->name);
	g_free (ctx);
}

static void
on_delete_clicked (GtkButton *btn, gpointer user_data)
{
	struct browser *br = user_data;
	HxFileEntry *e;
	AdwDialog *dialog;
	struct delete_ctx *ctx;
	char *body;
	(void) btn;

	if (!br->active) return;
	e = files_panel_get_single_selected (br->active);
	if (!e) return;

	body = g_strdup_printf (
		_("Delete “%s”? This cannot be undone."),
		hx_file_entry_get_name (e));
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

	ctx = g_new0 (struct delete_ctx, 1);
	ctx->br    = br;
	ctx->panel = br->active;
	ctx->name  = g_strdup (hx_file_entry_get_name (e));

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
		hx_local_files_provider_navigate_up (
			files_panel_get_provider (br->active));
	return TRUE;
}

/* ---- CSS for the active-panel highlight ---- */

static const char *active_css =
	".files-panel-active {\n"
	"  border: 2px solid @accent_color;\n"
	"  border-radius: 8px;\n"
	"}\n"
	".files-panel-active > * {\n"
	"  border-radius: 6px;\n"
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

	if (br->css) {
		display = gdk_display_get_default ();
		if (display)
			gtk_style_context_remove_provider_for_display (
				display, GTK_STYLE_PROVIDER (br->css));
		g_clear_object (&br->css);
	}

	files_panel_free (br->left);
	files_panel_free (br->right);
	g_free (br);
	return FALSE;
}

void
open_files_browser (void)
{
	struct browser *br;
	GtkWidget *header, *paned, *refresh_btn, *mkdir_btn, *delete_btn;
	HxLocalFilesProvider *left_prov, *right_prov;
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

	/* Headerbar with cross-panel actions. Phase 3 will add the
	 * Copy / Move buttons; Phase 1 keeps it to Refresh / New
	 * Folder / Delete operating on the active panel. */
	header      = adw_header_bar_new ();
	refresh_btn = gtk_button_new_from_icon_name ("view-refresh-symbolic");
	mkdir_btn   = gtk_button_new_from_icon_name ("folder-new-symbolic");
	delete_btn  = gtk_button_new_from_icon_name ("user-trash-symbolic");

	gtk_widget_set_tooltip_text (refresh_btn, _("Reload active panel (Ctrl+R)"));
	gtk_widget_set_tooltip_text (mkdir_btn,   _("New folder in active panel (Ctrl+N)"));
	gtk_widget_set_tooltip_text (delete_btn,  _("Delete selection in active panel (Ctrl+D)"));

	g_signal_connect (refresh_btn, "clicked", G_CALLBACK (on_refresh_clicked), br);
	g_signal_connect (mkdir_btn,   "clicked", G_CALLBACK (on_mkdir_clicked),   br);
	g_signal_connect (delete_btn,  "clicked", G_CALLBACK (on_delete_clicked),  br);

	adw_header_bar_pack_start (ADW_HEADER_BAR (header), refresh_btn);
	adw_header_bar_pack_start (ADW_HEADER_BAR (header), mkdir_btn);
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

	left_prov  = hx_local_files_provider_new (NULL);
	right_prov = hx_local_files_provider_new (NULL);
	br->left  = files_panel_new (left_prov);
	br->right = files_panel_new (right_prov);
	g_object_unref (left_prov);    /* panel holds a ref */
	g_object_unref (right_prov);

	gtk_paned_set_start_child (GTK_PANED (paned),
		files_panel_get_widget (br->left));
	gtk_paned_set_end_child (GTK_PANED (paned),
		files_panel_get_widget (br->right));

	gtk_window_set_child (GTK_WINDOW (br->window), paned);

	/* Track which panel has focus so the headerbar actions know
	 * who to operate on. Connect AFTER both panels exist. */
	g_signal_connect (files_panel_get_column_view (br->left),
		"notify::has-focus", G_CALLBACK (on_focus_notify), br);
	g_signal_connect (files_panel_get_column_view (br->right),
		"notify::has-focus", G_CALLBACK (on_focus_notify), br);

	/* Window-level keyboard shortcuts.
	 *
	 *   Tab        — switch active panel
	 *   Backspace  — up one directory in active panel
	 *   Ctrl+R     — reload active panel
	 *   Ctrl+N     — new folder
	 *   Ctrl+D     — delete selection
	 *
	 * The Tab + Backspace bindings are intercepted at the window
	 * level (rather than per-panel) so they fire regardless of
	 * which sub-widget happens to have focus. */
	shortcuts = gtk_shortcut_controller_new ();
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

	/* Initial focus on the left panel so the user has a working
	 * active selection right away. */
	set_active (br, br->left);
	gtk_widget_grab_focus (files_panel_get_column_view (br->left));

	gtk_window_present (GTK_WINDOW (br->window));
}
