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
/*
 * Phase 4.2: GtkAttachOptions / GTK_FILL / GTK_EXPAND were the legacy
 * bit flags for gtk_table_attach. They're gone in GTK 4. Provide
 * compat values keyed off the bit pattern that gtkhx_grid_attach_table
 * already understands so unmodified call sites continue to compile.
 */
#ifndef GTK_FILL
#define GTK_FILL    1
#endif
#ifndef GTK_EXPAND
#define GTK_EXPAND  2
#endif

extern GtkWidget *gtkhx_grid_new_table (int rows, int cols, gboolean homogeneous);
extern void gtkhx_grid_attach_table (GtkGrid *grid, GtkWidget *child,
                                     int left, int right,
                                     int top,  int bottom,
                                     int xoptions, int yoptions,
                                     int xpad, int ypad);
extern void gtkhx_grid_attach_table_defaults (GtkGrid *grid, GtkWidget *child,
                                              int left, int right,
                                              int top,  int bottom);

/*
 * Phase 4.2: GtkContainer is gone in GTK 4. Each widget that used to
 * be a container exposes a type-specific child setter instead
 * (gtk_window_set_child / gtk_scrolled_window_set_child /
 * gtk_frame_set_child / gtk_button_set_child / etc.) and boxes use
 * gtk_box_append instead of gtk_box_pack_start.
 *
 * Rather than rewrite every site by hand to pick the right setter,
 * dispatch on the parent type at runtime in one place. The cost of
 * the GTK_IS_* checks is negligible for UI construction code that
 * runs once at startup.
 */
extern void gtkhx_widget_set_child (GtkWidget *parent, GtkWidget *child);

/*
 * Phase 4.2: gtk_container_remove is gone too — dispatch to the
 * type-specific remove call (gtk_box_remove / gtk_list_box_remove /
 * etc.), or unparent for plain children.
 */
extern void gtkhx_widget_remove_child (GtkWidget *parent, GtkWidget *child);

/*
 * Phase 4.2: gtk_box_pack_start / pack_end are gone too. The old API
 * carried four parameters that GTK 4 splits across the box (which
 * just appends in order) and the child widget (hexpand / vexpand
 * for `expand`, halign / valign for `fill`, margin properties for
 * `padding`). This wrapper applies the child-side properties and
 * appends, mirroring the legacy call shape so the migration is a
 * per-site rename.
 *
 * For gtk_box_pack_end semantics, use gtkhx_box_pack_end which
 * appends and then sets halign/valign to GTK_ALIGN_END so the child
 * is shifted toward the box's trailing edge.
 */
extern void gtkhx_box_pack (GtkWidget *box, GtkWidget *child,
                            gboolean expand, gboolean fill, guint padding);
extern void gtkhx_box_pack_end (GtkWidget *box, GtkWidget *child,
                                gboolean expand, gboolean fill, guint padding);

/*
 * Phase 4.2: gtk_widget_destroy is gone. Toplevels use
 * gtk_window_destroy; non-toplevel widgets get unparented (their
 * parent drops the only ref) or g_object_unrefed if floating. This
 * helper dispatches on type so the legacy gtk_widget_destroy(w)
 * call sites become a per-site rename rather than per-site rewrite.
 */
extern void gtkhx_widget_destroy (GtkWidget *widget);

/*
 * Phase 4.13: gtk_image_new_from_pixbuf is deprecated in GTK 4.12 in
 * favor of gtk_image_new_from_paintable + a GdkTexture built off the
 * pixbuf. This helper centralizes the conversion so the ~50 call
 * sites become a single sed-rename. Returns a fresh-floating
 * GtkImage; the caller takes ownership the same way as the legacy
 * function.
 */
extern GtkWidget *gtkhx_image_new_from_pixbuf (GdkPixbuf *pixbuf);

/*
 * Phase 5: build a GtkButton with a pixel-art XPM icon loaded from a
 * GResource path. The pixbuf is scaled up by an integer factor with
 * nearest-neighbor interpolation before becoming the button's child,
 * which preserves the crisp pixel-art look at modern desktop sizes
 * (16px sources upscaled to 32px work well). Pass scale = 1 for the
 * legacy 1x rendering, scale = 2 for the standard "bigger toolbar
 * button" treatment.
 *
 * If cb is non-NULL it's wired to the button's "clicked" signal with
 * user_data; if cb is NULL the caller is responsible for hooking up
 * the button. The returned button has its tooltip set; pack it into
 * an AdwHeaderBar / GtkBox / etc. as needed.
 */
extern GtkWidget *gtkhx_pixmap_button (const char *resource_name,
                                       const char *tooltip,
                                       int         scale,
                                       GCallback   cb,
                                       gpointer    user_data);

#endif
