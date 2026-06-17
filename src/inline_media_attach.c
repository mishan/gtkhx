/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * Inline-media attach flow (Phase 9.C UI).
 *
 * Click → GtkFileDialog → g_file_load_bytes_async → pre-flight
 * (magic-byte sniff + size cap against server-advertised limits)
 * → hx_send_upload_media_single → on success
 *   hx_send_chat_with_media to the chat's cid.
 *
 * The per-flow context is heap-allocated; lifetime crosses
 * three async boundaries (file dialog response, file load,
 * upload TASK reply). Each callback either reschedules the next
 * step or frees the context on completion.
 *
 * Feedback to the user is via toolbar_show_toast (libadwaita
 * AdwToast). The error mapping is the same set as the dialog's
 * (Phase 9.D): translates MediaErrorCode to actionable text.
 */

#include "config.h"
#include <adwaita.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <string.h>

#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "hx.h"
#include "session.h"
#include "toolbar.h"
#include "inline_media.h"
#include "inline_media_attach.h"
#include "inline_media_decode.h"  /* sniff + format_is_allowed */
#include "inline_media_upload.h"
#include "debug.h"

/* Per-attach state. Allocated on click. Reclaimed:
 *   - when on_upload_done fires (success or failure),
 *   - when an earlier step synchronously rejects,
 *   - when the underlying connection drops while the upload
 *     task is still in flight (hx_send_upload_media_single
 *     gets attach_ctx_free as user_data_free; the upload
 *     helper's ptr_free chain invokes it from the task
 *     table's g_hash_table_remove_all sweep).
 *
 * The third bullet is what closes the previous leak: pre-this
 * change, on_upload_done never fired and the attach ctx (plus
 * its display_name string) lived until process exit. */
typedef struct {
    struct gtkhx_chat *gchat;
    struct htlc_conn *htlc;
    guint32 cid;
    /* Cached basename of the picked file, used in error toasts. */
    char *display_name;
} hx_attach_ctx;

static void
attach_ctx_free (hx_attach_ctx *ctx)
{
    if (!ctx) {
        return;
    }
    g_free (ctx->display_name);
    g_free (ctx);
}

/* Effective per-upload byte cap. The server's
 * CHAT_MEDIA_MAX_BYTES is the authoritative ceiling; we don't
 * impose a wire-framing cap on top of it any more because
 * hx_send_upload_media dispatches to the chunked state machine
 * when the payload exceeds a single chunk. */
static guint32
effective_max_bytes (struct htlc_conn *htlc)
{
    return inline_media_max_bytes (htlc);
}

/* Format-conditional pre-flight: size cap against the
 * effective max (see above), then magic-byte sniff. Returns
 * TRUE on pass; FALSE on reject (and the reject path emits a
 * toast describing why). */
static gboolean
preflight_check (struct htlc_conn *htlc, GBytes *bytes,
                 const char *display_name)
{
    gsize len = 0;
    const guchar *data = g_bytes_get_data (bytes, &len);

    if (len > effective_max_bytes (htlc)) {
        char *msg = g_strdup_printf (
            _ ("%s is %.1f KB — over the %u KB cap. "
               "Pick a smaller image."),
            display_name ? display_name : _ ("image"),
            (double) len / 1024.0,
            (unsigned) (effective_max_bytes (htlc) / 1024));
        toolbar_show_toast (msg);
        g_free (msg);
        return FALSE;
    }

    HxInlineMediaFormat fmt = inline_media_sniff (data, len);
    if (!inline_media_format_is_allowed (fmt)) {
        const char *kind = inline_media_format_to_mime (fmt);
        char *msg;
        if (kind) {
            msg = g_strdup_printf (
                _ ("%s uses %s, which isn't supported by inline media. "
                   "Try PNG, JPEG, or GIF."),
                display_name ? display_name : _ ("image"), kind);
        } else {
            msg = g_strdup_printf (
                _ ("%s isn't a recognised image. "
                   "Try PNG, JPEG, or GIF."),
                display_name ? display_name : _ ("this file"));
        }
        toolbar_show_toast (msg);
        g_free (msg);
        return FALSE;
    }
    return TRUE;
}

/* Map a spec MediaErrorCode + DATA_ERROR text to a user-readable
 * toast string. */
