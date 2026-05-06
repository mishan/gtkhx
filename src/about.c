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

/* Phase 5: AdwAboutDialog (libadwaita 1.6+) replaces the hand-laid
 * GtkFixed-based GtkDialog. AdwDialog is the modern adaptive dialog
 * family — presents as a centered window on desktop and a bottom
 * sheet on mobile. We don't need to track the dialog as a singleton:
 * adw_dialog_present is idempotent for an already-presented dialog
 * (it just re-focuses), and the dialog auto-destroys when closed. */

void
create_about_window (GtkWidget *widget, gpointer data)
{
	(void) widget; (void) data;

	/* Authors of the original GtkHx development effort, preserved
	 * from the previous about dialog. Format follows AdwAboutDialog
	 * convention: "Name <email>". */
	static const char *developers[] = {
		"Misha Nasledov <misha@nasledov.com>",
		"Ryan Nielsen <ran@krazynet.com>",
		"David Raufeisen <david@fortyoz.org>",
		"Aaron Lehmann <aaronl@vitelus.com>",
		NULL,
	};
	static const char *artists[] = {
		"apocalypse <apocalypse@cafelinux.dhs.org>",
		"Philip Neustrom <codetoad@pacbell.net>",
		NULL,
	};
	static const char *documenters[] = {
		"Philip Neustrom <codetoad@pacbell.net>",
		NULL,
	};

	AdwDialog *dlg = ADW_DIALOG (adw_about_dialog_new ());

	g_object_set (dlg,
		"application-name", "GtkHx",
		"application-icon", "applications-internet-symbolic",
		"version", VERSION,
		"developer-name", "Misha Nasledov",
		"copyright", "© 2000–2026 Misha Nasledov",
		"license-type", GTK_LICENSE_GPL_2_0,
		"comments", _("A GTK client for the Hotline protocol."),
		"website", "https://github.com/nasledov/gtkhx",
		"issue-url", "https://github.com/nasledov/gtkhx/issues",
		NULL);

	adw_about_dialog_set_developers  (ADW_ABOUT_DIALOG (dlg), developers);
	adw_about_dialog_set_artists     (ADW_ABOUT_DIALOG (dlg), artists);
	adw_about_dialog_set_documenters (ADW_ABOUT_DIALOG (dlg), documenters);
	/* "translator-credits" is a magic gettext string — translators
	 * fill it in via po files. The fallback English string lists the
	 * historic French translator. */
	adw_about_dialog_set_translator_credits (
		ADW_ABOUT_DIALOG (dlg),
		_("Jean-Sebastien Hubert <jshubert@mirabellug.org>"));

	adw_dialog_present (dlg, GTK_WIDGET (gtkhx_active_window ()));
}
