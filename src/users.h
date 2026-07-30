#ifndef HX_USERS_H
#define HX_USERS_H

#include "session.h"

extern GtkWidget *msgbtn;
extern GtkWidget *kickbtn;
extern GtkWidget *infobtn;
extern GtkWidget *banbtn;
extern GtkWidget *chatbtn;
extern GtkWidget *ignobtn;

extern GdkRGBA user_colors[8];
extern GdkRGBA gdk_user_colors[4];

/* Resolve a user's status colour to the GdkRGBA the user list uses
 * for that row's foreground. Returns NULL for the regular-user slot
 * so callers can fall through to the GTK theme's default colour
 * (hard-coding black would be invisible on dark themes). */
extern GdkRGBA *user_color_gdk (guint16 color);

/* Colored-Nicknames foreground: prefers the RGB nick_color (0x00RRGGBB)
 * when set, else falls back to user_color_gdk's status palette
 * (Admin/Guest/Away). `status` is the user's 2-bit status field passed
 * explicitly so the caller renders the freshly-parsed wire status. `out`
 * is a caller-provided GdkRGBA. Returns out (filled, possibly idle-
 * dimmed), a static status-slot GdkRGBA, or NULL for the regular-user
 * slot — callers then fall through to the GTK theme default, which reads
 * correctly under both light and dark themes. */
extern GdkRGBA *user_nick_color_rgb (guint32 nick_color, guint16 status,
                                     GdkRGBA *out);

extern PangoFontDescription *users_font_desc;

extern void create_users_window (GtkWidget *toolbar_window, gpointer data);
extern void user_list (session *sess);
extern void user_create (struct htlc_conn *htlc, struct chat *chat, guint16 uid,
                         guint32 nick_color, const char *nam, guint16 icon,
                         guint16 color);
extern void user_delete (struct htlc_conn *htlc, struct chat *chat,
                         guint16 uid);
extern void user_change (struct htlc_conn *htlc, struct chat *chat, guint16 uid,
                         guint32 nick_color, const char *nam, guint16 icon,
                         guint16 color);
/* Refresh every user-list view that shows `uid` so its cell re-reads
 * the GIF avatar cache (Phase 10.B). Called from the gif-icon-data
 * handler after the avatar cache is updated. */
extern void users_refresh_avatar (guint16 uid);
extern void users_clear (struct htlc_conn *htlc, struct chat *chat);
/* Right-click context menu for a user, used by the GtkGestureClick
 * controller inside HxUserListView. Both the standalone Users
 * window and the pchat sidebars share this single popover builder.
 *
 * `anchor` is the widget the popover gets parented to (and where
 * pointing-to coords are taken from); `x`/`y` are widget-local. */
/* Install (once) the CSS for bare-popover menu items: padding plus a
 * legible :hover background. Add the "gtkhx-popup-item" class to a
 * button to pick it up. Shared with the chat view's context menu. */
extern void hx_popup_item_install_css (void);

extern void user_popup_show (GtkWidget *anchor, session *sess, guint32 cid,
                             guint16 uid, double x, double y);

/* Shared headerbar / sidebar button handlers. `data` is the
 * HxUserListView* the button is attached to — selection + session
 * are pulled off the view. Drive both the Users window's headerbar
 * (users.c::create_users_window) and the pchat sidebars
 * (chat.c::create_pchat_window). */
extern void view_msg_btn (GtkWidget *widget, gpointer data);
extern void view_info_btn (GtkWidget *widget, gpointer data);
extern void view_kick_btn (GtkWidget *widget, gpointer data);
extern void view_igno_btn (GtkWidget *widget, gpointer data);
extern void view_ban_btn (GtkWidget *widget, gpointer data);
extern void view_chat_btn (GtkWidget *widget, gpointer data);

extern void hx_change_name_icon (struct htlc_conn *htlc);
extern void hx_get_user_info (struct htlc_conn *htlc, guint16 uid);
extern void hx_kick_user (struct htlc_conn *htlc, guint16 uid, guint16 ban);

#endif
