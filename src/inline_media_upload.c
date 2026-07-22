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
#include "hxconn.h"
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
 * Chunked uploads add a second wrinkle: each chunk sends a fresh
 * UPLOAD_MEDIA transaction with its own trans id and its own
 * task entry, but the SAME ctx threads through all of them.
 * rcv_task_upload_media hands ctx ownership off to the next task
 * by clearing the current entry's ptr before send_next_chunk runs
 * (the same pattern inline_media_download.c::rcv_task_download_media
 * uses for chunked replies). When the final reply lands, ctx falls
 * out of the table via the final task's ptr_free hook. */
typedef struct {
    HxInlineMediaUploadCallback on_done;
    gpointer user_data;
    GDestroyNotify user_data_free;
    gboolean callback_fired;

    /* Chunked-upload state. payload == NULL for single-shot. */
    guint8 *payload;        /* owned copy; freed in upload_ctx_free */
    gsize payload_len;
    char *declared_type;    /* NUL-terminated; or NULL */
    gsize declared_type_len;
    guint32 chunk_size;     /* per-chunk slice size */
    guint16 part_count;     /* total number of chunks */
    guint16 next_part_index; /* index of the chunk we'll send NEXT */
    guint8 *upload_token;   /* owned copy; NULL until server hands one over */
    gsize upload_token_len;
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
    g_free (ctx->payload);
    g_free (ctx->declared_type);
    g_free (ctx->upload_token);
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
    if (!tsk) {
        /* Synchronous reject. Same shape as the chunked entry —
		 * no wire write, ctx reclaimed, user_data left to the
		 * caller. */
        debug_log ("media",
                   "upload: task_new failed for single-shot; "
                   "synchronous reject");
        ctx->user_data = NULL;
        ctx->user_data_free = NULL;
        upload_ctx_free (ctx);
        return FALSE;
    }
    tsk->ptr_free = (GDestroyNotify) upload_ctx_free;
    hlwrite_chunks (htlc, HTLC_HDR_UPLOAD_MEDIA, 0, chunks, hc);
    return TRUE;
}

/* ---- Chunked upload state machine ----
 *
 * Wire shape (recap of inline_media.rs):
 *
 *   chunk 0:  PAYLOAD + optional DECLARED_TYPE + PART_INDEX=0 +
 *             PART_COUNT + PART_FINAL=0
 *   chunk k (1 ≤ k < part_count - 1):
 *             UPLOAD_TOKEN + PART_INDEX=k + PAYLOAD + PART_FINAL=0
 *   chunk part_count - 1 (final):
 *             UPLOAD_TOKEN + PART_INDEX + PAYLOAD + PART_FINAL=1
 *
 * Server reply on chunk 0: intermediate, carries UPLOAD_TOKEN.
 * Server reply on follow-ups except the last: intermediate (may
 *   echo UPLOAD_TOKEN; the spec says safe-to-echo so we accept
 *   both with-and-without).
 * Server reply on final chunk: TranUploadMedia success reply with
 *   CHAT_MEDIA_ID + CHAT_MEDIA_TYPE + advisory dims/bytes. Same
 *   shape rcv_task_upload_media's success path already handles.
 *
 * State machine entry:
 *   hx_send_upload_media_chunked
 *     ├─ build chunk 0 via build_upload_media_first_chunks
 *     ├─ task_new("upload-media", ctx, ptr_free=upload_ctx_free)
 *     ├─ ctx->next_part_index = 1   (next chunk to send)
 *     └─ hlwrite_chunks
 *
 * State machine continuation (inside rcv_task_upload_media):
 *   ├─ if task_inerror → deliver_failure → done
 *   ├─ if next_part_index >= part_count → final reply expected
 *   │     parse_upload_final_reply → deliver_success → done
 *   ├─ else (intermediate reply):
 *   │     ├─ if next_part_index == 1 → parse_upload_token_reply,
 *   │     │     stash token. (Required on first reply.)
 *   │     ├─ else → optionally re-stash token if server echoed it.
 *   │     ├─ hand ctx ownership off (clear current task's ptr)
 *   │     ├─ send_next_chunk → builds via _followup_chunks +
 *   │     │     task_new with ptr_free again
 *   │     └─ ctx->next_part_index++
 *   └─ return — rcv.c runs task_delete on the current entry,
 *       which is now ptr=NULL → no double free.
 */

