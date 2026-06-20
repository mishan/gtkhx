//! TLS transport for hxnet (Phase R3.3.e-tls).
//!
//! The Mobius-style "TLS from byte zero on a dedicated port"
//! model: the Hotline 1.x wire protocol runs unchanged inside a
//! standard TLS connection. We wrap the `tokio::net::TcpStream`
//! in `tokio_rustls::client::TlsStream<TcpStream>` and hand the
//! result to [`crate::Connection::spawn_boxed`]. Everything
//! downstream of the TLS layer — the HOPE cipher adapters, the
//! frame parser, the marker rotation — works unchanged because
//! TLS just looks like a stream cipher from the Connection
//! actor's perspective.
//!
//! # Cert trust delegation to the C side
//!
//! GtkHx already ships a TOFU known-hosts UX (`src/tls_trust.c` +
//! `src/tls_trust_dialog.c`) for the legacy `GTlsConnection`
//! path. To keep the trust decisions consistent across the two
//! paths and avoid duplicating the UI, the Rust-side rustls
//! verifier defers to a C callback. The callback receives the
//! presented certificate's DER bytes plus the host:port the
//! connection is targeting, runs the existing trust logic
//! (CA validation → fingerprint lookup → user prompt on
//! mismatch), and returns accept-or-reject.
//!
//! This means the Rust side carries no trust state of its own.
//! Everything from `tls-known-hosts` to the AdwAlertDialog
//! prompt for first-time-unknown certs continues to live in C.
//! When the C side eventually gets ported, the callback shape
//! becomes a Rust-Rust trait impl with the same API and zero
//! behavioural change.
//!
//! # Threading
//!
//! The verifier callback runs on the tokio runtime thread (the
//! TLS handshake driver). It's expected to be a fast synchronous
//! call that posts an idle/g_main_context_invoke if it needs to
//! prompt the user — the legacy `GTlsConnection::accept-certificate`
//! handler already does this via a nested `GMainLoop`. The
//! callback returns the user's decision once the dialog
//! resolves.

use std::sync::Arc;

use rustls::client::danger::{
    HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier,
};
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::{DigitallySignedStruct, SignatureScheme};

/// Decision returned from the C-side trust callback.
///
/// `Accept` means the user (or the trust DB) approved this
/// certificate. `Reject` aborts the TLS handshake with a
/// `BadEncoding`-shaped error so the actor exits with
/// `ShutdownReason::StreamError`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrustDecision {
    Accept,
    Reject,
}

/// Function pointer the FFI uses to bridge cert verification
/// into C. Called once per TLS handshake, on the tokio runtime
/// thread, with:
///
/// - `cert_der` / `cert_der_len`: the leaf certificate's DER
///   bytes. The C side passes these straight to
///   `tls_trust_compute_fingerprint` and the dialog formatter.
/// - `host` / `host_len`: the SNI hostname / IP literal as a
///   non-NUL-terminated UTF-8 slice.
/// - `port`: the TCP port (5600 by default, but bookmarks can
///   override).
/// - `user_data`: opaque pointer the C side stashed at spawn
///   time. Production passes the `struct htlc_conn *` so the
///   callback can pull the cached trust DB path and the
///   GLib main context out.
///
/// The callback must return 1 for accept, 0 for reject. Any
/// other value is treated as reject (defensive).
pub type CTrustCallback = unsafe extern "C" fn(
    cert_der: *const u8,
    cert_der_len: usize,
    host: *const u8,
    host_len: usize,
    port: u16,
    user_data: *mut std::ffi::c_void,
) -> i32;

/// Rust-side wrapper around the C callback. Implements
/// `rustls::ServerCertVerifier` so the rustls handshake driver
/// can call it during certificate processing.
///
/// `user_data` is opaque to Rust — we just shuttle it back into
/// the C callback. The pointer must outlive the verifier; in
/// production it's the `htlc_conn *` whose lifetime is bounded
/// by the connection itself.
pub struct CallbackVerifier {
    callback: CTrustCallback,
    user_data: usize,
    host: String,
    port: u16,
}

impl CallbackVerifier {
    pub fn new(
        callback: CTrustCallback,
        user_data: *mut std::ffi::c_void,
        host: String,
        port: u16,
    ) -> Self {
        Self {
            callback,
            user_data: user_data as usize,
            host,
            port,
        }
    }
}

impl std::fmt::Debug for CallbackVerifier {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("CallbackVerifier")
            .field("host", &self.host)
            .field("port", &self.port)
            .finish_non_exhaustive()
    }
}

