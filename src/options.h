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

/* Typed by-name pref accessors — the bridge the Rust settings form
 * (gtkhx-ui options.rs, Phase R5) reads/writes prefs through. A setter
 * writes the cfgvar's gtkhx_prefs field, fires its changefunc, and
 * persists via prefs_write() — same apply semantics as the old C rows.
 * gtkhx_prefs_type returns the cfgvar type tag (INT/BOOLEAN/STRING/…),
 * 0 for an unknown name. gtkhx_prefs_get_string returns a g_malloc'd
 * copy (free with g_free), never NULL. BOOLEAN writes use the existing
 * gtkhx_prefs_set_bool above. */
extern int gtkhx_prefs_type (const char *name);
extern int gtkhx_prefs_get_bool (const char *name);
extern int gtkhx_prefs_get_int (const char *name);
extern void gtkhx_prefs_set_int (const char *name, int val);
extern char *gtkhx_prefs_get_string (const char *name);
extern void gtkhx_prefs_set_string (const char *name, const char *val);

extern time_t start_time, total_time;

#endif
