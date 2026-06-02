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

    /* Phase R2: chunk walk + CR2LF + strip_ansi moved to
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

    /* Phase R2: the chunk walk + CR2LF + strip_ansi + leading-LF strip
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

    /* Phase R2: chunk walk moved to gtkhx_proto_parse_msg. */
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

    /* Phase R2: chunk walk moved to gtkhx_proto_parse_banner. The type
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
    /* Phase R2: the chunk walk moved to the Rust hotline-proto crate
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
     *   - htlc->name is deliberately NOT overwritten with the server's
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
        memcpy (&htlc->access, si.access, 8);
    }
    if (seen & HX_SELFINFO_USER_LIST) {
        htlc->uid = si.uid;
        htlc->icon = si.icon;
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
        htlc->nick_color = si.nick_color;
    }

    return seen;
}

gboolean
hx_user_part_extract (struct htlc_conn *htlc, struct hx_user_part_msg *out)
{
    if (!out) {
        return FALSE;
    }

    /* Phase R2: chunk walk moved to gtkhx_proto_parse_user_part. */
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

    /* Phase R2: chunk walk moved to gtkhx_proto_parse_chat_subject.
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

    /* Phase R2: chunk walk moved to gtkhx_proto_parse_chat_invite, which
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

    /* Phase R2: chunk walk moved to gtkhx_proto_parse_user_change. The
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

gboolean
hx_xfer_queue_extract (struct htlc_conn *htlc, struct hx_xfer_queue_msg *out)
{
    if (!out) {
        return FALSE;
    }

    /* Phase R2: chunk walk moved to gtkhx_proto_parse_xfer_queue. */
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

    dh_start (htlc)
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
    /* Phase R2: chunk walk moved to gtkhx_proto_parse_agreement. The
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

    /* Phase R2: chunk walk + sanitise moved to
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

gboolean
hx_news_dirlist_parse_categoryitem (const guint8 *data, gsize dlen,
                                    struct hx_news_dirlist_entry *out)
{
    if (!out) {
        return FALSE;
    }

    /* Phase R2: byte-level parse moved to
     * gtkhx_proto_parse_news_categoryitem. */
    struct gtkhx_proto_news_dir_entry e;
    if (!gtkhx_proto_parse_news_categoryitem (data, dlen,
                                              (uint8_t *) out->name,
                                              sizeof (out->name), &e)) {
        return FALSE;
    }
    out->kind = e.kind;
    out->name_len = e.name_len;
    return TRUE;
}

gboolean
hx_news_dirlist_parse_folderitem (const guint8 *data, gsize dlen,
                                  struct hx_news_dirlist_entry *out)
{
    if (!out) {
        return FALSE;
    }

    /* Phase R2: byte-level parse moved to
     * gtkhx_proto_parse_news_folderitem. The C extractor preserved a
     * subtle contract — folderitem == 1 ⇒ folder, anything else ⇒
     * category — which the Rust impl mirrors. */
    struct gtkhx_proto_news_dir_entry e;
    if (!gtkhx_proto_parse_news_folderitem (data, dlen, (uint8_t *) out->name,
                                            sizeof (out->name), &e)) {
        return FALSE;
    }
    out->kind = e.kind;
    out->name_len = e.name_len;
    return TRUE;
}

/* ---- HTLC_DATA_CATLIST parser ------------------------------------- */

static void
newscat_post_clear (struct hx_newscat_post *p)
{
    guint16 i;
    if (!p) {
        return;
    }
    g_free (p->subject);
    g_free (p->sender);
    if (p->parts) {
        for (i = 0; i < p->partcount; i++) {
            g_free (p->parts[i].mime_type);
        }
        g_free (p->parts);
    }
    memset (p, 0, sizeof (*p));
}

void
hx_newscat_clear (struct hx_newscat *r)
{
    guint32 i;
    if (!r) {
        return;
    }
    if (r->posts) {
        for (i = 0; i < r->post_count; i++) {
            newscat_post_clear (&r->posts[i]);
        }
        g_free (r->posts);
    }
    memset (r, 0, sizeof (*r));
}