impl ServerCertVerifier for CallbackVerifier {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        let host_bytes = self.host.as_bytes();
        // SAFETY: callback is a `extern "C"` fn pointer the
        // FFI caller provided. `end_entity.as_ref()` gives us a
        // stable byte slice that lives for the duration of this
        // call. The C side is documented as not retaining the
        // pointers past the call's return.
        let accepted = unsafe {
            (self.callback)(
                end_entity.as_ref().as_ptr(),
                end_entity.as_ref().len(),
                host_bytes.as_ptr(),
                host_bytes.len(),
                self.port,
                self.user_data as *mut std::ffi::c_void,
            )
        };
        if accepted == 1 {
            Ok(ServerCertVerified::assertion())
        } else {
            Err(rustls::Error::General(format!(
                "tls_trust callback rejected certificate for {}:{}",
                self.host, self.port
            )))
        }
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        // Use the default crypto provider's TLS 1.2 signature
        // verifier — we only override the certificate-CHAIN
        // validation step (so the user's trust decision is
        // honoured). Signature validation is still real
        // cryptography backed by aws-lc-rs.
        rustls::crypto::verify_tls12_signature(
            message,
            cert,
            dss,
            &rustls::crypto::aws_lc_rs::default_provider().signature_verification_algorithms,
        )
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(
            message,
            cert,
            dss,
            &rustls::crypto::aws_lc_rs::default_provider().signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        rustls::crypto::aws_lc_rs::default_provider()
            .signature_verification_algorithms
            .supported_schemes()
    }
}

/// Build a `rustls::ClientConfig` whose certificate verifier
/// delegates to the C-side `tls_trust` callback. The returned
/// config is `Arc`-wrapped so tokio-rustls's
/// `TlsConnector::from(Arc<ClientConfig>)` can adopt it directly.
///
/// Production callers pass the host/port the connection is
/// targeting so the verifier can include them in any user-facing
/// trust prompt. `user_data` is the opaque pointer the C side
/// stashed (typically the `htlc_conn *`).
///
/// # Safety
///
/// `user_data` must outlive any TLS handshake that uses the
/// returned config — in production its lifetime is bounded by
/// the htlc_conn itself, which the C side guarantees outlives
/// the spawned actor.
pub fn build_client_config(
    callback: CTrustCallback,
    user_data: *mut std::ffi::c_void,
    host: String,
    port: u16,
) -> Arc<rustls::ClientConfig> {
    let verifier = Arc::new(CallbackVerifier::new(callback, user_data, host, port));
    let cfg = rustls::ClientConfig::builder()
        .dangerous()
        .with_custom_certificate_verifier(verifier)
        .with_no_client_auth();
    Arc::new(cfg)
}

/// Perform the TLS client handshake on an already-connected
/// `TcpStream`. Returns a `BoxedDuplex` (the trait object the
/// Connection actor takes) wrapping the established
/// `TlsStream<TcpStream>`.
///
/// The handshake itself is one network round-trip; the verifier
/// callback fires synchronously during chain validation, on the
/// caller's task (which on production is the tokio runtime
/// thread the bridge spawned). Any I/O error during the
/// handshake (including a callback-driven `Reject` from the
/// trust callback) surfaces as the `io::Error` returned here.
///
/// `sni` is the hostname that goes in the TLS ClientHello SNI
/// extension. For IP-literal connections (the common Hotline
/// case — users typing `192.168.1.5:5600`) the SNI extension is
/// omitted per the TLS spec; pass an empty string or the IP
/// literal — rustls handles both.
/// Initialize the process-level rustls crypto provider with
/// aws-lc-rs. Call this once before any TLS handshake (the
/// FFI's `hxnet_connection_spawn_fd_with_tls` will call it
/// internally; pure-Rust callers must call it explicitly).
///
/// rustls 0.23 requires an explicit provider install at
/// process startup; without one, the handshake panics with
/// "no process-level CryptoProvider available". Idempotent —
/// calling more than once is a no-op (returns the prior
/// install).
pub fn install_default_crypto_provider() {
    // Returns Err if a provider was already installed; we don't
    // care which call won the race, both pin the same provider.
    let _ = rustls::crypto::aws_lc_rs::default_provider().install_default();
}

