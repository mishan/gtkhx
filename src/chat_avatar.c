/* chat_avatar.c — see chat_avatar.h. */

#include "config.h"

#include <glib.h>
#include <gtk/gtk.h>

#include "hx.h"
#include "chat_avatar.h"
#include "chat.h"         /* chat_with_cid / hx_chat_member_model */
#include "chat_members.h" /* hx_member_model_get_info */
#include "cicn.h"         /* load_icon */
#include "gif_avatar.h"
#include "gtkhx.h"   /* icon_files */
#include "gtkutil.h" /* gtkhx_texture_from_pixbuf */

/* cicn sprites keyed by icon id. Small, bounded by how many distinct
 * icons a server's users pick, and decoding one per frame per visible
 * row would be absurd. Avatars are deliberately NOT cached here — see
 * the header. */
static GHashTable *icon_cache; /* guint icon_id -> GdkTexture* (owned) */

static GdkTexture *
icon_texture (GtkWidget *anchor, guint16 icon_id)
{
    GdkPixbuf *pixbuf = NULL;
    GdkPixbuf *mask_unused = NULL;
    GdkTexture *tex;

    if (icon_id == 0) {
        return NULL;
    }
    if (!icon_cache) {
        icon_cache = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                            g_object_unref);
    }
    tex = g_hash_table_lookup (icon_cache, GUINT_TO_POINTER (icon_id));
    if (tex) {
        return tex;
    }

    load_icon (anchor, icon_id, &icon_files, 1, &pixbuf, &mask_unused);
    if (!pixbuf) {
        return NULL;
    }
    tex = gtkhx_texture_from_pixbuf (pixbuf);
    g_object_unref (pixbuf);
    if (tex) {
        g_hash_table_insert (icon_cache, GUINT_TO_POINTER (icon_id), tex);
    }
    return tex;
}

GdkTexture *
hx_chat_avatar_for_uid (GtkWidget *anchor, guint16 uid)
{
    GdkTexture *avatar;
    struct hx_member_info info;
    struct chat *conv;

    if (uid == 0 || !anchor) {
        return NULL;
    }

    /* Same precedence as users_cell.c: the GIF avatar wins. Re-fetched
     * every call because it advances on a shared frame timer, so a
     * cached texture would freeze the animation. */
    avatar = gtkhx_avatar_get (hx_active_session ()->htlc, uid);
    if (avatar) {
        return avatar;
    }

    /* The icon id comes from the membership model — the same record the
     * user list draws from, so the two cannot show different icons for
     * one person. Public chat holds every connected user, which is where
     * a chat speaker's icon lives regardless of which room they spoke
     * in. */
    conv = chat_with_cid (hx_active_session (), 0);
    if (!conv) {
        return NULL;
    }
    if (!hx_member_model_get_info (hx_chat_member_model (conv), uid, &info)) {
        return NULL;
    }
    return icon_texture (anchor, info.icon);
}
