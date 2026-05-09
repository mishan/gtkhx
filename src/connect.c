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
#include "connect.h"

/* Phase 5: the file-level G_GNUC_BEGIN_IGNORE_DEPRECATIONS pragma
 * that used to live here suppressed warnings from the GtkComboBoxText
 * dropdowns + the GtkDialog bookmark prompts. Both migrations are
 * done (AdwComboRow / AdwAlertDialog), so the pragma is no longer
 * needed and the matching G_GNUC_END is also gone from the bottom
 * of the file. */

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

/* Wired to AdwDialog::closed — fires after the dialog has dismissed
 * (close-X, ESC, or our own adw_dialog_close from server_connect).
 * All we do is clear the singleton pointer so the next
 * create_connect_window builds a fresh dialog. */
static void close_connect_window (void)
{
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

/* Phase 5: shared "actually connect" path. Plumbs the compress /
 * cipher algorithm strings onto sess->htlc (zeroing them when the
 * connection isn't HOPE-secure or when no algorithm is selected),
 * resolves the port string, and fires hx_connect.
 *
 * Used by server_connect (form-driven) and the SplitButton bookmark
 * paths (data-driven, dialog-bypassing).
 *
 * compress / cipher are the AdwComboRow indexes (0 == NONE), the
 * non-zero values index into valid_compressors[] / valid_ciphers[]. */
static void
connect_with_args (session *sess,
                   const char *server, guint16 port,
                   const char *login,  const char *pass,
                   char secure, char compress, char cipher)
{
	(void) compress; (void) cipher;

#ifdef CONFIG_COMPRESS
	memset (sess->htlc.compressalg, 0, sizeof (sess->htlc.compressalg));
	if (secure && compress) {
		const char *compress_algo = valid_compressors[compress - 1];
		if (compress_algo && valid_compress (compress_algo)) {
			size_t colen = strlen (compress_algo);
			if (colen >= sizeof (sess->htlc.compressalg))
				colen = sizeof (sess->htlc.compressalg) - 1;
			memcpy (sess->htlc.compressalg, compress_algo, colen);
			sess->htlc.compressalg[colen] = 0;
		}
	}
#endif
#ifdef CONFIG_CIPHER
	memset (sess->htlc.cipheralg, 0, sizeof (sess->htlc.cipheralg));
	if (secure && cipher) {
		const char *cipher_algo = valid_ciphers[cipher - 1];
		if (cipher_algo && valid_cipher (cipher_algo)) {
			size_t cilen = strlen (cipher_algo);
			if (cilen >= sizeof (sess->htlc.cipheralg))
				cilen = sizeof (sess->htlc.cipheralg) - 1;
			memcpy (sess->htlc.cipheralg, cipher_algo, cilen);
			sess->htlc.cipheralg[cilen] = 0;
		}
	}
#endif

	hx_connect (&sess->htlc, server, port, login, pass, secure);
}

static void server_connect (GtkWidget *widget, gpointer data)
{
	const char *server, *login, *pass, *portstr;
	int secure;
	session *sess = data;
	guint16 port = 5500;
	char compress = 0, cipher = 0;
	(void) widget;

	login   = gtk_editable_get_text (GTK_EDITABLE (login_entry));
	server  = gtk_editable_get_text (GTK_EDITABLE (address_entry));
	pass    = gtk_editable_get_text (GTK_EDITABLE (password_entry));
	portstr = gtk_editable_get_text (GTK_EDITABLE (port_entry));
	secure  = adw_switch_row_get_active (ADW_SWITCH_ROW (hope));
#ifdef CONFIG_COMPRESS
	compress = adw_combo_row_get_selected (ADW_COMBO_ROW (compress_menu));
#endif
#ifdef CONFIG_CIPHER
	cipher = adw_combo_row_get_selected (ADW_COMBO_ROW (cipher_menu));
#endif

	if (portstr && portstr[0])
		port = atoi (portstr);

	connect_with_args (sess, server, port, login, pass,
	                   (char) secure, compress, cipher);

	if (connect_window) {
		adw_dialog_close (ADW_DIALOG (connect_window));
		connect_window = 0;
	}
}

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

	if (hope)
		adw_switch_row_set_active (ADW_SWITCH_ROW (hope), secure ? TRUE : FALSE);
#ifdef CONFIG_COMPRESS
	if (compress_menu)
		adw_combo_row_set_selected (ADW_COMBO_ROW (compress_menu), compress);
#endif
#ifdef CONFIG_CIPHER
	if (cipher_menu)
		adw_combo_row_set_selected (ADW_COMBO_ROW (cipher_menu), cipher);
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
	fprintf (bm, "%c", (int) len);
	fprintf (bm, "%s", login);
	fwrite (zeros, 1, len_total, bm);

	len = strlen (pass);
	len_total = 33 - len;
	fprintf (bm, "%c", (int) len);
	fprintf (bm, "%s", pass);
	fwrite (zeros, 1, len_total, bm);

	len = strlen (server);
	len_total = 256 - len;
	fprintf (bm, "%c", (int) len);
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

/* Phase 5: parsed contents of an HTsc-format bookmark file.
 * server/port are split (port populated only if the bookmark stored
 * "host:port"); login/pass are NUL-terminated within their 33-byte
 * fields. */
struct bookmark_parsed {
	char server[128];
	char login[33];
	char pass[33];
	char port[HOSTLEN];
	char secure;
	char compress;
	char cipher;
};

/* Phase 5: read the new-format (HTsc) bookmark at $name and fill *out.
 * Returns 0 on success.
 *  -1: bookmark doesn't exist
 *  -2: file is in the legacy format (caller should run prompt_conversion)
 *  -3: short read / corrupt file
 *
 * On the legacy-format path the caller gets the file path through
 * *out_legacy_path so it can hand it to prompt_conversion; the path
 * is g_strdup'd, caller g_frees. */
static int
bookmark_parse (const char *name, struct bookmark_parsed *out, char **out_legacy_path)
{
	char *path = NULL;
	int   bm   = open_bookmark_file (name, &path);
	char junk[132];
	char header[5];
	char len_addr;
	char *p;
	size_t len;

	if (out_legacy_path)
		*out_legacy_path = NULL;
	if (bm < 0)
		return -1;

	memset (out, 0, sizeof (*out));

	if (read (bm, header, 4) != 4) goto bad;
	header[4] = '\0';
	if (strcmp (header, "HTsc") != 0) {
		close (bm);
		if (out_legacy_path)
			*out_legacy_path = path;
		else
			g_free (path);
		return -2;
	}
	g_free (path);

	if (read (bm, junk, 132)         != 132) goto bad;
	if (read (bm, out->login, 33)    !=  33) goto bad;
	if (read (bm, &len_addr, 1)      !=   1) goto bad;
	if (read (bm, out->pass, 33)     !=  33) goto bad;
	if (read (bm, &len_addr, 1)      !=   1) goto bad;

	len = len_addr;
	if (len >= sizeof (out->server)) goto bad;
	if (read (bm, out->server, len) != (ssize_t) len) goto bad;
	out->server[len] = 0;

	if (read (bm, &out->secure,   1) != 1) goto bad;
	if (read (bm, &out->compress, 1) != 1) goto bad;
	if (read (bm, &out->cipher,   1) != 1) goto bad;

	close (bm);

	out->port[0] = '\0';
	if ((p = strrchr (out->server, ':'))) {
		size_t i;
		for (i = 0; i < strlen (out->server); i++) {
			if (&out->server[i] == p) {
				out->server[i] = 0;
				break;
			}
		}
		p++;
		g_snprintf (out->port, sizeof (out->port), "%u", atoi (p));
	}
	return 0;

bad:
	close (bm);
	return -3;
}

/* Phase 5 legacy entry point — fills the connect-dialog widgets from
 * a saved bookmark. Used internally by code that opens the dialog
 * first (the SplitButton menu uses connect_open_bookmark_by_name
 * instead, which connects directly without showing the dialog). */
static void open_bookmark(GtkWidget *widget, gpointer data)
{
	struct bookmark_parsed bm;
	char *legacy_path = NULL;
	int rc;
	(void) widget;

	rc = bookmark_parse ((char *) data, &bm, &legacy_path);
	if (rc == -1) {
		g_warning ("%s \"%s\"\n", _("No such bookmark"), (char *) data);
		return;
	}
	if (rc == -2) {
		prompt_conversion (legacy_path);
		g_free (legacy_path);
		return;
	}
	if (rc != 0) {
		g_warning ("%s \"%s\"\n",
		           _("Could not read bookmark"), (char *) data);
		return;
	}

	set_the_entries (bm.server, bm.login, bm.pass, bm.port,
	                 bm.secure, bm.compress, bm.cipher);
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
};
#define BUILTIN_BOOKMARK_MAX 1

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

/* Phase 5: connect to a saved bookmark directly — no dialog. The
 * SplitButton dropdown calls this when the user picks a bookmark.
 * Parses the file via bookmark_parse, sets up the session's
 * compress/cipher state, and calls hx_connect.
 *
 * Legacy-format bookmarks fall back to the prompt_conversion flow,
 * which opens an AdwAlertDialog and rewrites the file in-place;
 * after conversion the user has to click the bookmark again to
 * connect. We don't auto-retry because converting + connecting
 * silently would hide the format change from the user. */
void
connect_open_bookmark_by_name (const char *name)
{
	struct bookmark_parsed bm;
	char *legacy_path = NULL;
	int rc;
	guint16 port = 5500;

	if (!name || !*name)
		return;

	rc = bookmark_parse (name, &bm, &legacy_path);
	if (rc == -1) {
		g_warning ("%s \"%s\"\n", _("No such bookmark"), name);
		return;
	}
	if (rc == -2) {
		prompt_conversion (legacy_path);
		g_free (legacy_path);
		return;
	}
	if (rc != 0) {
		g_warning ("%s \"%s\"\n", _("Could not read bookmark"), name);
		return;
	}

	if (bm.port[0])
		port = atoi (bm.port);

	connect_with_args (&the_session, bm.server, port,
	                   bm.login, bm.pass,
	                   bm.secure, bm.compress, bm.cipher);
}

/* Phase 5: connect to a built-in "well-known" Hotline server. After
 * the recent cleanup only Hotline Communications (idx 1, hlserver.com)
 * remains; the switch is preserved as a structure so adding more
 * built-ins later is a single new case. */
void
connect_open_builtin_bookmark (int idx)
{
	const char *server;

	switch (idx) {
	case 1: server = "hlserver.com"; break;
	default:
		g_warning ("connect_open_builtin_bookmark: unknown idx %d", idx);
		return;
	}

	connect_with_args (&the_session, server, 5500, "", "", 0, 0, 0);
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
	secure = adw_switch_row_get_active (ADW_SWITCH_ROW (hope));
#ifdef CONFIG_COMPRESS
	compress = adw_combo_row_get_selected (ADW_COMBO_ROW (compress_menu));
#endif
#ifdef CONFIG_CIPHER
	cipher = adw_combo_row_get_selected (ADW_COMBO_ROW (cipher_menu));
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
	fprintf (bookmark, "%c%s", (int) len, login);
	fwrite (zeros, 1, len_total, bookmark);

	len = strlen (pass);
	len_total = 33 - len;
	fprintf (bookmark, "%c%s", (int) len, pass);
	fwrite (zeros, 1, len_total, bookmark);

	len = strlen (server_str);
	len_total = 256 - len;
	fprintf (bookmark, "%c%s", (int) len, server_str);

	fprintf (bookmark, "%c%c%c", secure, compress, cipher);
	len_total -= 3;
	fwrite (zeros, 1, len_total, bookmark);

	fclose (bookmark);

	/* Phase 5: refresh the SplitButton's bookmark dropdown so the
	 * just-saved entry shows up without an app restart. Skipped on
	 * the failure paths via the goto out below. */
	toolbar_refresh_bookmarks ();

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

/* Phase 5: AdwDialog with AdwPreferencesGroup form rows replaces the
 * hand-laid GtkGrid + GtkFrame + per-cell label/entry layout. The
 * bookmark combo is gone — the SplitButton dropdown on the toolbar
 * Connect button does that job now, and there's no point repeating
 * the menu inside the dialog itself.
 *
 * Layout:
 *   AdwHeaderBar
 *     end:  Save Bookmark…   (pill button)
 *     end:  Connect           (pill button, suggested-action)
 *   AdwPreferencesGroup "Server"
 *     description: help text
 *     AdwEntryRow Server, AdwSpinRow Port
 *   AdwPreferencesGroup "Account"
 *     AdwEntryRow Login, AdwPasswordEntryRow Password
 *   AdwPreferencesGroup "Connection"
 *     AdwSwitchRow Secure (HOPE)
 *   #ifdef CONFIG_COMPRESS / CIPHER: AdwComboRow Compress / Cipher
 *     in the same Connection group
 *
 * Cancel is the close-X on the headerbar; ESC dismisses. The header's
 * Connect button is the default response so Enter from any of the
 * entry rows submits. */
void create_connect_window (GtkWidget *btn, gpointer data)
{
	AdwDialog *dlg;
	GtkWidget *toolbar_view, *header;
	GtkWidget *content, *clamp;
	GtkWidget *connect_action_btn, *save_action_btn;
	AdwPreferencesGroup *server_grp, *account_grp, *conn_grp;
	session *sess = data;
	(void) btn;

	if (connect_window) {
		adw_dialog_present (ADW_DIALOG (connect_window), NULL);
		return;
	}

	dlg = ADW_DIALOG (adw_dialog_new ());
	adw_dialog_set_title (dlg, _("Connect"));
	adw_dialog_set_content_width  (dlg, 480);
	adw_dialog_set_content_height (dlg, 560);

	connect_window = GTK_WIDGET (dlg);
	g_signal_connect (dlg, "closed",
	                  G_CALLBACK (close_connect_window), NULL);

	/* Header bar: Save..., Connect (suggested) on the end. Close-X
	 * is automatic on the start (handled by AdwDialog). */
	header = adw_header_bar_new ();

	save_action_btn = gtk_button_new_with_label (_("Save Bookmark…"));
	g_signal_connect (save_action_btn, "clicked",
	                  G_CALLBACK (save_dialog), NULL);
	adw_header_bar_pack_end (ADW_HEADER_BAR (header), save_action_btn);

	connect_action_btn = gtk_button_new_with_label (_("Connect"));
	gtk_widget_add_css_class (connect_action_btn, "suggested-action");
	g_signal_connect (connect_action_btn, "clicked",
	                  G_CALLBACK (server_connect), sess);
	adw_header_bar_pack_end (ADW_HEADER_BAR (header), connect_action_btn);

	/* AdwToolbarView wraps the headerbar + scrollable content. */
	toolbar_view = adw_toolbar_view_new ();
	adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

	/* Content vbox holding the preference groups. AdwClamp keeps the
	 * form a comfortable width (no edge-to-edge stretching on wide
	 * dialogs) and centers it horizontally. */
	content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
	gtk_widget_set_margin_top    (content, 18);
	gtk_widget_set_margin_bottom (content, 18);
	gtk_widget_set_margin_start  (content, 18);
	gtk_widget_set_margin_end    (content, 18);

	/* ------------------------------ Server group ------------------------------ */
	server_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (server_grp, _("Server"));
	adw_preferences_group_set_description (server_grp,
		_("Enter the server address. If you have an account, fill in your "
		  "login and password below; otherwise leave them blank."));

	address_entry = adw_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (address_entry),
	                               _("Server"));
	gtk_entry_set_activates_default (GTK_ENTRY (address_entry), TRUE);
	adw_preferences_group_add (server_grp, address_entry);

	port_entry = adw_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (port_entry),
	                               _("Port"));
	gtk_editable_set_text (GTK_EDITABLE (port_entry), "5500");
	gtk_entry_set_activates_default (GTK_ENTRY (port_entry), TRUE);
	adw_preferences_group_add (server_grp, port_entry);

	gtk_box_append (GTK_BOX (content), GTK_WIDGET (server_grp));

	/* ----------------------------- Account group ----------------------------- */
	account_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (account_grp, _("Account"));

	login_entry = adw_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (login_entry),
	                               _("Login"));
	gtk_entry_set_activates_default (GTK_ENTRY (login_entry), TRUE);
	adw_preferences_group_add (account_grp, login_entry);

	password_entry = adw_password_entry_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (password_entry),
	                               _("Password"));
	gtk_entry_set_activates_default (GTK_ENTRY (password_entry), TRUE);
	adw_preferences_group_add (account_grp, password_entry);

	gtk_box_append (GTK_BOX (content), GTK_WIDGET (account_grp));

	/* --------------------------- Connection group --------------------------- */
	conn_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (conn_grp, _("Connection"));

	hope = adw_switch_row_new ();
	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (hope),
	                               _("Secure (HOPE)"));
	adw_action_row_set_subtitle   (ADW_ACTION_ROW (hope),
	                               _("Encrypt and optionally compress the connection"));
	adw_switch_row_set_active     (ADW_SWITCH_ROW (hope), FALSE);
	adw_preferences_group_add (conn_grp, hope);

