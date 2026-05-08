#ifndef HX_PATH_UTIL_H
#define HX_PATH_UTIL_H 1

/*
 * Pure path helpers, decoupled from the dir_char global so the unit
 * tests can compile them without dragging in files.c (and therefore
 * GTK / Adwaita / xtext / the entire UI tree).
 *
 * dirchar_basename in files.c is now a one-line wrapper around
 * path_basename(path, dir_char).
 */

#include <glib.h>

/*
 * Return a pointer into `path` to the last component, where
 * components are separated by `sep`. If there is no `sep` in the
 * string, the whole path IS the basename and we return `path` as-is.
 *
 * The returned pointer is into the input buffer — callers must not
 * free it. NULL input is undefined (as it was for dirchar_basename).
 */
extern char *path_basename (char *path, char sep);

#endif /* HX_PATH_UTIL_H */
