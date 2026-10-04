#ifndef HX_CHAT_HISTORY_H
#define HX_CHAT_HISTORY_H 1

/*
 * Chat-history extension — packed-binary entry parser + request
 * sender. fogWraith Capabilities-Chat-History.md.
 *
 * Each DATA_HISTORY_ENTRY (0x0F05) chunk in a GET_CHAT_HISTORY
 * reply carries one chat message in packed binary. Layout:
 *
 *   offset 0   uint64  message_id      (server-assigned, monotonic)
 *   offset 8   int64   timestamp       (Unix epoch UTC seconds)
 *   offset 16  uint16  flags           (see HX_HISTORY_FLAG_*)
 *   offset 18  uint16  icon_id         (sender's icon at send time)
 *   offset 20  uint16  nick_len
 *   offset 22  nick    (nick_len bytes, server-transcoded)
 *   offset 22+nick_len  uint16  msg_len
 *   offset 24+nick_len  msg     (msg_len bytes)
 *   ... optional mini-TLV sub-fields follow (skip unknown
 *       sub-types, advance by sub-length, forward-compat)
 *
 * The parser allocates an HxHistoryEntry per chunk. Mini-TLV
 * sub-fields are skipped silently in v1 (no sub-field type IDs
 * are defined in the spec yet, but the parser walks past them
 * cleanly so a future server emitting them doesn't choke us).
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

/* Mask of bits the parser knows about. Future versions add more —
 * receivers MUST ignore unknown bits without erroring. */
#define HX_HISTORY_FLAG_KNOWN_MASK                                             \
    (HX_HISTORY_FLAG_ACTION | HX_HISTORY_FLAG_SERVER_MSG                       \
     | HX_HISTORY_FLAG_DELETED)

/* Channel ID 0 is the canonical public-chat channel; the spec
 * reserves 1+ for future named channels. */
#define HX_HISTORY_CHANNEL_PUBLIC ((guint32)0u)

/* ---- Decoded entry struct -------------------------------------- */

typedef struct {
    guint64 message_id;
    gint64 timestamp; /* Unix epoch UTC seconds */
    guint16 flags;    /* HX_HISTORY_FLAG_* */
    guint16 icon_id;
    /* nick / message are NUL-terminated, owned by the struct.
     * The wire bytes are NOT NUL-terminated; the parser appends
     * a trailing zero for convenience. The server sends them in
     * the negotiated text encoding (UTF-8 if CAP_TEXT_ENCODING
     * is set, Mac Roman otherwise); the parser decodes both to
     * UTF-8 and turns the message's CR line breaks into LF, the
     * way a live chat line is decoded. */
    gchar *nick;
    gsize nick_len;
    gchar *message;
    gsize message_len;
} HxHistoryEntry;

/* Allocate and parse a single packed-binary entry from `data`
 * (`len` bytes). Returns NULL on a malformed entry (too short,
 * lengths exceed buffer, ...). Caller frees via
 * hx_history_entry_free.
 *
 * The minimum well-formed entry is 24 bytes (empty nick, empty
 * msg). Anything shorter returns NULL.
 *
 * Optional mini-TLV sub-fields after the message body are walked
 * but not surfaced — every spec'd sub-field is "future use" as
 * of this draft.
 *
 * NOTE: both functions moved to the Rust gtkhx-core crate
 * (boxed/history.rs) — parse delegates to
 * hxsession::HistoryEntry::parse, free releases the glib
 * buffers. The struct above stays C-visible (chat.c reads its fields)
 * and its layout is pinned by _Static_asserts in chat_history.c. */
extern HxHistoryEntry *hx_history_entry_parse (const guint8 *data, gsize len);

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

/* Caller-owned backing storage for hx_get_chat_history_build_chunks.
 * The struct hx_chunk array it fills points into these fields, so the
 * scratch must outlive the eventual hlpack_chunks call. The Rust shim
 * uses it only as opaque ≥22-byte backing storage; the field layout is
 * retained for the C caller (the harness) that allocates it. */
struct hx_get_chat_history_scratch {
    guint32 channel_be;
    guint64 before_be;
    guint64 after_be;
    guint16 limit_be;
};

struct hx_chunk;

/*
 * Build the HTLC_DATA_* chunk array for a GET_CHAT_HISTORY request
 * (TRAN 700). channel_id is mandatory, before/after/limit are emitted only when
 * non-zero. Returns the chunk count (always <= 4), or 0 on bad args.
 *
 * Moved to the Rust hxproto crate: a C-ABI shim over the native
 * build_get_chat_history_chunks, kept under this historical name for the
 * one remaining C caller — the integration test harness, which packs the
 * chunks via hlpack_chunks and sends them synchronously over its blocking
 * fd. Production sends through hx_chat_history_fetch_* above.
 */
extern int
hx_get_chat_history_build_chunks (guint32 channel_id, guint64 before,
                                  guint64 after, guint16 limit,
                                  struct hx_chunk *chunks, int chunks_cap,
                                  struct hx_get_chat_history_scratch *scratch);

#endif /* HX_CHAT_HISTORY_H */
