/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 */

/*
 * See proto_helpers.h for the rationale and the contract for each
 * function in here.
 *
 * Pure-GLib + protocol.h. Do NOT include hx.h, gtk/gtk.h, or
 * anything else that would force the unit tests to link the GUI
 * tree — that's the whole point of this translation unit.
 */

#include "config.h"
#include <string.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <glib.h>
#include <glib-object.h>
#include "protocol.h"
#include "hxconn.h"
#include "hotline.h"
#include "proto_helpers.h"
#include "hotline_proto.h"
#include "text_util.h"
#include "debug.h"

gboolean
task_error_extract (struct htlc_conn *htlc, char *out, gsize out_size,
                    gsize *out_len)
{
    if (!out || out_size == 0) {
        return FALSE;
    }

    /* chunk walk + CR2LF + strip_ansi moved to
     * gtkhx_proto_parse_task_error. The crate sentinel SIZE_MAX
     * distinguishes "no TASK_ERROR chunk" from "empty error string". */
    size_t n = gtkhx_proto_parse_task_error (htlc->in.buf, htlc->in.pos,
                                             (uint8_t *) out, out_size);
    if (n == (size_t)-1) {
        return FALSE;
    }
    if (out_len) {
        *out_len = n;
    }
    return TRUE;
}

gboolean
hx_chat_extract (struct htlc_conn *htlc, struct hx_chat_msg *out)
{
    if (!out) {
        return FALSE;
    }

    /* the chunk walk + CR2LF + strip_ansi + leading-LF strip
     * moved to the Rust hotline-proto crate (gtkhx_proto_parse_chat). It
     * writes the full sanitised line into out->buf (NUL-terminated, capped
     * at sizeof(out->buf)-1) and reports where the display text starts. */
    struct gtkhx_proto_chat c;
    if (!gtkhx_proto_parse_chat (htlc->in.buf, htlc->in.pos,
                                 (uint8_t *) out->buf, sizeof (out->buf),
                                 &c)) {
        return FALSE;
    }

    out->cid = c.cid;
    out->uid = c.uid;
    out->text = out->buf + c.text_off;
    out->text_len = c.text_len;

    return TRUE;
}

gboolean
hx_msg_extract (struct htlc_conn *htlc, struct hx_msg_msg *out)
{
    if (!out) {
        return FALSE;
    }

    /* chunk walk moved to gtkhx_proto_parse_msg. */
    struct gtkhx_proto_msg m;
    if (!gtkhx_proto_parse_msg (htlc->in.buf, htlc->in.pos,
                                (uint8_t *) out->name, sizeof (out->name),
                                (uint8_t *) out->msg, sizeof (out->msg),
                                &m)) {
        return FALSE;
    }

    out->uid = m.uid;
    out->name_len = m.name_len;
    out->msg_len = m.msg_len;

    return TRUE;
}

gboolean
hx_banner_extract (struct htlc_conn *htlc, struct hx_banner_msg *out)
{
    if (!out) {
        return FALSE;
    }

    /* chunk walk moved to gtkhx_proto_parse_banner. The type
     * is gated at exactly 4 bytes per mhxd's rcv_agreementagree (always
     * 4 bytes, right-padded with spaces for shorter codes like "URL").
     * The crate zeroes type_code when got_type is false; we still copy
     * those 4 bytes into out->type and NUL-terminate to match the
     * existing C contract. */
    struct gtkhx_proto_banner b;
    bool got_type = gtkhx_proto_parse_banner (htlc->in.buf, htlc->in.pos,
                                              (uint8_t *) out->url,
                                              sizeof (out->url), &b);

    memcpy (out->type, b.type_code, 4);
    out->type[4] = '\0';
    out->has_url = b.has_url ? TRUE : FALSE;
    out->url_len = b.url_len;

    return got_type ? TRUE : FALSE;
}

unsigned
hx_selfinfo_parse (struct htlc_conn *htlc)
{
    /* the chunk walk moved to the Rust hotline-proto crate
     * (gtkhx_proto_parse_selfinfo). The crate enforces the same
     * field-length gates the C code did (ACCESS exactly 8, USER_LIST
     * >= 8 fixed bytes, COLOR exactly 4) and clamps the cached name to
     * 31 bytes. The behavioural nuances this handler grew in Phase 5
     * are preserved on the C side here:
     *
     *   - htlc->uid / icon come from the USER_LIST chunk. This is the
     *     fix for the old self-aliasing HN16(&htlc->uid, &htlc->uid)
     *     bug; the crate reads the wire uid out of the record
     *     explicitly (see gtkhx_selfinfo_uid_bug.md in memory).
     *   - hx_conn_name (htlc) is deliberately NOT overwritten with the server's
     *     cached nick. Local prefs win, to avoid the corrupt-nick
     *     feedback loop documented in hx_rcv_user_selfinfo (server
     *     caches our nick by IP and echoes back whatever a previous
     *     broken client left). We only log the cached bytes under
     *     category 'name' for forensics.
     *   - htlc->nick_color mirrors the server's view; network.c
     *     re-seeds it from prefs and pushes a USER_CHANGE after
     *     AGREEMENTAGREE, so the local value still wins.
     *     HX_NICK_COLOR_NONE passes through verbatim. */
    struct gtkhx_proto_selfinfo si;
    unsigned seen
        = gtkhx_proto_parse_selfinfo (htlc->in.buf, htlc->in.pos, &si);

    if (seen & HX_SELFINFO_ACCESS) {
        hx_conn_set_access (htlc, si.access);
    }
    if (seen & HX_SELFINFO_USER_LIST) {
        hx_conn_set_uid (htlc, si.uid);
        hx_conn_set_icon (htlc, si.icon);
        if (si.cached_name_len) {
            GString *hex = g_string_new (NULL);
            for (gsize i = 0; i < si.cached_name_len; i++) {
                if (i) {
                    g_string_append_c (hex, ' ');
                }
                g_string_append_printf (hex, "%02x",
                                        (unsigned)si.cached_name_ptr[i]);
            }
            debug_log ("name",
                       "SELFINFO USER_LIST cached name ignored "
                       "(nlen=%u hex=[%s]) — local prefs nick wins; "
                       "will push via USER_CHANGE",
                       (unsigned)si.cached_name_len, hex->str);
            g_string_free (hex, TRUE);
        }
    }
    if (seen & HX_SELFINFO_NICK_COLOR) {
        hx_conn_set_nick_color (htlc, si.nick_color);
    }

    return seen;
}

