/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <adwaita.h>
#include <gdk/gdkkeysyms.h>
#include "hx.h"
#include "hxconn.h"
#include "chat.h"
#include "chat_members.h"
#include "chat_tabs.h"
#include "emoji.h"
#include "hotline_proto.h"
#include "network.h"
#include "proto_helpers.h"
#include "gtkhx.h"
#include "gtkhx_log.h"
#include "gtkutil.h"
#include "chat_view.h"
#include "rcv.h"
#include "tasks.h"
#include "connect.h"
#include "notify.h"
#include "sound.h"
#include "toolbar.h"
#include "users.h"
#include "cicn.h"
#include "gtkurl.h"
#include "msg.h"

void
hx_send_msg (struct htlc_conn *htlc, guint16 uid, const char *msg, guint16 len,
             void *data)
{
    /* Phase E2/E3: body field — UTF-8 / Mac Roman conversion plus
	 * LF→CR for legacy servers. See [[gtkhx_text_for_wire]] in
	 * src/text_util.c. */
    gboolean utf8 = (hx_conn_has_cap (htlc, HTLC_CAP_TEXT_ENCODING)) != 0;
    gsize wire_len = 0;
    char *wire
        = gtkhx_text_for_wire (msg, len, utf8, /*is_body=*/TRUE, &wire_len);

    /* chunk layout moved to gtkhx_proto_build_msg_chunks.
	 * Build chunks BEFORE registering the task — task_new() snapshots
	 * the current htlc->trans into a new task table entry (which then
	 * waits for the server's matching TASK reply); the actual increment
	 * of htlc->trans happens later inside hlpack_chunks during packing.
	 * If we registered the task first and the builder then failed
	 * (validation reject), hlwrite_chunks would be skipped — leaving a
	 * pending task with no on-wire request to reply to and hanging the
	 * tasks UI. Build, register, send is the safe order. */
    struct hx_chunk chunks[2];
    guint8 scratch[2];
    int hc = (int)gtkhx_proto_build_msg_chunks (uid, (const uint8_t *)wire,
                                                wire_len, chunks,
                                                G_N_ELEMENTS (chunks), scratch,
                                                sizeof (scratch));
    if (hc > 0) {
        task_new (htlc, RCV_TASK_FN (rcv_task_msg), data, 0,
                  data ? data : "msg");
        hlwrite_chunks (htlc, HTLC_HDR_MSG, 0, chunks, hc);
    }
    g_free (wire);
}

/* hx_send_broadcast (the admin-broadcast wire sender) moved to Rust
 * (gtkhx-ui broadcast.rs) alongside the Broadcast composer, its only
 * caller. */

/* msgwin lifecycle on GHashTable.
 *
 * msgwin_free() is the GDestroyNotify the hashtable invokes when an
 * entry is removed or the table itself is destroyed. Mirrors the
 * heap-cleanup that the old msgwin_delete linked-list unhook used to
 * do (name + the heap-allocated uid pointer + the struct itself).
 * The GTK widgets owned by msg->window are reclaimed by GTK's own
 * teardown when the window is destroyed by the close-request
 * handler. msg->history (a Rust InputHistory) is freed below. */
static void
msgwin_free (gpointer p)
{
    struct msgwin *msg = p;
    if (!msg) {
        return;
    }
    g_free (msg->name);
    g_free (msg->uid);
    /* The PM input line history is now a Rust InputHistory (owns its draft
     * internally); free it (no more leak). */
    if (msg->history) {
        hx_input_history_free (msg->history);
        msg->history = NULL;
    }
    g_free (msg);
}

/* Forward decl so msg_windows_init can wire it as the chat-tabs
 * close handler before destroy_msgwin's old static destroy_msgwin
 * function (now retired) was defined. */
static void msg_tab_on_close (guint16 uid);

