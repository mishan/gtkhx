/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * See inline_media_download.h for the contract.
 *
 * The state machine is a thin loop on top of the Phase 9.A FFI:
 * for each chunk in the chunked-reply sequence, we re-register a
 * task and re-send TranDownloadMedia with the next PART_INDEX.
 * Single-chunk replies fall out of the loop on the first
 * iteration because the server sets PART_FINAL on the only chunk.
 *
 * Cancel: the dialog can request cancel; the helper marks the
 * context's `cancelled` flag and prevents the callback from
 * firing. The TASK reply still arrives (we can't unwind a
 * task_new), so rcv_task_download_media checks the flag before
 * doing anything with the reply.
 */

#include "config.h"
#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"        /* PACKED — before hotline.h */
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h" /* hx_chunk, task_error_extract */
#include "hotline_proto.h"
#include "hx.h"
#include "network.h"       /* hlwrite_chunks */
#include "tasks.h"         /* task_new + RCV_TASK_FN */
#include "inline_media.h"  /* cap gate */
#include "inline_media_download.h"
#include "debug.h"

struct hx_inline_media_download {
    HxInlineMediaDownloadCallback on_done;
    gpointer user_data;
    guint8 *handle;        /* owned copy */
    gsize handle_len;
    /* Chunk accumulator. Bytes appended in order; freed in the
	 * cleanup path. */
    GByteArray *payload;
    /* Canonical MIME from the first reply that carried it.
	 * Owned heap copy (NUL-terminated). */
    char *mime;
    /* Spec says PART_INDEX defaults absent on the first request,
	 * present on subsequent requests. We track the next index
	 * to send (0 means we haven't sent the first request yet,
	 * but state_first_sent below distinguishes "haven't sent"
	 * from "sent index 0 then need 1 next" — the spec's
	 * convention is for clients to start at index 1 on follow-
	 * ups because the server's first chunk implicitly carries
	 * index 0). */
    guint16 next_part_index;
    gboolean state_first_sent;
    /* When TRUE, rcv_task_download_media silently drops the
	 * reply and frees the context. Flipped by cancel(). */
    gboolean cancelled;
};

static void
ctx_free (hx_inline_media_download *ctx)
{
    if (!ctx) {
        return;
    }
    g_free (ctx->handle);
    if (ctx->payload) {
        g_byte_array_unref (ctx->payload);
    }
    g_free (ctx->mime);
    g_free (ctx);
}

/* Build + send a TranDownloadMedia request. When `with_part_index`
 * is TRUE, include CHAT_MEDIA_PART_INDEX = ctx->next_part_index.
 * Returns TRUE on success. */
static gboolean
send_download_request (struct htlc_conn *htlc, hx_inline_media_download *ctx,
                       gboolean with_part_index)
{
    struct hx_chunk chunks[2];
    guint8 scratch[2];
    int32_t hc = gtkhx_proto_build_download_media_chunks (
        ctx->handle, ctx->handle_len,
        with_part_index ? ctx->next_part_index : 0,
        with_part_index ? true : false,
        chunks, G_N_ELEMENTS (chunks), scratch, sizeof (scratch));
    if (hc <= 0) {
        debug_log ("media",
                   "download: builder rejected (handle_len=%zu, "
                   "with_part_index=%d)",
                   ctx->handle_len, (int) with_part_index);
        return FALSE;
    }
    task_new (htlc, RCV_TASK_FN (rcv_task_download_media), ctx, NULL,
              "download-media");
    hlwrite_chunks (htlc, HTLC_HDR_DOWNLOAD_MEDIA, 0, chunks, hc);
    return TRUE;
}

hx_inline_media_download *
inline_media_download_start (struct htlc_conn *htlc,
                             const guint8 *handle, gsize handle_len,
                             HxInlineMediaDownloadCallback on_done,
                             gpointer user_data)
{
    if (!inline_media_cap_ok (htlc)) {
        return NULL;
    }
    if (!handle || handle_len == 0 || handle_len > 65535) {
        debug_log ("media",
                   "download: bad handle (len=%zu)", handle_len);
        return NULL;
    }

    hx_inline_media_download *ctx = g_new0 (hx_inline_media_download, 1);
    ctx->on_done = on_done;
    ctx->user_data = user_data;
    ctx->handle = g_malloc (handle_len);
    memcpy (ctx->handle, handle, handle_len);
    ctx->handle_len = handle_len;
    ctx->payload = g_byte_array_new ();
    ctx->next_part_index = 1; /* first follow-up if needed */

    debug_log ("media",
               "→ DOWNLOAD_MEDIA first request (handle_len=%zu)",
               handle_len);

    if (!send_download_request (htlc, ctx, /*with_part_index=*/FALSE)) {
        ctx_free (ctx);
        return NULL;
    }
    ctx->state_first_sent = TRUE;
    return ctx;
}

