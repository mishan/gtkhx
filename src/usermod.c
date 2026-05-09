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
#include <adwaita.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include "hx.h"
#include "tasks.h"
#include "rcv.h"
#include "gtk_hlist.h"
#include "gtkutil.h"
#include "toolbar.h"
#include "usermod.h"

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
	task_new(htlc, RCV_TASK_FN(rcv_task_user_open), uespfn, 0, "user open");
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

	gtk_editable_set_text(GTK_EDITABLE(ues->name_entry), name);
	gtk_editable_set_text(GTK_EDITABLE(ues->login_entry), login);
	gtk_editable_set_text(GTK_EDITABLE(ues->pass_entry), pass);
	strcpy(ues->name, name);
	strcpy(ues->login, login);
	strcpy(ues->pass, pass);
	ues->access_buf = access;
	for (i = 0; i < NACCESS; i++) {
		if (!ues->access_widgets[i].widget)
			continue;
		on = test_bit(ues->access_buf, ues->access_widgets[i].bitno);
		adw_switch_row_set_active (
			ADW_SWITCH_ROW (ues->access_widgets[i].widget), on);
	}
}
static void
useredit_login (char *login, struct useredit_session *ues)
{
	size_t len;


	hx_useredit_open(&the_session.htlc, login, user_open, ues);
	len = strlen(login);
	if (len > 31)
		len = 31;
	memcpy(ues->login, login, len+1);
}

/* Phase 5: AdwAlertDialog response callback. The "open" response opens
 * the User Editor for the entered login; "cancel" / window-close
 * just dismisses. */
static void
useredit_open_response (AdwAlertDialog *dialog, const char *response,
                        gpointer data)
{
	GtkWidget *entry = data;

	if (g_strcmp0 (response, "open") == 0) {
		const char *login = gtk_editable_get_text (GTK_EDITABLE (entry));
		if (login && *login)
			create_useredit_window ((char *) login, 0);
	}
	(void) dialog;
}

static void useredit_name_pass(GtkWidget *name_entry, GtkWidget *pass_entry, struct useredit_session *ues)
{
	const char *name;
	const char *pass;
	size_t len;

	name = gtk_editable_get_text(GTK_EDITABLE(name_entry));
	len = strlen(name);
	if (len > 31)
		len = 31;
	memcpy(ues->name, name, len);
	ues->name[len] = 0;

	pass = gtk_editable_get_text(GTK_EDITABLE(pass_entry));
	len = strlen(pass);
	if (len > 31)
		len = 31;
	memcpy(ues->pass, pass, len);
	ues->pass[len] = 0;
}

static void useredit_get_login(GtkWidget *login_entry, struct useredit_session *ues)
{
	const char *login;
	size_t len;

	login = gtk_editable_get_text(GTK_EDITABLE(login_entry));
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

	/* Phase 5: notify::active passes the GObject; AdwSwitchRow's
	 * active property is the source of truth for the toggle. */
	for (i = 0; i < NACCESS; i++) {
		if (ues->access_widgets[i].widget == widget)
			break;
	}
	if (i == NACCESS)
		return;
	bitno = ues->access_widgets[i].bitno;
	if (adw_switch_row_get_active (ADW_SWITCH_ROW (widget)))
		set_bit(ues->access_buf, bitno);
	else
		unset_bit(ues->access_buf, bitno);
}

static void
useredit_save (GtkWidget *widget, gpointer data)
{
	struct useredit_session *ues = (struct useredit_session *)data;
	int new = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "new"));


	useredit_name_pass(ues->name_entry, ues->pass_entry, ues);
	if(new) {
		useredit_get_login(ues->login_entry, ues);
		new = 0;
	}
	hx_useredit_create(&the_session.htlc, ues->login, ues->pass, ues->name, ues->access_buf);
}

