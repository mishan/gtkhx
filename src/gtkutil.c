/*
 * Copyright (C) 2001 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include <adwaita.h>
#include <gdk/gdkkeysyms.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include "hx.h"
#include "gtk_hlist.h"
#include "news.h"
#include "network.h"
#include "toolbar.h"
#include "tasks.h"
#include "users.h"
#include "chat.h"
#include "connect.h"
#include "gtkhx.h"
#include "files.h"
#include "xtext.h"
#include "gtkutil.h"
#include "hl_access.h"

/* Phase 4.11: GtkAccelGroup / gtk_accel_group_new /
 * gtk_widget_add_accelerator / gtk_window_add_accel_group are gone
 * in GTK 4 — replaced by GtkShortcutController plus GtkShortcut
 * instances bound to GtkKeyvalTrigger triggers and GtkCallbackAction
 * actions.
 *
 * Behavior: every window the user opens gets Ctrl+K (connect dialog)
 * and Ctrl+Q (quit) wired up. Connect still goes through the
 * AdwHeaderBar's connect_btn (so visual feedback works). Quit fires
 * the app.quit GAction directly, since after the Phase 5 toolbar
 * refactor Quit lives in the hamburger menu and there is no quit_btn
 * to "click". The GAction approach is the modern path for the rest
 * of these too — see gtk_application_set_accels_for_action — but
 * Connect's button-driven flow is harmless for now. */

static gboolean
keyaccel_connect_cb (GtkWidget *w, GVariant *args, gpointer data)
{
	(void) w; (void) args; (void) data;
	if (connect_btn)
		g_signal_emit_by_name (connect_btn, "clicked");
	return TRUE;
}

static gboolean
keyaccel_quit_cb (GtkWidget *w, GVariant *args, gpointer data)
{
	GApplication *app = g_application_get_default ();
	(void) w; (void) args; (void) data;
	if (app)
		g_action_group_activate_action (G_ACTION_GROUP (app),
		                                "quit", NULL);
	return TRUE;
}

void init_keyaccel (GtkWidget *widget)
{
	GtkEventController *ctrl = gtk_shortcut_controller_new ();
	GtkShortcut *sc;

	gtk_event_controller_set_propagation_phase (ctrl, GTK_PHASE_CAPTURE);

	sc = gtk_shortcut_new (
		gtk_keyval_trigger_new ('k', GDK_CONTROL_MASK),
		gtk_callback_action_new (keyaccel_connect_cb, NULL, NULL));
	gtk_shortcut_controller_add_shortcut (
		GTK_SHORTCUT_CONTROLLER (ctrl), sc);

	sc = gtk_shortcut_new (
		gtk_keyval_trigger_new ('q', GDK_CONTROL_MASK),
		gtk_callback_action_new (keyaccel_quit_cb, NULL, NULL));
	gtk_shortcut_controller_add_shortcut (
		GTK_SHORTCUT_CONTROLLER (ctrl), sc);

	gtk_widget_add_controller (widget, ctrl);
}

void set_disconnect_btn(session *sess, int stat)
{
	gtk_widget_set_sensitive(disconnect_btn, stat);
}

/* Phase 5: helper to flip a hamburger-menu GAction's enabled state.
 * The Admin submenu's New User / Edit User entries used to be
 * standalone toolbar buttons whose sensitivity was driven by the
 * connection state; with the actions on the application instead,
 * we toggle GSimpleAction::enabled and the menu items grey out
 * automatically. */
static void
set_app_action_enabled (const char *name, gboolean enabled)
{
	GApplication *app = g_application_get_default ();
	GAction *action;

	if (!app)
		return;
	action = g_action_map_lookup_action (G_ACTION_MAP (app), name);
	if (G_IS_SIMPLE_ACTION (action))
		g_simple_action_set_enabled (G_SIMPLE_ACTION (action), enabled);
}