void
msg_windows_init (session *sess)
{
    if (!sess->msg_windows) {
        sess->msg_windows = g_hash_table_new_full (
            g_direct_hash, g_direct_equal, NULL, msgwin_free);
    }
    /* register the close-tab dispatcher target
     * so user-clicks on a msg tab's X end up calling msgwin_delete.
     * Idempotent — re-registers the same callback on each session
     * init. */
    gtkhx_chat_tabs_set_close_msg_handler (msg_tab_on_close);
}

/* Unhook the window from the per-session table; the value-destroy
 * notify (msgwin_free) reclaims the msgwin. Called from
 * destroy_msgwin (the close-request handler). */
static void
msgwin_delete (struct msgwin *msg)
{
    session *sess = hx_active_session ();
    if (!msg || !sess->msg_windows) {
        return;
    }
    g_hash_table_remove (sess->msg_windows,
                         GUINT_TO_POINTER ((guint)*msg->uid));
}

struct msgwin *
msgwin_with_uid (guint16 uid)
{
    session *sess = hx_active_session ();
    if (!sess->msg_windows) {
        return NULL;
    }
    return g_hash_table_lookup (sess->msg_windows,
                                GUINT_TO_POINTER ((guint)uid));
}

static void msg_input_activate (GtkWidget *widget, gpointer data);

/* GTK 4 widgets don't emit key-press-event; keyboard input
 * comes via a GtkEventControllerKey installed on the widget. The
 * "key-pressed" signal carries (controller, keyval, keycode, state,
 * user_data). Returning TRUE inhibits further propagation, FALSE lets
 * the GtkTextView's default text input proceed (used here for the
 * Shift+Return → newline path and for ordinary typing). */
static gboolean
msg_input_key_pressed (GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                       GdkModifierType state, gpointer user_data)
{
    GtkWidget *widget
        = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (ctrl));
    GtkTextView *text;
    GtkTextBuffer *buf;
    struct msgwin *msg = g_object_get_data (G_OBJECT (widget), "msg");
    (void)keycode;
    (void)user_data;

    if (!msg) {
        return FALSE;
    }

    text = GTK_TEXT_VIEW (widget);
    buf = gtk_text_view_get_buffer (text);

    if (state & GDK_CONTROL_MASK) {
        switch (keyval) {
        case 'k':
        case 'K':
            create_connect_window (0, hx_active_session ());
            return TRUE;
        }
    } else if ((state & GDK_SHIFT_MASK) && keyval == GDK_KEY_Return) {
        /* Insert a linebreak if shift is held — let GtkTextView default. */
        return FALSE;
    } else if (keyval == GDK_KEY_Return) {
        GtkTextIter start, end;
        char *line;

        gtk_text_buffer_get_start_iter (buf, &start);
        gtk_text_buffer_get_end_iter (buf, &end);
        line = gtk_text_buffer_get_text (buf, &start, &end, FALSE);
        hx_input_history_record (msg->history, line);
        g_free (line);

        msg_input_activate (widget, msg->uid);
        return TRUE;
    } else if (keyval == GDK_KEY_Up) {
        /* Draft snapshot + restore live in the Rust InputHistory (see chat.c);
         * pass the current buffer in, a non-NULL result is the line to show. */
        GtkTextIter s, e;
        char *cur, *nt = NULL;

        gtk_text_buffer_get_start_iter (buf, &s);
        gtk_text_buffer_get_end_iter (buf, &e);
        cur = gtk_text_buffer_get_text (buf, &s, &e, FALSE);
        if (hx_input_history_up (msg->history, cur, &nt)) {
            GtkTextIter end;
            gtk_text_buffer_set_text (buf, nt, -1);
            gtk_text_buffer_get_end_iter (buf, &end);
            gtk_text_buffer_place_cursor (buf, &end);
            g_free (nt);
            g_free (cur);
            return TRUE;
        }
        g_free (cur);
    } else if (keyval == GDK_KEY_Down) {
        char *nt = NULL;

        if (hx_input_history_down (msg->history, &nt)) {
            GtkTextIter end;
            gtk_text_buffer_set_text (buf, nt, -1);
            gtk_text_buffer_get_end_iter (buf, &end);
            gtk_text_buffer_place_cursor (buf, &end);
            g_free (nt);
            return TRUE;
        }
    }

    return FALSE;
}