static void
useredit_delete (GtkWidget *widget, gpointer data)
{
	struct useredit_session *ues = (struct useredit_session *)data;

	hx_useredit_delete(&the_session.htlc, ues->login);
	gtkhx_widget_destroy(ues->window);
}


static void
useredit_destroy (GtkWidget *widget, gpointer data)
{
	/* data is a useredit_session */
	g_free(data);
}


/* Phase 5: full Adwaita rewrite of the User Editor. The legacy layout
 * was a vbox of GtkFrames containing GtkCheckButtons — functionally
 * fine but visually noisy and inconsistent with the rest of the app
 * (Settings dialog uses AdwPreferencesPage with AdwSwitchRow per
 * toggle).
 *
 * New layout:
 *   AdwHeaderBar (Save / Delete / window controls)
 *   AdwPreferencesPage in a scrolled view, containing
 *     AdwPreferencesGroup "Identity"
 *       AdwEntryRow Login        (insensitive when editing existing)
 *       AdwEntryRow Display name
 *       AdwPasswordEntryRow Password
 *     AdwPreferencesGroup "File Privileges"
 *       AdwSwitchRow per access bit
 *     AdwPreferencesGroup "Chat Privileges"
 *       AdwSwitchRow per access bit
 *     AdwPreferencesGroup "News" / "User Privileges" / "Admin Privileges"
 *       (same)
 *
 * The header-bar Save button is .suggested-action (accent-coloured),
 * Delete is .destructive-action (red); both are hidden in the New
 * User flow until something useful would happen there. */
void create_useredit_window (char *login, int new)
{
	GtkWidget         *window, *header;
	GtkWidget         *toolbar_view;
	GtkWidget         *page;
	GtkWidget         *save_btn, *delete_btn;
	GtkWidget         *info_grp, *current_grp = NULL;
	GtkWidget         *login_row, *name_row, *pass_row;
	GtkWidget         *switch_row;
	unsigned int       i, awi, nframes = 0;
	struct useredit_session *ues;
	char              *title;

	window = gtk_window_new();
	gtk_window_set_default_size (GTK_WINDOW (window), 520, 680);

	if (!new) {
		title = g_strdup_printf ("%s: %s", _("User Editor"), login);
		gtk_window_set_title (GTK_WINDOW (window), title);
		g_free (title);
	} else {
		gtk_window_set_title (GTK_WINDOW (window), _("New User"));
	}

	ues = g_malloc0 (sizeof (struct useredit_session));
	ues->window = window;
	g_signal_connect (window, "destroy",
	                  G_CALLBACK (useredit_destroy), ues);

	/* ---- Header bar ---- */
	header = adw_header_bar_new ();

	save_btn = gtk_button_new_with_label (_("Save"));
	gtk_widget_add_css_class (save_btn, "suggested-action");
	g_object_set_data (G_OBJECT (save_btn), "new", GINT_TO_POINTER (new));
	g_signal_connect (save_btn, "clicked",
	                  G_CALLBACK (useredit_save), ues);
	adw_header_bar_pack_end (ADW_HEADER_BAR (header), save_btn);

	if (!new) {
		/* No 'Delete' for a brand-new account that isn't on the
		 * server yet — there's nothing to delete. */
		delete_btn = gtk_button_new_with_label (_("Delete"));
		gtk_widget_add_css_class (delete_btn, "destructive-action");
		g_signal_connect (delete_btn, "clicked",
		                  G_CALLBACK (useredit_delete), ues);
		adw_header_bar_pack_start (ADW_HEADER_BAR (header), delete_btn);
	}

	/* ---- Content: AdwPreferencesPage in a scrolled view ----
	 * AdwPreferencesPage already does its own AdwClamp-ed layout
	 * with proper spacing; we don't need an outer scroll because the
	 * page is itself scrollable. */
	page = adw_preferences_page_new ();

	/* Identity group */
	info_grp = adw_preferences_group_new ();
	adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (info_grp),
	                                 _("Identity"));

	login_row = adw_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (login_row),
	                               _("Login"));
	if (!new) {
		/* Login is the primary key on the server side; not editable
		 * once set. */
		gtk_editable_set_editable (GTK_EDITABLE (login_row), FALSE);
	}
	adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_grp), login_row);
	ues->login_entry = login_row;

	name_row = adw_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (name_row),
	                               _("Display name"));
	adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_grp), name_row);
	ues->name_entry = name_row;

	pass_row = adw_password_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (pass_row),
	                               _("Password"));
	adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_grp), pass_row);
	ues->pass_entry = pass_row;

	adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
	                          ADW_PREFERENCES_GROUP (info_grp));

	/* ---- Access bits as AdwPreferencesGroup of AdwSwitchRow ----
	 * Sentinels in access_names[] (bitno == -1) are section headers;
	 * each one starts a new group with that title. Subsequent
	 * non-sentinel entries become AdwSwitchRow children of that
	 * group. */
	for (i = 0; i < sizeof (access_names) / sizeof (struct access_name); i++) {
		if (access_names[i].bitno == -1) {
			nframes++;
			current_grp = adw_preferences_group_new ();
			adw_preferences_group_set_title (
				ADW_PREFERENCES_GROUP (current_grp),
				access_names[i].name);
			adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
				ADW_PREFERENCES_GROUP (current_grp));
			continue;
		}
		switch_row = adw_switch_row_new ();
		adw_preferences_row_set_title (ADW_PREFERENCES_ROW (switch_row),
		                               access_names[i].name);
		awi = i - nframes;
		ues->access_widgets[awi].bitno  = access_names[i].bitno;
		ues->access_widgets[awi].widget = switch_row;
		g_signal_connect (switch_row, "notify::active",
		                  G_CALLBACK (useredit_chk_activate), ues);
		if (current_grp)
			adw_preferences_group_add (
				ADW_PREFERENCES_GROUP (current_grp), switch_row);
	}

	/* ---- Wrap in AdwToolbarView so the headerbar gets the proper
	 * top-bar styling and the page sits in the content slot. */
	toolbar_view = adw_toolbar_view_new ();
	adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);
	adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), page);
	gtk_window_set_child (GTK_WINDOW (window), toolbar_view);

	if (!new)
		useredit_login (login, ues);

	gtk_window_present (GTK_WINDOW (window));
}


