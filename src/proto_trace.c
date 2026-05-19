/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>
#include <netinet/in.h>
/* Deliberately do NOT include hx.h. proto_trace.c only needs the
 * wire-protocol opcode constants and the htlc_conn struct, both of
 * which protocol.h provides (it transitively pulls in hotline.h
 * after compat.h's PACKED macro is in scope). Skipping hx.h means
 * the unit tests can compile this translation unit without dragging
 * in GTK, libadwaita, or any session / widget plumbing. */
#include "protocol.h"
#include "debug.h"
#include "proto_trace.h"

#define CAT "proto"

/* Hex fallback formatter. Single static buffer per-call type means
 * a single trace line that uses both proto_hdr_name and
 * proto_data_name with unknown opcodes will overwrite — fine for our
 * use, since we only ever interleave one hex name per log call (the
 * format substitutes happen before the next call). */
static const char *
hex_hdr (guint32 type)
{
    static char buf[16];
    g_snprintf (buf, sizeof (buf), "0x%06x", type);
    return buf;
}

static const char *
hex_data (guint16 type)
{
    static char buf[12];
    g_snprintf (buf, sizeof (buf), "0x%04x", type);
    return buf;
}

const char *
proto_hdr_name (guint32 type)
{
    switch (type) {
    /* Client → server */
    case HTLC_HDR_NEWS_GETFILE:
        return "HTLC_HDR_NEWS_GETFILE";
    case HTLC_HDR_NEWS_POST:
        return "HTLC_HDR_NEWS_POST";
    case HTLC_HDR_CHAT:
        return "HTLC_HDR_CHAT";
    case HTLC_HDR_LOGIN:
        return "HTLC_HDR_LOGIN";
    case HTLC_HDR_MSG:
        return "HTLC_HDR_MSG";
    case HTLC_HDR_USER_KICK:
        return "HTLC_HDR_USER_KICK";
    case HTLC_HDR_CHAT_CREATE:
        return "HTLC_HDR_CHAT_CREATE";
    case HTLC_HDR_CHAT_INVITE:
        return "HTLC/S_HDR_CHAT_INVITE";
    case HTLC_HDR_CHAT_DECLINE:
        return "HTLC_HDR_CHAT_DECLINE";
    case HTLC_HDR_CHAT_JOIN:
        return "HTLC_HDR_CHAT_JOIN";
    case HTLC_HDR_CHAT_PART:
        return "HTLC_HDR_CHAT_PART";
    case HTLC_HDR_CHAT_SUBJECT:
        return "HTLC_HDR_CHAT_SUBJECT";
    case HTLC_HDR_FILE_LIST:
        return "HTLC_HDR_FILE_LIST";
    case HTLC_HDR_FILE_GET:
        return "HTLC_HDR_FILE_GET";
    case HTLC_HDR_FILE_PUT:
        return "HTLC_HDR_FILE_PUT";
    case HTLC_HDR_FILE_DELETE:
        return "HTLC_HDR_FILE_DELETE";
    case HTLC_HDR_FILE_MKDIR:
        return "HTLC_HDR_FILE_MKDIR";
    case HTLC_HDR_FILE_GETINFO:
        return "HTLC_HDR_FILE_GETINFO";
    case HTLC_HDR_FILE_SETINFO:
        return "HTLC_HDR_FILE_SETINFO";
    case HTLC_HDR_FILE_MOVE:
        return "HTLC_HDR_FILE_MOVE";
    case HTLC_HDR_FILE_SYMLINK:
        return "HTLC_HDR_FILE_SYMLINK";
    case HTLC_HDR_USER_GETLIST:
        return "HTLC_HDR_USER_GETLIST";
    case HTLC_HDR_USER_GETINFO:
        return "HTLC_HDR_USER_GETINFO";
    case HTLC_HDR_USER_CHANGE:
        return "HTLC_HDR_USER_CHANGE";
    case HTLC_HDR_ACCOUNT_CREATE:
        return "HTLC_HDR_ACCOUNT_CREATE";
    case HTLC_HDR_ACCOUNT_DELETE:
        return "HTLC_HDR_ACCOUNT_DELETE";
    case HTLC_HDR_ACCOUNT_READ:
        return "HTLC_HDR_ACCOUNT_READ";
    case HTLC_HDR_ACCOUNT_MODIFY:
        return "HTLC_HDR_ACCOUNT_MODIFY";
    case HTLC_HDR_MSG_BROADCAST:
        return "HTLC/S_HDR_MSG_BROADCAST";
    case HTLC_HDR_NEWSDIRLIST:
        return "HTLC_HDR_NEWSDIRLIST";
    case HTLC_HDR_NEWSCATLIST:
        return "HTLC_HDR_NEWSCATLIST";
    case HTLC_HDR_DELNEWSDIRCAT:
        return "HTLC_HDR_DELNEWSDIRCAT";
    case HTLC_HDR_MAKENEWSDIR:
        return "HTLC_HDR_MAKENEWSDIR";
    case HTLC_HDR_MAKECATEGORY:
        return "HTLC_HDR_MAKECATEGORY";
    case HTLC_HDR_GETTHREAD:
        return "HTLC_HDR_GETTHREAD";
    case HTLC_HDR_POSTTHREAD:
        return "HTLC_HDR_POSTTHREAD";
    case HTLC_HDR_DELETETHREAD:
        return "HTLC_HDR_DELETETHREAD";
    case HTLC_HDR_ICON_GET:
        return "HTLC_HDR_ICON_GET";
    case HTLC_HDR_FILE_HASH:
        return "HTLC_HDR_FILE_HASH";

    /* Phase 5 mhxd additions; HTLC and HTLS PING share opcode 0x1f4. */
    case HTLC_HDR_PING:
        return "HTLC/S_HDR_PING";
    case HTLC_HDR_AGREEMENTAGREE:
        return "HTLC_HDR_AGREEMENTAGREE";
    case HTLC_HDR_KILLDOWNLOAD:
        return "HTLC_HDR_KILLDOWNLOAD";
    case HTLC_HDR_DOWNLOAD_BANNER:
        return "HTLC_HDR_DOWNLOAD_BANNER";

    /* Server → client */
    case HTLS_HDR_NEWS_POST:
        return "HTLS_HDR_NEWS_POST";
    case HTLS_HDR_MSG:
        return "HTLS_HDR_MSG";
    case HTLS_HDR_CHAT:
        return "HTLS_HDR_CHAT";
    case HTLS_HDR_AGREEMENT:
        return "HTLS_HDR_AGREEMENT";
    case HTLS_HDR_POLITEQUIT:
        return "HTLS_HDR_POLITEQUIT";
    case HTLS_HDR_CHAT_USER_CHANGE:
        return "HTLS_HDR_CHAT_USER_CHANGE";
    case HTLS_HDR_CHAT_USER_PART:
        return "HTLS_HDR_CHAT_USER_PART";
    case HTLS_HDR_CHAT_SUBJECT:
        return "HTLS_HDR_CHAT_SUBJECT";
    case HTLS_HDR_USER_CHANGE:
        return "HTLS_HDR_USER_CHANGE";
    case HTLS_HDR_USER_PART:
        return "HTLS_HDR_USER_PART";
    case HTLS_HDR_USER_SELFINFO:
        return "HTLS_HDR_USER_SELFINFO";
    case HTLS_HDR_TASK:
        return "HTLS_HDR_TASK";
    case HTLS_HDR_QUEUE:
        return "HTLS_HDR_QUEUE";
    case HTLS_HDR_BANNER:
        return "HTLS_HDR_BANNER";

    default:
        return hex_hdr (type);
    }
}

