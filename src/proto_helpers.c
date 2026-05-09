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
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"

gboolean
task_error_extract (struct htlc_conn *htlc, char *out,
                    gsize out_size, gsize *out_len)
{
	if (!out || out_size == 0)
		return FALSE;

	gboolean found = FALSE;
	dh_start (htlc) {
		if (_type == HTLS_DATA_TASKERROR && !found) {
			gsize copy_len = _len;
			if (copy_len > out_size - 1)
				copy_len = out_size - 1;
			memcpy (out, dh->data, copy_len);
			CR2LF (out, copy_len);
			strip_ansi (out, copy_len);
			out[copy_len] = '\0';
			if (out_len) *out_len = copy_len;
			found = TRUE;
		}
	} dh_end ();

	return found;
}

gboolean
hx_chat_extract (struct htlc_conn *htlc, struct hx_chat_msg *out)
{
	if (!out)
		return FALSE;

	out->cid = 0;
	out->uid = 0;
	out->buf[0] = '\0';
	out->text = out->buf;
	out->text_len = 0;

	guint16 len = 0;

	dh_start (htlc) {
		switch (_type) {
		case HTLS_DATA_CHAT:
			len = (_len > (sizeof (out->buf) - 1))
			      ? (sizeof (out->buf) - 1)
			      : _len;
			memcpy (out->buf, dh->data, len);
			break;
		case HTLS_DATA_CHAT_ID:
			dh_getint (out->cid);
			break;
		case HTLS_DATA_UID:
			dh_getint (out->uid);
			break;
		}
	} dh_end ();

	CR2LF (out->buf, len);
	strip_ansi (out->buf, len);
	out->buf[len] = '\0';

	/* Hotline servers commonly format a chat line as
	 * "\nUser: message" — the leading LF would render as a blank
	 * line in the chat widget, so the original handler stripped
	 * it. Preserve the behaviour. */
	if (out->buf[0] == '\n') {
		out->text = out->buf + 1;
		out->text_len = (len > 0) ? (guint16) (len - 1) : 0;
	} else {
		out->text = out->buf;
		out->text_len = len;
	}

	return TRUE;
}

gboolean
hx_msg_extract (struct htlc_conn *htlc, struct hx_msg_msg *out)
{
	if (!out)
		return FALSE;

	out->uid = 0;
	out->name[0] = '\0';
	out->name_len = 0;
	out->msg[0] = '\0';
	out->msg_len = 0;

	guint16 nlen = 0, msglen = 0;

	dh_start (htlc) {
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
	} dh_end ();

	CR2LF (out->msg, msglen);
	strip_ansi (out->msg, msglen);
	out->name[nlen] = '\0';
	out->msg[msglen] = '\0';
	out->name_len = nlen;
	out->msg_len  = msglen;

	return TRUE;
}

unsigned
hx_selfinfo_parse (struct htlc_conn *htlc)
{
	struct hl_userlist_hdr *uh;
	guint16 nlen;
	unsigned seen = 0;

	dh_start (htlc) {
		switch (_type) {
		case HTLS_DATA_ACCESS:
			if (_len != 8)
				break;
			memcpy (&htlc->access, dh->data, 8);
			seen |= HX_SELFINFO_ACCESS;
			break;
		case HTLS_DATA_USER_LIST:
			if (_len < (SIZEOF_HL_USERLIST_HDR - SIZEOF_HL_DATA_HDR))
				break;
			uh = (struct hl_userlist_hdr *) dh;
			/* Phase 5: the line used to be
			 *   HN16 (&htlc->uid, &htlc->uid);
			 * which is a self-aliasing HN16. The macro reads
			 * from[1] twice (the first line clobbers from[0]),
			 * so both bytes ended up as the original high byte
			 * — corrupting htlc->uid into (high<<8)|high on every
			 * SELFINFO. The user_change handler at rcv.c uses
			 * htlc->uid to detect "this is me" and copy
			 * icon/color/name back out of the user-list row;
			 * with the corruption that branch never matched.
			 * Fix: extract the wire uid out of the chunk like
			 * the surrounding HN16 calls do for icon and nlen. */
			HN16 (&htlc->uid,  &uh->uid);
			HN16 (&htlc->icon, &uh->icon);
			/* Phase 5: the original handler also had
			 *   HN16 (&uh->color, &uh->color);
			 * Same self-alias issue, but on a chunk field nothing
			 * else reads. Drop it — if we ever wanted to capture
			 * our own colour out of SELFINFO we'd need an
			 * htlc->color field write parallel to the icon line. */
			HN16 (&nlen,       &uh->nlen);
			nlen = (nlen > 31) ? 31 : nlen;
			memcpy (htlc->name, uh->name, nlen);
			htlc->name[nlen] = 0;
			seen |= HX_SELFINFO_USER_LIST;
			break;
		}
	} dh_end ();

	return seen;
}

