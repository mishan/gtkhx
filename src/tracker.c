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
#include "regex.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <adwaita.h>
#include "hx.h"
#include "gtkhx.h"
#include "gtkutil.h"
#include "connect.h"
#include "gtk_hlist.h"
#include "dfa.h"
#include "chat.h"
#include "options.h"
#include "cfgkeys.h"
#include "tracker.h"

static GtkWidget *tracker_window;
static GtkWidget *tracker_list;
static GtkWidget *tracker_search_entry;
static GtkWidget *tracker_case_btn;
static GtkWidget *lbl_found, *lbl_total;
static int num_found, num_total;
static struct dfa *current_search;

struct tracker_server {
    char *name;
    char *desc;

    guint16 nusers;
    guint16 port;

    struct in_addr addr;
    struct tracker_server *left, *right;
};

struct tracker_server *tracker_server_tree = NULL;

static void
tracker_list_destroy (struct tracker_server *root)
{
    if (!root) {
        return;
    }

    tracker_list_destroy (root->left);
    tracker_list_destroy (root->right);

    if (root->name) {
        g_free (root->name);
    }
    if (root->desc) {
        g_free (root->desc);
    }
    g_free (root);

    tracker_server_tree = NULL;
}

void
tracker_clear (void)
{
    gtk_hlist_clear (GTK_HLIST (tracker_list));
}

/* Phase 4.5: GTK 4 close-request on (GtkWindow *, gpointer). */
static gboolean
close_tracker_window (GtkWindow *window, gpointer data)
{
    (void)window;
    (void)data;

    tracker_clear ();
    tracker_window = 0;
    tracker_list = 0;
    tracker_search_entry = NULL;
    tracker_case_btn = NULL;

    tracker_list_destroy (tracker_server_tree);
    dfafree (current_search);
    current_search = NULL;
    return FALSE;
}

/* Phase 5+ (async tracker rewrite): the pthread_t / SIGUSR1 /
 * pthread_kill / pthread_join dance that used to live here is gone.
 * The tracker fetch now runs on the main loop via GSocketClient +
 * GInputStream async; cancellation is a g_cancellable_cancel against
 * a GCancellable held inside network.c's tracker_run_ctx.
 * tracker_kill_threads() in network.c trips that cancellation; the
 * in-flight async callback unwinds cleanly.
 *
 * Net effect: no thread to spawn, no signal handler, no joinable
 * handle to manage, and the UAF that the old (Phase 5) code spent
 * a paragraph defending against (track_tid pointing at freed
 * libpthread state) is structurally impossible. */

static void
tracker_getlist (GtkWidget *widget, gpointer data)
{
    session *sess = data;
    (void)widget;

    /* Cancel any in-flight fetch and start fresh. */
    tracker_kill_threads ();

    tracker_clear ();
    num_found = 0;
    num_total = 0;

    tracker_list_destroy (tracker_server_tree);

    gtk_label_set_text (GTK_LABEL (lbl_found), "  0");
    gtk_label_set_text (GTK_LABEL (lbl_total), " / 0");

    hx_tracker_list_async (sess);
}

static void
tracker_search_tree (struct dfa *preg, struct tracker_server *root)
{
    int row;
    char nusersstr[8], portstr[8], *text[5];
    int namelen, desclen;
    char flag;

    if (!root) {
        return;
    }
    /* Defensive: the worker thread feeds new servers through here as
	 * tracker_server_create runs, so a stray result that arrives before
	 * current_search is set up (or after teardown) would deref NULL in
	 * dfaexec. The window's first-open path now initializes preg before
	 * spawning the worker, but keep the guard for paranoia. */
    if (!preg) {
        return;
    }

    namelen = strlen (root->name);
    desclen = strlen (root->desc);
    flag = dfaexec (preg, root->name, &root->name[namelen], 0, NULL, NULL)
           || dfaexec (preg, root->desc, &root->desc[desclen], 0, NULL, NULL);
    root->name[namelen] = '\0';
    root->desc[desclen] = '\0';
    if (flag) {
        snprintf (nusersstr, sizeof (nusersstr), "%u", root->nusers);
        snprintf (portstr, sizeof (portstr), "%u", root->port);

        text[0] = root->name;
        text[1] = nusersstr;
        text[2] = g_malloc (HOSTLEN);

        inet_ntop (AF_INET, &root->addr, text[2], HOSTLEN);

        text[3] = portstr;
        text[4] = root->desc;

        row = gtk_hlist_append (GTK_HLIST (tracker_list), text);
        /* Phase 5: no per-row foreground override — let the GTK theme's
		 * default foreground apply so the tracker list reads correctly
		 * under both light and dark themes. */
        g_free (text[2]);
        gtk_hlist_set_row_data (GTK_HLIST (tracker_list), row, root);
        num_found++;
    }

    tracker_search_tree (preg, root->left);
    tracker_search_tree (preg, root->right);
}

