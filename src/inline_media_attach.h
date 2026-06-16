/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Inline-media attach UI (Phase 9.C UI).
 *
 * Builds the paperclip button that lives next to the per-chat
 * input row, plus the click → file-dialog → load → pre-flight
 * decode → upload → chat-send-with-handle flow that fires when
 * the user picks an image.
 *
 * The button is cap-gated by visibility: shown only when
 * HTLC_CAP_INLINE_MEDIA is negotiated for the current session,
 * hidden otherwise. inline_media_attach_refresh_all_chats() runs
 * from setbtns() to flip every existing button on/off across the
 * disconnect / reconnect cycle, and hx_inline_media_attach_button_new
 * additionally sets the right initial state at creation time so
 * pchat windows opened LATER in the session don't stay hidden
 * waiting for the next refresh tick. A defensive cap check still
 * lives in the click handler in case the cap disappears between
 * paint and click.
 *
 * The upload runs against the Phase 9.A wire-protocol stack and
 * the Phase 9.C send-state-machine helpers; the result is
 * attached to a subsequent TranChatSend via
 * hx_send_chat_with_media so capable recipients see the
 * companion fields. Incapable recipients see the body text
 * fallback the helper sends — '[image]' today (mirroring the
 * spec-suggested placeholder); a future compose-preview UX
 * could let the user type a caption alongside the attachment.
 */

#ifndef HX_INLINE_MEDIA_ATTACH_H
#define HX_INLINE_MEDIA_ATTACH_H 1

#include <gtk/gtk.h>

#include "protocol.h"

struct gtkhx_chat;
struct _session;

/* Build a 'paperclip' attach-image button for the chat input
 * row. The gchat + htlc pointers are stored borrowed (no ref —
 * gtkhx_chat is a plain struct, not refcounted); both must
 * outlive the button. Caller keeps ownership.
 *
 * Initial visibility is computed from
 * htlc->caps & HTLC_CAP_INLINE_MEDIA at button-creation time so
 * pchats opened after caps have already landed start in the
 * right state without waiting for the next setbtns→refresh
 * tick. The refresh helper still walks every gchat from
 * setbtns to handle the disconnect / reconnect cycle.
 *
 * Caller should stash the returned pointer on
 * gchat->media_attach_btn so the refresh helper can find it,
 * and gtk_box_append it alongside the emoji button. The button
 * connects its own clicked handler. */
extern GtkWidget *hx_inline_media_attach_button_new (
    struct gtkhx_chat *gchat, struct htlc_conn *htlc);

/* Walk every gchat in the session and refresh each attach
 * button's visibility against the current
 * htlc->caps & HTLC_CAP_INLINE_MEDIA. Called from setbtns()
 * alongside the voice-panel refresh, so the button appears
 * once SELFINFO + LOGIN-reply caps have landed and disappears
 * on disconnect (network.c clears htlc->caps in hx_htlc_close).
 *
 * Safe to call with NULL sess or before any gchats exist
 * (e.g. during early startup). */
extern void inline_media_attach_refresh_all_chats (struct _session *sess);

#endif /* HX_INLINE_MEDIA_ATTACH_H */