/* msg_update_trans was a configure-event handler that
 * forced an xtext refresh on resize so transparency would track the
 * new window position. configure-event is gone in GTK 4, and Wayland
 * doesn't expose true window-relative transparency anyway. */

static void
msg_input_activate (GtkWidget *widget, gpointer data)
{
    GtkTextView *text;
    GtkTextBuffer *buf;
    GtkTextIter start, end;
    guint len;
    char *termed_buf = NULL;
    guint16 *uid = data;

    text = GTK_TEXT_VIEW (widget);
    buf = gtk_text_view_get_buffer (text);
    gtk_text_buffer_get_start_iter (buf, &start);
    gtk_text_buffer_get_end_iter (buf, &end);
    termed_buf = gtk_text_buffer_get_text (buf, &start, &end, FALSE);

    gtk_text_buffer_delete (buf, &start, &end);

    if (termed_buf[0] == 0) {
        g_free (termed_buf);
        return;
    }

    len = strlen (termed_buf);
    msg_output (hx_conn_name (hx_active_session ()->htlc), *uid, termed_buf);
    LF2CR (termed_buf, len);
    hx_send_msg (hx_active_session ()->htlc, *uid, termed_buf, len, 0);
    g_free (termed_buf);
}

/* header pane above the PM chat that mirrors the bits of
 * the recipient's identity that the global user list shows — icon,
 * name, idle/admin status. Built from the member model entry the chat
 * keeps for that uid; falls back to the name we were created with
 * (and uid only) when the user has already left the public chat by
 * the time the PM window opens.
 *
 * Colour: regular users (slot 0) intentionally don't get a foreground
 * override, so the GTK theme's default colour kicks in and reads on
 * both light and dark themes — same rule user_color_gdk() applies in
 * the user list. Admins / idle / admin-idle get the matching colour
 * from gdk_user_colors[] applied as a Pango <span> on the bold name.
 *
 * Icon: load_icon() yields a GdkPixbuf out of the Mac-classic cicn
 * resource bundle. We feed it to GtkImage at default 32px pixel size,
 * scaled if the source isn't already that. When the icon is unknown
 * (server iconset doesn't ship it, or user picked an icon we don't
 * have) the image clears to nothing so the layout doesn't sag. */
/* Low-level renderer: take the recipient's display name + status
 * triple as explicit values and paint the info pane. Both public
 * entry points below funnel through here. `display_name == NULL` and
 * `have_status == FALSE` together mean "we don't know who this
 * person is" — render the bare uid + fallback name from create_msgwin
 * with no Admin/Guest line. */
