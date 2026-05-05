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
#include <sys/stat.h>
#include <netinet/in.h>
#include "hx.h"
#include "chat.h"
#include "gtkutil.h"
#include "gtkhx.h"
#include "toolbar.h"

/* Phase 4.13: this file uses GtkComboBoxText for the cipher /
 * compress / bookmark dropdowns, plus GtkDialog for the bookmark
 * convert + save prompts. GtkComboBoxText is deprecated in GTK 4.10
 * in favor of GtkDropDown + GtkStringList; GtkDialog in favor of
 * GtkWindow / GtkAlertDialog. Both migrations are tracked as
 * follow-ups (Phase 5 UX for the dropdowns, Phase 4.7 for dialogs);
 * until then suppress deprecations across the file. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

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
	gtkhx_widget_destroy(connect_window);
	connect_window = 0;
}

void connect_set_entries (const char *address, const char *login, const char *password, guint16 port)
{
	char buf[HOSTLEN];
	g_snprintf(buf, sizeof(buf), "%u", port);

	if (address)
		gtk_editable_set_text(GTK_EDITABLE(address_entry), address);
	if (login)
		gtk_editable_set_text(GTK_EDITABLE(login_entry), login);
	if (password)
		gtk_editable_set_text(GTK_EDITABLE(password_entry), password);

	gtk_editable_set_text(GTK_EDITABLE(port_entry), buf);
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

	login = gtk_editable_get_text(GTK_EDITABLE(login_entry));
	server = gtk_editable_get_text(GTK_EDITABLE(address_entry));
	pass = gtk_editable_get_text(GTK_EDITABLE(password_entry));
	portstr = gtk_editable_get_text(GTK_EDITABLE(port_entry));
	secure = gtk_check_button_get_active((GtkCheckButton*)hope);
	if(secure) {
#ifdef CONFIG_COMPRESS
		char compress = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(compress_menu), "compress"));
		char *compress_algo = NULL;
		int colen = 0;
#endif
#ifdef CONFIG_CIPHER
		char cipher = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cipher_menu), "cipher"));
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
	
	gtkhx_widget_destroy(connect_window);
	connect_window = 0;
}

#ifdef CONFIG_COMPRESS
static void compress_combo_changed (GtkComboBox *combo, gpointer data);
#endif
#ifdef CONFIG_CIPHER
static void cipher_combo_changed (GtkComboBox *combo, gpointer data);
#endif
static void builtin_bookmark (GtkWidget *widget, gpointer data);

void set_the_entries (char *address, char *login, char *password, char *port,
					  char secure, char compress, char cipher)
{
	if (address && address[0]) {
		gtk_editable_set_text(GTK_EDITABLE(address_entry), address);
	}
	else {
		gtk_editable_set_text(GTK_EDITABLE(address_entry), "");
	}
	if (login && login[0]) {
		gtk_editable_set_text(GTK_EDITABLE(login_entry), login);
	}
	else {
		gtk_editable_set_text(GTK_EDITABLE(login_entry), "");
	}
	if (password && password[0]) {
		gtk_editable_set_text(GTK_EDITABLE(password_entry), password);
	}
	else {
		gtk_editable_set_text(GTK_EDITABLE(password_entry), "");
	}
	if(port && port[0]) {
		gtk_editable_set_text(GTK_EDITABLE(port_entry), port);
	}
	else {
		gtk_editable_set_text(GTK_EDITABLE(port_entry), "5500");
	}

	gtk_check_button_set_active((GtkCheckButton*)hope, secure);
#ifdef CONFIG_COMPRESS
	gtk_combo_box_set_active(GTK_COMBO_BOX(compress_menu), compress);
#endif
#ifdef CONFIG_CIPHER
	gtk_combo_box_set_active(GTK_COMBO_BOX(cipher_menu), cipher);
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
	dialog = g_object_get_data(G_OBJECT(widget), "dialog");
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
		g_snprintf(port, sizeof(port), "%u", atoi(p));
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
	gtkhx_widget_destroy(dialog);
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
    /* Phase 4.5: anchor to the toolbar window — there's no other obvious
     * parent at this entry-point (called from the prefs load path). */
    if (toolbar_window)
        gtk_window_set_transient_for(GTK_WINDOW(dialog),
                                     GTK_WINDOW(toolbar_window));
    (gtk_widget_set_margin_start(dialog, 5), gtk_widget_set_margin_end(dialog, 5), gtk_widget_set_margin_top(dialog, 5), gtk_widget_set_margin_bottom(dialog, 5));
    label = gtk_label_new ("This bookmark is written in an old GtkHx format.\nWould you like to convert it to the new format?");
    gtk_widget_set_size_request(dialog, 250, 200);
    gtk_window_present(GTK_WINDOW(dialog));

    gtkhx_box_pack(gtk_dialog_get_content_area(GTK_DIALOG (dialog)), label, TRUE, TRUE, 0);


    okbutton = gtk_button_new_with_label ("Yes");
	cancelbtn = gtk_button_new_with_label("No");

    g_signal_connect (okbutton, "clicked", G_CALLBACK(convert_bookmark), path);
	g_object_set_data(G_OBJECT(okbutton), "dialog", dialog);


	g_signal_connect_swapped(cancelbtn, "clicked", (GCallback)gtkhx_widget_destroy, dialog);

    /* Phase 4.2: gtk_widget_set_can_default removed */

    gtkhx_box_pack(gtkhx_dialog_action_area(GTK_DIALOG(dialog)), okbutton, 0, 0, 0);
    gtkhx_box_pack(gtkhx_dialog_action_area(GTK_DIALOG(dialog)), cancelbtn, 0, 0, 0);

    /* Phase 4.2: gtk_widget_grab_default removed (use gtk_window_set_default_widget if needed) */

    gtk_window_present(GTK_WINDOW(dialog));

}

