/*
 * tests/integration/test_banner.c — verify the server sends an
 * HTLS_HDR_BANNER broadcast after the client agrees to the
 * agreement, and verify the URL is the specific one our test
 * mhxd container is configured to serve.
 *
 * mhxd's rcv_agreementagree (mhxd/src/hxd/rcv.c) sends the banner
 * unconditionally if hxd_cfg.banner.type is non-empty. The
 * gtkhx-mhxd container under tests/mhxd patches hxd.conf to set:
 *   banner {
 *       type "URL";
 *       url  "https://placehold.co/468x60/png?text=GtkHx+Test+Banner";
 *   }
 * so an HTLS_HDR_BANNER should arrive carrying both
 * HTLS_DATA_BANNER_TYPE = "URL " (4 bytes) and
 * HTLS_DATA_BANNER_URL  = exactly that URL. The test asserts an
 * exact string match so the container's banner configuration
 * stays a known test fixture — both for the automated check and
 * for manual smoke-testing: connecting GtkHx to the container
 * should display a 468x60 banner image reading "GtkHx Test
 * Banner".
 *
 * mhxd gates rcv_agreementagree on htlc->access_extra.can_agree,
 * which is only set in rcv_login's `got_name == false` branch
 * (the legacy flow where the client sends NAME later in the
 * agreement-agree message). The standard
 * integration_open_login_or_skip helper sends NAME at login time,
 * which puts us in the modern-client branch where can_agree == 0
 * — and AGREEMENTAGREE gets rejected.
 *
 * So this test does a hand-rolled "skinny" login without NAME,
 * then sends AGREEMENTAGREE carrying NAME + ICON to trigger
 * mhxd's banner-send code path. GtkHx itself does the modern
 * (with-NAME) login and never sees the banner from mhxd as a
 * result; the integration suite covers the flow GtkHx WOULD see
 * if it followed the legacy path.
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

/* The URL the gtkhx-mhxd test container is configured to advertise.
 * Kept in sync by hand with tests/mhxd/Dockerfile's sed-patch of
 * hxd.conf. Override at run time via $GTKHX_TEST_BANNER_URL — useful
 * when pointing the suite at a non-default test server. */
#define GTKHX_TEST_BANNER_URL_DEFAULT \
	"https://placehold.co/468x60/png?text=GtkHx+Test+Banner"

static guint32
hdr_type (const struct htlc_conn *htlc)
{
	const struct hl_hdr *h = (const struct hl_hdr *) htlc->in.buf;
	return ntohl (h->type);
}

/* hl_code: XOR-with-0xff cipher used by the LOGIN chunk encoding. */
static void
hl_code_inline (void *dst, const void *src, gsize len)
{
	const guint8 *s = src;
	guint8 *d = dst;
	for (gsize i = 0; i < len; i++)
		d[i] = ~s[i];
}

/* Skinny login: ICON + LOGIN + CLIENTVERSION, no NAME. mhxd's
 * rcv_login then takes the in_login=1 branch and sets can_agree=1,
 * deferring user_loginupdate (and the SELFINFO send) until we
 * follow up with AGREEMENTAGREE.
 *
 * The standard integration_send_message uses the calling htlc's
 * out buffer; we work directly here to keep banner self-contained. */
static gboolean
send_skinny_login (int fd, struct htlc_conn *htlc, guint16 icon)
{
	const char *login = "guest";
	gsize llen = strlen (login);
	guint8 enclogin[16];
	hl_code_inline (enclogin, login, llen);
	guint16 icon_be = htons (icon);
	guint16 cv_be   = htons (185);

	return integration_send_message (
		fd, htlc,
		HTLC_HDR_LOGIN, /*flag=*/0, /*hc=*/3,
		(int) HTLC_DATA_ICON,           (int) sizeof (icon_be), &icon_be,
		(int) HTLC_DATA_LOGIN,          (int) llen, enclogin,
		(int) HTLC_DATA_CLIENTVERSION,  (int) sizeof (cv_be), &cv_be);
}

static void
test_banner_arrives_after_agreementagree (void)
{
	struct htlc_conn htlc;
	memset (&htlc, 0, sizeof (htlc));

	int fd = integration_open_or_skip ();
	if (fd < 0)
		return;

	g_assert_true (send_skinny_login (fd, &htlc, 412));

	/* Drain past the loginreply TASK and any AGREEMENT chunks
	 * mhxd sent. We don't expect SELFINFO at this point because
	 * in_login=1 keeps user_loginupdate gated until
	 * AGREEMENTAGREE. Drain a few messages opportunistically; the
	 * actual messages we DO want come AFTER the AGREEMENTAGREE we
	 * send next. */
	for (int i = 0; i < 4; i++) {
		if (!integration_recv_message (
				fd, &htlc, /*timeout_ms=*/2000))
			break;
	}

	/* Send HTLC_HDR_AGREEMENTAGREE with NAME + ICON. mhxd's
	 * rcv_agreementagree finishes login, runs user_loginupdate
	 * (sending SELFINFO), and unconditionally sends the banner
	 * if banner.type is configured. */
	const char *name = "Banner Tier-3";
	guint16 icon_be = htons (412);
	g_assert_true (integration_send_message (
		fd, &htlc,
		HTLC_HDR_AGREEMENTAGREE, /*flag=*/0, /*hc=*/2,
		(int) HTLC_DATA_NAME, (int) strlen (name), (guint8 *) name,
		(int) HTLC_DATA_ICON, (int) sizeof (icon_be), &icon_be));

	/* Drain looking for HTLS_HDR_BANNER. */
	gboolean got_banner = FALSE;
	gchar *banner_type = NULL;
	gchar *banner_url = NULL;
	for (int i = 0; i < 64 && !got_banner; i++) {
		if (!integration_recv_message (fd, &htlc, /*timeout_ms=*/3000))
			break;
		if (hdr_type (&htlc) != HTLS_HDR_BANNER)
			continue;
		got_banner = TRUE;

		dh_start (&htlc) {
			switch (_type) {
			case HTLS_DATA_BANNER_TYPE:
				banner_type = g_strndup (
					(const char *) dh->data, _len);
				break;
			case HTLS_DATA_BANNER_URL:
				banner_url = g_strndup (
					(const char *) dh->data, _len);
				break;
			}
		} dh_end ();
	}
	g_assert_true (got_banner);

	/* Banner type is exactly 4 bytes — "URL " (trailing-space
	 * padded) per the container config. */
	g_assert_nonnull (banner_type);
	g_assert_cmpuint (strlen (banner_type), ==, 4);
	g_assert_cmpstr (banner_type, ==, "URL ");
	g_test_message ("banner type: \"%s\"", banner_type);

	/* URL must match the container's configured banner exactly.
	 * If they drift, either the Dockerfile or the test was
	 * updated without the other — and that drift is the bug.
	 * $GTKHX_TEST_BANNER_URL overrides for ad-hoc runs against
	 * a different test server. */
	const char *expected_url = g_getenv ("GTKHX_TEST_BANNER_URL");
	if (!expected_url || !*expected_url)
		expected_url = GTKHX_TEST_BANNER_URL_DEFAULT;
	g_assert_nonnull (banner_url);
	g_assert_cmpstr (banner_url, ==, expected_url);
	g_test_message ("banner url:  \"%s\"", banner_url);

	g_free (banner_type);
	g_free (banner_url);
	integration_release_htlc (&htlc);
	integration_close (fd);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/integration/banner/arrives_after_agreementagree",
	                 test_banner_arrives_after_agreementagree);

	return g_test_run ();
}
