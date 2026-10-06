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
 * The original handlers (task_error, etc.) remain in
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
extern gboolean task_error_extract (const guint8 *frame, gsize frame_len,
                                    char *out, gsize out_size, gsize *out_len);

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

extern gboolean hx_banner_extract (const guint8 *frame, gsize frame_len,
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
 * Returns a newly g_malloc'd buffer holding the wire bytes (header +
 * chunks); the caller owns it and frees with g_free. `*out_len` (when
 * non-NULL) receives the byte length. htlc->trans is incremented — the
 * transaction ID assigned to this message is the value htlc->trans had
 * on entry.
 *
 * Pure packing: no transport, no proto_trace logging (that stays in
 * hlwrite()). Cipher / compression are not applied here or in hlwrite —
 * the hxnet orchestrator owns the control-channel transform stack, so
 * hlwrite ships plaintext through hx_bridge_send_frame. There is no
 * per-connection send buffer: the message is packed straight into the
 * returned block, handed to the transport, and freed.
 */
extern guint8 *hlpack (struct htlc_conn *htlc, guint32 type, guint32 flag,
                       int hc, va_list ap, gsize *out_len);

/*
 * Chunk-array variant of hlpack.
 *
 * Same wire format and same return contract as hlpack (a fresh
 * caller-freed buffer + length), but the chunks come from a
 * caller-built array rather than a va_list. This lets shared message
 * builders (e.g. the news / chat senders) assemble their chunks
 * programmatically and hand them to a single packer — no need for each
 * builder to wrap its own variadic dispatch.
 *
 * The struct hx_chunk type is a thin (type, len, data) triple; the
 * caller owns the backing storage for the data pointers (they must
 * outlive the hlpack_chunks call, which copies their bytes into the
 * returned buffer). hc is the number of chunks in the array.
 *
 * No fd write, no cipher / compression, no proto_trace logging — those
 * layers stay in hlwrite() and the harness's integration_send_chunks()
 * wrapper.
 */
struct hx_chunk {
    guint16 type;
    guint16 len;
    const void *data;
};

extern guint8 *hlpack_chunks (struct htlc_conn *htlc, guint32 type,
                              guint32 flag, const struct hx_chunk *chunks,
                              int hc, gsize *out_len);

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
extern gboolean hl_hdr_decode (const void *hdr_bytes, guint32 *type_out,
                               guint32 *trans_out, guint32 *flag_out,
                               guint16 *hc_out, guint32 *wire_len_out,
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
extern gboolean hx_htxf_reply_extract (const guint8 *frame, gsize frame_len,
                                       struct hx_htxf_reply *out);

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

extern hx_agreement_result hx_agreement_extract (const guint8 *frame,
                                                 gsize frame_len, char *out,
                                                 gsize out_size,
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
 * whole line through rotulus_view_append unchanged.
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
 * The session hands over a chat line decoded to UTF-8. Several
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
 * gtkhx-core's chat_event_new does that work — the emoji
 * shortcodes, hx_chat_split_nick_body, the own-nick compare — once
 * at emit time and packages the result. The GtkhxSession::chat
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
    guint8 *id; /* opaque handle bytes (owned) */
    gsize id_len;
    char *mime; /* canonical MIME (NUL-terminated, owned) */
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
    /* Sender's Hotline user id, straight off the wire's UID chunk.
     *
     * 0 means the server didn't send one — parse_chat defaults it, and
     * older servers omit the chunk entirely. It does *not* mean "user
     * zero". The render path falls back to a nick lookup against the
     * membership model in that case; see chat.c::chat_speaker_for.
     *
     * Sits in the padding after `cid`, so adding it moved no other
     * field and the struct is still 72 bytes. */
    guint16 uid;
    char *line; /* UTF-8-valid; NUL-terminated; owned */
    gsize line_len;

    gsize sender_off, sender_len;
    gsize body_off, body_len;

    gboolean is_info; /* never set: a server's line is not ours */
    gboolean is_self; /* sender == own nick */

    /* Inline-media extension (Phase 9.D). NULL when the chat
     * carried no media chunks. The companion-fields-orphan case
     * (exactly one of ID / TYPE present) never reaches here —
     * the session drops the line, per spec. */
    HxChatMedia *media;
};

#define HX_TYPE_CHAT_EVENT (hx_chat_event_get_type ())
extern GType hx_chat_event_get_type (void) G_GNUC_CONST;

extern HxChatEvent *hx_chat_event_copy (HxChatEvent *e);
extern void hx_chat_event_free (HxChatEvent *e);

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
 * MediaTable — the per-chat token → HxChatMedia handle table (M3;
 * gtkhx-core::boxed/src/media_table.rs). Replaces gchat->media_handles
 * (GHashTable) + gchat->media_next_id: output_chat_from_event registers a
 * deep copy of the event's media under a fresh token and embeds it in the
 * placeholder; the word_click handler looks the token back up to pop the
 * dialog. The table owns each copy and frees them on _free. Owned by C as an
 * opaque pointer (typed void* so this header needn't know the Rust type);
 * token 0 is the "absent" sentinel. */
extern void *hx_media_table_new (void);
extern void hx_media_table_free (void *table);
extern guint hx_media_table_register (void *table, const HxChatMedia *src);
extern const HxChatMedia *hx_media_table_lookup (void *table, guint token);

/* Append the picture `media` names to the chat view `view` and fetch it
 * into the row (gtkhx-ui inline_media_row.rs). `table` is the
 * conversation's; NULL keeps one on the view, whose clicks then open the
 * picture. */
struct _GtkWidget;
extern void hx_inline_media_row_append (struct _GtkWidget *view, void *table,
                                        const HxChatMedia *media,
                                        struct htlc_conn *htlc);

/*
 * HxMsgEvent — a parsed private-message value object.
 *
 * Same architectural move as HxChatEvent, applied to HTLS_HDR_MSG.
 * The session gives us uid + name + body already decoded; the Rust
 * constructor (gtkhx-core boxed::msg) stamps the is_self /
 * is_broadcast flags so consumers don't redo the work.
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
 * is_broadcast is true only when uid == 0; broadcasts take the
 * broadcast signal instead, so consumers will see is_broadcast =
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
    HxChatMedia *media; /* NULL when the message carries no picture */
};

#define HX_TYPE_MSG_EVENT (hx_msg_event_get_type ())
extern GType hx_msg_event_get_type (void) G_GNUC_CONST;

extern HxMsgEvent *hx_msg_event_copy (HxMsgEvent *e);
extern void hx_msg_event_free (HxMsgEvent *e);

#endif /* HX_PROTO_HELPERS_H */