/* Phase 5: bookmarks live under $CONFIG/bookmarks/. Legacy
 * ~/.hx/bookmarks/ is consulted as a read-fallback only — bookmarks
 * saved from this version always go to the new path. */
static char *
bookmarks_dir_primary (void)
{
	return g_build_filename (gtkhx_config_dir (), "bookmarks", NULL);
}

static char *
bookmarks_dir_legacy (void)
{
	const char *home = g_getenv ("HOME");
	if (!home || !*home)
		home = g_get_home_dir ();
	if (!home)
		return NULL;
	return g_build_filename (home, ".hx", "bookmarks", NULL);
}

/* Resolve a bookmark name to an open fd and the path it came from.
 * Tries $CONFIG/bookmarks/<name> first; if absent, falls back to the
 * legacy ~/.hx/bookmarks/<name>. Caller g_frees *out_path on the
 * non-error return. Returns -1 with *out_path set to NULL on failure. */
static int
open_bookmark_file (const char *name, char **out_path)
{
	char *primary = bookmarks_dir_primary ();
	char *path    = g_build_filename (primary, name, NULL);
	int   bm      = open (path, O_RDONLY);

	g_free (primary);

	if (bm < 0 && errno == ENOENT) {
		char *legacy = bookmarks_dir_legacy ();
		if (legacy) {
			char *legacy_path = g_build_filename (legacy, name, NULL);
			bm = open (legacy_path, O_RDONLY);
			if (bm >= 0) {
				g_free (path);
				path = legacy_path;
			} else {
				g_free (legacy_path);
			}
			g_free (legacy);
		}
	}

	if (bm < 0) {
		g_free (path);
		*out_path = NULL;
	} else {
		*out_path = path;
	}
	return bm;
}

static void open_bookmark(GtkWidget *widget, gpointer data)
{
	char *path = NULL;
	int   bm   = open_bookmark_file ((char *) data, &path);
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
		g_snprintf(port, sizeof(port), "%u", atoi(p));

	}

	set_the_entries(server, login, pass, port, secure, compress, cipher);
	close(bm);
}

/*
 * Bookmark dropdown bookkeeping. Each combo entry maps to either a
 * filesystem bookmark (file != NULL) or a built-in (builtin_idx
 * 1..4). The GArray is stashed on the combo via g_object_set_data_full
 * so it (and the strdup'd file strings) get freed when the combo is
 * destroyed.
 */
typedef struct {
	int   builtin_idx;
	char *file;
} BookmarkEntry;

static void
bookmark_entries_free (gpointer data)
{
	GArray *a = data;
	guint i;
	if (!a)
		return;
	for (i = 0; i < a->len; i++) {
		BookmarkEntry *e = &g_array_index (a, BookmarkEntry, i);
		g_free (e->file);
	}
	g_array_free (a, TRUE);
}