/* Slice into ctx->payload for the chunk at index k. Returns
 * (slice_ptr, slice_len) without copying — the bytes live in
 * ctx->payload's owned buffer for the full upload lifetime. */
static void
upload_chunk_slice (const hx_upload_ctx *ctx, guint16 index,
                    const guint8 **out_ptr, gsize *out_len)
{
    gsize start = (gsize) index * (gsize) ctx->chunk_size;
    gsize remaining
        = ctx->payload_len > start ? ctx->payload_len - start : 0;
    gsize len = remaining < (gsize) ctx->chunk_size ? remaining
                                                   : (gsize) ctx->chunk_size;
    *out_ptr = ctx->payload + start;
    *out_len = len;
}

/* Send the chunk at ctx->next_part_index using the follow-up
 * builder + UPLOAD_TOKEN echo. Returns TRUE on send success. The
 * caller is responsible for handing ctx ownership off from the
 * current task entry BEFORE invoking this. */
static gboolean
send_next_chunk (struct htlc_conn *htlc, hx_upload_ctx *ctx)
{
    if (!ctx->upload_token || ctx->upload_token_len == 0) {
        debug_log ("media",
                   "upload: chunked continuation without UPLOAD_TOKEN");
        return FALSE;
    }
    if (ctx->next_part_index == 0
        || ctx->next_part_index >= ctx->part_count) {
        debug_log ("media",
                   "upload: chunked continuation index %u out of range "
                   "(part_count=%u)",
                   (unsigned) ctx->next_part_index,
                   (unsigned) ctx->part_count);
        return FALSE;
    }
    const guint8 *slice = NULL;
    gsize slice_len = 0;
    upload_chunk_slice (ctx, ctx->next_part_index, &slice, &slice_len);

    gboolean is_final
        = (guint32) (ctx->next_part_index + 1) >= (guint32) ctx->part_count;

    struct hx_chunk chunks[4];
    guint8 scratch[3];
    int32_t hc = gtkhx_proto_build_upload_media_followup_chunks (
        ctx->upload_token, ctx->upload_token_len, slice, slice_len,
        ctx->next_part_index, is_final, chunks, G_N_ELEMENTS (chunks),
        scratch, sizeof (scratch));
    if (hc <= 0) {
        debug_log ("media",
                   "upload: follow-up builder rejected chunk %u/%u",
                   (unsigned) ctx->next_part_index,
                   (unsigned) ctx->part_count);
        return FALSE;
    }

    struct task *tsk = task_new (
        htlc, RCV_TASK_FN (rcv_task_upload_media), ctx, NULL, "upload-media");
    if (!tsk) {
        /* No task entry means no rcv routing — sending the chunk
		 * would put bytes on the wire we'd never reconcile a reply
		 * for. Fail closed; the caller restores ctx ownership on
		 * the current task entry and surfaces a synthetic failure. */
        debug_log ("media",
                   "upload: task_new failed for chunked follow-up %u/%u",
                   (unsigned) ctx->next_part_index,
                   (unsigned) ctx->part_count);
        return FALSE;
    }
    tsk->ptr_free = (GDestroyNotify) upload_ctx_free;
    hlwrite_chunks (htlc, HTLC_HDR_UPLOAD_MEDIA, 0, chunks, hc);

    debug_log ("media",
               "→ UPLOAD_MEDIA chunk %u/%u (slice=%zu, final=%d)",
               (unsigned) ctx->next_part_index,
               (unsigned) ctx->part_count, slice_len, (int) is_final);

    ctx->next_part_index++;
    return TRUE;
}

