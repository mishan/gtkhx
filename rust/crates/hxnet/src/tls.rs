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
//! # Certificate trust — two phases
//!
//! Trust is enforced in two steps:
//!
//! 1. **Handshake (this module):** the rustls verifier
//!    ([`AcceptAnyServerCert`]) accepts ANY certificate so the TLS
//!    handshake always completes. It performs no identity or chain
//!    check — by design.
//! 2. **Post-handshake (the lifecycle):**
//!    [`peer_cert_fingerprint`] extracts the leaf cert's SHA-256
//!    fingerprint and `run_plaintext_tls_lifecycle` hands it to the
//!    C-side `verify_cert` callback BEFORE any LOGIN. That callback
//!    (`hx_tls_orchestrator_verify_cert` →
//!    `hx_tls_trust_lookup`) runs the same trust-on-first-use (TOFU)
//!    decision the legacy GIOStream path uses: look the fingerprint
//!    up in `$CONFIG/known_hosts`, accept TRUSTED silently, and on
//!    UNKNOWN / MISMATCH prompt the user (marshalled to the GLib main
//!    thread) and pin on accept. A reject closes the stream before
//!    any credentials are sent.
//!
//! So the cert is genuinely untrusted-by-default: a permissive
//! handshake followed by a real TOFU gate, rather than a handshake
//! that silently trusts everything. The split keeps the (possibly
//! blocking, main-thread) trust decision off the handshake's hot
//! path. If `wrap_tls` is used without a post-handshake verify (e.g.
//! the live test probe), the connection is accept-any.

use std::io;
use std::sync::Arc;

use tokio::net::TcpStream;
use tokio_rustls::client::TlsStream;
use tokio_rustls::rustls::client::danger::{
    HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier,
};
use tokio_rustls::rustls::crypto::{ring, CryptoProvider};
use tokio_rustls::rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use tokio_rustls::rustls::{ClientConfig, DigitallySignedStruct, SignatureScheme};
use tokio_rustls::TlsConnector;

/// Certificate verifier that accepts any server certificate.
///
/// This performs no identity or chain validation at handshake time —
/// that is deliberate. The real trust gate is the post-handshake
/// `verify_cert` TOFU callback (see the module docs); a permissive
/// handshake just lets us get the cert in hand to fingerprint it. The
/// handshake-signature checks still run against the presented cert via
/// the crypto provider. Used only on the `GTKHX_NEW_CONNECT`
/// orchestrator TLS path.
#[derive(Debug)]
struct AcceptAnyServerCert {
    provider: Arc<CryptoProvider>,
}

impl ServerCertVerifier for AcceptAnyServerCert {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, tokio_rustls::rustls::Error> {
        // No identity/chain check (TOFU bridge pending).
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

/// Build a rustls `ClientConfig` with the ring provider and the
/// accept-any verifier. The provider is passed explicitly so we
/// don't rely on a process-wide default being installed.
fn accept_any_client_config() -> ClientConfig {
    let provider = Arc::new(ring::default_provider());
    ClientConfig::builder_with_provider(provider.clone())
        .with_safe_default_protocol_versions()
        .expect("ring provider supports the default protocol versions")
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(AcceptAnyServerCert { provider }))
        .with_no_client_auth()
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
/// `ServerName`. Returns the established `TlsStream` ready for the
/// magic exchange, or an `io::Error` on a bad server name or a
/// handshake failure.
pub async fn wrap_tls(
    stream: TcpStream,
    host: &str,
) -> io::Result<TlsStream<TcpStream>> {
    let connector = TlsConnector::from(Arc::new(accept_any_client_config()));
    let server_name = ServerName::try_from(host.to_string()).map_err(|e| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("invalid TLS server name {host:?}: {e}"),
        )
    })?;
    connector.connect(server_name, stream).await
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn config_builds_with_ring_provider() {
        // Construction shouldn't panic (provider supports the
        // default protocol versions) and the verifier advertises a
        // non-empty scheme set.
        let _config = accept_any_client_config();
        let provider = Arc::new(ring::default_provider());
        let v = AcceptAnyServerCert { provider };
        assert!(
            !v.supported_verify_schemes().is_empty(),
            "verifier must advertise signature schemes for the ClientHello"
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