void setbtns(session *sess, int stat)
{
	if(gtkhx_prefs.geo.users.open) {
		gtk_widget_set_sensitive(msgbtn, stat);
		gtk_widget_set_sensitive(infobtn, stat);
		gtk_widget_set_sensitive(chatbtn, stat);
		gtk_widget_set_sensitive(ignobtn, stat);
		/* Phase 5: kick / ban get visibility gating in the Users
		 * window — hide them entirely when the account doesn't
		 * have HL_ACCESS_DISCONNECT_USERS. (One bit gates both per
		 * mhxd's struct.) On disconnect, hide them too: we don't
		 * know what the next server will allow, and an unauthorised
		 * kick button next to a friendly Msg button is worse UX
		 * than just dropping the icon.
		 *
		 * Same access-bit gate as the right-click popup's Kick/Ban
		 * section, so the toolbar and the popup agree on what's
		 * available. */
		if (stat &&
		    hl_access_has ((const guint8 *) &sess->htlc.access,
		                   HL_ACCESS_DISCONNECT_USERS)) {
			gtk_widget_set_visible   (kickbtn, TRUE);
			gtk_widget_set_sensitive (kickbtn, TRUE);
			gtk_widget_set_visible   (banbtn,  TRUE);
			gtk_widget_set_sensitive (banbtn,  TRUE);
		} else {
			gtk_widget_set_visible (kickbtn, FALSE);
			gtk_widget_set_visible (banbtn,  FALSE);
		}
	}
	if(gtkhx_prefs.geo.news.open) {
		gtk_widget_set_sensitive(sess->postButton, stat);

		gtk_widget_set_sensitive(sess->reloadButton, stat);

	}

	gtk_widget_set_sensitive(files_btn, stat);

	/* Phase 5: New User / Edit User moved from toolbar buttons to
	 * the hamburger menu's Admin submenu. Flip the corresponding
	 * GActions instead of the old GtkWidget pointers. */
	set_app_action_enabled ("user_new",  stat);
	set_app_action_enabled ("user_edit", stat);

	/* Phase 5: News-related toolbar buttons get sensitivity-only
	 * gating — they always remain visible so the toolbar shape
	 * doesn't reshape between connections. Three buttons, three
	 * independent decisions:
	 *
	 *   news_btn   (legacy News): enabled when the account has
	 *               HL_ACCESS_READ_NEWS. The legacy news file
	 *               protocol exists on every Hotline server
	 *               version including 1.5+ — Badmoon (1.9) and
	 *               other modern servers serve both legacy and
	 *               threaded news side by side, so don't gate this
	 *               on server version.
	 *   post_btn   (legacy Post): enabled when the account has
	 *               HL_ACCESS_POST_NEWS. Same multi-version
	 *               availability as news_btn.
	 *   news15_btn (threaded News): enabled on 1.5+ servers when
	 *               the account has HL_ACCESS_READ_NEWS. mhxd's
	 *               struct has one read bit gating both legacy and
	 *               threaded news, so the same access bit applies. */
	if (!stat) {
		gtk_widget_set_sensitive (news_btn,   FALSE);
		gtk_widget_set_sensitive (post_btn,   FALSE);
		gtk_widget_set_sensitive (news15_btn, FALSE);
	} else {
		const guint8 *access = (const guint8 *) &sess->htlc.access;
		gboolean can_read  = hl_access_has (access, HL_ACCESS_READ_NEWS);
		gboolean can_post  = hl_access_has (access, HL_ACCESS_POST_NEWS);
		gboolean is_15plus = sess->htlc.version >= 150;

		gtk_widget_set_sensitive (news_btn,   can_read);
		gtk_widget_set_sensitive (post_btn,   can_post);
		gtk_widget_set_sensitive (news15_btn, is_15plus && can_read);
	}
}

/* Phase 5: status_bar is now a GtkLabel (was GtkStatusbar — deprecated
 * in GTK 4.10). The toolbar always replaced the message wholesale, so
 * the message-stack model the GtkStatusbar provided was overhead that
 * earned us nothing. A single gtk_label_set_text per state change
 * does what we want.
 *
 * The label shows the persistent state ("Logged in to ...") for
 * ambient awareness. Important state transitions also fire an
 * AdwToast over the toolbar so the change is visible without the
 * user having to glance at the corner of the window: login success
 * is the canonical positive transition, disconnect-from-connected is
 * the canonical "you've lost connectivity" negative transition. The
 * intermediate "Connecting..." / TCP-connected states are
 * label-only because they're either expected (you just clicked
 * Connect) or short-lived (TCP-connected almost always becomes
 * Logged-in within milliseconds). */
