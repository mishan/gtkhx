#ifndef HX_USERMOD_H
#define HX_USERMOD_H

/* Ported to Rust (gtkhx-ui, useredit.rs); the C ABI is unchanged so
 * toolbar.c's callers link against the Rust exports. */
extern void create_useredit_window (const char *login, int new);
extern void useredit_open_dialog (void);

/* Access-bit table accessors — read by the Rust User Editor. */
extern int gtkhx_useredit_access_count (void);
extern const char *gtkhx_useredit_access_name (int i);
extern int gtkhx_useredit_access_bitno (int i);

extern void hx_useredit_create (struct htlc_conn *htlc, const char *login,
                                const char *pass, const char *name,
                                const hl_access_bits access);
extern void hx_useredit_delete (struct htlc_conn *htlc, const char *login);
extern void hx_useredit_open (struct htlc_conn *htlc, const char *login,
                              void (*fn) (void *, const char *, const char *,
                                          const char *, const hl_access_bits),
                              void *uesp);

#endif
