/*
 * tests/integration/test_user_account.c — Tier 3 coverage for the
 * Hotline account-management opcodes (HTLC_HDR_ACCOUNT_MODIFY,
 * HTLC_HDR_ACCOUNT_READ, HTLC_HDR_ACCOUNT_DELETE) used by the
 * GtkHx "New User" / "Edit User" / Delete flows in src/usermod.c.
 *
 * Why this exists: the User Editor dialog had a long-standing bug
 * where toggling any access-bit switch failed to update the in-
 * memory bitmap (wrong callback signature for notify::active),
 * so every account created from the dialog was persisted with an
 * all-zero access mask. Users observed it as "the account I just
 * created can't log in / can't do anything", which is
 * indistinguishable from "the account wasn't created" without
 * reading mhxd's logs.
 *
 * The fix is in src/usermod.c (useredit_chk_activate gained its
 * missing GParamSpec parameter); this test pins the wire-format
 * contract those callers depend on so a future regression in
 * either the dialog OR in the chunk builders surfaces here.
 *
 * Server requirement: this test needs an admin-like account
 * (HTLC_DATA_LOGIN="admin", no password), which is mhxd's default
 * (tests/mhxd/Dockerfile lays down upstream's run/hxd/ skeleton
 * that ships an admin account with the full access bitmap). The
 * test logs in as admin, exercises the full create / read /
 * modify / delete lifecycle against a freshly-named account,
 * and cleans up after itself.
 *
 * Subtests:
 *
 *   /integration/user_account/create_and_read — log in as admin,
 *     create a new account with a specific access bitmap, read it
 *     back, verify the NAME / LOGIN / ACCESS chunks round-trip
 *     unchanged and the PASSWORD chunk is present in the reply.
 *     (PASSWORD bytes are not compared: mhxd's rcv_account_read
 *     deliberately replies with a single 0x00 sentinel byte rather
 *     than leaking the stored password — the test only pins that
 *     the chunk exists in the reply, not its contents.)
 *
 *   /integration/user_account/modify_and_read — establish a known
 *     pre-state (create), modify the same account (new name, new
 *     access), read back, verify the new fields replaced the old.
 *
 *   /integration/user_account/delete — create an account, delete
 *     it, attempt to read it back, verify the server returns a
 *     task-error.
 *
 *   /integration/user_account/access_bits_roundtrip — focused
 *     regression net for the dialog bug: send an access bitmap with
 *     specific bits set, read it back, verify the same bits are set
 *     on the server side. An all-zero ACCESS chunk used to be the
 *     symptom of the dialog bug (the per-bit toggles silently
 *     dropped); this subtest creates an account with a non-trivial
 *     bitmap and asserts the wire bytes match.
 *
 * Each subtest is independent: each one opens its own connection,
 * each one creates whatever pre-state it needs (so subtests pass
 * cleanly when run in isolation via `--p /integration/user_account/X`),
 * and each one cleans up its synthesized account at the end. The
 * synthesized login names carry a per-process random suffix so
 * concurrent meson runs and reruns in the same container don't
 * collide on the server's accounts/ directory.
 */

#include "config.h"
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include "compat.h"
#include "hotline.h"
#include "hotline_proto.h" /* gtkhx_proto_build_account_* — production builders */
#include "protocol.h"
#include "proto_helpers.h"
#include "hl_code.h"
#include "hxconn.h" /* hx_conn_trans_post_inc for the LOGIN frame's trans */

/* hxnet/src/ffi.rs — build a plaintext LOGIN frame via the production Rust
 * builder (was src/login_packet.c, now retired; production login is Rust). */
extern size_t hxnet_build_login_frame (const guint8 *login, size_t login_len,
                                       const guint8 *password,
                                       size_t password_len, const guint8 *name,
                                       size_t name_len, guint16 icon,
                                       guint16 version, guint16 caps,
                                       guint32 trans, guint8 *out,
                                       size_t out_cap);
#include "hl_access.h"
#include "integration_harness.h"

/* ---- Admin login inline -------------------------------------------- */

/* Log in as "admin" with no password. Mirrors integration_login_guest
 * but with a different account name. mhxd's upstream skeleton ships
 * an `admin` account with no password and a fully-set access bitmap;
 * we rely on that. */
