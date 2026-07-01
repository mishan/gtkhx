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
    /* Spec says PART_INDEX is absent on the first request and
	 * present on every follow-up. The first-request vs follow-up
	 * distinction is driven by the `with_part_index` argument
	 * send_download_request takes — FALSE at start() time, TRUE
	 * when the rcv handler resends for the next chunk. This
	 * field is the index value to send on the next follow-up;
	 * the server's first chunk implicitly carries index 0, so
	 * follow-ups start at 1. */
    guint16 next_part_index;
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
    /* ptr_free ties ctx's lifetime to the task entry. The
	 * chunked-continuation path inside rcv_task_download_media
	 * hands ctx ownership from the current task to the next by
	 * clearing the current task's ptr before calling back in
	 * here — task_free is a no-op on a NULL ptr. On the terminal
	 * paths and on disconnect, ptr_free fires from task_delete /
	 * g_hash_table_remove_all and reclaims ctx. */
    struct task *tsk = task_new (
        htlc, RCV_TASK_FN (rcv_task_download_media), ctx, NULL,
        "download-media");
    if (tsk) {
        tsk->ptr_free = (GDestroyNotify) ctx_free;
    }
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
    /* ctx reclaimed by task_delete → ptr_free. */
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
    /* ctx reclaimed by task_delete → ptr_free. */
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
        /* ctx reclaimed by task_delete → ptr_free. */
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
        r.error_message = _ ("Download reply malformed");
        r.error_message_len = strlen (r.error_message);
        if (ctx->on_done) {
            ctx->on_done (htlc, &r, ctx->user_data);
        }
        /* ctx reclaimed by task_delete → ptr_free. */
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

    /* Defence against an out-of-spec server: if the chunk count
	 * we've already issued has reached PART_COUNT, we should
	 * have seen PART_FINAL by now. A server that never sets
	 * PART_FINAL (or sends a smaller-than-advertised
	 * PART_COUNT) would otherwise loop us indefinitely until
	 * next_part_index wraps the u16. Treat this as a malformed
	 * reply: stop, deliver a synthetic failure, free the ctx.
	 *
	 * The chunk-index-at-this-point semantics: we've just
	 * consumed the (next_part_index)-th chunk (0 on the first
	 * reply since next_part_index starts at 1 for the first
	 * follow-up; counting starts at 0). PART_COUNT is the spec
	 * total. So 'we've consumed >= total' is the wrap guard. */
    if (parsed.part_count > 0
        && (guint32) ctx->next_part_index >= (guint32) parsed.part_count) {
        debug_log ("media",
                   "DOWNLOAD_MEDIA exhausted PART_COUNT=%u without "
                   "PART_FINAL — treating as malformed",
                   (unsigned) parsed.part_count);
        HxInlineMediaDownloadResult r;
        memset (&r, 0, sizeof (r));
        r.error_code = 0;
        r.error_message = _ ("Server didn't terminate chunked download");
        r.error_message_len = strlen (r.error_message);
        if (ctx->on_done) {
            ctx->on_done (htlc, &r, ctx->user_data);
        }
        /* ctx reclaimed by task_delete → ptr_free. */
        return;
    }

    /* More chunks coming — hand ctx ownership off to the next
	 * task entry. Without this, rcv.c's post-rcv task_delete
	 * would fire ptr_free on the current task and free ctx
	 * before the next reply lands, leaving the new task entry
	 * pointing at freed memory. Clearing ptr makes ptr_free a
	 * no-op for the current task; send_download_request then
	 * creates a new task with ctx + ptr_free = ctx_free, and the
	 * chunk-by-chunk handoff continues. */
    guint32 cur_trans = 0;
    gtkhx_proto_header_trans (htlc->in.buf, htlc->in.pos, &cur_trans);
    struct task *cur = task_with_trans (sess_from_htlc (htlc), cur_trans);
    if (cur) {
        cur->ptr = NULL;
    }

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
        r.error_message = _ ("Chunked-download resend failed");
        r.error_message_len = strlen (r.error_message);
        if (ctx->on_done) {
            ctx->on_done (htlc, &r, ctx->user_data);
        }
        /* We just orphaned ctx by clearing the current task's
		 * ptr above, and send_download_request didn't take
		 * ownership. Free explicitly. */
        ctx_free (ctx);
        return;
    }
    ctx->next_part_index++;
}
