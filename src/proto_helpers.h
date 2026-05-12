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
#include <glib-object.h>     /* HxChatEvent is a G_DEFINE_BOXED_TYPE */
#include <stdarg.h>

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

/*
 * HTLS_HDR_BANNER — extract the banner type (4 bytes) and optional
 * URL from the message. Server protocol shape (per mhxd's
 * rcv_agreementagree):
 *
 *   HTLS_DATA_BANNER_TYPE  — exactly 4 bytes, e.g. "URL ", "JPEG",
 *                            "GIFf", "PICT". Trailing-space padded
 *                            for shorter codes.
 *   HTLS_DATA_BANNER_URL   — optional. Present only when the server
 *                            is configured for URL-mode banners
 *                            (banner.url set). Other modes (file-
 *                            backed JPEG/GIF) omit this and expect
 *                            the client to follow up with
 *                            HTLC_HDR_BANNER_GET.
 *
 * On success returns TRUE and fills `out`:
 *   .type[5]  — NUL-terminated 4-byte type code (always 4 chars).
 *   .url[]    — empty string when URL chunk wasn't present.
 *   .url_len  — strlen(url).
 *   .has_url  — TRUE only when HTLS_DATA_BANNER_URL was present.
 *
 * Returns FALSE if the type chunk is missing or wrong-sized
 * (anything other than exactly 4 bytes — the protocol pins this).
 */
struct hx_banner_msg {
	char     type[5];
	gboolean has_url;
	char     url[1024 + 1];
	guint16  url_len;
};

extern gboolean hx_banner_extract (struct htlc_conn *htlc,
                                   struct hx_banner_msg *out);

/*
 * Pack a single Hotline message (22-byte hl_hdr + `hc` data chunks)
 * into htlc->out.buf at the current end-of-buffer position.
 *
 * Pure protocol-packing logic, broken out of hlwrite() in network.c
 * so the Tier 2 unit tests can exercise the SEND path without the
 * worker-thread / fd / cipher / compress side-effects hlwrite layers
 * on top.
 *
 * The varargs (passed via va_list `ap`) are HC triples of:
 *
 *   guint16 type, guint16 len, guint8 *data
 *
 * one per chunk. `data` is allowed to be NULL when `len == 0`.
 *
 * Side effects:
 *   - htlc->out.buf is g_realloc'd to fit the new message and
 *     populated with the wire bytes (header + chunks).
 *   - htlc->out.len is bumped.
 *   - htlc->trans is incremented (the transaction ID assigned to
 *     this message is the value htlc->trans had on entry).
 *
 * No fd write, no cipher / compression, no proto_trace logging —
 * those layers stay in hlwrite(). Caller must initialise
 * htlc->out (qbuf_set or zeroed via memset) before the first
 * hlpack call.
 */
extern void hlpack (struct htlc_conn *htlc, guint32 type, guint32 flag,
                    int hc, va_list ap);

/*
 * Smaller chunk-walkers, all sharing the shape "extract uid/cid/name
 * from a fixed set of chunks":
 *
 *   hx_user_part_extract:
 *     HTLS_HDR_USER_PART — fills uid + cid only.
 *
 *   hx_chat_subject_extract:
 *     HTLS_HDR_CHAT_SUBJECT — fills cid + subject (max 255, no
 *     CR2LF / strip_ansi; subjects shouldn't have line endings).
 *
 *   hx_chat_invite_extract:
 *     HTLS_HDR_CHAT_INVITE — fills uid + cid + name. Name is
 *     strip_ansi-sanitised, max 31 bytes, NUL-terminated.
 *
 * All three return TRUE unconditionally on a well-formed message;
 * missing chunks default to zero / "". NULL out is a programming
 * error and returns FALSE.
 */

struct hx_user_part_msg {
	guint16 uid;
	guint32 cid;
};
extern gboolean hx_user_part_extract (struct htlc_conn *htlc,
                                      struct hx_user_part_msg *out);

struct hx_chat_subject_msg {
	guint32 cid;
	char    subject[256];   /* NUL-terminated, max 255 chars */
	guint16 subject_len;
};
extern gboolean hx_chat_subject_extract (struct htlc_conn *htlc,
                                         struct hx_chat_subject_msg *out);

struct hx_chat_invite_msg {
	guint16 uid;
	guint32 cid;
	char    name[32];       /* NUL-terminated, max 31 chars */
	guint16 name_len;
};
extern gboolean hx_chat_invite_extract (struct htlc_conn *htlc,
                                        struct hx_chat_invite_msg *out);

