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
#include <gtk/gtk.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include "hx.h"
#include "tasks.h"
#include "rcv.h"
#include "gtk_hlist.h"

#define NACCESS	28
void create_useredit_window (char *login, int new);


void
hx_useredit_create (struct htlc_conn *htlc, const char *login, const char *pass, const char *name, hl_access_bits access)
{
	char elogin[32], epass[32];
	guint16 llen, plen;

	llen = strlen(login);
	hl_encode(elogin, login, llen);
	if (!*pass) {
		plen = 1;
		epass[0] = 0;
	} else {
		plen = strlen(pass);
		hl_encode(epass, pass, plen);
	}
	task_new(htlc, 0, 0, 0, "user create");
	hlwrite(htlc, HTLC_HDR_ACCOUNT_MODIFY, 0, 4,
		HTLC_DATA_LOGIN, llen, elogin,
		HTLC_DATA_PASSWORD, plen, epass,
		HTLC_DATA_NAME, strlen(name), name,
		HTLC_DATA_ACCESS, 8, &access);
}

void
hx_useredit_delete (struct htlc_conn *htlc, const char *login)
{
	char elogin[32];
	guint16 llen;

	llen = strlen(login);
	hl_encode(elogin, login, llen);
	task_new(htlc, 0, 0, 0, "user delete");
	hlwrite(htlc, HTLC_HDR_ACCOUNT_DELETE, 0, 1,
		HTLC_DATA_LOGIN, llen, elogin);
}

void
hx_useredit_open (struct htlc_conn *htlc, const char *login, void (*fn)(void *, const char *, const char *, const char *, const hl_access_bits), void *uesp)
{
	struct uesp_fn *uespfn;

	uespfn = g_malloc(sizeof(struct uesp_fn));
	uespfn->uesp = uesp;
	uespfn->fn = fn;
	task_new(htlc, rcv_task_user_open, uespfn, 0, "user open");
	hlwrite(htlc, HTLC_HDR_ACCOUNT_READ, 0, 1,
		HTLC_DATA_LOGIN, strlen(login), login);
}


struct access_name {
	char bitno;
	char *name;
} access_names[] = {
#define ENTRY(x,y) {(x!=-1)? \
						(63-((G_BYTE_ORDER==G_BIG_ENDIAN)? \
							 x: \
							 (x%8)+8*(7-x/8))): \
						-1, \
					y}
	ENTRY( -1, "File Privileges" ),
	ENTRY(1, "Can Upload Files" ),
	ENTRY(2, "Can Download Files" ),
	ENTRY(4, "Can Move Files" ),
	ENTRY(8, "Can Move Folders" ),
	ENTRY(5, "Can Create Folders"),
	ENTRY(0, "Can Delete Files"),
	ENTRY(6, "Can Delete Folders"),
	ENTRY(3, "Can Rename Files"),
	ENTRY(7, "Can Rename Folders"),
	ENTRY(28, "Can Comment Files"),
	ENTRY(29, "Can Comment Folders"),
	ENTRY(31, "Can Make Aliases"),
	ENTRY(25, "Can Upload Anywhere"),
	ENTRY(30, "Can View Drop Boxes"),
	ENTRY(-1, "Chat Privileges"),
	ENTRY(9, "Can Read Chat"),
	ENTRY(10, "Can Send Chat"),
	ENTRY(-1, "News"),
	ENTRY(20, "Can Read News"),
	ENTRY(21, "Can Post News"),
	ENTRY(-1, "User Privileges"),
	ENTRY(14, "Can Create Users"),
	ENTRY(15, "Can Delete Users"),
	ENTRY(16, "Can Read Users"),
	ENTRY(17, "Can Modify Users"),
	ENTRY(22, "Can Disconnect Users"),
	ENTRY(23, "Cannot Be Disconnected"),
	ENTRY(24, "Can Get User Info"),
	ENTRY(26, "Can Use Any Name"),
	ENTRY(27, "Cannot Be Shown Agreement"),
	ENTRY(-1, "Admin Privileges"),
	ENTRY(32, "Can Broadcast"),
#undef ENTRY
};

