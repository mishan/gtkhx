/*
 * tests/integration/test_banner.c — verify the server's banner
 * announcement and (in file mode) the HTXF subchannel fetch.
 *
 * Hotline 1.9 has two banner modes:
 *
 *   URL  — server's HTLS_HDR_BANNER carries TYPE="URL " plus a
 *          HTLS_DATA_BANNER_URL string. The client (in production)
 *          fetches the image via HTTP itself.
 *   FILE — server's HTLS_HDR_BANNER carries a binary TYPE
 *          ("GIFf", "JPEG", "PICT", ...) and no URL. The client
 *          follows up with HTLC_HDR_DOWNLOAD_BANNER; the server
 *          replies with HTLS_DATA_HTXF_REF + HTLS_DATA_HTXF_SIZE;
 *          the client opens base_port+1, writes the HTXF preamble,
 *          and reads `size` bytes of image data.
 *
 * The gtkhx-mhxd container is runtime-configurable via BANNER_MODE
 * (see tests/mhxd/docker-entrypoint.sh). Each test below is
 * mode-aware: it inspects HTLS_HDR_BANNER's TYPE field and fails
 * (g_test_fail_printf) if the server is serving the other mode.
 * That way the matching test for the container's current mode
 * passes; the non-matching test loudly indicates a mode mismatch
 * (which is real signal that the container isn't configured for
 * the test you wanted). To run both modes you currently need two
 * container instances or a re-run with the env var flipped.
 *
 *   docker run gtkhx-mhxd                        → URL test fires
 *   docker run -e BANNER_MODE=JPEG gtkhx-mhxd    → HTXF test fires
 *
 * mhxd gates rcv_agreementagree on htlc->access_extra.can_agree,
 * which is only set in rcv_login's `got_name == false` branch
 * (the legacy flow where the client sends NAME later in the
 * agreement-agree message). The standard
 * integration_open_login_or_skip helper sends NAME at login time,
 * which puts us in the modern-client branch where can_agree == 0
 * — and AGREEMENTAGREE gets rejected. So we hand-roll a "skinny"
 * login (no NAME) below to put mhxd in the legacy flow.
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
#include "server_matrix.h"
#include "htxf_subchannel.h"

/* The URL the gtkhx-mhxd test container is configured to advertise
 * when BANNER_MODE=URL (the default). Kept in sync by hand with
 * tests/mhxd/docker-entrypoint.sh's URL_DEFAULT. Override at run
 * time via $GTKHX_TEST_BANNER_URL — useful when pointing the
 * suite at a non-default test server. */
#define GTKHX_TEST_BANNER_URL_DEFAULT                                          \
    "https://placehold.co/468x60/png?text=GtkHx+Test+Banner"

/* hl_code: XOR-with-0xff cipher used by the LOGIN chunk encoding. */
static void
hl_code_inline (void *dst, const void *src, gsize len)
{
    const guint8 *s = src;
    guint8 *d = dst;
    for (gsize i = 0; i < len; i++) {
        d[i] = ~s[i];
    }
}

/* Skinny login: ICON + LOGIN + CLIENTVERSION, no NAME. mhxd's
 * rcv_login then takes the in_login=1 branch and sets can_agree=1,
 * deferring user_loginupdate (and the SELFINFO send) until we
 * follow up with AGREEMENTAGREE. */
static gboolean
send_skinny_login (int fd, struct htlc_conn *htlc, guint16 icon)
{
    const char *login = "guest";
    gsize llen = strlen (login);
    guint8 enclogin[16];
    hl_code_inline (enclogin, login, llen);
    guint16 icon_be = htons (icon);
    guint16 cv_be = htons (185);

    return integration_send_message (
        fd, htlc, HTLC_HDR_LOGIN, /*flag=*/0, /*hc=*/3, (int)HTLC_DATA_ICON,
        (int)sizeof (icon_be), &icon_be, (int)HTLC_DATA_LOGIN, (int)llen,
        enclogin, (int)HTLC_DATA_CLIENTVERSION, (int)sizeof (cv_be), &cv_be);
}

