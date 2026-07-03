/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * users_voice_col.c — see users_voice_col.h. The user-list voice-indicator
 * column, split verbatim out of users_view.c when HxUserListView moved to
 * Rust (Phase R5.9). Everything here is #ifdef HAVE_VOICE; with voice
 * compiled out gtkhx_users_voice_column_new is a stub that returns NULL.
 */

#include "config.h"

#include <gtk/gtk.h>

#include "hx.h"
#include "users_row.h"
#include "users_voice_col.h"
#ifdef HAVE_VOICE
#include "voice_model.h"
#endif

#ifdef HAVE_VOICE

/* Stash the GtkListItem on `cell` — the factory-created GtkImage — under the
 * "user-list-item" qdata key the Rust view's uid/name binds also use, so the
 * right-click handler can recover the row from a click on this cell. Do NOT
 * walk up and write onto GtkColumnView's internal cell/row widgets: that
 * corrupts its cell lifecycle and frees a live row on the first append (a
 * GTK_IS_ACCESSIBLE abort in gtk_list_item_base_update). Same fix as the Rust
 * stash_list_item in users_view.rs. */
static void
stash_list_item (GtkWidget *cell, GtkListItem *item)
{
    g_object_set_data (G_OBJECT (cell), "user-list-item", item);
}

/*
 * Renders a single small icon per row reflecting the user's voice state
 * per HxVoiceModel: NONE (empty), IN_VOICE (dim speaker), SPEAKING (accent
 * speaker), MUTED (mic-disabled). Each cell subscribes to the model's
 * "indicator-changed" signal at setup and disconnects at finalize; the
 * handler ignores frames whose uid doesn't match the bound row.
 *
 * Storage: a VoiceCellData on the GtkImage's qdata holds a strong model
 * ref, the borrowed bound row, and the handler id.
 */

typedef struct {
    /* Strong ref: the cell takes its own ref at setup time and releases
     * it at finalize. Holds the model alive for the cell's full lifetime
     * (cells outlive bind/unbind — GtkColumnView recycles widgets) so the
     * disconnect at finalize is always safe. */
    HxVoiceModel *model; /* owned strong ref; NULL allowed (no model) */
    HxUserRow *row;      /* borrowed; NULL when unbound */
    gulong indicator_changed_id;
} VoiceCellData;

static const char *
voice_indicator_icon (HxVoiceIndicator i)
{
    switch (i) {
    case HX_VOICE_INDICATOR_NONE:
        return NULL;
    case HX_VOICE_INDICATOR_IN_VOICE:
        /* Dim speaker — "in voice, silent right now." Stock Adwaita icon. */
        return "audio-volume-low-symbolic";
    case HX_VOICE_INDICATOR_SPEAKING:
        return "audio-volume-high-symbolic";
    case HX_VOICE_INDICATOR_MUTED:
        return "microphone-disabled-symbolic";
    }
    return NULL;
}

static void
voice_cell_refresh (GtkImage *img, HxVoiceModel *model, guint16 uid)
{
    HxVoiceIndicator ind
        = model ? hx_voice_model_get_indicator (model, uid)
                : HX_VOICE_INDICATOR_NONE;
    const char *icon = voice_indicator_icon (ind);
    if (icon) {
        gtk_image_set_from_icon_name (img, icon);
        gtk_widget_set_visible (GTK_WIDGET (img), TRUE);
    } else {
        gtk_image_clear (img);
        gtk_widget_set_visible (GTK_WIDGET (img), FALSE);
    }
    /* Speaking gets an accent colour via Adwaita's `.accent` style class;
     * the other states use `.dim-label`. Apply additively rather than
     * swapping so the right one wins the cascade. */
    GtkWidget *w = GTK_WIDGET (img);
    if (ind == HX_VOICE_INDICATOR_SPEAKING) {
        gtk_widget_remove_css_class (w, "dim-label");
        gtk_widget_add_css_class (w, "accent");
    } else {
        gtk_widget_remove_css_class (w, "accent");
        gtk_widget_add_css_class (w, "dim-label");
    }
}

/* Fires whenever ANY uid's indicator flips. Each cell only cares about
 * its currently-bound row's uid; skip the rest. */
