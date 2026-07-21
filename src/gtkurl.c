/*
 * gtkurl.c — URL detection, opening, and right-click popup. Shared
 * between the xtext-based windows (chat / msg / pchat) and the
 * GtkTextView-based windows (news / news15).
 *
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include "config.h"
#include <string.h>
#include <ctype.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include "compat.h"
#include "connect.h"
#include "gtkurl.h"
#include "hx.h"
#include "toolbar.h"

/* ------------------------------------------------------------------- *
 * Detection / normalisation
 * ------------------------------------------------------------------- */

/* The canonical set of prefixes we consider URL-y. Both the
 * GtkTextView path (gtkurl_textview_install) and the xtext path
 * (chat.c's word_check, which calls gtkurl_word_has_url_scheme) key
 * off this single list, so adding a new scheme is a one-line edit
 * here.
 *
 * Trailing ":" on scheme prefixes ensures we don't misfire on
 * "httpfoo" or "wwwbar"; the bare "www.", "ftp.", "irc." entries
 * accept the historical "www.example.com" pattern that lots of
 * users still type without a scheme. */
static const char *url_schemes[]
    = { "http://",    "https://", "ftp://",  "ftps://", "irc://", "mailto:",
        "magnet:",    "git://",   "ssh://",  "sftp://",
        /* hotline:// is the protocol-native URL — we route it through
		 * a different right-click popup (Save Bookmark / Connect to
		 * Server / Copy Link) instead of the browser-launch menu. The
		 * branch lives in gtkurl_show_popup. */
        "hotline://", NULL };
static const char *url_bare_prefixes[] = { "www.", "ftp.", "irc.", NULL };

/* Trailing punctuation we strip from URL matches. "https://foo.com."
 * at end of sentence shouldn't include the period; same for the
 * other common sentence terminators / closers. Parens are awkward
 * because they appear inside Wikipedia-style URLs, so we only strip
 * a closing paren if there's no opening paren anywhere in the
 * candidate — handled in trim_trailing_url_punct. */
static gboolean
is_strip_char (char c)
{
    switch (c) {
    case '.':
    case ',':
    case ';':
    case ':':
    case '!':
    case '?':
    case ')':
    case ']':
    case '\'':
    case '"':
        return TRUE;
    default:
        return FALSE;
    }
}

static int
count_char (const char *s, int n, char c)
{
    int k = 0, i;
    for (i = 0; i < n; i++) {
        if (s[i] == c) {
            k++;
        }
    }
    return k;
}

static int
trim_trailing_url_punct (const char *s, int len)
{
    while (len > 0 && is_strip_char (s[len - 1])) {
        char c = s[len - 1];
        /* Don't strip a closing paren / bracket if the URL
		 * itself contains a matching opener (Wikipedia URL
		 * shape). */
        if (c == ')' && count_char (s, len, '(') > 0) {
            break;
        }
        if (c == ']' && count_char (s, len, '[') > 0) {
            break;
        }
        len--;
    }
    return len;
}

