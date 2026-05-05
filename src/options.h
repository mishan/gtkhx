#ifndef HX_OPTIONS_H
#define HX_OPTIONS_H

extern void create_options_window (GtkWidget *widget, gpointer data);
extern void init_variables (void);
extern void prefs_read (void);
extern void prefs_write (void);
extern time_t start_time, total_time;

#endif