const char *
proto_data_name (guint16 type)
{
    switch (type) {
    case HTLS_DATA_TASKERROR:
        return "HTLS_DATA_TASKERROR";

    case HTLC_DATA_CHAT:
        return "HTLC/S_DATA_CHAT/MSG/NEWS_POST/AGREEMENT/USER_INFO (0x65)";
    case HTLC_DATA_NAME:
        return "HTLC/S_DATA_NAME";
    case HTLC_DATA_UID:
        return "HTLC/S_DATA_UID";
    case HTLC_DATA_ICON:
        return "HTLC/S_DATA_ICON";
    case HTLC_DATA_LOGIN:
        return "HTLC/S_DATA_LOGIN";
    case HTLC_DATA_PASSWORD:
        return "HTLC/S_DATA_PASSWORD";
    case HTLS_DATA_HTXF_REF:
        return "HTLS_DATA_HTXF_REF";
    case HTLC_DATA_HTXF_SIZE:
        return "HTLC/S_DATA_HTXF_SIZE";
    case HTLC_DATA_STYLE:
        return "HTLC/S_DATA_STYLE";
    case HTLC_DATA_ACCESS:
        return "HTLC/S_DATA_ACCESS";
    case HTLS_DATA_COLOUR:
        return "HTLS_DATA_COLOUR";
    case HTLC_DATA_BAN:
        return "HTLC_DATA_BAN";
    case HTLC_DATA_CHAT_ID:
        return "HTLC/S_DATA_CHAT_ID";
    case HTLC_DATA_CHAT_SUBJECT:
        return "HTLC/S_DATA_CHAT_SUBJECT";
    case HTLS_DATA_QUEUE:
        return "HTLS_DATA_QUEUE";
    case HTLS_DATA_VERSION:
        return "HTLS_DATA_VERSION";
    case HTLS_DATA_BANNER_TYPE:
        return "HTLS_DATA_BANNER_TYPE";
    case HTLS_DATA_BANNER_URL:
        return "HTLS_DATA_BANNER_URL";
    /* HTLS_DATA_HTXF_SIZE shares opcode 0x6c with
	 * HTLC_DATA_HTXF_SIZE — already named above. */
    case HTLS_DATA_SERVERNAME:
        return "HTLS_DATA_SERVERNAME";
    case HTLS_DATA_NOAGREEMENT:
        return "HTLS_DATA_NOAGREEMENT";

    case HTLC_DATA_FILE_NAME:
        return "HTLC/S_DATA_FILE_NAME";
    case HTLC_DATA_DIR:
        return "HTLC_DATA_DIR";
    case HTLC_DATA_RFLT:
        return "HTLC/S_DATA_RFLT";
    case HTLC_DATA_FILE_PREVIEW:
        return "HTLC_DATA_FILE_PREVIEW";
    case HTLS_DATA_FILE_TYPE:
        return "HTLS_DATA_FILE_TYPE";
    case HTLS_DATA_FILE_CREATOR:
        return "HTLS_DATA_FILE_CREATOR";
    case HTLS_DATA_FILE_SIZE:
        return "HTLS_DATA_FILE_SIZE";
    case HTLS_DATA_FILE_DATE_CREATE:
        return "HTLS_DATA_FILE_DATE_CREATE";
    case HTLS_DATA_FILE_DATE_MODIFY:
        return "HTLS_DATA_FILE_DATE_MODIFY";
    case HTLC_DATA_FILE_COMMENT:
        return "HTLC/S_DATA_FILE_COMMENT";
    case HTLC_DATA_FILE_RENAME:
        return "HTLC_DATA_FILE_RENAME";
    case HTLC_DATA_DIR_RENAME:
        return "HTLC_DATA_DIR_RENAME";
    case HTLS_DATA_FILE_LIST:
        return "HTLS_DATA_FILE_LIST";
    case HTLS_DATA_FILE_ICON:
        return "HTLS_DATA_FILE_ICON";
    case HTLS_DATA_FILE_NFILES:
        return "HTLS_DATA_FILE_NFILES";
    case HTLS_DATA_USER_LIST:
        return "HTLS_DATA_USER_LIST";

    case HTLS_DATA_SESSIONKEY:
        return "HTLC/S_DATA_SESSIONKEY";
    case HTLS_DATA_MAC_ALG:
        return "HTLC/S_DATA_MAC_ALG";
    case HTLS_DATA_CIPHER_ALG:
        return "HTLS_DATA_CIPHER_ALG";
    case HTLC_DATA_CIPHER_ALG:
        return "HTLC_DATA_CIPHER_ALG";
    case HTLS_DATA_COMPRESS_ALG:
        return "HTLS_DATA_COMPRESS_ALG";
    case HTLC_DATA_COMPRESS_ALG:
        return "HTLC_DATA_COMPRESS_ALG";

    case HTLC_DATA_NEWSFOLDERITEM:
        return "HTLC_DATA_NEWSFOLDERITEM";
    case HTLC_DATA_CATLIST:
        return "HTLC_DATA_CATLIST";
    case HTLC_DATA_CATEGORY:
        return "HTLC_DATA_CATEGORY";
    case HTLC_DATA_CATEGORYITEM:
        return "HTLC_DATA_CATEGORYITEM";
    case HTLC_DATA_NEWSPATH:
        return "HTLC_DATA_NEWSPATH";
    case HTLC_DATA_THREADID:
        return "HTLC_DATA_THREADID";
    case HTLC_DATA_NEWSTYPE:
        return "HTLC_DATA_NEWSTYPE";
    case HTLC_DATA_NEWSSUBJECT:
        return "HTLC_DATA_NEWSSUBJECT";
    case HTLC_DATA_NEWSAUTHOR:
        return "HTLC_DATA_NEWSAUTHOR";
    case HTLC_DATA_NEWSDATE:
        return "HTLC_DATA_NEWSDATE";
    case HTLC_DATA_NEWSDIR:
        return "HTLC_DATA_NEWSDIR";
    case HTLC_DATA_NEWSDATA:
        return "HTLC_DATA_NEWSDATA";

    default:
        return hex_data (type);
    }
}