/* Phase 5: AdwAlertDialog with an AdwEntryRow extra-child replaces
 * the hand-rolled GtkWindow + Open/Cancel buttons. The dialog
 * presents over the toolbar window, has Cancel + Open responses
 * (Open is the default + suggested-action style), and routes
 * window-close (Esc / X) to Cancel. */
void useredit_open_dialog (void)
{
	AdwDialog *dialog;
	GtkWidget *prefs_grp;
	GtkWidget *entry;

	dialog = adw_alert_dialog_new (_("Open User"),
	                               _("Enter the login of the account to edit."));

	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "cancel", _("_Cancel"));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "open",   _("_Open"));
	adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
	                                          "open",
	                                          ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "open");
	adw_alert_dialog_set_close_response   (ADW_ALERT_DIALOG (dialog), "cancel");

	/* AdwEntryRow inside an AdwPreferencesGroup gives the dialog the
	 * same row-style input field used elsewhere in the app
	 * (Connect dialog, Settings). */
	prefs_grp = adw_preferences_group_new ();
	entry = adw_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (entry), _("Login"));
	adw_preferences_group_add (ADW_PREFERENCES_GROUP (prefs_grp), entry);
	adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), prefs_grp);

	g_signal_connect (dialog, "response",
	                  G_CALLBACK (useredit_open_response), entry);

	adw_dialog_present (dialog,
	                    toolbar_window ? GTK_WIDGET (toolbar_window) : NULL);

	gtk_widget_grab_focus (entry);
}
