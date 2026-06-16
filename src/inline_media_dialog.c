/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * See inline_media_dialog.h for the contract.
 *
 * Lifecycle:
 *
 *   inline_media_show_dialog
 *     ├─ allocate hx_media_dialog state
 *     ├─ build the AdwDialog (header + GtkStack body)
 *     ├─ inline_media_download_start (stashes the download
 *     │   handle on the state)
 *     ├─ adw_dialog_present
 *     └─ on dialog close → free state, cancel download if
 *        in-flight
 *
 *   download callback (main thread):
 *     ├─ on error: swap to error page with the spec-mapped
 *     │   message
 *     ├─ on success: hand bytes to inline_media_decode (Phase
 *     │   9.B), set the resulting GdkTexture on the GtkPicture,
 *     │   enable Save / Open buttons
 *     └─ stash the canonical bytes + mime on the state so the
 *        button handlers can use them later
 *
 *   Save As button:
 *     GtkFileDialog → user picks a path → write bytes via
 *     g_file_replace_contents.
 *
 *   Open Externally button:
 *     Write bytes to a unique temp file under XDG_RUNTIME_DIR
 *     (or g_get_tmp_dir as fallback), launch via
 *     GtkFileLauncher.
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
#include "inline_media.h"
#include "inline_media_decode.h"
#include "inline_media_dialog.h"
#include "inline_media_download.h"
#include "debug.h"

typedef struct {
    AdwDialog *dialog;
    GtkWidget *stack;       /* GtkStack with Loading / Image / Error pages */
    GtkWidget *picture;     /* GtkPicture, owned by the image page */
    GtkWidget *error_label;
    GtkWidget *save_btn;
    GtkWidget *open_btn;

    struct htlc_conn *htlc;
    hx_inline_media_download *download;
    /* Filled on download success — the canonical bytes the Save /
	 * Open buttons write out. Both are NULL until the download
	 * completes. */
    GBytes *bytes;
    char *mime;
} hx_media_dialog;

static const char *
mime_to_suffix (const char *mime)
{
    if (!mime) {
        return ".bin";
    }
    if (g_ascii_strcasecmp (mime, "image/png") == 0) {
        return ".png";
    }
    if (g_ascii_strcasecmp (mime, "image/jpeg") == 0) {
        return ".jpg";
    }
    if (g_ascii_strcasecmp (mime, "image/gif") == 0) {
        return ".gif";
    }
    return ".bin";
}

static void
md_free (hx_media_dialog *md)
{
    if (!md) {
        return;
    }
    if (md->bytes) {
        g_bytes_unref (md->bytes);
    }
    g_free (md->mime);
    g_free (md);
}

/* AdwDialog ::closed handler. Cancel the in-flight download (if
 * any) and free the state. */
static void
on_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
    (void) dialog;
    hx_media_dialog *md = user_data;
    if (md->download) {
        inline_media_download_cancel (md->download);
        md->download = NULL;
    }
    md_free (md);
}

static void
swap_to_error (hx_media_dialog *md, const char *message)
{
    if (md->error_label) {
        gtk_label_set_text (GTK_LABEL (md->error_label),
                            message ? message : "Failed to load image");
    }
    if (md->stack) {
        gtk_stack_set_visible_child_name (GTK_STACK (md->stack), "error");
    }
}

/* Save As button handler. */
static void
on_save_finished (GObject *src, GAsyncResult *res, gpointer user_data)
{
    hx_media_dialog *md = user_data;
    GError *err = NULL;
    GFile *file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (src), res, &err);
    if (!file) {
        if (err && !g_error_matches (err, GTK_DIALOG_ERROR,
                                     GTK_DIALOG_ERROR_DISMISSED)) {
            debug_log ("media", "save-as cancel/error: %s", err->message);
        }
        g_clear_error (&err);
        return;
    }

    if (!md->bytes) {
        g_object_unref (file);
        return;
    }
    gsize len = 0;
    const void *data = g_bytes_get_data (md->bytes, &len);

    /* g_file_replace_contents writes synchronously. The payload
	 * is ≤ MAX_BYTES (256 KiB by default) so this is fine on the
	 * main thread; if larger images ever ship we can swap to the
	 * async variant. */
    if (!g_file_replace_contents (file, data, len, NULL, FALSE,
                                  G_FILE_CREATE_NONE, NULL, NULL, &err)) {
        debug_log ("media", "save-as write failed: %s",
                   err ? err->message : "unknown");
        g_clear_error (&err);
    }
    g_object_unref (file);
}

