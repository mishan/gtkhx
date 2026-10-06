#ifndef HX_CHAT_HISTORY_H
#define HX_CHAT_HISTORY_H 1

/*
 * Chat-history extension (fogWraith Capabilities-Chat-History.md):
 * the decoded entry the chat view renders, and the request senders.
 * The session (hxsession) parses each DATA_HISTORY_ENTRY (0x0F05)
 * chunk; hxhandlers turns its entries into HxHistoryEntry values.
 *
 * The file is named `chat_history` rather than just `history` so
 * it doesn't collide with the readline-style history library that
 * lives in src/history.c (chat input-line recall, used by the
 * chat / PM input GtkTextView).
 */

#include <glib.h>

struct htlc_conn;

/* ---- Flag bits (uint16, big-endian on the wire) ----------------- */

/* /me emote-style message ("*** nick does X"). Equivalent to
 * DATA_CHATOPTIONS = 1 on the live TRAN_CHAT_MSG path. */
#define HX_HISTORY_FLAG_ACTION ((guint16)0x0001)

/* Message originated from the server (admin broadcast, server
 * message), not a real user. nick is typically empty or set to
 * the server name. */
#define HX_HISTORY_FLAG_SERVER_MSG ((guint16)0x0002)

/* Tombstone: admin removed this message. message_id + timestamp
 * preserved for cursor stability; nick + msg MAY be empty.
 * Clients render "[message removed]" or similar placeholder. */
#define HX_HISTORY_FLAG_DELETED ((guint16)0x0004)

/* ---- Decoded entry struct -------------------------------------- */

typedef struct {
    guint64 message_id;
    gint64 timestamp; /* Unix epoch UTC seconds */
    guint16 flags;    /* HX_HISTORY_FLAG_* */
    guint16 icon_id;
    /* nick / message are NUL-terminated UTF-8, owned by the struct,
     * decoded from the negotiated text encoding with the message's CR
     * line breaks turned into LF, the way a live chat line is. */
    gchar *nick;
    gsize nick_len;
    gchar *message;
    gsize message_len;
} HxHistoryEntry;

/* Built and freed by the Rust gtkhx-core crate (boxed/history.rs); the
 * struct stays C-visible because chat.c reads its fields, and its layout
 * is pinned by _Static_asserts in chat_history.c. */
extern void hx_history_entry_free (HxHistoryEntry *entry);

/* ---- Requests -------------------------------------------------- */

/* GET_CHAT_HISTORY (TRAN 700), sent only where the server agreed to
 * chat history, its reply handed on by the session as the
 * chat-history-batch signal (hxhandlers send/chat_history.rs). Each
 * returns whether it sent anything.
 *
 * _initial: the public chat's history once the login has settled — the
 * catch-up since the newest line seen after a reconnect to the same
 * server, or else the last chat.history_initial lines.
 * _older: the page of chat `cid` before line `before`. */
extern gboolean hx_chat_history_fetch_initial (struct htlc_conn *htlc);
extern gboolean hx_chat_history_fetch_older (struct htlc_conn *htlc,
                                             guint32 cid, guint64 before);

#endif /* HX_CHAT_HISTORY_H */