static void
msg_apply_user_view (struct msgwin *msg, const char *display_name, guint16 icon,
                     guint16 color, gboolean have_status)
{
    GdkRGBA *rgba = user_color_gdk (color);
    char *name_esc;
    char *markup;
    GdkPixbuf *pixbuf = NULL;
    GdkPixbuf *unused_mask = NULL;

    if (!msg || !msg->info_label || !msg->info_image) {
        return;
    }
    if (!display_name || !*display_name) {
        display_name = msg->name ? msg->name : "";
    }

    name_esc = g_markup_escape_text (display_name, -1);

    if (have_status) {
        const char *status = (color >= 2) ? _ ("Admin") : _ ("Guest");
        const char *away = (color % 2) ? _ (" (Away)") : "";
        if (rgba) {
            /* gdk_user_colors stores values in [0..1] floats;
			 * convert to the 8-bit hex Pango wants. */
            char hex[8];
            g_snprintf (hex, sizeof (hex), "#%02x%02x%02x",
                        (int)(rgba->red * 255.0 + 0.5),
                        (int)(rgba->green * 255.0 + 0.5),
                        (int)(rgba->blue * 255.0 + 0.5));
            markup = g_strdup_printf (
                "<span foreground=\"%s\"><b>%s</b></span>\n"
                "<small>UID %u · Icon %u · %s%s</small>",
                hex, name_esc, *msg->uid, icon, status, away);
        } else {
            markup = g_strdup_printf ("<b>%s</b>\n"
                                      "<small>UID %u · Icon %u · %s%s</small>",
                                      name_esc, *msg->uid, icon, status, away);
        }
    } else {
        markup = g_strdup_printf ("<b>%s</b>\n<small>UID %u</small>", name_esc,
                                  *msg->uid);
    }

    gtk_label_set_markup (GTK_LABEL (msg->info_label), markup);
    g_free (markup);
    g_free (name_esc);

    /* Always reload — icon ID can change when the user changes their
	 * icon mid-conversation. load_icon falls back through the icon
	 * file chain; pixbuf comes back NULL when nothing matches and we
	 * just blank the GtkImage in that case. GTK 4 deprecates
	 * gtk_image_set_from_pixbuf; wrap the pixbuf in a GdkTexture via
	 * gtkhx_texture_from_pixbuf (the non-deprecated gdk_memory_
	 * texture_new helper) and feed it to set_from_paintable. */
    load_icon (msg->info_image, icon, &icon_files, 1, &pixbuf, &unused_mask);
    if (pixbuf) {
        /* load_icon transfers ownership of the freshly-allocated
		 * pixbuf to us. gtkhx_texture_from_pixbuf takes its own
		 * ref (via the GBytes free_func that holds the pixbuf
		 * alive for the texture's lifetime), so we always drop
		 * our reference — both on the success path AND on the
		 * texture-conversion-failed path, otherwise every
		 * msg_apply_user_view refresh would leak one pixbuf.
		 * (The pre-migration code had the same shape and the
		 * same leak; fixing it here as part of the texture
		 * conversion review.) */
        GdkTexture *tex = gtkhx_texture_from_pixbuf (pixbuf);
        if (tex) {
            gtk_image_set_from_paintable (GTK_IMAGE (msg->info_image),
                                          GDK_PAINTABLE (tex));
            gtk_image_set_pixel_size (GTK_IMAGE (msg->info_image), 32);
            g_object_unref (tex);
        } else {
            gtk_image_clear (GTK_IMAGE (msg->info_image));
        }
        g_object_unref (pixbuf);
    } else {
        gtk_image_clear (GTK_IMAGE (msg->info_image));
    }
}

void
msgwin_refresh_user_info (struct msgwin *msg)
{
    struct chat *pubchat;
    struct hx_member_info mi;

    if (!msg) {
        return;
    }

    /* The user list is per-chat; the public chat (cid=0) carries the
     * server-wide membership we want here. chat_with_cid is the canonical
     * "global user list" lookup; the pubchat + member_model NULL checks
     * below cover the mid-disconnect case. */
    pubchat = chat_with_cid (hx_active_session (), 0);

    if (pubchat
        && hx_member_model_get_info (hx_chat_member_model (pubchat), *msg->uid, &mi)) {
        msg_apply_user_view (msg, mi.name, mi.icon, mi.status, TRUE);
    } else {
        msg_apply_user_view (msg, NULL, 0, 0, FALSE);
    }
}

/* Bypass the cache lookup. Called from users.c::user_change with the
 * NEW name/icon/color values straight off the wire — at that point
 * rcv.c hasn't yet applied them to the chat's member model (the
 * rename-detection comparison needs the old values
 * to still be there when user_change returns), so a cache-based
 * refresh would render stale data. Take the new values directly. */
void
msgwin_apply_user_change (struct msgwin *msg, const char *nam, guint16 icon,
                          guint16 color)
{
    msg_apply_user_view (msg, nam, icon, color, TRUE);
}

/* Non-static since the Rust create_msgwin (gtkhx-ui `msg`) calls it to build
 * the model + leaf widgets before assembling the PM tab layout. */
