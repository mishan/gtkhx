#ifndef GTKHX_CICN_H
#define GTKHX_CICN_H

#include <gtk/gtk.h>

/* OSType 'cicn' — the Mac colour-icon resource. */
#define TYPE_cicn 0x6369636e

struct ifn;

/*
 * The cicn decoder now lives in the Rust `hxcicn` crate: it walks the resource
 * and produces a packed RGBA buffer (the Mac icon mask folds into the alpha
 * channel). cicn.c is the thin GdkPixbuf wrapper around it.
 *
 * cicn_to_pixbuf decodes one cicn resource to an owned GdkPixbuf (NULL on
 * malformed input). load_icon looks an icon up by Mac resource id across the
 * loaded resource files and hands back ownership via *pixbuf_out; the mask
 * out-param is vestigial (alpha lives in the pixbuf) and always set to NULL.
 */
extern GdkPixbuf *cicn_to_pixbuf (void *cicn_rsrc, unsigned int len);
void load_icon (GtkWidget *widget, guint16 icon, struct ifn *ifn, char recurse,
                GdkPixbuf **pixbuf_out, GdkPixbuf **mask_unused);

#endif /* GTKHX_CICN_H */