/* Shared setup: skinny-login → AGREEMENTAGREE → drain until
 * HTLS_HDR_BANNER. On success returns a valid `fd` (caller closes),
 * populates out_type / out_url with the banner's chunks (g_strndup'd;
 * nullable on absent), and leaves htlc->in holding the
 * HTLS_HDR_BANNER frame so the caller can match on type without
 * reparsing. Returns -1 on any failure path (test_fail already
 * called).
 *
 * `srv` selects which container to drive — the two banner subtests
 * use different ones (url_mode → mhxd in URL mode; htxf_mode → Janus
 * which serves HTXF banner unconditionally). Routing per-subtest
 * keeps each assertion exercising the matching server-side
 * configuration. */
static int
banner_setup_or_skip (const hx_test_server *srv, struct htlc_conn *htlc,
                      gchar **out_type, gchar **out_url)
{
    memset (htlc, 0, sizeof (*htlc));
    *out_type = NULL;
    *out_url = NULL;

    if (!srv) {
        g_test_fail_printf ("no matching server in matrix for this subtest");
        return -1;
    }

    int fd = hx_integration_connect_to (srv->host, srv->port,
                                        /*timeout_ms=*/2000);
    if (fd < 0) {
        g_test_fail_printf ("connect to %s (%s:%d) failed", srv->name,
                            srv->host, (int) srv->port);
        return -1;
    }
    if (!integration_handshake (fd)) {
        g_test_fail_printf ("handshake against %s failed", srv->name);
        integration_close (fd);
        return -1;
    }

    if (!send_skinny_login (fd, htlc, 412)) {
        g_test_fail_printf ("skinny login send failed");
        integration_release_htlc (htlc);
        integration_close (fd);
        return -1;
    }

    /* Drain past the loginreply TASK + AGREEMENT mhxd sends
	 * before we agree. The real interesting messages arrive
	 * after AGREEMENTAGREE. */
    for (int i = 0; i < 4; i++) {
        if (!integration_recv_message (fd, htlc, /*timeout_ms=*/2000)) {
            break;
        }
    }

    const char *name = "Banner Tier-3";
    guint16 icon_be = htons (412);
    /* OPTIONS bitmap (0x0071, same code as HTLC_DATA_BAN in mhxd's
	 * naming). Mhxd ignores the body; Mobius reads it as a big-
	 * endian u16 and panics if the chunk is missing. Send 0x0000.
	 * Matches the production hx_send_agreement_agree shape so an
	 * eventual Mobius Tier-3 target exercises the same wire bytes
	 * the real client sends. See [[gtkhx_mobius_options_field]]. */
    guint16 options_be = htons (0);
    if (!integration_send_message (
            fd, htlc, HTLC_HDR_AGREEMENTAGREE, /*flag=*/0, /*hc=*/3,
            (int)HTLC_DATA_NAME, (int)strlen (name), (guint8 *)name,
            (int)HTLC_DATA_ICON, (int)sizeof (icon_be), &icon_be,
            (int)HTLC_DATA_OPTIONS, (int)sizeof (options_be), &options_be)) {
        g_test_fail_printf ("AGREEMENTAGREE send failed");
        integration_release_htlc (htlc);
        integration_close (fd);
        return -1;
    }

    if (!integration_drain_until_type (fd, htlc, HTLS_HDR_BANNER, 64)) {
        g_test_fail_printf ("no HTLS_HDR_BANNER received");
        integration_release_htlc (htlc);
        integration_close (fd);
        return -1;
    }
    dh_start (htlc->in.buf, htlc->in.pos)
    {
        switch (_type) {
        case HTLS_DATA_BANNER_TYPE:
            *out_type = g_strndup ((const char *)dh->data, _len);
            break;
        case HTLS_DATA_BANNER_URL:
            *out_url = g_strndup ((const char *)dh->data, _len);
            break;
        }
    }
    dh_end ();
    g_test_message ("banner type=\"%s\" url=\"%s\"",
                    *out_type ? *out_type : "(null)",
                    *out_url ? *out_url : "(null)");
    return fd;
}

/* Strip a single trailing space to normalise "URL " ↔ "URL". */
static gboolean
banner_type_is_url (const char *t)
{
    if (!t) {
        return FALSE;
    }
    gsize n = strlen (t);
    if (n > 0 && t[n - 1] == ' ') {
        n--;
    }
    return n == 3 && g_ascii_strncasecmp (t, "URL", 3) == 0;
}

