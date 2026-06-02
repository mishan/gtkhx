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

    gboolean found = FALSE;
    dh_start (htlc)
    {
        if (_type == HTLS_DATA_TASKERROR && !found) {
            gsize copy_len = _len;
            if (copy_len > out_size - 1) {
                copy_len = out_size - 1;
            }
            memcpy (out, dh->data, copy_len);
            CR2LF (out, copy_len);
            strip_ansi (out, copy_len);
            out[copy_len] = '\0';
            if (out_len) {
                *out_len = copy_len;
            }
            found = TRUE;
        }
    }
    dh_end ();

    return found;
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

    out->uid = 0;
    out->name[0] = '\0';
    out->name_len = 0;
    out->msg[0] = '\0';
    out->msg_len = 0;

    guint16 nlen = 0, msglen = 0;

    dh_start (htlc)
    {
        switch (_type) {
        case HTLS_DATA_UID:
            dh_getint (out->uid);
            break;
        case HTLS_DATA_MSG:
            msglen = (_len > 8192) ? 8192 : _len;
            memcpy (out->msg, dh->data, msglen);
            break;
        case HTLS_DATA_NAME:
            nlen = (_len > 128) ? 128 : _len;
            memcpy (out->name, dh->data, nlen);
            strip_ansi (out->name, nlen);
            break;
        }
    }
    dh_end ();

    CR2LF (out->msg, msglen);
    strip_ansi (out->msg, msglen);
    out->name[nlen] = '\0';
    out->msg[msglen] = '\0';
    out->name_len = nlen;
    out->msg_len = msglen;

    return TRUE;
}

gboolean
hx_banner_extract (struct htlc_conn *htlc, struct hx_banner_msg *out)
{
    gboolean got_type = FALSE;

    if (!out) {
        return FALSE;
    }

    memset (out->type, 0, sizeof (out->type));
    out->has_url = FALSE;
    out->url[0] = '\0';
    out->url_len = 0;

    dh_start (htlc)
    {
        switch (_type) {
        case HTLS_DATA_BANNER_TYPE:
            /* Per mhxd's rcv_agreementagree, the type is always
			 * 4 bytes (right-padded with spaces for shorter codes
			 * like "URL"). Reject anything else as malformed. */
            if (_len != 4) {
                continue;
            }
            memcpy (out->type, dh->data, 4);
            out->type[4] = '\0';
            got_type = TRUE;
            break;
        case HTLS_DATA_BANNER_URL:
            out->url_len = (_len > sizeof (out->url) - 1)
                               ? (guint16)(sizeof (out->url) - 1)
                               : _len;
            memcpy (out->url, dh->data, out->url_len);
            out->url[out->url_len] = '\0';
            out->has_url = TRUE;
            break;
        }
    }
    dh_end ();

    return got_type;
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

    out->ref = 0;
    out->queueid = 0;

    dh_start (htlc)
    {
        switch (_type) {
        case HTLS_DATA_HTXF_REF:
            dh_getint (out->ref);
            break;
        case HTLS_DATA_QUEUE:
            dh_getint (out->queueid);
            break;
        }
    }
    dh_end ();

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
    dh_start (htlc)
    {
        if (_type == HTLS_DATA_NOAGREEMENT) {
            return HX_AGREEMENT_NONE;
        }
        if (_type != HTLS_DATA_AGREEMENT) {
            continue;
        }

        if (out && out_size > 0) {
            gsize copy_len = _len;
            if (copy_len > out_size - 1) {
                copy_len = out_size - 1;
            }
            memcpy (out, dh->data, copy_len);
            CR2LF (out, copy_len);
            strip_ansi (out, copy_len);
            out[copy_len] = '\0';
            if (out_len) {
                *out_len = copy_len;
            }
        }
        return HX_AGREEMENT_OK;
    }
    dh_end ();

    return HX_AGREEMENT_NOT_FOUND;
}

