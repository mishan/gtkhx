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
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>

#include "hx.h"
#include "hl_access.h"
#include "network.h"
#include "gtkutil.h"
#include "gtkhx.h"
#include "tasks.h"
#include "rcv.h"
#include "debug.h"
#include "gtkurl.h"
#include "news.h"

static GtkWidget *post_window;
static GtkWidget *postprompt;

/* ---- News-window in-content search ---------------------------------
 *
 * Ctrl+F (or the Find toolbar button) reveals an AdwSearchBar above
 * the news GtkTextView. The bar holds a GtkSearchEntry, a "i of N"
 * counter, and Up/Down nav buttons. Matches are highlighted in the
 * buffer via two GtkTextTags — "search-match" for all hits,
 * "search-current" stronger for the active one. Esc dismisses the
 * bar and clears the highlights.
 *
 * State is per-news-window. We allocate one news_search_ctx in
 * create_news_window, hang it off the window via g_object_set_data,
 * and free it in close_news_window. The struct caches the widgets
 * (so navigation doesn't have to re-walk the headerbar) and an
 * GArray of (start,end) match offsets so Next/Prev are O(1).
 */
struct news_match {
    int start_offset;
    int end_offset;
};

struct news_search_ctx {
    GtkWidget *search_bar;
    GtkWidget *search_entry;
    GtkWidget *count_label;
    GtkWidget *prev_btn;
    GtkWidget *next_btn;
    GtkTextView *text_view;
    GArray *matches;   /* of struct news_match */
    int current_match; /* -1 = none */
};

static void news_search_clear_highlights (struct news_search_ctx *ctx);
static void news_search_run (struct news_search_ctx *ctx);
static void news_search_navigate (struct news_search_ctx *ctx, int delta);
static void news_search_update_count (struct news_search_ctx *ctx);

/* Convenience: the search-match / search-current tags are buffer-
 * scoped, so we ensure they exist on first use. Tag colours are
 * Adwaita-friendly (warning-yellow + accent-orange), and they apply
 * via background-rgba so light/dark themes both look right against
 * the buffer's default foreground. */
static void
news_search_ensure_tags (GtkTextBuffer *buf)
{
    GtkTextTagTable *tt = gtk_text_buffer_get_tag_table (buf);
    if (!gtk_text_tag_table_lookup (tt, "search-match")) {
        gtk_text_buffer_create_tag (buf, "search-match", "background",
                                    "#f6d32d", "foreground", "#000000", NULL);
    }
    if (!gtk_text_tag_table_lookup (tt, "search-current")) {
        gtk_text_buffer_create_tag (buf, "search-current", "background",
                                    "#ff7800", "foreground", "#ffffff", NULL);
    }
}

static void
news_search_clear_highlights (struct news_search_ctx *ctx)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer (ctx->text_view);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds (buf, &start, &end);
    gtk_text_buffer_remove_tag_by_name (buf, "search-match", &start, &end);
    gtk_text_buffer_remove_tag_by_name (buf, "search-current", &start, &end);
}

static void
news_search_apply_current_tag (struct news_search_ctx *ctx)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer (ctx->text_view);
    GtkTextIter start, end;
    struct news_match *m;

    /* Remove previous active highlight from every match — cheaper than
	 * tracking which one was last active across edits. */
    gtk_text_buffer_get_bounds (buf, &start, &end);
    gtk_text_buffer_remove_tag_by_name (buf, "search-current", &start, &end);

    if (ctx->current_match < 0
        || (guint)ctx->current_match >= ctx->matches->len) {
        return;
    }

    m = &g_array_index (ctx->matches, struct news_match, ctx->current_match);
    gtk_text_buffer_get_iter_at_offset (buf, &start, m->start_offset);
    gtk_text_buffer_get_iter_at_offset (buf, &end, m->end_offset);
    gtk_text_buffer_apply_tag_by_name (buf, "search-current", &start, &end);

    /* Scroll just enough to bring the match on screen. within_margin
	 * 0.1 keeps a small breathing area at top/bottom. */
    gtk_text_view_scroll_to_iter (ctx->text_view, &start, 0.1, FALSE, 0.0, 0.0);
}

