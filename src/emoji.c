/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "config.h"
#include <string.h>
#include <gtk/gtk.h>
#include "compat.h" /* _() gettext wrapper */
#include "emoji.h"
#include "hotline_proto.h" /* gtkhx_proto_shortcode_matches */
#include "prefs.h"         /* gtkhx_prefs.emoji_typeahead */

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

/* ===================================================================
 * Inline :shortcode: typeahead (phase E5)
 * ===================================================================
 *
 * A keyboard-driven companion to the picker: typing `:jo` pops up a list
 * of matching shortcodes, and Tab/Enter inserts the chosen emoji glyph in
 * place of the partial token. The match list comes from the Rust
 * gtkhx_proto_shortcode_matches; this file owns only the GTK wiring.
 *
 * Key routing: a dedicated GtkEventControllerKey in the CAPTURE phase
 * intercepts Up/Down/Tab/Enter/Esc *only while the popover is open*,
 * leaving the chat input's own bubble-phase key handler (Tab nick
 * completion, Return-to-send, Up/Down history) untouched the rest of the
 * time.
 */

/* Minimum prefix length (chars after the colon) before the popup opens —
 * keeps a lone ":" or ":a" from dumping the whole table. */
#define TA_MIN_PREFIX 2
/* Max suggestions shown. */
#define TA_MAX_MATCHES 8
/* Record buffer: TA_MAX_MATCHES records of "name\temoji\n"; the longest
 * shortcode is ~76 bytes, emoji clusters ~28, so 128/record is ample. */
#define TA_MATCH_BUF (TA_MAX_MATCHES * 128)
/* Runaway guard for the backward token scan: stop if more than this many
 * name characters precede the cursor without an opening colon. Must clear
 * the longest real shortcode (76 chars today, e.g.
 * couple_with_heart_person_person_medium-dark_skin_tone_medium-light_skin_tone)
 * with headroom so typing/pasting a long shortcode still triggers the
 * popup; only meant to bound pathological input. */
#define TA_MAX_PREFIX_SCAN 128

typedef struct {
    GtkTextView *view;     /* the input; owns this struct via set_data_full */
    GtkWidget *popover;    /* child of view (set_parent); GTK frees it      */
    GtkWidget *listbox;    /* suggestion rows                               */
    gboolean open;         /* is the popover currently shown?               */
    gboolean suppress;     /* guard re-entrancy while we edit the buffer    */
    int tok_start_off;     /* char offset of the ':' that opened the token  */
} EmojiTypeahead;

static gboolean
ta_is_name_char (gunichar c)
{
    return c < 128
           && (g_ascii_islower ((char) c) || g_ascii_isdigit ((char) c)
               || c == '_' || c == '+' || c == '-');
}

/* If the cursor sits inside an open `:prefix` token (opening colon at line
 * start or after whitespace, then >= TA_MIN_PREFIX name chars, no closing
 * colon), return TRUE and hand back the colon's char offset and the
 * prefix text (caller g_frees). Otherwise FALSE. */
static gboolean
ta_current_token (GtkTextView *view, int *colon_off, char **prefix_out)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer (view);
    GtkTextIter cur, it;

    gtk_text_buffer_get_iter_at_mark (buf, &cur, gtk_text_buffer_get_insert (buf));
    it = cur;

    int steps = 0;
    while (gtk_text_iter_backward_char (&it)) {
        gunichar c = gtk_text_iter_get_char (&it);
        if (ta_is_name_char (c)) {
            if (++steps > TA_MAX_PREFIX_SCAN) {
                return FALSE; /* runaway — not a shortcode */
            }
            continue;
        }
        if (c == ':') {
            GtkTextIter before = it;
            gboolean at_start = !gtk_text_iter_backward_char (&before);
            gboolean ok_prev
                = at_start || g_unichar_isspace (gtk_text_iter_get_char (&before));
            if (!ok_prev || steps < TA_MIN_PREFIX) {
                return FALSE;
            }
            GtkTextIter pstart = it;
            gtk_text_iter_forward_char (&pstart); /* skip the colon */
            *prefix_out = gtk_text_buffer_get_text (buf, &pstart, &cur, FALSE);
            *colon_off = gtk_text_iter_get_offset (&it);
            return TRUE;
        }
        return FALSE; /* some other char ends the run with no opener */
    }
    return FALSE; /* hit buffer start without an opening colon */
}

static void
ta_hide (EmojiTypeahead *ta)
{
    if (ta->open) {
        ta->open = FALSE;
        gtk_popover_popdown (GTK_POPOVER (ta->popover));
    }
}

static void
ta_clear_rows (EmojiTypeahead *ta)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child (ta->listbox)) != NULL) {
        gtk_list_box_remove (GTK_LIST_BOX (ta->listbox), child);
    }
}

/* Parse the FFI's "name\temoji\n" record run into selectable rows. The
 * buffer was zero-filled before the call, so the trailing (unwritten)
 * region is NULs and the strchr scans can't run off the end. */
