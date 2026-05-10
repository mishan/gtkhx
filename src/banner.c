/*
 * banner.c — server banner UI surface and fetch state machines.
 *
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This file owns:
 *   - The banner widget docked at the bottom of the toolbar
 *     window (a GtkPicture wrapped in a clickable area, with a
 *     dim "Server banner" caption that doubles as a fallback when
 *     the image can't be fetched / decoded).
 *   - The URL-mode fetch state machine: libsoup-3 async GET, then
 *     decode via gdk_pixbuf_new_from_stream, then set on the
 *     picture. Cancellable on banner_clear.
 *   - The HTXF-mode fetch will land in a follow-up commit; for
 *     now the file-mode banner shows the type-only caption and
 *     does NOT issue HTLC_HDR_BANNER_GET yet.
 *
 * Released under GPL-2.0-or-later. See COPYING.
 */

#include "config.h"
#include <string.h>
#include <gtk/gtk.h>
#ifdef HAVE_LIBSOUP
# include <libsoup/soup.h>
#endif
#include "compat.h"
#include "debug.h"
#include "banner.h"

/* ------------------------------------------------------------------- *
 * Module state — single banner per process. The toolbar is the only
 * window that calls banner_widget_new(), so a handful of statics
 * suffice. */

/* Maximum displayed image dimensions. Most servers ship 468x60
 * Tracker-era banners; cap at 600x60 so unusually-large or
 * unusually-tall images don't blow out the toolbar layout. */
#define BANNER_MAX_W 600
#define BANNER_MAX_H 60

static GtkWidget    *banner_root    = NULL;  /* outer GtkBox */
static GtkWidget    *banner_picture = NULL;  /* GtkPicture */
static GtkWidget    *banner_caption = NULL;  /* GtkLabel */
#ifdef HAVE_LIBSOUP
static SoupSession  *soup_session   = NULL;
static GCancellable *fetch_cancel   = NULL;
#endif
static char         *current_url    = NULL;  /* for click-to-open */

/* ------------------------------------------------------------------- *
 * Forward declarations
 * ------------------------------------------------------------------- */

static void banner_show_caption (const char *text);
#ifdef HAVE_LIBSOUP
static void banner_show_pixbuf  (GdkPixbuf *pb);
static void banner_start_url_fetch (const char *url);
static void on_soup_send_done (GObject *source, GAsyncResult *result,
                               gpointer user_data);
#endif
static void on_banner_clicked (GtkGestureClick *gesture, int n_press,
                               double x, double y, gpointer user_data);

/* ------------------------------------------------------------------- *
 * Public API
 * ------------------------------------------------------------------- */

GtkWidget *
banner_widget_new (void)
{
	if (banner_root)
		return banner_root;

	banner_root = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_start  (banner_root, 6);
	gtk_widget_set_margin_end    (banner_root, 6);
	gtk_widget_set_margin_top    (banner_root, 4);
	gtk_widget_set_margin_bottom (banner_root, 4);
	gtk_widget_set_visible (banner_root, FALSE);

	/* GtkPicture, not GtkImage: GtkImage caps its natural size at
	 * the icon-size CSS variable (~16px), which would silently
	 * shrink any banner pixbuf. GtkPicture renders at the
	 * paintable's natural size and we control bounds via
	 * set_size_request.
	 *
	 * gtk_picture_set_keep_aspect_ratio is available on the 4.6
	 * floor; it was deprecated in 4.8 in favour of set_content_fit
	 * but the replacement isn't available pre-4.8 so we keep the
	 * older call and suppress the deprecation. */
	banner_picture = gtk_picture_new ();
	gtk_widget_set_size_request (banner_picture,
	                             -1, BANNER_MAX_H);
	gtk_picture_set_can_shrink (GTK_PICTURE (banner_picture), TRUE);
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	gtk_picture_set_keep_aspect_ratio (GTK_PICTURE (banner_picture),
	                                   TRUE);
	G_GNUC_END_IGNORE_DEPRECATIONS
	gtk_widget_set_visible (banner_picture, FALSE);
	gtk_box_append (GTK_BOX (banner_root), banner_picture);

	banner_caption = gtk_label_new (NULL);
	gtk_label_set_xalign (GTK_LABEL (banner_caption), 0.0);
	gtk_label_set_ellipsize (GTK_LABEL (banner_caption),
	                         PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand (banner_caption, TRUE);
	gtk_widget_add_css_class (banner_caption, "dim-label");
	gtk_box_append (GTK_BOX (banner_root), banner_caption);

	/* Click anywhere on the banner row → open the URL in the
	 * user's default browser. Tooltip mirrors the URL so a hover
	 * preview is enough to verify the destination. */
	{
		GtkGesture *click = gtk_gesture_click_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click),
		                               GDK_BUTTON_PRIMARY);
		g_signal_connect (click, "pressed",
		                  G_CALLBACK (on_banner_clicked), NULL);
		gtk_widget_add_controller (banner_root,
		                           GTK_EVENT_CONTROLLER (click));
	}

	return banner_root;
}