/*
 * Result of parsing a HTLS_HDR_USER_CHANGE message.
 *
 * The handler walks five chunks:
 *   HTLS_DATA_UID     — affected user's UID
 *   HTLS_DATA_ICON    — new icon
 *   HTLS_DATA_NAME    — new display name (max 31, strip_ansi-sanitised,
 *                       NUL-terminated)
 *   HTLS_DATA_COLOUR  — new colour. Optional — set `got_color` only
 *                       when the chunk was present, since "no change"
 *                       is the default.
 *   HTLS_DATA_CHAT_ID — which chat (cid 0 = main chat)
 *
 * Same shape as the other extractors: missing chunks default to zero
 * / "" / FALSE; NULL out returns FALSE.
 */
struct hx_user_change_msg {
	guint16  uid;
	guint16  icon;
	guint16  color;
	gboolean got_color;
	guint32  cid;
	char     name[32];        /* NUL-terminated */
	guint16  name_len;
};
extern gboolean hx_user_change_extract (struct htlc_conn *htlc,
                                        struct hx_user_change_msg *out);

/*
 * Result of parsing a HTLS_HDR_XFER_QUEUE message.
 *
 * Two chunks:
 *   HTLS_DATA_HTXF_REF — file-transfer reference (32-bit)
 *   HTLS_DATA_QUEUE    — queue position (32-bit; 0 means "ready,
 *                        you can start the transfer")
 *
 * Both default to 0 if the chunk is missing.
 */
struct hx_xfer_queue_msg {
	guint32 ref;
	guint32 queueid;
};
extern gboolean hx_xfer_queue_extract (struct htlc_conn *htlc,
                                       struct hx_xfer_queue_msg *out);

/*
 * Walk every HTLS_DATA_NEWS chunk in an HTLS_HDR_NEWS_POST message,
 * sanitise each one (CR2LF + strip_ansi), and invoke `cb` with the
 * sanitised bytes. `bytes` is NUL-terminated for caller convenience;
 * `len` is the byte length excluding the NUL.
 *
 * Phase 5 cleanup: hx_rcv_news_post used to maintain a file-scope
 * news_buf accumulator that the caller never read — the accumulator
 * shifted older content right and put the newest chunk at offset 0,
 * but the emit always passed just the new chunk's `_len` bytes
 * through. The accumulator was dead code that also leaked memory
 * (grew on every news post, never freed) and raced against
 * rcv_task_news_file (which reuses news_buf as a wholesale-overwrite
 * scratch). This walker drops the accumulator and just sanitises +
 * emits each chunk to a stack buffer the cb sees as a const view.
 *
 * `cb` may be NULL (the walker still iterates and counts chunks).
 * Returns the number of NEWS chunks seen.
 */
typedef void (*hx_news_post_cb) (void *user, const char *bytes,
                                 gsize len);
extern int hx_news_post_walk (struct htlc_conn *htlc,
                              hx_news_post_cb cb, void *user);

/*
 * Parse a single HTLC_DATA_CATEGORYITEM (0x0143) chunk body — the
 * "extended" form of a 1.5 threaded-news directory entry. The plain
 * form (HTLC_DATA_NEWSFOLDERITEM / 0x0140) carries just `u8 type +
 * name`; the extended form adds a per-category GUID + add/delete
 * serial numbers used for incremental sync. Different 1.5+ servers
 * choose between the two forms.
 *
 * Wire format (chunk body, hl_data_hdr already consumed):
 *
 *   u16 ntype                (2 = bundle/folder, 3 = category)
 *   u16 count                (sub-item count; informational)
 *   if ntype == 3 (category):
 *     u8[16] guid            (6×u16 + u32 — opaque identifier)
 *     u32    addsn           (add-serial-number, for incremental sync)
 *     u32    deletesn        (delete-serial-number)
 *   u8     namelen
 *   u8[namelen] name
 *   trailing bytes           (some servers pad; ignored)
 *
 * On success returns TRUE and fills `out`:
 *   .kind     1 = folder, 2 = category (matches the legacy
 *             folder_item->type contract used downstream).
 *   .name[]   NUL-terminated, up to 255 bytes (the wire namelen is
 *             u8 so 255 is the protocol cap).
 *   .name_len = strlen(name).
 *
 * Returns FALSE on a malformed chunk: truncated header, unknown
 * ntype, or namelen overruns dlen. The output struct is left
 * untouched on failure. NULL `out` is a programmer error and
 * returns FALSE.
 */
struct hx_dirlist_ext_entry {
	int     kind;             /* 1 = folder, 2 = category */
	char    name[256];        /* NUL-terminated, max 255 */
	guint16 name_len;         /* strlen(name) */
};

extern gboolean hx_dirlist_parse_extended (const guint8 *data, gsize dlen,
                                            struct hx_dirlist_ext_entry *out);