static void
news_search_update_count (struct news_search_ctx *ctx)
{
    const char *text = gtk_editable_get_text (GTK_EDITABLE (ctx->search_entry));
    gboolean searching = text && *text;

    if (!searching) {
        gtk_label_set_text (GTK_LABEL (ctx->count_label), "");
        gtk_widget_set_sensitive (ctx->prev_btn, FALSE);
        gtk_widget_set_sensitive (ctx->next_btn, FALSE);
        return;
    }

    if (ctx->matches->len == 0) {
        gtk_label_set_text (GTK_LABEL (ctx->count_label), _ ("No results"));
        gtk_widget_set_sensitive (ctx->prev_btn, FALSE);
        gtk_widget_set_sensitive (ctx->next_btn, FALSE);
    } else {
        char *fmt = g_strdup_printf ("%d / %u", ctx->current_match + 1,
                                     ctx->matches->len);
        gtk_label_set_text (GTK_LABEL (ctx->count_label), fmt);
        g_free (fmt);
        gtk_widget_set_sensitive (ctx->prev_btn, ctx->matches->len > 1);
        gtk_widget_set_sensitive (ctx->next_btn, ctx->matches->len > 1);
    }
}

static void
news_search_run (struct news_search_ctx *ctx)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer (ctx->text_view);
    const char *needle
        = gtk_editable_get_text (GTK_EDITABLE (ctx->search_entry));
    GtkTextIter cursor, end_buf;

    news_search_clear_highlights (ctx);
    g_array_set_size (ctx->matches, 0);
    ctx->current_match = -1;

    if (!needle || !*needle) {
        news_search_update_count (ctx);
        return;
    }

    news_search_ensure_tags (buf);

    gtk_text_buffer_get_start_iter (buf, &cursor);
    gtk_text_buffer_get_end_iter (buf, &end_buf);

    /* Walk the buffer collecting every case-insensitive substring
	 * match. Visible-only=FALSE matches inside any tag region;
	 * GTK_TEXT_SEARCH_CASE_INSENSITIVE handles the i18n folding via
	 * Pango's normalize+casefold pair, so "café" matches "CAFÉ". */
    while (TRUE) {
        GtkTextIter ms, me;
        if (!gtk_text_iter_forward_search (&cursor, needle,
                                           GTK_TEXT_SEARCH_CASE_INSENSITIVE
                                               | GTK_TEXT_SEARCH_VISIBLE_ONLY,
                                           &ms, &me, &end_buf)) {
            break;
        }

        struct news_match m;
        m.start_offset = gtk_text_iter_get_offset (&ms);
        m.end_offset = gtk_text_iter_get_offset (&me);
        g_array_append_val (ctx->matches, m);

        gtk_text_buffer_apply_tag_by_name (buf, "search-match", &ms, &me);
        cursor = me;
    }

    if (ctx->matches->len > 0) {
        ctx->current_match = 0;
        news_search_apply_current_tag (ctx);
    }
    news_search_update_count (ctx);
}

static void
news_search_navigate (struct news_search_ctx *ctx, int delta)
{
    int n;
    if (!ctx->matches || ctx->matches->len == 0) {
        return;
    }

    n = (int)ctx->matches->len;
    ctx->current_match = ((ctx->current_match + delta) % n + n) % n;
    news_search_apply_current_tag (ctx);
    news_search_update_count (ctx);
}

/* ---- Search-bar signal handlers ------------------------------------ */

static void
on_search_changed (GtkSearchEntry *entry, gpointer data)
{
    (void)entry;
    news_search_run ((struct news_search_ctx *)data);
}

static void
on_search_next (GtkSearchEntry *entry, gpointer data)
{
    (void)entry;
    news_search_navigate ((struct news_search_ctx *)data, +1);
}

static void
on_search_prev (GtkSearchEntry *entry, gpointer data)
{
    (void)entry;
    news_search_navigate ((struct news_search_ctx *)data, -1);
}

static void
on_search_btn_prev (GtkButton *btn, gpointer data)
{
    (void)btn;
    news_search_navigate ((struct news_search_ctx *)data, -1);
}

static void
on_search_btn_next (GtkButton *btn, gpointer data)
{
    (void)btn;
    news_search_navigate ((struct news_search_ctx *)data, +1);
}

