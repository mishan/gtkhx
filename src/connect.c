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
#include <fcntl.h>
#include <gtk/gtk.h>
#include <dirent.h>
#include <sys/types.h>
#include <netinet/in.h>
#include "hx.h"
#include "chat.h"
#include "gtkutil.h"

static GtkWidget *connect_window;
static GtkWidget *address_entry;
static GtkWidget *login_entry;
static GtkWidget *password_entry;
static GtkWidget *port_entry;
static GtkWidget *hope;
#ifdef CONFIG_COMPRESS
static GtkWidget *compress_menu;
#endif
#ifdef CONFIG_CIPHER
static GtkWidget *cipher_menu;
#endif

#if defined(CONFIG_CIPHER)

#define DEFAULT_CIPHER "BLOWFISH"
char *valid_ciphers[] = {"RC4", "BLOWFISH", 
#ifndef CONFIG_NO_IDEA
"IDEA", 
#endif
0};

int valid_cipher (const char *cipheralg)
{
	unsigned int i;

	for (i = 0; valid_ciphers[i]; i++) {
		if (!strcmp(valid_ciphers[i], cipheralg))
			return 1;
	}

	return 0;
}
#endif

#if defined(CONFIG_COMPRESS)

#define DEFAULT_COMPRESS "GZIP"
char *valid_compressors[] = {"GZIP", 0};

int valid_compress (const char *compressalg)
{
	unsigned int i;

	for (i = 0; valid_compressors[i]; i++) {
		if (!strcmp(valid_compressors[i], compressalg))
			return 1;
	}

	return 0;
}
#endif

guint8 *list_n (guint8 *list, guint16 listlen, unsigned int n)
{
	unsigned int i;
	guint16 pos = 1;
	guint8 *p = list + 2;

	for (i = 0; ; i++) {
		if (pos + *p > listlen)
			return 0;
		if (i == n)
			return p;
		pos += *p+1;
		p += *p+1;
	}
}

static void close_connect_window (void)
{
	gtk_widget_destroy(connect_window);
	connect_window = 0;
}

void connect_set_entries (const char *address, const char *login, const char *password, guint16 port)
{
	char buf[HOSTLEN];
	sprintf(buf, "%u", port);

	if (address)
		gtk_entry_set_text(GTK_ENTRY(address_entry), address);
	if (login)
		gtk_entry_set_text(GTK_ENTRY(login_entry), login);
	if (password)
		gtk_entry_set_text(GTK_ENTRY(password_entry), password);

	gtk_entry_set_text(GTK_ENTRY(port_entry), buf);
}

static void server_connect (GtkWidget *widget, gpointer data)
{
	char *server;
	char *login;
	char *pass;
	char *portstr;
	int secure;
	session *sess = data;
	guint16 port = 5500;

	login = gtk_entry_get_text(GTK_ENTRY(login_entry));
	server = gtk_entry_get_text(GTK_ENTRY(address_entry));
	pass = gtk_entry_get_text(GTK_ENTRY(password_entry));
	portstr = gtk_entry_get_text(GTK_ENTRY(port_entry));
	secure = gtk_toggle_button_get_active((GtkToggleButton*)hope);
	if(secure) {
#ifdef CONFIG_COMPRESS
		char compress = GPOINTER_TO_INT(gtk_object_get_data(GTK_OBJECT(compress_menu), "compress"));
		char *compress_algo = NULL;
		int colen = 0;
#endif
#ifdef CONFIG_CIPHER
		char cipher = GPOINTER_TO_INT(gtk_object_get_data(GTK_OBJECT(cipher_menu), "cipher"));
		char *cipher_algo = NULL;
		int cilen = 0;
#endif

#ifdef CONFIG_COMPRESS
		if(compress) {
			compress_algo = valid_compressors[compress-1];
			colen = strlen(compress_algo);
			if(compress_algo && valid_compress(compress_algo)) {
				strncpy(sess->htlc.compressalg, compress_algo, colen);
				sess->htlc.compressalg[colen] = 0;
			}
			else {
				memset(sess->htlc.compressalg, 0, sizeof(sess->htlc.compressalg));
			}
		}
		else {
			memset(sess->htlc.compressalg, 0, sizeof(sess->htlc.compressalg));
		}

#endif
#ifdef CONFIG_CIPHER
		if(cipher) {
			cipher_algo = valid_ciphers[cipher-1];
			cilen = strlen(cipher_algo);
			if(cipher_algo && valid_cipher(cipher_algo)) {
				strncpy(sess->htlc.cipheralg, cipher_algo, cilen);
				sess->htlc.cipheralg[cilen] = 0;
			}
			else {
				memset(sess->htlc.cipheralg, 0, sizeof(sess->htlc.cipheralg));
			}
		}
		else {
			memset(sess->htlc.cipheralg, 0, sizeof(sess->htlc.cipheralg));
		}
#endif
	}

	else {
#ifdef CONFIG_COMPRESS
		memset(sess->htlc.compressalg, 0, sizeof(sess->htlc.compressalg));
#endif
#ifdef CONFIG_CIPHER
		memset(sess->htlc.cipheralg, 0, sizeof(sess->htlc.cipheralg));
#endif
	}

	
	if(portstr[0]) {
		port = atoi(portstr);
	}
	
	hx_connect(&sess->htlc, server, port, login, pass, secure);
	
	gtk_widget_destroy(connect_window);
	connect_window = 0;
}