gboolean
gtkurl_word_has_url_scheme (const char *word)
{
    int i;

    if (!word || !*word) {
        return FALSE;
    }

    for (i = 0; url_schemes[i]; i++) {
        if (g_ascii_strncasecmp (word, url_schemes[i], strlen (url_schemes[i]))
            == 0) {
            return TRUE;
        }
    }

    for (i = 0; url_bare_prefixes[i]; i++) {
        if (g_ascii_strncasecmp (word, url_bare_prefixes[i],
                                 strlen (url_bare_prefixes[i]))
            == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

gboolean
gtkurl_is_url (const char *word)
{
    const char *dot, *at;
    size_t len;

    if (!word || !*word) {
        return FALSE;
    }

    if (gtkurl_word_has_url_scheme (word)) {
        return TRUE;
    }

    /* Bare email address — handled here so right-click on an email
	 * gets the same Open / Copy popup, mailto:-prefixed at launch
	 * time. word_check returns WORD_EMAIL for these, kept in
	 * lockstep. */
    len = strlen (word);
    at = strchr (word, '@');
    dot = strrchr (word, '.');
    if (at && dot && at < dot && dot < word + len - 1) {
        return TRUE;
    }

    return FALSE;
}

char *
gtkurl_normalize (const char *word)
{
    int i;
    const char *at, *dot;

    if (!word || !*word) {
        return g_strdup ("");
    }

    for (i = 0; url_schemes[i]; i++) {
        if (g_ascii_strncasecmp (word, url_schemes[i], strlen (url_schemes[i]))
            == 0) {
            return g_strdup (word);
        }
    }

    if (g_ascii_strncasecmp (word, "ftp.", 4) == 0) {
        return g_strdup_printf ("ftp://%s", word);
    }
    if (g_ascii_strncasecmp (word, "www.", 4) == 0
        || g_ascii_strncasecmp (word, "irc.", 4) == 0) {
        return g_strdup_printf ("https://%s", word);
    }

    at = strchr (word, '@');
    dot = strrchr (word, '.');
    if (at && dot && at < dot) {
        return g_strdup_printf ("mailto:%s", word);
    }

    return g_strdup (word);
}

/* ------------------------------------------------------------------- *
 * In-text URL scanning (for GtkTextView consumers)
 * ------------------------------------------------------------------- */

/* Walk the buffer and report every URL-shaped substring. Word-
 * boundary detection is whitespace + a few control chars; this is
 * intentionally lenient so we don't lose URLs that happen to abut
 * quotes / parens / etc. The match endpoints are then run through
 * trim_trailing_url_punct so sentence punctuation isn't included. */
void
gtkurl_scan (const char *text, gssize length, gtkurl_match_cb cb, gpointer user)
{
    int len;
    int i;

    if (!text || !cb) {
        return;
    }
    if (length < 0) {
        length = (gssize)strlen (text);
    }
    len = (int)length;

    i = 0;
    while (i < len) {
        gboolean at_boundary = (i == 0) || g_ascii_isspace (text[i - 1])
                               || text[i - 1] == '<' || text[i - 1] == '('
                               || text[i - 1] == '[' || text[i - 1] == '"'
                               || text[i - 1] == '\'';
        gboolean matched = FALSE;
        int j;
        const char *prefix = NULL;

        if (at_boundary) {
            for (j = 0; url_schemes[j]; j++) {
                int n = (int)strlen (url_schemes[j]);
                if (i + n <= len
                    && g_ascii_strncasecmp (&text[i], url_schemes[j], n) == 0) {
                    prefix = url_schemes[j];
                    matched = TRUE;
                    break;
                }
            }
            if (!matched) {
                for (j = 0; url_bare_prefixes[j]; j++) {
                    int n = (int)strlen (url_bare_prefixes[j]);
                    if (i + n <= len
                        && g_ascii_strncasecmp (&text[i], url_bare_prefixes[j],
                                                n)
                               == 0) {
                        prefix = url_bare_prefixes[j];
                        matched = TRUE;
                        break;
                    }
                }
            }
        }

        if (matched) {
            int start = i;
            int end = i + (int)strlen (prefix);
            int trimmed_len;
            while (end < len) {
                char c = text[end];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '<'
                    || c == '>' || c == '"' || c == '\'') {
                    break;
                }
                end++;
            }
            trimmed_len = trim_trailing_url_punct (&text[start], end - start);
            if (trimmed_len > (int)strlen (prefix)) {
                cb (text, start, start + trimmed_len, user);
            }
            i = end;
            continue;
        }
        i++;
    }
}

/* ------------------------------------------------------------------- *
 * Browser launch + alternate-browser discovery
 * ------------------------------------------------------------------- */

/* Open `url' through the system's default http handler. On Linux
 * this routes through gio → xdg-open; on Wayland / sandboxed
 * Flatpak, GtkUriLauncher uses the org.freedesktop.portal.OpenURI
 * portal automatically. GtkUriLauncher needs GTK 4.10+; the rest of
 * the codebase already uses 4.10+ APIs unconditionally
 * (GtkFileDialog in inline_media_dialog.c, GtkColorDialogButton in
 * options.c) so the legacy gtk_show_uri fallback that used to live
 * here is dead — dropped. */
static void
launch_default_browser (GtkWidget *anchor, const char *url)
{
    GtkWindow *parent = GTK_IS_WINDOW (anchor)
                            ? GTK_WINDOW (anchor)
                            : GTK_WINDOW (gtk_widget_get_root (anchor));
    GtkUriLauncher *launcher = gtk_uri_launcher_new (url);
    gtk_uri_launcher_launch (launcher, parent, NULL, NULL, NULL);
    g_object_unref (launcher);
}

static void
launch_app_with_url (GAppInfo *app, const char *url)
{
    GList uris = { 0 };
    uris.data = (gpointer)url;
    g_app_info_launch_uris (app, &uris, NULL, NULL);
}

/* Return the GAppInfo registered as the default for http URIs, or
 * NULL if the platform doesn't have one. The default is what the
 * "Open Link in Browser" item routes to; we want it labelled with
 * the actual app name so the user can see what it'll launch. */
static GAppInfo *
default_http_appinfo (void)
{
    return g_app_info_get_default_for_uri_scheme ("http");
}

/* Build a list of alternate-browser GAppInfos: every registered
 * http handler EXCEPT the default. Caller g_object_unref's each
 * entry and g_list_free's the list. */
static GList *
alternate_browsers (GAppInfo *default_app)
{
    GList *all = g_app_info_get_all_for_type ("x-scheme-handler/http");
    GList *out = NULL;
    GList *l;
    const char *default_id
        = default_app ? g_app_info_get_id (default_app) : NULL;

    for (l = all; l; l = l->next) {
        GAppInfo *ai = l->data;
        const char *id = g_app_info_get_id (ai);
        if (default_id && id && strcmp (id, default_id) == 0) {
            g_object_unref (ai);
            continue;
        }
        out = g_list_append (out, ai);
    }
    g_list_free (all);
    return out;
}

/* ------------------------------------------------------------------- *
 * Right-click popover
 * ------------------------------------------------------------------- */

struct popup_ctx {
    char *url;
    GAppInfo *app; /* NULL = system default */
    GtkPopover *popover;
};

static void
popup_ctx_free (gpointer data, GClosure *closure)
{
    struct popup_ctx *ctx = data;
    (void)closure;
    if (ctx->app) {
        g_object_unref (ctx->app);
    }
    g_free (ctx->url);
    g_free (ctx);
}

static void
on_open_default (GtkButton *btn, gpointer data)
{
    struct popup_ctx *ctx = data;
    (void)btn;
    launch_default_browser (GTK_WIDGET (ctx->popover), ctx->url);
    gtk_popover_popdown (ctx->popover);
}

static void
on_open_alt (GtkButton *btn, gpointer data)
{
    struct popup_ctx *ctx = data;
    (void)btn;
    if (ctx->app) {
        launch_app_with_url (ctx->app, ctx->url);
    } else {
        launch_default_browser (GTK_WIDGET (ctx->popover), ctx->url);
    }
    gtk_popover_popdown (ctx->popover);
}

static void
on_copy_link (GtkButton *btn, gpointer data)
{
    struct popup_ctx *ctx = data;
    GdkClipboard *cb = gtk_widget_get_clipboard (GTK_WIDGET (btn));
    gdk_clipboard_set_text (cb, ctx->url);
    gtk_popover_popdown (ctx->popover);
}

/* hotline:// URL — branches the popup to a Hotline-specific action set
 * (Save Bookmark / Connect to Server / Copy Link). Browsers don't
 * speak Hotline, so launching xdg-open on a hotline:// URL would
 * either pop the "no handler" dialog or, if some kind soul registered
 * gtkhx as the handler, recurse — neither is what the user meant.
 *
 * The is_hotline_url predicate keeps the matching local: it's also
 * what gtkurl_show_popup keys off when deciding which menu to build. */
static gboolean
is_hotline_url (const char *url)
{
    return url && g_ascii_strncasecmp (url, "hotline://", 10) == 0;
}

static void
on_connect_hotline (GtkButton *btn, gpointer data)
{
    struct popup_ctx *ctx = data;
    (void)btn;
    if (!connect_open_hotline_url (ctx->url)) {
        toolbar_show_toast (_ ("Couldn't parse hotline:// URL"));
    }
    gtk_popover_popdown (ctx->popover);
}

static void
on_save_bookmark_hotline (GtkButton *btn, gpointer data)
{
    struct popup_ctx *ctx = data;
    char *saved_name = NULL;
    GError *err = NULL;
    char toastbuf[256];
    (void)btn;

    if (connect_save_hotline_url_as_bookmark (ctx->url, &saved_name, &err)) {
        g_snprintf (toastbuf, sizeof (toastbuf), _ ("Saved bookmark \"%s\""),
                    saved_name ? saved_name : "");
        toolbar_show_toast (toastbuf);
    } else {
        g_snprintf (toastbuf, sizeof (toastbuf),
                    _ ("Couldn't save bookmark: %s"),
                    err && err->message ? err->message : _ ("unknown error"));
        toolbar_show_toast (toastbuf);
    }
    g_free (saved_name);
    g_clear_error (&err);
    gtk_popover_popdown (ctx->popover);
}

static GtkWidget *
make_menu_button (const char *icon_name, const char *label_text, GCallback cb,
                  struct popup_ctx *ctx)
{
    GtkWidget *btn = gtk_button_new ();
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lbl = gtk_label_new (label_text);

    gtk_widget_add_css_class (btn, "flat");
    gtk_widget_set_halign (btn, GTK_ALIGN_FILL);

    if (icon_name) {
        GtkWidget *img = gtk_image_new_from_icon_name (icon_name);
        gtk_box_append (GTK_BOX (box), img);
    }
    gtk_label_set_xalign (GTK_LABEL (lbl), 0.0);
    gtk_widget_set_hexpand (lbl, TRUE);
    gtk_box_append (GTK_BOX (box), lbl);

    gtk_button_set_child (GTK_BUTTON (btn), box);
    g_signal_connect_data (btn, "clicked", cb, ctx, popup_ctx_free, 0);
    return btn;
}

void
gtkurl_show_popup (GtkWidget *anchor, const char *url, double x, double y)
{
    GtkWidget *popover, *vbox, *header, *sep;
    GtkWidget *open_btn, *copy_btn;
    GAppInfo *def_app;
    GList *alts, *l;
    GdkRectangle rect = { 0 };
    GtkWidget *parent;

    if (!anchor || !url || !*url) {
        return;
    }

    /* GtkPopover has to be parented to a widget that's safe
	 * to grab from. Anchoring directly to a GtkTextView nested inside
	 * a GtkScrolledWindow (or an xtext deep in the chat layout) tripped
	 * GDK's "Tried to map a grabbing popup with a non-top most parent"
	 * — the popover appeared but click-outside-to-dismiss broke and
	 * Escape leaked the grab, freezing the rest of the app. Parenting
	 * to the toplevel sidesteps this.
	 *
	 * Contract: callers pass (x, y) in TOPLEVEL-ROOT-RELATIVE coords.
	 * gtkurl_textview_install's gesture handler hands us widget-local
	 * coords, which it converts via gtk_widget_compute_point before
	 * calling. gtkurl_xtext_word_click hands us coords from
	 * gdk_event_get_position, which are surface-local (= root-local
	 * in single-surface GTK 4) — no translation needed. The earlier
	 * version of this function did the anchor→root translation
	 * unconditionally, which left the xtext path double-translated
	 * and the popup landed off-screen, looking inert. */
    parent = GTK_WIDGET (gtk_widget_get_root (anchor));
    if (!parent) {
        parent = anchor;
    }

    popover = gtk_popover_new ();
    gtk_widget_set_parent (popover, parent);
    gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
    gtk_popover_set_autohide (GTK_POPOVER (popover), TRUE);

    rect.x = (int)x;
    rect.y = (int)y;
    rect.width = 1;
    rect.height = 1;
    gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start (vbox, 6);
    gtk_widget_set_margin_end (vbox, 6);
    gtk_widget_set_margin_top (vbox, 6);
    gtk_widget_set_margin_bottom (vbox, 6);
    gtk_popover_set_child (GTK_POPOVER (popover), vbox);

    /* Header: the URL itself, single-line, ellipsised. dim-label
	 * makes it read as caption rather than command. */
    header = gtk_label_new (url);
    gtk_label_set_xalign (GTK_LABEL (header), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (header), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (header), 48);
    gtk_widget_add_css_class (header, "dim-label");
    gtk_widget_set_margin_start (header, 6);
    gtk_widget_set_margin_end (header, 6);
    gtk_widget_set_margin_bottom (header, 4);
    gtk_box_append (GTK_BOX (vbox), header);

    sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append (GTK_BOX (vbox), sep);

    if (is_hotline_url (url)) {
        /* hotline:// — Hotline-native menu. Connect routes through
		 * connect_open_hotline_url (parses + plain-Hotline connect_with_args);
		 * Save Bookmark routes through connect_save_hotline_url_as_
		 * bookmark (parses + persists to the bookmarks TOML store). The
		 * browser-launch entries below are skipped — xdg-open on a
		 * hotline:// URL has nowhere good to go. */
        GtkWidget *connect_item, *bookmark_item;
        {
            struct popup_ctx *ctx = g_new0 (struct popup_ctx, 1);
            ctx->url = g_strdup (url);
            ctx->popover = GTK_POPOVER (popover);
            connect_item = make_menu_button (
                "network-server-symbolic", _ ("Connect to Server"),
                G_CALLBACK (on_connect_hotline), ctx);
            gtk_box_append (GTK_BOX (vbox), connect_item);
        }
        {
            struct popup_ctx *ctx = g_new0 (struct popup_ctx, 1);
            ctx->url = g_strdup (url);
            ctx->popover = GTK_POPOVER (popover);
            bookmark_item = make_menu_button (
                "bookmark-new-symbolic", _ ("Save Bookmark"),
                G_CALLBACK (on_save_bookmark_hotline), ctx);
            gtk_box_append (GTK_BOX (vbox), bookmark_item);
        }
        {
            struct popup_ctx *ctx = g_new0 (struct popup_ctx, 1);
            ctx->url = g_strdup (url);
            ctx->popover = GTK_POPOVER (popover);
            copy_btn = make_menu_button ("edit-copy-symbolic",
                                         _ ("Copy Link"),
                                         G_CALLBACK (on_copy_link), ctx);
            gtk_box_append (GTK_BOX (vbox), copy_btn);
        }
        /* Auto-destroy when the popover hides. */
        g_signal_connect (popover, "closed", G_CALLBACK (gtk_widget_unparent),
                          NULL);
        gtk_popover_popup (GTK_POPOVER (popover));
        return;
    }

    /* Default-browser entry. */
    def_app = default_http_appinfo ();
    {
        struct popup_ctx *ctx = g_new0 (struct popup_ctx, 1);
        ctx->url = g_strdup (url);
        ctx->popover = GTK_POPOVER (popover);
        open_btn = make_menu_button ("web-browser-symbolic",
                                     _ ("Open Link in Browser"),
                                     G_CALLBACK (on_open_default), ctx);
        gtk_box_append (GTK_BOX (vbox), open_btn);
    }

    /* Copy. */
    {
        struct popup_ctx *ctx = g_new0 (struct popup_ctx, 1);
        ctx->url = g_strdup (url);
        ctx->popover = GTK_POPOVER (popover);
        copy_btn
            = make_menu_button ("edit-copy-symbolic", _ ("Copy Selected Link"),
                                G_CALLBACK (on_copy_link), ctx);
        gtk_box_append (GTK_BOX (vbox), copy_btn);
    }

    /* Alternates. Skipped silently if there aren't any. */
    alts = alternate_browsers (def_app);
    if (alts) {
        gtk_box_append (GTK_BOX (vbox),
                        gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
    }
    for (l = alts; l; l = l->next) {
        GAppInfo *app = l->data;
        struct popup_ctx *ctx = g_new0 (struct popup_ctx, 1);
        const char *name = g_app_info_get_display_name (app);
        char *label;
        GtkWidget *btn;

        ctx->url = g_strdup (url);
        ctx->app = g_object_ref (app);
        ctx->popover = GTK_POPOVER (popover);

        label = g_strdup_printf ("→ %s", name ? name : "?");
        btn = make_menu_button (NULL, label, G_CALLBACK (on_open_alt), ctx);
        g_free (label);
        gtk_box_append (GTK_BOX (vbox), btn);
    }
    g_list_free_full (alts, g_object_unref);
    if (def_app) {
        g_object_unref (def_app);
    }

    /* Auto-destroy when the popover hides. */
    g_signal_connect (popover, "closed", G_CALLBACK (gtk_widget_unparent),
                      NULL);

    gtk_popover_popup (GTK_POPOVER (popover));
}

/* ------------------------------------------------------------------- *
 * xtext WORD_CLICK handler
 * ------------------------------------------------------------------- */

void
gtkurl_xtext_word_click (GtkWidget *xtext, char *word, GdkEvent *event,
                         gpointer data)
{
    guint button;
    double x, y;
    char *normalized;
    GdkEventType evtype;
    (void)data;

    if (!event || !word || !*word) {
        return;
    }
    evtype = gdk_event_get_event_type (event);
    if (evtype != GDK_BUTTON_PRESS && evtype != GDK_BUTTON_RELEASE) {
        return;
    }
    button = gdk_button_event_get_button (event);
    if (button != GDK_BUTTON_SECONDARY && button != GDK_BUTTON_MIDDLE) {
        return;
    }
    if (!gtkurl_is_url (word)) {
        return;
    }

    /* gdk_event_get_position is surface-relative, which is the same
	 * as toplevel-root-relative in GTK 4's single-surface model.
	 * gtkurl_show_popup expects root-local coords, so pass through
	 * directly with no translation. */
    if (!gdk_event_get_position (event, &x, &y)) {
        x = y = 0;
    }

    normalized = gtkurl_normalize (word);
    gtkurl_show_popup (xtext, normalized, x, y);
    g_free (normalized);
}

/* ------------------------------------------------------------------- *
 * GtkTextView wiring
 * ------------------------------------------------------------------- */

/* Tag plus per-textview state lives as object data on the GtkTextView
 * itself so the install function is safely re-entrant. */
#define KEY_INSTALLED "gtkurl-installed"
#define KEY_HOVER_TAG "gtkurl-hover-tag"
#define KEY_HOVER_LO "gtkurl-hover-lo"
#define KEY_HOVER_HI "gtkurl-hover-hi"
#define KEY_HAS_HOVER "gtkurl-has-hover"

static void
ensure_url_tags (GtkTextView *tv)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer (tv);
    GtkTextTagTable *tt = gtk_text_buffer_get_tag_table (buf);

    if (!gtk_text_tag_table_lookup (tt, "url")) {
        /* Adwaita-friendly accent for links. The exact hex matches
		 * libadwaita's @accent_color in light mode; dark theme also
		 * reads acceptably. */
        gtk_text_buffer_create_tag (buf, "url", "foreground", "#1c71d8", NULL);
    }
    if (!gtk_text_tag_table_lookup (tt, "url-hover")) {
        gtk_text_buffer_create_tag (buf, "url-hover", "underline",
                                    PANGO_UNDERLINE_SINGLE, NULL);
    }
}

struct apply_ctx {
    GtkTextBuffer *buf;
};

static void
apply_url_tag_cb (const char *text, int start_byte, int end_byte, gpointer user)
{
    struct apply_ctx *ctx = user;
    GtkTextIter s, e;
    (void)text;

    /* GtkTextBuffer iterates by character offset, not byte offset.
	 * Convert via gtk_text_iter_get_offset on a fresh start iter
	 * advanced by g_utf8_pointer_to_offset. */
    gtk_text_buffer_get_start_iter (ctx->buf, &s);
    gtk_text_buffer_get_start_iter (ctx->buf, &e);
    {
        const char *all = text;
        long off_s = g_utf8_pointer_to_offset (all, all + start_byte);
        long off_e = g_utf8_pointer_to_offset (all, all + end_byte);
        gtk_text_iter_set_offset (&s, (int)off_s);
        gtk_text_iter_set_offset (&e, (int)off_e);
    }
    gtk_text_buffer_apply_tag_by_name (ctx->buf, "url", &s, &e);
}

void
gtkurl_textview_apply_tags (GtkTextView *tv)
{
    GtkTextBuffer *buf;
    GtkTextIter start, end;
    char *text;
    struct apply_ctx ctx;

    if (!tv) {
        return;
    }

    buf = gtk_text_view_get_buffer (tv);
    ensure_url_tags (tv);

    gtk_text_buffer_get_bounds (buf, &start, &end);
    gtk_text_buffer_remove_tag_by_name (buf, "url", &start, &end);
    gtk_text_buffer_remove_tag_by_name (buf, "url-hover", &start, &end);
    g_object_set_data (G_OBJECT (tv), KEY_HAS_HOVER, GINT_TO_POINTER (0));

    text = gtk_text_buffer_get_text (buf, &start, &end, FALSE);
    if (text && *text) {
        ctx.buf = buf;
        gtkurl_scan (text, -1, apply_url_tag_cb, &ctx);
    }
    g_free (text);
}

/* Find the URL tag covering an iter. Returns the malloc'd URL text
 * if any, NULL otherwise. */
static char *
url_at_iter (GtkTextView *tv, GtkTextIter *iter)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer (tv);
    GtkTextTagTable *tt = gtk_text_buffer_get_tag_table (buf);
    GtkTextTag *url_tag = gtk_text_tag_table_lookup (tt, "url");
    GtkTextIter start, end;

    if (!url_tag || !gtk_text_iter_has_tag (iter, url_tag)) {
        return NULL;
    }

    start = end = *iter;
    if (!gtk_text_iter_starts_tag (&start, url_tag)) {
        gtk_text_iter_backward_to_tag_toggle (&start, url_tag);
    }
    if (!gtk_text_iter_ends_tag (&end, url_tag)) {
        gtk_text_iter_forward_to_tag_toggle (&end, url_tag);
    }

    return gtk_text_buffer_get_text (buf, &start, &end, FALSE);
}

static gboolean
window_to_buffer_iter (GtkTextView *tv, double wx, double wy, GtkTextIter *out)
{
    int bx, by;
    gtk_text_view_window_to_buffer_coords (tv, GTK_TEXT_WINDOW_WIDGET, (int)wx,
                                           (int)wy, &bx, &by);
    return gtk_text_view_get_iter_at_location (tv, out, bx, by);
}

static void
clear_hover (GtkTextView *tv)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer (tv);
    GtkTextIter start, end;

    if (!GPOINTER_TO_INT (g_object_get_data (G_OBJECT (tv), KEY_HAS_HOVER))) {
        return;
    }

    gtk_text_buffer_get_bounds (buf, &start, &end);
    gtk_text_buffer_remove_tag_by_name (buf, "url-hover", &start, &end);
    g_object_set_data (G_OBJECT (tv), KEY_HAS_HOVER, GINT_TO_POINTER (0));
    gtk_widget_set_cursor (GTK_WIDGET (tv), NULL);
}