#define test_bit(buf,bitno) ((buf>>bitno)&1)
#define set_bit(buf,bitno) (buf|=((long long)1<<bitno))
#define unset_bit(buf,bitno) (buf&=~((long long)1<<bitno))

struct access_widget {
	int bitno;
	GtkWidget *widget;
};

struct useredit_session {
	hl_access_bits access_buf;
	char name[32];
	char login[32];
	char pass[32];
	GtkWidget *window;
	GtkWidget *name_entry;
	GtkWidget *login_entry;
	GtkWidget *pass_entry;

	struct access_widget access_widgets[NACCESS];
};

static void
user_open (void *__uesp, const char *name, const char *login, const char *pass, const hl_access_bits access)
{
	struct useredit_session *ues = (struct useredit_session *)__uesp;
	unsigned int i;
	int on;

	gtk_entry_set_text(GTK_ENTRY(ues->name_entry), name);
	gtk_entry_set_text(GTK_ENTRY(ues->login_entry), login);
	gtk_entry_set_text(GTK_ENTRY(ues->pass_entry), pass);
	strcpy(ues->name, name);
	strcpy(ues->login, login);
	strcpy(ues->pass, pass);
	ues->access_buf = access;
	for (i = 0; i < NACCESS; i++) {
		on = test_bit(ues->access_buf, ues->access_widgets[i].bitno);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ues->access_widgets[i].widget), on);
	}
}
static void
useredit_login (char *login, struct useredit_session *ues)
{
	size_t len;


	if (&sessions[0].htlc)
		hx_useredit_open(&sessions[0].htlc, login, user_open, ues);
	len = strlen(login);
	if (len > 31)
		len = 31;
	memcpy(ues->login, login, len+1);
}

static void useredit_open_close(GtkWidget *widget, gpointer data)
{
	gtk_widget_destroy(GTK_WIDGET(data));
}

static void useredit_open(GtkWidget *widget, gpointer data)
{
	GtkWidget *loginentry = (GtkWidget *)gtk_object_get_data(GTK_OBJECT(widget), "login");
	char *login = gtk_entry_get_text(GTK_ENTRY(loginentry));

	create_useredit_window(login, 0);
	gtk_widget_destroy(GTK_WIDGET(data));
}

static void useredit_name_pass(GtkWidget *name_entry, GtkWidget *pass_entry, struct useredit_session *ues)
{
	char *name;
	char *pass;
	size_t len;

	name = gtk_entry_get_text(GTK_ENTRY(name_entry));
	len = strlen(name);
	if (len > 31)
		len = 31;
	memcpy(ues->name, name, len);
	ues->name[len] = 0;

	pass = gtk_entry_get_text(GTK_ENTRY(pass_entry));
	len = strlen(pass);
	if (len > 31)
		len = 31;
	memcpy(ues->pass, pass, len);
	ues->pass[len] = 0;
}

static void useredit_get_login(GtkWidget *login_entry, struct useredit_session *ues)
{
	char *login;
	size_t len;

	login = gtk_entry_get_text(GTK_ENTRY(login_entry));
	len = strlen(login);
	if (len > 31)
		len = 31;
	memcpy(ues->login, login, len);
	ues->login[len] = 0;
}

static void
useredit_chk_activate (GtkWidget *widget, gpointer data)
{
	struct useredit_session *ues = (struct useredit_session *)data;
	unsigned int i;
	int bitno;

	for (i = 0; i < NACCESS; i++) {
		if (ues->access_widgets[i].widget == widget)
			break;
	}
	if (i == NACCESS)
		return;
	bitno = ues->access_widgets[i].bitno;
	if (GTK_TOGGLE_BUTTON(widget)->active)
		set_bit(ues->access_buf, bitno);
	else
		unset_bit(ues->access_buf, bitno);
}

static void
useredit_save (GtkWidget *widget, gpointer data)
{
	struct useredit_session *ues = (struct useredit_session *)data;
	int new = GPOINTER_TO_INT(gtk_object_get_data(GTK_OBJECT(widget), "new"));


	useredit_name_pass(ues->name_entry, ues->pass_entry, ues);
	if(new) {
		useredit_get_login(ues->login_entry, ues);
		new = 0;
	}
	if (&sessions[0].htlc) {
		hx_useredit_create(&sessions[0].htlc, ues->login, ues->pass, ues->name, ues->access_buf);
	}
}