static void
bookmark_combo_changed (GtkComboBox *combo, gpointer user_data)
{
	GArray *entries = g_object_get_data (G_OBJECT (combo), "entries");
	int idx = gtk_combo_box_get_active (combo);
	BookmarkEntry *e;

	(void) user_data;
	if (!entries || idx < 0 || (guint) idx >= entries->len)
		return;
	e = &g_array_index (entries, BookmarkEntry, idx);
	if (e->builtin_idx)
		builtin_bookmark (NULL, GINT_TO_POINTER (e->builtin_idx));
	else if (e->file)
		open_bookmark (NULL, e->file);
}

/* Phase 5: list bookmarks from both the new $CONFIG/bookmarks and the
 * legacy ~/.hx/bookmarks. Names that exist in both win on the new
 * path (we walk new first; legacy entries only get appended if their
 * name isn't already present). */
static gboolean
combo_already_has (GArray *entries, const char *name)
{
	guint i;
	for (i = 0; i < entries->len; i++) {
		BookmarkEntry *e = &g_array_index (entries, BookmarkEntry, i);
		if (e->file && g_strcmp0 (e->file, name) == 0)
			return TRUE;
	}
	return FALSE;
}

static void
list_bookmarks_from_dir (GtkWidget *combo, GArray *entries, const char *path)
{
	struct dirent *ent;
	DIR *dir = opendir (path);

	if (!dir)
		return;

	while ((ent = readdir (dir))) {
		if (*ent->d_name == '.')
			continue;
		if (combo_already_has (entries, ent->d_name))
			continue;
		BookmarkEntry e = { 0, g_strdup (ent->d_name) };
		gtk_combo_box_text_append_text (
			GTK_COMBO_BOX_TEXT (combo), ent->d_name);
		g_array_append_val (entries, e);
	}
	closedir (dir);
}

static void list_bookmarks (GtkWidget *combo, GArray *entries)
{
	char *primary = bookmarks_dir_primary ();
	char *legacy  = bookmarks_dir_legacy ();

	list_bookmarks_from_dir (combo, entries, primary);
	if (legacy)
		list_bookmarks_from_dir (combo, entries, legacy);

	g_free (primary);
	g_free (legacy);
}

static void cancel_save(GtkWidget *widget, gpointer data)
{
	GtkWidget *dialog = (GtkWidget *)g_object_get_data(G_OBJECT(widget), "dialog");
	gtkhx_widget_destroy(dialog);
}

/* cut down this tree with a herring */
static void bookmark_save(GtkWidget *widget, gpointer data)
{
	GtkWidget *name_entry = (GtkWidget *)g_object_get_data(G_OBJECT(widget), "name");
	GtkWidget *dialog = (GtkWidget *)g_object_get_data(G_OBJECT(widget), "dialog");
	char *server = gtk_editable_get_text(GTK_EDITABLE(address_entry));
	char *login = gtk_editable_get_text(GTK_EDITABLE(login_entry));
	char *pass = gtk_editable_get_text(GTK_EDITABLE(password_entry));
	char *port = gtk_editable_get_text(GTK_EDITABLE(port_entry));
	char secure = gtk_check_button_get_active((GtkCheckButton*)hope);
#ifdef CONFIG_COMPRESS
	char compress = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(compress_menu), "compress"));
#else
	char compress = 0;
#endif
#ifdef CONFIG_CIPHER
	char cipher = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cipher_menu), "cipher"));
#else
	char cipher = 0;
