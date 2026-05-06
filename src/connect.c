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
#include <adwaita.h>
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

/* Phase 5: legacy-bookmark conversion runs from the AdwAlertDialog
 * "convert" response. Path is passed through user_data; freed via
 * the dialog's "closed" signal once the response is handled.
 *
 * Old shape was a click handler on the OK button that pulled the
 * dialog pointer out of qdata so it could destroy itself. With
 * AdwAlertDialog the dialog auto-dismisses when the response
 * handler returns, so the explicit destroy is gone — we only do
 * the file-rewrite work here. */
static void
convert_bookmark (AdwAlertDialog *dialog, const char *response, gpointer data)
{
	FILE *bm;
	char server[128];
	char login[64];
	char pass[64];
	char zeros[256];
	char *p, port[HOSTLEN];
	size_t len, len_total;

	(void) dialog;

	if (g_strcmp0 (response, "convert") != 0)
		return;

	bm = fopen ((char *) data, "r");
	if (!bm) {
		fprintf (stderr,
		         "Could not open '%s' for reading...Aborting\n",
		         (char *) data);
		exit (1);
	}
	memset (zeros, 0, 256);

	fgets (server, 128, bm);
	server[strlen (server) - 1] = '\0';
	fgets (login, 64, bm);
	login[strlen (login) - 1] = '\0';
	fgets (pass, 64, bm);
	strip_lf (pass);
	fclose (bm);

	port[0] = '\0';
	if ((p = strrchr (server, ':'))) {
		int i;
		for (i = 0; i < strlen (server); i++) {
			if (&(server[i]) == p) {
				server[i] = 0;
				break;
			}
		}
		p++;
		g_snprintf (port, sizeof (port), "%u", atoi (p));
	}

	set_the_entries (server, login, pass, port, 0, 0, 0);

	bm = fopen (data, "w");
	if (!bm) {
		fprintf (stderr,
		         "Could not open '%s' for writing...Aborting\n",
		         (char *) data);
		return;
	}

	fprintf (bm, "HTsc%c%c", 0, 1);
	fwrite (zeros, 1, 129, bm);

	len = strlen (login);
	len_total = 33 - len;
	fprintf (bm, "%c", len);
	fprintf (bm, "%s", login);
	fwrite (zeros, 1, len_total, bm);

	len = strlen (pass);
	len_total = 33 - len;
	fprintf (bm, "%c", len);
	fprintf (bm, "%s", pass);
	fwrite (zeros, 1, len_total, bm);

	len = strlen (server);
	len_total = 256 - len;
	fprintf (bm, "%c", len);
	fprintf (bm, "%s", server);

	/* secure:0, compress:0, cipher:0 */
	fprintf (bm, "%c%c%c", 0, 0, 0);
	fwrite (zeros, 1, len_total - 3, bm);

	fclose (bm);
}

static void
prompt_conversion_closed (AdwDialog *dialog, gpointer data)
{
	(void) dialog;
	g_free (data);
}

static void prompt_conversion (char *name)
{
	AdwDialog *dialog;
	char *path = g_strdup (name);

	dialog = adw_alert_dialog_new (
		_("Convert Bookmark"),
		_("This bookmark is written in an old GtkHx format. "
		  "Would you like to convert it to the new format?"));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "no",      _("_No"));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "convert", _("_Convert"));
	adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
	                                          "convert",
	                                          ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog),
	                                       "convert");
	adw_alert_dialog_set_close_response   (ADW_ALERT_DIALOG (dialog),
	                                       "no");

	g_signal_connect (dialog, "response",
	                  G_CALLBACK (convert_bookmark), path);
	g_signal_connect (dialog, "closed",
	                  G_CALLBACK (prompt_conversion_closed), path);

	adw_dialog_present (dialog,
	                    toolbar_window ? GTK_WIDGET (toolbar_window) : NULL);
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

/* Phase 5: scan a bookmarks dir and append each entry as a menu item
 * targeting app.open_bookmark with the bookmark name as a string
 * variant. Skips dotfiles (".", "..", and any hidden override files)
 * and de-dupes against names already in the menu so the legacy
 * ~/.hx/bookmarks/ pass doesn't add doubles for entries that exist
 * in both locations. */
