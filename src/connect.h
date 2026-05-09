#ifndef HX_CONNECT_H
#define HX_CONNECT_H

extern void connect_set_entries (const char *address, const char *login, const char *pasword, guint16 port);
extern void set_the_entries(char *address, char *login, char *password, char *port, char secure, char compress, char cipher);
extern void connect_bookmark_name(char *name);
extern void create_connect_window (GtkWidget *btn, gpointer data);

#ifdef CONFIG_COMPRESS
extern int valid_compress (const char *compressalg);
extern char *valid_compressors[];
#endif
#ifdef CONFIG_CIPHER
extern int valid_cipher (const char *cipheralg);
extern char *valid_ciphers[];
#endif
extern guint8 *list_n (guint8 *list, guint16 listlen, unsigned int n);

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

/* Phase 5: load one of the hardcoded "well-known" Hotline server
 * bookmarks (Hotline Communications / CafeLinux / GtkHx / SiN
 * Grafix). idx is 1..4 — same numbering the connect dialog's
 * builtin combo uses. */
extern void connect_open_builtin_bookmark (int idx);

#endif
