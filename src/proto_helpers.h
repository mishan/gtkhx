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
#include <glib-object.h> /* HxChatEvent is a G_DEFINE_BOXED_TYPE */
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
extern gboolean task_error_extract (struct htlc_conn *htlc, char *out,
                                    gsize out_size, gsize *out_len);

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
    HX_SELFINFO_ACCESS = 1u << 0,
    HX_SELFINFO_USER_LIST = 1u << 1,
    /* Colored-Nicknames extension. The optional HTLS_DATA
     * _COLOR (0x0500) chunk on SELFINFO carries the server's record
     * of our own RGB nick color — either the per-account override
     * from server config, or the default the server assigned us at
     * login. Caller mirrors it onto htlc->nick_color so subsequent
     * USER_CHANGE pushes can preserve / override it. */
    HX_SELFINFO_NICK_COLOR = 1u << 2,
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
    char buf[8192 + 1]; /* NUL-terminated */
    char *text;         /* points into buf */
    guint16 text_len;   /* bytes (no NUL), matches strlen(text) */
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
    char name[128 + 1];
    guint16 name_len;
    char msg[8192 + 1];
    guint16 msg_len;
};

extern gboolean hx_msg_extract (struct htlc_conn *htlc, struct hx_msg_msg *out);

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
    char type[5];
    gboolean has_url;
    char url[1024 + 1];
    guint16 url_len;
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
extern void hlpack (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc,
                    va_list ap);

/*
 * Chunk-array variant of hlpack.
 *
 * Same wire format and same effect on htlc->out as hlpack, but the
 * chunks come from a caller-built array rather than a va_list. This
 * lets shared message builders (e.g. login_packet.c::hx_login_pack)
 * assemble their chunks programmatically and hand them to a single
 * packer — no need for each builder to wrap its own variadic
 * dispatch.
 *
 * The struct hx_chunk type is a thin (type, len, data) triple; the
 * caller owns the backing storage for the data pointers (they must
 * outlive the hlpack_chunks call, which copies bytes into
 * htlc->out). hc is the number of chunks in the array.
 *
 * No fd write, no cipher / compression, no proto_trace logging —
 * those layers stay in hlwrite() and the harness's
 * integration_send_chunks() wrapper.
 */
struct hx_chunk {
    guint16 type;
    guint16 len;
    const void *data;
};

extern void hlpack_chunks (struct htlc_conn *htlc, guint32 type, guint32 flag,
                           const struct hx_chunk *chunks, int hc);

/*
 * Decode the 22-byte Hotline message header into host-order fields.
 *
 * `hdr_bytes` points at a buffer of at least SIZEOF_HL_HDR bytes
 * already read from the wire (production's hx_rcv_hdr / the test
 * harness's integration_recv_message both call this with the just-
 * read header). Fills any non-NULL out-parameter with the host-
 * order field and returns the raw wire `len` field (host order).
 *
 *   wire_len_out:  the unchanged host-order `len` field, used by
 *                  proto_trace_recv_hdr so the trace shows the
 *                  server's claimed length even on oversize input.
 *
 *   body_len_out:  the number of payload bytes after the 22-byte
 *                  header — i.e., the count the caller still needs
 *                  to read off the fd. Computed as
 *                  min(wire_len, MAX_HOTLINE_PACKET_LEN) - 2
 *                  (the wire `len` encodes "body bytes plus the 2-
 *                  byte hc field"; hc lives at the tail of the
 *                  22-byte hdr struct but counts as data section
 *                  per the protocol). Production uses this verbatim;
 *                  the harness compares wire_len_out against
 *                  MAX_HOTLINE_PACKET_LEN and rejects oversize input
 *                  entirely.
 *
 * Returns FALSE only for NULL input. (Bounds enforcement is per-
 * caller: production clamps and continues, the harness rejects.)
 *
 * Pre-refactor, this math was implemented twice — once in
 * src/rcv.c::hx_rcv_hdr (production) and once in the integration
 * harness's integration_recv_message. The two formulas were
 * equivalent but written differently; centralising the decode
 * here prevents drift.
 */
extern gboolean hl_hdr_decode (const void *hdr_bytes,
                               guint32 *type_out,
                               guint32 *trans_out,
                               guint32 *flag_out,
                               guint16 *hc_out,
                               guint32 *wire_len_out,
                               guint32 *body_len_out);