static gboolean
admin_login (int fd, struct htlc_conn *htlc, const char *display_name,
             guint16 icon)
{

    const char *dn = display_name ? display_name : "";
    guint8 frame[512];
    size_t flen = hxnet_build_login_frame (
        (const guint8 *) "admin", 5, NULL, 0, (const guint8 *) dn, strlen (dn),
        icon, /*version=*/185, /*caps=*/0, hx_conn_trans_post_inc (htlc), frame,
        sizeof (frame));
    if (flen == 0) {
        return FALSE;
    }
    return integration_send (fd, frame, flen);
}

static int
open_admin_login_or_fail (struct htlc_conn *htlc, const char *display_name)
{
    memset (htlc, 0, sizeof (*htlc));
    int fd = integration_open_or_skip ();
    if (fd < 0) {
        return -1;
    }

    if (!admin_login (fd, htlc, display_name, 412)) {
        g_test_fail_printf ("admin_login send failed");
        integration_release_htlc (htlc);
        integration_close (fd);
        return -1;
    }

    guint32 type = integration_drain_until_selfinfo_or_error (fd, htlc, 8);
    if (type == HTLS_HDR_TASK) {
        char err[256];
        gsize err_len = 0;
        if (task_error_extract (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos, err, sizeof (err), &err_len)) {
            g_test_fail_printf (
                "server rejected admin login: \"%s\". Check the test server's "
                "accounts/ for an `admin` account with no password.",
                err);
        } else {
            g_test_fail_printf ("server rejected admin login (no error chunk).");
        }
        integration_release_htlc (htlc);
        integration_close (fd);
        return -1;
    }
    if (type != HTLS_HDR_USER_SELFINFO) {
        g_test_fail_printf ("timed out waiting for SELFINFO after admin login");
        integration_release_htlc (htlc);
        integration_close (fd);
        return -1;
    }

    hx_selfinfo_parse (htlc, hx_test_in(htlc)->buf, hx_test_in(htlc)->pos);

    /* Sanity: admin needs at least the user-management bits, otherwise
     * mhxd silently drops the ACCOUNT_* dispatches and our trans-
     * matching drain spins waiting for a TASK reply that never comes.
     *
     * htlc->access is a guint64 holding the on-wire 8 raw bytes in
     * memory order (big-endian); hl_access_has takes a byte pointer
     * so cast through that. */
    const guint8 *acc_bytes = (const guint8 *) &htlc->access;
    if (!hl_access_has (acc_bytes, HL_ACCESS_CREATE_USERS)
        || !hl_access_has (acc_bytes, HL_ACCESS_MODIFY_USERS)
        || !hl_access_has (acc_bytes, HL_ACCESS_DELETE_USERS)
        || !hl_access_has (acc_bytes, HL_ACCESS_READ_USERS)) {
        g_test_fail_printf (
            "admin account is missing one of CREATE/MODIFY/DELETE/READ "
            "user bits — the server's accounts/admin/UserData needs a "
            "full access bitmap for these tests to run.");
        integration_release_htlc (htlc);
        integration_close (fd);
        return -1;
    }
    return fd;
}

/* ---- Chunk-walker helpers ------------------------------------------ */

/* Walk an ACCOUNT_READ TASK reply and report which of the four
 * expected chunks were present. Optionally extracts the human-
 * readable fields into caller-provided buffers:
 *
 *   name_out / name_cap     — HTLS_DATA_NAME bytes, NUL-terminated.
 *                             May be NULL (cap then ignored) when the
 *                             caller only cares about presence.
 *   login_out / login_cap   — HTLS_DATA_LOGIN bytes after hl_code
 *                             decode (mhxd ships the login back
 *                             XOR-obfuscated). NUL-terminated. May
 *                             be NULL.
 *   access_out              — HTLS_DATA_ACCESS bytes (exactly 8;
 *                             see below). May be NULL. The on-wire
 *                             access bitmap is big-endian-in-memory.
 *
 * HTLS_DATA_PASSWORD is reported as present-or-absent only — mhxd's
 * rcv_account_read deliberately replies with a single 0x00 sentinel
 * byte rather than leaking stored password bytes, so there's
 * nothing for the test to compare against.
 *
 * ACCESS is pinned to exactly 8 bytes per the protocol — the
 * GOT_ACCESS flag is only set when _len == 8. A future server (or
 * a future client-side regression in the chunk builder) that
 * shipped a shorter or longer ACCESS chunk wouldn't pass the
 * presence check and the test would fail on the missing flag
 * rather than silently accepting a truncated bitmap.
 *
 * Returns the bitwise OR of GOT_NAME / GOT_LOGIN / GOT_PASSWORD /
 * GOT_ACCESS — tests assert against the expected mask. */