struct msgwin *
create_msg (guint16 _uid, char *name)
{
    struct msgwin *msg;
    guint16 *uid = g_malloc (sizeof (guint16));
    *uid = _uid;

    /* g_malloc0: see chat.c, same reason — history_draft must be
     * NULL before the first g_free(msg->history_draft). */
    msg = g_malloc0 (sizeof (struct msgwin));

    msg->name = g_strdup (name);
    msg->uid = uid;

    msg->history = hx_input_history_new ();

    /* msg->window is no longer a standalone
     * GtkWindow. It's the content widget of an AdwTabPage inside
     * the Chat panel's tab view; create_msgwin builds the layout
     * vbox and points msg->window at it after gtkhx_chat_tabs_add_msg.
     * Until then it stays NULL — create_msg only builds the
     * sub-widgets (xtext + input + ctrl). */
    {
        gchar *fontname = pango_font_description_to_string (gtkhx_font_desc);
        msg->outputbuf = hx_chat_view_new (colors, TRUE);
        hx_chat_view_set_font (msg->outputbuf, fontname);
        g_free (fontname);
    }
    hx_chat_view_set_word_wrap (msg->outputbuf, gtkhx_prefs.word_wrap);
    hx_chat_view_set_urlcheck_function (msg->outputbuf, word_check);
    hx_chat_view_set_max_lines (msg->outputbuf, gtkhx_prefs.xbuf_max);
    /* view-native timestamps — see chat.c::create_chat_window
	 * for the rationale. */
    hx_chat_view_set_indent (msg->outputbuf, TRUE);
    hx_chat_view_set_time_stamp (msg->outputbuf, gtkhx_prefs.timestamp);
    hx_chat_view_set_max_indent (msg->outputbuf, 256);
    g_signal_connect (msg->outputbuf, "word_click",
                      G_CALLBACK (gtkurl_xtext_word_click), NULL);

    msg->vscroll = gtk_scrollbar_new (
        GTK_ORIENTATION_VERTICAL,
        hx_chat_view_get_vadjustment (msg->outputbuf));
    msg->inputbuf = gtk_text_view_new ();

    /* Theme monospace via gtk_text_view_set_monospace — see chat.c for
	 * the rationale and gtkhx_apply_input_font for the implementation. */
    gtkhx_apply_input_font (msg->inputbuf);
    /* GtkHx-theme fg/bg via .gtkhx-input. */
    gtkhx_apply_input_style (msg->inputbuf);
    gtk_text_view_set_editable (GTK_TEXT_VIEW (msg->inputbuf), TRUE);
    gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (msg->inputbuf), GTK_WRAP_WORD);
    /* Inner margins so the text isn't clipped by the input frame's
	 * rounded corners — same fix as the chat inputs. */
    gtk_text_view_set_left_margin (GTK_TEXT_VIEW (msg->inputbuf), 6);
    gtk_text_view_set_right_margin (GTK_TEXT_VIEW (msg->inputbuf), 6);
    gtk_text_view_set_top_margin (GTK_TEXT_VIEW (msg->inputbuf), 4);
    gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (msg->inputbuf), 4);

    g_object_set_data (G_OBJECT (msg->inputbuf), "msg", msg);
    g_object_set_data (G_OBJECT (msg->inputbuf), "sess", hx_active_session ());
    /* Note: GtkTextView has no "activate" signal — Return is dispatched
	 * from msg_input_key_pressed, which calls msg_input_activate().
	 * key-press-event is gone in GTK 4; install a
	 * GtkEventControllerKey on the input view instead. */
    {
        GtkEventController *kctrl = gtk_event_controller_key_new ();
        g_signal_connect (kctrl, "key-pressed",
                          G_CALLBACK (msg_input_key_pressed), uid);
        gtk_widget_add_controller (msg->inputbuf, kctrl);
    }

    /* stash the msgwin in the session's PM-windows table
	 * keyed on uid. msg_windows_init() at startup guarantees the
	 * table exists by the time we land here. */
    g_hash_table_insert (hx_active_session ()->msg_windows,
                         GUINT_TO_POINTER ((guint)_uid), msg);
    return msg;
}

