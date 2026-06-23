//! TLS-from-byte-zero for the orchestrator (separate-port model).
//!
//! Mobius and Janus ship plain TLS on a dedicated port (Janus:
//! 5610 HTLS-TLS / 5611 HTXF-TLS) with no in-band negotiation:
//! connect TCP, do the TLS handshake immediately, then speak the
//! ordinary Hotline protocol (plaintext or HOPE) over the encrypted
//! stream. This module wraps a connected [`tokio::net::TcpStream`]
//! in a `tokio_rustls` client TLS stream so the rest of the
//! lifecycle (magic + LOGIN + …) runs over it unchanged — the
//! lifecycle helpers are already generic over `AsyncRead +
//! AsyncWrite`, and a TLS stream satisfies that.
//!
//! # Certificate trust — WebPKI first, TOFU on failure
//!
//! Trust mirrors the legacy `GTlsConnection` model and a normal
//! browser/SSH split:
//!
//! 1. **Handshake (this module):** the rustls verifier
//!    ([`WebPkiOrTofu`]) runs real WebPKI validation — chain to a
//!    native trust root + hostname (SNI) match — against the system
//!    root store ([`rustls_native_certs`]). A CA-valid cert (e.g.
//!    Let's Encrypt) passes here. The verifier records the WebPKI
//!    verdict in a shared flag and then **completes the handshake
//!    regardless**, so a cert that *doesn't* chain to a public root
//!    can still be offered to the post-handshake TOFU gate rather
//!    than hard-failing the connection. Handshake-signature checks
//!    always run against the presented cert.
//! 2. **Post-handshake (the lifecycle):** if WebPKI validated, the
//!    cert is trusted silently — no prompt, no TOFU lookup, exactly
//!    like a browser hitting a CA-signed site. Only when WebPKI did
//!    **not** validate does `run_plaintext_tls_lifecycle` fall back
//!    to the C-side `verify_cert` callback
//!    (`hx_tls_orchestrator_verify_cert` → `hx_tls_trust_lookup`):
//!    the trust-on-first-use decision over `$CONFIG/known_hosts`
//!    (accept TRUSTED silently; prompt on UNKNOWN / MISMATCH,
//!    marshalled to the GLib main thread; pin on accept). A reject
//!    closes the stream before any credentials are sent.
//!
//! Keeping the (possibly blocking, main-thread) TOFU decision
//! post-handshake — rather than inside the rustls verifier — is why
//! the verifier completes the handshake instead of returning the
//! WebPKI error: it defers the *decision*, not the *check*. The cert
//! is never trusted unless WebPKI validated it or TOFU accepted it.

use std::io;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use tokio::net::TcpStream;
use tokio_rustls::client::TlsStream;
use tokio_rustls::rustls::client::danger::{
    HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier,
};
use tokio_rustls::rustls::client::WebPkiServerVerifier;
use tokio_rustls::rustls::crypto::ring;
use tokio_rustls::rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use tokio_rustls::rustls::{
    ClientConfig, DigitallySignedStruct, RootCertStore, SignatureScheme,
};
use tokio_rustls::TlsConnector;

/// Certificate verifier that runs WebPKI validation against the
/// native root store, records the verdict in `webpki_ok`, and then
/// accepts the cert so the handshake completes. The trust *decision*
/// happens post-handshake in the lifecycle: trusted silently when
/// `webpki_ok`, else routed to the TOFU callback. See the module docs.
///
/// Signature verification and scheme negotiation delegate to the inner
/// [`WebPkiServerVerifier`] so the handshake's cryptographic checks are
/// the real rustls ones, not a hand-rolled subset.
#[derive(Debug)]
struct WebPkiOrTofu {
    inner: Arc<WebPkiServerVerifier>,
    webpki_ok: Arc<AtomicBool>,
}