gboolean
hx_user_part_extract (struct htlc_conn *htlc, struct hx_user_part_msg *out)
{
    if (!out) {
        return FALSE;
    }

    /* chunk walk moved to gtkhx_proto_parse_user_part. */
    struct gtkhx_proto_user_part p;
    if (!gtkhx_proto_parse_user_part (htlc->in.buf, htlc->in.pos, &p)) {
        return FALSE;
    }

    out->uid = p.uid;
    out->cid = p.cid;

    return TRUE;
}

gboolean
hx_chat_subject_extract (struct htlc_conn *htlc,
                         struct hx_chat_subject_msg *out)
{
    if (!out) {
        return FALSE;
    }

    /* chunk walk moved to gtkhx_proto_parse_chat_subject.
     * Subjects are NOT CR2LF'd / strip_ansi'd (they carry no line
     * endings); the crate preserves that. */
    struct gtkhx_proto_chat_subject sub;
    if (!gtkhx_proto_parse_chat_subject (htlc->in.buf, htlc->in.pos,
                                         (uint8_t *) out->subject,
                                         sizeof (out->subject), &sub)) {
        return FALSE;
    }

    out->cid = sub.cid;
    out->subject_len = sub.subject_len;

    return TRUE;
}

gboolean
hx_chat_invite_extract (struct htlc_conn *htlc, struct hx_chat_invite_msg *out)
{
    if (!out) {
        return FALSE;
    }

    /* chunk walk moved to gtkhx_proto_parse_chat_invite, which
     * strip_ansi's the inviter name (no CR2LF) and caps it at
     * sizeof(out->name)-1. */
    struct gtkhx_proto_chat_invite inv;
    if (!gtkhx_proto_parse_chat_invite (htlc->in.buf, htlc->in.pos,
                                        (uint8_t *) out->name,
                                        sizeof (out->name), &inv)) {
        return FALSE;
    }

    out->uid = inv.uid;
    out->cid = inv.cid;
    out->name_len = inv.name_len;

    return TRUE;
}

gboolean
hx_user_change_extract (struct htlc_conn *htlc, struct hx_user_change_msg *out)
{
    if (!out) {
        return FALSE;
    }

    /* chunk walk moved to gtkhx_proto_parse_user_change. The
     * crate strip_ansi's the nickname (no CR2LF — names carry no line
     * endings) and gates the Colored-Nicknames COLOR chunk at exactly
     * 4 bytes; nick_color defaults to HX_NICK_COLOR_NONE when absent. */
    struct gtkhx_proto_user_change uc;
    if (!gtkhx_proto_parse_user_change (htlc->in.buf, htlc->in.pos,
                                        (uint8_t *) out->name,
                                        sizeof (out->name), &uc)) {
        return FALSE;
    }

    out->uid = uc.uid;
    out->icon = uc.icon;
    out->color = uc.color;
    out->got_color = uc.got_color ? TRUE : FALSE;
    out->nick_color = uc.nick_color;
    out->got_nick_color = uc.got_nick_color ? TRUE : FALSE;
    out->cid = uc.cid;
    out->name_len = uc.name_len;

    return TRUE;
}

/* hx_user_change_plan_resolve moved to Rust (hotline-proto's user_change
 * module). These pins guard the #[repr(C)] mirrors it reads/writes against
 * silent drift of the C structs here + in proto_helpers.h. */
_Static_assert (sizeof (struct hx_user_change_msg) == 60,
                "hx_user_change_msg layout drifted from the Rust mirror");
_Static_assert (offsetof (struct hx_user_change_msg, got_color) == 8, "");
_Static_assert (offsetof (struct hx_user_change_msg, nick_color) == 12, "");
_Static_assert (offsetof (struct hx_user_change_msg, got_nick_color) == 16, "");
_Static_assert (offsetof (struct hx_user_change_msg, cid) == 20, "");
_Static_assert (offsetof (struct hx_user_change_msg, name) == 24, "");
_Static_assert (offsetof (struct hx_user_change_msg, name_len) == 56, "");
_Static_assert (sizeof (struct hx_user_change_plan) == 28,
                "hx_user_change_plan layout drifted from the Rust mirror");
_Static_assert (offsetof (struct hx_user_change_plan, eff_color) == 20, "");
_Static_assert (offsetof (struct hx_user_change_plan, eff_nick_color) == 24, "");