/*
 * Decode the variable-width unsigned big-endian integer the
 * HTLS_DATA_CAPABILITIES chunk carries. The spec says the field is
 * "typically 2 bytes, extensible to 8" — chunks of width 1..8 are
 * decoded into the host-order u64 return value with the leading byte
 * weighing most. Excess bytes past 8 are silently ignored (leading
 * bits of a hypothetical >64-bit advertisement would already be in
 * the lower 64 we kept). Width 0 returns 0.
 *
 * Used by both src/rcv.c::rcv_task_login (production echo decode)
 * and tests/integration/integration_harness.c::integration_drain_
 * until_selfinfo_or_error (opportunistic stash). Pre-refactor, the
 * two had identical for-loop copies that would have to be touched
 * in two places if the wire encoding ever grew (e.g. variable-
 * length-encoded > 8 bytes per a future spec revision).
 */
extern guint64 hl_capabilities_decode (const guint8 *bytes, guint16 len);

/*
 * Pack the 16-byte HTXF subchannel handshake header into a caller-
 * provided buffer (must be at least SIZEOF_HTXF_HDR bytes).
 *
 *   ref:    matches the HTXF_REF the main-port TASK reply carried.
 *   len:    total payload size estimate (legacy 32-bit field;
 *           callers in 64-bit mode pass 0 here and append an
 *           explicit 8-byte big-endian size after the 16-byte
 *           header — see network.c::htxf_connect).
 *   type:   HTXF_TYPE_FILE / FOLDER / BANNER — high u16 of the
 *           last field, dispatches Mac-native servers' subchannel
 *           routing.
 *   flags:  low u16 of the last field. HTXF_FLAG_LARGE_FILE,
 *           HTXF_FLAG_SIZE64 per Large-File spec. Pass 0 for the
 *           legacy 16-byte handshake.
 *
 * All fields are big-endian on the wire. Production htxf_connect
 * and banner.c, the integration harness, and the Tier 1 layout
 * test all funnel through this helper — no fork.
 */
extern void hl_htxf_hdr_pack (guint8 *buf, guint32 ref, guint32 len,
                              guint16 type, guint16 flags);

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
    char subject[256]; /* NUL-terminated, max 255 chars */
    guint16 subject_len;
};
extern gboolean hx_chat_subject_extract (struct htlc_conn *htlc,
                                         struct hx_chat_subject_msg *out);

struct hx_chat_invite_msg {
    guint16 uid;
    guint32 cid;
    char name[32]; /* NUL-terminated, max 31 chars */
    guint16 name_len;
};
extern gboolean hx_chat_invite_extract (struct htlc_conn *htlc,
                                        struct hx_chat_invite_msg *out);

/*
 * Result of parsing a HTLS_HDR_USER_CHANGE message.
 *
 * The handler walks six chunks:
 *   HTLS_DATA_UID     — affected user's UID
 *   HTLS_DATA_ICON    — new icon
 *   HTLS_DATA_NAME    — new display name (max 31, strip_ansi-sanitised,
 *                       NUL-terminated)
 *   HTLS_DATA_COLOUR  — new status colour (Admin/Guest/Away u16
 *                       bitmap). Optional — set `got_color` only
 *                       when the chunk was present.
 *   HTLS_DATA_COLOR   — Colored-Nicknames extension RGB
 *                       u32 (0x00RRGGBB). Optional — set
 *                       `got_nick_color` only when present. A wire
 *                       value of HX_NICK_COLOR_NONE means "client
 *                       explicitly cleared its color" and should
 *                       be propagated as-is so the renderer drops
 *                       back to the legacy `color` palette.
 *   HTLS_DATA_CHAT_ID — which chat (cid 0 = main chat)
 *
 * Same shape as the other extractors: missing chunks default to zero
 * / "" / FALSE; NULL out returns FALSE.
 */