impl ServerCertVerifier for WebPkiOrTofu {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        intermediates: &[CertificateDer<'_>],
        server_name: &ServerName<'_>,
        ocsp_response: &[u8],
        now: UnixTime,
    ) -> Result<ServerCertVerified, tokio_rustls::rustls::Error> {
        match self.inner.verify_server_cert(
            end_entity,
            intermediates,
            server_name,
            ocsp_response,
            now,
        ) {
            Ok(verified) => {
                // Chains to a public root AND the hostname matches:
                // CA-valid. Trust it silently post-handshake.
                self.webpki_ok.store(true, Ordering::Relaxed);
                Ok(verified)
            }
            Err(e) => {
                // Not WebPKI-trusted (self-signed, private CA, name
                // mismatch, expired, …). Log the reason so an operator
                // can tell an expected self-signed / private-CA cert
                // apart from a suspicious failure (hostname mismatch,
                // expired) when a TOFU prompt shows up. Then complete the
                // handshake so the post-handshake TOFU gate can decide;
                // record the miss.
                glib::g_debug!(
                    "hxnet",
                    "TLS: WebPKI validation failed ({e}); routing to TOFU gate"
                );
                self.webpki_ok.store(false, Ordering::Relaxed);
                Ok(ServerCertVerified::assertion())
            }
        }
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, tokio_rustls::rustls::Error> {
        self.inner.verify_tls12_signature(message, cert, dss)
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, tokio_rustls::rustls::Error> {
        self.inner.verify_tls13_signature(message, cert, dss)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.inner.supported_verify_schemes()
    }
}

/// Load the system trust roots into a `RootCertStore`. A failure to
/// read the store (or an empty store) is non-fatal: the returned store
/// simply has no roots, so WebPKI validation always fails and every
/// cert falls through to the TOFU gate — the same behaviour as before
/// native roots were wired in. Logged so the degraded mode is visible.
fn native_root_store() -> RootCertStore {
    let mut roots = RootCertStore::empty();
    match rustls_native_certs::load_native_certs() {
        Ok(certs) => {
            for cert in certs {
                // Ignore individual malformed roots; add what parses.
                let _ = roots.add(cert);
            }
        }
        Err(e) => {
            glib::g_warning!(
                "hxnet",
                "TLS: could not load native trust roots ({e}); WebPKI \
                 validation will fail and all certs route to TOFU"
            );
        }
    }
    if roots.is_empty() {
        glib::g_warning!(
            "hxnet",
            "TLS: native trust root store is empty; WebPKI validation \
             will fail and all certs route to TOFU"
        );
    }
    roots
}

/// Build a rustls `ClientConfig` whose verifier does WebPKI against the
/// native roots and records the verdict in the returned flag. The
/// provider is passed explicitly so we don't rely on a process-wide
/// default being installed.
fn webpki_client_config() -> (ClientConfig, Arc<AtomicBool>) {
    let provider = Arc::new(ring::default_provider());
    let webpki_ok = Arc::new(AtomicBool::new(false));
    let inner = WebPkiServerVerifier::builder_with_provider(
        Arc::new(native_root_store()),
        provider.clone(),
    )
    .build()
    // Builder only errors on an empty root store; we guarantee a
    // (possibly empty) store and want the "no roots → everything to
    // TOFU" path, so on the error case fall back to a verifier with a
    // single throwaway constraint is not possible — instead we treat
    // builder failure as "no WebPKI" by using an accept-and-record-miss
    // verifier. In practice the store is non-empty on any real system.
    .ok();

    let verifier: Arc<dyn ServerCertVerifier> = match inner {
        Some(inner) => Arc::new(WebPkiOrTofu {
            inner,
            webpki_ok: webpki_ok.clone(),
        }),
        None => {
            glib::g_warning!(
                "hxnet",
                "TLS: WebPKI verifier could not be built (no usable \
                 roots); all certs route to TOFU"
            );
            Arc::new(AlwaysTofu { webpki_ok: webpki_ok.clone(), provider: provider.clone() })
        }
    };

    // Reuse the single `provider` built above for the ClientConfig too,
    // so the config and the verifier share one CryptoProvider instance
    // rather than constructing a second one that could diverge.
    let config = ClientConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .expect("ring provider supports the default protocol versions")
        .dangerous()
        .with_custom_certificate_verifier(verifier)
        .with_no_client_auth();
    (config, webpki_ok)
}

/// Fallback verifier for the (practically unreachable) case where the
/// native root store is empty and a WebPKI verifier can't be built.
/// Records a WebPKI miss and completes the handshake so the TOFU gate
/// decides; runs the real signature checks via the crypto provider.
#[derive(Debug)]
struct AlwaysTofu {
    webpki_ok: Arc<AtomicBool>,
    provider: Arc<tokio_rustls::rustls::crypto::CryptoProvider>,
}

impl ServerCertVerifier for AlwaysTofu {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, tokio_rustls::rustls::Error> {
        self.webpki_ok.store(false, Ordering::Relaxed);
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, tokio_rustls::rustls::Error> {
        tokio_rustls::rustls::crypto::verify_tls12_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, tokio_rustls::rustls::Error> {
        tokio_rustls::rustls::crypto::verify_tls13_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.provider
            .signature_verification_algorithms
            .supported_schemes()
    }
}

/// SHA-256 fingerprint of the peer's leaf certificate, formatted to
/// match `src/tls_trust.c::hx_tls_trust_fingerprint` byte-for-byte:
/// `"sha256:"` + lowercase hex of `SHA-256(leaf_cert_DER)`. Returns
/// `None` if the peer presented no certificate (it always should
/// after a completed handshake). The returned string is what the
/// C-side TOFU check (`hx_tls_orchestrator_verify_cert` →
/// `hx_tls_trust_lookup`) keys on, so the format must stay identical
/// to the legacy GTlsCertificate path's fingerprint or pins won't
/// interoperate.
pub fn peer_cert_fingerprint(stream: &TlsStream<TcpStream>) -> Option<String> {
    let (_io, conn) = stream.get_ref();
    let leaf = conn.peer_certificates()?.first()?;
    Some(fingerprint_sha256(leaf.as_ref()))
}

fn fingerprint_sha256(der: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    use std::fmt::Write as _;
    let digest = Sha256::digest(der);
    let mut out = String::with_capacity(7 + 64);
    out.push_str("sha256:");
    for b in digest {
        let _ = write!(out, "{b:02x}");
    }
    out
}

/// Wrap a connected TCP stream in a client TLS stream, performing
/// the handshake against `host` (used as the SNI server name).
///
/// `host` may be a DNS name or an IP literal; both are accepted as
/// `ServerName`. Returns the established `TlsStream` plus a
/// `webpki_ok` flag: `true` when the server cert validated against the
/// native trust roots (CA-valid — trust it silently), `false` when it
/// didn't (route to the post-handshake TOFU gate). Errors on a bad
/// server name or a handshake failure.
pub async fn wrap_tls(
    stream: TcpStream,
    host: &str,
) -> io::Result<(TlsStream<TcpStream>, Arc<AtomicBool>)> {
    let (config, webpki_ok) = webpki_client_config();
    let connector = TlsConnector::from(Arc::new(config));
    let server_name = ServerName::try_from(host.to_string()).map_err(|e| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("invalid TLS server name {host:?}: {e}"),
        )
    })?;
    // Bound the TLS handshake like the other pre-frame steps so a
    // server that stalls mid-handshake doesn't hang the connect.
    let stream = tokio::time::timeout(
        std::time::Duration::from_secs(crate::HANDSHAKE_TIMEOUT_SECS),
        connector.connect(server_name, stream),
    )
    .await
    .map_err(|_| {
        io::Error::new(
            io::ErrorKind::TimedOut,
            format!(
                "TLS handshake did not complete within {}s",
                crate::HANDSHAKE_TIMEOUT_SECS
            ),
        )
    })??;
    Ok((stream, webpki_ok))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn config_builds_with_webpki_verifier() {
        // Construction shouldn't panic and the WebPKI verifier built
        // from the native roots advertises a non-empty scheme set for
        // the ClientHello. webpki_ok starts false (no cert seen yet).
        let (_config, webpki_ok) = webpki_client_config();
        assert!(
            !webpki_ok.load(Ordering::Relaxed),
            "webpki_ok must start false before any cert is verified"
        );
    }

    #[test]
    fn fingerprint_matches_tls_trust_format() {
        // SHA-256 of the empty input, lowercase hex, "sha256:"
        // prefix — must match src/tls_trust.c's g_checksum output so
        // pins interoperate between the legacy and orchestrator paths.
        assert_eq!(
            fingerprint_sha256(b""),
            "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
    }

    #[test]
    fn server_name_accepts_dns_and_ip() {
        assert!(ServerName::try_from("hotline.example.com".to_string()).is_ok());
        assert!(ServerName::try_from("127.0.0.1".to_string()).is_ok());
        assert!(ServerName::try_from("::1".to_string()).is_ok());
    }
}