gboolean
hx_newscat_parse (struct htlc_conn *htlc, struct hx_newscat *out)
{
    if (!out) {
        return FALSE;
    }
    memset (out, 0, sizeof (*out));

    /* Phase R2: chunk walk + bounds checks + pstring decoding moved to
     * gtkhx_proto_parse_catlist in the Rust hotline-proto crate. The
     * crate owns the parse tree; we copy each pstring out via
     * g_strndup so the per-post char* fields are g_malloc'd and
     * news_item_take_from_wire can steal them as before. Empty
     * pstrings stay NULL to match the C extractor's contract (the
     * Tier 2 test_newscat_empty_pstrings case pins this). */
    struct gtkhx_proto_catlist *cl
        = gtkhx_proto_parse_catlist (htlc->in.buf, htlc->in.pos);
    if (!cl) {
        return FALSE;
    }

    out->post_count = gtkhx_proto_catlist_post_count (cl);
    if (out->post_count == 0) {
        gtkhx_proto_catlist_free (cl);
        return TRUE;
    }

    out->posts = g_new0 (struct hx_newscat_post, out->post_count);

    for (guint32 i = 0; i < out->post_count; i++) {
        struct hx_newscat_post *pp = &out->posts[i];
        struct gtkhx_proto_catlist_post_view v;
        if (!gtkhx_proto_catlist_post_get (cl, i, &v)) {
            hx_newscat_clear (out);
            gtkhx_proto_catlist_free (cl);
            return FALSE;
        }

        pp->postid = v.postid;
        pp->parentid = v.parentid;
        pp->date_base_year = v.date_base_year;
        pp->date_pad = v.date_pad;
        pp->date_seconds = v.date_seconds;
        pp->partcount = v.partcount;
        pp->size_total = v.size_total;

        if (v.subject_len) {
            pp->subject = g_strndup ((const char *)v.subject_ptr, v.subject_len);
        }
        if (v.sender_len) {
            pp->sender = g_strndup ((const char *)v.sender_ptr, v.sender_len);
        }

        if (v.partcount) {
            pp->parts = g_new0 (struct hx_newscat_part, v.partcount);
            for (guint16 j = 0; j < v.partcount; j++) {
                struct gtkhx_proto_catlist_part_view pv;
                if (!gtkhx_proto_catlist_part_get (cl, i, j, &pv)) {
                    hx_newscat_clear (out);
                    gtkhx_proto_catlist_free (cl);
                    return FALSE;
                }
                if (pv.mime_type_len) {
                    pp->parts[j].mime_type
                        = g_strndup ((const char *)pv.mime_type_ptr,
                                     pv.mime_type_len);
                }
                pp->parts[j].size = pv.size;
            }
        }
    }

    gtkhx_proto_catlist_free (cl);
    return TRUE;
}

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
    /* Phase R2: chunk walk + sanitise moved to
     * gtkhx_proto_walk_news_post. Per-chunk buffer ownership is
     * Rust's; the trampoline above adapts the FFI callback signature
     * to the public hx_news_post_cb's (char *, gsize) shape. */
    struct hx_news_post_trampoline tr = { cb, user };
    return (int)gtkhx_proto_walk_news_post (htlc->in.buf, htlc->in.pos,
                                            hx_news_post_emit, &tr);
}

