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

/* Phase 4.11: GtkAccelGroup / gtk_accel_group_new /
 * gtk_widget_add_accelerator / gtk_window_add_accel_group are gone
 * in GTK 4 — replaced by GtkShortcutController plus GtkShortcut
 * instances bound to GtkKeyvalTrigger triggers and GtkCallbackAction
 * actions.
 *
 * The original behavior: every window the user opens gets Ctrl+K
 * (connect dialog) and Ctrl+Q (quit) wired to the toolbar's
 * connect_btn and quit_btn "clicked" signal. We preserve that by
 * installing a fresh per-window controller whose callbacks emit
 * "clicked" on those buttons directly. (A future Phase 5 cleanup
 * could move these to GtkApplication-level GActions with
 * gtk_application_set_accels_for_action — that's the modern idiom
 * but requires also reworking the toolbar buttons to fire actions
 * instead of "clicked", which is more invasive.) */

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
	(void) w; (void) args; (void) data;
	if (quit_btn)
		g_signal_emit_by_name (quit_btn, "clicked");
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

void setbtns(session *sess, int stat)
{
	if(gtkhx_prefs.geo.users.open) {
		gtk_widget_set_sensitive(msgbtn, stat);
		gtk_widget_set_sensitive(kickbtn, stat);
		gtk_widget_set_sensitive(infobtn, stat);
		gtk_widget_set_sensitive(banbtn, stat);
		gtk_widget_set_sensitive(chatbtn, stat);
		gtk_widget_set_sensitive(ignobtn, stat);
	}
	if(gtkhx_prefs.geo.news.open) {
		gtk_widget_set_sensitive(sess->postButton, stat);

		gtk_widget_set_sensitive(sess->reloadButton, stat);

	}

	gtk_widget_set_sensitive(files_btn, stat);
	gtk_widget_set_sensitive(usermod_btn, stat);
	gtk_widget_set_sensitive(usernew_btn, stat);
	
	if(!stat) {
		gtk_widget_set_sensitive(news15_btn, stat);
		gtk_widget_set_sensitive(post_btn, stat);
	}
	else if(sess->htlc.version >= 150) {
		gtk_widget_set_sensitive(news15_btn, TRUE);
		gtk_widget_set_sensitive(post_btn, FALSE);
	}
	else if(sess->htlc.version < 150) {
		gtk_widget_set_sensitive(post_btn, TRUE);
		gtk_widget_set_sensitive(news15_btn, FALSE);
	}
}

void set_status_bar(int status)
{
	if(!status_bar) {
		return;
	}

	/* XXX: switch statement here */
	if(status == -1) {
		char *str;

		gtk_statusbar_remove(GTK_STATUSBAR(status_bar), context_status, status_msg);
		str = g_strdup_printf("%s %s", _("Connecting to"), server_addr);
		status_msg = gtk_statusbar_push(GTK_STATUSBAR(status_bar), context_status, str);
		g_free(str);
	}
	else if(!status) {
		gtk_statusbar_remove(GTK_STATUSBAR(status_bar), context_status, status_msg);
		status_msg = gtk_statusbar_push(GTK_STATUSBAR(status_bar), context_status, _("Not Connected"));
	}
	else if(status == 1) {
		char *str = g_strdup_printf("%s %s", _("Connected to"), server_addr);
		gtk_statusbar_remove(GTK_STATUSBAR(status_bar), context_status, status_msg);
		status_msg = gtk_statusbar_push(GTK_STATUSBAR(status_bar), context_status, str);
		g_free(str);
	}
	else if(status == 2) {
		char *str = g_strdup_printf("%s %s", _("Logged in to"), server_addr);
		gtk_statusbar_remove(GTK_STATUSBAR(status_bar), context_status, status_msg);
		status_msg = gtk_statusbar_push(GTK_STATUSBAR(status_bar), context_status, str);
		g_free(str);
	}
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

char *add_break(char *msg, int pos)
{
	size_t len = strlen(msg);	

	msg = g_realloc(msg, len+1);
	memmove(&msg[pos+1], &msg[pos], len-pos);
	msg[pos] = '\n';

	return msg;
}

void error_dialog (char *title, char *msg)
{
    GtkWidget *label;
    GtkWidget *dialog;
    GtkWidget *okbutton;
	char *message = g_strdup(msg);
	size_t len = strlen(message);
	int i;

	/* insert a line break at every 50 chars, otherwise the message will just
	   run off */
	if(len > 50) {
		for(i = 0; i < len; i++) {
			if((!(i%50)) && i) {
				message = add_break(message, i);
				len++;
			}
		}
	}

    dialog = gtk_dialog_new();

    gtk_window_set_title(GTK_WINDOW(dialog), title);
    (gtk_widget_set_margin_start(dialog, 5), gtk_widget_set_margin_end(dialog, 5), gtk_widget_set_margin_top(dialog, 5), gtk_widget_set_margin_bottom(dialog, 5));
    label = gtk_label_new (message);
    gtk_widget_set_size_request(dialog, 250, 200);

    gtkhx_box_pack(gtk_dialog_get_content_area(GTK_DIALOG (dialog)), label, TRUE, TRUE, 0);

    okbutton = gtk_button_new_with_label ("Ok");

    g_signal_connect_swapped (okbutton, "clicked", 
							   (GCallback)gtkhx_widget_destroy, 
							   dialog);

    /* Phase 4.2: gtk_widget_set_can_default removed */

    gtkhx_box_pack(gtkhx_dialog_action_area(GTK_DIALOG(dialog)), okbutton, TRUE, TRUE, 0);


    /* Phase 4.2: gtk_widget_grab_default removed (use gtk_window_set_default_widget if needed) */

    gtk_widget_show(dialog);
	g_free(message);
}

/* Phase 4.2: gtk_dialog_get_action_area is fully removed in GTK 4
 * (the action area widget is gone too). Synthesize one: a horizontal
 * GtkBox attached to the bottom of the dialog's content area on
 * first call, cached on the dialog via g_object_set_data so repeat
 * calls return the same box. Callers' gtkhx_box_pack(area, btn, ...)
 * just append to this box. */
GtkWidget *
gtkhx_dialog_action_area (GtkDialog *dialog)
{
	GtkWidget *area;

	if (!dialog)
		return NULL;
	area = g_object_get_data (G_OBJECT (dialog), "gtkhx-action-area");
	if (!area) {
		GtkWidget *content = gtk_dialog_get_content_area (dialog);
		area = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
		gtk_widget_set_halign (area, GTK_ALIGN_END);
		gtk_widget_set_margin_top (area, 6);
		if (GTK_IS_BOX (content))
			gtk_box_append (GTK_BOX (content), area);
		g_object_set_data (G_OBJECT (dialog), "gtkhx-action-area", area);
	}
	return area;
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