/* Phase 5: tracker_search runs every time the visible filter needs to
 * be re-applied — when the search entry's text changes, when the user
 * hits Enter, or when the case-sensitive toggle flips. We always read
 * the current entry text out of the cached tracker_search_entry rather
 * than out of the signal's `widget` argument, so the same routine can
 * be called from any of the three signal handlers (search-changed,
 * activate, case-toggle) plus the on-resync paths that don't get
 * called with a widget at all. */
static void
tracker_rerun_search (void)
{
    char *num;
    const char *str;

    if (!tracker_list || !tracker_search_entry) {
        return;
    }

    dfafree (current_search);
    current_search = g_malloc (sizeof (struct dfa));

    num_found = 0;
    str = gtk_editable_get_text (GTK_EDITABLE (tracker_search_entry));
    if (!str) {
        str = "";
    }
    dfacomp ((char *)str, strlen (str), current_search, 1);
    tracker_clear ();
    gtk_hlist_freeze (GTK_HLIST (tracker_list));

    tracker_search_tree (current_search, tracker_server_tree);

    gtk_hlist_thaw (GTK_HLIST (tracker_list));
    num = g_strdup_printf ("  %d", num_found);
    gtk_label_set_text (GTK_LABEL (lbl_found), num);
    g_free (num);
}

static void
tracker_search (GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    tracker_rerun_search ();
}

void
tracker_search_refresh (void)
{
    tracker_rerun_search ();
}

/* "Aa" case-sensitive toggle in the search row. Routes through the
 * generic prefs setter so the Settings switch (if open) stays in
 * lockstep, the dfa fold flag updates via the cfgvar change-callback,
 * and the new value persists. Then re-run the search so the visible
 * result list reflects the new case mode immediately. */
static void
tracker_case_toggled (GtkToggleButton *btn, gpointer data)
{
    (void)data;
    gtkhx_prefs_set_bool (CFG_TRACKER_CASE,
                          gtk_toggle_button_get_active (btn) ? 1 : 0);
    tracker_rerun_search ();
}

static int
find_server (struct in_addr addr, guint16 port, struct tracker_server *root)
{
    /* a one-liner to traverse a binary search tree ...
	   kids: don't do this for your computer science course! */
    return (root
            && (((root->addr.s_addr == addr.s_addr) && (root->port == port))
                || (find_server (
                    addr, port,
                    (root->addr.s_addr == addr.s_addr)
                        ? ((root->port > port) ? root->left : root->right)
                        : ((root->addr.s_addr > addr.s_addr) ? root->left
                                                             : root->right)))));
}

static void
insert_server (struct tracker_server *server, struct tracker_server *root)
{
    /* tree is null...set this server as the root */
    if (!tracker_server_tree) {
        tracker_server_tree = server;
        server->left = 0;
        server->right = 0;
        return;
    }

    if (root->addr.s_addr == server->addr.s_addr) {
        if (root->port > server->port) {
            if (root->left) {
                insert_server (server, root->left);
                return;
            }
            root->left = server;
            return;
        }
        if (root->right) {
            insert_server (server, root->right);
            return;
        }
        root->right = server;
        return;
    }

    else if (root->addr.s_addr > server->addr.s_addr) {
        if (root->left) {
            insert_server (server, root->left);
            return;
        }
        root->left = server;
        return;
    }

    if (root->right) {
        insert_server (server, root->right);
        return;
    }

    root->right = server;
    return;
}

void
tracker_server_create (struct in_addr addr, guint16 port, guint16 nusers,
                       const char *nam, const char *desc, int total)
{
    struct tracker_server *server;
    char *num;
    int old_num_found;

    if (!tracker_list) {
        return;
    }

    if (find_server (addr, port, tracker_server_tree)) {
        return;
    }

    num_total++;
    server = g_malloc (sizeof (struct tracker_server));
    server->addr = addr;
    server->port = port;
    server->nusers = nusers;
    /* Phase 3.x: server names from old Hotline trackers are MacRoman-
	 * encoded. Pango requires UTF-8; without conversion any non-ASCII
	 * byte (very common in Mac-era server names) makes
	 * gtk_label_set_text → pango_layout_set_text print the
	 * "Invalid UTF-8 string passed to pango_layout_set_text" critical.
	 * Convert with fallback so unmappable bytes degrade to '?' rather
	 * than dropping the whole row. */
    server->name = g_convert_with_fallback (nam, -1, "UTF-8", "MACINTOSH", "?",
                                            NULL, NULL, NULL);
    if (!server->name) {
        server->name = g_strdup (nam ? nam : "");
    }
    server->desc = g_convert_with_fallback (desc, -1, "UTF-8", "MACINTOSH", "?",
                                            NULL, NULL, NULL);
    if (!server->desc) {
        server->desc = g_strdup (desc ? desc : "");
    }
    server->left = 0;
    server->right = 0;
    insert_server (server, tracker_server_tree);

    old_num_found = num_found;
    tracker_search_tree (current_search, server);

    /* avoid unnecessary redraws */
    if (old_num_found != num_found) {
        num = g_strdup_printf ("  %d", num_found);
        gtk_label_set_text (GTK_LABEL (lbl_found), num);
        g_free (num);

        num = g_strdup_printf (" / %d", num_total);
        gtk_label_set_text (GTK_LABEL (lbl_total), num);
        g_free (num);
    }
}

