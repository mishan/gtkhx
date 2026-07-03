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

/* user->uid, or 0 if NULL. A tiny accessor so the Rust HxUserRow
 * (rust/crates/gtkhx-ui/src/user_row.rs) reads the uid without pinning
 * the struct hx_user field layout on its side. */
extern guint16 hx_user_uid (const struct hx_user *user);

/* Colored-Nicknames preference. Prefers user->nick_color
 * (RGB) when set, else falls back to user_color_gdk's status palette
 * (Admin/Guest/Away). `status` is the user's 2-bit status field
 * (idle/admin) passed explicitly rather than read off user->color so
 * the caller in users.c::user_change can render the freshly-parsed
 * status — rcv.c stamps user->color AFTER emitting the signal, so
 * the field would still hold the previous value if read here.
 * `out` is a caller-provided GdkRGBA used to hold the unpacked RGB
 * result.
 *
 * Return: caller-owned pointer that's either &`out` (filled with the
 * unpacked nick_color, possibly idle-dimmed) or the static GdkRGBA
 * for the user's status slot — OR NULL when neither applies (the
 * regular-user slot in user_color_gdk returns NULL so callers fall
 * through to the GTK theme's default foreground, which is what reads
 * correctly under both light and dark themes).
 * The cell renderer treats NULL as "use theme default", so callers
 * can pass the result through unconditionally. */
extern GdkRGBA *user_nick_color_gdk (const struct hx_user *user, guint16 status,
                                     GdkRGBA *out);

extern PangoFontDescription *users_font_desc;

extern void create_users_window (GtkWidget *toolbar_window, gpointer data);
extern void user_list (session *sess);
extern void user_create (struct htlc_conn *htlc, struct chat *chat,
                         struct hx_user *user, const char *nam, guint16 icon,
                         guint16 color);
extern void user_delete (struct htlc_conn *htlc, struct chat *chat,
                         struct hx_user *user);
extern void user_change (struct htlc_conn *htlc, struct chat *chat,
                         struct hx_user *user, const char *nam, guint16 icon,
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
extern void user_popup_show (GtkWidget *anchor, struct hx_user *user,
                             session *sess, double x, double y);

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

extern struct hx_user *hx_user_new (struct chat *chat, guint16 uid);
extern void hx_user_delete (struct chat *chat, struct hx_user *user);
extern struct hx_user *hx_user_with_uid (struct chat *chat, guint16 uid);
extern struct hx_user *hx_user_with_name (struct chat *chat, const char *name);
extern void hx_change_name_icon (struct htlc_conn *htlc);
extern void hx_get_user_info (struct htlc_conn *htlc, guint16 uid);
extern void hx_kick_user (struct htlc_conn *htlc, guint16 uid, guint16 ban);

#endif
