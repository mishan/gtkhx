/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "config.h"
#include <gtk/gtk.h>
#include "compat.h" /* _() gettext wrapper */
#include "emoji.h"

/*
 * The on-pick handler. GtkEmojiChooser::emoji-picked fires once per
 * selection with the picked emoji as a UTF-8 string (e.g. "😀" or a
 * multi-codepoint cluster like a skin-tone variant). Insert that
 * verbatim at the input's cursor position via the buffer's "insert at
 * cursor" helper — it handles the case where there's a selection too,
 * replacing it cleanly.
 *
 * Re-focus the input after insertion. Without this, focus stays on
 * the menu button (since the chooser popover closed on pick) and the
 * next keystroke goes nowhere useful — the chat's key controller
 * lives on the text view, so we need it to have keyboard focus for
 * Return-to-send to keep working.
 */
static void
on_emoji_picked (GtkEmojiChooser *chooser,
                 const char *text,
                 gpointer user_data)
{
    GtkWidget *target = GTK_WIDGET (user_data);

    (void) chooser;

    if (!text || !*text) {
        return;
    }
    if (!GTK_IS_TEXT_VIEW (target)) {
        return;
    }

    GtkTextBuffer *buffer =
        gtk_text_view_get_buffer (GTK_TEXT_VIEW (target));
    if (!buffer) {
        return;
    }
    gtk_text_buffer_insert_at_cursor (buffer, text, -1);

    gtk_widget_grab_focus (target);
}

GtkWidget *
hx_emoji_button_new (GtkWidget *target_text_view)
{
    g_return_val_if_fail (GTK_IS_TEXT_VIEW (target_text_view), NULL);

    GtkWidget *button = gtk_menu_button_new ();
    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (button),
                                   "face-smile-symbolic");
    /* Adwaita's flat style keeps the button visually unobtrusive next
     * to the input frame — without it we get a heavyweight raised
     * button that fights for attention with the actual chat text. */
    gtk_widget_add_css_class (button, "flat");
    gtk_widget_set_tooltip_text (button, _("Insert emoji"));

    GtkWidget *chooser = gtk_emoji_chooser_new ();
    g_signal_connect (chooser, "emoji-picked",
                      G_CALLBACK (on_emoji_picked), target_text_view);
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), chooser);

    return button;
}
