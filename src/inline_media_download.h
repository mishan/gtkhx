/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * TranDownloadMedia state machine (Phase 9.D dialog wiring).
 *
 * Drives the download of a previously-announced media handle:
 *
 *   1. inline_media_download_start packages a per-download
 *      context with the handle bytes + callback + user_data,
 *      registers task_new (rcv_task_download_media), and emits
 *      the first TranDownloadMedia request (handle only, no
 *      part_index).
 *
 *   2. The TASK reply lands in rcv_task_download_media. On
 *      success, the helper parses the reply via
 *      gtkhx_proto_parse_download_reply, appends the chunk
 *      payload to the in-context GByteArray, and either:
 *        - final_chunk set: invokes the callback with the
 *          accumulated bytes + canonical mime, frees context.
 *        - else: re-registers task_new and sends the next
 *          TranDownloadMedia with PART_INDEX = previous+1.
 *
 *   3. On task error, the callback fires once with NULL bytes
 *      and the spec MediaErrorCode (0..=5) — same shape as the
 *      Phase 9.C upload result.
 *
 * The dialog (inline_media_dialog.c) owns the per-download
 * lifecycle: it constructs the download, holds the context's
 * cancel flag, and frees the result bytes when the dialog
 * closes.
 *
 * Cap gate (HTLC_CAP_INLINE_MEDIA) is enforced inside the
 * helper, same as the upload path. Empty / oversized handles
 * are synchronous rejects.
 */

#ifndef HX_INLINE_MEDIA_DOWNLOAD_H
#define HX_INLINE_MEDIA_DOWNLOAD_H 1

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

/* Opaque handle the dialog passes back for cancel. NULL is
 * always safe to pass to inline_media_download_cancel — the
 * helper treats it as a no-op. */
typedef struct hx_inline_media_download hx_inline_media_download;

/* Result delivered to the dialog when the download completes
 * (success or failure). On success bytes is non-NULL with the
 * full canonical payload; canonical_mime is a borrowed pointer
 * to the in-context heap copy (valid for the duration of the
 * callback). On failure bytes is NULL and error_code is the
 * spec MediaErrorCode (0..=5). */
typedef struct {
    /* Accumulated canonical bytes. NULL on failure. The pointer
	 * is borrowed and only valid for the duration of the
	 * callback — copy out (or grab a ref via g_byte_array_ref
	 * before the helper g_byte_array_unref's its copy) if
	 * needed afterwards. */
    GByteArray *bytes;
    /* Canonical MIME (NUL-terminated). NULL on failure. Borrowed
	 * for the callback's duration. */
    const char *canonical_mime;
    /* Spec MediaErrorCode wire value. 0 on success. */
    guint16 error_code;
    /* Best-effort error text borrowed from htlc->in.buf. NULL on
	 * success. */
    const char *error_message;
    gsize error_message_len;
} HxInlineMediaDownloadResult;

/* Callback shape. Runs on the main thread (task dispatch is
 * main-only). user_data is whatever the caller handed in. */
typedef void (*HxInlineMediaDownloadCallback) (
    struct htlc_conn *htlc, const HxInlineMediaDownloadResult *result,
    gpointer user_data);

/* Kick off a download. Returns a context the caller can pass to
 * inline_media_download_cancel; NULL on synchronous reject
 * (NULL htlc, missing cap, empty/oversize handle).
 *
 * The returned pointer is owned by the helper and freed when
 * the callback fires (success or failure). DO NOT free it
 * yourself — only use it for cancel. */
extern hx_inline_media_download *inline_media_download_start (
    struct htlc_conn *htlc,
    const guint8 *handle, gsize handle_len,
    HxInlineMediaDownloadCallback on_done,
    gpointer user_data);

/* Cancel an in-flight download. The callback will NOT fire after
 * this call returns; the context is invalidated. Safe to call
 * with NULL. */
extern void inline_media_download_cancel (hx_inline_media_download *dl);

/* TASK-reply handler hooked through task_new from
 * inline_media_download_start. Not called directly. */
extern void rcv_task_download_media (struct htlc_conn *htlc, void *ctx_ptr,
                                     void *unused);

#endif /* HX_INLINE_MEDIA_DOWNLOAD_H */