void set_status_bar(int status)
{
	static int last_status = 0;
	const char *fixed = NULL;
	char *fmt = NULL;
	char *toast = NULL;

	if (!status_bar) {
		return;
	}

	switch (status) {
	case -1:
		fmt = g_strdup_printf ("%s %s", _("Connecting to"), server_addr);
		/* Hide any leftover "lost connection" banner — the user
		 * is actively trying to reconnect. */
		toolbar_hide_banner ();
		break;
	case 0:
		fixed = _("Not Connected");
		/* Toast + banner only on a real disconnect — first-launch
		 * state change of 0 -> 0 shouldn't surface a notification,
		 * and neither should a Connect-canceled (last_status == -1). */
		if (last_status == 1 || last_status == 2) {
			toast = g_strdup_printf ("%s %s",
			                         _("Disconnected from"),
			                         server_addr);
			toolbar_show_connection_lost (server_addr);
		}
		break;
	case 1:
		fmt = g_strdup_printf ("%s %s", _("Connected to"), server_addr);
		toolbar_hide_banner ();
		break;
	case 2:
		fmt = g_strdup_printf ("%s %s", _("Logged in to"), server_addr);
		toast = g_strdup (fmt);
		toolbar_hide_banner ();
		break;
	default:
		return;
	}

	gtk_label_set_text (GTK_LABEL (status_bar), fmt ? fmt : fixed);
	if (toast)
		toolbar_show_toast (toast);
	g_free (fmt);
	g_free (toast);
	last_status = status;
}

void changetitlesconnected(session *sess)
{
	char *newstitle;
	char *taskstitle;
	char *chattitle;
	char *userstitle;
	char *tooltitle;

	tooltitle = g_strdup_printf("%s (%s)", _("GtkHx"), server_addr);
	gtk_window_set_title(GTK_WINDOW(sess->toolbar_window), tooltitle);
	g_free(tooltitle);

	if(gtkhx_prefs.geo.news.open) {
			newstitle = g_strdup_printf("%s (%s)", _("News"), server_addr);
			gtk_window_set_title(GTK_WINDOW(sess->news_window), newstitle);
			g_free(newstitle);
		}
	if(gtkhx_prefs.geo.chat.open) {
			chattitle = g_strdup_printf("%s (%s)", _("Chat"), server_addr);
			gtk_window_set_title(GTK_WINDOW(sess->chat_window), chattitle);
			g_free(chattitle);
		}
	if(gtkhx_prefs.geo.users.open) {
			userstitle = g_strdup_printf("%s (%s)", _("Users"), server_addr);
			gtk_window_set_title(GTK_WINDOW(sess->users_window), userstitle);
			g_free(userstitle);
		}
	if(gtkhx_prefs.geo.tasks.open) {
			taskstitle = g_strdup_printf("%s (%s)", _("Tasks"), server_addr);
			gtk_window_set_title(GTK_WINDOW(sess->tasks_window), taskstitle);
			g_free(taskstitle);
		}
}

void changetitlespecific(GtkWidget *widget, char *name)
{
	char *futuretitle;
	futuretitle = g_strdup_printf("%s (%s)", name, server_addr);
	gtk_window_set_title(GTK_WINDOW(widget), futuretitle);
	g_free(futuretitle);
}

void changetitlesdisconnected(session *sess)
{
	if(gtkhx_prefs.geo.news.open) {
		gtk_window_set_title(GTK_WINDOW(sess->news_window), _("News"));
	}
	if(gtkhx_prefs.geo.chat.open) {
		gtk_window_set_title(GTK_WINDOW(sess->chat_window), _("Chat"));
	}
	if(gtkhx_prefs.geo.users.open) {
		gtk_window_set_title(GTK_WINDOW(sess->users_window), _("Users"));
	}
	if(gtkhx_prefs.geo.tasks.open) {
		gtk_window_set_title(GTK_WINDOW(sess->tasks_window), _("Tasks"));
	}

	gtk_window_set_title(GTK_WINDOW(sess->toolbar_window), _("GtkHx"));
}

