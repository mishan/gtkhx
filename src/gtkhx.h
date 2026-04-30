#ifndef GTKHX_H
#define GTKHX_H

extern struct ifn icon_files;
extern struct ifn user_icon_files;
extern GtkWidget *agreementwin;
extern PangoFontDescription *gtkhx_font_desc;

extern struct ifn user_icon_files;
extern struct ifn icon_files;
extern GdkColor fg_col;
extern GdkColor bg_col;

extern void init_icons (void);

/* Apply gtkhx_font_desc plus fg_col/bg_col to a text-bearing widget
 * (GtkEntry, GtkTextView, GtkLabel, …). Replaces the GTK 1.2 idiom of
 * gtk_widget_set_style(w, gtktext_style). */
extern void gtkhx_apply_text_style (GtkWidget *w);

#endif
