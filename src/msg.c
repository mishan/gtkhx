/*
 * Copyright (C) 2000-2002 Misha Nasledov <misha@nasledov.com>
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
#include <stdlib.h>
#include <gtk/gtk.h>
#include <adwaita.h>
#include <gdk/gdkkeysyms.h>
#include <netinet/in.h>
#include "hx.h"
#include "chat.h"
#include "gtkhx.h"
#include "gtkutil.h"
#include "history.h"
#include "xtext.h"
#include "plugin.h"
#include "rcv.h"
#include "tasks.h"
#include "connect.h"
#include "toolbar.h"
#include "users.h"
#include "cicn.h"
#include "gtkurl.h"
#include "msg.h"

void
hx_send_msg (struct htlc_conn *htlc, guint16 uid, const char *msg, guint16 len, void *data)
{
	uid = htons(uid);
	task_new(htlc, RCV_TASK_FN(rcv_task_msg), data, 0, data ? data : "msg");
	hlwrite(htlc, HTLC_HDR_MSG, 0, 2,
		HTLC_DATA_UID, 2, &uid,
		HTLC_DATA_MSG, len, msg);
}

struct msgwin *msg_list;
void msg_output (char *name, guint16 uid, char *buf);

static void msgwin_delete (struct msgwin *msg)
{
	if (msg->next)
		msg->next->prev = msg->prev;
	if (msg->prev)
		msg->prev->next = msg->next;
	if (msg == msg_list)
		msg_list = msg->prev;

	g_free(msg->name);
	g_free(msg->uid);
	g_free(msg);
}


struct msgwin *msgwin_with_uid (guint16 uid)
{
	struct msgwin *msg;


	for (msg = msg_list; msg; msg = msg->prev) {
		if (*(msg->uid) == uid)
			return msg;

	}


	return 0;
}

static void msg_input_activate (GtkWidget *widget, gpointer data);

/* Phase 4.5: GTK 4 widgets don't emit key-press-event; keyboard input
 * comes via a GtkEventControllerKey installed on the widget. The
 * "key-pressed" signal carries (controller, keyval, keycode, state,
 * user_data). Returning TRUE inhibits further propagation, FALSE lets
 * the GtkTextView's default text input proceed (used here for the
 * Shift+Return → newline path and for ordinary typing). */
static gboolean
msg_input_key_pressed (GtkEventControllerKey *ctrl, guint keyval,
                       guint keycode, GdkModifierType state, gpointer user_data)
{
	GtkWidget *widget = gtk_event_controller_get_widget (
		GTK_EVENT_CONTROLLER (ctrl));
	GtkTextView *text;
	GtkTextBuffer *buf;
	HIST_ENTRY *hent = NULL;
	struct msgwin *msg = g_object_get_data (G_OBJECT (widget), "msg");
	(void) keycode; (void) user_data;

	if (!msg)
		return FALSE;

	text = GTK_TEXT_VIEW (widget);
	buf  = gtk_text_view_get_buffer (text);

	if (state & GDK_CONTROL_MASK) {
		switch (keyval) {
		case 'k':
		case 'K':
			create_connect_window (0, &the_session);
			return TRUE;
		}
	}
	else if ((state & GDK_SHIFT_MASK) && keyval == GDK_KEY_Return) {
		/* Insert a linebreak if shift is held — let GtkTextView default. */
		return FALSE;
	}
	else if (keyval == GDK_KEY_Return) {
		GtkTextIter start, end;
		char *line;

		gtk_text_buffer_get_start_iter (buf, &start);
		gtk_text_buffer_get_end_iter   (buf, &end);
		line = gtk_text_buffer_get_text (buf, &start, &end, FALSE);
		add_history (msg->history, line);
		using_history (msg->history);
		g_free (line);

		msg_input_activate (widget, msg->uid);
		return TRUE;
	}
	else if (keyval == GDK_KEY_Up) {
		hent = previous_history (msg->history);
	}
	else if (keyval == GDK_KEY_Down) {
		hent = next_history (msg->history);
	}

	if (hent) {
		GtkTextIter end;

		gtk_text_buffer_set_text (buf, hent->line, strlen (hent->line));
		gtk_text_buffer_get_end_iter (buf, &end);
		gtk_text_buffer_place_cursor (buf, &end);
		return TRUE;
	}

	return FALSE;
}