/* search-bar-driven Esc closes the bar — set 'search-mode-enabled'
 * back to FALSE. Once the bar hides we also clear highlights so the
 * news view goes back to its plain rendering. */
static void
on_search_mode_notify (GObject *obj, GParamSpec *pspec, gpointer data)
{
    struct news_search_ctx *ctx = data;
    (void)pspec;
    if (!gtk_search_bar_get_search_mode (GTK_SEARCH_BAR (obj))) {
        news_search_clear_highlights (ctx);
        g_array_set_size (ctx->matches, 0);
        ctx->current_match = -1;
        gtk_editable_set_text (GTK_EDITABLE (ctx->search_entry), "");
    }
}

/* Headerbar Find button — toggles the search bar and focuses the
 * entry. AdwHeaderBar -> button click route. */
static void
on_find_btn_clicked (GtkButton *btn, gpointer data)
{
    struct news_search_ctx *ctx = data;
    gboolean was_open;
    (void)btn;
    was_open
        = gtk_search_bar_get_search_mode (GTK_SEARCH_BAR (ctx->search_bar));
    gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (ctx->search_bar),
                                    !was_open);
    if (!was_open) {
        gtk_widget_grab_focus (ctx->search_entry);
    }
}

/* GtkShortcutController callback for Ctrl+F. Same behaviour as
 * clicking the Find headerbar button: open the bar (if not already
 * open) and focus the entry so typing starts narrowing matches. */
static gboolean
on_find_shortcut (GtkWidget *widget, GVariant *args, gpointer data)
{
    struct news_search_ctx *ctx = data;
    (void)widget;
    (void)args;
    gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (ctx->search_bar), TRUE);
    gtk_widget_grab_focus (ctx->search_entry);
    return TRUE;
}

/* Build the search bar and its child layout. Caller wires it into the
 * window content above the news scrolled view. */
static GtkWidget *
news_search_bar_new (struct news_search_ctx *ctx)
{
    GtkWidget *bar = gtk_search_bar_new ();
    GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *entry, *count, *prev_btn, *next_btn;

    entry = gtk_search_entry_new ();
    gtk_widget_set_hexpand (entry, TRUE);
    gtk_box_append (GTK_BOX (hbox), entry);

    count = gtk_label_new ("");
    gtk_widget_add_css_class (count, "dim-label");
    gtk_widget_set_margin_start (count, 6);
    gtk_widget_set_margin_end (count, 6);
    gtk_box_append (GTK_BOX (hbox), count);

    prev_btn = gtk_button_new_from_icon_name ("go-up-symbolic");
    gtk_widget_set_tooltip_text (prev_btn, _ ("Previous match"));
    gtk_widget_add_css_class (prev_btn, "flat");
    gtk_widget_set_sensitive (prev_btn, FALSE);
    gtk_box_append (GTK_BOX (hbox), prev_btn);

    next_btn = gtk_button_new_from_icon_name ("go-down-symbolic");
    gtk_widget_set_tooltip_text (next_btn, _ ("Next match"));
    gtk_widget_add_css_class (next_btn, "flat");
    gtk_widget_set_sensitive (next_btn, FALSE);
    gtk_box_append (GTK_BOX (hbox), next_btn);

    gtk_search_bar_set_child (GTK_SEARCH_BAR (bar), hbox);
    gtk_search_bar_connect_entry (GTK_SEARCH_BAR (bar), GTK_EDITABLE (entry));

    ctx->search_bar = bar;
    ctx->search_entry = entry;
    ctx->count_label = count;
    ctx->prev_btn = prev_btn;
    ctx->next_btn = next_btn;

    g_signal_connect (entry, "search-changed", G_CALLBACK (on_search_changed),
                      ctx);
    g_signal_connect (entry, "next-match", G_CALLBACK (on_search_next), ctx);
    g_signal_connect (entry, "previous-match", G_CALLBACK (on_search_prev),
                      ctx);
    g_signal_connect (entry, "activate", G_CALLBACK (on_search_next), ctx);
    g_signal_connect (prev_btn, "clicked", G_CALLBACK (on_search_btn_prev),
                      ctx);
    g_signal_connect (next_btn, "clicked", G_CALLBACK (on_search_btn_next),
                      ctx);
    g_signal_connect (bar, "notify::search-mode-enabled",
                      G_CALLBACK (on_search_mode_notify), ctx);

    return bar;
}