gboolean
hx_xfer_queue_extract (struct htlc_conn *htlc, struct hx_xfer_queue_msg *out)
{
    if (!out) {
        return FALSE;
    }

    /* chunk walk moved to gtkhx_proto_parse_xfer_queue. */
    struct gtkhx_proto_xfer_queue q;
    if (!gtkhx_proto_parse_xfer_queue (htlc->in.buf, htlc->in.pos, &q)) {
        return FALSE;
    }

    out->ref = q.htxf_ref;
    out->queueid = q.queueid;

    return TRUE;
}

gboolean
hx_htxf_reply_extract (struct htlc_conn *htlc, struct hx_htxf_reply *out)
{
    if (!out) {
        return FALSE;
    }

    out->ref = 0;
    out->size = 0;

    dh_start (htlc->in.buf, htlc->in.pos)
    {
        switch (_type) {
        case HTLS_DATA_HTXF_REF:
            dh_getint (out->ref);
            break;
        case HTLS_DATA_HTXF_SIZE:
            dh_getint (out->size);
            break;
        }
    }
    dh_end ();

    return out->ref != 0;
}

hx_agreement_result
hx_agreement_extract (struct htlc_conn *htlc, char *out, gsize out_size,
                      gsize *out_len)
{
    /* chunk walk moved to gtkhx_proto_parse_agreement. The
     * Rust crate leaves *out untouched on NONE / MISSING (matching the
     * "untouched" sentinel assertions in tests/proto/test_agreement.c),
     * and only writes *out_len when both out and out_len are non-NULL. */
    uint32_t r = gtkhx_proto_parse_agreement (htlc->in.buf, htlc->in.pos,
                                              (uint8_t *) out, out_size,
                                              out_len);
    switch (r) {
    case GTKHX_PROTO_AGREEMENT_OK:
        return HX_AGREEMENT_OK;
    case GTKHX_PROTO_AGREEMENT_NONE:
        return HX_AGREEMENT_NONE;
    default:
        return HX_AGREEMENT_NOT_FOUND;
    }
}

gboolean
hx_news_file_extract (struct htlc_conn *htlc, char *out, gsize out_size,
                      gsize *out_len)
{
    if (!out || out_size == 0) {
        return FALSE;
    }

    /* chunk walk + sanitise moved to
     * gtkhx_proto_parse_news_file. The SIZE_MAX sentinel return
     * preserves the "leave *out untouched when no NEWS chunk is
     * present" contract tests/proto/test_news_file.c pins. */
    size_t n = gtkhx_proto_parse_news_file (htlc->in.buf, htlc->in.pos,
                                            (uint8_t *) out, out_size);
    if (n == (size_t)-1) {
        return FALSE;
    }
    if (out_len) {
        *out_len = n;
    }
    return TRUE;
}

/* hx_news_dirlist_parse_folderitem / _categoryitem and hx_newscat_parse
 * (the C shims that marshalled the Rust parse results into struct
 * hx_news_dirlist_entry / struct hx_newscat) are gone — the 1.5 news
 * receive path now parses to owned handles (gtkhx_proto_parse_dirlist /
 * _catlist) read directly by hxnews-model. The underlying parsers stay
 * covered by hotline-proto's native cargo tests. */

/* Trampoline: gtkhx_proto_walk_news_post invokes a typedef'd C
 * callback with a uint8_t* buffer; the public hx_news_post_walk
 * promises a (char *bytes, gsize len) callback. Pack the user's
 * callback + state into a small struct, hand the trampoline to Rust,
 * and route the per-chunk emit through it. */
struct hx_news_post_trampoline {
    hx_news_post_cb cb;
    void *user;
};

static void
hx_news_post_emit (void *t, const uint8_t *bytes, size_t len)
{
    struct hx_news_post_trampoline *tr = t;
    if (tr->cb) {
        tr->cb (tr->user, (const char *)bytes, (gsize)len);
    }
}

int
hx_news_post_walk (struct htlc_conn *htlc, hx_news_post_cb cb, void *user)
{
    /* chunk walk + sanitise moved to
     * gtkhx_proto_walk_news_post. Per-chunk buffer ownership is
     * Rust's; the trampoline above adapts the FFI callback signature
     * to the public hx_news_post_cb's (char *, gsize) shape. */
    struct hx_news_post_trampoline tr = { cb, user };
    return (int)gtkhx_proto_walk_news_post (htlc->in.buf, htlc->in.pos,
                                            hx_news_post_emit, &tr);
}

guint8 *
hlpack (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc, va_list ap,
        gsize *out_len)
{
    /* Marshal the varargs (type, len, data triples) into an hx_chunk array
     * and delegate to hlpack_chunks, so BOTH send entry points serialize
     * through the one Rust packer (gtkhx_proto_pack_message) rather than a
     * hand-rolled C loop. Every variadic caller passes a handful of chunks;
     * the 64 cap matches hotline-proto's MAX_PACK_CHUNKS (above which
     * pack_message_size rejects anyway) and guards the stack array. The data
     * arg is consumed even for a zero-length chunk, matching the old walk. */
    struct hx_chunk chunks[64];

    g_return_val_if_fail (htlc != NULL, NULL);
    g_return_val_if_fail (hc >= 0 && hc <= (int) G_N_ELEMENTS (chunks), NULL);

    for (int i = 0; i < hc; i++) {
        chunks[i].type = (guint16) va_arg (ap, int);
        chunks[i].len = (guint16) va_arg (ap, int);
        chunks[i].data = va_arg (ap, const void *);
    }

    return hlpack_chunks (htlc, type, flag, chunks, hc, out_len);
}