static void
useredit_delete (GtkWidget *widget, gpointer data)
{
	struct useredit_session *ues = (struct useredit_session *)data;

	if (&sessions[0].htlc)
		hx_useredit_delete(&sessions[0].htlc, ues->login);
	gtk_widget_destroy(ues->window);
}

static void
useredit_close (GtkWidget *widget, gpointer data)
{
	struct useredit_session *ues = (struct useredit_session *)data;

	gtk_widget_destroy(ues->window);
}

static void
useredit_destroy (GtkWidget *widget, gpointer data)
{
	/* data is a useredit_session */
	g_free(data);
}


void create_useredit_window (char *login, int new)
{
	GtkWidget *window;
	GtkWidget *usermod_scroll;
	GtkWidget *wvbox;
	GtkWidget *avbox;
	GtkWidget *vbox;
	GtkWidget *frame;
	GtkWidget *info_frame;
	GtkWidget *chk;
	GtkWidget *info_table;
	GtkWidget *btnhbox;
	GtkWidget *wid;
	unsigned int i, awi, nframes = 0;
	struct useredit_session *ues;
	char *title;


	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	if(!new) {
		title = g_strdup_printf("%s: %s", _("User Editor"), login);
		gtk_window_set_title(GTK_WINDOW(window), title);
		g_free(title);
	}
	else {
		gtk_window_set_title(GTK_WINDOW(window), _("User Editor"));
	}

	ues = g_malloc0(sizeof(struct useredit_session));
	ues->window = window;
	gtk_signal_connect(GTK_OBJECT(window), "destroy",
			   GTK_SIGNAL_FUNC(useredit_destroy), ues);
	vbox = 0;
	usermod_scroll = gtk_scrolled_window_new(0, 0);
	gtk_widget_set_usize(usermod_scroll, 250, 500);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(usermod_scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);

	wvbox = gtk_vbox_new(0, 0);
	btnhbox = gtk_hbox_new(0, 0);

	wid = gtk_button_new_with_label(_("Save"));
	gtk_object_set_data(GTK_OBJECT(wid), "new", GINT_TO_POINTER(new));
	gtk_signal_connect(GTK_OBJECT(wid), "clicked",
			   GTK_SIGNAL_FUNC(useredit_save), ues);
	gtk_box_pack_start(GTK_BOX(btnhbox), wid, 0, 0, 2);

	wid = gtk_button_new_with_label(_("Delete User"));
	gtk_signal_connect(GTK_OBJECT(wid), "clicked",
			   GTK_SIGNAL_FUNC(useredit_delete), ues);
	gtk_box_pack_start(GTK_BOX(btnhbox), wid, 0, 0, 2);

	wid = gtk_button_new_with_label(_("Close"));
	gtk_signal_connect(GTK_OBJECT(wid), "clicked",
			   GTK_SIGNAL_FUNC(useredit_close), ues);
	gtk_box_pack_start(GTK_BOX(btnhbox), wid, 0, 0, 2);

	gtk_box_pack_start(GTK_BOX(wvbox), btnhbox, 0, 0, 2);

	info_frame = gtk_frame_new(_("User Info"));
	info_table = gtk_table_new(3, 2, 0);
	gtk_container_add(GTK_CONTAINER(info_frame), info_table);

	wid = gtk_entry_new();
	ues->login_entry = wid;
	if(!new) {
		gtk_editable_set_editable(GTK_EDITABLE(wid), 0);
	}
	gtk_table_attach(GTK_TABLE(info_table), wid, 1, 2, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
	wid = gtk_label_new(_("Login:"));
	gtk_misc_set_alignment(GTK_MISC(wid), 0, 0.5);
	gtk_label_set_justify(GTK_LABEL(wid), GTK_JUSTIFY_LEFT);
	gtk_table_attach(GTK_TABLE(info_table), wid, 0, 1, 0, 1, GTK_FILL, GTK_FILL, 0, 0);

	wid = gtk_entry_new();
	ues->name_entry = wid;
	gtk_table_attach(GTK_TABLE(info_table), wid, 1, 2, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);
	wid = gtk_label_new(_("Name:"));
	gtk_misc_set_alignment(GTK_MISC(wid), 0, 0.5);
	gtk_label_set_justify(GTK_LABEL(wid), GTK_JUSTIFY_LEFT);
	gtk_table_attach(GTK_TABLE(info_table), wid, 0, 1, 1, 2, GTK_FILL, GTK_FILL, 0, 0);

	wid = gtk_entry_new();
	ues->pass_entry = wid;
	gtk_table_attach(GTK_TABLE(info_table), wid, 1, 2, 2, 3, GTK_EXPAND|GTK_FILL, 0, 0, 0);
	wid = gtk_label_new(_("Pass:"));
	gtk_misc_set_alignment(GTK_MISC(wid), 0, 0.5);
	gtk_label_set_justify(GTK_LABEL(wid), GTK_JUSTIFY_LEFT);
	gtk_table_attach(GTK_TABLE(info_table), wid, 0, 1, 2, 3, GTK_FILL, GTK_FILL, 0, 0);

	gtk_table_set_row_spacings(GTK_TABLE(info_table), 10);
	gtk_table_set_col_spacings(GTK_TABLE(info_table), 5);
	gtk_box_pack_start(GTK_BOX(wvbox), info_frame, 0, 0, 2);

	avbox = gtk_vbox_new(0, 0);
	gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(usermod_scroll), avbox);
	gtk_container_add(GTK_CONTAINER(window), wvbox);
	gtk_box_pack_start(GTK_BOX(wvbox), usermod_scroll, 0, 0, 2);


	for (i = 0; i < sizeof(access_names)/sizeof(struct access_name); i++) {
		if (access_names[i].bitno == -1) {
			nframes++;
			frame = gtk_frame_new(access_names[i].name);
			vbox = gtk_vbox_new(0, 0);
			gtk_container_add(GTK_CONTAINER(frame), vbox);
			gtk_box_pack_start(GTK_BOX(avbox), frame, 0, 0, 0);
			continue;
		}
		chk = gtk_check_button_new_with_label(access_names[i].name);
		awi = i - nframes;
		ues->access_widgets[awi].bitno = access_names[i].bitno;
		ues->access_widgets[awi].widget = chk;
		gtk_signal_connect(GTK_OBJECT(chk), "clicked",
				   GTK_SIGNAL_FUNC(useredit_chk_activate), ues);
		gtk_box_pack_start(GTK_BOX(vbox), chk, 0, 0, 0);
	}

	if(!new) {
		useredit_login(login, ues);
	}
	gtk_widget_show_all(window);
}


