#ifndef HX_CHAT_HISTORY_H
#define HX_CHAT_HISTORY_H 1

/*
 * Chat-history extension (fogWraith Capabilities-Chat-History.md):
 * the request senders. The session (hxsession) parses each
 * DATA_HISTORY_ENTRY (0x0F05) chunk, and gtkhx-ui's chat_output
 * draws the entries.
 *
 * The file is named `chat_history` rather than just `history` so
 * it doesn't collide with the readline-style history library that
 * lives in src/history.c (chat input-line recall, used by the
 * chat / PM input GtkTextView).
 */

#include <glib.h>

struct htlc_conn;

/* ---- Requests -------------------------------------------------- */

/* GET_CHAT_HISTORY (TRAN 700), sent only where the server agreed to
 * chat history, its reply handed on by the session as the
 * chat-history-batch signal (hxhandlers send/chat_history.rs). It
 * returns whether it sent anything.
 *
 * _initial: the public chat's history once the login has settled: the
 * last chat.history_initial lines, or the catch-up after the cursor
 * should one survive the connect (none does yet). */
extern gboolean hx_chat_history_fetch_initial (struct htlc_conn *htlc);

#endif /* HX_CHAT_HISTORY_H */
