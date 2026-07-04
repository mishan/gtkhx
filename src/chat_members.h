#ifndef HX_CHAT_MEMBERS_H
#define HX_CHAT_MEMBERS_H 1

#include <glib.h>

G_BEGIN_DECLS

/*
 * chat_members.h — C ABI over the Rust HxMemberModel (the authoritative
 * per-chat membership data, gtkhx-ui/src/chat_members.rs) + the M1 nick
 * completion. See the module doc + docs/rust/gchats-model-rethink.md.
 *
 * Ownership: one model per chat, created in chat_new and held on
 * struct chat::member_model. It's a C-owned GObject — hx_member_model_new
 * returns a ref the caller owns (transfer full) and hx_member_model_free
 * drops it. Typed void* here so this header needn't know the Rust GObject
 * type.
 */

extern void *hx_member_model_new (void);
extern void hx_member_model_free (void *model);
extern void hx_member_model_upsert (void *model, guint16 uid, const char *name,
                                    guint16 icon, guint16 status,
                                    guint32 nick_color);
extern void hx_member_model_remove (void *model, guint16 uid);
extern void hx_member_model_clear (void *model);

/* Nick-complete the token at `cursor` (a char offset) in `input` against the
 * model's members. On a result returns TRUE and fills *out_text (a g_malloc'd
 * replacement for the whole buffer), *out_cursor (char offset, or -1 for end),
 * and *out_info (a g_malloc'd space-joined candidate list to echo, or NULL).
 * The caller g_free's *out_text / *out_info. Returns FALSE (outputs untouched)
 * when there's nothing to complete. `suffix` is the nick-completion suffix
 * char (a gunichar; ':' by default). */
extern gboolean hx_nick_complete (void *model, const char *input, gsize cursor,
                                  gboolean reverse, gunichar suffix,
                                  gboolean old_style, char **out_text,
                                  int *out_cursor, char **out_info);

/*
 * Chat input line history — the Rust InputHistory (hxchat-model) replacing the
 * gchat->chat_history GNU-readline HISTORY + chat_history_draft. Owned by C as
 * an opaque pointer. record() = readline add_history + using_history; up/down
 * return a g_malloc'd line to show (caller g_free's) or FALSE for no change.
 */
extern void *hx_input_history_new (void);
extern void hx_input_history_free (void *hist);
extern void hx_input_history_record (void *hist, const char *line);
extern gboolean hx_input_history_up (void *hist, const char *current,
                                     char **out);
extern gboolean hx_input_history_down (void *hist, char **out);

G_END_DECLS

#endif /* HX_CHAT_MEMBERS_H */
