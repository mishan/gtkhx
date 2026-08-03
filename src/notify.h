/*
 * notify.h — desktop-notification dispatch.
 *
 * Builds a GNotification per qualifying event and ships it via
 * g_application_send_notification, which in turn talks to
 * org.freedesktop.Notifications on the session bus (libnotify
 * portal). Each event class has a pref (CFG_NOTIFY_*) and the
 * dispatch consults that plus the global "omit when relevant
 * window is focused" gate.
 *
 * Threading: every entry point here must be called on the main
 * thread. The model→view edge is the GtkhxSession signal
 * dispatchers in gtkhx.c, which already marshal worker output to
 * main, so call sites Just Work.
 */

#ifndef HX_NOTIFY_H
#define HX_NOTIFY_H

#include <gtk/gtk.h>
#include "proto_helpers.h" /* HxChatEvent */

/* Stash the GApplication pointer and prime the highlight matcher's
 * once-cached state. Call once from gtkhx_activate after the app
 * is alive. */
extern void gtkhx_notify_init (GtkApplication *app);

/* One entry point per event class. Each consults gtkhx_prefs and
 * the focus state of the relevant window before posting.
 *
 * news / xfer / broadcast keep a constant id and take no connection: they are
 * genuinely app-level ("something finished downloading"), and collapsing
 * several into one notification is the intended behaviour.
 *
 * Chat-side entry points (chat / pchat) take an HxChatEvent so the
 * notification title can be the actual sender's name. The other
 * event classes still take plain strings — they don't have the
 * sender embedded in a chat-line format. */
/* The four connection-scoped classes take the connection the event arrived
 * on. It is not decoration: a chat id and a uid are only unique *within* a
 * connection, and the notification id is app-global — g_application_send_
 * notification replaces by id, so without a connection dimension server B's
 * message from uid 5 silently replaces server A's. The connection also picks
 * which session's windows the omit-when-focused check consults, and whose
 * nickname counts as a mention. */
extern void gtkhx_notify_chat (struct htlc_conn *htlc, HxChatEvent *event);
extern void gtkhx_notify_msg (struct htlc_conn *htlc, HxMsgEvent *event);
extern void gtkhx_notify_pchat (struct htlc_conn *htlc, HxChatEvent *event);
extern void gtkhx_notify_pchat_invite (struct htlc_conn *htlc, guint32 cid,
                                       const char *inviter);
extern void gtkhx_notify_news (const char *headline);
extern void gtkhx_notify_xfer_done (const char *filename);
extern void gtkhx_notify_broadcast (const char *text);

#endif /* HX_NOTIFY_H */