void close_connected_windows(session *sess)
{
	struct gtkhx_chat *gchat, *prev = NULL;

	if(sess->agreementwin) {
		gtkhx_widget_destroy(sess->agreementwin);
		sess->agreementwin = NULL;
	}
	destroy_gfl_list();


	for(gchat = sess->gchat_list; gchat; gchat = prev) {
		prev = gchat->prev;
		if(gchat->cid) {
			gtkhx_widget_destroy(gchat->window);
			gchat_delete(sess, gchat);
		}
	}
}

/* Phase 5: gtkhx_text_to_utf8 lives in text_util.c now so the unit
 * tests can compile it without dragging in gtkutil's GTK / Adwaita
 * dependency tree. The prototype is forwarded via gtkutil.h →
 * text_util.h so existing #include "gtkutil.h" callers don't need to
 * change. */

/* Phase 5: error_dialog is an AdwAlertDialog now. The old GtkDialog
 * + manual GtkLabel + manual OK button + manual line-wrapping path
 * was the canonical example of "things AdwAlertDialog gives you for
 * free". libadwaita handles line wrapping inside the body text, the
 * dialog is auto-modal to its parent, ESC dismisses, and the visual
 * styling matches every other modern GNOME app's error popup. The
 * old add_break() helper that hand-inserted '\n' every 50 chars is
 * gone with it. */
void error_dialog (char *title, char *msg)
{
	AdwDialog *dlg;

	dlg = adw_alert_dialog_new (title, msg);
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dlg), "ok", _("_OK"));
	adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "ok");
	adw_alert_dialog_set_close_response   (ADW_ALERT_DIALOG (dlg), "ok");

	adw_dialog_present (dlg, GTK_WIDGET (gtkhx_active_window ()));
}

GtkWidget *
gtkhx_grid_new_table (int rows, int cols, gboolean homogeneous)
{
	GtkWidget *grid = gtk_grid_new ();
	(void) rows; (void) cols;  /* Grid grows automatically. */
	if (homogeneous) {
		gtk_grid_set_row_homogeneous    (GTK_GRID (grid), TRUE);
		gtk_grid_set_column_homogeneous (GTK_GRID (grid), TRUE);
	}
	return grid;
}

void
gtkhx_grid_attach_table (GtkGrid *grid, GtkWidget *child,
                         int left, int right,
                         int top,  int bottom,
                         int xoptions, int yoptions,
                         int xpad, int ypad)
{
	if (xoptions & GTK_EXPAND) gtk_widget_set_hexpand (child, TRUE);
	if (yoptions & GTK_EXPAND) gtk_widget_set_vexpand (child, TRUE);
	gtk_widget_set_halign (child, (xoptions & GTK_FILL)
	                       ? GTK_ALIGN_FILL : GTK_ALIGN_CENTER);
	gtk_widget_set_valign (child, (yoptions & GTK_FILL)
	                       ? GTK_ALIGN_FILL : GTK_ALIGN_CENTER);
	if (xpad) {
		gtk_widget_set_margin_start (child, xpad);
		gtk_widget_set_margin_end   (child, xpad);
	}
	if (ypad) {
		gtk_widget_set_margin_top    (child, ypad);
		gtk_widget_set_margin_bottom (child, ypad);
	}
	gtk_grid_attach (grid, child, left, top, right - left, bottom - top);
}

void
gtkhx_grid_attach_table_defaults (GtkGrid *grid, GtkWidget *child,
                                  int left, int right,
                                  int top,  int bottom)
{
	/* Mirror gtk_table_attach_defaults: GTK_EXPAND|GTK_FILL on both
	 * axes, no padding. */
	gtk_widget_set_hexpand (child, TRUE);
	gtk_widget_set_vexpand (child, TRUE);
	gtk_widget_set_halign  (child, GTK_ALIGN_FILL);
	gtk_widget_set_valign  (child, GTK_ALIGN_FILL);
	gtk_grid_attach (grid, child, left, top, right - left, bottom - top);
}

/* Phase 4.2: GtkContainer is gone — dispatch on parent type to the
 * right child setter. Box gets append (call sites that want
 * gtk_box_pack_start semantics should use gtkhx_box_pack instead;
 * this helper covers the simple "put one child in a parent" case
 * gtk_container_add was usually doing). */