static char *
error_toast_text (guint16 code, const char *server_text, gsize server_text_len)
{
    const char *what;
    switch (code) {
    case 1: what = _ ("Image too large for this server."); break;
    case 2: what = _ ("Server rejected the image format."); break;
    case 3: what = _ ("Rate limited — try again shortly."); break;
    case 4: what = _ ("Not authorised to send images."); break;
    case 5: what = _ ("Server temporarily busy."); break;
    case 0:
    default: what = _ ("Image upload failed."); break;
    }
    if (server_text && server_text_len > 0) {
        /* Pure layout combiner — no translatable content. */
        return g_strdup_printf (
            "%s (%.*s)", what,
            (int) (server_text_len > 200 ? 200 : server_text_len),
            server_text);
    }
    return g_strdup (what);
}

/* Upload-callback. Borrowed pointers; copy or use immediately. */
static void
on_upload_done (struct htlc_conn *htlc,
                const HxInlineMediaUploadResult *r, gpointer user_data)
{
    hx_attach_ctx *ctx = user_data;

    if (!r->media_id) {
        char *toast = error_toast_text (r->error_code, r->error_message,
                                        r->error_message_len);
        debug_log ("media", "attach upload failed: code=%u msg=%.*s",
                   (unsigned) r->error_code,
                   (int) (r->error_message_len > 200
                              ? 200
                              : r->error_message_len),
                   r->error_message ? r->error_message : "");
        toolbar_show_toast (toast);
        g_free (toast);
        attach_ctx_free (ctx);
        return;
    }

    /* Attach the handle + canonical mime to a chat send. The
	 * text body is empty for now; callers wanting a caption will
	 * be handled by a future 'compose preview' that lets the
	 * user type alongside the attachment. For v1 the text just
	 * defaults to "[image]" so legacy recipients see a sensible
	 * placeholder; capable recipients render the inline media. */
    hx_send_chat_with_media (htlc, "[image]", ctx->cid, /*style=*/0,
                             r->media_id, r->media_id_len,
                             r->media_type, r->media_type_len);

    char *toast
        = g_strdup_printf (_ ("Sent %s"),
                           ctx->display_name ? ctx->display_name
                                             : _ ("image"));
    toolbar_show_toast (toast);
    g_free (toast);

    attach_ctx_free (ctx);
}

/* g_file_load_bytes callback. Runs after the file is read into
 * memory. */
static void
on_bytes_loaded (GObject *src, GAsyncResult *res, gpointer user_data)
{
    hx_attach_ctx *ctx = user_data;
    GError *err = NULL;

    GBytes *bytes
        = g_file_load_bytes_finish (G_FILE (src), res, NULL, &err);
    if (!bytes) {
        char *msg = g_strdup_printf (
            _ ("Couldn't read %s: %s"),
            ctx->display_name ? ctx->display_name : _ ("image"),
            err ? err->message : _ ("unknown error"));
        toolbar_show_toast (msg);
        g_free (msg);
        g_clear_error (&err);
        attach_ctx_free (ctx);
        return;
    }

    /* Re-check the cap one more time before committing the
	 * upload — a disconnect / reconnect-to-non-capable-server
	 * race during the async load_bytes is the same shape as
	 * the file-dialog race above. Without the explicit check
	 * here, hx_send_upload_media_single's internal cap gate
	 * would fail and we'd surface the generic
	 * "couldn't start image upload" toast even though
	 * "inline media isn't available" is what actually
	 * happened. */
    if (!inline_media_cap_ok (ctx->htlc)) {
        toolbar_show_toast (
            _ ("Inline media isn't available on this server."));
        g_bytes_unref (bytes);
        attach_ctx_free (ctx);
        return;
    }

    if (!preflight_check (ctx->htlc, bytes, ctx->display_name)) {
        g_bytes_unref (bytes);
        attach_ctx_free (ctx);
        return;
    }

    gsize len = 0;
    const guchar *data = g_bytes_get_data (bytes, &len);

    /* declared_type passes the file's sniffed format through as
	 * a hint. Server uses magic-byte sniff for the authoritative
	 * decision regardless. */
    HxInlineMediaFormat fmt = inline_media_sniff (data, len);
    const char *declared = inline_media_format_to_mime (fmt);

    /* Pass attach_ctx_free as the upload's user_data_free hook.
	 * On the success / failure path on_upload_done frees ctx
	 * itself; the hook only fires when the upload helper's
	 * task_table entry is reclaimed without on_done having run
	 * (the connection-tear-down case). Without this the attach
	 * ctx leaks one-per-click during the disconnected window.
	 *
	 * The dispatcher picks single-shot vs chunked framing
	 * automatically based on the server-advertised CHAT_MEDIA_
	 * CHUNK_SIZE. */
    if (!hx_send_upload_media (
            ctx->htlc, data, len, declared,
            declared ? strlen (declared) : 0,
            on_upload_done, ctx, (GDestroyNotify) attach_ctx_free)) {
        /* Synchronous reject — cap gone, oversized, builder bug. */
        toolbar_show_toast (_ ("Couldn't start image upload."));
        g_bytes_unref (bytes);
        attach_ctx_free (ctx);
        return;
    }

    /* The upload helper copied the bytes it needed onto the
	 * wire; we're done with the GBytes. The Phase 9.A builder
	 * works against the payload pointer until hlwrite_chunks
	 * returns, which is inside hx_send_upload_media_single, so
	 * unref is safe here. */
    g_bytes_unref (bytes);
}

