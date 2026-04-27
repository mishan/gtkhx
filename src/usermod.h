#ifndef HX_USERMOD_H
#define HX_USERMOD_H

extern void create_useredit_window (char *login, int new);
extern void useredit_open_dialog();


extern void hx_useredit_create(struct htlc_conn *htlc, const char *login, const char *pass, const char *name, const hl_access_bits access);
extern void hx_useredit_delete (struct htlc_conn *htlc, const char *login);
extern void hx_useredit_open (struct htlc_conn *htlc, const char *login, void (*fn)(void *, const char *, const char *, const char *, const hl_access_bits), void *uesp);

#endif
