#ifndef HX_TLS_TRUST_H
#define HX_TLS_TRUST_H 1

/*
 * TLS trust-on-first-use (TOFU) cert pinning — C ABI.
 *
 * The whole trust brain (the known_hosts store, the classify /
 * silent-accept / auto-accept / prompt / pin decision, the test
 * seams, and known_hosts path resolution) lives in the Rust
 * `hxtls-trust` crate. When a peer's TLS certificate fails WebPKI
 * validation, hxnet's rustls verifier hands the leaf fingerprint
 * to hx_tls_verify_cert below, which returns whether to accept.
 * The Adwaita prompt itself is in the `gtkhx-ui` crate and is
 * registered into hxtls-trust at UI init via
 * gtkhx_tls_prompt_install.
 *
 * Storage: SSH known_hosts shape at `$CONFIG/known_hosts`, one line
 * per (host:port, sha256:<hex>) tuple. Full backward-compat with
 * the format the old C module wrote; comments / blank lines
 * round-trip a pin. See the crate + docs for the schema.
 */

#include <glib.h>

/*
 * TOFU verify entry. hxnet's rustls verifier calls this (on its
 * worker thread) with the peer leaf fingerprint ("sha256:<hex>")
 * ONLY when WebPKI validation against the native roots failed (a
 * CA-valid cert is trusted silently and never reaches here). Keyed
 * on (host, port). Returns TRUE to accept the cert, FALSE to
 * reject (the handshake then aborts). The user prompt, when
 * needed, is marshalled to the main thread inside the crate's
 * registered prompt callback.
 */
extern gboolean
hx_tls_verify_cert (const char *host, guint16 port, const char *fingerprint);

/*
 * Pin (host, port, fingerprint) into the known_hosts store (today's
 * date on the "added" comment), replacing any existing entry for the
 * same (host, port). Returns TRUE on success. Exposed for the
 * integration tests to seed a trust state through the same path
 * resolver production uses.
 */
extern gboolean
hx_tls_trust_pin (const char *host, guint16 port, const char *fingerprint);

/*
 * Register the Adwaita TOFU prompt (gtkhx-ui) with the trust brain.
 * Called once from fe_init before any connect. In headless test
 * binaries that don't link gtkhx-ui this is simply never called, and
 * the prompt path rejects (tests always set a seam first).
 */
extern void gtkhx_tls_prompt_install (void);

/*
 * Thread-safe test seams. The multi-threaded Tier 3 tests steer the
 * TLS trust / connect behaviour with these process-global overrides
 * rather than mutating environment variables — the verify callback
 * runs on the hxnet (tokio) worker thread, so a main-thread setenv
 * racing a worker-thread getenv is a data race on the global
 * `environ`. Each override defaults to "unset", in which case the
 * corresponding env var is consulted exactly as before, so
 * env-configured harnesses and production (which never sets an
 * override) are unchanged. Backed by atomics / a mutex in the crate;
 * call from any thread.
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

/* Resolver used by the connect path's TLS gate: override first, else
 * the GTKHX_TLS env var. */
gboolean hx_tls_test_force_tls (void);

#endif /* HX_TLS_TRUST_H */