/*
 * Extract the body of an HTLS_HDR_AGREEMENT_FILE message.
 *
 * Three outcomes:
 *   HX_AGREEMENT_OK         — HTLS_DATA_AGREEMENT chunk found, copied
 *                             into `out` with CR2LF + strip_ansi
 *                             applied, NUL-terminated. *out_len is
 *                             the byte length excluding the NUL.
 *   HX_AGREEMENT_NONE       — HTLS_DATA_NOAGREEMENT chunk found
 *                             (server has no agreement to display);
 *                             out is left untouched.
 *   HX_AGREEMENT_NOT_FOUND  — neither chunk type was present.
 *
 * The original handler walked the chunk list in order and returned
 * early on NOAGREEMENT. This extractor preserves that order
 * sensitivity: a NOAGREEMENT chunk *before* an AGREEMENT chunk
 * wins (we return _NONE and ignore the later text).
 *
 * `out_size` must be at least 1 if `out` is non-NULL. NULL `out`
 * with HX_AGREEMENT_OK fixture would return HX_AGREEMENT_OK without
 * filling anything, which is useless; pass NULL only when you don't
 * actually need the text.
 */
typedef enum {
	HX_AGREEMENT_OK,
	HX_AGREEMENT_NONE,
	HX_AGREEMENT_NOT_FOUND,
} hx_agreement_result;

extern hx_agreement_result hx_agreement_extract (struct htlc_conn *htlc,
                                                 char *out, gsize out_size,
                                                 gsize *out_len);

/*
 * Extract the body of an HTLS_HDR_NEWS_FILE response (the one-shot
 * news file fetched by HTLC_HDR_NEWS_GETFILE).
 *
 * Same shape as task_error_extract. Returns TRUE iff an
 * HTLS_DATA_NEWS chunk was found and `out` was filled. NUL-
 * terminates on success. Truncates to (out_size - 1) if the
 * message body is larger than the caller's buffer.
 *
 * The original handler used the rcv.c news_buf/news_len scratch
 * globals; this version uses caller-owned storage. The handler at
 * rcv_task_news_file in rcv.c can keep using the scratch globals
 * (which other code paths still reach for) — this extractor exists
 * to make the parsing testable without sharing global state.
 */
extern gboolean hx_news_file_extract (struct htlc_conn *htlc,
                                      char *out, gsize out_size,
                                      gsize *out_len);

/*
 * Split a single chat line into a "name" portion and a "body"
 * portion for HexChat-style indented rendering. Hotline servers
 * format chat messages as
 *
 *     "<padding>name:  body"
 *
 * where padding is some number of leading spaces and the
 * separator is a colon followed by one or more spaces. This
 * function locates the split point.
 *
 * Inputs:
 *   line, line_len   the un-terminated chat-line bytes (one line —
 *                    callers split a multi-line buffer on '\n'
 *                    before calling).
 *
 * Outputs (on TRUE return):
 *   *name_offset     byte index where the name starts inside line
 *   *name_len        byte length of the name
 *   *body_offset     byte index where the body starts inside line
 *   *body_len        byte length of the body (line_len - body_offset)
 *
 * Returns FALSE if no plausible "name: body" split exists — empty
 * name, no colon found, or a name longer than the 31-byte Hotline
 * nick cap (lines like "Subject Changed to: X" or "https://..."
 * pass through unsplit). Callers should fall back to passing the
 * whole line through gtk_xtext_append unchanged.
 *
 * The name length cap is intentional: it lets us reliably skip
 * URLs and other long colon-containing prose that isn't a chat
 * prefix, at the cost of occasionally missing a chat line whose
 * server padded the nick with trailing spaces past 31. The
 * trade-off favours conservative behaviour for non-chat content.
 */
extern gboolean hx_chat_split_nick_body (const char *line, gsize line_len,
                                         gsize *name_offset,
                                         gsize *name_len,
                                         gsize *body_offset,
                                         gsize *body_len);

/*
 * Highlight matcher. Scans `body` for occurrences of any word in
 * `words[]` (NULL-terminated list of NUL-terminated strings) at
 * word boundaries, ASCII case-insensitive.
 *
 * "Word boundary" means: the character before/after the match (or
 * the buffer edge) is not an alphanumeric ASCII character. So
 * `misha` matches in "hello misha!" and "(misha)" but not in
 * "mishap" or "amisha".
 *
 * NULL or empty entries in words[] are skipped. Returns TRUE on
 * the first match; FALSE if no entry was found anywhere in body.
 *
 * Pure ASCII implementation — for non-ASCII nicks the comparison
 * is still byte-level case-insensitive, which is correct for the
 * lowercase-letter-by-byte content normal Hotline nicks use.
 */
extern gboolean hx_highlight_match (const char *body, gsize body_len,
                                    const char * const *words);