struct hx_user_change_msg {
    guint16 uid;
    guint16 icon;
    guint16 color;
    gboolean got_color;
    guint32 nick_color;
    gboolean got_nick_color;
    guint32 cid;
    char name[32]; /* NUL-terminated */
    guint16 name_len;
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
 * Result of parsing the HTLS_HDR_TASK reply that follows any
 * transfer-initiating client message: HTLC_HDR_FILE_GET,
 * HTLC_HDR_FILE_PUT, HTLC_HDR_FILE_GETFOLDER, HTLC_HDR_FILE
 * _PUTFOLDER, HTLC_HDR_DOWNLOAD_BANNER.
 *
 *   HTLS_DATA_HTXF_REF   — 32-bit subchannel reference the client
 *                          must echo back in the HTXF preamble
 *                          (always present on a non-error reply).
 *   HTLS_DATA_HTXF_SIZE  — 32-bit total body byte count the server
 *                          will stream. Absent on uploads (the
 *                          client owns the size); 0 if absent.
 *
 * Returns TRUE if the REF chunk was present, FALSE otherwise (in
 * which case the caller has either an error TASK reply or a
 * malformed one — both deserve the same "bail" treatment).
 *
 * Production sites in rcv.c parse the same chunks plus additional
 * context-specific ones (QUEUE / SIZE64 / NFILES / RFLT) inline;
 * this helper exists for the Tier 3 test harness, which uniformly
 * wants just the two fields and was open-coding the same dh_start
 * walker at 7+ sites.
 */
struct hx_htxf_reply {
    guint32 ref;
    guint32 size;
};
extern gboolean hx_htxf_reply_extract (struct htlc_conn *htlc,
                                       struct hx_htxf_reply *out);

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
typedef void (*hx_news_post_cb) (void *user, const char *bytes, gsize len);
extern int hx_news_post_walk (struct htlc_conn *htlc, hx_news_post_cb cb,
                              void *user);

/*
 * Parse one entry from a 1.5 threaded-news directory listing
 * (reply to HTLC_HDR_NEWSDIRLIST).
 *
 * Hotline 1.5 introduced two distinct threaded-news containers:
 *
 *   - news folder:   contains news folders and/or news categories
 *   - news category: contains news posts
 *
 * A directory-listing reply for a folder enumerates that folder's
 * contents — a mix of folder-entries and category-entries. One
 * chunk per entry. The `kind` field on hx_news_dirlist_entry says
 * whether a given parsed chunk is a folder-entry or a
 * category-entry.
 *
 * Two wire chunk types are used to encode an entry. Either chunk
 * type can encode either kind of entry; the distinction is purely
 * in the on-the-wire framing and which fields are present:
 *
 *   HTLC_DATA_NEWSFOLDERITEM (0x0140):
 *     u8     ntype             (1 = folder-entry, else category-entry)
 *     u8[..] name              (rest of chunk body)
 *
 *   HTLC_DATA_CATEGORYITEM   (0x0143) — carries extra per-category
 *                                       sync metadata (GUID + add /
 *                                       delete serial numbers):
 *     u16    ntype             (2 = folder-entry, 3 = category-entry)
 *     u16    count             (number of children; informational)
 *     if category-entry:
 *       u8[16] guid            (6×u16 + u32, opaque identifier)
 *       u32    addsn           (add-serial-number, for incremental sync)
 *       u32    deletesn        (delete-serial-number)
 *     u8     namelen
 *     u8[namelen] name
 *     trailing bytes           (some servers pad; ignored)
 *
 * The chunk-type name "CATEGORYITEM" is a slight misnomer — that
 * encoding carries folder-entries AND category-entries alike; the
 * name reflects that the wire-format addition relative to the
 * NEWSFOLDERITEM encoding is the per-category sync metadata.
 *
 * Servers vary on which chunk type they emit; the client has to
 * accept both. Both parsers normalise their respective wire bytes
 * to the same output contract:
 *
 *   .kind      1 = folder-entry (contains folders / categories)
 *              2 = category-entry (contains posts)
 *   .name[]    NUL-terminated, up to 255 bytes (CATEGORYITEM namelen
 *              is u8 so 255 is the protocol cap; NEWSFOLDERITEM is
 *              truncated at the same cap for symmetry)
 *   .name_len  strlen(name)
 *
 * Both return FALSE on a malformed chunk (truncated header, name
 * overruns dlen, unknown ntype for the CATEGORYITEM encoding).
 * The output struct is left untouched on failure. NULL `out` is a
 * programmer error and returns FALSE.
 */
struct hx_news_dirlist_entry {
    int kind;         /* 1 = folder-entry, 2 = category-entry */
    char name[256];   /* NUL-terminated, max 255 */
    guint16 name_len; /* strlen(name) */
};

/* Parse a HTLC_DATA_NEWSFOLDERITEM (0x0140) chunk body. */
extern gboolean
hx_news_dirlist_parse_folderitem (const guint8 *data, gsize dlen,
                                  struct hx_news_dirlist_entry *out);

/* Parse a HTLC_DATA_CATEGORYITEM (0x0143) chunk body. */
extern gboolean
hx_news_dirlist_parse_categoryitem (const guint8 *data, gsize dlen,
                                    struct hx_news_dirlist_entry *out);

/*
 * Parse an HTLC_DATA_CATLIST (0x0141) chunk into a structured
 * thread list — the reply payload of HTLC_HDR_NEWSCATLIST. This
 * is the 1.5+ threaded-news article listing: per-category, one
 * struct hx_newscat_post per article (a "thread" in mhxd's terms).
 *
 * Wire format (see mhxd's struct hl_news_threadlist_hdr + struct
 * hl_news_thread_hdr in common/hotline.h, and tnews_send_catlist in
 * hxd/tnews.c for the emitter):
 *
 *   --- chunk body, after the 4-byte hl_data_hdr ---
 *   u32  __x0           opaque, ignored
 *   u32  post_count     number of articles that follow
 *   u16  __x1           opaque (mhxd always zeros this; some
 *                       legacy parsers depended on it being zero)
 *
 *   repeat post_count times:
 *     u32   postid       article ID
 *     u16   date.base_year
 *     u16   date.pad
 *     u32   date.seconds
 *     u32   parentid    parent article ID (0 if root)
 *     u32   __flags     opaque, ignored (mhxd's hl_news_thread_hdr.flags)
 *     u16   partcount   number of mime parts
 *     pstring  subject
 *     pstring  sender
 *     repeat partcount times:
 *       pstring   mime_type
 *       u16       size
 *
 * pstrings are length-prefixed (u8 len + bytes; len=0 means an
 * empty string).
 *
 * On success returns TRUE and fills *out with a freshly-allocated
 * posts array; caller owns the result and MUST call
 * hx_newscat_clear (or memset *out to zero) to release it.
 *
 * Returns FALSE if no CATLIST chunk was found OR if the chunk body
 * was malformed (truncated before the threadlist header, post_count
 * overruns dlen, pstring length overruns dlen). On FALSE, *out is
 * left zero-initialised and no allocation is leaked.
 *
 * NULL `out` is a programmer error and returns FALSE.
 *
 * The plain form (HTLC_DATA_NEWSFOLDERITEM / 0x0140) is a different
 * opcode entirely — see hx_dirlist_parse_extended above for that.
 */
struct hx_newscat_part {
    char *mime_type; /* NUL-terminated, g_malloc'd; NULL on empty */
    guint16 size;
};

struct hx_newscat_post {
    guint32 postid;
    guint32 parentid;
    /* Mac classic 8-byte date split the way news15.c consumes it: */
    guint16 date_base_year;
    guint16 date_pad;
    guint32 date_seconds;
    char *subject; /* NUL-terminated, g_malloc'd; NULL on empty */
    char *sender;  /* NUL-terminated, g_malloc'd; NULL on empty */
    guint16 partcount;
    guint16 size_total;            /* sum of parts[].size */
    struct hx_newscat_part *parts; /* g_malloc'd array, length partcount */
};

struct hx_newscat {
    guint32 post_count;
    struct hx_newscat_post *posts; /* g_malloc'd array, length post_count */
};

extern gboolean hx_newscat_parse (struct htlc_conn *htlc,
                                  struct hx_newscat *out);
extern void hx_newscat_clear (struct hx_newscat *r);

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
extern gboolean hx_news_file_extract (struct htlc_conn *htlc, char *out,
                                      gsize out_size, gsize *out_len);

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
                                         gsize *name_offset, gsize *name_len,
                                         gsize *body_offset, gsize *body_len);

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
                                    const char *const *words);

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
/* Optional inline-media metadata attached to a chat event. Only
 * populated when the inbound chat carried both DATA_CHAT_MEDIA_ID
 * and DATA_CHAT_MEDIA_TYPE (per spec, either both or neither —
 * orphan pairs are dropped at the receive site rather than
 * surfaced here). Owned by the parent HxChatEvent; freed alongside
 * it. */
