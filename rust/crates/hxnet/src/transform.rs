//! Transport-layer transform composition (Phase R3.3.e-2).
//!
//! The [`Connection`](crate::Connection) actor is generic over any
//! `AsyncRead + AsyncWrite + Unpin + Send + 'static` transport. In
//! production the inner transport is a TCP socket, and the HOPE
//! handshake may negotiate one of:
//!
//! - **A cipher** wrapping the socket bytes (Blowfish-OFB-64 for the
//!   legacy HOPE form; ChaCha20-Poly1305 AEAD for the modern form).
//! - **A compression** layer wrapping the cipher's plaintext (GZIP,
//!   LZ4, or ZSTD).
//!
//! Either or both may be absent. R3.3.c shipped the cipher adapters
//! ([`crate::cipher::BlowfishStream`], [`crate::cipher::AeadStream`])
//! and R3.3.d shipped the compression adapters
//! ([`crate::compress::GzipStream`], [`crate::compress::Lz4Stream`],
//! [`crate::compress::ZstdStream`]) — each one wraps any
//! `AsyncRead + AsyncWrite` inner. R3.3.e-2 is about *layering* them
//! correctly and presenting the result as a single type-erased
//! transport the FFI can hand to the Connection actor.
//!
//! # Layering order
//!
//! HOPE's wire shape is **compress, then encrypt**. On the send
//! path the application's plaintext frames go through the compressor
//! first; the compressor's output bytes are what the cipher seals
//! into ciphertext + tag for the wire. Receive path inverts: read
//! ciphertext, decrypt, then decompress. The composition is
//! therefore:
//!
//! ```text
//! send: app  →  compressor  →  cipher  →  TCP
//! recv: app  ←  decompressor ← decipher ← TCP
//! ```
//!
//! Expressed as Rust types from the outside in:
//!
//! ```ignore
//! type Stack = CompressionStream< CipherStream<TcpStream> >;
//! ```
//!
//! The application reads/writes against `Stack`; the compressor's
//! `poll_read`/`poll_write` calls bottom out at the cipher, which
//! bottoms out at the TCP socket. (`CompressionStream` here is
//! whichever of GzipStream/Lz4Stream/ZstdStream the handshake
//! chose; same for `CipherStream`.)
//!
//! # Type erasure: [`AsyncDuplex`]
//!
//! The FFI can't carry the concrete monomorphised stack type
//! across the language boundary, and the Connection actor doesn't
//! need to: it only cares that the inner transport satisfies
//! `AsyncRead + AsyncWrite + Unpin + Send + 'static`.
//!
//! [`AsyncDuplex`] is the marker-trait that bundles those bounds,
//! plus a blanket impl so any conforming type gets it for free.
//! [`BoxedDuplex`] is the boxed trait object the FFI hands to
//! [`Connection::spawn_boxed`](crate::Connection::spawn_boxed).
//!
//! Note on dyn-dispatch cost. The Connection actor calls into the
//! outermost layer through a single vtable per `poll_read` /
//! `poll_write`. When `compose` lays a compression layer on top
//! of a cipher layer, the cipher layer is *itself* boxed before
//! being wrapped, so a poll on the outer compression layer
//! traverses one additional vtable to reach the cipher. The
//! per-poll cost is therefore one indirect call for cipher-only
//! stacks and two for cipher+compress stacks — both negligible
//! against the syscall the poll usually triggers, and constant
//! per polled future.

use tokio::io::{AsyncRead, AsyncWrite};

/// Marker trait combining the bounds the [`Connection`](crate::Connection)
/// actor requires from its transport.
///
/// Any `T: AsyncRead + AsyncWrite + Unpin + Send + 'static` gets
/// this for free via the blanket impl below. The trait exists so
/// `Box<dyn AsyncDuplex>` is expressible as a single trait object
/// — without bundling, `Box<dyn AsyncRead + AsyncWrite + ...>` isn't
/// a legal type because Rust permits at most one non-auto trait per
/// trait object.
///
/// The `'static` bound mirrors `Connection::spawn`'s requirement
/// (the spawned tokio task outlives any local scope, so any
/// non-`'static` reference inside the transport would be UB).
/// Without it the trait's actual contract would be weaker than
/// what `BoxedDuplex` and `Connection::spawn_boxed` actually
/// require.
pub trait AsyncDuplex: AsyncRead + AsyncWrite + Unpin + Send + 'static {}