static void
on_save_clicked (GtkButton *btn, gpointer user_data)
{
    (void) btn;
    hx_media_dialog *md = user_data;
    if (!md->bytes) {
        return;
    }
    GtkFileDialog *fd = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (fd, "Save Image");
    gchar *suggested
        = g_strdup_printf ("image%s", mime_to_suffix (md->mime));
    gtk_file_dialog_set_initial_name (fd, suggested);
    g_free (suggested);

    GtkWindow *parent = NULL;
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (md->dialog));
    if (GTK_IS_WINDOW (root)) {
        parent = GTK_WINDOW (root);
    }
    gtk_file_dialog_save (fd, parent, NULL, on_save_finished, md);
    g_object_unref (fd);
}

/* Open Externally button handler — drop bytes to a temp file
 * and ask the OS / portal to open it. */
static void
on_launch_finished (GObject *src, GAsyncResult *res, gpointer user_data)
{
    (void) user_data;
    GError *err = NULL;
    if (!gtk_file_launcher_launch_finish (GTK_FILE_LAUNCHER (src), res,
                                          &err)) {
        if (err) {
            debug_log ("media", "open-externally failed: %s", err->message);
            g_clear_error (&err);
        }
    }
}

static void
on_open_clicked (GtkButton *btn, gpointer user_data)
{
    (void) btn;
    hx_media_dialog *md = user_data;
    if (!md->bytes) {
        return;
    }
    /* Mktemp-ish path under the runtime dir. unique per launch. */
    const char *tmp = g_get_user_runtime_dir ();
    if (!tmp || !*tmp) {
        tmp = g_get_tmp_dir ();
    }
    char *fname = g_strdup_printf ("gtkhx-media-%u%s",
                                   g_random_int (),
                                   mime_to_suffix (md->mime));
    char *path = g_build_filename (tmp, fname, NULL);
    g_free (fname);

    gsize len = 0;
    const void *data = g_bytes_get_data (md->bytes, &len);
    GError *err = NULL;
    if (!g_file_set_contents (path, data, (gssize) len, &err)) {
        debug_log ("media", "open-externally tempfile write failed: %s",
                   err ? err->message : "unknown");
        g_clear_error (&err);
        g_free (path);
        return;
    }

    GFile *file = g_file_new_for_path (path);
    GtkFileLauncher *fl = gtk_file_launcher_new (file);
    GtkWindow *parent = NULL;
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (md->dialog));
    if (GTK_IS_WINDOW (root)) {
        parent = GTK_WINDOW (root);
    }
    gtk_file_launcher_launch (fl, parent, NULL, on_launch_finished, NULL);
    g_object_unref (fl);
    g_object_unref (file);
    g_free (path);
}

/* Download callback. Runs on the main thread; can touch widgets
 * directly. */
static void
on_download_done (struct htlc_conn *htlc,
                  const HxInlineMediaDownloadResult *result,
                  gpointer user_data)
{
    (void) htlc;
    hx_media_dialog *md = user_data;
    md->download = NULL;

    if (!result->bytes) {
        /* Surface the spec MediaErrorCode meaningfully. */
        char buf[256];
        const char *what = "Failed to load image";
        switch (result->error_code) {
        case 1: what = "Image too large"; break;
        case 2: what = "Unsupported image format"; break;
        case 3: what = "Rate limited — try again shortly"; break;
        case 4: what = "Not authorised to view this image"; break;
        case 5: what = "Server temporarily busy"; break;
        default: break;
        }
        if (result->error_message && result->error_message_len > 0) {
            g_snprintf (buf, sizeof (buf), "%s\n%.*s", what,
                        (int) (result->error_message_len > 200
                                   ? 200
                                   : result->error_message_len),
                        result->error_message);
            swap_to_error (md, buf);
        } else {
            swap_to_error (md, what);
        }
        return;
    }

    /* Decode under spec-default caps; we don't have per-server caps
	 * threaded through to the dialog. Phase B's
	 * inline_media_decode handles the cap fall-through internally
	 * when caps fields are 0. */
    HxInlineMediaCaps caps = {0};
    HxInlineMediaDecoded decoded = inline_media_decode (
        result->bytes->data, result->bytes->len, &caps);
    if (!decoded.texture) {
        char buf[256];
        g_snprintf (buf, sizeof (buf), "Image decoder rejected: %s",
                    decoded.error_message ? decoded.error_message
                                          : "unknown error");
        swap_to_error (md, buf);
        return;
    }

    /* Stash the canonical bytes + mime on the dialog so the Save /
	 * Open handlers can use them. result->bytes is borrowed —
	 * dup into a GBytes that owns its own copy. */
    md->bytes = g_bytes_new (result->bytes->data, result->bytes->len);
    md->mime = g_strdup (result->canonical_mime);

    gtk_picture_set_paintable (GTK_PICTURE (md->picture),
                               GDK_PAINTABLE (decoded.texture));
    g_object_unref (decoded.texture);

    gtk_widget_set_sensitive (md->save_btn, TRUE);
    gtk_widget_set_sensitive (md->open_btn, TRUE);
    gtk_stack_set_visible_child_name (GTK_STACK (md->stack), "image");
}