void
hl_htxf_hdr_pack (guint8 *buf, guint32 ref, guint32 len, guint16 type,
                  guint16 flags)
{
    /* delegate to the Rust hotline-proto crate. The wire
     * layout (16 bytes, big-endian: magic, ref, len, (type<<16)|flags)
     * is byte-for-byte identical; callers in htxf_subchannel.c and the
     * Tier 3 harness use this as a leaf packer so the FFI signature
     * gains the explicit out_cap (always SIZEOF_HTXF_HDR here, the
     * production callers already size their buffer for it).
     *
     * The legacy C contract returns void and assumes the write
     * succeeded — callers send `buf` over the wire immediately after,
     * with no failure path. The Rust FFI is fallible (rejects NULL out
     * or out_cap < HTXF_HDR_SIZE), so a precondition violation has to
     * be a hard programmer error rather than a silent uninitialised-
     * buffer send.
     *
     * Use g_error rather than g_assert so the check survives release
     * builds — g_assert compiles out under G_DISABLE_ASSERT, which
     * downstream packagers can set without the project realising it,
     * and silently producing uninitialised wire bytes is precisely the
     * failure mode the Copilot review flagged. Same convention as
     * the LOGIN handshake check in rcv.c::rcv_task_login. g_error logs
     * the failure (under G_LOG_LEVEL_ERROR which is always fatal) and
     * aborts; it is unaffected by G_DISABLE_ASSERT. */
    if (buf == NULL) {
        g_error ("hl_htxf_hdr_pack: NULL buf — programmer error");
    }
    bool ok = gtkhx_proto_htxf_hdr_pack (buf, SIZEOF_HTXF_HDR, ref, len,
                                         type, flags);
    if (!ok) {
        g_error ("hl_htxf_hdr_pack: Rust FFI rejected the call — "
                 "out_cap %zu would not fit a %u-byte header; "
                 "programmer error",
                 (size_t) SIZEOF_HTXF_HDR, (unsigned) SIZEOF_HTXF_HDR);
    }
}

guint64
hl_capabilities_decode (const guint8 *bytes, guint16 len)
{
    /* delegate to the Rust hotline-proto crate. The decode
     * rule (1..8 bytes big-endian, MSB-first, truncate beyond 8, empty
     * is 0) is identical; this wrapper just bridges the GLib u16-len
     * signature to the FFI's size_t. */
    return gtkhx_proto_capabilities_decode (bytes, len);
}

gboolean
hl_hdr_decode (const void *hdr_bytes, guint32 *type_out, guint32 *trans_out,
               guint32 *flag_out, guint16 *hc_out, guint32 *wire_len_out,
               guint32 *body_len_out)
{
    /* delegate the full 22-byte decode to the Rust crate.
     * The wire_len → body_len clamp math (cap at MAX_HOTLINE_PACKET_LEN,
     * saturating_sub by sizeof(hc) so wire_len < 2 doesn't underflow)
     * lives in parse::decode_header_full. The C side just redistributes
     * the filled struct to whichever caller-provided pointers are
     * non-NULL. */
    if (!hdr_bytes) {
        return FALSE;
    }
    struct gtkhx_proto_header_decoded d;
    if (!gtkhx_proto_decode_header ((const uint8_t *) hdr_bytes, SIZEOF_HL_HDR,
                                    MAX_HOTLINE_PACKET_LEN, &d)) {
        return FALSE;
    }
    if (type_out) {
        *type_out = d.type_;
    }
    if (trans_out) {
        *trans_out = d.trans;
    }
    if (flag_out) {
        *flag_out = d.flag;
    }
    if (hc_out) {
        *hc_out = d.hc;
    }
    if (wire_len_out) {
        *wire_len_out = d.wire_len;
    }
    if (body_len_out) {
        *body_len_out = d.body_len;
    }
    return TRUE;
}

guint8 *
hlpack_chunks (struct htlc_conn *htlc, guint32 type, guint32 flag,
               const struct hx_chunk *chunks, int hc, gsize *out_len)
{
    /* the inner serialize loop (header byte layout, per-chunk
     * data hdr + payload writes, len/len2 wire-length math) moved to the
     * Rust hotline-proto crate (build::pack_message). The C side packs the
     * one message into a fresh block and hands ownership back — there is no
     * per-connection send buffer; the only lingering connection-lifecycle
     * side effect is the trans post-increment.
     *
     * Public-API guardrails: this function is the entry point for
     * every shared chunk-array builder (login_packet, chat_history,
     * future modules) plus the integration harness, so a NULL htlc or
     * a chunks=NULL+hc>0 caller is a programming bug we want loud —
     * not a silent NULL deref. The per-chunk NULL-data + len>0
     * validation moved to Rust along with the serialize loop
     * (build::pack_message rejects the malformed chunk and returns 0);
     * the size+pack mismatch g_error below fires on that path too, so
     * the failure mode still surfaces loudly. */
    g_return_val_if_fail (htlc != NULL, NULL);
    g_return_val_if_fail (hc >= 0, NULL);
    g_return_val_if_fail (hc == 0 || chunks != NULL, NULL);

    gsize needed = gtkhx_proto_pack_message_size (chunks, (size_t) hc);
    if (needed == 0) {
        /* pack_message_size returns 0 on hc > MAX_PACK_CHUNKS (currently
         * 64 — well above the largest production builder), on NULL chunks
         * with hc > 0 (caller-side bug we already guarded above), or on a
         * chunks_len that overflows the slice-byte ceiling. All three are
         * programmer errors; the in-tree caller paths can't legitimately
         * trip any of them. */
        g_error ("hlpack_chunks: pack_message_size rejected hc=%d "
                 "(hc above MAX_PACK_CHUNKS, NULL chunks, or "
                 "pathological size overflow)",
                 hc);
    }

    guint8 *buf = g_malloc (needed);

    guint32 my_trans = hx_conn_trans_post_inc (htlc);

    size_t written = gtkhx_proto_pack_message (buf, needed, type, my_trans,
                                               flag, chunks, (size_t) hc);
    if (written != needed) {
        /* pack_message returns 0 on: hc > MAX_PACK_CHUNKS, NULL chunks
         * with hc > 0, any chunk with len > 0 && data == NULL, or
         * out_cap < pack_message_size (we just sized to the size helper,
         * so the last case implies one of the others). */
        g_error ("hlpack_chunks: pack_message wrote %zu bytes, expected %zu "
                 "(programmer error — too many chunks (hc > MAX_PACK_CHUNKS "
                 "= 64), NULL chunks with hc > 0, or NULL data with non-zero "
                 "len in some chunk)",
                 written, (size_t) needed);
    }

    if (out_len) {
        *out_len = needed;
    }
    return buf;
}

