#ifndef HX_GTKUTIL_H
#define HX_GTKUTIL_H

extern void init_keyaccel(GtkWidget *widget);
extern void set_disconnect_btn(session *sess, int stat);
extern void setbtns(session *sess, int stat);
extern void set_status_bar(int status);
extern void changetitlesconnected(session *sess);
extern void changetitlespecific();
extern void changetitlesdisconnected(session *sess);
extern void close_connected_windows(session *sess);
extern void error_dialog(char *title, char *msg);

/*
 * gtk_dialog_get_action_area was deprecated in GTK 3.12 with no
 * direct replacement (gtk_dialog_add_button covers the common case
 * but we have many sites that pack pre-existing buttons with custom
 * click handlers). The action area itself is still functional and
 * remains the right place to pack response widgets — the wholesale
 * rewrite is Phase 4 work. This wrapper hides the deprecation in one
 * place so the per-call-site noise goes away.
 */
extern GtkWidget *gtkhx_dialog_action_area (GtkDialog *dialog);

/*
 * Phase 3.9: GtkTable → GtkGrid migration helpers. The two APIs have
 * different shapes: Table takes (left, right, top, bottom) inclusive
 * spans plus per-axis xoptions/yoptions/xpad/ypad; Grid takes
 * (left, top, width, height) and child-widget hexpand/vexpand +
 * halign/valign + margin properties for the rest.
 *
 * These wrappers preserve the gtk_table_* call shape so the migration
 * was a per-site rename rather than per-site rewrite of ~9 lines of
 * widget property setters. The underlying GTK 3 grid is the real
 * thing — these are not a re-implementation of GtkTable, just an
 * adapter layer that lets us keep the table-style spans/options
 * vocabulary at call sites.
 */
extern GtkWidget *gtkhx_grid_new_table (int rows, int cols, gboolean homogeneous);
extern void gtkhx_grid_attach_table (GtkGrid *grid, GtkWidget *child,
                                     int left, int right,
                                     int top,  int bottom,
                                     int xoptions, int yoptions,
                                     int xpad, int ypad);
extern void gtkhx_grid_attach_table_defaults (GtkGrid *grid, GtkWidget *child,
                                              int left, int right,
                                              int top,  int bottom);

#endif