#endif
	char *name = gtk_editable_get_text(GTK_EDITABLE(name_entry));
	/* Phase 5: bookmarks now save under $CONFIG/bookmarks/. The
	 * config dir is created by gtkhx_config_dir on first call;
	 * we just need to make sure the bookmarks subdir exists. */
	char *dir = bookmarks_dir_primary ();
	char *path = NULL;
	char *server_str = g_strdup_printf("%s:%s", server, port);
	FILE *bookmark = NULL;
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
		g_free(server_str);
		return;
	}

	len = strlen(name);
	for(i = 0; i < len; i++) {
		if(name[i] == '/')
			name[i] = '\\';
	}
	path = g_build_filename (dir, name, NULL);

	if (g_mkdir_with_parents (dir, 0770) != 0) {
		hx_printf_prefix(&the_session.htlc, 0,
		                 "Could not create bookmarks dir \"%s\": %s",
		                 dir, g_strerror (errno));
		g_free(dir);
		g_free(server_str);
		g_free(path);
		return;
	}
	if(!(bookmark = fopen(path, "w"))) {
		hx_printf_prefix(&the_session.htlc, 0, "Could not open \"%s\" for writing.", path);
		g_free(dir);
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

	gtkhx_widget_destroy(dialog);

	g_free(path);
	g_free(dir);
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
	/* Phase 4.2: gtk_widget_set_can_default removed */
	cancel = gtk_button_new_with_label(_("Cancel"));
	name_entry = gtk_entry_new();
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	label = gtk_label_new(_("Name:"));
	gtk_window_set_title(GTK_WINDOW(dialog), _("Save Bookmark..."));
	if (toolbar_window)
		gtk_window_set_transient_for(GTK_WINDOW(dialog),
		                             GTK_WINDOW(toolbar_window));
	gtk_widget_set_size_request(dialog, 200, 100);
    (gtk_widget_set_margin_start(dialog, 5), gtk_widget_set_margin_end(dialog, 5), gtk_widget_set_margin_top(dialog, 5), gtk_widget_set_margin_bottom(dialog, 5));
	gtkhx_box_pack(gtk_dialog_get_content_area(GTK_DIALOG(dialog)), hbox, 0, 0, 0);
	gtkhx_box_pack(hbox, label, 0, 0, 0);
	gtkhx_box_pack(hbox, name_entry, 0, 0, 0);
	gtkhx_box_pack(gtkhx_dialog_action_area(GTK_DIALOG(dialog)), ok, 0, 0, 0);
	gtkhx_box_pack(gtkhx_dialog_action_area(GTK_DIALOG(dialog)), cancel, 0, 0, 0);
	g_object_set_data(G_OBJECT(cancel), "dialog", dialog);
	g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_save), 0);
	g_object_set_data(G_OBJECT(ok), "name", name_entry);
	g_object_set_data(G_OBJECT(ok), "dialog", dialog);
	g_signal_connect(ok, "clicked", G_CALLBACK(bookmark_save), 0);


	/* Phase 4.2: gtk_widget_grab_default removed (use gtk_window_set_default_widget if needed) */
	gtk_window_present(GTK_WINDOW(dialog));
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
static void compress_combo_changed (GtkComboBox *combo, gpointer data)
{
	int i = gtk_combo_box_get_active (combo);
	(void) data;
	if (i < 0)
		return;
	g_object_set_data (G_OBJECT (combo), "compress", GINT_TO_POINTER (i));
}
#endif

