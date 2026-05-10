/*
 * banner.c — server banner UI surface and fetch state machines.
 *
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This file is the skeleton — widget plumbing + dispatch entry
 * point — landed in the first commit of the banner feature work.
 * URL-mode (libsoup fetch) and HTXF-mode (BANNER_GET subchannel)
 * implementations land in subsequent commits.
 *
 * Released under GPL-2.0-or-later. See COPYING.
 */

#include "config.h"
#include <string.h>
#include <gtk/gtk.h>
#include "compat.h"
#include "debug.h"
#include "banner.h"

/* The single banner widget for this process. Created by
 * banner_widget_new() from toolbar.c, NULL while no toolbar is up.
 * banner_handle_message routes to whatever this points at. */
static GtkWidget *banner_root  = NULL;
static GtkWidget *banner_label = NULL;

GtkWidget *
banner_widget_new (void)
{
	if (banner_root)
		return banner_root;

	/* Placeholder layout: a single horizontal box with a status
	 * label. The label gets replaced with the real GtkPicture +
	 * URL link in subsequent commits. Hidden by default — we
	 * reveal it only when a banner has actually arrived. */
	banner_root = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_start  (banner_root, 6);
	gtk_widget_set_margin_end    (banner_root, 6);
	gtk_widget_set_margin_top    (banner_root, 4);
	gtk_widget_set_margin_bottom (banner_root, 4);
	gtk_widget_set_visible (banner_root, FALSE);

	banner_label = gtk_label_new (NULL);
	gtk_widget_add_css_class (banner_label, "dim-label");
	gtk_box_append (GTK_BOX (banner_root), banner_label);

	return banner_root;
}

void
banner_handle_message (struct htlc_conn *htlc,
                       const char *type,
                       gboolean has_url,
                       const char *url)
{
	(void) htlc;

	if (!banner_root || !banner_label)
		return;

	debug_log ("banner",
	           "received: type='%s' has_url=%d url='%s'",
	           type ? type : "(null)", has_url ? 1 : 0,
	           url ? url : "");

	/* TODO Phase 5: route to URL fetcher (libsoup) for "URL " type
	 * and to HTLC_HDR_BANNER_GET + HTXF subchannel otherwise. The
	 * placeholder shows a one-line summary so we can confirm the
	 * receive path landed. */
	{
		gchar *text;
		if (has_url)
			text = g_strdup_printf (
				_("Server banner [%s]: %s"), type, url);
		else
			text = g_strdup_printf (
				_("Server banner [%s] — fetch not yet implemented"),
				type);
		gtk_label_set_text (GTK_LABEL (banner_label), text);
		g_free (text);
	}
	gtk_widget_set_visible (banner_root, TRUE);
}

void
banner_clear (void)
{
	if (!banner_root)
		return;
	if (banner_label)
		gtk_label_set_text (GTK_LABEL (banner_label), "");
	gtk_widget_set_visible (banner_root, FALSE);
}