/* See doc-comment in proto_helpers.h. */
gboolean
hx_chat_split_nick_body (const char *line, gsize line_len, gsize *name_offset,
                         gsize *name_len, gsize *body_offset, gsize *body_len)
{
    gsize ws, colon, body_start;

    if (!line || line_len == 0) {
        return FALSE;
    }

    /* Strip leading horizontal whitespace. Hotline servers
	 * commonly pad the line with spaces (right-aligning the
	 * nick) before the actual nick text. */
    ws = 0;
    while (ws < line_len && (line[ws] == ' ' || line[ws] == '\t')) {
        ws++;
    }
    if (ws == line_len) {
        return FALSE;
    }

    /* Find the first ':' after the whitespace. The
	 * "nick: body" Hotline format uses a single colon as the
	 * separator. */
    colon = ws;
    while (colon < line_len && line[colon] != ':') {
        colon++;
    }
    if (colon == line_len) {
        return FALSE;
    }
    if (colon == ws) {
        return FALSE; /* empty nick */
    }

    /* Cap nick length at 31 — the Hotline protocol's nick
	 * field is a 31-byte STRING32. Lines whose pre-colon
	 * portion exceeds that are almost certainly not chat
	 * (URLs, "Subject Changed to:", arbitrary system prose);
	 * pass them through unsplit. */
    if (colon - ws > 31) {
        return FALSE;
    }

    /* Skip the spaces between ':' and the body. Conventional
	 * mhxd output pads to two spaces; one-space variants exist;
	 * a body that's all-whitespace is still a valid (empty)
	 * message. */
    body_start = colon + 1;
    while (body_start < line_len && line[body_start] == ' ') {
        body_start++;
    }

    if (name_offset) {
        *name_offset = ws;
    }
    if (name_len) {
        *name_len = colon - ws;
    }
    if (body_offset) {
        *body_offset = body_start;
    }
    if (body_len) {
        *body_len = line_len - body_start;
    }
    return TRUE;
}

/* ASCII-only "is this byte alphanumeric" — used for the word-
 * boundary check below. Locale-agnostic on purpose: nick lookup
 * shouldn't care about the user's LC_CTYPE. */
static gboolean
is_word_byte (guchar c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
           || (c >= 'a' && c <= 'z') || c == '_';
}