gboolean
hx_send_upload_media_chunked (struct htlc_conn *htlc,
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

    guint32 chunk_size = inline_media_chunk_size (htlc);
    if (chunk_size == 0) {
        debug_log ("media", "upload: chunk_size is 0; refusing chunked send");
        return FALSE;
    }

    /* ceil(payload_len / chunk_size). part_count must fit in u16
	 * (and the builder requires >= 2 for the chunked first-chunk
	 * shape — payloads that fit in one chunk go via single-shot). */
    guint64 pc = ((guint64) payload_len + chunk_size - 1) / chunk_size;
    if (pc < 2) {
        debug_log ("media",
                   "upload: chunked called with single-chunk payload "
                   "(%zu bytes, chunk_size=%u) — caller should use _single",
                   payload_len, (unsigned) chunk_size);
        return FALSE;
    }
    if (pc > G_MAXUINT16) {
        debug_log ("media",
                   "upload: payload %zu would need %" G_GUINT64_FORMAT
                   " chunks at chunk_size=%u — exceeds u16 part_count",
                   payload_len, pc, (unsigned) chunk_size);
        return FALSE;
    }
    guint16 part_count = (guint16) pc;

    /* Build chunk 0. */
    const guint8 *first_slice = NULL;
    gsize first_slice_len = 0;
    hx_upload_ctx probe; /* stack scratch for upload_chunk_slice on the
                            same shape we're about to allocate */
    probe.payload = (guint8 *) payload;
    probe.payload_len = payload_len;
    probe.chunk_size = chunk_size;
    upload_chunk_slice (&probe, 0, &first_slice, &first_slice_len);

    struct hx_chunk chunks[5];
    guint8 scratch[5];
    int32_t hc = gtkhx_proto_build_upload_media_first_chunks (
        first_slice, first_slice_len,
        (const uint8_t *) declared_type, declared_type_len, part_count,
        chunks, G_N_ELEMENTS (chunks), scratch, sizeof (scratch));
    if (hc <= 0) {
        debug_log ("media",
                   "upload: first-chunk builder rejected (slice=%zu, "
                   "declared_type_len=%zu, part_count=%u)",
                   first_slice_len, declared_type_len,
                   (unsigned) part_count);
        return FALSE;
    }

    /* Allocate the upload ctx, copy in everything the state
	 * machine needs to refer back to across replies. */
    hx_upload_ctx *ctx = g_new0 (hx_upload_ctx, 1);
    ctx->on_done = on_done;
    ctx->user_data = user_data;
    ctx->user_data_free = user_data_free;
    ctx->payload = g_memdup2 (payload, payload_len);
    ctx->payload_len = payload_len;
    if (declared_type && declared_type_len > 0) {
        ctx->declared_type = g_strndup (declared_type, declared_type_len);
        ctx->declared_type_len = declared_type_len;
    }
    ctx->chunk_size = chunk_size;
    ctx->part_count = part_count;
    ctx->next_part_index = 1; /* chunk 0 is the one we're about to send */

    struct task *tsk = task_new (
        htlc, RCV_TASK_FN (rcv_task_upload_media), ctx, NULL, "upload-media");
    if (!tsk) {
        /* Synchronous reject — task_new couldn't register the rcv
		 * hook (no session, OOM via GLib's fatal path, etc.). We
		 * never wrote to the wire; treat this as a clean
		 * failed-to-send and reclaim ctx without invoking
		 * user_data_free (the caller still owns user_data — they
		 * see a FALSE return rather than a callback). */
        debug_log ("media",
                   "upload: task_new failed for chunked first chunk; "
                   "synchronous reject");
        ctx->user_data = NULL;
        ctx->user_data_free = NULL;
        upload_ctx_free (ctx);
        return FALSE;
    }
    tsk->ptr_free = (GDestroyNotify) upload_ctx_free;
    hlwrite_chunks (htlc, HTLC_HDR_UPLOAD_MEDIA, 0, chunks, hc);

    debug_log ("media",
               "→ UPLOAD_MEDIA chunked: payload_len=%zu chunk_size=%u "
               "part_count=%u declared_type_len=%zu",
               payload_len, (unsigned) chunk_size, (unsigned) part_count,
               declared_type_len);

    return TRUE;
}

