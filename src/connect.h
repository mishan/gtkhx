#ifndef HX_CONNECT_H
#define HX_CONNECT_H

extern void connect_set_entries (const char *address, const char *login,
                                 const char *pasword, guint16 port);
extern void set_the_entries (char *address, char *login, char *password,
                             char *port, char secure, char compress,
                             char cipher, char tls);
extern void connect_bookmark_name (char *name);
extern void create_connect_window (GtkWidget *btn, gpointer data);

extern int valid_compress (const char *compressalg);
extern char *valid_compressors[];
extern int valid_cipher (const char *cipheralg);
extern char *valid_ciphers[];

/* Translate the connect dialog's AdwComboRow cipher index (0 = no
 * cipher, 1..N indexes valid_ciphers[N-1]) to a stable on-disk
 * bookmark byte from bookmark_cipher.h. Save paths use this when
 * writing the cipher byte to the HTsc bookmark file so the byte's
 * semantics stay stable across UI dropdown reorderings. */
extern unsigned char connect_dropdown_to_cipher_byte (unsigned int dropdown_idx);

/* Inverse of connect_dropdown_to_cipher_byte: translate a stable
 * bookmark cipher byte to the matching AdwComboRow index, or 0
 * ("no cipher") if the byte names a cipher the dropdown no longer
 * offers (e.g. RC4 after claude/remove-rc4). Used by load paths
 * that pre-fill the connect dialog from a bookmark — the RC4
 * intercept fires earlier in the pipeline, so any caller reaching
 * here with an RC4 byte is a defensive fallback. */
extern unsigned int connect_cipher_byte_to_dropdown (unsigned char byte);
/* list_n moved to src/algo_list.{c,h} — re-include so historic
 * connect.h consumers keep finding the declaration without an
 * extra include. */
#include "algo_list.h"

/* Phase 5: build a fresh GMenu of saved bookmark names. Entries
 * target the "app.open_bookmark" GAction with the bookmark name as
 * the parameter (g_variant string). The toolbar's AdwSplitButton
 * uses this to populate its dropdown. The returned GMenu is a
 * floating-ref new GMenu — caller takes ownership. */
extern GMenu *connect_build_bookmark_menu (void);

/* Phase 5: invoked from app.open_bookmark to load and connect to
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

/* Phase 5: load one of the hardcoded "well-known" Hotline server
 * bookmarks (Hotline Communications / CafeLinux / GtkHx / SiN
 * Grafix). idx is 1..4 — same numbering the connect dialog's
 * builtin combo uses. */
extern void connect_open_builtin_bookmark (int idx);

#endif
