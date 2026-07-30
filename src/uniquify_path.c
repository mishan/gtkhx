/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 */

/*
 * uniquify_path — pick a non-colliding download target path by
 * inserting " (N)" before the last extension when the requested
 * path already exists. Extracted from xfers.c with the
 * filesystem-touching local_path_exists check replaced by a
 * caller-supplied predicate, so the Tier 1 unit test can drive
 * it with a synthetic "which paths exist" callback.
 *
 * The production wrapper in xfers.c passes a predicate that calls
 * stat() and checks for an Apple Double resource fork (HFS+
 * stragglers on Mac-formatted disks).
 *
 * Behavior pinned by the unit test:
 *
 *   /dl/foo.txt        + foo.txt present     →  /dl/foo (1).txt
 *   /dl/archive.tar.gz + that present        →  /dl/archive.tar (1).gz
 *   /dl/README         + that present        →  /dl/README (1)
 *   /dl/.bashrc        + that present        →  /dl/.bashrc (1)
 *
 * The "no extension" rule is "no dot before the last slash"; a
 * leading-dot basename (".bashrc") counts as having no extension.
 * Counting starts at N=1 and continues up to 10000, after which
 * the function gives up and leaves the path at its last attempt.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include "compat.h" /* MAXPATHLEN */
#include "uniquify_path.h"

void
uniquify_path (char *path, size_t cap, uniquify_exists_fn exists, void *ud)
{
    const char *base, *dot;
    char prefix[MAXPATHLEN];
    char suffix[MAXPATHLEN];
    size_t pre_len;
    int n;

    if (!exists (path, ud)) {
        return;
    }

    base = strrchr (path, '/');
    base = base ? base + 1 : path;
    dot = strrchr (base, '.');
    if (dot == base) { /* leading-dot basename, no extension */
        dot = NULL;
    }

    if (dot) {
        pre_len = dot - path;
        if (pre_len >= sizeof prefix) {
            pre_len = sizeof prefix - 1;
        }
        memcpy (prefix, path, pre_len);
        prefix[pre_len] = '\0';
        g_strlcpy (suffix, dot, sizeof suffix);
    } else {
        g_strlcpy (prefix, path, sizeof prefix);
        suffix[0] = '\0';
    }

    for (n = 1; n < 10000; n++) {
        /* The precision specifiers cap each component at slightly under
         * half of MAXPATHLEN so GCC can prove the format fits in path's
         * cap bytes. snprintf would truncate safely either way; the
         * specifiers exist only to satisfy the static analysis. */
        snprintf (path, cap, "%.*s (%d)%.*s", MAXPATHLEN / 2 - 16, prefix, n,
                  MAXPATHLEN / 2 - 16, suffix);
        if (!exists (path, ud)) {
            return;
        }
    }
}