void
gtkhx_widget_set_child (GtkWidget *parent, GtkWidget *child)
{
	if (!parent || !child)
		return;

	if (GTK_IS_WINDOW (parent))
		gtk_window_set_child (GTK_WINDOW (parent), child);
	else if (GTK_IS_SCROLLED_WINDOW (parent))
		gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (parent), child);
	else if (GTK_IS_FRAME (parent))
		gtk_frame_set_child (GTK_FRAME (parent), child);
	else if (GTK_IS_BUTTON (parent))
		gtk_button_set_child (GTK_BUTTON (parent), child);
	else if (GTK_IS_BOX (parent))
		gtk_box_append (GTK_BOX (parent), child);
	else if (GTK_IS_VIEWPORT (parent))
		gtk_viewport_set_child (GTK_VIEWPORT (parent), child);
	else if (GTK_IS_POPOVER (parent))
		gtk_popover_set_child (GTK_POPOVER (parent), child);
	else if (GTK_IS_LIST_BOX_ROW (parent))
		gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (parent), child);
	else if (GTK_IS_LIST_BOX (parent))
		gtk_list_box_append (GTK_LIST_BOX (parent), child);
	else
		g_warning ("gtkhx_widget_set_child: unhandled parent type %s",
		           G_OBJECT_TYPE_NAME (parent));
}

void
gtkhx_widget_remove_child (GtkWidget *parent, GtkWidget *child)
{
	if (!parent || !child)
		return;

	if (GTK_IS_BOX (parent))
		gtk_box_remove (GTK_BOX (parent), child);
	else if (GTK_IS_LIST_BOX (parent))
		gtk_list_box_remove (GTK_LIST_BOX (parent), child);
	else
		gtk_widget_unparent (child);
}

static void
gtkhx_box_pack_apply (GtkWidget *child, GtkOrientation orient,
                      gboolean expand, gboolean fill, guint padding)
{
	gboolean horiz = (orient == GTK_ORIENTATION_HORIZONTAL);

	if (expand) {
		if (horiz) gtk_widget_set_hexpand (child, TRUE);
		else       gtk_widget_set_vexpand (child, TRUE);
	}
	if (fill) {
		if (horiz) gtk_widget_set_halign (child, GTK_ALIGN_FILL);
		else       gtk_widget_set_valign (child, GTK_ALIGN_FILL);
	}
	if (padding) {
		if (horiz) {
			gtk_widget_set_margin_start (child, padding);
			gtk_widget_set_margin_end   (child, padding);
		} else {
			gtk_widget_set_margin_top    (child, padding);
			gtk_widget_set_margin_bottom (child, padding);
		}
	}
}

void
gtkhx_box_pack (GtkWidget *box, GtkWidget *child,
                gboolean expand, gboolean fill, guint padding)
{
	if (!box || !child)
		return;
	g_return_if_fail (GTK_IS_BOX (box));
	gtkhx_box_pack_apply (child, gtk_orientable_get_orientation (GTK_ORIENTABLE (box)),
	                      expand, fill, padding);
	gtk_box_append (GTK_BOX (box), child);
}

void
gtkhx_box_pack_end (GtkWidget *box, GtkWidget *child,
                    gboolean expand, gboolean fill, guint padding)
{
	GtkOrientation orient;

	if (!box || !child)
		return;
	g_return_if_fail (GTK_IS_BOX (box));
	orient = gtk_orientable_get_orientation (GTK_ORIENTABLE (box));
	gtkhx_box_pack_apply (child, orient, expand, fill, padding);
	/* Push toward the trailing edge to mimic gtk_box_pack_end. */
	if (orient == GTK_ORIENTATION_HORIZONTAL)
		gtk_widget_set_halign (child, GTK_ALIGN_END);
	else
		gtk_widget_set_valign (child, GTK_ALIGN_END);
	gtk_box_append (GTK_BOX (box), child);
}