impl<T> AsyncDuplex for T where T: AsyncRead + AsyncWrite + Unpin + Send + 'static {}

/// Owned, type-erased transport handed to
/// [`Connection::spawn_boxed`](crate::Connection::spawn_boxed). The
/// `'static` bound matches the actor's spawn requirements.
///
/// In practice this is the output of [`compose`] applied to a
/// `TcpStream` after the HOPE handshake resolved which cipher /
/// compression to layer on.
pub type BoxedDuplex = Box<dyn AsyncDuplex>;

/// Tag for the cipher layer requested in a transform stack.
///
/// The configuration that's actually needed to *build* the layer
/// (Blowfish key + ivec, AEAD keys + counter direction) is
/// supplied separately because it carries non-`Copy` data; the tag
/// here just answers "which constructor do we call".
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum CipherKind {
    /// No cipher; the inner transport bytes pass through unchanged.
    None,
    /// Blowfish-OFB-64. The HOPE handshake's legacy form.
    Blowfish,
    /// ChaCha20-Poly1305 length-prefixed AEAD frames. The HOPE
    /// handshake's modern form.
    ChaCha20Poly1305,
}

/// Tag for the compression layer requested in a transform stack.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum CompressionKind {
    /// No compression; the layer is skipped entirely.
    None,
    /// GZIP (RFC 1950 zlib via flate2 with `Sync` flush).
    Gzip,
    /// LZ4F frame format via the `lz4_flex` crate.
    Lz4,
    /// ZSTD via the `zstd` crate's raw streaming API.
    Zstd,
}

/// Convenience description of the full transform stack a Connection
/// should sit behind. Construct one from the result of the HOPE
/// handshake and pass it to whatever entry composes the stack — the
/// FFI in [`crate::ffi`] does this for the C side, and the Rust
/// helpers below take the same shape.
#[derive(Debug, Clone, Copy)]
pub struct TransformStack {
    /// Cipher layer applied directly above the raw transport.
    pub cipher: CipherKind,
    /// Compression layer applied above the cipher.
    pub compression: CompressionKind,
}

impl TransformStack {
    /// Plaintext stack — Connection writes/reads against the inner
    /// transport directly. Equivalent to `(None, None)`.
    pub const PLAINTEXT: Self = Self {
        cipher: CipherKind::None,
        compression: CompressionKind::None,
    };

    /// `true` when neither cipher nor compression is requested.
    /// Callers can short-circuit the box construction in this
    /// case by passing the inner transport straight to
    /// `Connection::spawn`.
    pub fn is_passthrough(&self) -> bool {
        matches!(self.cipher, CipherKind::None) && matches!(self.compression, CompressionKind::None)
    }
}

impl Default for TransformStack {
    fn default() -> Self {
        Self::PLAINTEXT
    }
}

