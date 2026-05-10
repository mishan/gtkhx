#ifndef HX_TRACKER_H
#define HX_TRACKER_H

extern void tracker_server_create (struct in_addr addr, guint16 port, guint16 nusers, const char *nam, const char *desc, int total);
extern void create_tracker_window (GtkWidget *widget, gpointer data);
extern void tracker_clear(void);
extern void tracker_kill_threads(void);
/* Re-run the tracker search filter against the cached server tree.
 * No-op when the tracker window isn't currently open. Called from
 * options.c's changed_case() so the visible result list stays in
 * lockstep when the user flips the Case-sensitive switch in Settings
 * while the Tracker is open. */
extern void tracker_search_refresh (void);
#endif
