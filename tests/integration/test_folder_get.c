/*
 * tests/integration/test_folder_get.c — download a whole folder
 * tree from mhxd via HTLC_HDR_FILE_GETFOLDER (0xd2).
 *
 * Hotline 1.5's folder-download opcode streams the entire tree
 * over a single HTXF subchannel. The high-level flow:
 *
 *   1. Login on main port.
 *   2. Send HTLC_HDR_FILE_GETFOLDER with HTLC_DATA_FILE_NAME =
 *      "test_folder" (the Dockerfile seeds files/test_folder/
 *      with leaf_a.txt + leaf_b.txt for this exact purpose).
 *   3. Drain the TASK reply; pull HTXF_REF + HTXF_SIZE (+
 *      optionally HTLS_DATA_FILE_NFILES which mhxd doesn't send
 *      but the spec allows).
 *   4. Open a NEW TCP connection to the subchannel port (+1).
 *   5. Send the 16-byte HTXF magic header.
 *   6. Drive the FILE_NEXT state machine: write FILE_NEXT (u16
 *      BE = 3), read 6-byte next_file_info, read pathcount * (3
 *      byte pad+nlen prefix + nlen byte name), and for file
 *      leaves write FILE_SEND (u16 BE = 1), read u32 BE size,
 *      read `size` bytes of FILP-wrapped payload.
 *   7. Substring-search each payload for our seed content.
 *
 * We verify both leaf_a.txt and leaf_b.txt come down with the
 * expected content. mhxd's folder_getpaths only enumerates the
 * immediate children of htxf->path (no recursion), so the seed
 * is intentionally flat — two files at the top level, no nested
 * subfolders. That's enough to prove the FILE_NEXT state machine
 * works end-to-end; deeper trees are a server-side capability,
 * not a client-side one.
 *
 * Fixture dependency: tests/mhxd/Dockerfile seeds
 * files/test_folder/leaf_{a,b}.txt with "folder-leaf-{A,B}\n"
 * content. If the container hasn't been rebuilt after this test
 * landed, mhxd returns a task-error for the GETFOLDER request
 * and the test skips with a pointer to the rebuild command.
 */

#include "config.h"
#include <string.h>
#include <netinet/in.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "protocol.h"
#include "proto_helpers.h"
#include "integration_harness.h"

static guint32
hdr_type (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *)htlc->in.buf;
    return ntohl (h->type);
}

static guint32
hdr_trans (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *)htlc->in.buf;
    return ntohl (h->trans);
}

static guint32
hdr_flag (const struct htlc_conn *htlc)
{
    const struct hl_hdr *h = (const struct hl_hdr *)htlc->in.buf;
    return ntohl (h->flag);
}

/* memmem-like substring search over raw bytes. The FILP wrapper
 * around each file's data fork puts the actual content somewhere
 * after a 133-byte preamble, so we want to find the seed content
 * regardless of where it lands in the streamed buffer. */