/* GtkFileDialog response. */
static void
on_file_picked (GObject *src, GAsyncResult *res, gpointer user_data)
{
    hx_attach_ctx *ctx = user_data;
    GError *err = NULL;
    GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (src), res,
                                               &err);
    if (!file) {
        /* User cancelled, or actual error — drop silently for the
		 * dismissed case (toast would be noisy). Surface a toast
		 * only on genuine errors. */
        if (err && !g_error_matches (err, GTK_DIALOG_ERROR,
                                     GTK_DIALOG_ERROR_DISMISSED)) {
            char *msg
                = g_strdup_printf (_ ("File picker failed: %s"),
                                   err->message);
            toolbar_show_toast (msg);
            g_free (msg);
        }
        g_clear_error (&err);
        attach_ctx_free (ctx);
        return;
    }

    char *basename = g_file_get_basename (file);
    ctx->display_name = basename;

    /* Re-check the inline-media cap before any further I/O.
	 * The file dialog is async: between the click that opened it
	 * and this callback firing the server connection can have
	 * dropped (or reconnected to a server that doesn't speak
	 * the extension). Surfacing the actionable "not available
	 * on this server" toast here avoids a slow path through
	 * stat / load_bytes / preflight only to land on a generic
	 * "couldn't start upload" message at the very end. */
    if (!inline_media_cap_ok (ctx->htlc)) {
        toolbar_show_toast (
            _ ("Inline media isn't available on this server."));
        g_object_unref (file);
        attach_ctx_free (ctx);
        return;
    }

    /* Pre-flight the file size BEFORE loading bytes — the MIME
	 * filter doesn't bound size, and reading e.g. a 50 MB photo
	 * into memory just to reject it after is wasteful. Query
	 * G_FILE_ATTRIBUTE_STANDARD_SIZE synchronously (one stat()
	 * on local files; cheap). On failure (network mounts that
	 * don't report size, permission errors) we fall through to
	 * the async load and let the preflight_check after the read
	 * catch it. */
    GError *info_err = NULL;
    GFileInfo *info = g_file_query_info (
        file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &info_err);
    if (info) {
        goffset size = g_file_info_get_size (info);
        g_object_unref (info);
        guint32 max_bytes = effective_max_bytes (ctx->htlc);
        if (size > 0 && (guint64) size > (guint64) max_bytes) {
            char *msg = g_strdup_printf (
                _ ("%s is %.1f KB — over the %u KB cap. "
                   "Pick a smaller image."),
                ctx->display_name ? ctx->display_name : _ ("image"),
                (double) size / 1024.0, (unsigned) (max_bytes / 1024));
            toolbar_show_toast (msg);
            g_free (msg);
            g_object_unref (file);
            attach_ctx_free (ctx);
            return;
        }
    } else {
        g_clear_error (&info_err);
    }

    /* Load the bytes async — small images finish on the main
	 * thread fast but still benefit from non-blocking I/O for
	 * slow disks / network mounts. */
    g_file_load_bytes_async (file, NULL, on_bytes_loaded, ctx);
    g_object_unref (file);
}

/* Build a GtkFileFilter that matches the spec allowlist mime
 * types. Used by the file dialog to grey out non-image files. */