/* Rides AdwTabView::close-page via the chat_tabs dispatcher, which
 * calls us with the uid of the tab being closed. msgwin_delete drops
 * the entry from sess->msg_windows; the value-destroy (msgwin_free)
 * reclaims the struct. AdwTabView destroys the page + content widget
 * tree as part of close-page-finish.
 *
 * Registered once at startup by msg_windows_init via
 * gtkhx_chat_tabs_set_close_handlers. */
static void
msg_tab_on_close (guint16 uid)
{
    struct msgwin *msg = msgwin_with_uid (uid);
    if (msg != NULL) {
        /* Unparent the emoji typeahead popover from the input before
         * AdwTabView disposes the content tree — otherwise
         * GtkTextView::dispose spins forever on the foreign popover
         * child and the app freezes. See hx_emoji_typeahead_detach. */
        if (msg->inputbuf) {
            hx_emoji_typeahead_detach (msg->inputbuf);
        }
        msgwin_delete (msg);
    }
}

/* Widget accessors + setters for the Rust create_msgwin (gtkhx-ui `msg`),
 * which builds the PM tab layout around the leaf widgets create_msg made.
 * The leaf widgets (xtext output, scrollbar, input) come out; the assembled
 * window + the info-pane widgets Rust creates go back in. */
GtkWidget *
hx_msgwin_outputbuf (struct msgwin *msg)
{
    return msg ? msg->outputbuf : NULL;
}
GtkWidget *
hx_msgwin_vscroll (struct msgwin *msg)
{
    return msg ? msg->vscroll : NULL;
}
GtkWidget *
hx_msgwin_inputbuf (struct msgwin *msg)
{
    return msg ? msg->inputbuf : NULL;
}
void
hx_msgwin_set_window (struct msgwin *msg, GtkWidget *w)
{
    if (msg) {
        msg->window = w;
    }
}
void
hx_msgwin_set_info_image (struct msgwin *msg, GtkWidget *w)
{
    if (msg) {
        msg->info_image = w;
    }
}
void
hx_msgwin_set_info_label (struct msgwin *msg, GtkWidget *w)
{
    if (msg) {
        msg->info_label = w;
    }
}

/* Render a private message into its msgwin's xtext, with the
 * "<name>" coloured nick prefix prepended.
 *
 * this is the shared body for both msg_output (legacy
 * raw-strings call site — kept for outgoing-msg / xfer-status
 * code paths that hand-roll a name + body string pair) and
 * msg_output_from_event (the msg-signal path that has the
 * pre-parsed HxMsgEvent). */
static void
msg_output_render (const char *name, guint16 uid, const char *body,
                   gboolean is_self)
{
    struct msgwin *msg;
    gint16 brack_col;
    gchar *valid_body;
    gsize valid_body_len;
    const char *cur;
    const char *end;

    msg = msgwin_with_uid (uid);
    if (!msg) {
        msg = create_msgwin (uid, (char *)name);
    }

    /* mIRC colour 13 (pink) for our own messages, 12 (light blue)
	 * for incoming. */
    brack_col = is_self ? 13 : 12;

    /* Validate the body bytes once. the chat view hands content to Pango,
	 * which asserts UTF-8 — and PM bodies can arrive in Mac Roman
	 * from vintage servers. */
    valid_body = gtkhx_text_to_utf8 (body ? body : "", body ? strlen (body) : 0,
                                     &valid_body_len);
    if (!valid_body) {
        return;
    }

    /* Each newline-separated line in the body becomes its own
	 * xtext entry. The first one carries the nick column via
	 * hx_chat_view_append_indent (two-column layout — names
	 * on the left, message on the right, with the auto-aligned
	 * separator the chat output uses); subsequent lines append
	 * as plain continuation rows so multi-line messages don't
	 * repeat the nick column on every line. */
    cur = valid_body;
    end = valid_body + valid_body_len;
    gboolean first = TRUE;
    while (cur <= end) {
        const char *nl = (cur < end) ? memchr (cur, '\n', end - cur) : NULL;
        gsize seg_len = nl ? (gsize)(nl - cur) : (gsize)(end - cur);
        if (first) {
            /* "<name>": brackets coloured, name in the default
			 * foreground. Same shape as chat.c's nick column, and
			 * the same three runs. */
            const char *nam = name ? name : "";
            HxChatRun gutter[3] = {
                { "<", 1, brack_col, HX_CHAT_ATTR_NONE },
                HX_CHAT_RUN_PLAIN (nam, (int)strlen (nam)),
                { ">", 1, brack_col, HX_CHAT_ATTR_NONE },
            };
            HxChatRun body_run = HX_CHAT_RUN_PLAIN (cur, (int)seg_len);
            hx_chat_view_append_runs (msg->outputbuf, gutter, 3, &body_run, 1,
                                      0);
            first = FALSE;
        } else {
            hx_chat_view_append (msg->outputbuf, cur, seg_len, 0);
        }
        if (!nl) {
            break;
        }
        cur = nl + 1;
    }

    g_free (valid_body);

    /* incoming messages set needs-attention on
     * the tab + the Chat dock panel so the user notices a new PM
     * arrived while they're elsewhere. Self-echoes (the user just
     * typed) skip the indicator — no need to flag yourself. */
    if (!is_self) {
        gtkhx_chat_tabs_set_attention_msg (uid, TRUE);
    }
}