#ifdef CONFIG_COMPRESS
void select_compressor(GtkWidget *compress_item, gpointer data);
#endif
#ifdef CONFIG_CIPHER
void select_cipher(GtkWidget *cipher_item, gpointer data);
#endif

void set_the_entries (char *address, char *login, char *password, char *port,
					  char secure, char compress, char cipher)
{
	if (address && address[0]) {
		gtk_entry_set_text(GTK_ENTRY(address_entry), address);
	}
	else {
		gtk_entry_set_text(GTK_ENTRY(address_entry), "");
	}
	if (login && login[0]) {
		gtk_entry_set_text(GTK_ENTRY(login_entry), login);
	}
	else {
		gtk_entry_set_text(GTK_ENTRY(login_entry), "");
	}
	if (password && password[0]) {
		gtk_entry_set_text(GTK_ENTRY(password_entry), password);
	}
	else {
		gtk_entry_set_text(GTK_ENTRY(password_entry), "");
	}
	if(port && port[0]) {
		gtk_entry_set_text(GTK_ENTRY(port_entry), port);
	}
	else {
		gtk_entry_set_text(GTK_ENTRY(port_entry), "5500");
	}

	gtk_toggle_button_set_active((GtkToggleButton*)hope, secure);
#ifdef CONFIG_COMPRESS
	gtk_option_menu_set_history(GTK_OPTION_MENU(compress_menu), compress);
	select_compressor(NULL, GINT_TO_POINTER((int)compress));
#endif
#ifdef CONFIG_CIPHER
	gtk_option_menu_set_history(GTK_OPTION_MENU(cipher_menu), cipher);
	select_cipher(NULL, GINT_TO_POINTER((int)cipher));
#endif
}

static void open_bookmark(GtkWidget *widget, gpointer data);

static void strip_lf(char *buf)
{
	int i, len = strlen(buf);

	for(i = 0; i < len; i++) {
		if(buf[i] == '\n') {
			buf[i] = '\0';
			return;
		}
	}

	return;
}

static void convert_bookmark(GtkWidget *widget, gpointer data)
{
	GtkWidget *dialog;
	FILE *bm = fopen((char *)data, "r");
	char server[128];
	char login[64];
	char pass[64];
	char zeros[256];
	char *p, port[HOSTLEN];
	size_t len, len_total;
	
	if(!widget) {
		return;
	}
	dialog = gtk_object_get_data(GTK_OBJECT(widget), "dialog");
	memset(zeros, 0, 256);

	if(!bm) {
		fprintf(stderr, "Could not open '%s' for reading...Aborting\n", (char *)data);
		exit(1);
	}

	fgets(server, 128, bm);
	server[strlen(server)-1] = '\0';
	fgets(login, 64, bm);
	login[strlen(login)-1] = '\0';
	fgets(pass, 64, bm);
	strip_lf(pass);
	fclose(bm);

	port[0] = '\0';
	if(( p = strrchr(server, ':')) ) {
		int i;
		for(i = 0; i < strlen(server); i++) {
			if(&(server[i]) == p) {
				server[i] = 0;
				break;
			}
		}
		p++;
		sprintf(port, "%u", atoi(p));
	}


	set_the_entries(server, login, pass, port, 0, 0, 0);

	bm = fopen(data, "w");
	if(!bm) {
		fprintf(stderr, "Could not open '%s' for writing...Aborting\n", (char *)data);
	}


	fprintf(bm, "HTsc%c%c", 0, 1);
	fwrite(zeros, 1, 129, bm);

	len = strlen(login);
	len_total = 33-len;
	fprintf(bm, "%c", len);
	fprintf(bm, "%s", login);
	fwrite(zeros, 1, len_total, bm);

	len = strlen(pass);
	len_total = 33-len;
	fprintf(bm, "%c", len);
	fprintf(bm, "%s", pass);
	fwrite(zeros, 1, len_total, bm);

	len = strlen(server);
	len_total = 256-len;
	fprintf(bm, "%c", len);
	fprintf(bm, "%s", server);

	/* secure:0, compress:0, cipher:0 */
	fprintf(bm, "%c%c%c", 0, 0, 0);
	fwrite(zeros, 1, len_total-3, bm);

	fclose(bm);
	g_free((char *)data);
	gtk_widget_destroy(dialog);
}

