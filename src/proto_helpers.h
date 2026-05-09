#ifndef HX_PROTO_HELPERS_H
#define HX_PROTO_HELPERS_H 1

/*
 * Pure-protocol-parsing helpers extracted from rcv.c / tasks.c so the
 * Tier 2 unit tests can drive them with canned wire bytes, without
 * dragging in GTK, libadwaita, the toolbar / chat / news / xtext
 * widgets, or the worker-thread plumbing that those source files
 * also bring in.
 *
 * Each helper takes a struct htlc_conn whose `in.buf` / `in.pos` are
 * the only fields it touches — the test sets those up via the
 * wire_fixture builder under tests/proto/.
 *
 * The original handlers (task_error, hx_rcv_chat, etc.) remain in
 * their old translation units; they now call into these helpers and
 * keep doing the GUI side-effects (toast, sound, hx_output dispatch)
 * themselves. The shape of the change is the same as the Tier 1
 * extractions: separate the protocol decision from the side effect.
 */

#include <glib.h>

struct htlc_conn;

/*
 * Find the HTLS_DATA_TASKERROR chunk in htlc->in and copy its body
 * into `out`, sanitising CR→LF and stripping ANSI control bytes
 * along the way. NUL-terminates `out` on success.
 *
 * Returns TRUE iff a task-error chunk was found and out was filled.
 * On TRUE, *out_len (if non-NULL) is the message byte length without
 * the trailing NUL. On FALSE the caller MUST NOT read out.
 *
 * Truncates to (out_size - 1) bytes if the message is too big.
 * out_size must be at least 1.
 */
extern gboolean task_error_extract (struct htlc_conn *htlc,
                                    char *out, gsize out_size,
                                    gsize *out_len);

/*
 * Parse a HTLS_HDR_USER_SELFINFO message from htlc->in into the
 * htlc fields the GUI then reads:
 *
 *   - htlc->access  — 8-byte access bitmap (HTLS_DATA_ACCESS chunk)
 *   - htlc->uid     — our session UID (read from the user-list
 *                     chunk's `uid` field, host-order)
 *   - htlc->icon    — our chat-list icon
 *   - htlc->name    — our display name (NUL-terminated, max 31)
 *
 * Returns the bitwise OR of every chunk type that was actually
 * recognised (e.g. (HX_SELFINFO_ACCESS | HX_SELFINFO_USER_LIST)).
 * Useful for tests asserting "we saw access but not the user-list
 * chunk" against truncated fixtures.
 */
enum {
	HX_SELFINFO_ACCESS    = 1u << 0,
	HX_SELFINFO_USER_LIST = 1u << 1,
};

extern unsigned hx_selfinfo_parse (struct htlc_conn *htlc);

/*
 * Result of parsing a HTLS_HDR_CHAT message. Output of hx_chat_extract.
 *
 * The handler in rcv.c reads HTLS_DATA_CHAT, HTLS_DATA_CHAT_ID, and
 * HTLS_DATA_UID into local variables, then sanitises the chat body:
 *
 *   1. Cap len at (buffer_size - 1) — defensive truncation.
 *   2. CR2LF: '\r' wire-line-endings → '\n'.
 *   3. strip_ansi: low-control bytes → printable.
 *   4. NUL-terminate.
 *   5. If the sanitised body begins with '\n', strip the leading LF
 *      (advances `text` by one and decrements `text_len` by one);
 *      this is what the original handler did to avoid a leading
 *      blank line in the chat widget for the common
 *      "\nUser: message" Hotline format.
 *
 * `text` is a NUL-terminated pointer into the embedded buffer; it
 * may equal &buf[0] or &buf[1] depending on the leading-LF strip.
 * Callers MUST NOT free `text`.
 *
 * Buffer size matches the historical 8 KiB stack buffer used by
 * hx_rcv_chat. Larger payloads are silently truncated.
 */
struct hx_chat_msg {
	guint32 cid;
	guint16 uid;
	char    buf[8192 + 1];   /* NUL-terminated */
	char   *text;            /* points into buf */
	guint16 text_len;        /* bytes (no NUL), matches strlen(text) */
};

/*
 * Walk the chunks in htlc->in, fill *out with the parsed + sanitised
 * chat message. Always returns TRUE on a well-formed wire buffer
 * (the function tolerates missing CHAT_ID / UID — those default to
 * zero — and an empty body is a valid "" message).
 *
 * NULL out is treated as a programmer error and returns FALSE.
 */
extern gboolean hx_chat_extract (struct htlc_conn *htlc,
                                 struct hx_chat_msg *out);

/*
 * Result of parsing a HTLS_HDR_MSG (private message) frame.
 *
 * The handler in rcv.c reads three chunks:
 *   HTLS_DATA_UID   — sender UID (0 = server broadcast)
 *   HTLS_DATA_NAME  — sender display name (max 128 bytes;
 *                     strip_ansi-sanitised)
 *   HTLS_DATA_MSG   — message body (max 8192 bytes; CR→LF +
 *                     strip_ansi-sanitised)
 *
 * Both `name` and `msg` are NUL-terminated. Lengths are bytes
 * (excluding the NUL).
 */
struct hx_msg_msg {
	guint16 uid;
	char    name[128 + 1];
	guint16 name_len;
	char    msg[8192 + 1];
	guint16 msg_len;
};

extern gboolean hx_msg_extract (struct htlc_conn *htlc,
                                struct hx_msg_msg *out);

#endif /* HX_PROTO_HELPERS_H */
