//! Hotline magic exchange (Phase C of
//! `hxnet-owns-the-whole-lifecycle`).
//!
//! Immediately after the TCP (or TLS, in Phase B+) handshake
//! completes, the Hotline 1.x protocol does a fixed
//! handshake-strings exchange:
//!
//!   - client writes 12 bytes — `HTLC_MAGIC` =
//!     `"TRTPHOTL\0\1\0\2"`
//!   - server replies 8 bytes — `HTLS_MAGIC` =
//!     `"TRTP\0\0\0\0"`
//!
//! Any deviation (length mismatch, wrong bytes) is a fatal
//! protocol violation — the server isn't a Hotline 1.x server
//! and there's nothing meaningful to do. We surface that as a
//! `MagicMismatch` shutdown reason so the C side's error dialog
//! can show a useful message rather than the generic "stream
//! error".

use std::io;

use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::sync::mpsc;

use crate::{ConnectionState, Event};

/// `HTLC_MAGIC` — what we send first. 12 bytes.
///
/// The high four bytes are ASCII "TRTP" (matching `HTLS_MAGIC`)
/// followed by "HOTL" + version (1.x.2). The trailing version
/// bytes are big-endian 2-byte fields — `\0\1` = major version
/// 1, `\0\2` = minor version 2. Production hasn't bumped either
/// since the original Hotline 1.x; the constants are pinned for
/// wire compat.
pub const HTLC_MAGIC: &[u8; 12] = b"TRTPHOTL\x00\x01\x00\x02";

/// `HTLS_MAGIC` — what the server replies with. 8 bytes.
///
/// Same "TRTP" prefix; the trailing four bytes are zeroes
/// (legacy unused fields). Any other 8-byte response is treated
/// as protocol violation.
pub const HTLS_MAGIC: &[u8; 8] = b"TRTP\x00\x00\x00\x00";

/// Run the Hotline magic exchange against an established
/// transport. Emits `Event::State(MagicExchange)` before the
/// write begins; the caller is responsible for any subsequent
/// state event (typically `LoginSending` in Phase D).
///
/// Returns `Ok(())` on a clean exchange. Returns `Err(io::Error)`
/// on:
///
///   - inner stream write/read failure (kernel error, EOF mid-magic),
///   - server reply length other than `HTLS_MAGIC.len()`,
///   - server reply bytes don't match `HTLS_MAGIC`.
pub async fn run_magic_exchange<S>(stream: &mut S, evt_tx: &mpsc::Sender<Event>) -> io::Result<()>
where
    S: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin,
{
    if evt_tx
        .send(Event::State(ConnectionState::MagicExchange))
        .await
        .is_err()
    {
        return Err(io::Error::other(
            "consumer dropped before MagicExchange state delivered",
        ));
    }
    stream.write_all(HTLC_MAGIC).await?;
    stream.flush().await?;

    // Bound the server-magic read: a server that accepts the TCP
    // connection but never sends its magic (or hangs) must not wedge
    // the handshake forever. Mirrors the legacy MAGIC_TIMEOUT_SEC.
    let mut reply = [0u8; HTLS_MAGIC.len()];
    tokio::time::timeout(
        std::time::Duration::from_secs(crate::HANDSHAKE_TIMEOUT_SECS),
        stream.read_exact(&mut reply),
    )
    .await
    .map_err(|_| {
        io::Error::new(
            io::ErrorKind::TimedOut,
            format!(
                "server magic not received within {}s",
                crate::HANDSHAKE_TIMEOUT_SECS
            ),
        )
    })??;
    if &reply != HTLS_MAGIC {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "HTLS_MAGIC mismatch: expected {:02x?}, got {:02x?}",
                HTLS_MAGIC, reply
            ),
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::duplex;

    /// Happy path. Stand up a tokio duplex pair, have the
    /// "server" side write HTLS_MAGIC + sink the client's
    /// HTLC_MAGIC, verify the exchange returns Ok and the right
    /// state event fires.
    #[tokio::test]
    async fn run_magic_exchange_happy_path() {
        let (mut client, mut server) = duplex(64);
        let (evt_tx, mut evt_rx) = mpsc::channel(8);

        let server_task = tokio::spawn(async move {
            // Sink the client's 12-byte HTLC_MAGIC.
            let mut buf = [0u8; 12];
            server.read_exact(&mut buf).await.expect("server read");
            assert_eq!(&buf, HTLC_MAGIC, "client should send HTLC_MAGIC");
            // Reply with HTLS_MAGIC.
            server.write_all(HTLS_MAGIC).await.expect("server write");
        });

        run_magic_exchange(&mut client, &evt_tx)
            .await
            .expect("exchange");
        server_task.await.unwrap();

        let evt = evt_rx.recv().await.expect("state event");
        assert!(
            matches!(evt, Event::State(ConnectionState::MagicExchange)),
            "expected MagicExchange state event, got {evt:?}"
        );
    }

    /// Server replies with a wrong magic. Should surface as
    /// io::Error{InvalidData} so the spawn caller can turn it
    /// into a clean ShutdownReason::StreamError with a useful
    /// message.
    #[tokio::test]
    async fn run_magic_exchange_wrong_server_magic() {
        let (mut client, mut server) = duplex(64);
        let (evt_tx, _evt_rx) = mpsc::channel(8);

        let server_task = tokio::spawn(async move {
            let mut buf = [0u8; 12];
            server.read_exact(&mut buf).await.expect("server read");
            // Reply with valid-length but wrong bytes.
            server.write_all(b"WRONG!!!").await.expect("server write");
        });

        let result = run_magic_exchange(&mut client, &evt_tx).await;
        server_task.await.unwrap();

        assert!(result.is_err(), "wrong magic should error");
        let err = result.unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidData);
        let msg = err.to_string();
        assert!(
            msg.contains("HTLS_MAGIC mismatch"),
            "error should mention HTLS_MAGIC mismatch, got: {msg}"
        );
    }

    /// Server closes after receiving HTLC_MAGIC but before
    /// sending HTLS_MAGIC. Read returns 0 bytes mid-read which
    /// AsyncReadExt::read_exact surfaces as
    /// io::ErrorKind::UnexpectedEof.
    #[tokio::test]
    async fn run_magic_exchange_server_eof_mid_reply() {
        let (mut client, mut server) = duplex(64);
        let (evt_tx, _evt_rx) = mpsc::channel(8);

        let server_task = tokio::spawn(async move {
            let mut buf = [0u8; 12];
            server.read_exact(&mut buf).await.expect("server read");
            // Drop without replying. The duplex half-closes the
            // other side once `server` drops.
        });

        let result = run_magic_exchange(&mut client, &evt_tx).await;
        server_task.await.unwrap();

        assert!(result.is_err(), "eof should error");
        let err = result.unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::UnexpectedEof);
    }
}