static void prompt_conversion (char *name)
{
    GtkWidget *label;
    GtkWidget *dialog;
    GtkWidget *okbutton;
	GtkWidget *cancelbtn;
	char *path = g_strdup(name);

    dialog = gtk_dialog_new();

    gtk_window_set_title(GTK_WINDOW(dialog), "Convert Bookmark");
    gtk_container_border_width (GTK_CONTAINER(dialog), 5);
    label = gtk_label_new ("This bookmark is written in an old GtkHx format.\nWould you like to convert it to the new format?");
    gtk_widget_set_usize(dialog, 250, 200);
    gtk_widget_show(dialog);

    gtk_box_pack_start (GTK_BOX (GTK_DIALOG (dialog) ->vbox), label, TRUE, TRUE , 0);

    gtk_widget_show(label);

    okbutton = gtk_button_new_with_label ("Yes");
	cancelbtn = gtk_button_new_with_label("No");

    gtk_signal_connect (GTK_OBJECT (okbutton), "clicked", GTK_SIGNAL_FUNC(convert_bookmark), path);
	gtk_object_set_data(GTK_OBJECT(okbutton), "dialog", dialog);


	gtk_signal_connect_object(GTK_OBJECT(cancelbtn), "clicked", (GtkSignalFunc)gtk_widget_destroy, GTK_OBJECT(dialog));

    GTK_WIDGET_SET_FLAGS (okbutton, GTK_CAN_DEFAULT);

    gtk_box_pack_start (GTK_BOX (GTK_DIALOG(dialog) ->action_area), okbutton, 0, 0, 0);
    gtk_box_pack_start (GTK_BOX (GTK_DIALOG(dialog) ->action_area), cancelbtn, 0, 0, 0);

    gtk_widget_grab_default (okbutton);

    gtk_widget_show_all (dialog);

}

static void open_bookmark(GtkWidget *widget, gpointer data)
{
	char *path = g_strdup_printf("%s/.hx/bookmarks/%s", getenv("HOME"), (char *)data);
	int bm = open(path, O_RDONLY);
	char junk[132];
	char login[33];
	char pass[33];
	char server[128];
	char secure;
	char compress;
	char cipher;
	char header[5];
	char len_addr;
	char *p, port[HOSTLEN];
	size_t len;

	if(bm < 0) {
		g_warning("%s \"%s\"\n", _("No such bookmark"), (char *)data);
		return;
	}

	read(bm, header, 4);
	header[4] = '\0';

	if(strcmp(header, "HTsc")) {
		close(bm);
		prompt_conversion(path);
		g_free(path);
		return;
	}
	g_free(path);

	read(bm, junk, 132);
	read(bm, login, 33);
	read(bm, &len_addr, 1);
	read(bm, pass, 33);
	read(bm, &len_addr, 1);

	len =  len_addr;

	read(bm, server, len);
	server[len] = 0;

	read(bm, &secure, 1);
	read(bm, &compress, 1);
	read(bm, &cipher, 1);


	port[0] = '\0';
	if(( p = strrchr(server, ':')) ) {
		int i;
		for(i = 0; i < strlen(server); i++) {
			if(&(server[i]) == p) {
				server[i] = 0;
				break;
			}
		}
		p++;
		sprintf(port, "%u", atoi(p));

	}

	set_the_entries(server, login, pass, port, secure, compress, cipher);
	close(bm);
}

