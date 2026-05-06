/*  This file adapted from Spruce
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

#include "config.h"
#include <stdio.h>
#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>
#include "hx.h"
#include "about.h"
#include "gtkhx.h"
#include "gtkutil.h"


#ifdef HAVE_DCGETTEXT
#include <libintl.h>
#define _(string) dgettext (PACKAGE, string)
#else
#define _(string) (string)
#endif

/* Phase 5: keep the GtkHx-original "logo + custom credits" feel
 * (AdwAboutDialog's rigid slot layout was too constraining), but
 * polish the implementation:
 *   - GtkBox/GtkGrid layout instead of GtkFixed pixel positioning
 *   - AdwHeaderBar in the title-bar slot for window chrome
 *     consistency with the rest of the app
 *   - Pango markup on the version + subtitle for proper typography
 *   - GtkNotebook (one tab) is gone; the credits view is just
 *     packed directly
 *   - No explicit Close button; the headerbar's close-X +
 *     ESC-dismiss handle teardown
 * The historic credit lines are preserved as-is. */

static const char *
get_credits (void)
{
	return
"       Main Programming: Misha Nasledov\n"
"                         <misha@nasledov.com>\n"
"                         Ryan Nielsen\n"
"                         <ran@krazynet.com>\n"
"                         David Raufeisen\n"
"                         <david@fortyoz.org>\n"
"                         Aaron Lehmann\n"
"                         <aaronl@vitelus.com>\n"
"\n"
"               Graphics: apocalypse\n"
"                         <apocalypse@cafelinux.dhs.org>\n"
"                         Philip Neustrom\n"
"                         <codetoad@pacbell.net>\n"
"\n"
"          Documentation: Philip Neustrom\n"
"                         <codetoad@pacbell.net>\n"
"\n"
"               Web Site: Jonathan C. Sitte\n"
"                         <jcsitte@jcsitte.com>\n"
"\n"
"    French Localization: Jean-Sebastien Hubert\n"
"                         <jshubert@mirabellug.org>\n";
}

static GtkWidget *about_window;

static void
on_about_destroy (GtkWidget *w, gpointer data)
{
	(void) w; (void) data;
	about_window = NULL;
}

void
create_about_window (GtkWidget *widget, gpointer data)
{
	GtkWidget *box, *logo, *title, *subtitle, *copyright;
	GtkWidget *scrolled, *credits;
	GtkTextBuffer *credits_buf;
	GdkPixbuf *logo_pb;
	GdkTexture *logo_tex;
	char *markup;

	(void) widget; (void) data;

	if (about_window) {
		gtk_window_present (GTK_WINDOW (about_window));
		return;
	}

	about_window = gtk_window_new ();
	gtk_window_set_title (GTK_WINDOW (about_window), _("About GtkHx"));
	gtk_window_set_resizable (GTK_WINDOW (about_window), FALSE);
	gtk_window_set_default_size (GTK_WINDOW (about_window), 480, 540);
	gtk_window_set_titlebar (GTK_WINDOW (about_window), adw_header_bar_new ());

	{
		GtkWindow *parent = gtkhx_active_window ();
		if (parent)
			gtk_window_set_transient_for (GTK_WINDOW (about_window),
			                              parent);
	}

	g_signal_connect (about_window, "destroy",
	                  G_CALLBACK (on_about_destroy), NULL);

	box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_top    (box, 18);
	gtk_widget_set_margin_bottom (box, 18);
	gtk_widget_set_margin_start  (box, 18);
	gtk_widget_set_margin_end    (box, 18);

	/* Logo — GtkPicture for natural-size rendering. The gtkhx.xpm
	 * resource is 400x200; GtkImage would treat it as a themed icon
	 * and scale it down to ~24px (giving the "thin colored bar"
	 * look). GtkPicture renders the paintable at its real
	 * dimensions, with set_can_shrink(FALSE) pinning it so the
	 * dialog's width drives sizing rather than the picture being
	 * stretched. */
	logo_pb = gdk_pixbuf_new_from_resource (
		"/com/nasledov/gtkhx/pixmaps/gtkhx.xpm", NULL);
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	logo_tex = gdk_texture_new_for_pixbuf (logo_pb);
	G_GNUC_END_IGNORE_DEPRECATIONS
	logo = gtk_picture_new_for_paintable (GDK_PAINTABLE (logo_tex));
	gtk_picture_set_can_shrink (GTK_PICTURE (logo), FALSE);
	gtk_widget_set_halign (logo, GTK_ALIGN_CENTER);
	gtk_box_append (GTK_BOX (box), logo);
	g_clear_object (&logo_tex);
	g_clear_object (&logo_pb);

	/* Title — "GtkHx 0.9.5-dev" in a larger weight */
	markup = g_strdup_printf (
		"<span size=\"x-large\" weight=\"bold\">GtkHx %s</span>",
		VERSION);
	title = gtk_label_new (NULL);
	gtk_label_set_markup (GTK_LABEL (title), markup);
	gtk_widget_set_halign (title, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top (title, 6);
	gtk_box_append (GTK_BOX (box), title);
	g_free (markup);

	/* One-line tagline below the title — dim, smaller */
	subtitle = gtk_label_new (_("A GTK client for the Hotline protocol"));
	gtk_widget_add_css_class (subtitle, "dim-label");
	gtk_widget_set_halign (subtitle, GTK_ALIGN_CENTER);
	gtk_box_append (GTK_BOX (box), subtitle);

	/* Copyright */
	copyright = gtk_label_new (_("Copyright © 2000–2026 Misha Nasledov"));
	gtk_widget_add_css_class (copyright, "dim-label");
	gtk_widget_set_halign (copyright, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top (copyright, 6);
	gtk_box_append (GTK_BOX (box), copyright);

	/* Credits — scrolled, monospace text view */
	scrolled = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
	                                GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request (scrolled, -1, 180);
	gtk_widget_set_vexpand (scrolled, TRUE);
	gtk_widget_set_margin_top (scrolled, 12);

	credits = gtk_text_view_new ();
	gtk_text_view_set_editable (GTK_TEXT_VIEW (credits), FALSE);
	gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (credits), FALSE);
	gtk_text_view_set_monospace (GTK_TEXT_VIEW (credits), TRUE);
	gtk_widget_add_css_class (credits, "view");
	credits_buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (credits));
	gtk_text_buffer_set_text (credits_buf, get_credits (), -1);
	gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), credits);

	gtk_box_append (GTK_BOX (box), scrolled);

	gtk_window_set_child (GTK_WINDOW (about_window), box);

	gtk_window_present (GTK_WINDOW (about_window));
}