void
msg_output (char *name, guint16 uid, char *buf)
{
    gboolean is_self = name && hx_conn_name (hx_active_session ()->htlc)[0]
                       && strcmp (name, hx_conn_name (hx_active_session ()->htlc)) == 0;
    msg_output_render (name, uid, buf, is_self);
}

void
msg_output_from_event (HxMsgEvent *event)
{
    if (!event) {
        return;
    }
    msg_output_render (event->name, event->uid, event->body, event->is_self);
}

/* short broadcasts go through toolbar_show_toast, long ones
 * through an AdwAlertDialog with a scrolled GtkTextView extra child.
 *
 * The split exists because servers use HTLS_HDR_MSG_BROADCAST for two
 * different things:
 *   - Real admin announcements ("Server going down in 5 minutes")
 *     that the user really should see and acknowledge.
 *   - Rate-limit / auto-rejection notes ("0 command(s) at a time,
 *     please.") that are short, automated, and not worth a modal
 *     dialog the user has to click through.
 *
 * Length is a usable proxy: short = automated nag, long = real
 * announcement. The 160-char cutoff is heuristic but matches the
 * shape of what we've seen in the wild on hlserver.com et al. */
#define BROADCAST_TOAST_MAX 160

/* Map the legacy user->color (16-bit, % 4 status field) to a mIRC
 * palette index so we can wrap a name in `\003NN…\003` for xtext.
 * Aligned with gdk_user_colors[]:
 *   0 (regular) → no escape; let xtext use HX_CHAT_PAL_FG so the name
 *                 stays legible against both light and dark bgs
 *                 (black on a black bg would be invisible).
 *   1 (idle)    → mIRC 14, grey
 *   2 (admin)   → mIRC  4, red
 *   3 (idle adm)→ mIRC 13, pink */
static const char *
broadcast_name_mirc_color (guint16 color)
{
    switch (color % 4) {
    case 2:
        return "04";
    case 3:
        return "13";
    case 1:
        return "14";
    case 0:
    default:
        return NULL;
    }
}

/* Defang a sender name before embedding it in the mIRC-coded
 * broadcast prefix. The wrapper structure is
 *
 *     " \00310[\003<col><name>\00310]\003 "
 *
 * and the chat-side detector in xprintline scans for the closing
 * "\00310]\003 " sequence to find where the name ends. A name
 * containing raw \003 (or any other xtext control byte from the
 * ATTR_* family — \002, \007, \017, \026, \031) could prematurely
 * terminate the wrapper, break info-prefix detection, or smuggle
 * its own colour escapes into the chat log.
 *
 * The wire-parse helpers (hx_msg_extract → strip_ansi) translate
 * any low-control bytes in the body but NOT in the name, so a
 * hostile server could ship `name = "\003foo"` and have us
 * render escape codes inside the brackets. Strip every ASCII
 * control byte (< 0x20) defensively here. Legitimate Hotline
 * nicks are printable ASCII / UTF-8 — control bytes have no
 * business in one. */