static void destroy_bookmark_item (GtkWidget *item, char *file)
{

	if(file) {
		g_free(file);
	}
}

static void list_bookmarks(GtkWidget *menu)
{
	struct dirent *ent;
	char *file;
	char *path = g_strdup_printf("%s/.hx/bookmarks/", getenv("HOME"));
	DIR *dir = opendir(path);
	GtkWidget *item;


	if(dir) {
		while((ent = readdir(dir)))	{
			if(*ent->d_name != '.')		{
				file = g_strdup(ent->d_name);
				item = gtk_menu_item_new_with_label(file);
				gtk_menu_append(GTK_MENU(menu), item);
				gtk_signal_connect(GTK_OBJECT(item), "activate", GTK_SIGNAL_FUNC(open_bookmark), file);
				gtk_signal_connect(GTK_OBJECT(item), "destroy", GTK_SIGNAL_FUNC(destroy_bookmark_item), file);
			}
		}
		closedir(dir);


	}
	item = gtk_menu_item_new();
	gtk_menu_append(GTK_MENU(menu), item);

	g_free(path);
}

static void cancel_save(GtkWidget *widget, gpointer data)
{
	GtkWidget *dialog = (GtkWidget *)gtk_object_get_data(GTK_OBJECT(widget), "dialog");
	gtk_widget_destroy(dialog);
}

/* cut down this tree with a herring */
static void bookmark_save(GtkWidget *widget, gpointer data)
{
	GtkWidget *name_entry = (GtkWidget *)gtk_object_get_data(GTK_OBJECT(widget), "name");
	GtkWidget *dialog = (GtkWidget *)gtk_object_get_data(GTK_OBJECT(widget), "dialog");
	char *server = gtk_entry_get_text(GTK_ENTRY(address_entry));
	char *login = gtk_entry_get_text(GTK_ENTRY(login_entry));
	char *pass = gtk_entry_get_text(GTK_ENTRY(password_entry));
	char *port = gtk_entry_get_text(GTK_ENTRY(port_entry));
	char secure = gtk_toggle_button_get_active((GtkToggleButton*)hope);
#ifdef CONFIG_COMPRESS
	char compress = GPOINTER_TO_INT(gtk_object_get_data(GTK_OBJECT(compress_menu), "compress"));
#else
	char compress = 0;
#endif
#ifdef CONFIG_CIPHER
	char cipher = GPOINTER_TO_INT(gtk_object_get_data(GTK_OBJECT(cipher_menu), "cipher"));
#else
	char cipher = 0;
#endif
	char *home = getenv("HOME");
	char *name = gtk_entry_get_text(GTK_ENTRY(name_entry));
	char *dir = g_strdup_printf("%s/.hx/bookmarks/", home);
	char *path = NULL;
	char *dirtwo = g_strdup_printf("%s/.hx/", home);
	char *server_str = g_strdup_printf("%s:%s", server, port);
	FILE *bookmark = NULL;
	DIR *hxdir = opendir(dirtwo);
	size_t len, len_total;
	char zeros[256];
	int i;

	memset (zeros, 0, 256);

	if(!name) {
		error_dialog( _("Error"),
					  _("You must specify a name for this bookmark " 
						"with at least one character.")
			);
		g_free(dir);
		g_free(dirtwo);
		g_free(server_str);
		return;
	}

	len = strlen(name);
	for(i = 0; i < len; i++) {
		if(name[i] == '/')
			name[i] = '\\';
	}
	path = g_strdup_printf("%s%s", dir, name);

	if(!hxdir) {
		mkdir(dirtwo, 0770);
		mkdir(dir, 0770);
	}
	else {
		closedir(hxdir);
		if(!(hxdir = opendir(dir))) {
			mkdir(dir, 0770);
		}
		else {
			closedir(hxdir);
		}
	}
	if(!(bookmark = fopen(path, "w"))) {
		hx_printf_prefix(&sessions[0].htlc, 0, "Could not open \"%s\" for writing.", path);
		g_free(dir);
		g_free(dirtwo);
		g_free(server_str);
		g_free(path);
		return;
	}

	fprintf(bookmark, "HTsc%c%c", 0, 1);
	fwrite(zeros, 1, 129, bookmark);

	len = strlen(login);
	len_total = 33-len;
	fprintf(bookmark, "%c%s", len, login);
	fwrite(zeros, 1, len_total, bookmark);

	len = strlen(pass);
	len_total = 33-len;
	fprintf(bookmark, "%c%s", len, pass);
	fwrite(zeros, 1, len_total, bookmark);

	len = strlen(server_str);
	len_total = 256-len;
	fprintf(bookmark, "%c%s", len, server_str);

	
	fprintf(bookmark, "%c%c%c", secure, compress, cipher);
	len_total -= 3;
	fwrite(zeros, 1, len_total, bookmark);

	fclose(bookmark);

	gtk_widget_destroy(dialog);

	g_free(path);
	g_free(dir);
	g_free(dirtwo);
	g_free(server_str);
}