enum {
    GOT_NAME = 1u << 0,
    GOT_LOGIN = 1u << 1,
    GOT_PASSWORD = 1u << 2,
    GOT_ACCESS = 1u << 3,
};

static unsigned
read_account_reply (struct htlc_conn *htlc, char *name_out, gsize name_cap,
                    char *login_out, gsize login_cap, guint8 *access_out)
{
    unsigned seen = 0;
    dh_start (hx_test_in(htlc)->buf, hx_test_in(htlc)->pos)
    {
        switch (_type) {
        case HTLS_DATA_NAME:
            if (name_out && name_cap > 0) {
                gsize copy = _len > name_cap - 1 ? name_cap - 1 : _len;
                memcpy (name_out, dh->data, copy);
                name_out[copy] = 0;
            }
            seen |= GOT_NAME;
            break;
        case HTLS_DATA_LOGIN:
            /* mhxd hl_encodes the login before sending it back, so we
             * have to decode (hl_code is its own inverse). */
            if (login_out && login_cap > 0) {
                gsize copy = _len > login_cap - 1 ? login_cap - 1 : _len;
                hl_code (login_out, dh->data, copy);
                login_out[copy] = 0;
            }
            seen |= GOT_LOGIN;
            break;
        case HTLS_DATA_PASSWORD:
            seen |= GOT_PASSWORD;
            break;
        case HTLS_DATA_ACCESS:
            /* ACCESS is exactly 8 bytes per the protocol — a
             * shorter or longer chunk would be a wire-format
             * regression worth failing the test for, not silently
             * truncating to 8. */
            if (_len == 8) {
                if (access_out) {
                    memcpy (access_out, dh->data, 8);
                }
                seen |= GOT_ACCESS;
            }
            break;
        }
    }
    dh_end ();
    return seen;
}

/* Pack a chunk array via the shared hlpack_chunks helper and ship it
 * down `fd`. Same shape as src/network.c::hlwrite_chunks's body, minus
 * the production cipher / compress / proto-trace plumbing the harness
 * doesn't carry. Used by the ACCOUNT_* senders below so they route
 * through the actual production wire-format builders rather than
 * open-coding their own chunk arrays — the whole point of these tests
 * is to pin the wire-format contract the dialog depends on. */
static gboolean
send_chunks (int fd, struct htlc_conn *htlc, guint32 type,
             const struct hx_chunk *chunks, int hc)
{
    gsize len = 0;
    guint8 *buf = hlpack_chunks (htlc, type, /*flag=*/0, chunks, hc, &len);

    if (!buf) {
        return FALSE;
    }
    gboolean ok = integration_send (fd, buf, len);
    g_free (buf);
    return ok;
}

/* Send HTLC_HDR_ACCOUNT_MODIFY via the same Rust builder src/usermod.c
 * uses (gtkhx_proto_build_account_modify_chunks). Returns the trans
 * id the caller can drain against, or 0 on send failure.
 *
 * Mirrors src/usermod.c::hx_useredit_create's caller-side hl_encode
 * of login + password (the builder takes already-encoded buffers). */