static void
on_voice_model_indicator_changed (HxVoiceModel *model, guint uid,
                                  guint indicator, gpointer user_data)
{
    GtkImage *img = user_data;
    VoiceCellData *data
        = g_object_get_data (G_OBJECT (img), "voice-cell-data");
    (void) indicator; /* fresh value re-read from the model below */
    if (!data || !data->row) {
        return;
    }
    if ((guint) hx_user_row_get_uid (data->row) != uid) {
        return;
    }
    voice_cell_refresh (img, model, (guint16) uid);
}

/* Cleanup at GtkImage finalize. The image outlives the cell binds
 * (GtkColumnView recycles widgets), so the per-cell subscription
 * installed at setup is torn down here, not on unbind. */
static void
voice_cell_data_free (gpointer p)
{
    VoiceCellData *data = p;
    if (!data) {
        return;
    }
    if (data->model) {
        if (data->indicator_changed_id) {
            g_signal_handler_disconnect (data->model,
                                         data->indicator_changed_id);
        }
        g_object_unref (data->model);
    }
    g_free (data);
}

static void
voice_setup (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    session *sess = d;
    GtkWidget *img = gtk_image_new ();
    (void) f;
    gtk_image_set_pixel_size (GTK_IMAGE (img), 12);
    gtk_widget_set_halign (img, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (img, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (img, "dim-label");
    /* Hidden until a bind reveals a non-NONE indicator. */
    gtk_widget_set_visible (img, FALSE);

    VoiceCellData *data = g_new0 (VoiceCellData, 1);
    HxVoiceModel *model = sess ? sess->voice_model : NULL;
    if (model) {
        data->model = g_object_ref (model);
        data->indicator_changed_id = g_signal_connect (
            data->model, "indicator-changed",
            G_CALLBACK (on_voice_model_indicator_changed), img);
    }
    g_object_set_data_full (G_OBJECT (img), "voice-cell-data", data,
                            voice_cell_data_free);
    gtk_list_item_set_child (item, img);
}

static void
voice_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkImage *img = GTK_IMAGE (gtk_list_item_get_child (item));
    HxUserRow *row = gtk_list_item_get_item (item);
    VoiceCellData *data
        = g_object_get_data (G_OBJECT (img), "voice-cell-data");
    (void) f;
    (void) d;
    stash_list_item (GTK_WIDGET (img), item);
    if (!data) {
        return;
    }
    data->row = row;
    voice_cell_refresh (img, data->model,
                        row ? hx_user_row_get_uid (row) : 0);
}

static void
voice_unbind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkImage *img = GTK_IMAGE (gtk_list_item_get_child (item));
    VoiceCellData *data
        = g_object_get_data (G_OBJECT (img), "voice-cell-data");
    (void) f;
    (void) d;
    if (!data) {
        return;
    }
    data->row = NULL;
    gtk_image_clear (img);
    gtk_widget_set_visible (GTK_WIDGET (img), FALSE);
}

GtkColumnViewColumn *
gtkhx_users_voice_column_new (session *sess)
{
    GtkListItemFactory *factory;
    GtkColumnViewColumn *col;

    if (!sess) {
        return NULL;
    }
    /* The factory is a no-op against a session with no voice model
     * (sess->voice_model NULL) — cells stay empty + invisible, so a
     * server without voice shows nothing here. */
    factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (voice_setup), sess);
    g_signal_connect (factory, "bind", G_CALLBACK (voice_bind), sess);
    g_signal_connect (factory, "unbind", G_CALLBACK (voice_unbind), sess);
    /* Header glyph: a single speaker emoji as a compact column label. */
    col = gtk_column_view_column_new ("\xf0\x9f\x94\x88", factory);
    gtk_column_view_column_set_fixed_width (col, 22);
    gtk_column_view_column_set_resizable (col, FALSE);
    return col; /* transfer full — caller's append_column takes its own ref */
}

#else /* !HAVE_VOICE */

GtkColumnViewColumn *
gtkhx_users_voice_column_new (session *sess)
{
    (void) sess;
    return NULL;
}

#endif /* HAVE_VOICE */
