/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Inline-media send-side glue (Phase 9.C).
 *
 * See inline_media_upload.h for the contract.
 *
 * Lifecycle of an upload:
 *
 *   hx_send_upload_media_single
 *     ├─ cap_ok / payload validation
 *     ├─ g_new0 hx_upload_ctx, stash on_done + user_data
 *     ├─ task_new (RCV_TASK_FN (rcv_task_upload_media), ctx, NULL,
 *     │           "upload-media") — must run BEFORE the wire send
 *     │           so hx_rcv_task finds the entry when the TASK reply
 *     │           lands
 *     ├─ hlwrite_chunks (... HTLC_HDR_UPLOAD_MEDIA ...)
 *     └─ return TRUE
 *
 *   server TASK reply lands → rcv_task_upload_media (htlc, ctx_ptr, _)
 *     ├─ if task_inerror — parse DATA_ERROR text + optional
 *     │   DATA_CHAT_MEDIA_ERROR_CODE; build HxInlineMediaUploadResult
 *     │   with error_code set; invoke callback
 *     ├─ else — parse upload-final-reply (id, mime, w/h/bytes);
 *     │   build success HxInlineMediaUploadResult; invoke callback
 *     └─ free ctx
 *
 * The callback runs on the main thread (rcv.c → task dispatch is
 * already on main). pointers inside the result struct live in
 * htlc->in.buf and are only valid for the duration of the
 * callback — callers that retain the handle MUST copy.
 */

#include "config.h"
#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"        /* PACKED — required before hotline.h */
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h" /* hx_chunk */
#include "hotline_proto.h" /* gtkhx_proto_build_upload_media_* */
#include "hx.h"
#include "network.h"       /* hlwrite_chunks */
#include "tasks.h"         /* task_new + RCV_TASK_FN */
#include "text_util.h"     /* gtkhx_text_for_wire */
#include "inline_media.h"  /* inline_media_cap_ok */
#include "inline_media_upload.h"
#include "debug.h"

/* Per-upload heap context. Lifetime is tied to the task table
 * entry via task->ptr_free = upload_ctx_free, so:
 *   - The normal TASK reply path: rcv_task_upload_media delivers
 *     on_done, sets callback_fired=TRUE, returns. rcv.c then
 *     runs task_delete, which fires task_free → ptr_free →
 *     upload_ctx_free.
 *   - The disconnect path: sess->tasks is cleared via
 *     g_hash_table_remove_all in hx_htlc_close, which fires
 *     task_free → ptr_free → upload_ctx_free for every
 *     still-in-flight upload. on_done never runs, so
 *     callback_fired stays FALSE and upload_ctx_free invokes
 *     user_data_free (when set) so the caller's per-upload
 *     context is reclaimed in the same pass.
 *
 * Future: chunk index, accumulated bytes, upload token for
 * chunked uploads. Single-shot needs none of this. */
typedef struct {
    HxInlineMediaUploadCallback on_done;
    gpointer user_data;
    GDestroyNotify user_data_free;
    gboolean callback_fired;
} hx_upload_ctx;

static void
upload_ctx_free (hx_upload_ctx *ctx)
{
    if (!ctx) {
        return;
    }
    if (!ctx->callback_fired && ctx->user_data_free && ctx->user_data) {
        /* Connection torn down before on_done fired — release
		 * the caller's per-upload context too. */
        ctx->user_data_free (ctx->user_data);
    }
    g_free (ctx);
}

/* Caller hands payload + an optional declared MIME hint. The
 * Phase 9.A builder is responsible for the chunk shape — including
 * the spec-required PART_FINAL byte that marks this as the only
 * chunk. */