static void
news_search_ctx_free (gpointer data)
{
    struct news_search_ctx *ctx = data;
    if (!ctx) {
        return;
    }
    if (ctx->matches) {
        g_array_free (ctx->matches, TRUE);
    }
    g_free (ctx);
}

/* Phase 5: gtkhx_text_to_utf8 → gtkhx_text_to_utf8 (moved to
 * gtkutil — server names, news bodies, post subjects all want the
 * same Mac Roman / Latin-1 / already-UTF-8 fallback chain). The
 * old name in this TU is gone; callers just include gtkutil.h. */

void
hx_get_news (struct htlc_conn *htlc)
{
    task_new (htlc, RCV_TASK_FN (rcv_task_news_file), 0, 0, "news");
    hlwrite (htlc, HTLC_HDR_NEWS_GETFILE, 0, 0);
}

void
hx_post_news (struct htlc_conn *htlc, const char *news, guint16 len)
{
    task_new (htlc, 0, 0, 0, "post");

    /* Phase E2/E3: news body — UTF-8 / Mac Roman + LF→CR for
	 * legacy servers. The flat 1.0 news file is line-oriented,
	 * so getting line endings right is what makes posts render
	 * correctly on Mac clients. */
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize wire_len = 0;
    char *wire
        = gtkhx_text_for_wire (news, len, utf8, /*is_body=*/TRUE, &wire_len);

    hlwrite (htlc, HTLC_HDR_NEWS_POST, 0, 1, HTLC_DATA_NEWS_POST,
             (guint16)wire_len, wire);
    g_free (wire);
}

void
reload_news (GtkWidget *widget, gpointer data)
{
    session *sess = data;

    if (!gtkhx_prefs.geo.news.open) {
        return;
    }

    /* Phase 5: ask the access bitmap before hitting the wire. The
	 * server told us at SELFINFO time which permissions our account
	 * has — see hx_rcv_user_selfinfo populating htlc->access — and
	 * sending HTLC_HDR_NEWS_GETFILE without HL_ACCESS_READ_NEWS just
	 * earns us a task error ("Uh, no.") on every login. Skip
	 * the request entirely instead.
	 *
	 * Only gate when access has actually been populated (any bit
	 * set is a good-enough proxy; an unauthenticated state has all
	 * zeros). If access is still zero — pre-login or some weird
	 * server that never sent SELFINFO — fall through and try the
	 * fetch the legacy way.
	 *
	 * The user-initiated Refresh button hits this same function;
	 * dropping the auto-fetch on a no-permission server also stops
	 * a manual refresh from working there, which is the right
	 * behaviour: the server would just reject it anyway. We surface
	 * a debug-channel note so the user can confirm via
	 * GTKHX_DEBUG=news what's happening. */
    {
        const guint8 *access = (const guint8 *)&sess->htlc.access;
        gboolean any_bit_set = FALSE;
        int i;
        for (i = 0; i < 8; i++) {
            if (access[i]) {
                any_bit_set = TRUE;
                break;
            }
        }
        if (any_bit_set && !hl_access_has (access, HL_ACCESS_READ_NEWS)) {
            debug_log ("news", "skipping HTLC_HDR_NEWS_GETFILE — account lacks "
                               "HL_ACCESS_READ_NEWS (bit 20)");
            return;
        }
    }

    {
        GtkTextBuffer *buf
            = gtk_text_view_get_buffer (GTK_TEXT_VIEW (sess->news_text));
        gtk_text_buffer_set_text (buf, "", -1);
        hx_get_news (&sess->htlc);
    }
}

/* Phase 4.5: GTK 4 close-request on (GtkWindow *, gpointer); FALSE
 * allows default destroy. */
static gboolean
close_news_window (GtkWindow *window, gpointer data)
{
    session *sess = data;
    (void)window;

    gtkhx_prefs.geo.news.open = 0;
    gtkhx_prefs.geo.news.init = 0;
    sess->news_window = 0;
    return FALSE;
}