/* Phase 4.5: msg_update_trans was a configure-event handler that
 * forced an xtext refresh on resize so transparency would track the
 * new window position. configure-event is gone in GTK 4, and Wayland
 * doesn't expose true window-relative transparency anyway. */

static void msg_input_activate (GtkWidget *widget, gpointer data)
{
	GtkTextView *text;
	GtkTextBuffer *buf;
	GtkTextIter start, end;
	guint len;
	char *termed_buf = NULL;
	guint16 *uid = data;

	text = GTK_TEXT_VIEW(widget);
	buf = gtk_text_view_get_buffer(text);
	gtk_text_buffer_get_start_iter(buf, &start);
	gtk_text_buffer_get_end_iter(buf, &end);
	termed_buf = gtk_text_buffer_get_text(buf, &start, &end, FALSE);

	gtk_text_buffer_delete(buf, &start, &end);

	if(termed_buf[0] == 0) {
		g_free(termed_buf);
		return;
	}

	/* send the plugins information that we're sending a private message
	   with content termed_buf to uid */
#ifdef USE_PLUGIN
	if(EMIT_SIGNAL(XP_SND_MSG, &the_session, termed_buf, &uid, 0, 0, 0) == 1) {
		return;
	}
#endif
	len = strlen(termed_buf);
	msg_output(the_session.htlc.name, *uid, termed_buf);
	LF2CR(termed_buf, len);
	hx_send_msg(&the_session.htlc, *uid, termed_buf, len, 0);
	g_free(termed_buf);
}


/* Phase 5: header pane above the PM chat that mirrors the bits of
 * the recipient's identity that the global user list shows — icon,
 * name, idle/admin status. Built from the cached hx_user the chat
 * keeps for that uid; falls back to the name we were created with
 * (and uid only) when the user has already left the public chat by
 * the time the PM window opens.
 *
 * Colour: regular users (slot 0) intentionally don't get a foreground
 * override, so the GTK theme's default colour kicks in and reads on
 * both light and dark themes — same rule user_color_gdk() applies in
 * the user list. Admins / idle / admin-idle get the matching colour
 * from gdk_user_colors[] applied as a Pango <span> on the bold name.
 *
 * Icon: load_icon() yields a GdkPixbuf out of the Mac-classic cicn
 * resource bundle. We feed it to GtkImage at default 32px pixel size,
 * scaled if the source isn't already that. When the icon is unknown
 * (server iconset doesn't ship it, or user picked an icon we don't
 * have) the image clears to nothing so the layout doesn't sag. */
/* Low-level renderer: take the recipient's display name + status
 * triple as explicit values and paint the info pane. Both public
 * entry points below funnel through here. `display_name == NULL` and
 * `have_status == FALSE` together mean "we don't know who this
 * person is" — render the bare uid + fallback name from create_msgwin
 * with no Admin/Guest line. */