#ifdef CONFIG_COMPRESS
	{
		GtkStringList *list = gtk_string_list_new (NULL);
		int i;
		gtk_string_list_append (list, "NONE");
		for (i = 0; valid_compressors[i]; i++)
			gtk_string_list_append (list, valid_compressors[i]);
		compress_menu = adw_combo_row_new ();
		adw_preferences_row_set_title (ADW_PREFERENCES_ROW (compress_menu),
		                               _("Compression"));
		adw_combo_row_set_model    (ADW_COMBO_ROW (compress_menu),
		                            G_LIST_MODEL (list));
		adw_combo_row_set_selected (ADW_COMBO_ROW (compress_menu), 0);
		g_object_unref (list);
		adw_preferences_group_add (conn_grp, compress_menu);
	}
#endif

#ifdef CONFIG_CIPHER
	{
		GtkStringList *list = gtk_string_list_new (NULL);
		int i;
		gtk_string_list_append (list, "NONE");
		for (i = 0; valid_ciphers[i]; i++)
			gtk_string_list_append (list, valid_ciphers[i]);
		cipher_menu = adw_combo_row_new ();
		adw_preferences_row_set_title (ADW_PREFERENCES_ROW (cipher_menu),
		                               _("Cipher"));
		adw_combo_row_set_model    (ADW_COMBO_ROW (cipher_menu),
		                            G_LIST_MODEL (list));
		adw_combo_row_set_selected (ADW_COMBO_ROW (cipher_menu), 0);
		g_object_unref (list);
		adw_preferences_group_add (conn_grp, cipher_menu);
	}
#endif

	gtk_box_append (GTK_BOX (content), GTK_WIDGET (conn_grp));

	/* AdwClamp wrapping content keeps the form readable on wide
	 * dialogs (max-width ~600 by default). Wrap in a scrolled
	 * window so the dialog can still shrink on small screens
	 * without clipping the bottom group. */
	clamp = adw_clamp_new ();
	adw_clamp_set_child (ADW_CLAMP (clamp), content);
	{
		GtkWidget *scrolled = gtk_scrolled_window_new ();
		gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
		                                GTK_POLICY_NEVER,
		                                GTK_POLICY_AUTOMATIC);
		gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled),
		                               clamp);
		adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view),
		                              scrolled);
	}

	adw_dialog_set_child (dlg, toolbar_view);

	/* Default-widget so Enter from any AdwEntryRow submits Connect. */
	gtk_widget_set_receives_default (connect_action_btn, TRUE);

	adw_dialog_present (dlg, GTK_WIDGET (gtkhx_active_window ()));
	gtk_widget_grab_focus (address_entry);
}

void connect_bookmark_name(char *name)
{
	create_connect_window(0,&the_session);
	open_bookmark(0, name);
}

