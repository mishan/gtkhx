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
#include <string.h>
#include "hx.h"
#include "about.h"
#include "gtkutil.h"


#ifdef HAVE_DCGETTEXT
#include <libintl.h>
#define _(string) dgettext (PACKAGE, string)
#else
#define _(string) (string)
#endif

static void set_notebook_tab (GtkWidget *notebook, gint page_num,
							  GtkWidget *widget);

#define ABOUT_DEFAULT_WIDTH               100
#define ABOUT_MAX_WIDTH                   600
#define LINE_SKIP                          4

static gchar Ver[]    = "GtkHx %s";

/* external variables */

gchar *get_credits()
{
   static gchar credits[] =
"\
       Main Programming: Misha Nasledov \n\
                         <misha@nasledov.com>\n\
                         Ryan Nielsen \n\
                         <ran@krazynet.com>\n\
                         David Raufeisen\n\
                         <david@fortyoz.org>\n\
                         Aaron Lehmann\n\
                         <aaronl@vitelus.com>\n\n\
               Graphics: apocalypse\n\
                         <apocalypse@cafelinux.dhs.org>\n\
                         Philip Neustrom\n\
                         <codetoad@pacbell.net>\n\n\
          Documentation: Philip Neustrom\n\
                         <codetoad@pacbell.net>\n\n\
               Web Site: Jonathan C. Sitte\n\
                         <jcsitte@jcsitte.com>\n\n\
    French Localization: Jean-Sebastien Hubert\n\
                         <jshubert@mirabellug.org>\n\
";
   return (gchar*) credits;
}

gboolean about_open = FALSE;
GtkWidget *frmAbout;

void on_cmdAboutClose_clicked ()
{
	gtkhx_widget_destroy(frmAbout);
}

void on_frmAbout_destroy ()
{
    gtkhx_widget_destroy(frmAbout);
	about_open = FALSE;

}

void create_about_window (GtkWidget *widget, gpointer data)
{
	(void) widget; (void) data;

	if(about_open == FALSE)
	{
    GtkWidget *fixed;
    GtkWidget *cmdAboutClose;
    GtkWidget *notebook;
    GtkWidget *fixed1;
    GtkWidget *frame;
    GtkWidget *lblTitle;
    GtkWidget *lblCopyright;
    GtkWidget *scrolledwindow;
    GtkWidget *txtCredits;
    GtkTextBuffer *credits_buf;
    GtkWidget *lblCredits;
    GtkWidget *pixmap;
    GdkPixmap *icon;
    GtkAdjustment *adj;
    char version [50];

    frmAbout = gtk_window_new();
    gtk_widget_set_size_request (frmAbout, 482, 450);
    gtk_window_set_title (GTK_WINDOW (frmAbout), _("About GtkHx"));
    gtk_window_set_resizable(GTK_WINDOW (frmAbout), FALSE);
    g_signal_connect (frmAbout, "destroy",
			G_CALLBACK (on_frmAbout_destroy),
			frmAbout);

    /* Phase 4.5: dropped GTK 1.2/2-era gtk_widget_realize. */

    fixed = gtk_fixed_new ();
    gtkhx_widget_set_child(frmAbout, fixed);

    cmdAboutClose = gtk_button_new_with_label (("Close"));
    gtk_fixed_put (GTK_FIXED (fixed), cmdAboutClose, 384, 403);
    gtk_widget_set_size_request (cmdAboutClose, 88, 36);
    /* Phase 4.2: gtk_widget_set_can_default removed */
    gtk_widget_grab_focus (cmdAboutClose);
    /* Phase 4.2: gtk_widget_grab_default removed (use gtk_window_set_default_widget if needed) */
    g_signal_connect (cmdAboutClose, "clicked",
			G_CALLBACK (on_cmdAboutClose_clicked),
			frmAbout);


    notebook = gtk_notebook_new ();
    gtk_fixed_put (GTK_FIXED (fixed), notebook, 8, 8);
    gtk_widget_set_size_request (notebook, 466, 382);


    fixed1 = gtk_fixed_new ();
    gtkhx_widget_set_child(notebook, fixed1);


    frame = gtk_frame_new (NULL);
    gtk_fixed_put (GTK_FIXED (fixed1), frame, 32, 12);
    gtk_widget_set_size_request (frame, 400, 200);

    icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/gtkhx.xpm", NULL);
    pixmap = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
    gtkhx_widget_set_child(frame, pixmap);

    g_snprintf (version, sizeof(version), Ver, VERSION); /* Insert version from config.h */

    lblTitle = gtk_label_new (version);
    gtk_fixed_put (GTK_FIXED (fixed1), lblTitle, 8, 215);
    gtk_widget_set_size_request (lblTitle, 448, 16);


    lblCopyright = gtk_label_new (_("Copyright (C) 2000-2002"));
    gtk_fixed_put (GTK_FIXED (fixed1), lblCopyright, 8, 321);
    gtk_widget_set_size_request (lblCopyright, 448, 16);


    scrolledwindow = gtk_scrolled_window_new();
    gtk_fixed_put (GTK_FIXED (fixed1), scrolledwindow, 12, 240);
    gtk_widget_set_size_request (scrolledwindow, 436, 100);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow),
									GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);


    txtCredits = gtk_text_view_new ();
    gtk_text_view_set_editable (GTK_TEXT_VIEW (txtCredits), FALSE);
    gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (txtCredits), FALSE);
    gtkhx_widget_set_child(scrolledwindow, txtCredits);
    /* Phase 4.5: dropped GTK 1.2/2-era gtk_widget_realize. */
    credits_buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (txtCredits));
    gtk_text_buffer_set_text (credits_buf, get_credits(), -1);

    adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW(
												   scrolledwindow));
    gtk_adjustment_set_value (adj, 0);

    lblCredits = gtk_label_new (_("Credits"));
    set_notebook_tab (notebook, 0, lblCredits);

    gtk_window_present(GTK_WINDOW(frmAbout));
    about_open = TRUE;
	}
}

/* This is an internally used function to set notebook tab widgets. */
static void set_notebook_tab (GtkWidget *notebook, gint page_num,
							  GtkWidget *widget)
{
    GtkWidget *notebook_page;
    notebook_page = gtk_notebook_get_nth_page (GTK_NOTEBOOK (notebook), page_num);
    gtk_notebook_set_tab_label (GTK_NOTEBOOK (notebook), notebook_page, widget);
}
