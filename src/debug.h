#ifndef HX_DEBUG_H
#define HX_DEBUG_H 1

/*
 * Category-based runtime debug logging for GtkHx.
 *
 * Enabled at startup from the GTKHX_DEBUG environment variable, which
 * takes a comma-separated list of category names:
 *
 *     GTKHX_DEBUG=proto                # protocol trace only
 *     GTKHX_DEBUG=proto,xfer,sound     # several at once
 *     GTKHX_DEBUG=all                  # everything
 *
 * Categories are arbitrary tags passed to debug_log(). The category
 * "all" turns every category on regardless of name. Empty / unset env
 * leaves all categories off.
 *
 * Output goes to stderr, prefixed with "[<category>]" and ending with
 * a newline if the format string didn't include one. Calls are cheap
 * when the category is disabled (single hash-lookup-then-bail).
 *
 * Threading: debug_init must run on the main thread before any worker
 * threads start; after that debug_category_enabled / debug_log are
 * thread-safe (they only read the GHashTable, never mutate it).
 */

#include <glib.h>

extern void debug_init (void);
extern gboolean debug_category_enabled (const char *cat);

extern void debug_log (const char *cat, const char *fmt, ...)
    G_GNUC_PRINTF (2, 3);

/* Phase 5: tracing helper for htlc->name corruption hunt. Logs every
 * htlc->name write under category 'name' with the call-site label,
 * byte count, and hex dump of the source bytes. Cheap when the
 * category is off; spammy but exhaustive when on. */
extern void debug_log_name_write (const char *label, const char *src,
                                  gsize srclen);

#endif /* HX_DEBUG_H */