static void save_dialog(GtkWidget *widget, gpointer data)
{
	GtkWidget *dialog;
	GtkWidget *ok;
	GtkWidget *cancel;
	GtkWidget *name_entry;
	GtkWidget *label;
	GtkWidget *hbox;


	dialog = gtk_dialog_new();
	ok = gtk_button_new_with_label(_("OK"));
	GTK_WIDGET_SET_FLAGS(ok, GTK_CAN_DEFAULT);
	cancel = gtk_button_new_with_label(_("Cancel"));
	name_entry = gtk_entry_new();
	hbox = gtk_hbox_new(0,0);
	label = gtk_label_new(_("Name:"));
	gtk_window_set_title(GTK_WINDOW(dialog), _("Save Bookmark..."));
	gtk_widget_set_usize(dialog, 200, 100);
    gtk_container_border_width (GTK_CONTAINER(dialog), 5);
	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), hbox, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), label, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), name_entry, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->action_area), ok, 0,0, 0);
	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->action_area), cancel, 0,0, 0);
	gtk_object_set_data(GTK_OBJECT(cancel), "dialog", dialog);
	gtk_signal_connect(GTK_OBJECT(cancel), "clicked", GTK_SIGNAL_FUNC(cancel_save), 0);
	gtk_object_set_data(GTK_OBJECT(ok), "name", name_entry);
	gtk_object_set_data(GTK_OBJECT(ok), "dialog", dialog);
	gtk_signal_connect(GTK_OBJECT(ok), "clicked", GTK_SIGNAL_FUNC(bookmark_save), 0);


	gtk_widget_grab_default(ok);
	gtk_widget_show_all(dialog);
	init_keyaccel(dialog);

	gtk_widget_grab_focus(name_entry);
}

static void builtin_bookmark(GtkWidget *widget, gpointer data)
{
	if(GPOINTER_TO_INT(data) == 1) {
		set_the_entries("hlserver.com", "", "", "5500", 0, 0, 0);
	}
	else if(GPOINTER_TO_INT(data) == 2) {
		set_the_entries("cafelinux.dhs.org", "", "", "5500", 1, 1, 2);
	}
	else if(GPOINTER_TO_INT(data) == 3) {
		set_the_entries("gtkhx.nasledov.com", "", "", "5500", 1, 1, 2);
	}
	else if(GPOINTER_TO_INT(data) == 4) {
		set_the_entries("hl.singrafix.com", "guest", "@sin", "5500", 1, 1, 2);
	}
}

#ifdef CONFIG_COMPRESS
void select_compressor(GtkWidget *compress_item, gpointer data)
{
	int i = GPOINTER_TO_INT(data);

//	if(i >= 0 && valid_compressors[i-1]) {
		gtk_object_remove_data(GTK_OBJECT(compress_menu), "compress");
		gtk_object_set_data(GTK_OBJECT(compress_menu), "compress", GINT_TO_POINTER(i));
//	}
}
#endif

#ifdef CONFIG_CIPHER
void select_cipher(GtkWidget *compress_item, gpointer data)
{
	int i = GPOINTER_TO_INT(data);

//	if(i >= 0 && valid_ciphers[i-1]) {
	gtk_object_remove_data(GTK_OBJECT(cipher_menu), "cipher");
	gtk_object_set_data(GTK_OBJECT(cipher_menu), "cipher", GINT_TO_POINTER(i));
//	}
}
#endif