/* Phase 4.5: configure-event is gone in GTK 4. News window size is
 * captured at hx_quit() time alongside position; see gtkhx.c
 * gtkhx_save_window_positions. */

static gboolean
close_post_window (GtkWindow *window, gpointer data)
{
    (void)window;
    (void)data;
    post_window = 0;
    return FALSE;
}

static void
post_news (GtkWidget *widget, gpointer data)
{
    char *posttext;
    session *sess = data;
    int len;
    GtkTextBuffer *buf;
    GtkTextIter start, end;

    buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (postprompt));
    gtk_text_buffer_get_start_iter (buf, &start);
    gtk_text_buffer_get_end_iter (buf, &end);
    posttext = gtk_text_buffer_get_text (buf, &start, &end, FALSE);
    len = strlen (posttext);
    LF2CR (posttext, len);
    if (len > 0 && posttext[len - 1] == '\r') {
        posttext[len - 1] = 0;
    }
    hx_post_news (&sess->htlc, posttext, len);

    g_free (posttext);
    gtkhx_widget_destroy (post_window);
    post_window = 0;
}

void
create_post_window (GtkWidget *widget, gpointer data)
{
    GtkWidget *okbut;
    GtkWidget *cancbut;
    GtkWidget *vbox, *hbox;
    session *sess = data;

    post_window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (post_window), _ ("Post News"));
    gtk_widget_set_size_request (post_window, 300, 280);
    g_signal_connect (post_window, "close-request",
                      G_CALLBACK (close_post_window), 0);

    postprompt = gtk_text_view_new ();
    gtk_text_view_set_editable (GTK_TEXT_VIEW (postprompt), TRUE);
    gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (postprompt), GTK_WRAP_WORD);
    gtkhx_apply_text_style (postprompt);

    {
        GtkWidget *post_scroll = gtk_scrolled_window_new ();
        gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (post_scroll),
                                        GTK_POLICY_AUTOMATIC,
                                        GTK_POLICY_AUTOMATIC);
        gtkhx_widget_set_child (post_scroll, postprompt);
        gtk_widget_set_size_request (post_scroll, 0, 260);

        vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
        gtkhx_widget_set_child (post_window, vbox);
        gtkhx_box_pack (vbox, post_scroll, 0, 0, 0);
    }

    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtkhx_box_pack (vbox, hbox, 0, 0, 0);

    okbut = gtk_button_new_with_label (_ ("OK"));
    g_signal_connect (okbut, "clicked", G_CALLBACK (post_news), sess);
    cancbut = gtk_button_new_with_label (_ ("Cancel"));
    g_signal_connect (cancbut, "clicked",

                      G_CALLBACK (close_post_window), 0);

    gtkhx_box_pack (hbox, okbut, 0, 0, 0);
    gtkhx_box_pack (hbox, cancbut, 0, 0, 0);

    gtk_window_present (GTK_WINDOW (post_window));
    gtk_widget_grab_focus (postprompt);
}

