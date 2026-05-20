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
    gint64  timestamp;   /* Unix epoch UTC seconds */
    guint16 flags;       /* HX_HISTORY_FLAG_* */
    guint16 icon_id;
    /* nick / message are NUL-terminated, owned by the struct.
	 * The wire bytes are NOT NUL-terminated; the parser appends
	 * a trailing zero for convenience. The server has already
	 * transcoded to whatever the negotiated text encoding is
	 * (UTF-8 if CAP_TEXT_ENCODING is set, Mac Roman otherwise);
	 * callers that need UTF-8 should pass these through
	 * gtkhx_text_to_utf8. */
    gchar  *nick;
    gsize   nick_len;
    gchar  *message;
    gsize   message_len;
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
 * of this draft. */
extern HxHistoryEntry *hx_history_entry_parse (const guint8 *data,
                                               gsize         len);

extern void hx_history_entry_free (HxHistoryEntry *entry);

/* ---- Request sender -------------------------------------------- */

/*
 * Send TRAN 700 (HTLC_HDR_GET_CHAT_HISTORY) for `channel_id`. The
 * three cursor / limit args are optional — pass zero for any of
 * them to omit the chunk on the wire (server treats absence as
 * "default"):
 *
 *   before == 0 && after == 0  → "most recent N messages"
 *   before  > 0                → "messages older than before"
 *   after   > 0                → "messages newer than after"
 *   both    > 0                → "messages in range (after, before)"
 *
 *   limit   == 0  → server picks (typically 50, capped at 200)
 *   limit   > 0   → client request, server MAY cap lower
 *
 * Channel 0 is the public chat. The spec reserves 1+ for future
 * named channels, but no server implements them yet.
 *
 * No-op (returns FALSE without sending) if the session didn't
 * negotiate CAP_CHAT_HISTORY — sending TRAN 700 to a server that
 * doesn't speak the extension earns a task-error toast every
 * time.
 */
extern gboolean hx_get_chat_history (struct htlc_conn *htlc,
                                     guint32 channel_id, guint64 before,
                                     guint64 after, guint16 limit);

/* Caller-owned backing storage for hx_get_chat_history_build_chunks.
 * The struct hx_chunk array it fills points into these fields, so the
 * scratch must outlive the eventual hlpack_chunks call. */
struct hx_get_chat_history_scratch {
    guint32 channel_be;
    guint64 before_be;
    guint64 after_be;
    guint16 limit_be;
};

struct hx_chunk;

/*
 * Build the HTLC_DATA_* chunk array for a GET_CHAT_HISTORY request
 * (TRAN 700). Same "0 means omit" semantics as hx_get_chat_history:
 * channel_id is mandatory, before/after/limit are emitted only when
 * non-zero. Returns the chunk count (always <= 4), or 0 on bad args.
 *
 * Used by both production (via hx_get_chat_history which wraps it
 * with cap-gate + hlwrite_chunks) and the integration test harness
 * (which packs the chunks via hlpack_chunks and sends them
 * synchronously over its blocking fd). Pre-refactor, the two had
 * independent 8-way variadic dispatches that drifted easily.
 */
extern int
hx_get_chat_history_build_chunks (guint32 channel_id, guint64 before,
                                  guint64 after, guint16 limit,
                                  struct hx_chunk *chunks, int chunks_cap,
                                  struct hx_get_chat_history_scratch *scratch);

#endif /* HX_CHAT_HISTORY_H */
