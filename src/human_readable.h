/*
 * human_readable / human_size — fileutils-vintage byte-count
 * formatter ("8.3k", "127M", "53G"). Extracted to its own
 * translation unit so the Tier 1 test can link it without dragging
 * in files.c's GTK + Adwaita pile.
 *
 * Output buffers must be at least LONGEST_HUMAN_READABLE + 1 bytes;
 * the writer fills from the right and returns a pointer into the
 * buffer rather than to its base, so the *return value* (not the
 * buf argument) is the string to display.
 */

#ifndef HX_HUMAN_READABLE_H
#define HX_HUMAN_READABLE_H 1

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LONGEST_HUMAN_READABLE 32

extern const char human_suffixes[];

/* General form. output_block_size < 0 selects abbreviation with
 * powers of |output_block_size| (typically -1024 for binary
 * suffixes). output_block_size > 0 uses raw decimal in those
 * units.
 *
 * `n` is guint64 to support the Large-File extension; the
 * formatter walks the size into binary suffixes (k / M / G / T
 * / …) so the result remains compact even for >4 GiB values. */
extern char *human_readable (guint64 n, char *buf, int from_block_size,
                             int output_block_size);

/* The default GtkHx convention: binary suffixes, from-block 1, so
 * `size` is interpreted as raw bytes. Equivalent to
 * human_readable (size, sizstr, 1, -1024). */
extern char *human_size (char *sizstr, guint64 size);

#ifdef __cplusplus
}
#endif

#endif /* HX_HUMAN_READABLE_H */
