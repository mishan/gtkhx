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

/* Stash the GApplication pointer and prime the highlight matcher's
 * once-cached state. Call once from gtkhx_activate after the app
 * is alive. */
extern void gtkhx_notify_init (GtkApplication *app);

/* One entry point per event class. Each consults gtkhx_prefs and
 * the focus state of the relevant window before posting.
 *
 * For the chat / pchat / broadcast paths, `body` is whatever bytes
 * came off the wire — typically a self-contained line like
 * " Sender:  text". We don't try to parse out the sender for the
 * notification title (the formatting varies enough that a robust
 * parse would duplicate chat.c); the title is a generic
 * "Public chat" / "Private chat" / "Server broadcast" and the
 * body carries the original line. The highlight matcher runs on
 * the body, so mentions still trigger correctly. */
extern void gtkhx_notify_chat        (guint32 cid, const char *body);
extern void gtkhx_notify_msg         (const char *sender,
                                       guint16 uid,
                                       const char *body);
extern void gtkhx_notify_pchat       (guint32 cid, const char *body);
extern void gtkhx_notify_pchat_invite (guint32 cid,
                                        const char *inviter);
extern void gtkhx_notify_news        (const char *headline);
extern void gtkhx_notify_xfer_done   (const char *filename);
extern void gtkhx_notify_broadcast   (const char *text);

#endif /* HX_NOTIFY_H */