typedef struct {
    guint8 *id;    /* opaque handle bytes (owned) */
    gsize id_len;
    char *mime;    /* canonical MIME (NUL-terminated, owned) */
    gsize mime_len;
    /* Server-advertised hints. value is 0 / *_present is FALSE
	 * when the field was absent on the wire; the placeholder
	 * formatter elides any column whose *_present flag is FALSE
	 * (no literal "unknown" substitution). Per spec, advisory
	 * only; clients MUST NOT trust these as a substitute for
	 * actually decoding the bytes. */
    guint32 width;
    guint32 height;
    guint32 bytes;
    gboolean width_present;
    gboolean height_present;
    gboolean bytes_present;
} HxChatMedia;

typedef struct _HxChatEvent HxChatEvent;
struct _HxChatEvent {
    guint32 cid;
    char *line; /* UTF-8-valid; NUL-terminated; owned */
    gsize line_len;

    gsize sender_off, sender_len;
    gsize body_off, body_len;

    gboolean is_info; /* "[hx]" info-prefix line */
    gboolean is_self; /* sender == own nick */

    /* Inline-media extension (Phase 9.D). NULL when the chat
	 * carried no media chunks. The companion-fields-orphan case
	 * (exactly one of ID / TYPE present) never reaches here —
	 * rcv.c drops those at the receive site per spec. */
    HxChatMedia *media;
};