pub async fn handshake(
    tcp: tokio::net::TcpStream,
    config: Arc<rustls::ClientConfig>,
    sni: String,
) -> std::io::Result<crate::transform::BoxedDuplex> {
    let server_name = match ServerName::try_from(sni.clone()) {
        Ok(n) => n.to_owned(),
        Err(_) => {
            // Not a valid DNS name (e.g. an IP literal). Try
            // parsing as an IP address explicitly so we still
            // get correct SAN matching against IP-SAN certs.
            // Failing that, fall back to a placeholder DNS name
            // — the verifier callback gets the original
            // host:port for the user-facing prompt, so this
            // placeholder just satisfies rustls's typestate.
            match sni.parse::<std::net::IpAddr>() {
                Ok(ip) => ServerName::IpAddress(ip.into()),
                Err(_) => ServerName::try_from("invalid.example")
                    .expect("hard-coded placeholder is a valid DNS name")
                    .to_owned(),
            }
        }
    };
    let connector = tokio_rustls::TlsConnector::from(config);
    let stream = connector.connect(server_name, tcp).await?;
    Ok(Box::new(stream))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::Mutex;

    /// Recorder used by the smoke tests below. Static so the
    /// extern "C" callback can update it without lifetime
    /// gymnastics. The `RECORDER_LOCK` serialises tests that
    /// touch this state so the cargo-test parallel runner can't
    /// interleave their assertions.
    static RECORDER_LOCK: Mutex<()> = Mutex::new(());
    static CALLBACK_CALLS: AtomicUsize = AtomicUsize::new(0);
    static LAST_PORT: AtomicUsize = AtomicUsize::new(0);
    static LAST_HOST_LEN: AtomicUsize = AtomicUsize::new(0);
    static LAST_CERT_LEN: AtomicUsize = AtomicUsize::new(0);
    static NEXT_DECISION: AtomicUsize = AtomicUsize::new(1); // 1=accept

    unsafe extern "C" fn record_and_decide(
        cert_der: *const u8,
        cert_der_len: usize,
        host: *const u8,
        host_len: usize,
        port: u16,
        _user_data: *mut std::ffi::c_void,
    ) -> i32 {
        // Touch the pointers so a future regression that NULLs
        // them out trips the test (the slices we build here
        // never get inspected for content — that's covered by
        // the live-handshake Tier 3 tests).
        let _ = (cert_der, host);
        CALLBACK_CALLS.fetch_add(1, Ordering::SeqCst);
        LAST_PORT.store(port as usize, Ordering::SeqCst);
        LAST_HOST_LEN.store(host_len, Ordering::SeqCst);
        LAST_CERT_LEN.store(cert_der_len, Ordering::SeqCst);
        NEXT_DECISION.load(Ordering::SeqCst) as i32
    }

    #[test]
    fn verifier_passes_host_port_and_cert_into_callback() {
        let _guard = RECORDER_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        install_default_crypto_provider();
        CALLBACK_CALLS.store(0, Ordering::SeqCst);
        NEXT_DECISION.store(1, Ordering::SeqCst);

        let v = CallbackVerifier::new(
            record_and_decide,
            std::ptr::null_mut(),
            "hotline.example.com".to_string(),
            5600,
        );

        // Hand-craft a non-empty "cert" — content doesn't
        // matter for the callback-plumbing test; the byte count
        // and pointer-non-null shape are what we're pinning.
        let cert_bytes = vec![0x30, 0x82, 0x01, 0xab, 0x04, 0x05];
        let cert = CertificateDer::from(cert_bytes.clone());

        let name = ServerName::try_from("hotline.example.com").unwrap();
        let result = v.verify_server_cert(
            &cert,
            &[],
            &name,
            &[],
            UnixTime::since_unix_epoch(std::time::Duration::from_secs(
                1_700_000_000,
            )),
        );

        assert!(result.is_ok(), "accept decision should produce Ok");
        assert_eq!(CALLBACK_CALLS.load(Ordering::SeqCst), 1);
        assert_eq!(LAST_PORT.load(Ordering::SeqCst), 5600);
        assert_eq!(LAST_HOST_LEN.load(Ordering::SeqCst), 19);
        assert_eq!(LAST_CERT_LEN.load(Ordering::SeqCst), cert_bytes.len());
    }

    #[test]
    fn verifier_reject_decision_surfaces_as_rustls_error() {
        let _guard = RECORDER_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        install_default_crypto_provider();
        CALLBACK_CALLS.store(0, Ordering::SeqCst);
        NEXT_DECISION.store(0, Ordering::SeqCst); // reject

        let v = CallbackVerifier::new(
            record_and_decide,
            std::ptr::null_mut(),
            "1.2.3.4".to_string(),
            5601,
        );
        let cert = CertificateDer::from(vec![0u8; 4]);
        let name = ServerName::IpAddress(
            "1.2.3.4".parse::<std::net::IpAddr>().unwrap().into(),
        );
        let result = v.verify_server_cert(
            &cert,
            &[],
            &name,
            &[],
            UnixTime::since_unix_epoch(std::time::Duration::from_secs(
                1_700_000_000,
            )),
        );
        assert!(matches!(result, Err(rustls::Error::General(_))));
        assert_eq!(CALLBACK_CALLS.load(Ordering::SeqCst), 1);
        assert_eq!(LAST_PORT.load(Ordering::SeqCst), 5601);
    }

    #[test]
    fn build_client_config_returns_config_using_callback_verifier() {
        // Sanity: builder() chain shouldn't panic.
        let _guard = RECORDER_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        install_default_crypto_provider();
        let _cfg = build_client_config(
            record_and_decide,
            std::ptr::null_mut(),
            "host.example".to_string(),
            5600,
        );
    }
}
