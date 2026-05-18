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
 * path_to_hldir + dirmask — pure path-encoding helpers, extracted
 * from files.c so Tier 1 unit tests can exercise them without
 * dragging in GTK / Adwaita / the entire file browser pile.
 *
 * path_to_hldir packs a slash-separated (or whatever dir_char is set
 * to) path into the Hotline DIR-chunk wire format: a u16 component
 * count followed by N records of {u16 zero, u8 namelen, name bytes}.
 * Every file-name-bearing opcode uses this encoding for the directory
 * portion of the target.
 *
 * The dir_char separator is set by the server during the handshake
 * (HTLC_HDR_DIRECTORYCHAR / dirchar_change in rcv.c); we read it as
 * an extern. The unit test provides its own definition.
 *
 * dirmask strips a leading prefix from src using mask as the
 * candidate match, then copies the unmatched tail to dst. Used by
 * rcv.c::rcv_task_folder_get to peel the request-time path prefix
 * off the server-sent per-entry path.
 */

#include "config.h"
#include <stddef.h>
#include <string.h>
#include <glib.h>
#include <arpa/inet.h>          /* htons */
#include "compat.h"             /* PACKED */
#include "path_hldir.h"

extern guint8 dir_char;

/* Wire layout for a single component header. The "enc" field is the
 * Hotline "encoding" tag — always zero in practice and the receiver
 * doesn't look at it. Kept as a separate field so the structure
 * lines up exactly with the spec / proto trace dumps. */
struct x_fhdr {
    guint16 enc PACKED;
    guint8 len, name[1];
};

guint8 *
path_to_hldir (const char *path, guint16 *hldirlen, int is_file)
{
    guint8 *hldir;
    struct x_fhdr *fh;
    char const *p, *p2;
    guint16 pos = 2, dc = 0;
    guint8 nlen;

    hldir = g_malloc (2);
    p = path;
    while ((p2 = strchr (p, dir_char))) {
        if (!(p2 - p)) {
            p++;
            continue;
        }
        nlen = (guint8)(p2 - p);
        pos += 3 + nlen;
        hldir = g_realloc (hldir, pos);
        fh = (struct x_fhdr *)(&(hldir[pos - (3 + nlen)]));
        memset (&fh->enc, 0, 2);
        fh->len = nlen;
        memcpy (fh->name, p, nlen);
        dc++;
        p = p2 + 1;
    }
    if (!is_file && *p) {
        nlen = (guint8)strlen (p);
        pos += 3 + nlen;
        hldir = g_realloc (hldir, pos);
        fh = (struct x_fhdr *)(&(hldir[pos - (3 + nlen)]));
        memset (&fh->enc, 0, 2);
        fh->len = nlen;
        memcpy (fh->name, p, nlen);
        dc++;
    }
    *((guint16 *)hldir) = htons (dc);

    *hldirlen = pos;
    return hldir;
}

void
dirmask (char *dst, char *src, char *mask)
{
    while (*mask && *src && *mask++ == *src++)
        ;

    strcpy (dst, src);
}
