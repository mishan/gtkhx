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
 */

/*
 * users_bridge.c — see users_bridge.h. The C content leaves of the Users
 * panel, extracted verbatim from the old users.c::create_users_window so
 * the gtk4-rs shell can build the panel content tree + register it via
 * dock_bridge.
 */

#include "config.h"

#include <gtk/gtk.h>

#include "hx.h"
#include "hxconn.h"
#include "users.h"
#include "users_view.h"
#include "gtkhx_theme.h"
#include "gtkutil.h" /* gtkhx_pixmap_button, gtkhx_widget_set_child */
#include "gtkhx.h"   /* gtkhx_refresh_userlist_css */
#include "network.h" /* connected */
#ifdef HAVE_VOICE
#include "voice_panel.h"
#endif
#include "users_bridge.h"
#include "panel_registry.h"

static GtkWidget *
build_view (session *sess)
{
    GtkWidget *scroll;
    GtkWidget *cv_widget;
    HxUserListView *view;

    g_return_val_if_fail (sess != NULL, NULL);

    /* HxUserListView wraps a GtkColumnView with a custom snapshot()-
     * rendered Name cell (icon-as-background + name-on-top). STYLE_USERS
     * picks the standalone chrome: 24-px rows, 1.25× pixel scale, text
     * outline, 36-px text offset. The view installs its own right-click
     * gesture that pops user_popup. */
    /* The standalone Users window always lists the public chat (cid 0). */
    view = hx_user_list_view_new (sess, 0, HX_USER_LIST_STYLE_USERS);
    cv_widget = hx_user_list_view_get_widget (view);

    if (!users_font_desc) {
        users_font_desc = pango_font_description_from_string ("Sans 10");
    }
    /* Refresh the CSS provider that paints the .gtkhx-userlist class so
     * the global font / fg / bg tracking covers our column-view cells.
     * The view applied the class to its column_view at construction. */
    gtkhx_refresh_userlist_css (users_font_desc);

    scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_widget_set_vexpand (scroll, TRUE);
    gtkhx_widget_set_child (scroll, cv_widget);

    sess->users_view = view;
    return scroll;
}