void useredit_open_dialog()
{
	GtkWidget *window;
	GtkWidget *hbox;
	GtkWidget *vbox;
	GtkWidget *hboxtwo;
	GtkWidget *loginentry;
	GtkWidget *okbtn;
	GtkWidget *cancelbtn;
	GtkWidget *loginlbl;

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	hbox = gtk_hbox_new(0,0);
	vbox = gtk_vbox_new(0,0);
	hboxtwo = gtk_hbox_new(0,0);
	loginentry = gtk_entry_new();
	okbtn = gtk_button_new_with_label(_("Open"));
	cancelbtn = gtk_button_new_with_label(_("Cancel"));
	loginlbl = gtk_label_new(_("Login: "));
	gtk_window_set_title(GTK_WINDOW(window), _("Open User"));


	gtk_container_add(GTK_CONTAINER(window), vbox);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), loginlbl, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), loginentry, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hboxtwo, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hboxtwo), okbtn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hboxtwo), cancelbtn, 0, 0, 0);

	gtk_object_set_data(GTK_OBJECT(okbtn), "login", loginentry);
	gtk_signal_connect(GTK_OBJECT(okbtn), "clicked", GTK_SIGNAL_FUNC(useredit_open), window);

	gtk_signal_connect(GTK_OBJECT(cancelbtn), "clicked", GTK_SIGNAL_FUNC(useredit_open_close), window);

	gtk_widget_show_all(window);

	gtk_widget_grab_focus(loginentry);
}
