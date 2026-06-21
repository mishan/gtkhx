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
//! # Certificate trust — first cut
//!
//! **This module currently accepts ANY server certificate.** That
//! matches the *initial* state the legacy GIOStream TLS path shipped
//! in (`docs/tls-scoping.md` Phase 1's accept-everything stub), but
//! it is **not** the trust-on-first-use (TOFU) the legacy path now
//! has via `src/tls_trust.c` + the Adwaita confirmation dialog. The
//! orchestrator TLS path is gated behind `GTKHX_NEW_CONNECT` and is
//! explicitly NOT production-safe until the TOFU bridge lands:
//!
//! - compute the SHA-256 fingerprint of the presented cert's DER,
//! - look it up in the same `known_hosts` store `tls_trust.c` uses,
//! - on UNKNOWN / MISMATCH, prompt the user (which means a callback
//!   from this tokio thread back into the GTK main thread).
//!
//! That bridge is the TLS follow-up (tracked in the Phase G docs).
//! Until it lands, do not flip `GTKHX_NEW_CONNECT` on by default for
//! TLS connections.

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
/// SECURITY: this performs no identity or chain validation — see the
/// module docs. It exists so the orchestrator TLS path can reach
/// feature parity with the legacy path's *transport* (TLS-from-byte-
/// zero) ahead of the TOFU trust bridge. The handshake-signature
/// checks still run against the presented cert via the crypto
/// provider, so a passive observer can't trivially inject — but an
/// active MITM with any cert is accepted. Gated behind
/// `GTKHX_NEW_CONNECT`.
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
    fn server_name_accepts_dns_and_ip() {
        assert!(ServerName::try_from("hotline.example.com".to_string()).is_ok());
        assert!(ServerName::try_from("127.0.0.1".to_string()).is_ok());
        assert!(ServerName::try_from("::1".to_string()).is_ok());
    }
}
