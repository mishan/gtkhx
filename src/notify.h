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
#include "proto_helpers.h"          /* HxChatEvent */

/* Stash the GApplication pointer and prime the highlight matcher's
 * once-cached state. Call once from gtkhx_activate after the app
 * is alive. */
extern void gtkhx_notify_init (GtkApplication *app);

/* One entry point per event class. Each consults gtkhx_prefs and
 * the focus state of the relevant window before posting.
 *
 * Chat-side entry points (chat / pchat) take an HxChatEvent so the
 * notification title can be the actual sender's name. The other
 * event classes still take plain strings — they don't have the
 * sender embedded in a chat-line format. */
extern void gtkhx_notify_chat        (HxChatEvent *event);
extern void gtkhx_notify_msg         (const char *sender,
                                       guint16 uid,
                                       const char *body);
extern void gtkhx_notify_pchat       (HxChatEvent *event);
extern void gtkhx_notify_pchat_invite (guint32 cid,
                                        const char *inviter);
extern void gtkhx_notify_news        (const char *headline);
extern void gtkhx_notify_xfer_done   (const char *filename);
extern void gtkhx_notify_broadcast   (const char *text);

#endif /* HX_NOTIFY_H */