void
inline_media_show_dialog (GtkWidget *parent_widget, struct htlc_conn *htlc,
                          const guint8 *media_id, gsize media_id_len,
                          const char *mime, guint32 width_hint,
                          guint32 height_hint, guint32 bytes_hint)
{
    if (!media_id || media_id_len == 0) {
        return;
    }

    hx_media_dialog *md = g_new0 (hx_media_dialog, 1);
    md->htlc = htlc;

    /* AdwDialog with a content child = AdwToolbarView (header +
	 * scrollable body). */
    md->dialog = adw_dialog_new ();
    adw_dialog_set_title (md->dialog, "Image");
    adw_dialog_set_content_width (md->dialog, 720);
    adw_dialog_set_content_height (md->dialog, 540);

    AdwToolbarView *tv = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());

    GtkWidget *header = adw_header_bar_new ();
    md->save_btn = gtk_button_new_with_label ("Save As…");
    gtk_widget_set_sensitive (md->save_btn, FALSE);
    g_signal_connect (md->save_btn, "clicked", G_CALLBACK (on_save_clicked),
                      md);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), md->save_btn);

    md->open_btn = gtk_button_new_with_label ("Open Externally");
    gtk_widget_set_sensitive (md->open_btn, FALSE);
    g_signal_connect (md->open_btn, "clicked", G_CALLBACK (on_open_clicked),
                      md);
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), md->open_btn);
    adw_toolbar_view_add_top_bar (tv, header);

    /* Stack body: Loading / Image / Error. */
    md->stack = gtk_stack_new ();
    gtk_widget_set_vexpand (md->stack, TRUE);
    gtk_widget_set_hexpand (md->stack, TRUE);

    /* Loading page — AdwStatusPage with a spinner. */
    GtkWidget *loading_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign (loading_box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (loading_box, GTK_ALIGN_CENTER);
    GtkWidget *spinner = adw_spinner_new ();
    gtk_widget_set_size_request (spinner, 64, 64);
    gtk_box_append (GTK_BOX (loading_box), spinner);
    char hint_buf[128] = "Loading…";
    if (width_hint && height_hint && bytes_hint) {
        g_snprintf (hint_buf, sizeof (hint_buf), "Loading %u×%u (%u KB)…",
                    (unsigned) width_hint, (unsigned) height_hint,
                    (unsigned) (bytes_hint + 1023) / 1024);
    } else if (width_hint && height_hint) {
        g_snprintf (hint_buf, sizeof (hint_buf), "Loading %u×%u…",
                    (unsigned) width_hint, (unsigned) height_hint);
    }
    GtkWidget *loading_label = gtk_label_new (hint_buf);
    gtk_widget_add_css_class (loading_label, "dim-label");
    gtk_box_append (GTK_BOX (loading_box), loading_label);
    gtk_stack_add_named (GTK_STACK (md->stack), loading_box, "loading");

    /* Image page — scrolled GtkPicture. */
    md->picture = gtk_picture_new ();
    gtk_picture_set_can_shrink (GTK_PICTURE (md->picture), TRUE);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_picture_set_keep_aspect_ratio (GTK_PICTURE (md->picture), TRUE);
    G_GNUC_END_IGNORE_DEPRECATIONS
    GtkWidget *scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), md->picture);
    gtk_widget_set_vexpand (scroll, TRUE);
    gtk_widget_set_hexpand (scroll, TRUE);
    gtk_stack_add_named (GTK_STACK (md->stack), scroll, "image");

    /* Error page. */
    GtkWidget *err_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign (err_box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (err_box, GTK_ALIGN_CENTER);
    md->error_label = gtk_label_new ("Failed to load image");
    gtk_label_set_wrap (GTK_LABEL (md->error_label), TRUE);
    gtk_widget_add_css_class (md->error_label, "dim-label");
    gtk_box_append (GTK_BOX (err_box), md->error_label);
    gtk_stack_add_named (GTK_STACK (md->stack), err_box, "error");

    gtk_stack_set_visible_child_name (GTK_STACK (md->stack), "loading");
    adw_toolbar_view_set_content (tv, md->stack);
    adw_dialog_set_child (md->dialog, GTK_WIDGET (tv));

    g_signal_connect (md->dialog, "closed", G_CALLBACK (on_dialog_closed),
                      md);

    (void) mime; /* could pre-populate gtk_picture mime, not needed */

    /* Kick off the download. If start fails (cap not negotiated,
	 * bad handle), swap straight to error. */
    md->download = inline_media_download_start (
        htlc, media_id, media_id_len, on_download_done, md);
    if (!md->download) {
        swap_to_error (md, "Inline media unavailable on this server");
    }

    adw_dialog_present (md->dialog, parent_widget);
}