gboolean
hx_news_file_extract (struct htlc_conn *htlc, char *out, gsize out_size,
                      gsize *out_len)
{
    if (!out || out_size == 0) {
        return FALSE;
    }

    gboolean found = FALSE;
    dh_start (htlc)
    {
        if (_type != HTLS_DATA_NEWS) {
            continue;
        }
        if (found) {
            continue; /* first NEWS chunk wins */
        }

        gsize copy_len = _len;
        if (copy_len > out_size - 1) {
            copy_len = out_size - 1;
        }
        memcpy (out, dh->data, copy_len);
        CR2LF (out, copy_len);
        strip_ansi (out, copy_len);
        out[copy_len] = '\0';
        if (out_len) {
            *out_len = copy_len;
        }
        found = TRUE;
    }
    dh_end ();

    return found;
}

gboolean
hx_news_dirlist_parse_categoryitem (const guint8 *data, gsize dlen,
                                    struct hx_news_dirlist_entry *out)
{
    guint16 ntype;
    gsize off;
    guint8 namelen;

    if (!out) {
        return FALSE;
    }
    if (!data || dlen < 4) {
        return FALSE;
    }

    HN16 (&ntype, data);

    if (ntype == 2) {
        /* bundle / folder: ntype(2) + count(2) before namelen */
        off = 4;
    } else if (ntype == 3) {
        /* category: ntype(2) + count(2) + guid(16) + addsn(4) +
		 * deletesn(4) = 28 bytes before namelen */
        off = 28;
    } else {
        /* Unknown subtype — refuse the entry. Don't fail the
		 * surrounding dirlist; the caller skips and keeps walking. */
        return FALSE;
    }

    if (dlen < off + 1u) {
        return FALSE;
    }
    namelen = data[off];
    off++;

    if (dlen < off + namelen) {
        return FALSE;
    }

    out->kind = (ntype == 2) ? 1 : 2;
    out->name_len = namelen;
    if (namelen) {
        memcpy (out->name, data + off, namelen);
    }
    out->name[namelen] = '\0';
    return TRUE;
}

gboolean
hx_news_dirlist_parse_folderitem (const guint8 *data, gsize dlen,
                                  struct hx_news_dirlist_entry *out)
{
    guint16 nlen;

    if (!out) {
        return FALSE;
    }
    if (!data || dlen < 1) {
        return FALSE;
    }

    /* u8 ntype, then name[dlen - 1]. The original gtkhx parser at
	 * rcv_task_newsfolder_list copied dh->data[0] straight into
	 * item->type and used the rest as the name. We preserve that
	 * contract exactly: ntype==1 → folder (kind 1), anything else
	 * → category (kind 2). */
    nlen = (dlen > sizeof (out->name)) ? (sizeof (out->name) - 1)
                                       : (guint16)(dlen - 1);

    out->kind = (data[0] == 1) ? 1 : 2;
    out->name_len = nlen;
    if (nlen) {
        memcpy (out->name, data + 1, nlen);
    }
    out->name[nlen] = '\0';
    return TRUE;
}

/* ---- HTLC_DATA_CATLIST parser ------------------------------------- */

/* Read one length-prefixed Hotline pstring from `*pp`.
 *
 * Wire format: u8 length, then `length` bytes. Length 0 means an
 * empty string; the original gtkhx parser returned NULL for that
 * case and we preserve the contract (consumers null-check before
 * use).
 *
 * On success advances `*pp` past the pstring (length byte + content)
 * and decrements `*remaining` accordingly. Returns TRUE.
 *
 * On overrun (not enough bytes for the length byte itself, or
 * length > remaining-after-len-byte) returns FALSE without
 * advancing. `*out` is set to NULL on FALSE so the caller's free
 * walk is safe. */