/// Build a [`BoxedDuplex`] by wrapping `inner` with the requested
/// cipher and compression layers. Order is **cipher first,
/// compression on top** — matches the HOPE wire shape described in
/// the module docs.
///
/// This helper is the canonical "I already negotiated the handshake,
/// give me the transport the Connection should sit behind" entry.
/// FFI consumers go through [`crate::ffi`] which calls this; pure-
/// Rust consumers (tests, future direct consumers) can call it
/// directly.
///
/// # Errors
///
/// Returns [`io::Error`] when a layer constructor fails. The only
/// currently-fallible adapter is the ZSTD decoder (the raw
/// `zstd::stream::raw::Decoder::new` call can fail on libzstd
/// initialisation issues). The cipher constructors and Gzip/LZ4
/// adapters are infallible, so non-ZSTD `compose` calls always
/// succeed; the `Result` keeps the surface uniform.
pub fn compose<S>(
    inner: S,
    cipher: CipherLayer,
    compression: CompressionKind,
) -> std::io::Result<BoxedDuplex>
where
    S: AsyncRead + AsyncWrite + Unpin + Send + 'static,
{
    let with_cipher: BoxedDuplex = match cipher {
        CipherLayer::None => Box::new(inner),
        CipherLayer::Blowfish {
            read_state,
            write_state,
        } => Box::new(crate::cipher::BlowfishStream::new(
            inner,
            read_state,
            write_state,
        )),
        CipherLayer::HopeBlowfish {
            read_state,
            read_key,
            write_state,
            write_key,
            session_key,
            macalg,
        } => {
            // Mirror the legacy C send path's
            // `compress_encode_type == COMPRESS_NONE` gate
            // around the HOPE per-message rekey marker
            // (src/cipher.c::cipher_check_rekey_marker). The
            // marker is wire-incompatible with a compression
            // layer on top because pre-spec servers don't
            // expect a marker byte when they negotiated
            // compression on. Read side still detects+strips
            // incoming markers either way — servers that
            // ignore this rule are honored.
            let write_marker_enabled = matches!(compression, CompressionKind::None);
            Box::new(crate::hope_blowfish::HopeBlowfishStream::new(
                inner,
                read_state,
                read_key,
                write_state,
                write_key,
                session_key,
                macalg,
                write_marker_enabled,
            ))
        }
        CipherLayer::ChaCha20Poly1305 { read, write } => {
            Box::new(crate::cipher::AeadStream::new(inner, read, write))
        }
    };
    Ok(match compression {
        CompressionKind::None => with_cipher,
        CompressionKind::Gzip => Box::new(crate::compress::GzipStream::new(with_cipher)),
        CompressionKind::Lz4 => Box::new(crate::compress::Lz4Stream::new(with_cipher)),
        CompressionKind::Zstd => Box::new(crate::compress::ZstdStream::new(with_cipher)?),
    })
}

/// Cipher-layer configuration carried into [`compose`]. Non-`Copy`
/// key material lives inside the relevant variant. [`CipherKind`]
/// above is the matching `Copy` tag for places that only need to
/// *describe* the negotiated layer (logging, FFI shape) without
/// holding key material.
///
/// The `Blowfish` variant is large (~8 KiB — two full Blowfish key
/// schedules), but a `CipherLayer` is built exactly once per
/// connection and consumed immediately by [`compose`], which moves
/// the states out to their long-lived home. There's never an array
/// of these or a hot-path move, so boxing the variant would only buy
/// a setup-time allocation for no real benefit — the size-difference
/// lint is suppressed deliberately.
#[allow(clippy::large_enum_variant)]
pub enum CipherLayer {
    /// No cipher.
    None,
    /// Blowfish-OFB-64. Each direction owns its own
    /// [`hxcrypto_stream::BlowfishOfb64State`]; the HOPE handshake
    /// supplies the key-and-ivec pair that derives these states.
    Blowfish {
        read_state: hxcrypto_stream::BlowfishOfb64State,
        write_state: hxcrypto_stream::BlowfishOfb64State,
    },
    /// HOPE-aware Blowfish-OFB-64. Same OFB primitive as
    /// [`Self::Blowfish`] but the resulting transport carries the
    /// per-message rekey marker logic
    /// ([`crate::hope_blowfish::HopeBlowfishStream`]). The
    /// session key and HMAC algorithm are passed in so the
    /// adapter can run the same HMAC iteration loop the legacy C
    /// `cipher_change_decode_key` runs.
    HopeBlowfish {
        read_state: hxcrypto_stream::BlowfishOfb64State,
        read_key: Vec<u8>,
        write_state: hxcrypto_stream::BlowfishOfb64State,
        write_key: Vec<u8>,
        session_key: Vec<u8>,
        macalg: crate::hope_blowfish::HopeMacAlg,
    },
    /// ChaCha20-Poly1305 with independent per-direction state
    /// (each direction has its own key, counter, and AEAD dir
    /// tag).
    ChaCha20Poly1305 {
        /// State used to decrypt incoming frames.
        read: hxcrypto_aead::AeadState,
        /// State used to encrypt outgoing frames.
        write: hxcrypto_aead::AeadState,
    },
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn passthrough_stack_classification() {
        assert!(TransformStack::PLAINTEXT.is_passthrough());
        assert!(TransformStack::default().is_passthrough());
        assert!(!TransformStack {
            cipher: CipherKind::Blowfish,
            compression: CompressionKind::None,
        }
        .is_passthrough());
        assert!(!TransformStack {
            cipher: CipherKind::None,
            compression: CompressionKind::Gzip,
        }
        .is_passthrough());
    }
}