#ifdef CONFIG_CIPHER
static void cipher_combo_changed (GtkComboBox *combo, gpointer data)
{
	int i = gtk_combo_box_get_active (combo);
	(void) data;
	if (i < 0)
		return;
	g_object_set_data (G_OBJECT (combo), "cipher", GINT_TO_POINTER (i));
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
#endif
#ifdef CONFIG_CIPHER
	GtkWidget *cipher_label;
#endif
	GtkWidget *button_connect, *button_cancel;
	GtkWidget *bookmarkmenu;
	GtkWidget *hbuttonbox1;
	GtkWidget *save_button;
	GtkWidget *port_label;
	session *sess = data;

	if (connect_window) {
		gtk_window_present(GTK_WINDOW(connect_window));
		return;
	}

	connect_window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(connect_window), "Connect");
	/* Phase 4.2: gtk_window_set_position removed in GTK 4 */
	g_signal_connect(connect_window, "destroy",
			   G_CALLBACK(close_connect_window), 0);
	vbox1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtkhx_widget_set_child(connect_window, vbox1);
	(gtk_widget_set_margin_start(vbox1, 10), gtk_widget_set_margin_end(vbox1, 10), gtk_widget_set_margin_top(vbox1, 10), gtk_widget_set_margin_bottom(vbox1, 10));


	help_label = gtk_label_new(_("Enter the server address, and if you have an account, your login and password. If not, leave the login and password blank."));
	gtkhx_box_pack(vbox1, help_label, 0, 1, 0);
	gtk_label_set_justify(GTK_LABEL(help_label), GTK_JUSTIFY_LEFT);
	gtk_label_set_wrap(GTK_LABEL(help_label), 1);
	gtk_label_set_xalign(GTK_LABEL(help_label), 0.0);

	frame1 = gtk_frame_new(0);
	gtkhx_box_pack(vbox1, frame1, 1, 1, 0);

 	table1 = gtkhx_grid_new_table(3, 6, 0);
	gtkhx_widget_set_child(frame1, table1);
	(gtk_widget_set_margin_start(table1, 10), gtk_widget_set_margin_end(table1, 10), gtk_widget_set_margin_top(table1, 10), gtk_widget_set_margin_bottom(table1, 10));
	gtk_grid_set_row_spacing(GTK_GRID(table1), 5);
	gtk_grid_set_column_spacing(GTK_GRID(table1), 5);

	server_label = gtk_label_new(_("Server:"));
	gtkhx_grid_attach_table(GTK_GRID(table1), server_label, 0, 1, 0, 1,
			 (GTK_FILL),
			 0, 0, 0);
	gtk_label_set_justify(GTK_LABEL(server_label), GTK_JUSTIFY_LEFT);
	gtk_label_set_xalign(GTK_LABEL(server_label), 0.0);

	login_label = gtk_label_new(_("Login:"));
	gtkhx_grid_attach_table(GTK_GRID(table1), login_label, 0, 1, 1, 2,
			 (GTK_FILL),
			 0, 0, 0);
	gtk_label_set_justify(GTK_LABEL(login_label), GTK_JUSTIFY_LEFT);
	gtk_label_set_xalign(GTK_LABEL(login_label), 0.0);

	pass_label = gtk_label_new(_("Password:"));
	gtkhx_grid_attach_table(GTK_GRID(table1), pass_label, 0, 1, 2, 3,
			 (GTK_FILL),
			 0, 0, 0);
	gtk_label_set_justify(GTK_LABEL(pass_label), GTK_JUSTIFY_LEFT);
	gtk_label_set_xalign(GTK_LABEL(pass_label), 0.0);

	hope = gtk_check_button_new_with_label(_("Secure (HOPE)"));
	gtk_check_button_set_active((GtkCheckButton*)hope, 0);
	gtkhx_grid_attach_table(GTK_GRID(table1), hope, 0, 1, 3, 4,
			 (GTK_EXPAND|GTK_FILL),
			 0, 0, 0);
	
#ifdef CONFIG_COMPRESS
	compress_label = gtk_label_new(_("Compress: "));
	gtkhx_grid_attach_table(GTK_GRID(table1), compress_label, 0, 1, 4, 5,
			 (GTK_FILL),
			 0, 0, 0);
	gtk_label_set_justify(GTK_LABEL(compress_label), GTK_JUSTIFY_LEFT);
	gtk_label_set_xalign(GTK_LABEL(compress_label), 0.0);

	compress_menu = gtk_combo_box_text_new ();
	gtkhx_grid_attach_table(GTK_GRID(table1), compress_menu, 1, 2, 4, 5, GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (compress_menu), "NONE");
	{
		int i;
		for (i = 0; valid_compressors[i]; i++)
			gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (compress_menu),
			                                valid_compressors[i]);
	}
	g_signal_connect (compress_menu, "changed",
	                  G_CALLBACK (compress_combo_changed), NULL);
	gtk_combo_box_set_active (GTK_COMBO_BOX (compress_menu), 0);
#endif

#ifdef CONFIG_CIPHER
	cipher_label = gtk_label_new(_("Cipher: "));
	gtkhx_grid_attach_table(GTK_GRID(table1), cipher_label, 0, 1, 5, 6,
			 (GTK_FILL),
			 0, 0, 0);
	gtk_label_set_justify(GTK_LABEL(cipher_label), GTK_JUSTIFY_LEFT);
	gtk_label_set_xalign(GTK_LABEL(cipher_label), 0.0);

	cipher_menu = gtk_combo_box_text_new ();
	gtkhx_grid_attach_table(GTK_GRID(table1), cipher_menu, 1, 2, 5, 6, GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (cipher_menu), "NONE");
	{
		int i;
		for (i = 0; valid_ciphers[i]; i++)
			gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (cipher_menu),
			                                valid_ciphers[i]);
	}
	g_signal_connect (cipher_menu, "changed",
	                  G_CALLBACK (cipher_combo_changed), NULL);
	gtk_combo_box_set_active (GTK_COMBO_BOX (cipher_menu), 0);
