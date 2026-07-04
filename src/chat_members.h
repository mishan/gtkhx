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

/* Client-local ignore flag, authoritative on the model (M4b.4a) — the
 * per-chat member_model owns it, not hx_user::ignore. set/get are no-ops /
 * FALSE for an absent uid; toggle returns the new state (FALSE if absent). */
extern void hx_member_model_set_ignore (void *model, guint16 uid,
                                        gboolean ignore);
extern gboolean hx_member_model_get_ignore (void *model, guint16 uid);
extern gboolean hx_member_model_toggle_ignore (void *model, guint16 uid);

/* Read a member's display fields by uid (M4b.4b-ii) — the model-backed
 * replacement for the old hx_user_with_uid(...)->field reads. `status` is
 * the Admin/Guest/Away bitmap (was hx_user::color); `name` is NUL-terminated.
 * Layout mirrored by #[repr(C)] HxMemberInfo in chat_members.rs. */
struct hx_member_info {
    guint16 uid;
    guint16 icon;
    guint16 status;
    guint32 nick_color;
    char name[32];
};
/* Fill *out with `uid`'s fields; FALSE (out untouched) if absent / NULL. */
extern gboolean hx_member_model_get_info (void *model, guint16 uid,
                                          struct hx_member_info *out);
/* uid of the member named `name` (exact match), or 0 if none. */
extern guint16 hx_member_model_find_by_name (void *model, const char *name);

/* Member count + read-by-insertion-index, for a full walk (e.g. user_list
 * repopulating a freshly-opened view). get_at fills *out; FALSE out-of-range. */
extern guint hx_member_model_count (void *model);
extern gboolean hx_member_model_get_at (void *model, guint index,
                                        struct hx_member_info *out);
/* Whether `uid` is a member (the user_change fan-out's "is this user in this
 * pchat?" gate). */
extern gboolean hx_member_model_contains (void *model, guint16 uid);

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
