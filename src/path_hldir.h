/*
 * path_to_hldir — the Hotline DIR-chunk path encoder. Implemented in Rust
 * (hxrequest::path, whose tests pin the wire layout); this is its C ABI.
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
#ifdef __cplusplus
}
#endif

#endif /* HX_PATH_HLDIR_H */
