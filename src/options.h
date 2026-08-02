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

/* Identity, resolved `override ?? global ?? startup` and copied onto a
 * connection. See the block comment in options.c for what each level answers.
 *
 * hx_identity_set_pending_override arms a one-shot override for the next
 * connect — a bookmark's per-connection nickname and icon. A NULL or empty
 * nick and a negative icon each mean "no override" (zero cannot mean it: zero
 * is a real, blank icon). hx_identity_apply consumes it, so a connect that
 * doesn't arm one gets the global, which is also what puts a /nick-dirtied
 * name back on reconnect.
 *
 * hx_identity_set_startup_default records what the connection was stamped
 * with before any settings file was read, so that reconnect restore works for
 * a profile that has never set a nickname. */
extern void hx_identity_set_pending_override (const char *nick, int icon);
extern void hx_identity_apply (struct htlc_conn *htlc);
extern void hx_identity_set_startup_default (const char *nick);
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
 * gtkhx_prefs_set_bool writes through hxconfig like any other setter; the
 * Settings rows are Rust now and read their value when built, so there is no
 * live widget for it to drive. */
extern int gtkhx_prefs_type (const char *name);
extern int gtkhx_prefs_get_bool (const char *name);
extern int gtkhx_prefs_get_int (const char *name);
extern char *gtkhx_prefs_get_string (const char *name);
extern void gtkhx_prefs_set_bool (const char *name, int value);
extern void gtkhx_prefs_set_int (const char *name, int val);
extern void gtkhx_prefs_set_string (const char *name, const char *val);

#endif