static int tracker_storow;
static int tracker_stocol;

/* Phase 4.5: button-press-event is gone in GTK 4. The tracker list's
 * single/double-click handling lives on a GtkGestureClick controller
 * now; the "pressed" signal fires with widget-local x/y, and n_press
 * gates the double-click connect. */
static void
tracker_pressed (GtkGestureClick *gesture, int n_press, double x, double y,
                 gpointer data)
{
    GtkWidget *widget
        = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
    (void)data;

    gtk_hlist_get_selection_info (GTK_HLIST (widget), (int)x, (int)y,
                                  &tracker_storow, &tracker_stocol);

    if (n_press == 2) {
        struct tracker_server *server;
        char buf[HOSTLEN];

        server
            = gtk_hlist_get_row_data (GTK_HLIST (tracker_list), tracker_storow);
        if (!server) {
            return;
        }
        inet_ntop (AF_INET, &server->addr, buf, HOSTLEN);
#ifdef CONFIG_COMPRESS
        memset (the_session.htlc.compressalg, 0,
                sizeof (the_session.htlc.compressalg));
#endif
#ifdef CONFIG_CIPHER
        memset (the_session.htlc.cipheralg, 0,
                sizeof (the_session.htlc.cipheralg));
#endif
        hx_connect (&the_session.htlc, buf, server->port, "", "", 0);
    }
}

static void
tracker_connect (void)
{
    struct tracker_server *server;

    server = gtk_hlist_get_row_data (GTK_HLIST (tracker_list), tracker_storow);
    if (server) {
        char buf[HOSTLEN];

        create_connect_window (0, &the_session);
        inet_ntop (AF_INET, &server->addr, buf, HOSTLEN);
        connect_set_entries (buf, 0, 0, server->port);
    }
}

