#ifndef GTKHX_MACRES_H
#define GTKHX_MACRES_H

#include <unistd.h>
#include <glib.h>
#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

/*
 * Macintosh resource-fork reader. The parser lives in the Rust `hxmacres`
 * crate (port of the old macres.c); this header is its C ABI. GtkHx uses it to
 * pull `cicn` colour-icon resources out of the bundled icons.rsrc files.
 */

/* Opaque parsed-resource-file handle (a Rust ResourceFork). */
typedef struct macres_file macres_file;

/*
 * A single resource. `data` (`datalen` bytes) is what consumers read; the
 * buffer is owned by the macres_file and stays valid until macres_file_delete.
 * The wrapper itself is g_malloc'd, so callers g_free it (as options.c does).
 * name / namelen are always NULL / 0.
 */
struct macres_res {
	guint32 datalen;
	guint16 resid;
	guint16 namelen;
	guint8 *name;
	void *data;
};

typedef struct macres_res macres_res;

/* Read + parse the resource fork at `path`. NULL if the file can't be read or
 * isn't a valid resource fork. (Takes a path rather than an fd so the
 * implementation stays portable — no raw-fd handling.) */
extern macres_file *macres_file_open (const char *path);
extern void macres_file_delete (macres_file *mrf);

extern guint32 macres_file_num_res_of_type (macres_file *mrf, guint32 type);
/* Both lookups return a g_malloc'd macres_res (caller g_free's) or NULL. */
extern macres_res *macres_file_get_nth_res_of_type (macres_file *mrf,
                                                    guint32 type, guint32 n);
extern macres_res *macres_file_get_resid_of_type (macres_file *mrf,
                                                  guint32 type, gint16 resid);

#endif /* GTKHX_MACRES_H */