/*
 * HxChatEvent — a parsed chat-message value object.
 *
 * The wire side hands us raw bytes from HTLS_HDR_CHAT (already
 * CR-to-LF'd and strip_ansi'd by hx_chat_extract). Several
 * consumers downstream want the same set of derived facts about
 * that line:
 *
 *   - chat.c::output_chat: needs the UTF-8-valid line plus the
 *     sender / body slices to drive xtext's nick column, plus
 *     is_info to suppress highlighting on info lines, plus
 *     is_self to colour the brackets.
 *
 *   - notify.c::gtkhx_notify_chat: wants the sender as a separate
 *     string for the notification title, the body for the
 *     notification preview, plus the is_info / is_self flags to
 *     decide whether to fire at all.
 *
 * Both used to do the parse work themselves on the raw bytes —
 * UTF-8 fix-up, hx_chat_split_nick_body, INFOPREFIX detect, own-
 * nick compare. Now hx_chat_event_new runs that work once at
 * emit time and packages the result. The GtkhxSession::chat
 * signal carries an HxChatEvent * payload (boxed type — copy /
 * free hooks make multi-subscriber refcounting work).
 *
 * `line` is the UTF-8-valid, NUL-terminated rendering of the
 * incoming bytes. sender_off / sender_len and body_off /
 * body_len index into it. sender_len == 0 means the parser
 * didn't find a "Nick: body" pattern (emotes, raw server
 * prose) — consumers should render `line` verbatim with no
 * special handling.
 */
typedef struct _HxChatEvent HxChatEvent;
struct _HxChatEvent {
	guint32  cid;
	char    *line;            /* UTF-8-valid; NUL-terminated; owned */
	gsize    line_len;

	gsize    sender_off, sender_len;
	gsize    body_off,   body_len;

	gboolean is_info;         /* "[hx]" info-prefix line */
	gboolean is_self;         /* sender == own nick */
};

#define HX_TYPE_CHAT_EVENT (hx_chat_event_get_type ())
extern GType        hx_chat_event_get_type (void) G_GNUC_CONST;

/* Build an HxChatEvent from raw wire bytes. `raw` may carry any
 * encoding seen on the wire (Mac Roman, Latin-1, UTF-8); the
 * constructor runs it through gtkhx_text_to_utf8 once. `self_nick`
 * is NULL-safe — passing NULL means is_self always comes back
 * FALSE. Returns a freshly-allocated event the caller owns. */
extern HxChatEvent *hx_chat_event_new  (const char *raw, gsize raw_len,
                                        guint32 cid,
                                        const char *self_nick);

extern HxChatEvent *hx_chat_event_copy (HxChatEvent *e);
extern void         hx_chat_event_free (HxChatEvent *e);

/*
 * HxMsgEvent — a parsed private-message value object.
 *
 * Same architectural move as HxChatEvent, applied to HTLS_HDR_MSG.
 * The wire side gives us uid + name + body as three separate
 * chunks (no formatted-line parse needed); the constructor just
 * UTF-8-sanitises the strings and stamps the is_self / is_broadcast
 * flags so consumers don't redo the work.
 *
 * Consumers:
 *
 *   - msg.c::msg_output: opens a private-message window keyed on
 *     uid, prefixes the body with a coloured "<name>" header, and
 *     hands the line to xtext.
 *
 *   - notify.c::gtkhx_notify_msg: posts a notification titled
 *     "name (private message)" with the body as preview. Skipped
 *     when is_self (you can't usefully self-PM) or when the msg
 *     window for that uid is focused.
 *
 * `name` and `body` are NUL-terminated and owned by the event.
 * is_broadcast is true only when uid == 0; the rcv.c path
 * currently routes those to msg.c::broadcastmsg directly, not
 * through the msg signal, so consumers will see is_broadcast =
 * FALSE in practice — the flag stays in the struct for future
 * uniformity. */
typedef struct _HxMsgEvent HxMsgEvent;
struct _HxMsgEvent {
	guint16  uid;
	char    *name;
	gsize    name_len;
	char    *body;
	gsize    body_len;
	gboolean is_self;
	gboolean is_broadcast;
};

#define HX_TYPE_MSG_EVENT (hx_msg_event_get_type ())
extern GType       hx_msg_event_get_type (void) G_GNUC_CONST;

/* Build an HxMsgEvent. `name` / `body` may carry any encoding the
 * wire delivered (Mac Roman, Latin-1, UTF-8); they get run through
 * gtkhx_text_to_utf8 once. `self_nick` is NULL-safe. */
extern HxMsgEvent *hx_msg_event_new  (guint16 uid,
                                      const char *name, gsize name_len,
                                      const char *body, gsize body_len,
                                      const char *self_nick);
extern HxMsgEvent *hx_msg_event_copy (HxMsgEvent *e);
extern void        hx_msg_event_free (HxMsgEvent *e);

#endif /* HX_PROTO_HELPERS_H */
