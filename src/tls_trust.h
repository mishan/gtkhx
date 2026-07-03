#ifndef HX_TLS_TRUST_H
#define HX_TLS_TRUST_H 1

/*
 * TLS Phase 3 — trust-on-first-use (TOFU) cert pinning.
 *
 * Replaces the Phase 1 accept-everything-cert stub with a real
 * trust check: compute a SHA-256 fingerprint over the peer's
 * leaf certificate, look it up in a per-user known-hosts file,
 * and surface a structured decision (TRUSTED / UNKNOWN /
 * MISMATCH) so network.c can either accept silently or hand the
 * decision to the TOFU prompt (tls_trust_dialog.{c,h}).
 *
 * Storage: SSH known_hosts shape at
 * `$CONFIG/known_hosts` — one line per (host:port,
 * fingerprint) tuple. Plain text so it's inspectable / editable
 * with a text editor; new entries appended; existing entries
 * matched line-by-line. Unknown lines (comments, future
 * extension formats) are preserved on round-trip so a manual
 * edit doesn't get clobbered by the next pin.
 *
 * The module is pure GIO/GLib — no UI, no GTK includes. The
 * dialog wrapping lives in tls_trust_dialog. Tier 1 tests drive
 * this layer directly with a tmpdir + synthetic fingerprints;
 * Phase 3's dialog flow is exercised via Tier 3 against the
 * Janus container with GTKHX_TLS_AUTO_ACCEPT=1 so headless test
 * binaries don't deadlock waiting on a prompt.
 */

#include <glib.h>
#include <gio/gio.h>

typedef enum {
    /* Fingerprint matches a pinned entry for this host:port —
     * accept the cert silently. */
    HX_TLS_TRUST_TRUSTED = 0,
    /* No entry pinned for this host:port — first time we've
     * seen this server. Caller should prompt the user and call
     * hx_tls_trust_pin on accept. */
    HX_TLS_TRUST_UNKNOWN,
    /* An entry IS pinned for this host:port but the fingerprint
     * doesn't match. Could be a cert rotation, a server move,
     * or a MITM. Caller should prompt with a stronger warning
     * than UNKNOWN. */
    HX_TLS_TRUST_MISMATCH,
} hx_tls_trust_status;

/*
 * Compute the SHA-256 fingerprint of a peer certificate (the
 * leaf cert's DER encoding). Returns a newly-allocated
 * lowercase-hex string of the form
 * "sha256:aabbccdd..." (71 chars: 7 for the prefix + 64 hex
 * digits). NULL on failure (cert without DER blob — shouldn't
 * happen for a real GTlsClientConnection peer cert).
 *
 * Caller frees with g_free.
 */
extern gchar *hx_tls_trust_fingerprint (GTlsCertificate *cert);

/*
 * Look up (host, port, fingerprint) in the known-hosts file.
 *
 * Returns one of HX_TLS_TRUST_TRUSTED / UNKNOWN / MISMATCH.
 * The file path is resolved by hx_tls_trust_known_hosts_path
 * (see below). A missing or unreadable file is treated as
 * empty (every cert is UNKNOWN), which gives a sane first-run
 * experience.
 *
 * Inputs not validated for format — caller passes the strings
 * produced by hx_tls_trust_fingerprint and the host/port the
 * client connected to.
 */
extern hx_tls_trust_status
hx_tls_trust_lookup (const char *host, guint16 port, const char *fingerprint);

/*
 * Returns TRUE if any entry in the known-hosts file pins
 * `fingerprint` for `host` — at any port, including hostname-
 * only entries.
 *
 * Used by the accept-certificate handler to silently accept a
 * cert that was already trusted for the same host on a
 * different port. The typical case: the user pins the control-
 * channel cert (host:5600), then opens the file-transfer
 * subchannel (host:5601) which presents the same cert. The
 * strict (host, port, fp) lookup misses on 5601, but this
 * any-port check sees the 5600 entry's fingerprint matches
 * and we can accept without a second prompt.
 *
 * Only safe to consult on a strict-UNKNOWN result. A
 * strict-MISMATCH (a different cert for THIS host:port) is a
 * security signal that this function deliberately does not
 * override.
 */
extern gboolean
hx_tls_trust_host_has_fingerprint (const char *host, const char *fingerprint);

/*
 * Append (host, port, fingerprint) to the known-hosts file as
 * a new line. If the file doesn't exist it's created with mode
 * 0600 (the file may contain sensitive trust state, mirrors
 * SSH's known_hosts permissions).
 *
 * On a MISMATCH followed by user-accepted pin, this overwrites
 * the existing entry: the old fingerprint line is removed and
 * the new one appended. The trailing comment carries an "added
 * <ISO date>" marker for forensics.
 *
 * Returns TRUE on success.
 */
extern gboolean
hx_tls_trust_pin (const char *host, guint16 port, const char *fingerprint);

/*
 * Resolve the known-hosts file path: $CONFIG/known_hosts.
 *
 * Exposed so the dialog can mention the path in its body text
 * ("delete the offending line in <path> to clear this pin"),
 * and so tests can assert against an expected location. The
 * GTKHX_KNOWN_HOSTS env var overrides for tests + ad-hoc runs
 * — set to a tmpdir path in Tier 1 tests so the user's real
 * known-hosts file isn't touched.
 *
 * Returned string is g_strdup'd; caller frees.
 */
extern gchar *hx_tls_trust_known_hosts_path (void);


/*
 * Thread-safe test seams (integration tests in practice).
 *
 * The multi-threaded Tier 3 tests used to steer the TLS trust / connect
 * behaviour by mutating environment variables (GTKHX_TLS_AUTO_ACCEPT /
 * GTKHX_TLS_TEST_PROMPT / GTKHX_KNOWN_HOSTS / GTKHX_TLS) with
 * g_setenv/g_unsetenv. But the TLS verify callback runs on the hxnet
 * (tokio) worker thread and reads those vars with g_getenv, so a
 * main-thread g_setenv racing a worker-thread g_getenv is a data race on
 * the global `environ` (setenv can realloc it) -> a SIGSEGV under the
 * wrong timing. See tests/integration/test_real_connect.c.
 *
 * These setters install a process-global override that the connect /
 * verify code consults FIRST, so a test never has to touch `environ`
 * after the worker threads exist. Each override defaults to "unset", in
 * which case the corresponding env var is consulted exactly as before --
 * so env-configured harnesses (which set the var once in main() before
 * any thread spawns) and the single-threaded Tier 1 trust tests keep
 * working unchanged, and production (which never sets an override) is
 * byte-for-byte identical. Backed by atomics / a mutex; call from any
 * thread.
 *
 * The tri-state ints are: <0 = unset (consult env), 0 = force off,
 * >0 = force on.
 */
void hx_tls_test_set_auto_accept (int tri);
void hx_tls_test_set_force_tls (int tri);
/* prompt verdict: 0 = unset (consult env / show the real prompt),
 * 1 = accept, 2 = reject. */
void hx_tls_test_set_prompt_verdict (int verdict);
/* known-hosts path override; NULL clears it (consult env / default).
 * The string is copied. */
void hx_tls_test_set_known_hosts (const char *path);

/* Resolvers used by the connect / verify code: override first, else the
 * env var. */
gboolean hx_tls_test_auto_accept (void);
gboolean hx_tls_test_force_tls (void);
int      hx_tls_test_prompt_verdict (void);

#endif /* HX_TLS_TRUST_H */
