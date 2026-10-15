#ifndef HX_COMMANDS_H
#define HX_COMMANDS_H

extern void gen_command_hash (void);
extern void hx_command (struct htlc_conn *htlc, char *str, guint32 cid);

#endif /* HX_COMMANDS_H */
