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
 * Two entry points wire the Phase 9.A wire builders into the live
 * connection:
 *
 *   hx_send_upload_media_single — single-shot TranUploadMedia (750).
 *     Caller hands raw bytes + optional declared MIME hint + a
 *     completion callback; the helper registers a task waiting on
 *     the server's reply, builds + sends the upload chunk, and
 *     fires the callback when the TASK reply lands. Single-shot
 *     only; chunked-upload follow-up tracked separately.
 *
 *   hx_send_chat_with_media — TranChatSend (105) with optional
 *     CHAT_MEDIA_ID + CHAT_MEDIA_TYPE companion chunks attached.
 *     Mirrors hx_send_chat's encoder path (Mac Roman / UTF-8
 *     negotiation) but emits a 4- or 5-chunk TranChatSend
 *     instead of the bare 2/3-chunk variant when media is
 *     attached.
 *
 * The cap gate (HTLC_CAP_INLINE_MEDIA) is enforced inside both
 * helpers; the production UI never has to gate its own click
 * handler. Specific reject behaviour differs by helper:
 *
 *   hx_send_upload_media_single — returns FALSE for NULL htlc,
 *     missing cap, empty payload, oversized payload (> u16::MAX
 *     single-shot ceiling), or a builder validation failure.
 *     The cap-missing / payload-validation paths additionally
 *     emit a debug_log("media", …) line; NULL htlc is dropped
 *     silently by inline_media_cap_ok.
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
 * user_data is opaque; passed to on_done as-is. When on_done
 * fires (success OR failure path), the helper does NOT free
 * user_data — the callback claims ownership. When the upload
 * is cancelled before on_done can fire — most importantly, when
 * the connection is torn down and sess->tasks is cleared — the
 * helper invokes user_data_free (if non-NULL) so the caller's
 * own per-upload context is reclaimed at the same time as the
 * internal upload-helper context. Pass NULL for user_data_free
 * when user_data has no heap ownership semantics.
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
extern gboolean hx_send_upload_media_single (
    struct htlc_conn *htlc,
    const guint8 *payload, gsize payload_len,
    const char *declared_type, gsize declared_type_len,
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
extern void hx_send_chat_with_media (
    struct htlc_conn *htlc, const char *str, guint32 cid, guint16 style,
    const guint8 *media_id, gsize media_id_len,
    const char *mime, gsize mime_len);

/* TASK-reply handler for HTLC_HDR_UPLOAD_MEDIA. Hooked into
 * rcv.c's task table via task_new(...) inside
 * hx_send_upload_media_single. Not called directly. */
extern void rcv_task_upload_media (struct htlc_conn *htlc, void *ctx_ptr,
                                   void *unused);

#endif /* HX_INLINE_MEDIA_UPLOAD_H */