static void
msg_apply_user_view (struct msgwin *msg,
                     const char *display_name,
                     guint16 icon, guint16 color,
                     gboolean have_status)
{
	GdkRGBA *rgba = user_color_gdk (color);
	char *name_esc;
	char *markup;
	GdkPixbuf *pixbuf = NULL;
	GdkPixbuf *unused_mask = NULL;

	if (!msg || !msg->info_label || !msg->info_image)
		return;
	if (!display_name || !*display_name)
		display_name = msg->name ? msg->name : "";

	name_esc = g_markup_escape_text (display_name, -1);

	if (have_status) {
		const char *status = (color >= 2) ? _("Admin") : _("Guest");
		const char *away   = (color % 2)  ? _(" (Away)") : "";
		if (rgba) {
			/* gdk_user_colors stores values in [0..1] floats;
			 * convert to the 8-bit hex Pango wants. */
			char hex[8];
			g_snprintf (hex, sizeof (hex), "#%02x%02x%02x",
			            (int) (rgba->red   * 255.0 + 0.5),
			            (int) (rgba->green * 255.0 + 0.5),
			            (int) (rgba->blue  * 255.0 + 0.5));
			markup = g_strdup_printf (
				"<span foreground=\"%s\"><b>%s</b></span>\n"
				"<small>UID %u · Icon %u · %s%s</small>",
				hex, name_esc, *msg->uid, icon, status, away);
		} else {
			markup = g_strdup_printf (
				"<b>%s</b>\n"
				"<small>UID %u · Icon %u · %s%s</small>",
				name_esc, *msg->uid, icon, status, away);
		}
	} else {
		markup = g_strdup_printf (
			"<b>%s</b>\n<small>UID %u</small>",
			name_esc, *msg->uid);
	}

	gtk_label_set_markup (GTK_LABEL (msg->info_label), markup);
	g_free (markup);
	g_free (name_esc);

	/* Always reload — icon ID can change when the user changes their
	 * icon mid-conversation. load_icon falls back through the icon
	 * file chain; pixbuf comes back NULL when nothing matches and we
	 * just blank the GtkImage in that case. GTK 4 deprecates
	 * gtk_image_set_from_pixbuf; wrap the pixbuf in a GdkTexture and
	 * feed it to set_from_paintable instead. The texture wrapper
	 * itself (gdk_texture_new_for_pixbuf) was deprecated in GTK 4.16
	 * — same migration story as gtkhx_image_new_from_pixbuf in
	 * gtkutil.c, suppress here until we move icons off GdkPixbuf. */
	load_icon (msg->info_image, icon, &icon_files, 1,
	           &pixbuf, &unused_mask);
	if (pixbuf) {
		GdkTexture *tex;
		G_GNUC_BEGIN_IGNORE_DEPRECATIONS
		tex = gdk_texture_new_for_pixbuf (pixbuf);
		G_GNUC_END_IGNORE_DEPRECATIONS
		gtk_image_set_from_paintable (GTK_IMAGE (msg->info_image),
		                              GDK_PAINTABLE (tex));
		gtk_image_set_pixel_size (GTK_IMAGE (msg->info_image), 32);
		g_object_unref (tex);
	} else {
		gtk_image_clear (GTK_IMAGE (msg->info_image));
	}
}

void
msgwin_refresh_user_info (struct msgwin *msg)
{
	struct chat *pubchat;
	struct hx_user *user = NULL;

	if (!msg)
		return;

	/* The user list is per-chat; the public chat (cid=0) carries the
	 * server-wide list we want here. chat_with_cid is the canonical
	 * "global user list" lookup the rest of the codebase uses
	 * (rcv.c, commands.c, users.c). The chat_list pointer can be
	 * reset to all-zeros mid-disconnect (network.c:215), so guard
	 * the user_list deref against the brief NULL window too. */
	pubchat = chat_with_cid (&the_session, 0);
	if (pubchat && pubchat->user_list)
		user = hx_user_with_uid (pubchat->user_list, *msg->uid);

	if (user)
		msg_apply_user_view (msg, user->name, user->icon, user->color,
		                     TRUE);
	else
		msg_apply_user_view (msg, NULL, 0, 0, FALSE);
}

/* Bypass the cache lookup. Called from users.c::user_change with the
 * NEW name/icon/color values straight off the wire — at that point
 * rcv.c hasn't yet patched them onto the cached hx_user struct (the
 * rename-detection comparison at rcv.c:338-339 needs the old values
 * to still be there when user_change returns), so a cache-based
 * refresh would render stale data. Take the new values directly. */