/* URL mode: server's HTLS_HDR_BANNER carried TYPE="URL " plus the
 * expected URL string. Runs against the default matrix server
 * (mhxd) — the asserted URL is mhxd-specific (set by
 * tests/mhxd/docker-entrypoint.sh) so this subtest is intentionally
 * scoped to that container. */
static void
test_banner_url_mode (void)
{
    struct htlc_conn htlc;
    gchar *banner_type = NULL, *banner_url = NULL;
    const hx_test_server *srv = hx_test_server_default ();
    int fd = banner_setup_or_skip (srv, &htlc, &banner_type, &banner_url);
    if (fd < 0) {
        return;
    }

    if (!banner_type_is_url (banner_type)) {
        g_test_fail_printf ("server is not in URL banner mode");
        goto cleanup;
    }

    g_assert_nonnull (banner_type);
    g_assert_cmpuint (strlen (banner_type), ==, 4);
    g_assert_cmpstr (banner_type, ==, "URL ");

    /* URL must match the container's configured banner exactly.
	 * Kept in sync with tests/mhxd/docker-entrypoint.sh's
	 * URL_DEFAULT. $GTKHX_TEST_BANNER_URL overrides for ad-hoc
	 * runs against a non-default test server. */
    const char *expected_url = g_getenv ("GTKHX_TEST_BANNER_URL");
    if (!expected_url || !*expected_url) {
        expected_url = GTKHX_TEST_BANNER_URL_DEFAULT;
    }
    g_assert_nonnull (banner_url);
    g_assert_cmpstr (banner_url, ==, expected_url);

cleanup:
    g_free (banner_type);
    g_free (banner_url);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

/* Validate that a buffer starts with the magic bytes for the
 * declared banner image type. */
static gboolean
banner_bytes_match_type (const char *type, const guint8 *bytes, gsize len)
{
    if (!type || !bytes || len < 4) {
        return FALSE;
    }
    if (strcmp (type, "JPEG") == 0) {
        return bytes[0] == 0xff && bytes[1] == 0xd8;
    }
    if (strcmp (type, "GIFf") == 0 || strcmp (type, "GIF ") == 0) {
        return bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F'
               && bytes[3] == '8';
    }
    if (strcmp (type, "PICT") == 0) {
        /* QuickDraw PICT: first 512 bytes are header padding;
		 * we don't validate the inner format, just accept any
		 * non-zero prefix. */
        return TRUE;
    }
    if (strcmp (type, "PNG ") == 0) {
        return bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N'
               && bytes[3] == 'G';
    }
    /* Unknown / unhandled image types: accept rather than fail
	 * the test on an unfamiliar magic. */
    return TRUE;
}

/* Pick the first server in the matrix advertising the file-mode
 * banner cap. Today that's Janus — mhxd's container defaults to
 * URL mode and had BANNER_HTXF stripped from its caps in the
 * matrix-fix branch. Returns NULL (→ test_fail) if no server in
 * scope can serve the HTXF banner path. */
static const hx_test_server *
pick_banner_htxf_server (void)
{
    GPtrArray *servers = hx_test_servers_with (HX_TEST_CAP_BANNER_HTXF);
    if (!servers) {
        return NULL;
    }
    const hx_test_server *srv = NULL;
    if (servers->len > 0) {
        srv = g_ptr_array_index (servers, 0);
    }
    g_ptr_array_unref (servers);
    return srv;
}

/* HTXF (file) mode: server announced a binary TYPE without a URL.
 * Exercise the full follow-up — HTLC_HDR_DOWNLOAD_BANNER → TASK
 * reply with REF+SIZE → HTXF subchannel → bytes — and verify the
 * downloaded data starts with the correct magic for the declared
 * type. Drives the first server in the matrix that advertises
 * HX_TEST_CAP_BANNER_HTXF (Janus today). */
static void
test_banner_htxf_mode (void)
{
    struct htlc_conn htlc;
    gchar *banner_type = NULL, *banner_url = NULL;
    const hx_test_server *srv = pick_banner_htxf_server ();
    if (!srv) {
        g_test_fail_printf ("no server in matrix advertising "
                            "HX_TEST_CAP_BANNER_HTXF. Bring Janus up "
                            "(or mhxd with BANNER_MODE=JPEG and the "
                            "cap restored).");
        return;
    }
    int fd = banner_setup_or_skip (srv, &htlc, &banner_type, &banner_url);
    if (fd < 0) {
        return;
    }

    if (banner_type_is_url (banner_type)) {
        g_test_fail_printf ("server %s is in URL banner mode", srv->name);
        goto cleanup;
    }
    if (banner_url && *banner_url) {
        /* Server is in a confused state — declared a binary type
		 * but also shipped a URL. Per 1.9 spec the client
		 * dispatches on type, so this is still a valid test
		 * target; flag it in the log but proceed with HTXF. */
        g_test_message ("warning: server type=\"%s\" but also sent URL=\"%s\"; "
                        "proceeding with HTXF fetch per spec",
                        banner_type, banner_url);
    }

    /* hlpack (called inside integration_send_message) reads
	 * htlc->trans for the on-wire value and *then* increments
	 * it. Capture the pre-send value so we can match the TASK
	 * reply's trans field against what actually went out — not
	 * the post-increment value. */
    guint32 our_trans = htlc.trans;
    if (!integration_send_message (fd, &htlc, HTLC_HDR_DOWNLOAD_BANNER,
                                   /*flag=*/0, /*hc=*/0)) {
        g_test_fail_printf ("HTLC_HDR_DOWNLOAD_BANNER send failed");
        goto cleanup;
    }

    /* Drain looking for the TASK reply matching our trans. */
    struct hx_htxf_reply reply = { 0 };
    gboolean got_reply = FALSE;
    for (int i = 0; i < 16 && !got_reply; i++) {
        if (!integration_recv_message (fd, &htlc, /*timeout_ms=*/3000)) {
            break;
        }
        if (hdr_type (&htlc) != HTLS_HDR_TASK) {
            continue;
        }
        if (hdr_trans (&htlc) != our_trans) {
            continue;
        }
        got_reply = TRUE;
        hx_htxf_reply_extract (htlc.in.buf, htlc.in.pos, &reply);
    }
    g_assert_true (got_reply);
    g_assert_cmpuint (reply.ref, >, 0);
    g_assert_cmpuint (reply.size, >, 0);
    g_assert_cmpuint (reply.size, <, 1u << 20); /* sanity cap: <1 MB */
    guint32 ref = reply.ref, size = reply.size;
    g_test_message ("HTXF ref=%u size=%u", ref, size);

    /* Open the HTXF subchannel on the SAME server we drove the
	 * control channel against — integration_connect_xfer routes
	 * through hx_test_server_default and would return mhxd's xfer
	 * port (5501), which knows nothing about the ref Janus issued.
	 * Same fix-pattern as the HOPE+stream banner tests. Also use
	 * the production preamble packer with HTXF_TYPE_BANNER (not
	 * integration_send_xfer_hdr's default FILE — Janus refuses a
	 * banner ref on a FILE-typed connection). */
    int xfer_fd = hx_integration_connect_to (srv->host, srv->xfer_port,
                                             /*timeout_ms=*/2000);
    g_assert_cmpint (xfer_fd, >=, 0);

    guint8 hdr_buf[HX_HTXF_PREAMBLE_MAX_BYTES];
    size_t hdr_len = hx_htxf_subchannel_pack_preamble (
        hdr_buf, sizeof (hdr_buf), ref, size, HTXF_TYPE_BANNER,
        /*flags=*/0, /*size64=*/FALSE);
    g_assert_cmpuint (hdr_len, >, 0);
    g_assert_true (integration_send (xfer_fd, hdr_buf, hdr_len));

    guint8 *bytes = g_malloc (size);
    g_assert_true (integration_recv (xfer_fd, bytes, size));
    integration_close (xfer_fd);

    g_test_message ("first 4 bytes: %02x %02x %02x %02x", bytes[0], bytes[1],
                    bytes[2], bytes[3]);
    g_assert_true (banner_bytes_match_type (banner_type, bytes, size));

    g_free (bytes);

cleanup:
    g_free (banner_type);
    g_free (banner_url);
    integration_release_htlc (&htlc);
    integration_close (fd);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/integration/banner/url_mode", test_banner_url_mode);
    g_test_add_func ("/integration/banner/htxf_mode", test_banner_htxf_mode);

    return g_test_run ();
}
