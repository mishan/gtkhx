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
#include "about.h"
#include "pixmaps/gtkhx.xpm"


#ifdef HAVE_DCGETTEXT
#include <libintl.h>
#define _(string) dgettext (PACKAGE, string)
#else
#define _(string) (string)
#endif

static void set_notebook_tab (GtkWidget *notebook, gint page_num, 
							  GtkWidget *widget);

/* FONTS */
#define HELVETICA_20_BFONT "-adobe-helvetica-bold-r-normal-*-20-*-*-*-*-*-*-*"
#define HELVETICA_14_BFONT "-adobe-helvetica-bold-r-normal-*-14-*-*-*-*-*-*-*"
#define HELVETICA_12_BFONT "-adobe-helvetica-bold-r-normal-*-12-*-*-*-*-*-*-*"
#define HELVETICA_12_FONT  "-adobe-helvetica-medium-r-normal-*-12-*-*-*-*-*-*-*"
#define HELVETICA_10_FONT  "-adobe-helvetica-medium-r-normal-*-10-*-*-*-*-*-*-*"
#define CREDITS_FONT       "-misc-fixed-medium-r-normal--13-120-75-75-c-70-" \
"iso8859-1"

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
	gtk_widget_destroy(frmAbout);
}

void on_frmAbout_destroy ()
{
    gtk_widget_destroy(frmAbout);
	about_open = FALSE;

}

void create_about_window ()
{
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
    GtkWidget *lblCredits;
    GtkWidget *pixmap;
    GdkPixmap *icon;
    GdkBitmap *mask;
    GdkFont   *fixed_font;
    GtkAdjustment *adj;
    char version [50];

    frmAbout = gtk_window_new (GTK_WINDOW_DIALOG);
    gtk_widget_set_usize (frmAbout, 482, 450);
    gtk_window_set_title (GTK_WINDOW (frmAbout), _("About GtkHx"));
    gtk_window_set_policy (GTK_WINDOW (frmAbout), FALSE, FALSE, FALSE);
    gtk_signal_connect (GTK_OBJECT (frmAbout), "destroy",
			GTK_SIGNAL_FUNC (on_frmAbout_destroy),
			GTK_OBJECT (frmAbout));

    gtk_widget_realize(frmAbout);


    fixed = gtk_fixed_new ();
    gtk_container_add (GTK_CONTAINER (frmAbout), fixed);

    cmdAboutClose = gtk_button_new_with_label (("Close"));
    gtk_fixed_put (GTK_FIXED (fixed), cmdAboutClose, 384, 393);
    gtk_widget_set_uposition (cmdAboutClose, 384, 403);
    gtk_widget_set_usize (cmdAboutClose, 88, 36);
    GTK_WIDGET_SET_FLAGS (cmdAboutClose, GTK_CAN_DEFAULT);
    gtk_widget_grab_focus (cmdAboutClose);
    gtk_widget_grab_default (cmdAboutClose);
    gtk_signal_connect (GTK_OBJECT (cmdAboutClose), "clicked",
			GTK_SIGNAL_FUNC (on_cmdAboutClose_clicked),
			GTK_OBJECT(frmAbout));


    notebook = gtk_notebook_new ();
    gtk_fixed_put (GTK_FIXED (fixed), notebook, 8, 8);
    gtk_widget_set_uposition (notebook, 8, 8);
    gtk_widget_set_usize (notebook, 466, 382);


    fixed1 = gtk_fixed_new ();
    gtk_container_add (GTK_CONTAINER (notebook), fixed1);


    frame = gtk_frame_new (NULL);
    gtk_fixed_put (GTK_FIXED (fixed1), frame, 32, 12);
    gtk_widget_set_uposition (frame, 32, 12);
    gtk_widget_set_usize (frame, 400, 200);
    gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_OUT);

    icon = gdk_pixmap_create_from_xpm_d (frmAbout->window, &mask,
										 &frmAbout->style->white, gtkhx_xpm);
    pixmap = gtk_pixmap_new(icon, mask);
    gtk_container_add (GTK_CONTAINER (frame), pixmap);

    sprintf (version, (Ver), VERSION); /* Insert version from config.h */

    lblTitle = gtk_label_new (version);
    gtk_fixed_put (GTK_FIXED (fixed1), lblTitle, 8, 175);
    gtk_widget_set_uposition (lblTitle, 8, 215);
    gtk_widget_set_usize (lblTitle, 448, 16);


    lblCopyright = gtk_label_new (_("Copyright (C) 2000-2002"));
    gtk_fixed_put (GTK_FIXED (fixed1), lblCopyright, 8, 191);
    gtk_widget_set_uposition (lblCopyright, 8, 321);
    gtk_widget_set_usize (lblCopyright, 448, 16);


    scrolledwindow = gtk_scrolled_window_new (NULL, NULL);
    gtk_fixed_put (GTK_FIXED (fixed1), scrolledwindow, 12, 220);
    gtk_widget_set_uposition (scrolledwindow, 12, 240);
    gtk_widget_set_usize (scrolledwindow, 436, 100);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolledwindow), 
									GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);


    fixed_font = gdk_font_load (CREDITS_FONT);

    txtCredits = gtk_text_new (NULL, NULL);
    gtk_container_add (GTK_CONTAINER (scrolledwindow), txtCredits);
    gtk_widget_realize (txtCredits);
    gtk_text_insert (GTK_TEXT (txtCredits), fixed_font, NULL, NULL, 
					 (get_credits()), -1);
    gtk_text_set_point(GTK_TEXT(txtCredits), 0);

    adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW(
												   scrolledwindow));
    gtk_adjustment_set_value (adj, 0);

    lblCredits = gtk_label_new (_("Credits"));
    set_notebook_tab (notebook, 0, lblCredits);

    gtk_widget_show_all(frmAbout);
    about_open = TRUE;
	}
}

/* This is an internally used function to set notebook tab widgets. */
static void set_notebook_tab (GtkWidget *notebook, gint page_num, 
							  GtkWidget *widget)
{
    GtkNotebookPage *page;
    GtkWidget *notebook_page;
    page = (GtkNotebookPage*) g_list_nth (GTK_NOTEBOOK (notebook)->children,
										  page_num)->data;
    notebook_page = page->child;
    gtk_widget_ref (notebook_page);
    gtk_notebook_remove_page (GTK_NOTEBOOK (notebook), page_num);
    gtk_notebook_insert_page (GTK_NOTEBOOK (notebook), notebook_page,
			      widget, page_num);
    gtk_widget_unref (notebook_page);
}
