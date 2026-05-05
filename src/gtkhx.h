#ifndef GTKHX_H
#define GTKHX_H

extern struct ifn icon_files;
extern struct ifn user_icon_files;
extern GtkWidget *agreementwin;
extern PangoFontDescription *gtkhx_font_desc;

extern struct ifn user_icon_files;
extern struct ifn icon_files;
extern GdkRGBA fg_col;
extern GdkRGBA bg_col;

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

/* Returns the application's currently-active toplevel, or NULL during
 * early startup. Callers use it as a transient parent for dialogs
 * spawned from a context without a natural parent widget. */
extern GtkWindow *gtkhx_active_window (void);

/* Returns the AdwApplication singleton as a plain GApplication, or
 * NULL during very early startup before loop() constructs it.
 * Callers that need to register GActions or hand the application
 * pointer to a constructor should use this rather than
 * g_application_get_default() — get_default() relies on a
 * thread-local that's set during GApplication's constructor and
 * isn't a guaranteed contract for our callers' timing assumptions. */
extern GApplication *gtkhx_get_application (void);

/* Returns the per-user config directory, creating it if needed.
 * Resolution: $GTKHX_PATH > $XDG_CONFIG_HOME/gtkhx > $HOME/.config/gtkhx.
 * String is owned by the function; do not free. Cached after the
 * first call. */
extern const char *gtkhx_config_dir (void);

#endif