static char *
broadcast_sanitise_name (const char *raw)
{
    GString *out;
    const char *p;

    if (!raw) {
        return NULL;
    }
    out = g_string_new (NULL);
    for (p = raw; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) {
            g_string_append_c (out, '?');
        } else {
            g_string_append_c (out, *p);
        }
    }
    return g_string_free (out, FALSE);
}

void
broadcastmsg (const char *sender_name, guint16 sender_color, char *text)
{
    AdwDialog *dialog;
    GtkWidget *textbox, *scroll;
    GtkTextBuffer *tbuf;
    gsize len = text ? strlen (text) : 0;

    /* notify-dispatch happens before the toast/alert so
	 * the user sees a system-level alert regardless of whether
	 * the broadcast renders as a transient toast or a modal
	 * dialog. */
    gtkhx_notify_broadcast (text);

    /* Broadcasts share the MSG chime with private messages. The "msg"
	 * signal (which the sound_events subscriber keys on) fires only for
	 * the private-message branch of hx_rcv_msg, not for broadcasts, so
	 * play it here to preserve the chime that used to fire inline for
	 * both branches. */
    play_sound (MSG);

    /* Log the broadcast to chat output. When the wire carried a
	 * sender name (mhxd-family servers echo broadcasts back with
	 * UID + NAME chunks), render as " [name] body" with the same
	 * blue brackets the INFOPREFIX uses and the name in the
	 * sender's user-color slot. When the sender is unknown (older
	 * servers, anonymous rate-limit nags), fall back to the
	 * legacy "[hx] broadcast: …" form so something still shows up
	 * in scrollback. Task errors come through task_error() →
	 * toolbar_show_toast directly and never reach this function,
	 * so they won't be logged as broadcasts here. */
    if (text && *text) {
        if (sender_name && *sender_name) {
            const char *col = broadcast_name_mirc_color (sender_color);
            char *safe_name = broadcast_sanitise_name (sender_name);
            char *prefix;
            if (col) {
                prefix = g_strdup_printf (" \00310[\003%s%s\00310]\003 ",
                                          col, safe_name);
            } else {
                prefix = g_strdup_printf (" \00310[\003%s\00310]\003 ",
                                          safe_name);
            }
            hx_printf_prefix (hx_active_session ()->htlc, 0, prefix, "%s\n", text);
            g_free (prefix);
            g_free (safe_name);
        } else {
            hx_printf_prefix (hx_active_session ()->htlc, 0, INFOPREFIX,
                              _ ("broadcast: %s\n"), text);
        }
    }

    if (len <= BROADCAST_TOAST_MAX && !strchr (text, '\n')) {
        toolbar_show_toast (text);
        return;
    }

    dialog = adw_alert_dialog_new (_ ("Broadcast"), NULL);
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "ok", _ ("_OK"));
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "ok");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "ok");

    textbox = gtk_text_view_new ();
    gtk_text_view_set_editable (GTK_TEXT_VIEW (textbox), FALSE);
    gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (textbox), FALSE);
    gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (textbox), GTK_WRAP_WORD);
    /* Long-broadcast viewer — read-only, themed via .gtkhx-text so
     * the body matches the rest of the chat surfaces. */
    gtkhx_apply_text_style (textbox);
    tbuf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (textbox));
    gtk_text_buffer_set_text (tbuf, text, strlen (text));

    scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request (scroll, 300, 220);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), textbox);

    adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), scroll);

    gtkhx_dialog_add_close_shortcuts (GTK_WIDGET (dialog));

    adw_dialog_present (dialog,
                        toolbar_window ? GTK_WIDGET (toolbar_window) : NULL);
}
