/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Inline-media extension send-side glue (Phase 9.C).
 *
 * Three send entry points wire the Phase 9.A wire builders into
 * the live connection:
 *
 *   hx_send_upload_media — dispatcher. Picks single-shot when
 *     the payload fits in one chunk, otherwise drives the
 *     chunked-upload state machine. This is the entry point
 *     production callers should use; the lower-level
 *     _single / _chunked siblings exist for tests and for
 *     callers that want to force a specific framing.
 *
 *   hx_send_upload_media_single — single-shot TranUploadMedia
 *     (750). Payload must fit in one chunk (≤ u16::MAX bytes
 *     after the wire framing overhead).
 *
 *   hx_send_upload_media_chunked — chunked TranUploadMedia.
 *     Caller hands the whole payload; the helper splits it into
 *     server-advertised CHAT_MEDIA_CHUNK_SIZE pieces, sends them
 *     one at a time, echoes the upload session token from the
 *     first reply onto every follow-up, and fires the same
 *     completion callback shape as single-shot when the final
 *     reply lands.
 *
 *   hx_send_chat_with_media — TranChatSend (105) with optional
 *     CHAT_MEDIA_ID + CHAT_MEDIA_TYPE companion chunks attached.
 *     Mirrors hx_send_chat's encoder path (Mac Roman / UTF-8
 *     negotiation) but emits a 4- or 5-chunk TranChatSend
 *     instead of the bare 2/3-chunk variant when media is
 *     attached.
 *
 * The cap gate (HTLC_CAP_INLINE_MEDIA) is enforced inside every
 * helper; the production UI never has to gate its own click
 * handler. Specific reject behaviour differs by helper:
 *
 *   hx_send_upload_media / _single / _chunked — return FALSE for
 *     NULL htlc, missing cap, empty payload, oversized payload
 *     (each helper enforces its own ceiling), or a builder
 *     validation failure. The cap-missing / payload-validation
 *     paths additionally emit a debug_log("media", …) line;
 *     NULL htlc is dropped silently by inline_media_cap_ok.
 *
 *   hx_send_chat_with_media — void return. NULL htlc, NULL
 *     body, or any media validation failure (oversized id_len /
 *     mime_len, cap not negotiated) drops the companion fields
 *     and falls back to a plain chat send; the cap-drop and
 *     wire-length rejects log under category 'media'. NULL ev
 *     and NULL str return early without sending.
 */

#ifndef HX_INLINE_MEDIA_UPLOAD_H
#define HX_INLINE_MEDIA_UPLOAD_H 1

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

/* ---- Upload completion ---- */

/* Result handed to the upload callback.
 *
 * Lifetime: every pointer field below is borrowed and valid ONLY
 * for the duration of the callback. The helper reuses caller-
 * external storage for each field (htlc->in.buf for media_id,
 * stack buffers in rcv_task_upload_media for media_type and
 * error_message); none of them are heap-owned, and none are
 * freed by the helper afterwards. Callers that need to retain
 * any field across the callback MUST copy out (e.g. g_memdup2
 * the handle bytes, g_strdup the mime + error message).
 *
 * On success error_code == 0, media_id + media_type are
 * non-NULL, and error_message is NULL.
 *
 * On failure media_id + media_type are NULL, error_code is the
 * spec MediaErrorCode wire value (0 generic, 1 too-large, 2
 * unsupported, 3 rate-limited, 4 not-authorized, 5 server-busy),
 * and error_message may be NULL or point at the server's
 * DATA_ERROR text. */
typedef struct {
    /* Server-issued opaque handle. NULL on failure. Borrows into
     * htlc->in.buf; only valid during the callback. */
    const guint8 *media_id;
    gsize media_id_len;

    /* Server-canonical MIME (NUL-terminated). NULL on failure.
     * Borrows a transient buffer; only valid during the callback. */
    const char *media_type;
    gsize media_type_len;

    /* Server-canonical image metadata. 0 / FALSE on failure or
     * when the server omitted the field (advisory only). */
    guint32 width;
    guint32 height;
    guint32 bytes;
    gboolean width_present;
    gboolean height_present;
    gboolean bytes_present;

    /* Error reporting. Only meaningful when error_code != 0 OR
     * the success fields above are NULL. error_message may be
     * NULL when the server didn't include DATA_ERROR; when set,
     * it borrows a transient buffer (NOT htlc->in.buf — the
     * helper copies the text out under sanitisation before
     * invoking the callback). Only valid during the callback. */
    guint16 error_code;
    const char *error_message;
    gsize error_message_len;
} HxInlineMediaUploadResult;

/* Callback fired on the main thread when the TASK reply for a
 * TranUploadMedia lands. user_data is whatever the caller passed
 * to hx_send_upload_media_single. The pointers inside `result`
 * are valid only for the duration of the callback. */
typedef void (*HxInlineMediaUploadCallback) (
    struct htlc_conn *htlc, const HxInlineMediaUploadResult *result,
    gpointer user_data);