static gboolean
newscat_read_pstring (const guint8 **pp, gsize *remaining, char **out)
{
    const guint8 *p = *pp;
    gsize r = *remaining;
    guint8 len;

    *out = NULL;

    if (r < 1) {
        return FALSE;
    }
    len = *p;
    p++;
    r--;

    if (len > r) {
        return FALSE;
    }

    if (len > 0) {
        *out = g_malloc (len + 1);
        memcpy (*out, p, len);
        (*out)[len] = '\0';
        p += len;
        r -= len;
    }

    *pp = p;
    *remaining = r;
    return TRUE;
}

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
    const guint8 *p;
    gsize remaining;
    guint32 i;
    gboolean found = FALSE, ok = TRUE;

    if (!out) {
        return FALSE;
    }
    memset (out, 0, sizeof (*out));

    dh_start (htlc)
    {
        if (_type != HTLC_DATA_CATLIST || found) {
            continue;
        }
        found = TRUE;

        p = (const guint8 *)dh->data;
        remaining = _len;

        /* Threadlist header: u32 __x0 + u32 post_count + u16 __x1.
		 * 10 bytes total. */
        if (remaining < 10) {
            ok = FALSE;
            break;
        }
        p += 4; /* skip __x0 */
        remaining -= 4;
        HN32 (&out->post_count, p);
        p += 4;
        remaining -= 4;
        p += 2; /* skip __x1 */
        remaining -= 2;

        if (out->post_count == 0) {
            break;
        }

        /* Defensive: refuse counts that obviously can't fit. Each
		 * post needs at minimum SIZEOF_HL_NEWS_THREAD_HDR (22) +
		 * 2 (two zero-len pstrings) = 24 bytes. Cap the up-front
		 * allocation against the wire's actual byte budget so a
		 * forged post_count can't make us allocate gigabytes. */
        if ((gsize)out->post_count > remaining / 24) {
            ok = FALSE;
            break;
        }

        out->posts = g_new0 (struct hx_newscat_post, out->post_count);

        for (i = 0; i < out->post_count && ok; i++) {
            struct hx_newscat_post *pp = &out->posts[i];
            guint16 j;

            /* Per-thread fixed header: 22 bytes
			 * (id + 8-byte date + parentid + 4-byte flags +
			 *  partcount). */
            if (remaining < 22) {
                ok = FALSE;
                break;
            }
            HN32 (&pp->postid, p);
            p += 4;
            remaining -= 4;
            HN16 (&pp->date_base_year, p);
            p += 2;
            remaining -= 2;
            HN16 (&pp->date_pad, p);
            p += 2;
            remaining -= 2;
            HN32 (&pp->date_seconds, p);
            p += 4;
            remaining -= 4;
            HN32 (&pp->parentid, p);
            p += 4;
            remaining -= 4;
            p += 4; /* skip flags */
            remaining -= 4;
            HN16 (&pp->partcount, p);
            p += 2;
            remaining -= 2;

            if (!newscat_read_pstring (&p, &remaining, &pp->subject)) {
                ok = FALSE;
                break;
            }
            if (!newscat_read_pstring (&p, &remaining, &pp->sender)) {
                ok = FALSE;
                break;
            }

            if (pp->partcount == 0) {
                continue;
            }

            /* Each part: pstring mime + u16 size = at least 3 bytes. */
            if ((gsize)pp->partcount > remaining / 3) {
                ok = FALSE;
                break;
            }
            pp->parts = g_new0 (struct hx_newscat_part, pp->partcount);
            for (j = 0; j < pp->partcount; j++) {
                if (!newscat_read_pstring (&p, &remaining,
                                           &pp->parts[j].mime_type)) {
                    ok = FALSE;
                    break;
                }
                if (remaining < 2) {
                    ok = FALSE;
                    break;
                }
                HN16 (&pp->parts[j].size, p);
                p += 2;
                remaining -= 2;
                pp->size_total += pp->parts[j].size;
            }
        }
    }
    dh_end ();

    if (!found || !ok) {
        hx_newscat_clear (out);
        return FALSE;
    }
    return TRUE;
}

int
hx_news_post_walk (struct htlc_conn *htlc, hx_news_post_cb cb, void *user)
{
    int seen = 0;

    dh_start (htlc)
    {
        if (_type != HTLS_DATA_NEWS) {
            continue;
        }

        /* Sanitise into a heap buffer we own — chunk bodies can
		 * be up to 64 KiB (uint16 length), too big for the stack
		 * on every platform. NUL-terminate for the cb's
		 * convenience. The buffer is freed before returning. */
        gsize len = _len;
        char *buf = g_malloc (len + 1);
        memcpy (buf, dh->data, len);
        CR2LF (buf, len);
        strip_ansi (buf, len);
        buf[len] = '\0';

        seen++;
        if (cb) {
            cb (user, buf, len);
        }

        g_free (buf);
    }
    dh_end ();

    return seen;
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
