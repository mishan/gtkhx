/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
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
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * usermod.c — the access-bit name table. The User Editor is Rust
 * (gtkhx-ui crate, useredit.rs), and reads the table through the
 * gtkhx_useredit_access_* accessors. The byte-order-dependent bit
 * numbering (the ENTRY macro) deliberately stays in C.
 */

#include "config.h"
#include <glib.h>
#include "usermod.h"

/* Access-bit name table. Sentinels (bitno == -1) are section headers.
 * The ENTRY macro maps a spec bit index to its position in the 64-bit
 * hl_access_bits big-endian layout; keeping it (and its byte-order
 * dependence) in C is deliberate — the Rust User Editor reads this table
 * through the gtkhx_useredit_access_* accessors below. */
struct access_name {
    char bitno;
    char *name;
} access_names[] = {
#define ENTRY(x, y)                                                            \
    { ((x) != -1) ? (63                                                        \
                     - ((G_BYTE_ORDER == G_BIG_ENDIAN)                         \
                            ? (x)                                              \
                            : ((x) % 8) + 8 * (7 - (x) / 8)))                  \
                  : -1,                                                        \
      (y) }
    ENTRY (-1, "File Privileges"),
    ENTRY (1, "Can Upload Files"),
    ENTRY (2, "Can Download Files"),
    ENTRY (4, "Can Move Files"),
    ENTRY (8, "Can Move Folders"),
    ENTRY (5, "Can Create Folders"),
    ENTRY (0, "Can Delete Files"),
    ENTRY (6, "Can Delete Folders"),
    ENTRY (3, "Can Rename Files"),
    ENTRY (7, "Can Rename Folders"),
    ENTRY (28, "Can Comment Files"),
    ENTRY (29, "Can Comment Folders"),
    ENTRY (31, "Can Make Aliases"),
    ENTRY (25, "Can Upload Anywhere"),
    ENTRY (30, "Can View Drop Boxes"),
    ENTRY (-1, "Chat Privileges"),
    ENTRY (9, "Can Read Chat"),
    ENTRY (10, "Can Send Chat"),
    ENTRY (-1, "News"),
    ENTRY (20, "Can Read News"),
    ENTRY (21, "Can Post News"),
    ENTRY (-1, "User Privileges"),
    ENTRY (14, "Can Create Users"),
    ENTRY (15, "Can Delete Users"),
    ENTRY (16, "Can Read Users"),
    ENTRY (17, "Can Modify Users"),
    ENTRY (22, "Can Disconnect Users"),
    ENTRY (23, "Cannot Be Disconnected"),
    ENTRY (24, "Can Get User Info"),
    ENTRY (26, "Can Use Any Name"),
    ENTRY (27, "Cannot Be Shown Agreement"),
    ENTRY (-1, "Admin Privileges"),
    ENTRY (32, "Can Broadcast"),
#undef ENTRY
};

/* Accessors for the Rust User Editor (useredit.rs). */
int
gtkhx_useredit_access_count (void)
{
    return (int)G_N_ELEMENTS (access_names);
}

const char *
gtkhx_useredit_access_name (int i)
{
    if (i < 0 || i >= (int)G_N_ELEMENTS (access_names)) {
        return NULL;
    }
    return access_names[i].name;
}

int
gtkhx_useredit_access_bitno (int i)
{
    if (i < 0 || i >= (int)G_N_ELEMENTS (access_names)) {
        return -1;
    }
    return access_names[i].bitno;
}
