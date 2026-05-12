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
#include "text_util.h"
#include "debug.h"

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

gboolean
hx_banner_extract (struct htlc_conn *htlc, struct hx_banner_msg *out)
{
	gboolean got_type = FALSE;

	if (!out)
		return FALSE;

	memset (out->type, 0, sizeof (out->type));
	out->has_url = FALSE;
	out->url[0]  = '\0';
	out->url_len = 0;

	dh_start (htlc) {
		switch (_type) {
		case HTLS_DATA_BANNER_TYPE:
			/* Per mhxd's rcv_agreementagree, the type is always
			 * 4 bytes (right-padded with spaces for shorter codes
			 * like "URL"). Reject anything else as malformed. */
			if (_len != 4)
				continue;
			memcpy (out->type, dh->data, 4);
			out->type[4] = '\0';
			got_type = TRUE;
			break;
		case HTLS_DATA_BANNER_URL:
			out->url_len = (_len > sizeof (out->url) - 1)
				? (guint16) (sizeof (out->url) - 1)
				: _len;
			memcpy (out->url, dh->data, out->url_len);
			out->url[out->url_len] = '\0';
			out->has_url = TRUE;
			break;
		}
	} dh_end ();

	return got_type;
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
			/* Phase 5: deliberately DO NOT overwrite htlc->name
			 * with the server-supplied bytes. The server caches
			 * our nick across sessions (hlserver.com keys by IP)
			 * and on every reconnect sends back whatever it
			 * remembered — including bytes from a previous broken
			 * client (Hotline 1.x's Mac Roman, gtkhx's pre-sanitiser
			 * raw-bytes era, an accidental one's-complement, etc.).
			 * Letting that overwrite our local prefs value sets up
			 * a feedback loop: server sends corrupt bytes →
			 * htlc->name picks them up → prefs_write persists →
			 * next session sends the same corrupt bytes back to the
			 * server. (See hx_rcv_user_selfinfo: it now pushes our
			 * local name + icon via hx_change_name_icon right after
			 * the parse, so the server's cache gets updated to
			 * match our prefs instead of the other way around.)
			 *
			 * uh->name is still chunk-bounded so the wire frame
			 * stays valid; we just don't write the bytes anywhere.
			 * Surface them under category 'name' for forensics. */
			if (nlen) {
				GString *hex = g_string_new (NULL);
				guint16 i;
				for (i = 0; i < nlen; i++) {
					if (i) g_string_append_c (hex, ' ');
					g_string_append_printf (hex, "%02x",
					                        ((const guint8 *) uh->name)[i]);
				}
				debug_log ("name",
				           "SELFINFO USER_LIST cached name "
				           "ignored (nlen=%u hex=[%s]) — local "
				           "prefs nick wins; will push via "
				           "USER_CHANGE",
				           (unsigned) nlen, hex->str);
				g_string_free (hex, TRUE);
			}
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

hx_agreement_result
hx_agreement_extract (struct htlc_conn *htlc, char *out,
                      gsize out_size, gsize *out_len)
{
	dh_start (htlc) {
		if (_type == HTLS_DATA_NOAGREEMENT)
			return HX_AGREEMENT_NONE;
		if (_type != HTLS_DATA_AGREEMENT)
			continue;

		if (out && out_size > 0) {
			gsize copy_len = _len;
			if (copy_len > out_size - 1)
				copy_len = out_size - 1;
			memcpy (out, dh->data, copy_len);
			CR2LF (out, copy_len);
			strip_ansi (out, copy_len);
			out[copy_len] = '\0';
			if (out_len) *out_len = copy_len;
		}
		return HX_AGREEMENT_OK;
	} dh_end ();

	return HX_AGREEMENT_NOT_FOUND;
}

gboolean
hx_news_file_extract (struct htlc_conn *htlc, char *out,
                      gsize out_size, gsize *out_len)
{
	if (!out || out_size == 0)
		return FALSE;

	gboolean found = FALSE;
	dh_start (htlc) {
		if (_type != HTLS_DATA_NEWS)
			continue;
		if (found)
			continue;   /* first NEWS chunk wins */

		gsize copy_len = _len;
		if (copy_len > out_size - 1)
			copy_len = out_size - 1;
		memcpy (out, dh->data, copy_len);
		CR2LF (out, copy_len);
		strip_ansi (out, copy_len);
		out[copy_len] = '\0';
		if (out_len) *out_len = copy_len;
		found = TRUE;
	} dh_end ();

	return found;
}

int
hx_news_post_walk (struct htlc_conn *htlc,
                   hx_news_post_cb cb, void *user)
{
	int seen = 0;

	dh_start (htlc) {
		if (_type != HTLS_DATA_NEWS)
			continue;

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
		if (cb)
			cb (user, buf, len);

		g_free (buf);
	} dh_end ();

	return seen;
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

/* See doc-comment in proto_helpers.h. */
gboolean
hx_chat_split_nick_body (const char *line, gsize line_len,
                         gsize *name_offset, gsize *name_len,
                         gsize *body_offset, gsize *body_len)
{
	gsize ws, colon, body_start;

	if (!line || line_len == 0)
		return FALSE;

	/* Strip leading horizontal whitespace. Hotline servers
	 * commonly pad the line with spaces (right-aligning the
	 * nick) before the actual nick text. */
	ws = 0;
	while (ws < line_len
	       && (line[ws] == ' ' || line[ws] == '\t'))
		ws++;
	if (ws == line_len)
		return FALSE;

	/* Find the first ':' after the whitespace. The
	 * "nick: body" Hotline format uses a single colon as the
	 * separator. */
	colon = ws;
	while (colon < line_len && line[colon] != ':')
		colon++;
	if (colon == line_len)
		return FALSE;
	if (colon == ws)
		return FALSE;	/* empty nick */

	/* Cap nick length at 31 — the Hotline protocol's nick
	 * field is a 31-byte STRING32. Lines whose pre-colon
	 * portion exceeds that are almost certainly not chat
	 * (URLs, "Subject Changed to:", arbitrary system prose);
	 * pass them through unsplit. */
	if (colon - ws > 31)
		return FALSE;

	/* Skip the spaces between ':' and the body. Conventional
	 * mhxd output pads to two spaces; one-space variants exist;
	 * a body that's all-whitespace is still a valid (empty)
	 * message. */
	body_start = colon + 1;
	while (body_start < line_len && line[body_start] == ' ')
		body_start++;

	if (name_offset) *name_offset = ws;
	if (name_len)    *name_len    = colon - ws;
	if (body_offset) *body_offset = body_start;
	if (body_len)    *body_len    = line_len - body_start;
	return TRUE;
}

/* ASCII-only "is this byte alphanumeric" — used for the word-
 * boundary check below. Locale-agnostic on purpose: nick lookup
 * shouldn't care about the user's LC_CTYPE. */
static gboolean
is_word_byte (guchar c)
{
	return (c >= '0' && c <= '9')
	    || (c >= 'A' && c <= 'Z')
	    || (c >= 'a' && c <= 'z')
	    || c == '_';
}

/* See doc-comment in proto_helpers.h. */
gboolean
hx_highlight_match (const char *body, gsize body_len,
                    const char * const *words)
{
	if (!body || body_len == 0 || !words)
		return FALSE;

	for (gsize wi = 0; words[wi] != NULL; wi++) {
		const char *w = words[wi];
		gsize wlen;

		if (!w || !*w)
			continue;
		wlen = strlen (w);
		if (wlen > body_len)
			continue;

		/* Walk every possible match start position. */
		for (gsize i = 0; i + wlen <= body_len; i++) {
			/* Before-edge boundary check: position i must
			 * either be at the buffer start, or follow a
			 * non-word byte. */
			if (i > 0 && is_word_byte ((guchar) body[i - 1]))
				continue;
			/* After-edge boundary check: position i + wlen
			 * must either be at the buffer end, or precede
			 * a non-word byte. */
			if (i + wlen < body_len
			    && is_word_byte ((guchar) body[i + wlen]))
				continue;

			/* ASCII case-insensitive byte compare. */
			if (g_ascii_strncasecmp (body + i, w, wlen) == 0)
				return TRUE;
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
hx_chat_event_new (const char *raw, gsize raw_len,
                   guint32 cid, const char *self_nick)
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
	    && memcmp (e->line, hx_info_prefix, HX_INFO_PREFIX_LEN) == 0)
		e->is_info = TRUE;

	if (!e->is_info && e->line_len > 0) {
		gsize so = 0, sl = 0, bo = 0, bl = 0;
		if (hx_chat_split_nick_body (e->line, e->line_len,
		                              &so, &sl, &bo, &bl)) {
			e->sender_off = so;
			e->sender_len = sl;
			e->body_off   = bo;
			e->body_len   = bl;

			if (self_nick && *self_nick && sl > 0
			    && strlen (self_nick) == sl
			    && memcmp (e->line + so, self_nick, sl) == 0)
				e->is_self = TRUE;
		}
	}

	return e;
}

HxChatEvent *
hx_chat_event_copy (HxChatEvent *e)
{
	HxChatEvent *c;
	if (!e)
		return NULL;
	c = g_new0 (HxChatEvent, 1);
	*c = *e;                          /* shallow copy first */
	c->line = g_strndup (e->line, e->line_len);
	return c;
}

void
hx_chat_event_free (HxChatEvent *e)
{
	if (!e)
		return;
	g_free (e->line);
	g_free (e);
}

G_DEFINE_BOXED_TYPE (HxChatEvent, hx_chat_event,
                     hx_chat_event_copy, hx_chat_event_free)

/* ---- HxMsgEvent ---------------------------------------------------- */

HxMsgEvent *
hx_msg_event_new (guint16 uid,
                  const char *name, gsize name_len,
                  const char *body, gsize body_len,
                  const char *self_nick)
{
	HxMsgEvent *e;
	gsize nlen = 0, blen = 0;

	e = g_new0 (HxMsgEvent, 1);
	e->uid          = uid;
	e->is_broadcast = (uid == 0);
	e->name         = gtkhx_text_to_utf8 (name, name_len, &nlen);
	e->name_len     = nlen;
	e->body         = gtkhx_text_to_utf8 (body, body_len, &blen);
	e->body_len     = blen;

	if (self_nick && *self_nick && nlen > 0
	    && strlen (self_nick) == nlen
	    && memcmp (e->name, self_nick, nlen) == 0)
		e->is_self = TRUE;

	return e;
}

HxMsgEvent *
hx_msg_event_copy (HxMsgEvent *e)
{
	HxMsgEvent *c;
	if (!e)
		return NULL;
	c = g_new0 (HxMsgEvent, 1);
	*c = *e;
	c->name = g_strndup (e->name, e->name_len);
	c->body = g_strndup (e->body, e->body_len);
	return c;
}

void
hx_msg_event_free (HxMsgEvent *e)
{
	if (!e)
		return;
	g_free (e->name);
	g_free (e->body);
	g_free (e);
}

G_DEFINE_BOXED_TYPE (HxMsgEvent, hx_msg_event,
                     hx_msg_event_copy, hx_msg_event_free)