static GtkFileFilter *
build_image_filter (void)
{
    GtkFileFilter *f = gtk_file_filter_new ();
    gtk_file_filter_set_name (f, _ ("Images (PNG, JPEG, GIF)"));
    gtk_file_filter_add_mime_type (f, "image/png");
    gtk_file_filter_add_mime_type (f, "image/jpeg");
    gtk_file_filter_add_mime_type (f, "image/gif");
    return f;
}

/* Button click handler. */
static void
on_attach_clicked (GtkButton *btn, gpointer user_data)
{
    hx_attach_ctx *seed = user_data;

    if (!inline_media_cap_ok (seed->htlc)) {
        /* Belt-and-braces: the button is normally hidden when
		 * the cap isn't negotiated (see
		 * inline_media_attach_refresh_all_chats), but the cap
		 * could disappear between paint and click on a session
		 * tear-down race. */
        toolbar_show_toast (
            _ ("Inline media isn't available on this server."));
        return;
    }

    /* Allocate a per-click context. The seed holds the shape
	 * (gchat + htlc + cid) but we don't share it across clicks
	 * since each upload owns its own lifecycle. */
    hx_attach_ctx *ctx = g_new0 (hx_attach_ctx, 1);
    ctx->gchat = seed->gchat;
    ctx->htlc = seed->htlc;
    ctx->cid = seed->cid;

    GtkFileDialog *fd = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (fd, _ ("Attach Image"));

    GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
    GtkFileFilter *image_filter = build_image_filter ();
    g_list_store_append (filters, image_filter);
    g_object_unref (image_filter);
    gtk_file_dialog_set_filters (fd, G_LIST_MODEL (filters));
    g_object_unref (filters);

    GtkWindow *parent = NULL;
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (btn));
    if (GTK_IS_WINDOW (root)) {
        parent = GTK_WINDOW (root);
    }
    gtk_file_dialog_open (fd, parent, NULL, on_file_picked, ctx);
    g_object_unref (fd);
}

GtkWidget *
hx_inline_media_attach_button_new (struct gtkhx_chat *gchat,
                                   struct htlc_conn *htlc)
{
    GtkWidget *btn = gtk_button_new_from_icon_name ("mail-attachment-symbolic");
    gtk_widget_set_tooltip_text (btn, _ ("Attach Image"));

    /* Visibility is gated on HTLC_CAP_INLINE_MEDIA. The seed
	 * sets the initial state to match the htlc's current caps
	 * directly so pchat windows created LATER in the session
	 * (private chats opened on first user-interaction with
	 * a user from users.c, etc.) don't stay hidden indefinitely
	 * waiting for the next setbtns→refresh cycle. The setbtns
	 * path keeps walking every existing button to handle the
	 * disconnect / reconnect case. Most Hotline servers don't
	 * support the extension; leaving the button visible (and
	 * inert on a click) would be misleading. */
    gboolean show_now = htlc && (htlc->caps & HTLC_CAP_INLINE_MEDIA) != 0;
    gtk_widget_set_visible (btn, show_now);

    /* Persistent click-seed holds the shape we need on every
	 * click — gchat + htlc + cid. Lifetime is tied to the
	 * button; freed via the "destroy" weak-notify. */
    hx_attach_ctx *seed = g_new0 (hx_attach_ctx, 1);
    seed->gchat = gchat;
    seed->htlc = htlc;
    seed->cid = gchat ? gchat->cid : 0;
    g_object_set_data_full (G_OBJECT (btn), "attach-seed", seed,
                            (GDestroyNotify) attach_ctx_free);

    g_signal_connect (btn, "clicked", G_CALLBACK (on_attach_clicked), seed);
    return btn;
}

void
inline_media_attach_refresh_all_chats (session *sess)
{
    if (!sess || !sess->gchats) {
        return;
    }
    gboolean show = (sess->htlc.caps & HTLC_CAP_INLINE_MEDIA) != 0;

    GHashTableIter iter;
    gpointer val;
    g_hash_table_iter_init (&iter, sess->gchats);
    while (g_hash_table_iter_next (&iter, NULL, &val)) {
        struct gtkhx_chat *gchat = val;
        if (gchat && gchat->media_attach_btn) {
            gtk_widget_set_visible (gchat->media_attach_btn, show);
        }
    }
}