gboolean
hx_send_upload_media_single (struct htlc_conn *htlc,
                             const guint8 *payload, gsize payload_len,
                             const char *declared_type,
                             gsize declared_type_len,
                             HxInlineMediaUploadCallback on_done,
                             gpointer user_data,
                             GDestroyNotify user_data_free)
{
    if (!inline_media_cap_ok (htlc)) {
        return FALSE;
    }
    if (!payload || payload_len == 0) {
        debug_log ("media", "upload: empty payload rejected");
        return FALSE;
    }
    if (payload_len > 65535) {
        /* Single-shot is bounded by the 16-bit wire chunk length.
		 * Larger payloads need the chunked path, not in 9.C scope. */
        debug_log ("media",
                   "upload: %zu byte payload exceeds single-shot cap "
                   "(chunked upload not yet implemented)",
                   payload_len);
        return FALSE;
    }

    struct hx_chunk chunks[3];
    guint8 scratch[1];
    int32_t hc = gtkhx_proto_build_upload_media_single_chunks (
        payload, payload_len,
        (const uint8_t *) declared_type, declared_type_len,
        chunks, G_N_ELEMENTS (chunks), scratch, sizeof (scratch));
    if (hc <= 0) {
        debug_log ("media", "upload: single-shot builder rejected payload "
                            "(len=%zu, declared_type_len=%zu)",
                   payload_len, declared_type_len);
        return FALSE;
    }

    hx_upload_ctx *ctx = g_new0 (hx_upload_ctx, 1);
    ctx->on_done = on_done;
    ctx->user_data = user_data;
    ctx->user_data_free = user_data_free;

    debug_log ("media",
               "→ UPLOAD_MEDIA single-shot (payload_len=%zu, "
               "declared_type_len=%zu)",
               payload_len, declared_type_len);

    /* Register the TASK-reply hook BEFORE hlwrite_chunks consumes
	 * htlc->trans. Mirrors voice.c::hx_send_voice_join. The
	 * ptr_free hook ties ctx's lifetime to the task entry — see
	 * the typedef block above for the full rationale. */
    struct task *tsk = task_new (
        htlc, RCV_TASK_FN (rcv_task_upload_media), ctx, NULL, "upload-media");
    if (tsk) {
        tsk->ptr_free = (GDestroyNotify) upload_ctx_free;
    }
    hlwrite_chunks (htlc, HTLC_HDR_UPLOAD_MEDIA, 0, chunks, hc);
    return TRUE;
}

/* Parse error reply: DATA_ERROR text + optional
 * DATA_CHAT_MEDIA_ERROR_CODE. The error text we surface comes
 * directly from htlc->in.buf — borrowed for the duration of the
 * callback. The Rust extractor for the error code is fully
 * tolerant of absent / unknown values (returns 0 = Generic). */
static void
deliver_failure (struct htlc_conn *htlc, hx_upload_ctx *ctx)
{
    HxInlineMediaUploadResult r;
    memset (&r, 0, sizeof (r));

    r.error_code = gtkhx_proto_extract_media_error_code (
        htlc->in.buf, htlc->in.pos);

    /* Best-effort error text from DATA_TASK_ERROR. Same pattern
	 * task_error in tasks.c uses. Pull it through task_error_extract
	 * into a local stack buffer so the callback can read it
	 * without traversing the chunk walker itself. */
    char err_buf[1024];
    gsize err_len = 0;
    if (task_error_extract (htlc, err_buf, sizeof (err_buf), &err_len)) {
        r.error_message = err_buf;
        r.error_message_len = err_len;
    }

    debug_log ("media",
               "← UPLOAD_MEDIA error_code=%u (msg='%.*s')",
               (unsigned) r.error_code,
               (int) (r.error_message_len > 256 ? 256 : r.error_message_len),
               r.error_message ? r.error_message : "");

    ctx->callback_fired = TRUE;
    if (ctx->on_done) {
        ctx->on_done (htlc, &r, ctx->user_data);
    }
}

static void
deliver_success (struct htlc_conn *htlc, hx_upload_ctx *ctx)
{
    struct gtkhx_proto_upload_final_reply parsed;
    if (!gtkhx_proto_parse_upload_final_reply (htlc->in.buf, htlc->in.pos,
                                               &parsed)) {
        /* Reply was structurally malformed — handle is missing.
		 * Surface as a generic failure with a synthetic error
		 * code so the caller doesn't have to special-case this. */
        debug_log ("media",
                   "← UPLOAD_MEDIA success TASK without CHAT_MEDIA_ID; "
                   "treating as generic failure");
        HxInlineMediaUploadResult r;
        memset (&r, 0, sizeof (r));
        r.error_code = 0;
        r.error_message = "Upload reply malformed";
        r.error_message_len = strlen (r.error_message);
        ctx->callback_fired = TRUE;
        if (ctx->on_done) {
            ctx->on_done (htlc, &r, ctx->user_data);
        }
        return;
    }

    /* mime_type wire bytes are NOT NUL-terminated; copy into a
	 * stack buffer so the callback sees a NUL-terminated C
	 * string. ≤ 64 bytes per spec; clamp defensively. */
    char mime_buf[128];
    gsize mime_len = parsed.type_len;
    if (mime_len >= sizeof (mime_buf)) {
        mime_len = sizeof (mime_buf) - 1;
    }
    memcpy (mime_buf, parsed.type_ptr, mime_len);
    mime_buf[mime_len] = '\0';

    HxInlineMediaUploadResult r;
    memset (&r, 0, sizeof (r));
    r.media_id = parsed.id_ptr;
    r.media_id_len = parsed.id_len;
    r.media_type = mime_buf;
    r.media_type_len = mime_len;
    r.width = parsed.width;
    r.height = parsed.height;
    r.bytes = parsed.bytes;
    r.width_present = parsed.width_present;
    r.height_present = parsed.height_present;
    r.bytes_present = parsed.bytes_present;

    debug_log ("media",
               "← UPLOAD_MEDIA ok: handle_len=%zu mime=%s dims=%ux%u "
               "bytes=%u",
               r.media_id_len, mime_buf,
               (unsigned) r.width, (unsigned) r.height,
               (unsigned) r.bytes);

    ctx->callback_fired = TRUE;
    if (ctx->on_done) {
        ctx->on_done (htlc, &r, ctx->user_data);
    }
}