/* See doc-comment in proto_helpers.h. */
gboolean
hx_highlight_match (const char *body, gsize body_len, const char *const *words)
{
    if (!body || body_len == 0 || !words) {
        return FALSE;
    }

    for (gsize wi = 0; words[wi] != NULL; wi++) {
        const char *w = words[wi];
        gsize wlen;

        if (!w || !*w) {
            continue;
        }
        wlen = strlen (w);
        if (wlen > body_len) {
            continue;
        }

        /* Walk every possible match start position. */
        for (gsize i = 0; i + wlen <= body_len; i++) {
            /* Before-edge boundary check: position i must
			 * either be at the buffer start, or follow a
			 * non-word byte. */
            if (i > 0 && is_word_byte ((guchar)body[i - 1])) {
                continue;
            }
            /* After-edge boundary check: position i + wlen
			 * must either be at the buffer end, or precede
			 * a non-word byte. */
            if (i + wlen < body_len && is_word_byte ((guchar)body[i + wlen])) {
                continue;
            }

            /* ASCII case-insensitive byte compare. */
            if (g_ascii_strncasecmp (body + i, w, wlen) == 0) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* ---- HxChatEvent --------------------------------------------------- */

/* The "[hx]" info-line prefix. Local copy of the same byte sequence
 * that gtkhx.c::INFOPREFIX exports — proto_helpers must stay free of
 * the GUI tree so we can't reference the gtkhx.c symbol from the Tier
 * 2 unit tests, but the prefix bytes are stable (hx_printf_prefix
 * emits exactly these) so a duplicated constant is acceptable.
 *
 * The full string is " <ETX>10[<ETX>03hx<ETX>10]<ETX> " — mIRC colour
 * 10 around brackets, colour 3 around "hx", trailing reset. */
static const char hx_info_prefix[] = " \00310[\00303hx\00310]\003 ";
#define HX_INFO_PREFIX_LEN (sizeof (hx_info_prefix) - 1)

/* Phase E3: render Slack/Discord-style :shortcodes: as emoji at display
 * time (the inverse of the legacy send-path rewrite in
 * gtkhx_text_for_wire). Returns a newly-allocated, NUL-terminated decoded
 * copy of src[0..len) with *out_len set, or NULL when nothing changed (so
 * the caller can keep the original buffer and skip a copy — the common
 * case for text with no shortcodes).
 *
 * Decode only ever shrinks or keeps length for normal shortcodes, but a
 * short alias mapping to a long ZWJ cluster could in principle grow, so we
 * use the shim's snprintf-style required-length return and a 2nd pass on
 * the rare overflow. */
static char *
hx_decode_emoji_shortcodes (const char *src, gsize len, gsize *out_len)
{
    /* Phase E6: honour the user's emoji-shortcode toggle (the same flag
	 * gates the send encode). Disabled → leave the text verbatim. The flag
	 * lives in text_util.c so this TU stays free of the gtkhx_prefs global
	 * for its unit tests. */
    if (len == 0 || !gtkhx_text_emoji_shortcodes_enabled ()) {
        return NULL;
    }
    gsize cap = len + 16;
    char *dec = g_malloc (cap + 1);
    gsize need = gtkhx_proto_shortcodes_to_emoji ((const uint8_t *) src, len,
                                                  (uint8_t *) dec, cap);
    if (need > cap) {
        dec = g_realloc (dec, need + 1);
        need = gtkhx_proto_shortcodes_to_emoji ((const uint8_t *) src, len,
                                                (uint8_t *) dec, need);
    }
    if (need == len && memcmp (dec, src, len) == 0) {
        g_free (dec); /* unchanged — let the caller keep the original */
        return NULL;
    }
    dec[need] = '\0';
    if (out_len) {
        *out_len = need;
    }
    return dec;
}

HxChatEvent *
hx_chat_event_new (const char *raw, gsize raw_len, guint32 cid,
                   const char *self_nick)
{
    HxChatEvent *e;
    gsize line_len = 0;

    e = g_new0 (HxChatEvent, 1);
    e->cid = cid;

    /* gtkhx_text_to_utf8 always returns a g_strdup-ed copy, even
	 * on empty input — caller owns the result. */
    e->line = gtkhx_text_to_utf8 (raw, raw_len, &line_len);
    e->line_len = line_len;

    /* Phase E3/E6: decode :shortcodes: → emoji across the WHOLE line,
	 * before the info-prefix check and the nick split. Whole-line (not
	 * body-only) is deliberate: some servers format public chat without a
	 * "Nick:" colon (e.g. " *** Name message"), so scoping to the
	 * post-colon "body" would let a shortcode's own colon be mistaken for
	 * the nick separator and skip the conversion. Decoding first is safe
	 * for the nick column because the grammar only matches colon-delimited
	 * lowercase tokens — a "Nick:" prefix (colon on one side only, or
	 * uppercase) never matches — and for info lines, whose mIRC-coloured
	 * "[hx]" prefix carries no shortcodes (the decoder skips colour runs
	 * regardless). The split below then runs on the final decoded text so
	 * the sender/body offsets stay consistent. */
    {
        gsize dlen = 0;
        char *dec = hx_decode_emoji_shortcodes (e->line, e->line_len, &dlen);
        if (dec) {
            g_free (e->line);
            e->line = dec;
            e->line_len = dlen;
        }
    }

    /* Detect the info-prefix branch up front — info lines should
	 * skip both the sender/body split and any highlight matching
	 * downstream. */
    if (e->line_len >= HX_INFO_PREFIX_LEN
        && memcmp (e->line, hx_info_prefix, HX_INFO_PREFIX_LEN) == 0) {
        e->is_info = TRUE;
    }

    if (!e->is_info && e->line_len > 0) {
        gsize so = 0, sl = 0, bo = 0, bl = 0;
        if (hx_chat_split_nick_body (e->line, e->line_len, &so, &sl, &bo,
                                     &bl)) {
            e->sender_off = so;
            e->sender_len = sl;
            e->body_off = bo;
            e->body_len = bl;

            if (self_nick && *self_nick && sl > 0 && strlen (self_nick) == sl
                && memcmp (e->line + so, self_nick, sl) == 0) {
                e->is_self = TRUE;
            }
        }
    }

    return e;
}

/* Phase R4.2c: hx_chat_media_copy moved to Rust (gtkhx-boxed::chat,
 * private media_copy helper) along with the HxChatEvent boxed copy that
 * was its only caller. hx_chat_media_free stays here because
 * hx_chat_event_attach_media (below) still calls it. */

static void
hx_chat_media_free (HxChatMedia *m)
{
    if (!m) {
        return;
    }
    g_free (m->id);
    g_free (m->mime);
    g_free (m);
}

/* Phase R4.2c: hx_chat_event_copy / hx_chat_event_free and the boxed-type
 * registration (hx_chat_event_get_type) moved to Rust —
 * rust/crates/gtkhx-boxed/src/chat.rs. The struct stays C-visible
 * (hx_chat_event_new + hx_chat_event_attach_media fill it; consumers and
 * the placeholder formatters read fields), so the Rust mirrors'
 * #[repr(C)] layouts are pinned against these asserts; bump both sides
 * together if either struct changes shape. */
_Static_assert (sizeof (HxChatEvent) == 72,
                "HxChatEvent layout must match the Rust #[repr(C)] mirror "
                "in gtkhx-boxed::chat");
_Static_assert (sizeof (HxChatMedia) == 56,
                "HxChatMedia layout must match the Rust #[repr(C)] mirror "
                "in gtkhx-boxed::chat");
/* Field offsets too — size alone misses reorderings / padding changes
 * that keep the total. Mirror gtkhx-boxed::chat's offset_of! asserts. */
_Static_assert (G_STRUCT_OFFSET (HxChatEvent, cid) == 0, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatEvent, line) == 8, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatEvent, line_len) == 16, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatEvent, sender_off) == 24, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatEvent, sender_len) == 32, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatEvent, body_off) == 40, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatEvent, body_len) == 48, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatEvent, is_info) == 56, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatEvent, is_self) == 60, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatEvent, media) == 64, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatMedia, id) == 0, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatMedia, id_len) == 8, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatMedia, mime) == 16, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatMedia, mime_len) == 24, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatMedia, width) == 32, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatMedia, height) == 36, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatMedia, bytes) == 40, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatMedia, width_present) == 44, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatMedia, height_present) == 48, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxChatMedia, bytes_present) == 52, "field offset");