void
msgwin_apply_user_change (struct msgwin *msg,
                          const char *nam,
                          guint16 icon, guint16 color)
{
	msg_apply_user_view (msg, nam, icon, color, TRUE);
}

static struct msgwin *create_msg (guint16 _uid, char *name)
{
	struct msgwin *msg;
	guint16 *uid = g_malloc(sizeof(guint16));
	*uid = _uid;

 	msg = g_malloc(sizeof(struct msgwin));

	msg->next = 0;
	msg->prev = msg_list;
	if(msg_list) {
		msg_list->next = msg;
	}
	msg->name = g_strdup(name);
	msg->uid = uid;
	
	msg->history = history_new();

	msg->window = gtk_window_new();
	/* Phase 5: AdwHeaderBar across all GtkHx secondary windows for
	 * consistent chrome with the toolbar / chat / news / files /
	 * preview / agreement windows. The msg window had been left as
	 * a bare GtkWindow with the system default titlebar. */
	gtk_window_set_titlebar (GTK_WINDOW (msg->window), adw_header_bar_new ());
	{
		gchar *fontname = pango_font_description_to_string (gtkhx_font_desc);
		msg->outputbuf = gtk_xtext_new (colors, 0);
		gtk_xtext_set_font (GTK_XTEXT (msg->outputbuf), fontname);
		g_free (fontname);
	}
	GTK_XTEXT(msg->outputbuf)->wordwrap = gtkhx_prefs.word_wrap;
	GTK_XTEXT(msg->outputbuf)->urlcheck_function = word_check;
	GTK_XTEXT(msg->outputbuf)->max_lines = gtkhx_prefs.xbuf_max;
	g_signal_connect (msg->outputbuf, "word_click",
	                  G_CALLBACK (gtkurl_xtext_word_click), NULL);

	msg->vscroll = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, GTK_XTEXT(msg->outputbuf)->adj);
	msg->inputbuf = gtk_text_view_new();

	gtkhx_apply_text_style(msg->inputbuf);
	gtk_text_view_set_editable(GTK_TEXT_VIEW(msg->inputbuf), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(msg->inputbuf), GTK_WRAP_WORD);

	g_object_set_data(G_OBJECT(msg->inputbuf), "msg", msg);
	g_object_set_data(G_OBJECT(msg->inputbuf), "sess", &the_session);
	/* Note: GtkTextView has no "activate" signal — Return is dispatched
	 * from msg_input_key_pressed, which calls msg_input_activate().
	 * Phase 4.5: key-press-event is gone in GTK 4; install a
	 * GtkEventControllerKey on the input view instead. */
	{
		GtkEventController *kctrl = gtk_event_controller_key_new ();
		g_signal_connect (kctrl, "key-pressed",
		                  G_CALLBACK (msg_input_key_pressed), uid);
		gtk_widget_add_controller (msg->inputbuf, kctrl);
	}
	

	msg_list = msg;
	return msg;
}

/* Phase 4.5: GTK 4 fires "close-request" on (GtkWindow *, gpointer)
 * returning TRUE to inhibit close, FALSE to allow default destroy.
 * Just unlink the msg from the list — the framework destroys the
 * widget. */
static gboolean destroy_msgwin (GtkWindow *window, gpointer data)
{
	struct msgwin *msg = g_object_get_data(G_OBJECT(window), "msg");
	(void) data;
	msgwin_delete(msg);
	return FALSE;
}


struct msgwin *create_msgwin (guint16 uid, char *name)
{
	GtkWidget *hbox;
	GtkWidget *outputframe, *inputframe;
	GtkWidget *vpane;
	GtkWidget *info_box, *outer_vbox;
	struct msgwin *msg;
	char *title;

	msg = create_msg(uid, name);

	title = g_strdup_printf("%s (%u)", name, uid);
	gtk_window_set_title(GTK_WINDOW(msg->window), title);
	g_free(title);