void
rcv_task_upload_media (struct htlc_conn *htlc, void *ctx_ptr, void *unused)
{
    (void) unused;
    hx_upload_ctx *ctx = ctx_ptr;
    if (!ctx) {
        return;
    }

    if (task_inerror (htlc)) {
        deliver_failure (htlc, ctx);
    } else {
        deliver_success (htlc, ctx);
    }
    /* ctx is reclaimed via task->ptr_free when rcv.c runs
	 * task_delete after this handler returns. Don't free here. */
}

/* ---- Chat send with attached media ---- */

void
hx_send_chat_with_media (struct htlc_conn *htlc, const char *str,
                         guint32 cid, guint16 style,
                         const guint8 *media_id, gsize media_id_len,
                         const char *mime, gsize mime_len)
{
    if (!htlc || !str) {
        return;
    }

    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize wire_len = 0;
    char *wire = gtkhx_text_for_wire (str, strlen (str), utf8,
                                      /*is_body=*/TRUE, &wire_len);

    /* Decide whether to attach media: cap negotiated AND caller
	 * supplied both id + mime, with both lengths fitting in the
	 * 16-bit Hotline chunk-length field. Per spec receivers MUST
	 * reject a chat that carries exactly one of the two — we
	 * don't send that shape from the client. The cap gate avoids
	 * the round-trip when the server has already said it won't
	 * relay media for our session.
	 *
	 * The u16 guard below is defence-in-depth: handles in
	 * practice are far below 64 KB (the spec caps them at 64
	 * bytes; Janus issues much smaller tokens), and MIME strings
	 * are tens of bytes, but a buggy caller passing a
	 * pathologically-large length would otherwise silently
	 * truncate when the (guint16) cast trims the high bits — the
	 * wire would carry the low-16-bits of the length with the
	 * full payload bytes following, and the server would parse
	 * either a short handle or run past it into the next chunk.
	 * Refuse to attach in that case; fall back to plain chat. */
    gboolean have_media = (media_id && media_id_len > 0
                           && media_id_len <= G_MAXUINT16 && mime
                           && mime_len > 0 && mime_len <= G_MAXUINT16);
    if (have_media && !(htlc->caps & HTLC_CAP_INLINE_MEDIA)) {
        debug_log ("media",
                   "chat-with-media: dropping companions, CAP_INLINE_MEDIA "
                   "not negotiated");
        have_media = FALSE;
    }

    /* Build the base 2/3-chunk chat send via the Phase R2 chat
	 * builder, then optionally append CHAT_MEDIA_ID +
	 * CHAT_MEDIA_TYPE chunks. We can't ride the chat builder for
	 * the companion chunks (it's shape-frozen as STYLE + BODY +
	 * optional CHAT_ID) so we extend its output array here. */
    enum
    {
        MAX_CHUNKS = 5
    };
    struct hx_chunk chunks[MAX_CHUNKS];
    guint8 scratch[8];
    int hc = (int) gtkhx_proto_build_chat_chunks (
        cid, style, (const uint8_t *) wire, wire_len, chunks, MAX_CHUNKS,
        scratch, sizeof (scratch));
    if (hc <= 0) {
        debug_log ("media", "chat-with-media: chat builder failed");
        g_free (wire);
        return;
    }

    if (have_media && hc + 2 <= MAX_CHUNKS) {
        chunks[hc].type = HTLC_DATA_CHAT_MEDIA_ID;
        chunks[hc].len = (guint16) media_id_len;
        chunks[hc].data = media_id;
        hc++;
        chunks[hc].type = HTLC_DATA_CHAT_MEDIA_TYPE;
        chunks[hc].len = (guint16) mime_len;
        chunks[hc].data = (const guint8 *) mime;
        hc++;
        debug_log ("media",
                   "→ TranChatSend with media (cid=%u handle_len=%zu mime=%.*s)",
                   cid, media_id_len, (int) mime_len, mime);
    }

    hlwrite_chunks (htlc, HTLC_HDR_CHAT, 0, chunks, hc);
    g_free (wire);
}