/* Send a single-shot TranUploadMedia request.
 *
 * Validates cap (HTLC_CAP_INLINE_MEDIA echoed) + payload (non-
 * empty, fits in one chunk — payload_len ≤ u16::MAX). Returns
 * TRUE on success (request queued, callback will eventually fire),
 * FALSE on a synchronous validation reject. On FALSE, on_done is
 * NOT called; the caller never sees the upload pipeline.
 *
 * declared_type is the sender's MIME hint; the server treats it
 * as advisory only — magic-byte sniff is authoritative. NULL with
 * declared_type_len=0 is fine.
 *
 * user_data is opaque; passed to on_done as-is. The helper
 * never frees user_data on a NORMAL completion (success,
 * failure, or synthetic-failure delivery). user_data_free is
 * only invoked when the upload is CANCELLED before delivery
 * can run — most importantly, when the connection is torn down
 * and sess->tasks is cleared, every still-in-flight upload's
 * ptr_free hook fires upload_ctx_free which in turn invokes
 * user_data_free on contexts that never reached a deliver_*
 * callsite.
 *
 * Caller responsibilities by handler shape:
 *   - on_done non-NULL: the callback claims ownership of
 *     user_data; free it (and your per-upload heap state) in
 *     the callback regardless of success / failure. The helper
 *     does not touch user_data on the normal-completion path.
 *   - on_done NULL: fire-and-forget. The caller MUST either
 *     pass NULL user_data + NULL user_data_free (no per-upload
 *     state to manage), or accept that user_data outlives the
 *     upload on normal completion — the helper has no callback
 *     to hand it off to, so user_data_free does NOT run on
 *     completion. Only the cancellation / disconnect path
 *     invokes user_data_free.
 *
 * Production state machine: hx_send_upload_media_single allocates
 * a per-upload context, registers task_new(rcv_task_upload_media)
 * keyed on the TASK trans id, then calls hlwrite_chunks. When the
 * TASK reply lands, rcv.c dispatches to rcv_task_upload_media,
 * which parses the reply (success → final-reply parser; failure
 * → task_error_extract + error-code extractor) and invokes
 * on_done. The upload-helper context is reclaimed via the task
 * table's ptr_free hook (task_free → upload_ctx_free), which also
 * runs on disconnect-time g_hash_table_remove_all. */
extern gboolean
hx_send_upload_media_single (struct htlc_conn *htlc, const guint8 *payload,
                             gsize payload_len, const char *declared_type,
                             gsize declared_type_len,
                             HxInlineMediaUploadCallback on_done,
                             gpointer user_data, GDestroyNotify user_data_free);

/* Send a chunked TranUploadMedia request.
 *
 * Drives the multi-chunk send-then-receive state machine: chunk 0
 * goes out with CHAT_MEDIA_PART_INDEX/COUNT and the optional
 * declared MIME hint; the server's intermediate reply carries an
 * UPLOAD_TOKEN; every follow-up echoes that token; the final
 * chunk sets PART_FINAL = 1 and the server's final reply carries
 * the canonical handle + MIME (same shape as single-shot's reply).
 *
 * Validates cap, payload non-empty, and that the resulting
 * part_count fits in u16 (i.e. payload doesn't need more chunks
 * than the spec allows). Returns FALSE on synchronous validation
 * reject without invoking on_done.
 *
 * payload is copied into the upload context — the caller's buffer
 * doesn't need to outlive this call. Each chunk slice is sent
 * straight from the ctx's owned copy.
 *
 * user_data + user_data_free semantics match _single. */
extern gboolean hx_send_upload_media_chunked (
    struct htlc_conn *htlc, const guint8 *payload, gsize payload_len,
    const char *declared_type, gsize declared_type_len,
    HxInlineMediaUploadCallback on_done, gpointer user_data,
    GDestroyNotify user_data_free);

/* Dispatcher: chooses single-shot vs chunked based on payload
 * size against the effective per-chunk cap (the smaller of the
 * server-advertised CHAT_MEDIA_CHUNK_SIZE and the 16-bit wire-
 * framing ceiling). Production callers should use this entry
 * point; single-shot and chunked siblings stay public for tests
 * and for callers wanting explicit framing. */
extern gboolean hx_send_upload_media (struct htlc_conn *htlc,
                                      const guint8 *payload, gsize payload_len,
                                      const char *declared_type,
                                      gsize declared_type_len,
                                      HxInlineMediaUploadCallback on_done,
                                      gpointer user_data,
                                      GDestroyNotify user_data_free);

/* ---- Chat with attached media ---- */

/* Send a TranChatSend (105) with the standard style + body
 * chunks PLUS optional CHAT_MEDIA_ID + CHAT_MEDIA_TYPE companion
 * chunks attached. When media_id is NULL or media_id_len is 0
 * (or mime is NULL or mime_len is 0), the helper falls back to
 * hx_send_chat semantics — same chunk layout, no media
 * companions. This is the production glue between
 * hx_send_upload_media_single's callback and the live chat send.
 *
 * Cap gate is enforced inside; calling with media attached but
 * the cap not negotiated drops the media and sends plain text.
 * (The spec says servers strip media fields from senders that
 * didn't negotiate the cap; we save the round-trip.)
 *
 * Style values:
 *   0 — normal chat
 *   1 — emote (/me) line
 */
extern void hx_send_chat_with_media (struct htlc_conn *htlc, const char *str,
                                     guint32 cid, guint16 style,
                                     const guint8 *media_id, gsize media_id_len,
                                     const char *mime, gsize mime_len);

/* TASK-reply handler for HTLC_HDR_UPLOAD_MEDIA. Hooked into
 * rcv.c's task table via task_new(...) inside
 * hx_send_upload_media_single. Not called directly. */
extern void rcv_task_upload_media (struct htlc_conn *htlc, const guint8 *frame,
                                   gsize frame_len, void *ctx_ptr,
                                   void *unused);

#endif /* HX_INLINE_MEDIA_UPLOAD_H */
