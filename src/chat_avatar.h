/* chat_avatar.h — resolve a user's icon for the chat gutter.
 *
 * One resolver, shared with the user list's rule: a fogWraith GIF avatar
 * wins over the classic 16-bit cicn id, and the cicn sprite is the
 * fallback. Duplicating that precedence would mean chat and Users
 * disagreeing about which icon a user "has". */

#ifndef GTKHX_CHAT_AVATAR_H
#define GTKHX_CHAT_AVATAR_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The texture to draw for `uid`, or NULL when the user has neither an
 * avatar nor a resolvable icon.
 *
 * **Borrowed, and only until the next call.** Animated avatars advance
 * on a shared frame timer, so the answer changes over time — which is
 * why the chat view asks per draw instead of caching a texture. The
 * cicn-derived side *is* cached internally, since decoding a sprite per
 * frame would be silly.
 *
 * `anchor` is any realized widget; load_icon needs one for its theme
 * lookup. */
GdkTexture *hx_chat_avatar_for_uid (GtkWidget *anchor, guint16 uid);

G_END_DECLS

#endif /* GTKHX_CHAT_AVATAR_H */
