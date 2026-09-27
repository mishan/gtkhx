/*
 * path_hldir — the Hotline DIR-chunk path encoder, in its own
 * translation unit so the Tier 1 test can link it alone.
 *
 * See path_hldir.c for the wire layout.
 */

#ifndef HX_PATH_HLDIR_H
#define HX_PATH_HLDIR_H 1

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encode `path` as a Hotline DIR chunk. Returns a freshly malloc'd
 * buffer of *hldirlen bytes the caller must g_free. is_file = 1
 * stops one component short — used when the directory portion of a
 * "dir/name" target is what's wanted, with `name` shipped as a
 * separate FILE_NAME chunk. */
extern guint8 *path_to_hldir (const char *path, guint16 *hldirlen, int is_file);

/* Strip the leading common prefix between src and mask, then copy
 * the unmatched tail of src into dst. */
#ifdef __cplusplus
}
#endif

#endif /* HX_PATH_HLDIR_H */
