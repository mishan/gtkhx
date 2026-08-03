/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * chat_tabs.h — internal tab strip inside the Chat panel.
 *
 * The Chat panel hosts an AdwTabView whose tabs are: a pinned public-chat tab
 * at position 0; one tab per private chat, keyed on (connection, cid); one tab
 * per private message conversation, keyed on (connection, uid). The connection
 * is half of each key because a cid or a uid is only unique *within* one —
 * two servers can each have a chat at cid 7 — and one tab view serves every
 * connection in turn. This file owns the AdwTabView widget + indices + close
 * routing; chat.c (public chat construction), chat.c (pchat construction) and
 * msg.c (msg construction) call in to add / find / raise / close tabs, each
 * passing the connection its tab belongs to.
 *
 * Why a separate module: the AdwTabView ownership and close-page
 * dispatch is a small piece of policy that's easier to read in
 * isolation than buried inside chat.c's 3 kLOC. Chat.c already has
 * the public-chat machinery (create_chat_window) and pchat
 * machinery (create_pchat_window); msg.c has the PM machinery.
 * chat_tabs.c just hosts the tabs and dispatches.
 *
 * Per-connection thinking (multi-conn, future): the AdwTabView is
 * a singleton today because there's a single Chat panel. When
 * multi-conn lands, each session gets its own Chat panel and its
 * own tab view; the singleton pointer here turns into a per-Chat
 * panel field. The API stays the same.
 */

#ifndef GTKHX_CHAT_TABS_H
#define GTKHX_CHAT_TABS_H 1

#include <gtk/gtk.h>
#include <adwaita.h>

G_BEGIN_DECLS

/* Declared rather than included: every entry point below takes a connection to
 * key on, but none of them looks inside one. Without this the first mention
 * would be inside a parameter list, which C scopes to that declaration alone —
 * a different incomplete type from everyone else's. */
struct htlc_conn;

/* Initialize the tab view. Returns the AdwTabView widget so the
 * Chat panel construction can embed it. Idempotent: subsequent
 * calls return the existing widget. */
GtkWidget *gtkhx_chat_tabs_init (void);

/* Add the pinned public-chat tab. Called once during
 * create_chat_window after the public-chat content widgets are
 * built. The tab is non-closeable and locked to position 0. */
void gtkhx_chat_tabs_add_public (GtkWidget *content, const char *title);

/* Add a private-chat tab for `htlc`'s chat `cid`. The pair is the tab's
 * identity: a cid is only unique within a connection, so two servers can each
 * have a chat at cid 7 and one tab view serves both. Returns the AdwTabPage so
 * the caller can stash extra data or query position. */
AdwTabPage *gtkhx_chat_tabs_add_pchat (struct htlc_conn *htlc,
                                       GtkWidget *content, guint32 cid,
                                       const char *title);

/* Add a private-message tab for the partner at `uid` on `htlc`. Keyed on the
 * pair, for the same reason as the private-chat tab above. */
AdwTabPage *gtkhx_chat_tabs_add_msg (struct htlc_conn *htlc, GtkWidget *content,
                                     guint16 uid, const char *title);

/* Lookup. Return NULL if that connection's cid/uid isn't currently a tab. */
AdwTabPage *gtkhx_chat_tabs_find_pchat (struct htlc_conn *htlc, guint32 cid);
AdwTabPage *gtkhx_chat_tabs_find_msg (struct htlc_conn *htlc, guint16 uid);

/* Make a specific tab the visible one + raise the Chat dock panel
 * if it isn't already focused. Used both on user actions (click
 * the Msg button) and on incoming-message paths (no-op if the
 * user is already in the right tab; otherwise sets attention
 * rather than auto-selecting — see _set_attention_* below for
 * the "subtle" indicator). */
void gtkhx_chat_tabs_raise_pchat (struct htlc_conn *htlc, guint32 cid);
void gtkhx_chat_tabs_raise_msg (struct htlc_conn *htlc, guint16 uid);
void gtkhx_chat_tabs_raise_public (void);

/* Set / clear the needs-attention indicator on a tab. Also flags
 * the Chat dock panel itself via panel_widget_set_needs_attention
 * when the panel isn't the visible dock tab. Selecting the tab
 * clears the indicator automatically. */
void gtkhx_chat_tabs_set_attention_pchat (struct htlc_conn *htlc, guint32 cid,
                                          gboolean state);
void gtkhx_chat_tabs_set_attention_msg (struct htlc_conn *htlc, guint16 uid,
                                        gboolean state);

/* Update the tab's title (e.g. rename or status change). */
void gtkhx_chat_tabs_set_title_pchat (struct htlc_conn *htlc, guint32 cid,
                                      const char *title);
void gtkhx_chat_tabs_set_title_msg (struct htlc_conn *htlc, guint16 uid,
                                    const char *title);

/* Programmatically close a tab. Used when the server tears down a
 * private chat or kicks us out, etc. (NOT the user clicking the
 * tab X — that path runs the close handlers below.) */
void gtkhx_chat_tabs_close_pchat (struct htlc_conn *htlc, guint32 cid);
void gtkhx_chat_tabs_close_msg (struct htlc_conn *htlc, guint16 uid);

/* Close handlers — chat.c (pchat) and msg.c (msg) register these
 * once at startup so the close-page dispatcher in chat_tabs.c can
 * call back into the right teardown logic when the user clicks
 * a tab's X. The handler is responsible for the full teardown:
 *
 *   pchat: send hx_part_chat, then call gchat_delete (which
 *          removes from sess->gchats).
 *   msg:   call msgwin_delete (which removes from
 *          sess->msg_windows).
 *
 * After the handler returns, the AdwTabPage and its content widget
 * are destroyed by AdwTabView. The handler must NOT touch the
 * tab widgets after it returns. */
/* The close dispatchers get the connection the tab belonged to, not just its
 * id: a cid or a uid is only unique within a connection, so a handler given
 * one alone can only ask which connection is *focused* — which is how closing
 * a background server's tab used to look up the wrong window. */
typedef void (*ChatTabsClosePchatFunc) (struct htlc_conn *htlc, guint32 cid);
typedef void (*ChatTabsCloseMsgFunc) (struct htlc_conn *htlc, guint16 uid);

void gtkhx_chat_tabs_set_close_pchat_handler (ChatTabsClosePchatFunc func);
void gtkhx_chat_tabs_set_close_msg_handler (ChatTabsCloseMsgFunc func);

G_END_DECLS

#endif /* GTKHX_CHAT_TABS_H */