#define HX_TYPE_CHAT_EVENT (hx_chat_event_get_type ())
extern GType hx_chat_event_get_type (void) G_GNUC_CONST;

/* Build an HxChatEvent from raw wire bytes. `raw` may carry any
 * encoding seen on the wire (Mac Roman, Latin-1, UTF-8); the
 * constructor runs it through gtkhx_text_to_utf8 once. `self_nick`
 * is NULL-safe — passing NULL means is_self always comes back
 * FALSE. Returns a freshly-allocated event the caller owns. */
extern HxChatEvent *hx_chat_event_new (const char *raw, gsize raw_len,
                                       guint32 cid, const char *self_nick);

extern HxChatEvent *hx_chat_event_copy (HxChatEvent *e);
extern void hx_chat_event_free (HxChatEvent *e);

/* Attach inline-media metadata to a chat event. Copies the id +
 * mime bytes into freshly-owned buffers; caller's pointers may
 * be released afterwards. Replaces any previously-attached
 * media. Idempotent on NULL `ev`. Setting `mime` to NULL or
 * `id_len`/`mime_len` to 0 detaches existing media. */
extern void hx_chat_event_attach_media (HxChatEvent *ev,
                                        const guint8 *id, gsize id_len,
                                        const char *mime, gsize mime_len,
                                        guint32 width, gboolean width_present,
                                        guint32 height,
                                        gboolean height_present,
                                        guint32 bytes,
                                        gboolean bytes_present);

/* Format-friendly helper for the placeholder row. Returns a
 * newly-allocated UTF-8 string the caller must g_free. Example
 * output with every field present:
 *
 *   [image · PNG · 800×600 · 121.1 KB · click to view]
 *
 * The trailing " · click to view]" suffix is always present (the
 * placeholder is a clickable affordance — the UX is part of the
 * line, not metadata that gets omitted). The interior columns
 * are conditional:
 *
 *   - When the MIME matches one of the spec-allowlisted types
 *     (image/png, image/jpeg, image/gif), the short label (PNG /
 *     JPEG / GIF) is used. Any other UTF-8-valid MIME is printed
 *     verbatim. UTF-8-invalid MIME bytes are replaced with "?"
 *     defensively (the Rust extractor doesn't UTF-8-validate
 *     CHAT_MEDIA_TYPE; a hostile or buggy server could otherwise
 *     interpolate arbitrary bytes into UI text).
 *   - width/height pair: only printed when BOTH *_present flags
 *     are set on the wire.
 *   - bytes: only printed when bytes_present is set; formatted
 *     short (e.g. "121.1 KB" or "1.2 MB") rather than literal.
 *
 * NULL `m` returns the bare "[image]" sentinel — used by call
 * sites that have signalled media presence but haven't extracted
 * meta yet. */
extern char *hx_chat_media_placeholder_line (const HxChatMedia *m);

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
    guint16 uid;
    char *name;
    gsize name_len;
    char *body;
    gsize body_len;
    gboolean is_self;
    gboolean is_broadcast;
};

#define HX_TYPE_MSG_EVENT (hx_msg_event_get_type ())
extern GType hx_msg_event_get_type (void) G_GNUC_CONST;

/* Build an HxMsgEvent. `name` / `body` may carry any encoding the
 * wire delivered (Mac Roman, Latin-1, UTF-8); they get run through
 * gtkhx_text_to_utf8 once. `self_nick` is NULL-safe. */
extern HxMsgEvent *hx_msg_event_new (guint16 uid, const char *name,
                                     gsize name_len, const char *body,
                                     gsize body_len, const char *self_nick);
extern HxMsgEvent *hx_msg_event_copy (HxMsgEvent *e);
extern void hx_msg_event_free (HxMsgEvent *e);

#endif /* HX_PROTO_HELPERS_H */