/* Render a small data preview as either a quoted printable-ASCII
 * string (when content looks textual) or a hex dump (when it doesn't),
 * truncated at PREVIEW_MAX bytes. Caller g_frees. */
#define PREVIEW_MAX 48

static char *
data_preview (const guint8 *data, guint16 len)
{
    gboolean printable;
    gsize show, i;
    GString *out;

    if (!data || !len) {
        return g_strdup ("");
    }

    show = len > PREVIEW_MAX ? PREVIEW_MAX : len;
    printable = TRUE;
    for (i = 0; i < show; i++) {
        guint8 c = data[i];
        if (c == 0 && i == show - 1 && i == (gsize)(len - 1)) {
            continue; /* trailing NUL is fine */
        }
        if (!isprint (c) && c != '\t' && c != '\n' && c != '\r') {
            printable = FALSE;
            break;
        }
    }

    out = g_string_new (NULL);
    if (printable) {
        g_string_append_c (out, '"');
        for (i = 0; i < show; i++) {
            guint8 c = data[i];
            switch (c) {
            case '"':
                g_string_append (out, "\\\"");
                break;
            case '\\':
                g_string_append (out, "\\\\");
                break;
            case '\n':
                g_string_append (out, "\\n");
                break;
            case '\r':
                g_string_append (out, "\\r");
                break;
            case '\t':
                g_string_append (out, "\\t");
                break;
            default:
                if (c == 0) {
                    /* mid-string NUL — show as \0 */
                    g_string_append (out, "\\0");
                } else {
                    g_string_append_c (out, c);
                }
            }
        }
        g_string_append_c (out, '"');
    } else {
        for (i = 0; i < show; i++) {
            g_string_append_printf (out, "%02x ", data[i]);
        }
        g_string_truncate (out, out->len - 1); /* drop trailing space */
    }

    if (len > PREVIEW_MAX) {
        g_string_append_printf (out, "... (+%u bytes)",
                                (unsigned)(len - PREVIEW_MAX));
    }
    return g_string_free (out, FALSE);
}