static gboolean
bytes_contain (const guint8 *haystack, gsize haystack_len, const char *needle)
{
    gsize needle_len = strlen (needle);
    if (needle_len == 0 || needle_len > haystack_len) {
        return FALSE;
    }
    for (gsize i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp (haystack + i, needle, needle_len) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static void
test_folder_get_round_trip (void)
{
    struct htlc_conn htlc;
    int fd = integration_open_login_or_skip (&htlc, "FolderGet Tier-3", 412);
    if (fd < 0) {
        return;
    }

    /* HTLC_HDR_FILE_GETFOLDER with FILE_NAME = "test_folder",
	 * no DIR (so server resolves it under the server root). */
    const char *fname = "test_folder";
    guint32 our_trans = htlc.trans;
    g_assert_true (integration_send_message (
        fd, &htlc, HTLC_HDR_FILE_GETFOLDER, /*flag=*/0, /*hc=*/1,
        (int)HTLC_DATA_FILE_NAME, (int)strlen (fname), (guint8 *)fname));

    /* Drain to the TASK reply matching our trans. */
    gboolean got_reply = FALSE;
    for (int i = 0; i < 64 && !got_reply; i++) {
        g_assert_true (
            integration_recv_message (fd, &htlc, /*timeout_ms=*/3000));
        if (hdr_type (&htlc) != HTLS_HDR_TASK) {
            continue;
        }
        if (hdr_trans (&htlc) != our_trans) {
            continue;
        }
        got_reply = TRUE;
    }
    g_assert_true (got_reply);

    if (hdr_flag (&htlc) & 1) {
        /* Task-error. The most likely cause on a fresh container
		 * is that the test_folder/ fixture isn't seeded — the
		 * Dockerfile mkdir+printf landed after the running image
		 * was built. Skip with a pointer to the rebuild command
		 * rather than failing the whole test. */
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (&htlc, err, sizeof (err), &err_len)) {
            gchar *msg = g_strdup_printf (
                "GETFOLDER refused: \"%s\". The test_folder/ fixture "
                "is added by tests/mhxd/Dockerfile but only takes "
                "effect on a fresh image. Rebuild with `docker build "
                "-t gtkhx-mhxd tests/mhxd`.",
                err);
            g_test_skip (msg);
            g_free (msg);
        } else {
            g_test_skip ("GETFOLDER refused (no error chunk); rebuild "
                         "the mhxd container with the new Dockerfile.");
        }
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }

    /* Success path — pull HTXF_REF + HTXF_SIZE. NFILES is
	 * mhxd-version-dependent. */
    guint32 xfer_ref = 0, xfer_size = 0, nfiles = 0;
    dh_start (&htlc)
    {
        switch (_type) {
        case HTLS_DATA_HTXF_REF:
            dh_getint (xfer_ref);
            break;
        case HTLS_DATA_HTXF_SIZE:
            dh_getint (xfer_size);
            break;
        case HTLS_DATA_FILE_NFILES:
            dh_getint (nfiles);
            break;
        }
    }
    dh_end ();
    g_assert_cmphex (xfer_ref, !=, 0);
    g_assert_cmpuint (xfer_size, >, 0);
    g_assert_cmpuint (xfer_size, <, 1024 * 1024);
    g_test_message ("got HTXF_REF=0x%08x HTXF_SIZE=%u NFILES=%u",
                    (unsigned)xfer_ref, (unsigned)xfer_size, (unsigned)nfiles);

    /* Open the subchannel. Skip if it's unreachable rather than
	 * failing — port-publish is a separate concern from server
	 * behaviour. */
    int xfd = integration_connect_xfer ();
    if (xfd < 0) {
        g_test_skip ("HTXF subchannel port (5501 by default) is not "
                     "reachable. Make sure your `docker run` publishes "
                     "it: -p 5501:5501.");
        integration_release_htlc (&htlc);
        integration_close (fd);
        return;
    }

    g_assert_true (integration_send_xfer_hdr (xfd, xfer_ref, xfer_size));

    /* Drive the FILE_NEXT loop. The seed has two files (leaf_a /
	 * leaf_b), each at depth 1 (pathcount=1). The server closes
	 * the socket once curfile >= nfiles, so a short read on the
	 * 6-byte next_file_info means clean end-of-stream — that's
	 * how we exit the loop. */
    gboolean found_leaf_a = FALSE, found_leaf_b = FALSE;
    int iterations = 0;
    const int max_iterations = 16; /* safety bound */

    while (iterations < max_iterations) {
        guint16 cmd_n;
        guint8 nfi[6];
        guint16 nfi_type, nfi_pathcount;
        char name[512];
        gsize name_total = 0;
        ssize_t n;

        iterations++;

        /* Send FILE_NEXT (u16 BE = 3). */
        cmd_n = htons (3);
        g_assert_true (integration_send (xfd, &cmd_n, 2));

        /* Read the 6-byte next_file_info. Short read = clean
		 * end-of-stream (server's curfile >= nfiles loop guard
		 * fires and the socket closes). */
        n = read (xfd, nfi, 6);
        if (n != 6) {
            break;
        }
        /* nfi[0..1] = len (advisory; receiver may ignore)
		 * nfi[2..3] = type (1 = folder marker, 0 = file leaf)
		 * nfi[4..5] = pathcount (mhxd always sends 1) */
        nfi_type = ((guint16)nfi[2] << 8) | nfi[3];
        nfi_pathcount = ((guint16)nfi[4] << 8) | nfi[5];
        g_assert_cmpuint (nfi_pathcount, >=, 1);
        g_assert_cmpuint (nfi_pathcount, <, 16); /* sanity bound */

        /* Read pathcount components. Each is 2 bytes pad + 1
		 * byte nlen + nlen bytes name. We join with '/'. */
        for (guint16 i = 0; i < nfi_pathcount; i++) {
            guint8 ph[3];
            guint8 nlen;
            g_assert_true (integration_recv (xfd, ph, 3));
            nlen = ph[2];
            g_assert_true (
                name_total + (name_total ? 1 : 0) + nlen + 1 < sizeof (name));
            if (name_total > 0) {
                name[name_total++] = '/';
            }
            if (nlen) {
                g_assert_true (integration_recv (xfd, &name[name_total], nlen));
                name_total += nlen;
            }
            name[name_total] = 0;
        }

        if (nfi_type == 1) {
            /* Folder marker — no payload. With our flat fixture,
			 * mhxd's folder_getpaths shouldn't emit any of
			 * these, but the code path is defined. */
            g_test_message ("folder marker: %s", name);
            continue;
        }

        /* File leaf — send FILE_SEND, then read u32 size + the
		 * FILP-wrapped payload. */
        cmd_n = htons (1); /* FILE_SEND */
        g_assert_true (integration_send (xfd, &cmd_n, 2));

        guint32 file_size_n;
        g_assert_true (integration_recv (xfd, &file_size_n, 4));
        guint32 file_size = ntohl (file_size_n);
        g_assert_cmpuint (file_size, >, 0);
        g_assert_cmpuint (file_size, <, 64 * 1024);

        guint8 *payload = g_malloc (file_size);
        g_assert_true (integration_recv (xfd, payload, file_size));

        g_test_message ("file leaf: %s (%u bytes)", name, (unsigned)file_size);

        /* Match by name to the seed content we expect. */
        if (strstr (name, "leaf_a")) {
            g_assert_true (bytes_contain (payload, file_size, "folder-leaf-A"));
            found_leaf_a = TRUE;
        } else if (strstr (name, "leaf_b")) {
            g_assert_true (bytes_contain (payload, file_size, "folder-leaf-B"));
            found_leaf_b = TRUE;
        }

        g_free (payload);
    }

    g_assert_true (found_leaf_a);
    g_assert_true (found_leaf_b);

    integration_close (xfd);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/folder_get/round_trip",
                     test_folder_get_round_trip);

    return g_test_run ();
}