static gboolean
menu_already_has_bookmark (GMenu *menu, const char *name)
{
	int i, n = g_menu_model_get_n_items (G_MENU_MODEL (menu));
	for (i = 0; i < n; i++) {
		GVariant *target;
		const char *existing;
		gboolean match = FALSE;

		target = g_menu_model_get_item_attribute_value (
			G_MENU_MODEL (menu), i, G_MENU_ATTRIBUTE_TARGET, NULL);
		if (!target)
			continue;
		existing = g_variant_get_string (target, NULL);
		match = g_strcmp0 (existing, name) == 0;
		g_variant_unref (target);
		if (match)
			return TRUE;
	}
	return FALSE;
}

static void
build_bookmark_menu_from_dir (GMenu *menu, const char *path)
{
	struct dirent *ent;
	DIR *dir;

	if (!path || !(dir = opendir (path)))
		return;

	while ((ent = readdir (dir))) {
		GMenuItem *item;

		if (*ent->d_name == '.')
			continue;
		if (menu_already_has_bookmark (menu, ent->d_name))
			continue;

		item = g_menu_item_new (ent->d_name, NULL);
		g_menu_item_set_action_and_target_value (
			item, "app.open_bookmark",
			g_variant_new_string (ent->d_name));
		g_menu_append_item (menu, item);
		g_object_unref (item);
	}
	closedir (dir);
}

/* Phase 5: same display names the connect-dialog combo uses (in
 * create_connect_window's builtin_names array). Indexes here MUST
 * line up with builtin_bookmark's switch on GPOINTER_TO_INT(data) —
 * 1..4 maps to hlserver / cafelinux / nasledov / singrafix. */
static const char *const builtin_bookmark_names[] = {
	NULL,                       /* index 0 unused */
	"Hotline Communications",
	"CafeLinux",
	"GtkHx",
	"SiN Grafix",
};
#define BUILTIN_BOOKMARK_MAX 4

GMenu *
connect_build_bookmark_menu (void)
{
	GMenu *menu = g_menu_new ();
	GMenu *builtins = g_menu_new ();
	GMenu *saved = g_menu_new ();
	char *primary = bookmarks_dir_primary ();
	char *legacy  = bookmarks_dir_legacy ();
	int i;

	/* Built-in section: hardcoded "well-known" Hotline servers from
	 * the connect dialog's builtin list. They target a separate
	 * action (app.connect_builtin) with an integer parameter so the
	 * action handler can dispatch to builtin_bookmark by index. */
	for (i = 1; i <= BUILTIN_BOOKMARK_MAX; i++) {
		GMenuItem *item = g_menu_item_new (builtin_bookmark_names[i], NULL);
		g_menu_item_set_action_and_target_value (
			item, "app.connect_builtin", g_variant_new_int32 (i));
		g_menu_append_item (builtins, item);
		g_object_unref (item);
	}
	g_menu_append_section (menu, NULL, G_MENU_MODEL (builtins));
	g_object_unref (builtins);

	/* Saved bookmarks: scanned from disk. Appended as a separate
	 * section so the GtkPopoverMenu draws a separator between the
	 * built-ins and the user-saved entries. */
	build_bookmark_menu_from_dir (saved, primary);
	build_bookmark_menu_from_dir (saved, legacy);
	if (g_menu_model_get_n_items (G_MENU_MODEL (saved)) > 0) {
		g_menu_append_section (menu, NULL, G_MENU_MODEL (saved));
	}
	g_object_unref (saved);

	g_free (primary);
	g_free (legacy);
	return menu;
}

void
connect_open_bookmark_by_name (const char *name)
{
	if (name && *name)
		open_bookmark (NULL, (gpointer) name);
}

/* Phase 5: invoked from app.connect_builtin to load one of the
 * hardcoded "well-known" servers by index (1..4). Wraps the static
 * builtin_bookmark so toolbar.c doesn't have to reach in. */
void
connect_open_builtin_bookmark (int idx)
{
	if (idx >= 1 && idx <= BUILTIN_BOOKMARK_MAX)
		builtin_bookmark (NULL, GINT_TO_POINTER (idx));
}

/* Phase 5: bookmark save migrates to AdwAlertDialog with the name
 * entry as extra-child. The response handler reads the entry, does
 * the file-write work, and lets the dialog auto-dismiss. The
 * cancel_save click handler is gone — "cancel" response (and ESC,
 * via close_response) handle it. */