static GtkWidget *
build_button_bar (session *sess)
{
    GtkWidget *button_bar;
    HxUserListView *view;

    g_return_val_if_fail (sess != NULL, NULL);
    view = sess->users_view;
    g_return_val_if_fail (view != NULL, NULL);

    /* Per-user action buttons. `data' is the HxUserListView itself — the
     * view_*_btn handlers read its current single-selection and its
     * borrowed session pointer. Same handlers drive the chat.c pchat
     * sidebars. */
    sess->user_btns.msg = gtkhx_pixmap_button (
        "/com/nasledov/gtkhx/pixmaps/message.png", _ ("Msg"),
        GTKHX_SCALE_WINDOW_BUTTONS, G_CALLBACK (view_msg_btn), view);
    sess->user_btns.kick = gtkhx_pixmap_button (
        "/com/nasledov/gtkhx/pixmaps/kick.png", _ ("Kick"),
        GTKHX_SCALE_WINDOW_BUTTONS, G_CALLBACK (view_kick_btn), view);
    sess->user_btns.info = gtkhx_pixmap_button (
        "/com/nasledov/gtkhx/pixmaps/info.png", _ ("User Info"),
        GTKHX_SCALE_WINDOW_BUTTONS, G_CALLBACK (view_info_btn), view);
    sess->user_btns.ban = gtkhx_pixmap_button (
        "/com/nasledov/gtkhx/pixmaps/ban.png", _ ("Ban"),
        GTKHX_SCALE_WINDOW_BUTTONS, G_CALLBACK (view_ban_btn), view);
    sess->user_btns.chat = gtkhx_pixmap_button (
        "/com/nasledov/gtkhx/pixmaps/chat.png", _ ("Private Chat"),
        GTKHX_SCALE_WINDOW_BUTTONS, G_CALLBACK (view_chat_btn), view);
    sess->user_btns.ignore = gtkhx_pixmap_button (
        "/com/nasledov/gtkhx/pixmaps/ignore.png", _ ("Ignore"),
        GTKHX_SCALE_WINDOW_BUTTONS, G_CALLBACK (view_igno_btn), view);

    gtk_widget_set_sensitive (sess->user_btns.msg, FALSE);
    gtk_widget_set_sensitive (sess->user_btns.kick, FALSE);
    gtk_widget_set_sensitive (sess->user_btns.info, FALSE);
    gtk_widget_set_sensitive (sess->user_btns.ban, FALSE);
    gtk_widget_set_sensitive (sess->user_btns.chat, FALSE);
    gtk_widget_set_sensitive (sess->user_btns.ignore, FALSE);

    /* The old standalone Users headerbar held Msg / Chat on the start
     * and Ignore / Ban / Kick / Info on the end. A PanelWidget's tab
     * strip is too narrow to host action buttons, so they relocate to a
     * slim GtkBox at the top of the panel content. Spacing + halign keep
     * the start / end grouping the headerbar layout implied. */
    button_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start (button_bar, 6);
    gtk_widget_set_margin_end (button_bar, 6);
    gtk_widget_set_margin_top (button_bar, 6);
    gtk_widget_set_margin_bottom (button_bar, 4);
    gtk_box_append (GTK_BOX (button_bar), sess->user_btns.msg);
    gtk_box_append (GTK_BOX (button_bar), sess->user_btns.chat);
#ifdef HAVE_VOICE
    /* Public-room (cid 0) voice Join/Leave + Mute icon controls. The
     * controls live with the user list rather than the chat window;
     * hidden entirely unless the server echoed HTLC_CAP_VOICE. Grouped
     * with Msg/Chat on the start side. */
    gtk_box_append (GTK_BOX (button_bar), voice_panel_new (sess, 0));
#endif
    {
        GtkWidget *spacer = gtk_label_new (NULL);
        gtk_widget_set_hexpand (spacer, TRUE);
        gtk_box_append (GTK_BOX (button_bar), spacer);
    }
    gtk_box_append (GTK_BOX (button_bar), sess->user_btns.info);
    gtk_box_append (GTK_BOX (button_bar), sess->user_btns.kick);
    gtk_box_append (GTK_BOX (button_bar), sess->user_btns.ban);
    gtk_box_append (GTK_BOX (button_bar), sess->user_btns.ignore);

    return button_bar;
}

GtkWidget *
gtkhx_users_bridge_build_content (session *sess)
{
    GtkWidget *scroll;
    GtkWidget *button_bar;
    GtkWidget *content_vbox;

    g_return_val_if_fail (sess != NULL, NULL);

    /* build_view first: it stashes sess->users_view, which
     * build_button_bar reads for the button handler data. */
    scroll = build_view (sess);
    button_bar = build_button_bar (sess);

    content_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (content_vbox), button_bar);
    gtk_box_append (GTK_BOX (content_vbox), scroll);
    return content_vbox;
}

void
gtkhx_users_bridge_after_embed (session *sess)
{
    g_return_if_fail (sess != NULL);

    /* Auto-open + size persistence are layout-restore work. For now we
     * just mark the panel open and trust libpanel's sidebar-width
     * default. */
    hx_panel_mark_constructed (HX_PANEL_ID_USERS);

    /* This connection's post-login state, not the process-global `connected`
     * flag — which is one connection's state under a name that reads like the
     * application's, and would have a second connection's button bar come up
     * live because the first one happened to be.
     *
     * post-login rather than merely socket-up, because that is what `connected`
     * meant: it is set after the LOGIN reply. hx_conn_fd would be true from TCP
     * connect onward — and again during teardown, where -1 is parked
     * deliberately — so sensitizing on it would put a USER_LIST on the wire
     * mid-handshake. */
    if (hx_conn_post_login_fetched (sess->htlc)) {
        gtk_widget_set_sensitive (sess->user_btns.msg, TRUE);
        gtk_widget_set_sensitive (sess->user_btns.ban, TRUE);
        gtk_widget_set_sensitive (sess->user_btns.info, TRUE);
        gtk_widget_set_sensitive (sess->user_btns.kick, TRUE);
        gtk_widget_set_sensitive (sess->user_btns.chat, TRUE);
        gtk_widget_set_sensitive (sess->user_btns.ignore, TRUE);
        user_list (sess);
    }
}