void
hx_chat_event_attach_media (HxChatEvent *ev,
                            const guint8 *id, gsize id_len,
                            const char *mime, gsize mime_len,
                            guint32 width, gboolean width_present,
                            guint32 height, gboolean height_present,
                            guint32 bytes, gboolean bytes_present)
{
    if (!ev) {
        return;
    }
    if (ev->media) {
        hx_chat_media_free (ev->media);
        ev->media = NULL;
    }
    if (!id || id_len == 0 || !mime || mime_len == 0) {
        return; /* detach */
    }
    HxChatMedia *m = g_new0 (HxChatMedia, 1);
    m->id_len = id_len;
    m->id = g_malloc (id_len);
    memcpy (m->id, id, id_len);
    m->mime_len = mime_len;
    m->mime = g_strndup (mime, mime_len);
    m->width = width;
    m->width_present = width_present;
    m->height = height;
    m->height_present = height_present;
    m->bytes = bytes;
    m->bytes_present = bytes_present;
    ev->media = m;
}

/* Map a canonical MIME like "image/png" → short label "PNG". The
 * placeholder line uses the short label so the row stays compact
 * in a typical chat width.
 *
 * NULL `mime` returns "?" (the placeholder formatter treats "?"
 * as "omit this column" — so the row reads "[image · ... · click
 * to view]" without a format label).
 *
 * Known allowlisted MIMEs (PNG / JPEG / GIF) return their short
 * literal label.
 *
 * Unknown MIME types are passed through verbatim — but only after
 * a g_utf8_validate check. The Rust extractor doesn't UTF-8-
 * validate CHAT_MEDIA_TYPE (it borrows the wire bytes; UTF-8
 * validation is the responsibility of the C side that
 * interpolates them into UI text). A hostile or buggy server
 * could otherwise emit a CHAT_MEDIA_TYPE chunk with arbitrary
 * bytes — embedded NULs, control characters, partial UTF-8
 * sequences — and have them land verbatim in the chat output via
 * the placeholder line. Collapsing invalid-UTF-8 input to "?"
 * (which the formatter elides) is the safer default. */
static const char *
mime_short_label (const char *mime)
{
    if (!mime) {
        return "?";
    }
    if (g_ascii_strcasecmp (mime, "image/png") == 0) {
        return "PNG";
    }
    if (g_ascii_strcasecmp (mime, "image/jpeg") == 0) {
        return "JPEG";
    }
    if (g_ascii_strcasecmp (mime, "image/gif") == 0) {
        return "GIF";
    }
    /* Unknown MIME — only pass through verbatim if UTF-8-valid.
	 * `mime` is NUL-terminated (hx_chat_event_attach_media
	 * g_strndup'd it), so g_utf8_validate's length=-1 walk
	 * terminates. Pass NULL for the end-of-valid-bytes out param;
	 * we only care about the all-or-nothing verdict. */
    if (!g_utf8_validate (mime, -1, NULL)) {
        return "?";
    }
    return mime;
}

/* Pretty-print a byte count in the spirit of GLib's
 * g_format_size_for_display. Kept local + lossy on purpose — the
 * placeholder doesn't need precise byte counts, just an order-of-
 * magnitude. */
static char *
format_bytes_short (guint32 bytes)
{
    if (bytes < 1024) {
        return g_strdup_printf ("%u B", bytes);
    }
    if (bytes < 1024 * 1024) {
        return g_strdup_printf ("%.1f KB", (double) bytes / 1024.0);
    }
    return g_strdup_printf ("%.1f MB", (double) bytes / (1024.0 * 1024.0));
}

/* Append a piece of text to `out`, replacing every ASCII space
 * with a NBSP (U+00A0 = "\xc2\xa0") so xtext's tokenizer keeps
 * the rendered row as a single clickable word. Other bytes pass
 * through verbatim. */
static void
nbsp_append (GString *out, const char *s)
{
    if (!s) {
        return;
    }
    for (const char *p = s; *p; p++) {
        if (*p == ' ') {
            g_string_append (out, "\xc2\xa0");
        } else {
            g_string_append_c (out, *p);
        }
    }
}

