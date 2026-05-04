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
 * gtk_widget_set_style(w, gtktext_style). Implementation in Phase 3.5
 * is a screen-wide GtkCssProvider keyed off the .gtkhx-text class. */
extern void gtkhx_apply_text_style (GtkWidget *w);

/* Same idea for the user list font (independent prefs entry, no
 * fg/bg). Tags the widget with the .gtkhx-userlist class. */
extern void gtkhx_apply_userlist_style (GtkWidget *w);

/* Rebuild the screen-wide CSS provider after gtkhx_font_desc / fg_col /
 * bg_col change. Already-tagged widgets re-render automatically. */
extern void gtkhx_refresh_css (void);

/* Rebuild the user list CSS provider after the user list font changes. */
extern void gtkhx_refresh_userlist_css (PangoFontDescription *fd);

#endif