void
create_tracker_window (GtkWidget *widget, gpointer data)
{
    GtkWidget *vbox;
    GtkWidget *header;
    GtkWidget *searchhbox;
    GtkWidget *searchentry;
    GtkWidget *tracker_window_scroll;
    GtkWidget *refreshbtn;
    GtkWidget *connbtn;
    GtkWidget *count_box;
    gchar *titles[5];
    session *sess = data;

    titles[0] = _ ("Name");
    titles[1] = _ ("Users");
    titles[2] = _ ("Address");
    titles[3] = _ ("Port");
    titles[4] = _ ("Description");

    if (tracker_window) {
        return;
    }

    tracker_window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (tracker_window), _ ("Tracker"));
    gtk_window_set_default_size (GTK_WINDOW (tracker_window), 720, 500);
    g_signal_connect (tracker_window, "close-request",
                      G_CALLBACK (close_tracker_window), 0);

    tracker_list = gtk_hlist_new_with_titles (5, titles);
    gtk_hlist_set_column_width (GTK_HLIST (tracker_list), 0, 200);
    gtk_hlist_set_column_width (GTK_HLIST (tracker_list), 1, 76);
    gtk_hlist_set_column_justification (GTK_HLIST (tracker_list), 1,
                                        GTK_JUSTIFY_CENTER);
    gtk_hlist_set_column_width (GTK_HLIST (tracker_list), 2, 150);
    gtk_hlist_set_column_width (GTK_HLIST (tracker_list), 3, 70);
    gtk_hlist_set_column_width (GTK_HLIST (tracker_list), 4, 320);
    {
        /* Phase 4.5: button-press-event is gone — install a gesture
		 * controller for the double-click-to-connect path. */
        GtkGesture *click = gtk_gesture_click_new ();
        gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click),
                                       GDK_BUTTON_PRIMARY);
        g_signal_connect (click, "pressed", G_CALLBACK (tracker_pressed), NULL);
        gtk_widget_add_controller (tracker_list, GTK_EVENT_CONTROLLER (click));
    }

    /* Phase 5: GtkSearchEntry replaces the legacy "Search:" label +
	 * GtkEntry pair. SearchEntry has its own search-glass icon and
	 * a clear-button when text is present, so the label becomes
	 * redundant. The placeholder text takes the label's job.
	 *
	 * "search-changed" fires with a small built-in debounce as the user
	 * types — that's the signal that drives live filtering. "activate"
	 * (Enter) is wired too as a no-op shortcut so the user's muscle
	 * memory of "type query, hit Enter" still works; with live filtering
	 * the list is already up to date by the time Enter lands. */
    searchentry = gtk_search_entry_new ();
    tracker_search_entry = searchentry;
    gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (searchentry),
                                           _ ("Search trackers"));
    g_signal_connect (searchentry, "search-changed",
                      G_CALLBACK (tracker_search), 0);
    g_signal_connect (searchentry, "activate", G_CALLBACK (tracker_search), 0);

    /* Case-sensitive toggle, tucked next to the search field so the
	 * user doesn't have to dig into Settings to flip it. State is
	 * mirrored to the gtkhx_prefs.track_case cfgvar (and from there to
	 * gtkhxrc) via gtkhx_prefs_set_bool, which also keeps the
	 * Settings page in lockstep when both happen to be open. */
    tracker_case_btn = gtk_toggle_button_new_with_label ("Aa");
    gtk_widget_set_tooltip_text (tracker_case_btn,
                                 _ ("Match case in tracker search"));
    gtk_widget_add_css_class (tracker_case_btn, "flat");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (tracker_case_btn),
                                  gtkhx_prefs.track_case ? TRUE : FALSE);
    g_signal_connect (tracker_case_btn, "toggled",
                      G_CALLBACK (tracker_case_toggled), NULL);

    num_found = 0;
    num_total = 0;
    lbl_found = gtk_label_new ("0");
    lbl_total = gtk_label_new (" / 0");
    gtk_widget_add_css_class (lbl_found, "dim-label");
    gtk_widget_add_css_class (lbl_total, "dim-label");

    /* Phase 5: action buttons live in the AdwHeaderBar now, not in a
	 * row beneath. Refresh + Connect on the leading edge, the live
	 * found-count "N / M" indicator on the trailing edge. */
    refreshbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/refresh.png",
                                      _ ("Refresh tracker list"), 2,
                                      G_CALLBACK (tracker_getlist), sess);
    connbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/connect.png",
                                   _ ("Connect to selected server"), 2,
                                   G_CALLBACK (tracker_connect), 0);

    header = adw_header_bar_new ();
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), refreshbtn);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), connbtn);

    count_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append (GTK_BOX (count_box), lbl_found);
    gtk_box_append (GTK_BOX (count_box), lbl_total);
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), count_box);

    gtk_window_set_titlebar (GTK_WINDOW (tracker_window), header);

    tracker_window_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (tracker_window_scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
    gtk_scrolled_window_set_has_frame (
        GTK_SCROLLED_WINDOW (tracker_window_scroll), TRUE);
    gtk_widget_set_vexpand (tracker_window_scroll, TRUE);
    gtkhx_widget_set_child (tracker_window_scroll, tracker_list);

    /* Phase 5 layout: actions live in the headerbar, content vbox
	 * just holds the search row + the list. 8 px outer margin so
	 * content doesn't touch the window frame; 8 px gutter between
	 * the search field and the list. */
    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start (vbox, 8);
    gtk_widget_set_margin_end (vbox, 8);
    gtk_widget_set_margin_top (vbox, 8);
    gtk_widget_set_margin_bottom (vbox, 8);

    searchhbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtkhx_box_pack (searchhbox, searchentry, 1, 1, 0);
    gtkhx_box_pack (searchhbox, tracker_case_btn, 0, 0, 0);
    gtkhx_box_pack (vbox, searchhbox, 0, 0, 0);

    gtkhx_box_pack (vbox, tracker_window_scroll, 1, 1, 0);
    gtkhx_widget_set_child (tracker_window, vbox);
    init_keyaccel (tracker_window);
    gtk_window_present (GTK_WINDOW (tracker_window));

    gtk_widget_grab_focus (searchentry);

    /* Initialize the search filter BEFORE spawning the tracker worker.
	 * Each result the worker streams in calls tracker_server_create →
	 * tracker_search_tree(current_search, ...), which dereferences
	 * current_search inside dfaexec. The legacy ordering raced: the
	 * worker could fire faster than the synchronous init below it. */
    current_search = g_malloc (sizeof (struct dfa));
    dfacomp ("", 0, current_search, 1);

    tracker_getlist (0, sess);
}

void
dfaerror (const char *mesg)
{
    /*	g_warning("%s\n", mesg); */
    hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX, "Tracker Regexps: %s\n",
                      mesg);
}
