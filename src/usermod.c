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
 * usermod.c — protocol send-path for account create / delete / read, plus
 * the access-bit name table. The User Editor UI itself is ported to Rust
 * (gtkhx-ui crate, useredit.rs); it calls the wire senders below and reads
 * the access table via the gtkhx_useredit_access_* accessors. The
 * byte-order-dependent bit numbering (the ENTRY macro) deliberately stays
 * in C.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "hx.h"
#include "hotline_proto.h"
#include "network.h"
#include "proto_helpers.h" /* struct hx_chunk (stack-allocated below) */
#include "tasks.h"
#include "rcv.h"
#include "usermod.h"

void
hx_useredit_create (struct htlc_conn *htlc, const char *login, const char *pass,
                    const char *name, hl_access_bits access)
{
    char elogin[32], epass[32];
    guint16 llen, plen;

    llen = strlen (login);
    hl_encode (elogin, login, llen);
    /* Empty-password convention: a single 0x00 byte (NOT a zero-length
     * field). The Rust builder accepts the byte buffer as-is. */
    if (!*pass) {
        plen = 1;
        epass[0] = 0;
    } else {
        plen = strlen (pass);
        hl_encode (epass, pass, plen);
    }

    /* chunk layout moved to gtkhx_proto_build_account_modify
     * _chunks. Build BEFORE task_new — task_new snapshots htlc->trans
     * into a pending entry; a builder failure must not leave a phantom
     * "user create" task in the task table. */
    struct hx_chunk chunks[4];
    guint8 scratch[8];
    int hc = (int)gtkhx_proto_build_account_modify_chunks (
        (const uint8_t *)elogin, llen, (const uint8_t *)epass, plen,
        (const uint8_t *)name, strlen (name), (const uint8_t *)&access, chunks,
        G_N_ELEMENTS (chunks), scratch, sizeof (scratch));
    if (hc > 0) {
        task_new (htlc, 0, 0, 0, "user create");
        hlwrite_chunks (htlc, HTLC_HDR_ACCOUNT_MODIFY, 0, chunks, hc);
    }
}

void
hx_useredit_delete (struct htlc_conn *htlc, const char *login)
{
    char elogin[32];
    guint16 llen;

    llen = strlen (login);
    hl_encode (elogin, login, llen);

    /* chunk layout moved to gtkhx_proto_build_account_delete
     * _chunks. Same build-before-task ordering as hx_useredit_create. */
    struct hx_chunk chunks[1];
    int hc = (int)gtkhx_proto_build_account_delete_chunks (
        (const uint8_t *)elogin, llen, chunks, G_N_ELEMENTS (chunks));
    if (hc > 0) {
        task_new (htlc, 0, 0, 0, "user delete");
        hlwrite_chunks (htlc, HTLC_HDR_ACCOUNT_DELETE, 0, chunks, hc);
    }
}

void
hx_useredit_open (struct htlc_conn *htlc, const char *login,
                  void (*fn) (void *, const char *, const char *, const char *,
                              const hl_access_bits),
                  void *uesp)
{
    /* chunk layout moved to gtkhx_proto_build_account_read
     * _chunks. Note the C call site passes login UNENCODED (a
     * deliberate mhxd convention — READ takes a raw login, MODIFY /
     * DELETE take an hl_encoded one). */
    struct hx_chunk chunks[1];
    int hc = (int)gtkhx_proto_build_account_read_chunks (
        (const uint8_t *)login, strlen (login), chunks, G_N_ELEMENTS (chunks));
    if (hc > 0) {
        struct uesp_fn *uespfn = g_malloc (sizeof (struct uesp_fn));
        uespfn->uesp = uesp;
        uespfn->fn = fn;
        task_new (htlc, RCV_TASK_FN (rcv_task_user_open), uespfn, 0,
                  "user open");
        hlwrite_chunks (htlc, HTLC_HDR_ACCOUNT_READ, 0, chunks, hc);
    }
}

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