/* ---------- Outgoing trace ---------- */

void
proto_trace_send_begin (guint32 type, guint32 trans, guint16 hc)
{
    if (!debug_category_enabled (CAT)) {
        return;
    }
    debug_log (CAT, "→ trans=%u type=%s (0x%06x) hc=%u", trans,
               proto_hdr_name (type), type, hc);
}

void
proto_trace_send_chunk (guint16 type, guint16 len, const guint8 *data)
{
    char *prev;
    if (!debug_category_enabled (CAT)) {
        return;
    }
    prev = data_preview (data, len);
    debug_log (CAT, "  chunk type=%s (0x%04x) len=%u %s",
               proto_data_name (type), type, len, prev);
    g_free (prev);
}

void
proto_trace_send_end (void)
{
    /* Reserved for a future "—" footer / total-byte report. No-op
	 * for now so the call sites are stable. */
}

/* ---------- Incoming trace ---------- */

void
proto_trace_recv_hdr (guint32 type, guint32 trans, guint32 flag, guint32 len)
{
    if (!debug_category_enabled (CAT)) {
        return;
    }
    debug_log (CAT, "← trans=%u type=%s (0x%06x) flag=%u len=%u", trans,
               proto_hdr_name (type), type, flag, len);
}

void
proto_trace_recv_chunks (struct htlc_conn *htlc)
{
    guint32 pos, max;

    if (!debug_category_enabled (CAT)) {
        return;
    }
    if (!htlc || !htlc->in.buf) {
        return;
    }

    /* Same walk dh_start does, but standalone so we don't have to
	 * weave a trace call into every consumer of those macros. */
    pos = SIZEOF_HL_HDR;
    max = htlc->in.pos;
    debug_log (CAT, "  recv-chunks entry: in.pos=%u in.len=%u rcv=%p",
               htlc->in.pos, htlc->in.len, (void *) htlc->rcv);
    while (pos + SIZEOF_HL_DATA_HDR < max) {
        struct hl_data_hdr *dh = (struct hl_data_hdr *)(&htlc->in.buf[pos]);
        guint16 dlen, dtype;
        char *prev;

        HN16 (&dlen, &dh->len);
        HN16 (&dtype, &dh->type);
        if (dlen > (max - pos) - SIZEOF_HL_DATA_HDR) {
            break;
        }

        prev = data_preview (dh->data, dlen);
        debug_log (CAT, "  chunk type=%s (0x%04x) len=%u %s",
                   proto_data_name (dtype), dtype, dlen, prev);
        g_free (prev);

        pos += SIZEOF_HL_DATA_HDR + dlen;
    }
}
