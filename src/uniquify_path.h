/*
 * uniquify_path — collision-resolving "foo.txt" → "foo (1).txt"
 * helper. Caller-supplied "exists" predicate so the Tier 1 test
 * can drive it without touching the filesystem. See uniquify_path.c
 * for the behavior contract.
 */

#ifndef HX_UNIQUIFY_PATH_H
#define HX_UNIQUIFY_PATH_H 1

#include <stddef.h>
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns non-zero when path is already taken. The production
 * caller in xfers.c passes a wrapper around stat() + Apple Double
 * resource fork detection; the unit test passes a synthetic
 * predicate over a GHashTable of seeded paths. */
typedef int (*uniquify_exists_fn) (const char *path, void *user_data);

/* Mutate `path` in-place so that exists(path, ud) returns 0. If
 * the original path is already free, leaves it untouched. */
extern void uniquify_path (char *path, size_t cap, uniquify_exists_fn exists,
                           void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* HX_UNIQUIFY_PATH_H */