void
banner_handle_message (struct htlc_conn *htlc,
                       const char *type,
                       gboolean has_url,
                       const char *url)
{
	(void) htlc;

	if (!banner_root)
		return;

	debug_log ("banner",
	           "received: type='%s' has_url=%d url='%s'",
	           type ? type : "(null)", has_url ? 1 : 0,
	           url ? url : "");

	/* Drop any in-flight fetch from a previous banner. */
	banner_clear ();

	gtk_widget_set_visible (banner_root, TRUE);

	if (has_url && url && *url) {
		/* URL-mode: cache the URL for click-to-open. With libsoup
		 * present we kick off an inline fetch and swap in the
		 * decoded image when it lands; without libsoup we leave
		 * the URL caption in place and rely on the click handler
		 * to open the URL in the user's browser. */
		g_free (current_url);
		current_url = g_strdup (url);
		gtk_widget_set_tooltip_text (banner_root, url);
		banner_show_caption (url);
#ifdef HAVE_LIBSOUP
		banner_start_url_fetch (url);
#endif
		return;
	}

	/* File-mode (no URL chunk) — server expects HTLC_HDR_BANNER_GET
	 * + HTXF subchannel. That code path lands in a follow-up commit;
	 * for now show a type-only caption so the user knows a banner
	 * was advertised but isn't displayable yet. */
	{
		gchar *caption = g_strdup_printf (
			_("Server banner [%s] — file-mode fetch not yet implemented"),
			type ? type : "?");
		banner_show_caption (caption);
		g_free (caption);
	}
}

void
banner_clear (void)
{
#ifdef HAVE_LIBSOUP
	if (fetch_cancel) {
		g_cancellable_cancel (fetch_cancel);
		g_clear_object (&fetch_cancel);
	}
#endif
	g_free (current_url);
	current_url = NULL;

	if (banner_picture) {
		gtk_picture_set_paintable (GTK_PICTURE (banner_picture), NULL);
		gtk_widget_set_visible (banner_picture, FALSE);
	}
	if (banner_caption)
		gtk_label_set_text (GTK_LABEL (banner_caption), "");
	if (banner_root) {
		gtk_widget_set_tooltip_text (banner_root, NULL);
		gtk_widget_set_visible (banner_root, FALSE);
	}
}

/* ------------------------------------------------------------------- *
 * Internals
 * ------------------------------------------------------------------- */

static void
banner_show_caption (const char *text)
{
	if (banner_caption)
		gtk_label_set_text (GTK_LABEL (banner_caption),
		                    text ? text : "");
}

#ifdef HAVE_LIBSOUP
static void
banner_show_pixbuf (GdkPixbuf *pb)
{
	GdkTexture *tex;
	int w, h;

	if (!banner_picture || !pb)
		return;

	w = gdk_pixbuf_get_width (pb);
	h = gdk_pixbuf_get_height (pb);

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	tex = gdk_texture_new_for_pixbuf (pb);
	G_GNUC_END_IGNORE_DEPRECATIONS

	gtk_picture_set_paintable (GTK_PICTURE (banner_picture),
	                           GDK_PAINTABLE (tex));
	g_object_unref (tex);

	/* Pin the picture's allocation so the layout doesn't reflow
	 * once the natural-size hint kicks in. Cap to BANNER_MAX_W /
	 * BANNER_MAX_H; aspect ratio preserved by CONTENT_FIT_CONTAIN. */
	{
		int cap_w = (w > BANNER_MAX_W) ? BANNER_MAX_W : w;
		int cap_h = (h > BANNER_MAX_H) ? BANNER_MAX_H : h;
		gtk_widget_set_size_request (banner_picture, cap_w, cap_h);
	}

	gtk_widget_set_visible (banner_picture, TRUE);
	/* Once the image is up the URL-as-caption is redundant; the
	 * tooltip and click-to-open carry that affordance. */
	banner_show_caption ("");
}

