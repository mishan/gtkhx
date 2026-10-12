/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * The inline-media paperclip beside a chat's input, from gtkhx-ui's
 * `media_attach`: picks a picture, uploads it and sends its handle on a
 * chat line. Shown only while the connection has inline media.
 */

#ifndef HX_INLINE_MEDIA_ATTACH_H
#define HX_INLINE_MEDIA_ATTACH_H 1

#include <gtk/gtk.h>

#include "protocol.h"

struct gtkhx_chat;

/* The paperclip for `gchat`'s input row; `gchat` and `htlc` must outlive
 * it. */
extern GtkWidget *hx_inline_media_attach_button_new (struct gtkhx_chat *gchat,
                                                     struct htlc_conn *htlc);

/* Show or hide every paperclip of `htlc` as it has inline media or not. */
extern void inline_media_attach_refresh (struct htlc_conn *htlc);

#endif /* HX_INLINE_MEDIA_ATTACH_H */
