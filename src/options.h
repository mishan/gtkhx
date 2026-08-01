#ifndef HX_OPTIONS_H
#define HX_OPTIONS_H

extern void create_options_window (GtkWidget *widget, gpointer data);
extern void init_variables (void);
/* Load the settings through hxconfig (importing an old gtkhxrc if there is no
 * gtkhx.toml yet), refresh the C mirror, seed the connection's identity, and
 * apply every change hook once. Call after the connection is allocated. */
extern void prefs_read (void);
/* Persist. Writing coalesces on a short timer — hx_prefs_save_soon arms it,
 * hx_prefs_save_now cancels any pending one and writes synchronously, which is
 * what the quit path wants. Both are no-ops when nothing has changed since the
 * last write. */
extern void hx_prefs_save_soon (void);
extern void hx_prefs_save_now (void);
extern void reinit_gtktexts (session *sess);

/* Typed by-name accessors — the one path every preference write takes,
 * whether it comes from a C row here, a Rust Settings page (gtkhx-ui
 * options.rs), or a per-window toggle like the Tracker's case-sensitive
 * button. A write that changes something refreshes the mirror, runs the key's
 * apply hook, and arms the save timer, so apply semantics can't drift between
 * callers.
 *
 * gtkhx_prefs_type returns the value-kind tag (INT / BOOLEAN / STRING /
 * UINT16), 0 for a name the schema doesn't have. gtkhx_prefs_get_string
 * returns a g_malloc'd copy (free with g_free), never NULL.
 *
 * gtkhx_prefs_set_bool additionally drives the live AdwSwitchRow when the
 * Settings dialog happens to be showing one for that key, so a toggle flipped
 * elsewhere stays in lockstep with what the user is looking at. */
extern int gtkhx_prefs_type (const char *name);
extern int gtkhx_prefs_get_bool (const char *name);
extern int gtkhx_prefs_get_int (const char *name);
extern char *gtkhx_prefs_get_string (const char *name);
extern void gtkhx_prefs_set_bool (const char *name, int value);
extern void gtkhx_prefs_set_int (const char *name, int val);
extern void gtkhx_prefs_set_string (const char *name, const char *val);

#endif