static void
bookmark_save_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
	GtkEditable *name_entry;
	const char *name;
	const char *server, *login, *pass, *port;
	char secure;
	char compress = 0, cipher = 0;
	char *dir, *path = NULL, *server_str;
	FILE *bookmark = NULL;
	size_t len, len_total;
	char zeros[256];
	char *editable_name;
	int i;

	(void) data;

	if (g_strcmp0 (response, "save") != 0)
		return;

	name_entry = GTK_EDITABLE (adw_alert_dialog_get_extra_child (dialog));
	name = name_entry ? gtk_editable_get_text (name_entry) : NULL;
	if (!name || !*name) {
		error_dialog (_("Error"),
		              _("You must specify a name for this bookmark "
		                "with at least one character."));
		return;
	}

	server = gtk_editable_get_text (GTK_EDITABLE (address_entry));
	login  = gtk_editable_get_text (GTK_EDITABLE (login_entry));
	pass   = gtk_editable_get_text (GTK_EDITABLE (password_entry));
	port   = gtk_editable_get_text (GTK_EDITABLE (port_entry));
	secure = gtk_check_button_get_active ((GtkCheckButton *) hope);
#ifdef CONFIG_COMPRESS
	compress = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (compress_menu),
	                                               "compress"));
#endif
#ifdef CONFIG_CIPHER
	cipher = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (cipher_menu),
	                                             "cipher"));
#endif

	dir = bookmarks_dir_primary ();
	server_str = g_strdup_printf ("%s:%s", server, port);
	memset (zeros, 0, 256);

	/* Convert any '/' in the name to '\\' to avoid path traversal. */
	editable_name = g_strdup (name);
	len = strlen (editable_name);
	for (i = 0; i < len; i++) {
		if (editable_name[i] == '/')
			editable_name[i] = '\\';
	}
	path = g_build_filename (dir, editable_name, NULL);

	if (g_mkdir_with_parents (dir, 0770) != 0) {
		hx_printf_prefix (&the_session.htlc, 0,
		                  "Could not create bookmarks dir \"%s\": %s",
		                  dir, g_strerror (errno));
		goto out;
	}
	if (!(bookmark = fopen (path, "w"))) {
		hx_printf_prefix (&the_session.htlc, 0,
		                  "Could not open \"%s\" for writing.", path);
		goto out;
	}

	fprintf (bookmark, "HTsc%c%c", 0, 1);
	fwrite (zeros, 1, 129, bookmark);

	len = strlen (login);
	len_total = 33 - len;
	fprintf (bookmark, "%c%s", len, login);
	fwrite (zeros, 1, len_total, bookmark);

	len = strlen (pass);
	len_total = 33 - len;
	fprintf (bookmark, "%c%s", len, pass);
	fwrite (zeros, 1, len_total, bookmark);

	len = strlen (server_str);
	len_total = 256 - len;
	fprintf (bookmark, "%c%s", len, server_str);

	fprintf (bookmark, "%c%c%c", secure, compress, cipher);
	len_total -= 3;
	fwrite (zeros, 1, len_total, bookmark);

	fclose (bookmark);

out:
	g_free (path);
	g_free (dir);
	g_free (server_str);
	g_free (editable_name);
}

static void
save_dialog_entry_activate (GtkEntry *entry, gpointer data)
{
	(void) entry;
	g_signal_emit_by_name (data, "response", "save");
}

static void save_dialog (GtkWidget *widget, gpointer data)
{
	AdwDialog *dialog;
	GtkWidget *name_entry;

	(void) widget; (void) data;

	dialog = adw_alert_dialog_new (_("Save Bookmark"),
	                               _("Enter a name for this bookmark."));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "cancel", _("_Cancel"));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "save",   _("_Save"));
	adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
	                                          "save",
	                                          ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "save");
	adw_alert_dialog_set_close_response   (ADW_ALERT_DIALOG (dialog), "cancel");

	name_entry = gtk_entry_new ();
	gtk_entry_set_activates_default (GTK_ENTRY (name_entry), TRUE);
	g_signal_connect (name_entry, "activate",
	                  G_CALLBACK (save_dialog_entry_activate), dialog);
	adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), name_entry);

	g_signal_connect (dialog, "response",
	                  G_CALLBACK (bookmark_save_response), NULL);

	adw_dialog_present (dialog,
	                    toolbar_window ? GTK_WIDGET (toolbar_window) : NULL);
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
