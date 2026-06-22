#ifndef GTKHX_H
#define GTKHX_H

extern struct ifn icon_files;
extern struct ifn user_icon_files;
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

/* Style a GtkTextView used as an editable input (chat / pchat / PM
 * input). Calls gtk_text_view_set_monospace(TRUE), which applies GTK's
 * built-in .monospace CSS class — the theme picks a monospace face at
 * the theme's UI size. Deliberately does NOT honor gtkhx_font_desc /
 * the Settings font preference: applying the configured font on the
 * input (whether via .gtkhx-text CSS, PangoContext, or GtkTextTag)
 * triggers an unresolved per-line ascender-ink clip on newly typed
 * glyphs at small Monospace sizes. See gtkhx_apply_input_font in
 * gtkhx.c for the full history. */
extern void gtkhx_apply_input_font (GtkWidget *w);

/* Tag an editable text widget (chat / pchat / PM input) with the
 * .gtkhx-input CSS class so it picks up the active theme's fg / bg /
 * caret colors. Independent of gtkhx_apply_input_font: that one
 * applies the monospace font class; this one only paints colors.
 * The two are usually called together. */
extern void gtkhx_apply_input_style (GtkWidget *w);

/* Tag a list-shaped widget (GtkColumnView / GtkListView) with the
 * .gtkhx-listview CSS class so its rows and the empty area below
 * them follow the active theme's fg/bg. Used for tracker, files
 * browser, and news browser. Selection styling is left to the
 * system theme so selected rows stay visually distinct. */
extern void gtkhx_apply_listview_style (GtkWidget *w);

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