void
create_news_window (session *sess)
{
    GtkWidget *news_scroll;
    GtkWidget *content_vbox;
    GtkWidget *news_text;
    GtkWidget *news_window;
    GtkWidget *postButton, *reloadButton, *findButton;
    GtkWidget *search_bar;
    struct news_search_ctx *search_ctx;

    if (gtkhx_prefs.geo.news.open) {
        gtk_window_present (GTK_WINDOW (sess->news_window));
        return;
    }

    news_window = gtk_window_new ();

    /* Phase 3.x: dropped GTK 1.2-era realize+get_style pair (style unused). */

    /* Phase 5: 2x-scaled headerbar buttons via the shared helper.
	 * Find is added at unscaled size (1) since it's a stock GTK
	 * symbolic icon — gtkhx_pixmap_button's XPM-upscale path
	 * would render it blurry. Use a plain GtkButton instead. */
    postButton
        = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/postnews.png",
                               _ ("Post News"), 2, NULL, NULL);
    reloadButton
        = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/refresh.png",
                               _ ("Reload News"), 2, NULL, NULL);
    findButton = gtk_button_new_from_icon_name ("system-search-symbolic");
    gtk_widget_set_tooltip_text (findButton, _ ("Find in News (Ctrl+F)"));

    gtk_window_set_resizable (GTK_WINDOW (news_window), TRUE);

    gtk_window_set_title (GTK_WINDOW (news_window), _ ("News"));
    gtk_widget_set_size_request (news_window, 412, 384);
    g_signal_connect (news_window, "close-request",
                      G_CALLBACK (close_news_window), sess);
    g_signal_connect (postButton, "clicked", G_CALLBACK (create_post_window),
                      sess);
    g_signal_connect (reloadButton, "clicked", G_CALLBACK (reload_news), sess);

    /* Phase 5: AdwHeaderBar replaces both the default GtkWindow title
	 * bar and the in-content btn_frame + posthbox row. Post on the
	 * start, Find + Reload on the end. */
    {
        GtkWidget *header = adw_header_bar_new ();
        adw_header_bar_pack_start (ADW_HEADER_BAR (header), postButton);
        adw_header_bar_pack_end (ADW_HEADER_BAR (header), reloadButton);
        adw_header_bar_pack_end (ADW_HEADER_BAR (header), findButton);
        gtk_window_set_titlebar (GTK_WINDOW (news_window), header);
    }

    news_text = gtk_text_view_new ();
    gtk_text_view_set_editable (GTK_TEXT_VIEW (news_text), FALSE);
    gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (news_text), FALSE);
    gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (news_text), GTK_WRAP_WORD);
    gtkhx_apply_text_style (news_text);
    gtkurl_textview_install (GTK_TEXT_VIEW (news_text));

    news_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (news_scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand (news_scroll, TRUE);
    gtkhx_widget_set_child (news_scroll, news_text);

    /* Search ctx and bar. The bar is placed above the news scroll in
	 * a vertical box; it stays hidden (search-mode-enabled=FALSE)
	 * until Ctrl+F or the headerbar Find button reveals it. */
    search_ctx = g_new0 (struct news_search_ctx, 1);
    search_ctx->text_view = GTK_TEXT_VIEW (news_text);
    search_ctx->matches
        = g_array_new (FALSE, FALSE, sizeof (struct news_match));
    search_ctx->current_match = -1;
    g_object_set_data_full (G_OBJECT (news_window), "search-ctx", search_ctx,
                            news_search_ctx_free);

    /* Create the search-match / search-current tags up front. The
	 * news content paths (output_news_post / output_news_file) call
	 * news_search_run after each new chunk arrives, and the very
	 * first call's news_search_clear_highlights tries to remove these
	 * tags by name — without a prior ensure-pass, GtkTextBuffer warns
	 * "Unknown tag 'search-match'" / "Unknown tag 'search-current'"
	 * once per news load. Ensuring the tags here once at window-build
	 * time is cheaper than guarding every remove_tag_by_name caller. */
    news_search_ensure_tags (gtk_text_view_get_buffer (search_ctx->text_view));

    search_bar = news_search_bar_new (search_ctx);
    /* set_key_capture_widget covers Esc-closes-bar and printable-
	 * key-opens-bar-with-text. It does NOT cover Ctrl+F — that
	 * needs an explicit GtkShortcutController. (My first attempt
	 * relied on the capture widget alone and Ctrl+F silently did
	 * nothing.) */
    gtk_search_bar_set_key_capture_widget (GTK_SEARCH_BAR (search_bar),
                                           news_window);
    {
        GtkEventController *sc = gtk_shortcut_controller_new ();
        gtk_event_controller_set_propagation_phase (sc, GTK_PHASE_CAPTURE);
        gtk_shortcut_controller_add_shortcut (
            GTK_SHORTCUT_CONTROLLER (sc),
            gtk_shortcut_new (
                gtk_keyval_trigger_new (GDK_KEY_f, GDK_CONTROL_MASK),
                gtk_callback_action_new (on_find_shortcut, search_ctx, NULL)));
        gtk_widget_add_controller (news_window, sc);
    }

    g_signal_connect (findButton, "clicked", G_CALLBACK (on_find_btn_clicked),
                      search_ctx);

    content_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (content_vbox), search_bar);
    gtk_box_append (GTK_BOX (content_vbox), news_scroll);
    gtk_window_set_child (GTK_WINDOW (news_window), content_vbox);
    gtk_widget_set_sensitive (postButton, FALSE);
    gtk_widget_set_sensitive (reloadButton, FALSE);

    /* Phase 3.x: only apply saved geometry when the prefs file actually
	 * has one (see users.c for rationale — zero-size collapses the
	 * window under GTK 3). */
    if (gtkhx_prefs.geo.news.xsize > 0 && gtkhx_prefs.geo.news.ysize > 0) {
        gtk_window_set_default_size (GTK_WINDOW (news_window),
                                     gtkhx_prefs.geo.news.xsize,
                                     gtkhx_prefs.geo.news.ysize);
    }
    if (gtkhx_prefs.geo.news.xpos > 0 || gtkhx_prefs.geo.news.ypos > 0) {
        /* Phase 4.2: gtk_window_move removed (Wayland) */

        gtk_window_present (GTK_WINDOW (news_window));
    }

    if (connected == 1) {
        changetitlespecific (news_window, _ ("News"));
        gtk_widget_set_sensitive (postButton, TRUE);
        gtk_widget_set_sensitive (reloadButton, TRUE);
    }

    init_keyaccel (news_window);
    gtkhx_prefs.geo.news.open = 1;
    gtkhx_prefs.geo.news.init = 1;

    sess->news_window = news_window;
    sess->news_text = news_text;
    sess->postButton = postButton;
    sess->reloadButton = reloadButton;
}