void create_connect_window (GtkWidget *btn, gpointer data)
{
	GtkWidget *vbox1;
	GtkWidget *help_label;
	GtkWidget *frame1;
	GtkWidget *table1;
	GtkWidget *server_label;
	GtkWidget *login_label;
	GtkWidget *pass_label;
#ifdef CONFIG_COMPRESS
	GtkWidget *compress_label;
	GtkWidget *compress_item;
	GtkWidget *compressmenu;
#endif
#ifdef CONFIG_CIPHER
	GtkWidget *cipher_label;
	GtkWidget *cipher_item;
	GtkWidget *ciphermenu;
#endif
	GtkWidget *button_connect, *button_cancel;
	GtkWidget *bookmarkmenu;
	GtkWidget *bookmarkmenu_menu;
	GtkWidget *hbuttonbox1;
	GtkWidget *save_button;
 	GtkWidget *built_in1;
  	GtkWidget *built_in2;
	GtkWidget *built_in3;
	GtkWidget *built_in4;
	GtkWidget *port_label;
	session *sess = data;

	if (connect_window) {
		gdk_window_raise(connect_window->window);
		return;
	}

	connect_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_wmclass(GTK_WINDOW(connect_window), "connect", "GtkHx");
	gtk_window_set_title(GTK_WINDOW(connect_window), "Connect");
	gtk_window_set_position(GTK_WINDOW(connect_window), GTK_WIN_POS_CENTER);
	gtk_signal_connect(GTK_OBJECT(connect_window), "destroy",
			   GTK_SIGNAL_FUNC(close_connect_window), 0);
	vbox1 = gtk_vbox_new(0, 10);
	gtk_container_add(GTK_CONTAINER(connect_window), vbox1);
	gtk_container_set_border_width(GTK_CONTAINER(vbox1), 10);


	help_label = gtk_label_new(_("Enter the server address, and if you have an account, your login and password. If not, leave the login and password blank."));
	gtk_box_pack_start(GTK_BOX(vbox1), help_label, 0, 1, 0);
	gtk_label_set_justify(GTK_LABEL(help_label), GTK_JUSTIFY_LEFT);
	gtk_label_set_line_wrap(GTK_LABEL(help_label), 1);
	gtk_misc_set_alignment(GTK_MISC(help_label), 0, 0.5);

	frame1 = gtk_frame_new(0);
	gtk_box_pack_start(GTK_BOX(vbox1), frame1, 1, 1, 0);
	gtk_frame_set_shadow_type(GTK_FRAME(frame1), GTK_SHADOW_IN);

 	table1 = gtk_table_new(3, 6, 0);
	gtk_container_add(GTK_CONTAINER(frame1), table1);
	gtk_container_set_border_width(GTK_CONTAINER(table1), 10);
	gtk_table_set_row_spacings(GTK_TABLE(table1), 5);
	gtk_table_set_col_spacings(GTK_TABLE(table1), 5);

	server_label = gtk_label_new(_("Server:"));
	gtk_table_attach(GTK_TABLE(table1), server_label, 0, 1, 0, 1,
			 (GtkAttachOptions)(GTK_FILL),
			 (GtkAttachOptions)0, 0, 0);
	gtk_label_set_justify(GTK_LABEL(server_label), GTK_JUSTIFY_LEFT);
	gtk_misc_set_alignment(GTK_MISC(server_label), 0, 0.5);

	login_label = gtk_label_new(_("Login:"));
	gtk_table_attach(GTK_TABLE(table1), login_label, 0, 1, 1, 2,
			 (GtkAttachOptions)(GTK_FILL),
			 (GtkAttachOptions)0, 0, 0);
	gtk_label_set_justify(GTK_LABEL(login_label), GTK_JUSTIFY_LEFT);
	gtk_misc_set_alignment(GTK_MISC(login_label), 0, 0.5);

	pass_label = gtk_label_new(_("Password:"));
	gtk_table_attach(GTK_TABLE(table1), pass_label, 0, 1, 2, 3,
			 (GtkAttachOptions)(GTK_FILL),
			 (GtkAttachOptions)0, 0, 0);
	gtk_label_set_justify(GTK_LABEL(pass_label), GTK_JUSTIFY_LEFT);
	gtk_misc_set_alignment(GTK_MISC(pass_label), 0, 0.5);

	hope = gtk_check_button_new_with_label(_("Secure (HOPE)"));
	gtk_toggle_button_set_active((GtkToggleButton*)hope, 0);
	gtk_table_attach(GTK_TABLE(table1), hope, 0, 1, 3, 4,
			 (GtkAttachOptions)(GTK_EXPAND|GTK_FILL),
			 (GtkAttachOptions)0, 0, 0);
	
#ifdef CONFIG_COMPRESS
	compress_label = gtk_label_new(_("Compress: "));
	gtk_table_attach(GTK_TABLE(table1), compress_label, 0, 1, 4, 5,
			 (GtkAttachOptions)(GTK_FILL),
			 (GtkAttachOptions)0, 0, 0);
	gtk_label_set_justify(GTK_LABEL(compress_label), GTK_JUSTIFY_LEFT);
	gtk_misc_set_alignment(GTK_MISC(compress_label), 0, 0.5);

	compress_menu = gtk_option_menu_new();
	gtk_table_attach(GTK_TABLE(table1), compress_menu, 1, 2, 4, 5, GTK_EXPAND|GTK_FILL, 0, 0, 0);

	compressmenu = gtk_menu_new();
	compress_item = gtk_menu_item_new_with_label("NONE");
	gtk_menu_append(GTK_MENU(compressmenu), compress_item);
	gtk_signal_connect(GTK_OBJECT(compress_item), "activate", GTK_SIGNAL_FUNC(select_compressor), GINT_TO_POINTER(0));
	{
		int i;
		
		for(i = 0; valid_compressors[i]; i++) {
			compress_item = gtk_menu_item_new_with_label(valid_compressors[i]);
			gtk_menu_append(GTK_MENU(compressmenu), compress_item);
			gtk_signal_connect(GTK_OBJECT(compress_item), "activate", GTK_SIGNAL_FUNC(select_compressor), GINT_TO_POINTER(i+1));			
		}
	}
	gtk_option_menu_set_menu(GTK_OPTION_MENU(compress_menu), compressmenu);
#endif

#ifdef CONFIG_CIPHER
	cipher_label = gtk_label_new(_("Cipher: "));
	gtk_table_attach(GTK_TABLE(table1), cipher_label, 0, 1, 5, 6,
			 (GtkAttachOptions)(GTK_FILL),
			 (GtkAttachOptions)0, 0, 0);
	gtk_label_set_justify(GTK_LABEL(cipher_label), GTK_JUSTIFY_LEFT);
	gtk_misc_set_alignment(GTK_MISC(cipher_label), 0, 0.5);

	cipher_menu = gtk_option_menu_new();
	gtk_table_attach(GTK_TABLE(table1), cipher_menu, 1, 2, 5, 6, GTK_EXPAND|GTK_FILL, 0, 0, 0);

	ciphermenu = gtk_menu_new();
	cipher_item = gtk_menu_item_new_with_label("NONE");
	gtk_menu_append(GTK_MENU(ciphermenu), cipher_item);
	gtk_signal_connect(GTK_OBJECT(cipher_item), "activate", GTK_SIGNAL_FUNC(select_cipher), GINT_TO_POINTER(0));
	{
		int i;
		
		for(i = 0; valid_ciphers[i]; i++) {
			cipher_item = gtk_menu_item_new_with_label(valid_ciphers[i]);
			gtk_menu_append(GTK_MENU(ciphermenu), cipher_item);
;			gtk_signal_connect(GTK_OBJECT(cipher_item), "activate", GTK_SIGNAL_FUNC(select_cipher), GINT_TO_POINTER(i+1));			
		}
	}
	gtk_option_menu_set_menu(GTK_OPTION_MENU(cipher_menu), ciphermenu);
#endif

	address_entry = gtk_entry_new();
	gtk_table_attach(GTK_TABLE(table1), address_entry, 1, 2, 0, 1,
			 (GtkAttachOptions)(GTK_EXPAND | GTK_FILL),
			 (GtkAttachOptions)0, 0, 0);

	port_label = gtk_label_new(_("Port:"));
	gtk_table_attach(GTK_TABLE(table1), port_label, 2, 3, 0, 1, GTK_FILL, 0, 0, 0);

	gtk_label_set_justify(GTK_LABEL(port_label), GTK_JUSTIFY_LEFT);
	gtk_misc_set_alignment(GTK_MISC(port_label), 0, 0.5);

	port_entry = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(port_entry), 6);
	gtk_entry_set_text(GTK_ENTRY(port_entry), "5500");
	gtk_widget_set_usize(port_entry, 45, 0);
	gtk_table_attach(GTK_TABLE(table1), port_entry, 3, 4, 0, 1,
			  0,
			 (GtkAttachOptions)0, 0, 0);


	login_entry = gtk_entry_new();
	gtk_table_attach(GTK_TABLE(table1), login_entry, 1, 2, 1, 2,
			 (GtkAttachOptions)(GTK_EXPAND | GTK_FILL),
			 (GtkAttachOptions)0, 0, 0);
	password_entry = gtk_entry_new();
	gtk_entry_set_visibility(GTK_ENTRY(password_entry), 0);
	gtk_table_attach(GTK_TABLE(table1), password_entry, 1, 2, 2, 3,
			 (GtkAttachOptions)(GTK_EXPAND | GTK_FILL),
			 (GtkAttachOptions)0, 0, 0);

	bookmarkmenu = gtk_option_menu_new();
	gtk_table_attach(GTK_TABLE(table1), bookmarkmenu, 4, 5, 0, 1,
			 (GtkAttachOptions)0,
			 (GtkAttachOptions)0, 0, 0);

	bookmarkmenu_menu = gtk_menu_new();
	list_bookmarks(bookmarkmenu_menu);
 	built_in1 = gtk_menu_item_new_with_label("Hotline Communications");
	built_in2 = gtk_menu_item_new_with_label("CafeLinux");
	built_in3 = gtk_menu_item_new_with_label("GtkHx");
	built_in4 = gtk_menu_item_new_with_label("SiN Grafix");
	gtk_menu_append(GTK_MENU(bookmarkmenu_menu), built_in1);
	gtk_menu_append(GTK_MENU(bookmarkmenu_menu), built_in2);
	gtk_menu_append(GTK_MENU(bookmarkmenu_menu), built_in3);
	gtk_menu_append(GTK_MENU(bookmarkmenu_menu), built_in4);

	gtk_signal_connect(GTK_OBJECT(built_in1), "activate", GTK_SIGNAL_FUNC(builtin_bookmark), GINT_TO_POINTER(1));
	gtk_signal_connect(GTK_OBJECT(built_in2), "activate", GTK_SIGNAL_FUNC(builtin_bookmark), GINT_TO_POINTER(2));
	gtk_signal_connect(GTK_OBJECT(built_in3), "activate", GTK_SIGNAL_FUNC(builtin_bookmark), GINT_TO_POINTER(3));
	gtk_signal_connect(GTK_OBJECT(built_in4), "activate", GTK_SIGNAL_FUNC(builtin_bookmark), GINT_TO_POINTER(4));

	gtk_option_menu_set_menu(GTK_OPTION_MENU(bookmarkmenu), bookmarkmenu_menu);

	hbuttonbox1 = gtk_hbutton_box_new();
	gtk_box_pack_start(GTK_BOX(vbox1), hbuttonbox1, 1, 1, 0);

	save_button = gtk_button_new_with_label(_("Save..."));
	gtk_container_add(GTK_CONTAINER(hbuttonbox1), save_button);
	gtk_signal_connect(GTK_OBJECT(save_button), "clicked", GTK_SIGNAL_FUNC(save_dialog), 0);

	button_cancel = gtk_button_new_with_label(_("Cancel"));
	gtk_signal_connect(GTK_OBJECT(button_cancel), "clicked", GTK_SIGNAL_FUNC(close_connect_window), 0);
	gtk_container_add(GTK_CONTAINER(hbuttonbox1), button_cancel);

	button_connect = gtk_button_new_with_label(_("Connect"));
	gtk_signal_connect(GTK_OBJECT(button_connect), "clicked", GTK_SIGNAL_FUNC(server_connect), sess);
	gtk_container_add (GTK_CONTAINER (hbuttonbox1), button_connect);

	gtk_widget_show_all(connect_window);
	init_keyaccel(connect_window);
	gtk_widget_grab_focus(address_entry);
}

void connect_bookmark_name(char *name)
{
	create_connect_window(0,&sessions[0]);
	open_bookmark(0, name);
}