static char *
build_placeholder (const HxChatMedia *m, gboolean nbsp_joined, guint token_id)
{
    if (!m) {
        return g_strdup ("[image]");
    }
    const char *fmt_label = mime_short_label (m->mime);
    GString *out = g_string_new ("[image");
    if (fmt_label && *fmt_label && g_strcmp0 (fmt_label, "?") != 0) {
        if (nbsp_joined) {
            g_string_append (out, "\xc2\xa0\xc2\xb7\xc2\xa0"); /* NBSP·NBSP */
            nbsp_append (out, fmt_label);
        } else {
            g_string_append_printf (out, " · %s", fmt_label);
        }
    }
    if (m->width_present && m->height_present) {
        char dims[64];
        g_snprintf (dims, sizeof (dims), "%u×%u", m->width, m->height);
        if (nbsp_joined) {
            g_string_append (out, "\xc2\xa0\xc2\xb7\xc2\xa0");
            nbsp_append (out, dims);
        } else {
            g_string_append_printf (out, " · %s", dims);
        }
    }
    if (m->bytes_present) {
        char *bytes_str = format_bytes_short (m->bytes);
        if (nbsp_joined) {
            g_string_append (out, "\xc2\xa0\xc2\xb7\xc2\xa0");
            nbsp_append (out, bytes_str);
        } else {
            g_string_append_printf (out, " · %s", bytes_str);
        }
        g_free (bytes_str);
    }
    if (nbsp_joined) {
        /* Embed the click-to-dialog token. The handler scans for
		 * `hxmedia:` and parses the digits. */
        g_string_append_printf (out, "\xc2\xa0\xc2\xb7\xc2\xa0hxmedia:%u",
                                token_id);
        g_string_append (out,
                         "\xc2\xa0\xc2\xb7\xc2\xa0"
                         "click\xc2\xa0to\xc2\xa0view]");
    } else {
        g_string_append (out, " · click to view]");
    }
    return g_string_free (out, FALSE);
}

char *
hx_chat_media_placeholder_line (const HxChatMedia *m)
{
    return build_placeholder (m, /*nbsp_joined=*/FALSE, /*token_id=*/0);
}

char *
hx_chat_media_placeholder_clickable (const HxChatMedia *m, guint token_id)
{
    return build_placeholder (m, /*nbsp_joined=*/TRUE, token_id);
}

gboolean
hx_chat_media_parse_token (const char *word, guint *out_token)
{
    if (!word || !out_token) {
        return FALSE;
    }
    /* Locate the `hxmedia:` substring anywhere in the word. The
	 * placeholder NBSP-joins the row so the entire row arrives at
	 * the click handler as a single token, with `hxmedia:N`
	 * embedded between NBSP punctuation. */
    const char *p = strstr (word, "hxmedia:");
    if (!p) {
        return FALSE;
    }
    p += strlen ("hxmedia:");
    if (!*p || !g_ascii_isdigit (*p)) {
        return FALSE;
    }
    guint64 v = 0;
    while (*p && g_ascii_isdigit (*p)) {
        v = v * 10 + (*p - '0');
        if (v > G_MAXUINT) {
            return FALSE;
        }
        p++;
    }
    *out_token = (guint) v;
    return TRUE;
}

/* HxChatEvent boxed-type registration (hx_chat_event_get_type) moved to
 * Rust in R4.2c — see gtkhx-boxed::chat. */

/* ---- HxMsgEvent ---------------------------------------------------- */

HxMsgEvent *
hx_msg_event_new (guint16 uid, const char *name, gsize name_len,
                  const char *body, gsize body_len, const char *self_nick)
{
    HxMsgEvent *e;
    gsize nlen = 0, blen = 0;

    e = g_new0 (HxMsgEvent, 1);
    e->uid = uid;
    e->is_broadcast = (uid == 0);
    e->name = gtkhx_text_to_utf8 (name, name_len, &nlen);
    e->name_len = nlen;
    e->body = gtkhx_text_to_utf8 (body, body_len, &blen);
    e->body_len = blen;

    /* Phase E3: decode :shortcodes: to emoji in the PM body (the name
	 * stays literal). Standalone buffer, so no offset juggling. */
    {
        gsize dlen = 0;
        char *dec = hx_decode_emoji_shortcodes (e->body, e->body_len, &dlen);
        if (dec) {
            g_free (e->body);
            e->body = dec;
            e->body_len = dlen;
        }
    }

    if (self_nick && *self_nick && nlen > 0 && strlen (self_nick) == nlen
        && memcmp (e->name, self_nick, nlen) == 0) {
        e->is_self = TRUE;
    }

    return e;
}

/* Phase R4.2a: hx_msg_event_copy / hx_msg_event_free and the boxed-type
 * registration (hx_msg_event_get_type) moved to Rust —
 * rust/crates/gtkhx-boxed/src/msg.rs. The struct stays C-visible
 * (hx_msg_event_new above fills it; consumers read fields), so the Rust
 * mirror's #[repr(C)] layout is pinned against this assert; bump both
 * sides together if HxMsgEvent ever changes shape. */
_Static_assert (sizeof (HxMsgEvent) == 48,
                "HxMsgEvent layout must match the Rust #[repr(C)] mirror "
                "in gtkhx-boxed::msg");
/* Field offsets too — size alone misses reorderings / padding changes
 * that keep the total. Mirror gtkhx-boxed::msg's offset_of! asserts. */
_Static_assert (G_STRUCT_OFFSET (HxMsgEvent, uid) == 0, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxMsgEvent, name) == 8, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxMsgEvent, name_len) == 16, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxMsgEvent, body) == 24, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxMsgEvent, body_len) == 32, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxMsgEvent, is_self) == 40, "field offset");
_Static_assert (G_STRUCT_OFFSET (HxMsgEvent, is_broadcast) == 44, "field offset");