void
inline_media_download_cancel (hx_inline_media_download *dl)
{
    if (!dl) {
        return;
    }
    dl->cancelled = TRUE;
    /* The TASK reply still arrives; rcv_task_download_media
	 * checks the flag and silently frees the context without
	 * invoking the callback. We can't unwind task_new, so this
	 * flag-then-drop approach is the cleanest we can do without
	 * adding cancel support upstream. */
}

static void
deliver_failure (struct htlc_conn *htlc, hx_inline_media_download *ctx)
{
    HxInlineMediaDownloadResult r;
    memset (&r, 0, sizeof (r));
    r.error_code = gtkhx_proto_extract_media_error_code (
        htlc->in.buf, htlc->in.pos);

    char err_buf[1024];
    gsize err_len = 0;
    if (task_error_extract (htlc, err_buf, sizeof (err_buf), &err_len)) {
        r.error_message = err_buf;
        r.error_message_len = err_len;
    }

    debug_log ("media",
               "← DOWNLOAD_MEDIA error_code=%u (msg='%.*s')",
               (unsigned) r.error_code,
               (int) (r.error_message_len > 256 ? 256 : r.error_message_len),
               r.error_message ? r.error_message : "");

    if (ctx->on_done) {
        ctx->on_done (htlc, &r, ctx->user_data);
    }
    ctx_free (ctx);
}

static void
deliver_success (struct htlc_conn *htlc, hx_inline_media_download *ctx)
{
    HxInlineMediaDownloadResult r;
    memset (&r, 0, sizeof (r));
    r.bytes = ctx->payload;
    r.canonical_mime = ctx->mime ? ctx->mime : "application/octet-stream";

    debug_log ("media",
               "← DOWNLOAD_MEDIA ok (total_bytes=%u, mime=%s)",
               (unsigned) ctx->payload->len, r.canonical_mime);

    if (ctx->on_done) {
        ctx->on_done (htlc, &r, ctx->user_data);
    }
    ctx_free (ctx);
}

void
rcv_task_download_media (struct htlc_conn *htlc, void *ctx_ptr, void *unused)
{
    (void) unused;
    hx_inline_media_download *ctx = ctx_ptr;
    if (!ctx) {
        return;
    }
    if (ctx->cancelled) {
        debug_log ("media", "DOWNLOAD_MEDIA reply for cancelled download");
        ctx_free (ctx);
        return;
    }

    if (task_inerror (htlc)) {
        deliver_failure (htlc, ctx);
        return;
    }

    struct gtkhx_proto_download_reply parsed;
    if (!gtkhx_proto_parse_download_reply (htlc->in.buf, htlc->in.pos,
                                           &parsed)) {
        debug_log ("media",
                   "DOWNLOAD_MEDIA reply missing required fields; "
                   "synthesising generic failure");
        /* Synthesise a generic failure so the dialog can show an
		 * error rather than spinning forever. */
        HxInlineMediaDownloadResult r;
        memset (&r, 0, sizeof (r));
        r.error_code = 0;
        r.error_message = "Download reply malformed";
        r.error_message_len = strlen (r.error_message);
        if (ctx->on_done) {
            ctx->on_done (htlc, &r, ctx->user_data);
        }
        ctx_free (ctx);
        return;
    }

    /* First reply that carries TYPE wins (subsequent chunks repeat
	 * the same value per spec; we honour the first one we saw). */
    if (!ctx->mime && parsed.type_len > 0) {
        ctx->mime = g_strndup ((const char *) parsed.type_ptr, parsed.type_len);
    }

    /* Append this chunk's payload to the accumulator. */
    if (parsed.payload_len > 0) {
        g_byte_array_append (ctx->payload, parsed.payload_ptr,
                             parsed.payload_len);
    }

    if (parsed.final_chunk) {
        deliver_success (htlc, ctx);
        return;
    }

    /* More chunks coming — issue the next request. */
    debug_log ("media",
               "DOWNLOAD_MEDIA chunk %u/%u accumulated (%u bytes so far)",
               (unsigned) ctx->next_part_index,
               (unsigned) parsed.part_count,
               (unsigned) ctx->payload->len);

    if (!send_download_request (htlc, ctx, /*with_part_index=*/TRUE)) {
        /* Builder rejected on resend — treat as failure. */
        HxInlineMediaDownloadResult r;
        memset (&r, 0, sizeof (r));
        r.error_code = 0;
        r.error_message = "Chunked-download resend failed";
        r.error_message_len = strlen (r.error_message);
        if (ctx->on_done) {
            ctx->on_done (htlc, &r, ctx->user_data);
        }
        ctx_free (ctx);
        return;
    }
    ctx->next_part_index++;
}
