/*
 * files_complete.h — path completion for a local path entry: a popover of
 * the matching subdirectories as the user types. Implemented in Rust
 * (gtkhx-ui's files::complete); hx_path_complete_free is safe after the
 * entry has been destroyed.
 */

#ifndef HX_FILES_COMPLETE_H
#define HX_FILES_COMPLETE_H 1

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _hx_path_complete hx_path_complete;

extern hx_path_complete *hx_path_complete_attach (GtkEntry *entry);
extern void hx_path_complete_free (hx_path_complete *c);

G_END_DECLS

#endif /* HX_FILES_COMPLETE_H */
