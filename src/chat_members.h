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

G_END_DECLS

#endif /* HX_CHAT_MEMBERS_H */
