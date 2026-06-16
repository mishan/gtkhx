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
 * handler. NULL htlc / empty payload / oversized payload all
 * surface as a return-value reject and a debug_log line under
 * the "media" category.
 */

#ifndef HX_INLINE_MEDIA_UPLOAD_H
#define HX_INLINE_MEDIA_UPLOAD_H 1

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

/* ---- Upload completion ---- */

/* Result handed to the upload callback. On success error_code is
 * 0 and media_id / media_type point at owned buffers the callback
 * MAY take ownership of by g_strndup-ing or by setting the
 * `take_ownership_*` flag (TBD — Phase 9.C2). Today they are
 * borrowed and the helper g_free's them after the callback
 * returns; callers that need to retain the handle MUST copy out.
 *
 * On failure media_id / media_type are NULL and error_code is the
 * spec MediaErrorCode wire value (0 generic, 1 too-large, 2
 * unsupported, 3 rate-limited, 4 not-authorized, 5 server-busy).
 * error_message is a borrowed pointer to the server's
 * DATA_ERROR text — owned by the htlc->in buffer and only valid
 * for the duration of the callback. */
typedef struct {
    /* Server-issued opaque handle, length bytes. NULL on failure. */
    const guint8 *media_id;
    gsize media_id_len;

    /* Server-canonical MIME (NUL-terminated). NULL on failure. */
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

    /* Error reporting (only meaningful when error_code != 0 OR
	 * the success fields above are NULL). */
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
 * user_data is opaque; passed to on_done as-is. The helper does
 * NOT free it.
 *
 * Production state machine: hx_send_upload_media_single allocates
 * a per-upload context, registers task_new(rcv_task_upload_media)
 * keyed on the TASK trans id, then calls hlwrite_chunks. When the
 * TASK reply lands, rcv.c dispatches to rcv_task_upload_media,
 * which parses the reply (success → final-reply parser; failure
 * → task_error_extract + error-code extractor), invokes
 * on_done, and frees the context. */
extern gboolean hx_send_upload_media_single (
    struct htlc_conn *htlc,
    const guint8 *payload, gsize payload_len,
    const char *declared_type, gsize declared_type_len,
    HxInlineMediaUploadCallback on_done,
    gpointer user_data);

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
