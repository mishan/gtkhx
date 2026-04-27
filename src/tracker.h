#ifndef HX_TRACKER_H
#define HX_TRACKER_H

extern void tracker_server_create (struct in_addr addr, guint16 port, guint16 nusers, const char *nam, const char *desc, int total);
extern void create_tracker_window (void);
extern void tracker_clear(void);
extern void tracker_kill_threads(void);
#endif