gboolean
hx_send_upload_media (struct htlc_conn *htlc, const guint8 *payload,
                      gsize payload_len, const char *declared_type,
                      gsize declared_type_len,
                      HxInlineMediaUploadCallback on_done, gpointer user_data,
                      GDestroyNotify user_data_free)
{
    if (!inline_media_cap_ok (htlc)) {
        return FALSE;
    }
    if (!payload || payload_len == 0) {
        debug_log ("media", "upload: empty payload rejected");
        return FALSE;
    }

    /* Pick framing: payload fits in one chunk → single-shot;
	 * otherwise drive the chunked state machine. The single-shot
	 * cap is the smaller of (server CHAT_MEDIA_CHUNK_SIZE, u16
	 * wire ceiling). */
    guint32 chunk_size = inline_media_chunk_size (htlc);
    if (chunk_size > 65535) {
        chunk_size = 65535;
    }
    if (payload_len <= (gsize) chunk_size && payload_len <= 65535) {
        return hx_send_upload_media_single (
            htlc, payload, payload_len, declared_type, declared_type_len,
            on_done, user_data, user_data_free);
    }
    return hx_send_upload_media_chunked (
        htlc, payload, payload_len, declared_type, declared_type_len,
        on_done, user_data, user_data_free);
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
    if (task_error_extract (htlc->in.buf, htlc->in.pos, err_buf, sizeof (err_buf), &err_len)) {
        r.error_message = err_buf;
        r.error_message_len = err_len;
    }

    debug_log ("media",
               "← UPLOAD_MEDIA error_code=%u (msg='%.*s')",
               (unsigned) r.error_code,
               (int) (r.error_message_len > 256 ? 256 : r.error_message_len),
               r.error_message ? r.error_message : "");

    /* Normal completion. user_data_free fires ONLY on cancellation
	 * / disconnect — flip callback_fired here regardless of whether
	 * the caller registered an on_done handler. A caller passing
	 * NULL on_done is signalling "I don't need notification" and
	 * accepts that their user_data persists past completion (they
	 * must arrange to free it themselves, or skip user_data_free
	 * entirely). See the upload-callback contract in
	 * inline_media_upload.h. */
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

/* Synthesise a generic failure reply (no error_code, custom
 * message). Used when chunked-upload state machine hits an
 * unrecoverable client-side condition (missing token in the first
 * reply, builder rejects a follow-up, etc.) and needs to surface
 * something useful to the caller's on_done. */
static void
deliver_synthetic_failure (struct htlc_conn *htlc, hx_upload_ctx *ctx,
                           const char *message)
{
    HxInlineMediaUploadResult r;
    memset (&r, 0, sizeof (r));
    r.error_code = 0;
    r.error_message = message;
    r.error_message_len = strlen (message);
    /* Synthetic failure is still a normal completion (see
	 * deliver_failure's comment for the contract). Mark fired
	 * regardless of on_done. */
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
        return;
    }

    /* Single-shot path: payload not stored on the ctx, so this is
	 * the only reply we'll see — must be the final reply. */
    if (!ctx->payload) {
        deliver_success (htlc, ctx);
        return;
    }

    /* Chunked path. We've just sent chunk (next_part_index - 1).
	 * If that was the final chunk, the server's reply carries
	 * CHAT_MEDIA_ID and we're done. Otherwise the reply is
	 * intermediate and (on the first reply at least) carries the
	 * UPLOAD_TOKEN we need to echo on every follow-up. */
    if ((guint32) ctx->next_part_index >= (guint32) ctx->part_count) {
        deliver_success (htlc, ctx);
        return;
    }

    /* Intermediate reply. The first reply (after we sent chunk 0,
	 * so next_part_index == 1) MUST carry the upload token; the
	 * spec lets the server echo it on later replies too, so we
	 * accept and refresh whenever the chunk is present.
	 *
	 * Bound the token at HX_MEDIA_MAX_UPLOAD_TOKEN bytes (well above
	 * the spec's ≤ 64 byte ceiling, with headroom for benign spec
	 * evolution). Without the bound, a malicious server could hand
	 * us a 64 KiB token and force a fresh allocation on every chunk
	 * reply — and an even larger one on every follow-up we
	 * re-echo it on. */
    const guint8 *tok_ptr = NULL;
    size_t tok_len = 0;
    if (gtkhx_proto_parse_upload_token_reply (htlc->in.buf, htlc->in.pos,
                                              &tok_ptr, &tok_len)
        && tok_len > 0) {
        if (tok_len > HX_MEDIA_MAX_UPLOAD_TOKEN) {
            debug_log ("media",
                       "← UPLOAD_MEDIA intermediate: token_len=%zu exceeds "
                       "ceiling %u — bailing",
                       tok_len, (unsigned) HX_MEDIA_MAX_UPLOAD_TOKEN);
            deliver_synthetic_failure (
                htlc, ctx, "Upload reply token oversized");
            return;
        }
        g_free (ctx->upload_token);
        ctx->upload_token = g_memdup2 (tok_ptr, tok_len);
        ctx->upload_token_len = tok_len;
        debug_log ("media",
                   "← UPLOAD_MEDIA intermediate: token_len=%zu (after chunk "
                   "%u/%u)",
                   tok_len, (unsigned) (ctx->next_part_index - 1),
                   (unsigned) ctx->part_count);
    }
    if (!ctx->upload_token) {
        debug_log ("media",
                   "← UPLOAD_MEDIA intermediate without UPLOAD_TOKEN "
                   "(next_part_index=%u) — bailing",
                   (unsigned) ctx->next_part_index);
        deliver_synthetic_failure (
            htlc, ctx, "Upload reply missing session token");
        return;
    }

    /* Hand ctx ownership off to the next task entry. rcv.c will
	 * fire task_delete on the current entry after we return; if
	 * we don't clear its ptr, task_free → ptr_free → upload_ctx_free
	 * would reclaim ctx before the follow-up reply lands.
	 *
	 * Both lookups MUST succeed before we register the follow-up
	 * task — if trans extraction or the table lookup fails, the
	 * current task still owns ctx via its ptr_free hook AND
	 * send_next_chunk would register a SECOND owner of the same
	 * ctx on the new entry. That double ownership is a UAF /
	 * double-free waiting for one of the task_delete sweeps to
	 * fire. Fail closed instead. */
    guint32 cur_trans = 0;
    struct task *cur = NULL;
    if (gtkhx_proto_header_trans (htlc->in.buf, htlc->in.pos, &cur_trans)) {
        cur = task_with_trans (sess_from_htlc (htlc), cur_trans);
    }
    if (!cur) {
        debug_log ("media",
                   "upload: cannot identify current task for chunked "
                   "follow-up (trans=%u) — bailing",
                   (unsigned) cur_trans);
        deliver_synthetic_failure (
            htlc, ctx, "Chunked upload: lost task entry mid-stream");
        return;
    }
    cur->ptr = NULL;

    if (!send_next_chunk (htlc, ctx)) {
        /* Builder / state-machine refused the follow-up. Restore
		 * the current task's ptr so the standard ptr_free chain
		 * reclaims ctx — we cleared it above expecting
		 * send_next_chunk to take ownership. */
        cur->ptr = ctx;
        deliver_synthetic_failure (htlc, ctx, "Chunked upload resend failed");
        return;
    }
    /* ctx now lives under the next task entry; the current
	 * entry's task_delete is about to fire harmlessly with
	 * ptr=NULL. */
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

    gboolean utf8 = (hx_conn_has_cap (htlc, HTLC_CAP_TEXT_ENCODING)) != 0;
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
    if (have_media && !(hx_conn_has_cap (htlc, HTLC_CAP_INLINE_MEDIA))) {
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
