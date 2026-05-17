/*
 * filelist_walker — walk a packed run of HTLS_DATA_FILE_LIST
 * chunks (struct hl_filelist_hdr back-to-back) and fire a callback
 * per entry with the decoded fields. See filelist_walker.c for the
 * wire shape and the Tier 2 unit test in
 * tests/proto/test_filelist_walker.c for the pinned-behavior
 * contract.
 */

#ifndef HX_FILELIST_WALKER_H
#define HX_FILELIST_WALKER_H 1

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called once per filelist entry. ftype / fsize / fnlen are
 * decoded to host order; `name` is a borrowed pointer into the
 * walker's input buffer (NOT NUL-terminated). The pointer is only
 * valid for the duration of the callback. */
typedef void (*hl_filelist_entry_cb) (guint32 ftype, guint32 fsize,
                                      const guint8 *name, gsize name_len,
                                      void *user_data);

extern void hl_filelist_walk (const void *buf, gsize buflen,
                              hl_filelist_entry_cb cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* HX_FILELIST_WALKER_H */