void
open_news (GtkWidget *widget, gpointer data)
{
    session *sess = data;

    if (!gtkhx_prefs.geo.news.open) {
        create_news_window (sess);
        if (connected) {
            hx_get_news (&sess->htlc);
        }
    } else {
        gtk_window_present (GTK_WINDOW (sess->news_window));
        gtk_widget_grab_focus (sess->news_window);
    }
}

void
output_news_post (struct htlc_conn *htlc, char *news, guint16 len)
{
    session *sess;

    if (!gtkhx_prefs.geo.news.open) {
        return;
    }

    sess = &the_session;
    if (!sess) {
        return;
    }

    {
        GtkTextBuffer *buf
            = gtk_text_view_get_buffer (GTK_TEXT_VIEW (sess->news_text));
        GtkTextIter start;
        gsize utf8_len;
        char *utf8 = gtkhx_text_to_utf8 (news, len, &utf8_len);
        gtk_text_buffer_get_start_iter (buf, &start);
        gtk_text_buffer_insert (buf, &start, utf8, (gint)utf8_len);
        g_free (utf8);
    }

    /* Re-tag URLs across the whole buffer so the new chunk's links
	 * pick up the "url" GtkTextTag (foreground + hover-underline +
	 * right-click popup). Cheap — single regex pass over the buffer. */
    gtkurl_textview_apply_tags (GTK_TEXT_VIEW (sess->news_text));

    /* If the user has Find open, re-run the search so the new post
	 * picks up its highlights too. No-op if the bar is hidden or the
	 * entry is empty. */
    {
        struct news_search_ctx *sctx
            = g_object_get_data (G_OBJECT (sess->news_window), "search-ctx");
        if (sctx) {
            news_search_run (sctx);
        }
    }
}

void
output_news_file (struct htlc_conn *htlc, char *news, guint16 len)
{
    session *sess;
    GtkTextBuffer *buf;
    GtkTextIter end;
    gsize utf8_len;
    char *utf8;
    struct news_search_ctx *sctx;

    if (!gtkhx_prefs.geo.news.open) {
        return;
    }

    sess = &the_session;
    if (!sess) {
        return;
    }

    buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (sess->news_text));
    gtk_text_buffer_get_end_iter (buf, &end);
    utf8 = gtkhx_text_to_utf8 (news, len, &utf8_len);
    gtk_text_buffer_insert (buf, &end, utf8, (gint)utf8_len);
    g_free (utf8);

    /* Re-tag URLs (mirrors output_news_post). */
    gtkurl_textview_apply_tags (GTK_TEXT_VIEW (sess->news_text));

    /* Same Find-still-active reconciliation as output_news_post —
	 * the bulk-load path appends the whole news file in one go, and
	 * the user might have a query already typed. */
    sctx = g_object_get_data (G_OBJECT (sess->news_window), "search-ctx");
    if (sctx) {
        news_search_run (sctx);
    }
}
