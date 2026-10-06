/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Inline-media upload and the chat line that carries the picture. Both are
 * Rust (hxhandlers' media.rs and send/chat.rs).
 */

#ifndef HX_INLINE_MEDIA_UPLOAD_H
#define HX_INLINE_MEDIA_UPLOAD_H 1

#include <glib.h>

#include "protocol.h"

/* How an upload ended. Every pointer is borrowed for the callback's
 * duration. On success media_id and media_type are set and error_code is
 * 0; on failure they are NULL, error_code is the spec's MediaErrorCode
 * (0 generic, 1 too large, 2 unsupported, 3 rate limited, 4 not
 * authorized, 5 busy) and error_message the server's reason, if any. */
typedef struct {
    const guint8 *media_id;
    gsize media_id_len;
    const char *media_type;
    gsize media_type_len;
    guint32 width;
    guint32 height;
    guint32 bytes;
    gboolean width_present;
    gboolean height_present;
    gboolean bytes_present;
    guint16 error_code;
    const char *error_message;
    gsize error_message_len;
} HxInlineMediaUploadResult;

typedef void (*HxInlineMediaUploadCallback) (
    struct htlc_conn *htlc, const HxInlineMediaUploadResult *result,
    gpointer user_data);

/* Send a picture, whole or in parts as the server's part size needs.
 * FALSE when nothing was sent (no inline media agreed, an empty payload, or
 * too many parts), and on_done is not called. Otherwise on_done runs once,
 * after which user_data is the caller's; only if the connection goes first
 * is it handed to user_data_free. */
extern gboolean hx_send_upload_media (struct htlc_conn *htlc,
                                      const guint8 *payload, gsize payload_len,
                                      const char *declared_type,
                                      gsize declared_type_len,
                                      HxInlineMediaUploadCallback on_done,
                                      gpointer user_data,
                                      GDestroyNotify user_data_free);

/* A chat line carrying the picture an upload gave the handle and type of;
 * a plain line without both, or where the server agreed to no inline
 * media. */
extern void hx_send_chat_with_media (struct htlc_conn *htlc, const char *str,
                                     guint32 cid, guint16 style,
                                     const guint8 *media_id, gsize media_id_len,
                                     const char *mime, gsize mime_len);

#endif /* HX_INLINE_MEDIA_UPLOAD_H */