	/* Phase 5: switch from set_size_request (which sets BOTH min
	 * AND natural size in GTK 4) to set_default_size for the
	 * initial window size, and drop the forced 500x400 minimum on
	 * the inner hbox. The old combination was the cause of the
	 * "chat appears cut off in top-left until resized" bug — the
	 * inner hbox demanded 400px tall but the paned was giving it
	 * 230px, so xtext rendered into a 400px-tall surface that got
	 * clipped to 230px visible. */
	gtk_window_set_default_size(GTK_WINDOW(msg->window), 460, 340);
	gtk_window_set_resizable(GTK_WINDOW(msg->window), TRUE);
	(gtk_widget_set_margin_start(msg->window, 0), gtk_widget_set_margin_end(msg->window, 0), gtk_widget_set_margin_top(msg->window, 0), gtk_widget_set_margin_bottom(msg->window, 0));
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	outputframe = gtk_frame_new(0);
	gtkhx_widget_set_child(outputframe, hbox);
	gtkhx_box_pack(hbox, msg->outputbuf, 1, 1, 0);
	gtkhx_box_pack(hbox, msg->vscroll, 0, 0, 0);

	inputframe = gtk_frame_new(0);
	gtkhx_widget_set_child(inputframe, msg->inputbuf);
	gtk_widget_set_size_request(inputframe, 0, 60);

	vpane = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
	gtk_paned_set_start_child(GTK_PANED(vpane), outputframe);
	gtk_paned_set_end_child(GTK_PANED(vpane), inputframe);
	/* Window grows → output area takes the extra room; input
	 * stays at whatever size the user picks. shrink_end=FALSE
	 * keeps the user from collapsing the input below 60px. */
	gtk_paned_set_resize_start_child(GTK_PANED(vpane), TRUE);
	gtk_paned_set_resize_end_child  (GTK_PANED(vpane), FALSE);
	gtk_paned_set_shrink_end_child  (GTK_PANED(vpane), FALSE);
	/* Initial divider position. With default 340px window and
	 * ~30px headerbar + 10px margins, the paned area is ~290px;
	 * 220 puts ~70px below the divider for the input. */
	gtk_paned_set_position(GTK_PANED(vpane), 220);
	(gtk_widget_set_margin_start(vpane, 5), gtk_widget_set_margin_end(vpane, 5), gtk_widget_set_margin_top(vpane, 5), gtk_widget_set_margin_bottom(vpane, 5));

	/* Recipient info pane: small horizontal strip with icon + name +
	 * status sitting between the headerbar and the chat paned. The
	 * vbox just below is the new top-level child of the window —
	 * info pane on top, paned filling the rest. */
	msg->info_image = gtk_image_new ();
	gtk_image_set_pixel_size (GTK_IMAGE (msg->info_image), 32);
	gtk_widget_set_size_request (msg->info_image, 32, 32);