static void
on_motion (GtkEventControllerMotion *ctrl, double x, double y, gpointer data)
{
    GtkTextView *tv = data;
    GtkTextBuffer *buf;
    GtkTextTagTable *tt;
    GtkTextTag *url_tag;
    GtkTextIter iter, start, end;
    (void)ctrl;

    buf = gtk_text_view_get_buffer (tv);
    tt = gtk_text_buffer_get_tag_table (buf);
    url_tag = gtk_text_tag_table_lookup (tt, "url");
    if (!url_tag) {
        clear_hover (tv);
        return;
    }

    if (!window_to_buffer_iter (tv, x, y, &iter)
        || !gtk_text_iter_has_tag (&iter, url_tag)) {
        clear_hover (tv);
        return;
    }

    /* Apply hover-underline only over the matched URL run. */
    start = end = iter;
    if (!gtk_text_iter_starts_tag (&start, url_tag)) {
        gtk_text_iter_backward_to_tag_toggle (&start, url_tag);
    }
    if (!gtk_text_iter_ends_tag (&end, url_tag)) {
        gtk_text_iter_forward_to_tag_toggle (&end, url_tag);
    }

    clear_hover (tv);
    gtk_text_buffer_apply_tag_by_name (buf, "url-hover", &start, &end);
    g_object_set_data (G_OBJECT (tv), KEY_HAS_HOVER, GINT_TO_POINTER (1));
    {
        GdkCursor *c = gdk_cursor_new_from_name ("pointer", NULL);
        gtk_widget_set_cursor (GTK_WIDGET (tv), c);
        if (c) {
            g_object_unref (c);
        }
    }
}