static guint32
send_account_modify (int fd, struct htlc_conn *htlc, const char *login,
                     const char *password, const char *name,
                     const guint8 access[8])
{
    char elogin[64], epass[64];
    int llen = (int)strlen (login);
    int plen;
    if (llen > 63) llen = 63;
    hl_code (elogin, login, llen);

    if (!password || !*password) {
        plen = 1;
        epass[0] = 0;
    } else {
        plen = (int)strlen (password);
        if (plen > 63) plen = 63;
        hl_code (epass, password, plen);
    }

    struct hx_chunk chunks[4];
    guint8 scratch[8];
    int hc = (int)gtkhx_proto_build_account_modify_chunks (
        (const uint8_t *)elogin, llen, (const uint8_t *)epass, plen,
        (const uint8_t *)name, strlen (name), access,
        chunks, G_N_ELEMENTS (chunks), scratch, sizeof (scratch));
    if (hc <= 0) {
        return 0;
    }

    guint32 trans = htlc->trans;
    return send_chunks (fd, htlc, HTLC_HDR_ACCOUNT_MODIFY, chunks, hc)
               ? trans
               : 0;
}

static guint32
send_account_read (int fd, struct htlc_conn *htlc, const char *login)
{
    /* The READ opcode takes a plain (unencoded) login per the mhxd
     * convention — see the comment in src/usermod.c::hx_useredit_open. */
    struct hx_chunk chunks[1];
    int hc = (int)gtkhx_proto_build_account_read_chunks (
        (const uint8_t *)login, strlen (login), chunks, G_N_ELEMENTS (chunks));
    if (hc <= 0) {
        return 0;
    }

    guint32 trans = htlc->trans;
    return send_chunks (fd, htlc, HTLC_HDR_ACCOUNT_READ, chunks, hc) ? trans
                                                                    : 0;
}

static guint32
send_account_delete (int fd, struct htlc_conn *htlc, const char *login)
{
    char elogin[64];
    int llen = (int)strlen (login);
    if (llen > 63) llen = 63;
    hl_code (elogin, login, llen);

    struct hx_chunk chunks[1];
    int hc = (int)gtkhx_proto_build_account_delete_chunks (
        (const uint8_t *)elogin, llen, chunks, G_N_ELEMENTS (chunks));
    if (hc <= 0) {
        return 0;
    }

    guint32 trans = htlc->trans;
    return send_chunks (fd, htlc, HTLC_HDR_ACCOUNT_DELETE, chunks, hc)
               ? trans
               : 0;
}

/* ---- Fixture ------------------------------------------------------- */

/* Per-process random suffix shared across subtests so concurrent
 * meson runs against the same container land on distinct accounts.
 * Each subtest derives its own login name from this seed plus a
 * per-subtest tag (so the create/modify/delete subtests don't trip
 * over each other even when run interleaved with --repeat). */
static guint32 g_random_seed;

static void
init_test_state (void)
{
    if (g_random_seed) {
        return;
    }
    GRand *rnd = g_rand_new ();
    g_random_seed = g_rand_int (rnd);
    g_rand_free (rnd);
}

/* Compose a per-subtest test login. `tag` is a 6-char-or-shorter
 * identifier so different subtests can't collide on each other's
 * accounts. The login fits in mhxd's 31-byte cap with room to spare. */
static void
make_test_login (char *out, gsize cap, const char *tag)
{
    g_snprintf (out, cap, "tier3_%s_%08x", tag, g_random_seed);
}

/* Try to delete `login`, ignoring any failures. Used both for
 * defensive pre-state cleanup (in case a prior aborted run left
 * residue) and post-test cleanup. */
static void
ensure_account_gone (int fd, struct htlc_conn *htlc, const char *login)
{
    guint32 trans = send_account_delete (fd, htlc, login);
    if (!trans) {
        return;
    }
    /* Drain the reply but ignore flag — the account may not exist,
     * in which case mhxd returns a task-error which is the expected
     * outcome on a clean slate. */
    integration_drain_until_task_trans (fd, htlc, trans, 8);
}

/* Create an account with the given fields and assert the server
 * accepts. Used by subtests that need a known starting state before
 * exercising MODIFY / DELETE. */
static void
create_account (int fd, struct htlc_conn *htlc, const char *login,
                const char *password, const char *name,
                const guint8 access[8])
{
    guint32 trans
        = send_account_modify (fd, htlc, login, password, name, access);
    g_assert_cmpuint (trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, htlc, trans, 16));
    g_assert_cmphex (hdr_flag (htlc) & 1, ==, 0);
}