	msg->info_label = gtk_label_new (NULL);
	gtk_label_set_xalign (GTK_LABEL (msg->info_label), 0.0);
	gtk_label_set_yalign (GTK_LABEL (msg->info_label), 0.5);
	gtk_label_set_use_markup (GTK_LABEL (msg->info_label), TRUE);
	gtk_label_set_ellipsize (GTK_LABEL (msg->info_label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand (msg->info_label, TRUE);

	info_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_widget_set_margin_start  (info_box, 10);
	gtk_widget_set_margin_end    (info_box, 10);
	gtk_widget_set_margin_top    (info_box, 6);
	gtk_widget_set_margin_bottom (info_box, 4);
	gtk_box_append (GTK_BOX (info_box), msg->info_image);
	gtk_box_append (GTK_BOX (info_box), msg->info_label);

	outer_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append (GTK_BOX (outer_vbox), info_box);
	gtk_box_append (GTK_BOX (outer_vbox),
	                gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
	gtk_widget_set_vexpand (vpane, TRUE);
	gtk_box_append (GTK_BOX (outer_vbox), vpane);

	gtkhx_widget_set_child(msg->window, outer_vbox);

	/* Populate from the cached user list now that the widgets exist. */
	msgwin_refresh_user_info (msg);


	gtk_window_present(GTK_WINDOW(msg->window));

	g_object_set_data(G_OBJECT(msg->window), "msg", msg);
	g_signal_connect(msg->window, "close-request", G_CALLBACK(destroy_msgwin), 0);
	init_keyaccel(msg->window);

	gtk_widget_grab_focus(msg->inputbuf);


	/* Phase 4.4: GdkWindow / gdk_window_lower / gtk_widget_get_window
	 * are gone in GTK 4. The "showback" pref used to lower the new
	 * message window to the back of the stack so it didn't steal focus.
	 * Wayland doesn't let clients re-order themselves in the stack, so
	 * the pref no longer has a meaningful implementation; the window
	 * comes up at whatever position the compositor picks. */

	return msg;
}


void msg_output (char *name, guint16 uid, char *buf)
{
	struct msgwin *msg;
	char *text;
	char *ptr;
	char *cr;
	int brack_col;


	brack_col = !(strcmp(name, the_session.htlc.name)) ? 13: 12;


	text = g_strdup_printf("\003%d<\003%s\003%d>\003 %s", brack_col, name, brack_col, buf);

	msg = msgwin_with_uid(uid);
	if(!msg) {
		msg = create_msgwin(uid, name);
	}
	ptr = text;

	cr = strchr(text, '\n');
	if(cr) {
		while(1) {
			xprintline(msg->outputbuf, text, cr-text);


			text = cr + 1;
			if(*text == 0) {
				break;
			}
			cr = strchr(text, '\n');
			if(!cr) {
				xprintline(msg->outputbuf, text, -1);
				break;
			}
		}
	}
	else {
		xprintline(msg->outputbuf, text, -1);
	}

	g_free(ptr);
}


/* Phase 5: short broadcasts go through toolbar_show_toast, long ones
 * through an AdwAlertDialog with a scrolled GtkTextView extra child.
 *
 * The split exists because servers use HTLS_HDR_MSG_BROADCAST for two
 * different things:
 *   - Real admin announcements ("Server going down in 5 minutes")
 *     that the user really should see and acknowledge.
 *   - Rate-limit / auto-rejection notes ("0 command(s) at a time,
 *     please.") that are short, automated, and not worth a modal
 *     dialog the user has to click through.
 *
 * Length is a usable proxy: short = automated nag, long = real
 * announcement. The 160-char cutoff is heuristic but matches the
 * shape of what we've seen in the wild on hlserver.com et al. */
#define BROADCAST_TOAST_MAX 160

void broadcastmsg(char *text)
{
	AdwDialog *dialog;
	GtkWidget *textbox, *scroll;
	GtkTextBuffer *tbuf;
	gsize len = text ? strlen (text) : 0;

	if (len <= BROADCAST_TOAST_MAX && !strchr (text, '\n')) {
		toolbar_show_toast (text);
		return;
	}

	dialog = adw_alert_dialog_new (_("Broadcast"), NULL);
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "ok", _("_OK"));
	adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "ok");
	adw_alert_dialog_set_close_response   (ADW_ALERT_DIALOG (dialog), "ok");

	textbox = gtk_text_view_new ();
	gtk_text_view_set_editable (GTK_TEXT_VIEW (textbox), FALSE);
	gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (textbox), FALSE);
	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (textbox), GTK_WRAP_WORD);
	tbuf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (textbox));
	gtk_text_buffer_set_text (tbuf, text, strlen (text));

	scroll = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
	                                GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request (scroll, 300, 220);
	gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), textbox);

	adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), scroll);

	adw_dialog_present (dialog,
	                    toolbar_window ? GTK_WIDGET (toolbar_window) : NULL);
}