/* libsoup async fetch ----------------------------------------------- */

static SoupSession *
ensure_session (void)
{
	if (!soup_session) {
		soup_session = soup_session_new ();
		/* Sensible defaults; we don't need persistent cookies or
		 * other per-app session state for banner fetches. */
		soup_session_set_user_agent (soup_session,
		                             "GtkHx/Phase5 ");
		soup_session_set_timeout    (soup_session, 10);
		soup_session_set_idle_timeout (soup_session, 20);
	}
	return soup_session;
}

static void
banner_start_url_fetch (const char *url)
{
	SoupMessage *msg;
	GUri *uri;
	GError *err = NULL;

	uri = g_uri_parse (url, G_URI_FLAGS_NONE, &err);
	if (!uri) {
		debug_log ("banner", "URL parse failed: %s",
		           err ? err->message : "?");
		g_clear_error (&err);
		banner_show_caption (
			_("Server banner: URL is not parseable"));
		return;
	}
	msg = soup_message_new_from_uri ("GET", uri);
	g_uri_unref (uri);

	fetch_cancel = g_cancellable_new ();

	soup_session_send_and_read_async (
		ensure_session (), msg,
		G_PRIORITY_DEFAULT, fetch_cancel,
		on_soup_send_done, msg);
}

static void
on_soup_send_done (GObject *source, GAsyncResult *result,
                   gpointer user_data)
{
	SoupSession *session = SOUP_SESSION (source);
	SoupMessage *msg = SOUP_MESSAGE (user_data);
	GError *err = NULL;
	GBytes *bytes;
	GdkPixbuf *pb;
	GInputStream *stream;
	guint status;

	bytes = soup_session_send_and_read_finish (
		session, result, &err);

	/* The cancellable was bound to this fetch; the next fetch
	 * (or banner_clear) will create a new one. Clear our handle
	 * so banner_clear's cancel-then-clear path doesn't try to
	 * cancel a finished operation. */
	g_clear_object (&fetch_cancel);

	if (!bytes) {
		debug_log ("banner", "soup fetch failed: %s",
		           err ? err->message : "?");
		if (err && !g_error_matches (err, G_IO_ERROR,
		                             G_IO_ERROR_CANCELLED))
			banner_show_caption (
				_("Server banner: fetch failed"));
		g_clear_error (&err);
		g_object_unref (msg);
		return;
	}

	status = soup_message_get_status (msg);
	if (status < 200 || status >= 300) {
		debug_log ("banner", "soup fetch http %u", status);
		banner_show_caption (
			_("Server banner: HTTP error"));
		g_bytes_unref (bytes);
		g_object_unref (msg);
		return;
	}

	stream = g_memory_input_stream_new_from_bytes (bytes);
	pb = gdk_pixbuf_new_from_stream (stream, NULL, &err);
	g_object_unref (stream);
	g_bytes_unref (bytes);

	if (!pb) {
		debug_log ("banner", "pixbuf decode failed: %s",
		           err ? err->message : "?");
		g_clear_error (&err);
		banner_show_caption (
			_("Server banner: image not decodable"));
		g_object_unref (msg);
		return;
	}

	banner_show_pixbuf (pb);
	g_object_unref (pb);
	g_object_unref (msg);
}
#endif /* HAVE_LIBSOUP */

/* Click handler ------------------------------------------------------ */

static void
on_banner_clicked (GtkGestureClick *gesture, int n_press,
                   double x, double y, gpointer user_data)
{
	GtkWidget *anchor;
	GtkUriLauncher *launcher;
	GtkWindow *parent;
	(void) n_press; (void) x; (void) y; (void) user_data;

	if (!current_url || !*current_url)
		return;

	anchor = gtk_event_controller_get_widget (
		GTK_EVENT_CONTROLLER (gesture));
	parent = GTK_WINDOW (gtk_widget_get_root (anchor));

	launcher = gtk_uri_launcher_new (current_url);
	gtk_uri_launcher_launch (launcher, parent, NULL, NULL, NULL);
	g_object_unref (launcher);
}