static void
ta_populate (EmojiTypeahead *ta, const char *recs, gsize nrec)
{
    ta_clear_rows (ta);

    const char *p = recs;
    for (gsize i = 0; i < nrec; i++) {
        const char *tab = strchr (p, '\t');
        if (!tab) {
            break;
        }
        const char *nl = strchr (tab + 1, '\n');
        if (!nl) {
            break;
        }
        char *name = g_strndup (p, (gsize) (tab - p));
        char *emoji = g_strndup (tab + 1, (gsize) (nl - (tab + 1)));

        GtkWidget *row = gtk_list_box_row_new ();
        GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start (hbox, 6);
        gtk_widget_set_margin_end (hbox, 6);
        gtk_widget_set_margin_top (hbox, 2);
        gtk_widget_set_margin_bottom (hbox, 2);
        gtk_box_append (GTK_BOX (hbox), gtk_label_new (emoji));
        char *label = g_strdup_printf (":%s:", name);
        GtkWidget *nlab = gtk_label_new (label);
        gtk_widget_set_halign (nlab, GTK_ALIGN_START);
        g_free (label);
        gtk_box_append (GTK_BOX (hbox), nlab);
        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), hbox);

        /* The row owns the emoji string; commit reads it back. */
        g_object_set_data_full (G_OBJECT (row), "hx-emoji", emoji, g_free);
        gtk_list_box_append (GTK_LIST_BOX (ta->listbox), row);

        g_free (name);
        p = nl + 1;
    }

    GtkListBoxRow *first
        = gtk_list_box_get_row_at_index (GTK_LIST_BOX (ta->listbox), 0);
    if (first) {
        gtk_list_box_select_row (GTK_LIST_BOX (ta->listbox), first);
    }
}

/* Anchor the popover at the caret. */
static void
ta_position (EmojiTypeahead *ta)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer (ta->view);
    GtkTextIter cur;
    GdkRectangle loc;
    int wx, wy;

    gtk_text_buffer_get_iter_at_mark (buf, &cur, gtk_text_buffer_get_insert (buf));
    gtk_text_view_get_iter_location (ta->view, &cur, &loc);
    gtk_text_view_buffer_to_window_coords (ta->view, GTK_TEXT_WINDOW_WIDGET,
                                           loc.x, loc.y, &wx, &wy);
    GdkRectangle r = { wx, wy, 1, loc.height > 0 ? loc.height : 1 };
    gtk_popover_set_pointing_to (GTK_POPOVER (ta->popover), &r);
}

/* Recompute the popup against the current cursor context. */
static void
ta_update (EmojiTypeahead *ta)
{
    if (ta->suppress) {
        return;
    }

    /* Phase E6: the popup is independently toggleable (Settings → Chat →
	 * Emoji). Read live so flipping it takes effect immediately, including
	 * on already-open input windows. */
    if (!gtkhx_prefs.emoji_typeahead) {
        ta_hide (ta);
        return;
    }

    int colon_off = 0;
    char *prefix = NULL;
    if (!ta_current_token (ta->view, &colon_off, &prefix)) {
        ta_hide (ta);
        return;
    }

    /* Reserve the final byte as a sentinel NUL: the FFI may fill exactly
	 * up to the cap it's given and never appends a NUL (per
	 * hotline_proto.h), so passing `sizeof outbuf - 1` guarantees
	 * outbuf[sizeof outbuf - 1] stays 0 and ta_populate's strchr scans
	 * can't read past the buffer. The memset makes the whole tail 0 too. */
    guint8 outbuf[TA_MATCH_BUF];
    memset (outbuf, 0, sizeof outbuf);
    gsize nrec = gtkhx_proto_shortcode_matches (
        (const uint8_t *) prefix, strlen (prefix), outbuf, sizeof outbuf - 1,
        TA_MAX_MATCHES);
    g_free (prefix);

    if (nrec == 0) {
        ta_hide (ta);
        return;
    }

    ta->tok_start_off = colon_off;
    ta_populate (ta, (const char *) outbuf, nrec);
    ta_position (ta);
    if (!ta->open) {
        ta->open = TRUE;
        gtk_popover_popup (GTK_POPOVER (ta->popover));
    }
}

/* Replace the `:prefix` token with the selected emoji glyph. */
static void
ta_commit (EmojiTypeahead *ta)
{
    GtkListBoxRow *row
        = gtk_list_box_get_selected_row (GTK_LIST_BOX (ta->listbox));
    const char *emoji
        = row ? g_object_get_data (G_OBJECT (row), "hx-emoji") : NULL;
    if (!emoji) {
        ta_hide (ta);
        return;
    }

    GtkTextBuffer *buf = gtk_text_view_get_buffer (ta->view);
    GtkTextIter start, cur;
    gtk_text_buffer_get_iter_at_offset (buf, &start, ta->tok_start_off);
    gtk_text_buffer_get_iter_at_mark (buf, &cur, gtk_text_buffer_get_insert (buf));

    ta->suppress = TRUE;
    gtk_text_buffer_begin_user_action (buf);
    gtk_text_buffer_delete (buf, &start, &cur); /* drop ":prefix" */
    gtk_text_buffer_insert (buf, &start, emoji, -1); /* insert the glyph */
    gtk_text_buffer_end_user_action (buf);
    ta->suppress = FALSE;

    ta_hide (ta);
    gtk_widget_grab_focus (GTK_WIDGET (ta->view));
}