/* Phase 4.13: gtk_image_new_from_pixbuf is deprecated in GTK 4.12.
 * The replacement chain is gdk_texture_new_for_pixbuf →
 * gtk_image_new_from_paintable. Wrap that here so the per-site
 * migration is just a name swap (and the returned floating GtkImage
 * has the same ownership story).
 *
 * gdk_texture_new_for_pixbuf is itself deprecated in GTK 4.16 (the
 * suggested replacement loads the pixbuf bytes via GBytes /
 * gdk_memory_texture_new). The pixbuf-first path is what the rest of
 * GtkHx hands us — every icon comes from gdk_pixbuf_new_from_resource —
 * so the GBytes round-trip is a Phase 5 follow-up alongside the
 * GResource-based texture loader. Suppress the inner deprecation
 * here so we keep the strict deprecation gate on. */
GtkWidget *
gtkhx_image_new_from_pixbuf (GdkPixbuf *pixbuf)
{
	GtkWidget *image;
	GdkTexture *tex;

	if (!pixbuf)
		return gtk_image_new ();

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	tex = gdk_texture_new_for_pixbuf (pixbuf);
	G_GNUC_END_IGNORE_DEPRECATIONS

	image = gtk_image_new_from_paintable (GDK_PAINTABLE (tex));
	g_object_unref (tex);
	return image;
}

/* Phase 5: build a button around a GResource pixbuf icon. The
 * default toolbar XPMs are 16x16 pixel art that looks tiny at
 * modern desktop sizes, so scale up by an integer factor with
 * GDK_INTERP_NEAREST (preserves the crisp blocky pixels — bilinear
 * scaling would blur them into mush).
 *
 * Rendering goes through GtkPicture rather than GtkImage:
 * GtkImage has a default min-width / min-height of ~16px from its
 * Adwaita CSS that clamps the visible size regardless of the
 * source paintable's natural dimensions, so a 32x32 pixbuf in a
 * GtkImage would still render at 16x16. GtkPicture doesn't carry
 * those constraints — with set_can_shrink(FALSE) it renders at
 * the paintable's natural size. */
GtkWidget *
gtkhx_pixmap_button (const char *resource_name,
                     const char *tooltip,
                     int         scale,
                     GCallback   cb,
                     gpointer    user_data)
{
	GtkWidget *btn = gtk_button_new ();
	GdkPixbuf *src, *use_pb;
	GdkTexture *tex;
	GtkWidget *picture;

	src = gdk_pixbuf_new_from_resource (resource_name, NULL);
	if (src && scale > 1) {
		int w = gdk_pixbuf_get_width  (src) * scale;
		int h = gdk_pixbuf_get_height (src) * scale;
		use_pb = gdk_pixbuf_scale_simple (src, w, h, GDK_INTERP_NEAREST);
		g_object_unref (src);
	} else {
		use_pb = src;
	}

	if (use_pb) {
		G_GNUC_BEGIN_IGNORE_DEPRECATIONS
		tex = gdk_texture_new_for_pixbuf (use_pb);
		G_GNUC_END_IGNORE_DEPRECATIONS
		picture = gtk_picture_new_for_paintable (GDK_PAINTABLE (tex));
		g_object_unref (tex);
		/* set_can_shrink(FALSE) pins the picture at the paintable's
		 * natural size — GtkButton then sizes itself around that. */
		gtk_picture_set_can_shrink (GTK_PICTURE (picture), FALSE);
	} else {
		picture = gtk_picture_new ();
	}
	gtkhx_widget_set_child (btn, picture);

	if (tooltip)
		gtk_widget_set_tooltip_text (btn, tooltip);
	if (cb)
		g_signal_connect (btn, "clicked", cb, user_data);
	g_clear_object (&use_pb);
	return btn;
}

/* Phase 4.2: gtkhx_widget_destroy is gone. Toplevels (GtkWindow) use
 * gtk_window_destroy which tears down the surface and drops refs.
 * Non-toplevels: if the widget has a parent, unparent it (the
 * parent drops its ref); if floating, sink + unref. */
void
gtkhx_widget_destroy (GtkWidget *widget)
{
	GtkWidget *parent;

	if (!widget)
		return;
	if (GTK_IS_WINDOW (widget)) {
		gtk_window_destroy (GTK_WINDOW (widget));
		return;
	}
	parent = gtk_widget_get_parent (widget);
	if (parent) {
		gtk_widget_unparent (widget);
	} else if (g_object_is_floating (widget)) {
		g_object_ref_sink (widget);
		g_object_unref (widget);
	} else {
		g_object_unref (widget);
	}
}