void
hlpack (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc, va_list ap)
{
    struct hl_hdr h;
    struct hl_data_hdr dhs;
    struct qbuf *q = &htlc->out;
    guint32 this_off, pos;
    guint32 my_trans;

    this_off = q->pos + q->len;
    pos = this_off + SIZEOF_HL_HDR;
    q->len += SIZEOF_HL_HDR;
    q->buf = g_realloc (q->buf, q->pos + q->len);

    h.type = htonl (type);
    my_trans = htlc->trans;
    h.trans = htonl (my_trans);
    htlc->trans++;
    h.flag = htonl (flag);
    h.hc = htons ((guint16)hc);

    while (hc) {
        guint16 t = (guint16)va_arg (ap, int);
        guint16 l = (guint16)va_arg (ap, int);
        guint8 *data;

        dhs.type = htons (t);
        dhs.len = htons (l);

        q->len += SIZEOF_HL_DATA_HDR + l;
        q->buf = g_realloc (q->buf, q->pos + q->len);
        memcpy (&q->buf[pos], (guint8 *)&dhs, SIZEOF_HL_DATA_HDR);
        pos += SIZEOF_HL_DATA_HDR;

        data = va_arg (ap, guint8 *);
        if (l) {
            memcpy (&q->buf[pos], data, l);
            pos += l;
        }
        hc--;
    }

    /* Header's len/len2 fields encode the byte count from the start
	 * of the data section (i.e. total - SIZEOF_HL_HDR + sizeof(hc),
	 * since hc is part of the data section in the wire format).
	 * Match hlwrite's encoding exactly. */
    guint32 packed_len = pos - this_off;
    h.len = h.len2 = htonl (packed_len - (SIZEOF_HL_HDR - sizeof (h.hc)));
    memcpy (q->buf + this_off, &h, SIZEOF_HL_HDR);
}

void
hl_htxf_hdr_pack (guint8 *buf, guint32 ref, guint32 len, guint16 type,
                  guint16 flags)
{
    struct htxf_hdr h;
    h.magic = htonl (HTXF_MAGIC_INT);
    h.ref = htonl (ref);
    h.len = htonl (len);
    /* Last 4 bytes are `unknown u32` in the struct; on the wire
     * they're (u16 type) (u16 flags). Mac-native servers read the
     * type to dispatch the subchannel; cap-aware peers read the
     * flags to know whether to expect the 24-byte large-file
     * variant. Both interpretations share the same word — the type
     * lives in the high u16 and is non-zero, the flags in the low
     * u16. */
    h.unknown = htonl ((((guint32) type) << 16) | flags);
    memcpy (buf, &h, SIZEOF_HTXF_HDR);
}

guint64
hl_capabilities_decode (const guint8 *bytes, guint16 len)
{
    if (!bytes || !len) {
        return 0;
    }
    guint64 caps = 0;
    /* Cap at 8 bytes — anything past that is more than u64 can hold
     * and the spec lets us truncate cleanly (unknown bits are
     * silently preserved by the wire format on round-trip; we just
     * can't store them). */
    guint16 n = len > 8 ? 8 : len;
    for (guint16 i = 0; i < n; i++) {
        caps = (caps << 8) | bytes[i];
    }
    return caps;
}

gboolean
hl_hdr_decode (const void *hdr_bytes, guint32 *type_out, guint32 *trans_out,
               guint32 *flag_out, guint16 *hc_out, guint32 *wire_len_out,
               guint32 *body_len_out)
{
    if (!hdr_bytes) {
        return FALSE;
    }
    const struct hl_hdr *h = (const struct hl_hdr *) hdr_bytes;
    guint32 wire_len = ntohl (h->len);

    if (type_out) {
        *type_out = ntohl (h->type);
    }
    if (trans_out) {
        *trans_out = ntohl (h->trans);
    }
    if (flag_out) {
        *flag_out = ntohl (h->flag);
    }
    if (hc_out) {
        *hc_out = ntohs (h->hc);
    }
    if (wire_len_out) {
        *wire_len_out = wire_len;
    }
    if (body_len_out) {
        /* The wire `len` field encodes "body bytes plus the
         * 2-byte hc field" — hc lives at the tail of the 22-byte
         * header struct but counts as the start of the data
         * section per the protocol spec. Back out to body byte
         * count, clamping wire_len at MAX_HOTLINE_PACKET_LEN and
         * guarding against wire_len < 2 to dodge underflow. */
        guint32 capped = wire_len > MAX_HOTLINE_PACKET_LEN
                             ? MAX_HOTLINE_PACKET_LEN
                             : wire_len;
        *body_len_out = capped < sizeof (h->hc)
                            ? 0
                            : capped - (guint32) sizeof (h->hc);
    }
    return TRUE;
}