gboolean
hx_user_part_extract (struct htlc_conn *htlc,
                      struct hx_user_part_msg *out)
{
	if (!out)
		return FALSE;

	out->uid = 0;
	out->cid = 0;

	dh_start (htlc) {
		switch (_type) {
		case HTLS_DATA_UID:
			dh_getint (out->uid);
			break;
		case HTLS_DATA_CHAT_ID:
			dh_getint (out->cid);
			break;
		}
	} dh_end ();

	return TRUE;
}

gboolean
hx_chat_subject_extract (struct htlc_conn *htlc,
                         struct hx_chat_subject_msg *out)
{
	if (!out)
		return FALSE;

	out->cid = 0;
	out->subject[0] = '\0';
	out->subject_len = 0;

	guint16 slen = 0;

	dh_start (htlc) {
		switch (_type) {
		case HTLS_DATA_CHAT_ID:
			dh_getint (out->cid);
			break;
		case HTLS_DATA_CHAT_SUBJECT:
			slen = (_len > 255) ? 255 : _len;
			memcpy (out->subject, dh->data, slen);
			break;
		}
	} dh_end ();

	out->subject[slen] = '\0';
	out->subject_len = slen;

	return TRUE;
}

gboolean
hx_chat_invite_extract (struct htlc_conn *htlc,
                        struct hx_chat_invite_msg *out)
{
	if (!out)
		return FALSE;

	out->uid = 0;
	out->cid = 0;
	out->name[0] = '\0';
	out->name_len = 0;

	guint16 nlen = 0;

	dh_start (htlc) {
		switch (_type) {
		case HTLS_DATA_UID:
			dh_getint (out->uid);
			break;
		case HTLS_DATA_CHAT_ID:
			dh_getint (out->cid);
			break;
		case HTLS_DATA_NAME:
			nlen = (_len > 31) ? 31 : _len;
			memcpy (out->name, dh->data, nlen);
			strip_ansi (out->name, nlen);
			break;
		}
	} dh_end ();

	out->name[nlen] = '\0';
	out->name_len = nlen;

	return TRUE;
}

gboolean
hx_user_change_extract (struct htlc_conn *htlc,
                        struct hx_user_change_msg *out)
{
	if (!out)
		return FALSE;

	out->uid = 0;
	out->icon = 0;
	out->color = 0;
	out->got_color = FALSE;
	out->cid = 0;
	out->name[0] = '\0';
	out->name_len = 0;

	guint16 nlen = 0;

	dh_start (htlc) {
		switch (_type) {
		case HTLS_DATA_UID:
			dh_getint (out->uid);
			break;
		case HTLS_DATA_ICON:
			dh_getint (out->icon);
			break;
		case HTLS_DATA_NAME:
			nlen = (_len > 31) ? 31 : _len;
			memcpy (out->name, dh->data, nlen);
			strip_ansi (out->name, nlen);
			break;
		case HTLS_DATA_COLOUR:
			dh_getint (out->color);
			out->got_color = TRUE;
			break;
		case HTLS_DATA_CHAT_ID:
			dh_getint (out->cid);
			break;
		}
	} dh_end ();

	out->name[nlen] = '\0';
	out->name_len = nlen;

	return TRUE;
}

gboolean
hx_xfer_queue_extract (struct htlc_conn *htlc,
                       struct hx_xfer_queue_msg *out)
{
	if (!out)
		return FALSE;

	out->ref = 0;
	out->queueid = 0;

	dh_start (htlc) {
		switch (_type) {
		case HTLS_DATA_HTXF_REF:
			dh_getint (out->ref);
			break;
		case HTLS_DATA_QUEUE:
			dh_getint (out->queueid);
			break;
		}
	} dh_end ();

	return TRUE;
}

void
hlpack (struct htlc_conn *htlc, guint32 type, guint32 flag,
        int hc, va_list ap)
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

	h.type  = htonl (type);
	my_trans = htlc->trans;
	h.trans = htonl (my_trans);
	htlc->trans++;
	h.flag = htonl (flag);
	h.hc   = htons ((guint16) hc);

	while (hc) {
		guint16 t = (guint16) va_arg (ap, int);
		guint16 l = (guint16) va_arg (ap, int);
		guint8 *data;

		dhs.type = htons (t);
		dhs.len  = htons (l);

		q->len += SIZEOF_HL_DATA_HDR + l;
		q->buf = g_realloc (q->buf, q->pos + q->len);
		memcpy (&q->buf[pos], (guint8 *) &dhs, SIZEOF_HL_DATA_HDR);
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
	h.len = h.len2 = htonl (packed_len -
	                        (SIZEOF_HL_HDR - sizeof (h.hc)));
	memcpy (q->buf + this_off, &h, SIZEOF_HL_HDR);
}
