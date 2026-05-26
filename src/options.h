#ifndef HX_OPTIONS_H
#define HX_OPTIONS_H

extern void create_options_window (GtkWidget *widget, gpointer data);
extern void init_variables (void);
extern void prefs_read (void);
extern void prefs_write (void);
extern void reinit_gtktexts (session *sess);
/* Set a BOOLEAN cfgvar from outside the Settings window. Looks up the
 * cfgvar by name, writes the new value, fires the cfgvar's change-
 * callback, mirrors the change into the Settings switch row if the
 * Settings window happens to be open, and persists via prefs_write().
 * Used by per-window UI (e.g. the Tracker case-sensitive toggle) so
 * those toggles stay in lockstep with Settings without each window
 * having to know about the GRegex / prefs_write plumbing. */
extern void gtkhx_prefs_set_bool (const char *name, int value);
extern time_t start_time, total_time;

#endif