void
hlpack_chunks (struct htlc_conn *htlc, guint32 type, guint32 flag,
               const struct hx_chunk *chunks, int hc)
{
    /* Mirror hlpack's wire layout exactly — same header math, same
     * length encoding, same trans++ side effect. We re-implement
     * the body here rather than build a fake va_list (which is
     * unportable) or call hlpack in a loop (which would emit one
     * packet per chunk).
     *
     * Public-API guardrails: this function is the entry point for
     * every shared chunk-array builder (login_packet,
     * chat_history, future modules) plus the integration harness,
     * so a NULL htlc or a chunks=NULL+hc>0 caller is a programming
     * bug we want loud — not a silent NULL deref inside memcpy. */
    g_return_if_fail (htlc != NULL);
    g_return_if_fail (hc >= 0);
    g_return_if_fail (hc == 0 || chunks != NULL);

    struct hl_hdr h;
    struct hl_data_hdr dhs;
    struct qbuf *q = &htlc->out;
    guint32 this_off, pos;
    guint32 my_trans;

    this_off = q->pos + q->len;
    pos = this_off + SIZEOF_HL_HDR;
    q->len += SIZEOF_HL_HDR;
    q->buf = g_realloc (q->buf, q->pos + q->len);

    h.type = htonl (type);
    my_trans = htlc->trans;
    h.trans = htonl (my_trans);
    htlc->trans++;
    h.flag = htonl (flag);
    h.hc = htons ((guint16) hc);

    for (int i = 0; i < hc; i++) {
        guint16 t = chunks[i].type;
        guint16 l = chunks[i].len;

        /* len==0 with data==NULL is a legitimate empty-chunk shape
		 * (HOPE Step 1 sends an empty HTLC_DATA_SESSIONKEY this
		 * way); a non-zero length with NULL data is a caller bug. */
        g_return_if_fail (l == 0 || chunks[i].data != NULL);

        dhs.type = htons (t);
        dhs.len = htons (l);

        q->len += SIZEOF_HL_DATA_HDR + l;
        q->buf = g_realloc (q->buf, q->pos + q->len);
        memcpy (&q->buf[pos], (guint8 *) &dhs, SIZEOF_HL_DATA_HDR);
        pos += SIZEOF_HL_DATA_HDR;

        if (l) {
            memcpy (&q->buf[pos], chunks[i].data, l);
            pos += l;
        }
    }

    guint32 packed_len = pos - this_off;
    h.len = h.len2 = htonl (packed_len - (SIZEOF_HL_HDR - sizeof (h.hc)));
    memcpy (q->buf + this_off, &h, SIZEOF_HL_HDR);
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

HxChatEvent *
hx_chat_event_copy (HxChatEvent *e)
{
    HxChatEvent *c;
    if (!e) {
        return NULL;
    }
    c = g_new0 (HxChatEvent, 1);
    *c = *e; /* shallow copy first */
    c->line = g_strndup (e->line, e->line_len);
    return c;
}

void
hx_chat_event_free (HxChatEvent *e)
{
    if (!e) {
        return;
    }
    g_free (e->line);
    g_free (e);
}

G_DEFINE_BOXED_TYPE (HxChatEvent, hx_chat_event, hx_chat_event_copy,
                     hx_chat_event_free)

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

    if (self_nick && *self_nick && nlen > 0 && strlen (self_nick) == nlen
        && memcmp (e->name, self_nick, nlen) == 0) {
        e->is_self = TRUE;
    }

    return e;
}

HxMsgEvent *
hx_msg_event_copy (HxMsgEvent *e)
{
    HxMsgEvent *c;
    if (!e) {
        return NULL;
    }
    c = g_new0 (HxMsgEvent, 1);
    *c = *e;
    c->name = g_strndup (e->name, e->name_len);
    c->body = g_strndup (e->body, e->body_len);
    return c;
}

void
hx_msg_event_free (HxMsgEvent *e)
{
    if (!e) {
        return;
    }
    g_free (e->name);
    g_free (e->body);
    g_free (e);
}

G_DEFINE_BOXED_TYPE (HxMsgEvent, hx_msg_event, hx_msg_event_copy,
                     hx_msg_event_free)