static void
on_motion_leave (GtkEventControllerMotion *ctrl, gpointer data)
{
    (void)ctrl;
    clear_hover (GTK_TEXT_VIEW (data));
}

static void
on_textview_pressed (GtkGestureClick *gesture, int n_press, double x, double y,
                     gpointer data)
{
    GtkTextView *tv = data;
    guint button;
    GtkTextIter iter;
    char *url;
    GtkWidget *root;
    graphene_point_t src_pt = { (float)x, (float)y };
    graphene_point_t dst_pt = { (float)x, (float)y };
    (void)n_press;

    button
        = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
    if (button != GDK_BUTTON_SECONDARY) {
        return;
    }

    if (!window_to_buffer_iter (tv, x, y, &iter)) {
        return;
    }
    url = url_at_iter (tv, &iter);
    if (!url) {
        return;
    }

    /* gesture x/y are widget-local; gtkurl_show_popup wants them in
	 * the toplevel-root coordinate space. Translate. */
    root = GTK_WIDGET (gtk_widget_get_root (GTK_WIDGET (tv)));
    if (root && root != GTK_WIDGET (tv)) {
        if (gtk_widget_compute_point (GTK_WIDGET (tv), root, &src_pt,
                                      &dst_pt)) {
            x = dst_pt.x;
            y = dst_pt.y;
        }
    }

    gtkurl_show_popup (GTK_WIDGET (tv), url, x, y);
    g_free (url);

    /* Swallow the event so the textview's default right-click
	 * (selection / paste menu) doesn't also fire. */
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

void
gtkurl_textview_install (GtkTextView *tv)
{
    GtkEventController *click;
    GtkEventController *motion;

    if (!tv) {
        return;
    }
    if (g_object_get_data (G_OBJECT (tv), KEY_INSTALLED)) {
        return;
    }

    ensure_url_tags (tv);

    click = GTK_EVENT_CONTROLLER (gtk_gesture_click_new ());
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click),
                                   GDK_BUTTON_SECONDARY);
    /* CAPTURE phase so we run before GtkTextView's own
	 * right-click-to-popup-context-menu controller, which is in
	 * BUBBLE phase by default. We claim the sequence inside the
	 * handler to suppress the default menu when the click landed
	 * on a URL. */
    gtk_event_controller_set_propagation_phase (click, GTK_PHASE_CAPTURE);
    g_signal_connect (click, "pressed", G_CALLBACK (on_textview_pressed), tv);
    gtk_widget_add_controller (GTK_WIDGET (tv), click);

    motion = gtk_event_controller_motion_new ();
    g_signal_connect (motion, "motion", G_CALLBACK (on_motion), tv);
    g_signal_connect (motion, "leave", G_CALLBACK (on_motion_leave), tv);
    gtk_widget_add_controller (GTK_WIDGET (tv), motion);

    g_object_set_data (G_OBJECT (tv), KEY_INSTALLED, GINT_TO_POINTER (1));
}