/* ---- Tests --------------------------------------------------------- */

static void
test_create_and_read (void)
{
    init_test_state ();

    struct htlc_conn htlc;
    int fd = open_admin_login_or_fail (&htlc, "AdminCreate");
    if (fd < 0) return;

    char test_login[40];
    make_test_login (test_login, sizeof (test_login), "create");

    /* Defensive cleanup: leftover state from a previous aborted run
     * would make this look like a MODIFY of an existing account,
     * masking the create-path regression net. */
    ensure_account_gone (fd, &htlc, test_login);

    /* Access bitmap with a deliberately-scattered set of bits. The
     * dialog bug presented as "every saved account has access=0"; this
     * bitmap is non-trivial in every byte so a regression to zeroed-
     * access would fail readback assertions on multiple bytes at once.
     *
     * Bits set (per hl_access.h):
     *   bit 1  UPLOAD_FILES        byte 0 bit 6 → 0x40
     *   bit 2  DOWNLOAD_FILES      byte 0 bit 5 → 0x20
     *   bit 9  READ_CHAT           byte 1 bit 6 → 0x40
     *   bit 10 SEND_CHAT           byte 1 bit 5 → 0x20
     *   bit 20 READ_NEWS           byte 2 bit 3 → 0x08
     *   bit 21 POST_NEWS           byte 2 bit 2 → 0x04
     *
     * Result: byte 0 = 0x60, byte 1 = 0x60, byte 2 = 0x0c. */
    guint8 access[8] = { 0x60, 0x60, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00 };

    create_account (fd, &htlc, test_login, "secret", "Tier3 Initial", access);

    /* Read back; verify all four chunks present with the expected
     * payload. */
    guint32 trans = send_account_read (fd, &htlc, test_login);
    g_assert_cmpuint (trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, trans, 16));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    char name_back[64] = { 0 };
    char login_back[64] = { 0 };
    guint8 access_back[8] = { 0 };
    unsigned seen = read_account_reply (&htlc, name_back, sizeof (name_back),
                                        login_back, sizeof (login_back),
                                        access_back);
    g_assert_cmphex (seen, ==,
                     GOT_NAME | GOT_LOGIN | GOT_PASSWORD | GOT_ACCESS);
    g_assert_cmpstr (name_back, ==, "Tier3 Initial");
    g_assert_cmpstr (login_back, ==, test_login);
    g_assert_cmpmem (access_back, 8, access, 8);

    ensure_account_gone (fd, &htlc, test_login);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

static void
test_modify_and_read (void)
{
    init_test_state ();

    struct htlc_conn htlc;
    int fd = open_admin_login_or_fail (&htlc, "AdminModify");
    if (fd < 0) return;

    char test_login[40];
    make_test_login (test_login, sizeof (test_login), "modify");

    /* Establish a known initial state. Without this, the first
     * ACCOUNT_MODIFY below would behave as a CREATE (mhxd's
     * account_write handles both) and the test wouldn't actually
     * exercise the "second write replaces fields" path it claims to. */
    ensure_account_gone (fd, &htlc, test_login);
    guint8 initial_access[8]
        = { 0x60, 0x60, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00 };
    create_account (fd, &htlc, test_login, "secret", "Tier3 Initial",
                    initial_access);

    /* Different bit pattern from the initial state — flips bits in
     * every byte that was previously set, plus turns on byte 4. */
    guint8 access[8] = { 0x80, 0x10, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00 };

    guint32 trans = send_account_modify (fd, &htlc, test_login, "secret",
                                         "Tier3 Modified", access);
    g_assert_cmpuint (trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, trans, 16));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    trans = send_account_read (fd, &htlc, test_login);
    g_assert_cmpuint (trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, trans, 16));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    char name_back[64] = { 0 };
    char login_back[64] = { 0 };
    guint8 access_back[8] = { 0 };
    unsigned seen = read_account_reply (&htlc, name_back, sizeof (name_back),
                                        login_back, sizeof (login_back),
                                        access_back);
    g_assert_cmphex (seen, ==,
                     GOT_NAME | GOT_LOGIN | GOT_PASSWORD | GOT_ACCESS);
    g_assert_cmpstr (name_back, ==, "Tier3 Modified");
    g_assert_cmpstr (login_back, ==, test_login);
    g_assert_cmpmem (access_back, 8, access, 8);

    ensure_account_gone (fd, &htlc, test_login);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

