#ifndef HX_CONNECT_H
#define HX_CONNECT_H

extern void connect_set_entries (const char *address, const char *login,
                                 const char *pasword, guint16 port);
extern void set_the_entries (char *address, char *login, char *password,
                             char *port, char secure, char compress,
                             char cipher, char tls);
extern void connect_bookmark_name (char *name);
extern void create_connect_window (GtkWidget *btn, gpointer data);

/* The cipher / compression picker vocabulary lives in the gtkhx-ui Rust
 * crate now (cipher_vocab.rs), shared between the Connect dialog and the
 * Connections settings page. No C consumer of it
 * remains, so connect.h no longer re-exports it. The stable on-disk
 * cipher byte ↔ name mapping now lives in the hxbookmarks Rust crate. */

/* build a fresh GMenu of saved bookmark names. Entries
 * target the "app.open_bookmark" GAction with the bookmark name as
 * the parameter (g_variant string). The toolbar's AdwSplitButton
 * uses this to populate its dropdown. The returned GMenu is a
 * floating-ref new GMenu — caller takes ownership. */
extern GMenu *connect_build_bookmark_menu (void);

/* invoked from app.open_bookmark to load and connect to
 * a named bookmark. Wraps the file-IO heavy open_bookmark call
 * with a public name so toolbar.c (and any future caller) doesn't
 * have to reach into connect.c's static functions. */
extern void connect_open_bookmark_by_name (const char *name);

/* Reconnect to the server captured in connect.c's last-connection
 * cache, bypassing the Connect dialog. The cache is populated by
 * every connect_with_args call (form-driven, bookmark-driven, and
 * URL-driven), and survives past hx_htlc_close. Falls back to
 * opening the Connect dialog if no successful connect has happened
 * this run. Wired to the toolbar's "Lost connection — Reconnect"
 * banner button. */
extern void connect_reconnect_last (void);

/* Parse a hotline:// URL and connect to the server it names.
 *
 * Accepted shape (de facto standard from the original Mac client):
 *
 *     hotline://[login[:password]@]host[:port][/]
 *
 * Components are URL-decoded. Missing port defaults to 5500. The
 * connection runs plain Hotline — no HOPE / no TLS / no compression /
 * no cipher — because the URL form doesn't carry those parameters.
 * The user can bookmark the URL first (connect_save_hotline_url_as_
 * bookmark) and edit transport security in Settings → Connections if
 * they want HOPE / TLS for that server. Returns TRUE on a launched
 * connect, FALSE on a malformed URL. */
extern gboolean connect_open_hotline_url (const char *url);

/* Parse a hotline:// URL and persist it as a bookmark in the TOML store.
 * The bookmark name is the URL host; if a bookmark with that name already
 * exists this returns FALSE with err set to a translated "already exists"
 * message — the caller surfaces it via toast so the user can rename or
 * delete the existing entry in Settings → Connections.
 *
 * `out_name` (optional) receives the chosen bookmark name on success;
 * caller frees with g_free. NULL on failure. */
extern gboolean connect_save_hotline_url_as_bookmark (const char *url,
                                                      char **out_name,
                                                      GError **err);

#endif