#endif

	address_entry = gtk_entry_new();
	gtkhx_grid_attach_table(GTK_GRID(table1), address_entry, 1, 2, 0, 1,
			 (GTK_EXPAND | GTK_FILL),
			 0, 0, 0);

	port_label = gtk_label_new(_("Port:"));
	gtkhx_grid_attach_table(GTK_GRID(table1), port_label, 2, 3, 0, 1, GTK_FILL, 0, 0, 0);

	gtk_label_set_justify(GTK_LABEL(port_label), GTK_JUSTIFY_LEFT);
	gtk_label_set_xalign(GTK_LABEL(port_label), 0.0);

	port_entry = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(port_entry), 6);
	gtk_editable_set_text(GTK_EDITABLE(port_entry), "5500");
	gtk_widget_set_size_request(port_entry, 45, 0);
	gtkhx_grid_attach_table(GTK_GRID(table1), port_entry, 3, 4, 0, 1,
			  0,
			 0, 0, 0);


	login_entry = gtk_entry_new();
	gtkhx_grid_attach_table(GTK_GRID(table1), login_entry, 1, 2, 1, 2,
			 (GTK_EXPAND | GTK_FILL),
			 0, 0, 0);
	password_entry = gtk_entry_new();
	gtk_entry_set_visibility(GTK_ENTRY(password_entry), 0);
	gtkhx_grid_attach_table(GTK_GRID(table1), password_entry, 1, 2, 2, 3,
			 (GTK_EXPAND | GTK_FILL),
			 0, 0, 0);

	bookmarkmenu = gtk_combo_box_text_new ();
	gtkhx_grid_attach_table(GTK_GRID(table1), bookmarkmenu, 4, 5, 0, 1,
			 0,
			 0, 0, 0);

	{
		static const char *const builtin_names[] = {
			"Hotline Communications", "CafeLinux", "GtkHx", "SiN Grafix"
		};
		GArray *entries = g_array_new (FALSE, FALSE, sizeof (BookmarkEntry));
		int i;

		list_bookmarks (bookmarkmenu, entries);
		for (i = 0; i < 4; i++) {
			BookmarkEntry e = { i + 1, NULL };
			gtk_combo_box_text_append_text (
				GTK_COMBO_BOX_TEXT (bookmarkmenu), builtin_names[i]);
			g_array_append_val (entries, e);
		}
		g_object_set_data_full (G_OBJECT (bookmarkmenu), "entries",
		                        entries, bookmark_entries_free);
		g_signal_connect (bookmarkmenu, "changed",
		                  G_CALLBACK (bookmark_combo_changed), NULL);
	}

	/* Phase 4.x: GtkButtonBox is gone. A horizontal GtkBox with the
	 * usual button-row spacing is the documented replacement;
	 * gtkhx_widget_set_child below dispatches to gtk_box_append for us. */
	hbuttonbox1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtkhx_box_pack(vbox1, hbuttonbox1, 1, 1, 0);

	save_button = gtk_button_new_with_label(_("Save..."));
	gtkhx_widget_set_child(hbuttonbox1, save_button);
	g_signal_connect(save_button, "clicked", G_CALLBACK(save_dialog), 0);

	button_cancel = gtk_button_new_with_label(_("Cancel"));
	g_signal_connect(button_cancel, "clicked", G_CALLBACK(close_connect_window), 0);
	gtkhx_widget_set_child(hbuttonbox1, button_cancel);

	button_connect = gtk_button_new_with_label(_("Connect"));
	g_signal_connect(button_connect, "clicked", G_CALLBACK(server_connect), sess);
	gtkhx_widget_set_child(hbuttonbox1, button_connect);

	gtk_window_present(GTK_WINDOW(connect_window));
	init_keyaccel(connect_window);
	gtk_widget_grab_focus(address_entry);
}

void connect_bookmark_name(char *name)
{
	create_connect_window(0,&the_session);
	open_bookmark(0, name);
}

G_GNUC_END_IGNORE_DEPRECATIONS
/* Phase 4.13: end of file-level deprecation suppression — see top of file. */