static void
test_delete (void)
{
    init_test_state ();

    struct htlc_conn htlc;
    int fd = open_admin_login_or_fail (&htlc, "AdminDelete");
    if (fd < 0) return;

    char test_login[40];
    make_test_login (test_login, sizeof (test_login), "delete");

    /* Ensure the account exists before we try to delete it —
     * otherwise the DELETE itself may return a task-error and the
     * test wouldn't actually exercise the successful-delete path. */
    ensure_account_gone (fd, &htlc, test_login);
    guint8 access[8] = { 0x60, 0x60, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00 };
    create_account (fd, &htlc, test_login, "secret", "Tier3 To Delete",
                    access);

    guint32 trans = send_account_delete (fd, &htlc, test_login);
    g_assert_cmpuint (trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, trans, 16));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    /* Verify the account is actually gone — a subsequent READ should
     * fail with a task error. */
    trans = send_account_read (fd, &htlc, test_login);
    g_assert_cmpuint (trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, trans, 16));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 1); /* error bit SET */

    integration_release_htlc (&htlc);
    integration_close (fd);
}

/* Direct regression net for the useredit_chk_activate signature bug.
 * The bug presented as: every account created from the dialog ended
 * up with access=0 because the access-bit toggle callback couldn't
 * find its useredit_session pointer (notify::active passes a pspec
 * arg the old signature elided, so `data` was the pspec).
 *
 * This test creates an account, reads it back, and asserts the
 * access bitmap on the server matches what we sent — i.e., the
 * ACCESS chunk wasn't silently zeroed somewhere along the wire.
 *
 * Distinct from create_and_read above in that the bitmap here is
 * specifically chosen to be ALL bits set in the first 5 bytes
 * (everything mhxd reasonably interprets), so a partial-byte
 * regression would still flag — a future refactor that, say,
 * truncated to 4 bytes would still pass the scattered-pattern
 * test in create_and_read but would fail here. */
static void
test_access_bits_roundtrip (void)
{
    init_test_state ();

    struct htlc_conn htlc;
    int fd = open_admin_login_or_fail (&htlc, "AdminBits");
    if (fd < 0) return;

    char test_login[40];
    make_test_login (test_login, sizeof (test_login), "bits");

    /* Ensure a clean slate so we're testing CREATE, not MODIFY. */
    ensure_account_gone (fd, &htlc, test_login);

    /* All bits in the first five bytes — this exercises every byte
     * a typical mhxd account uses. Bits 40+ are reserved on most
     * deployed servers; we leave them clear to keep the readback
     * comparison simple. */
    guint8 access[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00 };

    create_account (fd, &htlc, test_login, NULL, "Bits Roundtrip", access);

    guint32 trans = send_account_read (fd, &htlc, test_login);
    g_assert_cmpuint (trans, !=, 0);
    g_assert_true (integration_drain_until_task_trans (fd, &htlc, trans, 16));
    g_assert_cmphex (hdr_flag (&htlc) & 1, ==, 0);

    guint8 access_back[8] = { 0 };
    unsigned seen
        = read_account_reply (&htlc, NULL, 0, NULL, 0, access_back);
    g_assert_true (seen & GOT_ACCESS);
    g_assert_cmpmem (access_back, 8, access, 8);

    ensure_account_gone (fd, &htlc, test_login);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    /* Each subtest is independent — its own account name, its own
     * pre-state setup, its own cleanup — so they pass in any order
     * and individually via `meson test --p /integration/user_account/X`. */
    g_test_add_func ("/integration/user_account/create_and_read",
                     test_create_and_read);
    g_test_add_func ("/integration/user_account/modify_and_read",
                     test_modify_and_read);
    g_test_add_func ("/integration/user_account/delete", test_delete);
    g_test_add_func ("/integration/user_account/access_bits_roundtrip",
                     test_access_bits_roundtrip);

    return g_test_run ();
}