static void
ta_move (EmojiTypeahead *ta, int delta)
{
    GtkListBox *lb = GTK_LIST_BOX (ta->listbox);
    GtkListBoxRow *sel = gtk_list_box_get_selected_row (lb);
    int idx = sel ? gtk_list_box_row_get_index (sel) : -1;
    GtkListBoxRow *next = gtk_list_box_get_row_at_index (lb, idx + delta);
    if (next) {
        gtk_list_box_select_row (lb, next);
    }
}

static gboolean
ta_key_pressed (GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                GdkModifierType state, gpointer user_data)
{
    EmojiTypeahead *ta = user_data;
    (void) ctrl;
    (void) keycode;
    (void) state;

    if (!ta->open) {
        return FALSE; /* dormant — let the chat input's own handler run */
    }
    switch (keyval) {
    case GDK_KEY_Down:
        ta_move (ta, +1);
        return TRUE;
    case GDK_KEY_Up:
        ta_move (ta, -1);
        return TRUE;
    case GDK_KEY_Tab:
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        ta_commit (ta);
        return TRUE;
    case GDK_KEY_Escape:
        ta_hide (ta);
        return TRUE;
    default:
        return FALSE;
    }
}

static void
ta_row_activated (GtkListBox *lb, GtkListBoxRow *row, gpointer user_data)
{
    EmojiTypeahead *ta = user_data;
    gtk_list_box_select_row (lb, row);
    ta_commit (ta);
}

static void
ta_on_changed (GtkTextBuffer *buf, gpointer user_data)
{
    (void) buf;
    ta_update ((EmojiTypeahead *) user_data);
}

static void
ta_on_cursor (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void) obj;
    (void) pspec;
    ta_update ((EmojiTypeahead *) user_data);
}

void
hx_emoji_typeahead_attach (GtkWidget *target_text_view)
{
    g_return_if_fail (GTK_IS_TEXT_VIEW (target_text_view));

    /* Idempotent: a second attach on the same view would g_free the first
	 * state (via set_data_full's destroy) while leaving its signal handlers
	 * and key controller live, dangling their user_data → use-after-free on
	 * the next keystroke. Bail if already attached. */
    if (g_object_get_data (G_OBJECT (target_text_view), "hx-emoji-typeahead")) {
        return;
    }

    EmojiTypeahead *ta = g_new0 (EmojiTypeahead, 1);
    ta->view = GTK_TEXT_VIEW (target_text_view);

    ta->popover = gtk_popover_new ();
    gtk_widget_set_parent (ta->popover, target_text_view);
    /* Don't steal focus — the user keeps typing into the input while the
     * suggestions track along; we drive selection/commit ourselves. */
    gtk_popover_set_autohide (GTK_POPOVER (ta->popover), FALSE);
    gtk_popover_set_has_arrow (GTK_POPOVER (ta->popover), FALSE);
    gtk_popover_set_position (GTK_POPOVER (ta->popover), GTK_POS_BOTTOM);

    GtkWidget *scroller = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroller),
                                                220);
    gtk_scrolled_window_set_propagate_natural_height (
        GTK_SCROLLED_WINDOW (scroller), TRUE);

    ta->listbox = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (ta->listbox),
                                     GTK_SELECTION_BROWSE);
    gtk_widget_add_css_class (ta->listbox, "menu");
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), ta->listbox);
    gtk_popover_set_child (GTK_POPOVER (ta->popover), scroller);

    g_signal_connect (ta->listbox, "row-activated",
                      G_CALLBACK (ta_row_activated), ta);

    GtkTextBuffer *buf = gtk_text_view_get_buffer (ta->view);
    g_signal_connect (buf, "changed", G_CALLBACK (ta_on_changed), ta);
    g_signal_connect (buf, "notify::cursor-position",
                      G_CALLBACK (ta_on_cursor), ta);

    /* Capture phase so nav/commit/dismiss keys are seen before the chat
     * input's bubble-phase handler — but only consumed while open. */
    GtkEventController *kc = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (kc, GTK_PHASE_CAPTURE);
    g_signal_connect (kc, "key-pressed", G_CALLBACK (ta_key_pressed), ta);
    gtk_widget_add_controller (target_text_view, kc);

    /* The view owns the state; GTK frees the parented popover with the
     * view, so teardown is just g_free. */
    g_object_set_data_full (G_OBJECT (target_text_view), "hx-emoji-typeahead",
                            ta, g_free);
}
