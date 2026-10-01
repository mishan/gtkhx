//! End-to-end tests for the Connection actor using
//! `tokio::io::duplex` in-memory pairs.
//!
//! Strategy: each test builds a duplex pair `(client_side,
//! server_side)`. The Connection actor takes `client_side`; the
//! test writes scripted Hotline bytes onto `server_side` to drive
//! the read path, and reads what the actor writes back to verify
//! the send path.
//!
//! Hotline header layout (big-endian, 22 bytes):
//!   u32 type
//!   u32 trans
//!   u32 flag
//!   u32 len    — body bytes + sizeof(hc)
//!   u32 len2   — duplicate of len
//!   u16 hc     — chunk count
//!
//! Tests construct frames by hand because hxnet doesn't have a
//! frame-builder yet — that lives in hxproto's `build`
//! module, and pulling it in for tests just to construct test
//! fixtures isn't worth the layering. Hand-rolled 22-byte
//! arrays are easier to read.

use hxnet::{Command, Connection, Event, Frame, ShutdownReason};
use tokio::io::{AsyncReadExt, AsyncWriteExt};

/// Build a header byte sequence for a frame with `body_len` body
/// bytes following.
fn build_header(type_: u32, trans: u32, flag: u32, body_len: u32, hc: u16) -> [u8; 22] {
    // Wire `len` is body_len + sizeof(hc=2). hc itself is INSIDE
    // the header bytes (positions 20-21); body_len here counts the
    // bytes AFTER the 22-byte header.
    let wire_len = body_len + 2;
    let mut buf = [0u8; 22];
    buf[0..4].copy_from_slice(&type_.to_be_bytes());
    buf[4..8].copy_from_slice(&trans.to_be_bytes());
    buf[8..12].copy_from_slice(&flag.to_be_bytes());
    buf[12..16].copy_from_slice(&wire_len.to_be_bytes());
    buf[16..20].copy_from_slice(&wire_len.to_be_bytes()); // len2 mirrors len
    buf[20..22].copy_from_slice(&hc.to_be_bytes());
    buf
}

/// Like [`build_header`] but with a DISTINCT wire `len` (TotalSize, at
/// offset 12) and `len2` (DataSize, at offset 16). Both are wire values
/// (body bytes + sizeof(hc)=2). Servers set TotalSize > DataSize when a
/// transaction's whole-message size exceeds what a single frame carries;
/// the read path must frame the stream by DataSize (len2), this frame's
/// actual size.
fn build_header_split(
    type_: u32,
    trans: u32,
    flag: u32,
    total_wire: u32,
    data_wire: u32,
    hc: u16,
) -> [u8; 22] {
    let mut buf = [0u8; 22];
    buf[0..4].copy_from_slice(&type_.to_be_bytes());
    buf[4..8].copy_from_slice(&trans.to_be_bytes());
    buf[8..12].copy_from_slice(&flag.to_be_bytes());
    buf[12..16].copy_from_slice(&total_wire.to_be_bytes()); // len / TotalSize
    buf[16..20].copy_from_slice(&data_wire.to_be_bytes()); // len2 / DataSize
    buf[20..22].copy_from_slice(&hc.to_be_bytes());
    buf
}

#[tokio::test]
async fn reads_a_single_frame() {
    let (mut server, client) = tokio::io::duplex(4096);

    let (_handle, mut events, _join) =
        Connection::spawn(client).expect("spawn under tokio runtime");

    // Write one header + 8 body bytes onto the server side.
    let hdr = build_header(0x69, 1, 0, 8, 0);
    server.write_all(&hdr).await.unwrap();
    server.write_all(b"hello!\0\0").await.unwrap();

    let evt = events.recv().await.expect("event channel open");
    let frame = match evt {
        Event::Frame(f) => f,
        e => panic!("unexpected event: {e:?}"),
    };

    assert_eq!(frame.header.type_, 0x69);
    assert_eq!(frame.header.trans, 1);
    assert_eq!(frame.header.flag, 0);
    assert_eq!(frame.header.body_len, 8);
    assert_eq!(&frame.body, b"hello!\0\0");
}

#[tokio::test]
async fn reads_two_frames_in_sequence() {
    let (mut server, client) = tokio::io::duplex(4096);
    let (_handle, mut events, _join) =
        Connection::spawn(client).expect("spawn under tokio runtime");

    // Two frames back-to-back.
    let hdr1 = build_header(0x65, 10, 0, 4, 0);
    let hdr2 = build_header(0x66, 11, 0, 4, 0);
    server.write_all(&hdr1).await.unwrap();
    server.write_all(b"aaaa").await.unwrap();
    server.write_all(&hdr2).await.unwrap();
    server.write_all(b"bbbb").await.unwrap();

    let f1 = match events.recv().await.unwrap() {
        Event::Frame(f) => f,
        e => panic!("unexpected: {e:?}"),
    };
    let f2 = match events.recv().await.unwrap() {
        Event::Frame(f) => f,
        e => panic!("unexpected: {e:?}"),
    };

    assert_eq!(f1.header.trans, 10);
    assert_eq!(&f1.body, b"aaaa");
    assert_eq!(f2.header.trans, 11);
    assert_eq!(&f2.body, b"bbbb");
}

/// Regression (the MacSecret / large-reply desync): a frame whose wire
/// `len` (TotalSize, offset 12) is LARGER than its `len2` (DataSize,
/// offset 16 — this frame's actual byte count) must be framed by DataSize.
/// The read loop reads exactly this frame's body and stays aligned for the
/// following frame. If it (re)regresses to framing by TotalSize it
/// over-reads past the boundary — blocking forever waiting for bytes that
/// never come, and desyncing the stream — so a second, ordinary frame
/// right after would never arrive intact. The `timeout`s turn that
/// regression into a fast failure instead of a hang.
/// The next event, which must be a frame and must come promptly.
async fn next_frame(events: &mut tokio::sync::mpsc::Receiver<Event>, what: &str) -> hxnet::Frame {
    match tokio::time::timeout(std::time::Duration::from_secs(5), events.recv())
        .await
        .unwrap_or_else(|_| panic!("{what} timely"))
        .expect("event channel open")
    {
        Event::Frame(f) => f,
        e => panic!("{what}: unexpected {e:?}"),
    }
}

#[tokio::test]
async fn a_split_transaction_is_joined_and_the_stream_stays_aligned() {
    let (mut server, client) = tokio::io::duplex(4096);
    let (_handle, mut events, _join) =
        Connection::spawn(client).expect("spawn under tokio runtime");

    // One news reply of a single 994-byte field, cut in two the way a
    // fragmenting server sends it: TotalSize (offset 12) repeated in
    // each fragment, each with its own DataSize (offset 16), and only the
    // first holding the field count.
    let mut data = Vec::new();
    data.extend_from_slice(&1u16.to_be_bytes()); // field count
    data.extend_from_slice(&0x65u16.to_be_bytes());
    data.extend_from_slice(&994u16.to_be_bytes());
    data.extend(std::iter::repeat_n(b'n', 994));
    let total = data.len() as u32; // 1000
    let fragment = |chunk: &[u8]| {
        let mut f = Vec::new();
        f.extend_from_slice(&0x0001_0000u32.to_be_bytes());
        f.extend_from_slice(&10u32.to_be_bytes());
        f.extend_from_slice(&0u32.to_be_bytes());
        f.extend_from_slice(&total.to_be_bytes());
        f.extend_from_slice(&(chunk.len() as u32).to_be_bytes());
        f.extend_from_slice(chunk);
        f
    };
    server.write_all(&fragment(&data[..300])).await.unwrap();
    // Another transaction between the fragments goes through on its own,
    // and arrives intact only if the first fragment took exactly its
    // DataSize off the stream.
    server
        .write_all(&build_header(0x66, 11, 0, 4, 0))
        .await
        .unwrap();
    server.write_all(b"bbbb").await.unwrap();
    server.write_all(&fragment(&data[300..])).await.unwrap();

    let between = next_frame(&mut events, "the frame between").await;
    assert_eq!(between.header.trans, 11);
    assert_eq!(&between.body, b"bbbb");
    let joined = next_frame(&mut events, "the joined transaction").await;
    assert_eq!(joined.header.trans, 10);
    assert_eq!(joined.header.hc, 1);
    assert_eq!(joined.body, data[2..], "every fragment's bytes, in order");
}

#[tokio::test]
async fn a_whole_frame_with_an_overstated_totalsize_is_read_as_it_is() {
    let (mut server, client) = tokio::io::duplex(4096);
    let (_handle, mut events, _join) =
        Connection::spawn(client).expect("spawn under tokio runtime");

    // TotalSize claims more than the frame, but its one field fills it:
    // a server miscounting, not a split. It is delivered as the frame it
    // is, as GtkHx always read it.
    let mut body = Vec::new();
    body.extend_from_slice(&0x65u16.to_be_bytes());
    body.extend_from_slice(&2u16.to_be_bytes());
    body.extend_from_slice(b"hi");
    let hdr = build_header_split(0x6a, 0, 0, 1000 + 2, body.len() as u32 + 2, 1);
    server.write_all(&hdr).await.unwrap();
    server.write_all(&body).await.unwrap();
    let f = next_frame(&mut events, "the frame").await;
    assert_eq!(f.body, body);
}

#[tokio::test]
async fn handles_partial_reads_across_header_and_body_boundaries() {
    // Adversarial: deliver the header one byte at a time, then
    // the body one byte at a time. The actor's read loop must
    // assemble both.
    let (mut server, client) = tokio::io::duplex(4096);
    let (_handle, mut events, _join) =
        Connection::spawn(client).expect("spawn under tokio runtime");

    let hdr = build_header(0x69, 42, 0, 4, 0);
    let body = b"abcd";

    for &b in &hdr {
        server.write_all(&[b]).await.unwrap();
        server.flush().await.unwrap();
        tokio::task::yield_now().await;
    }
    for &b in body {
        server.write_all(&[b]).await.unwrap();
        server.flush().await.unwrap();
        tokio::task::yield_now().await;
    }

    let frame = match events.recv().await.unwrap() {
        Event::Frame(f) => f,
        e => panic!("unexpected: {e:?}"),
    };
    assert_eq!(frame.header.trans, 42);
    assert_eq!(&frame.body, b"abcd");
}

#[tokio::test]
async fn write_command_lands_on_the_wire() {
    let (mut server, client) = tokio::io::duplex(4096);
    let (handle, _events, _join) = Connection::spawn(client).expect("spawn under tokio runtime");

    // Build a complete frame as bytes and ship it as one command.
    let mut payload = Vec::new();
    payload.extend_from_slice(&build_header(0x6b, 99, 0, 5, 0));
    payload.extend_from_slice(b"login");

    handle
        .send(Command::WriteFrame(payload.clone()))
        .await
        .expect("send succeeds");

    let mut received = vec![0u8; payload.len()];
    server.read_exact(&mut received).await.unwrap();
    assert_eq!(received, payload);
}

#[tokio::test]
async fn eof_emits_shutdown_event_then_closes() {
    let (server, client) = tokio::io::duplex(4096);

    let (_handle, mut events, join) = Connection::spawn(client).expect("spawn under tokio runtime");

    // Close the server side without writing anything. The actor
    // sees EOF on its very first read attempt.
    drop(server);

    let final_evt = events.recv().await.expect("Shutdown event arrives");
    assert!(
        matches!(final_evt, Event::Shutdown(ShutdownReason::Eof)),
        "expected Shutdown(Eof), got {final_evt:?}"
    );
    // After the Shutdown event, the channel closes.
    assert!(events.recv().await.is_none());
    join.await.expect("actor task completes");
}

#[tokio::test]
async fn mid_frame_eof_is_stream_error_not_clean_eof() {
    let (mut server, client) = tokio::io::duplex(4096);
    let (_handle, mut events, _join) =
        Connection::spawn(client).expect("spawn under tokio runtime");

    // Write 5 bytes of a header then close.
    server.write_all(&[1, 2, 3, 4, 5]).await.unwrap();
    drop(server);

    let evt = events.recv().await.expect("Shutdown event arrives");
    match evt {
        Event::Shutdown(ShutdownReason::StreamError(msg)) => {
            assert!(
                msg.contains("mid-frame"),
                "stream error should name the truncation site: {msg}"
            );
        }
        e => panic!("expected StreamError, got {e:?}"),
    }
}

#[tokio::test]
async fn oversized_frame_is_rejected_with_shutdown() {
    let (mut server, client) = tokio::io::duplex(4096);
    let (_handle, mut events, _join) =
        Connection::spawn(client).expect("spawn under tokio runtime");

    // Build a header claiming a 64 MiB body. The actor MUST
    // refuse to allocate.
    let bad_wire_len: u32 = (hxnet::MAX_BODY_LEN + 1) + 2; // body + hc
    let mut hdr = [0u8; 22];
    hdr[0..4].copy_from_slice(&0x69u32.to_be_bytes());
    hdr[4..8].copy_from_slice(&1u32.to_be_bytes());
    hdr[8..12].copy_from_slice(&0u32.to_be_bytes());
    hdr[12..16].copy_from_slice(&bad_wire_len.to_be_bytes());
    hdr[16..20].copy_from_slice(&bad_wire_len.to_be_bytes());
    hdr[20..22].copy_from_slice(&0u16.to_be_bytes());
    server.write_all(&hdr).await.unwrap();

    let evt = events.recv().await.expect("Shutdown event arrives");
    match evt {
        Event::Shutdown(ShutdownReason::FrameTooLarge { wire_len }) => {
            assert_eq!(wire_len, bad_wire_len);
        }
        e => panic!("expected FrameTooLarge, got {e:?}"),
    }
}

#[tokio::test]
async fn explicit_shutdown_command_exits_the_actor() {
    let (_server, client) = tokio::io::duplex(4096);
    let (handle, mut events, join) = Connection::spawn(client).expect("spawn under tokio runtime");

    handle
        .send(Command::Shutdown)
        .await
        .expect("shutdown command goes through");

    let evt = events.recv().await.expect("Shutdown event arrives");
    assert!(
        matches!(evt, Event::Shutdown(ShutdownReason::HandleDropped)),
        "explicit Shutdown command becomes HandleDropped reason: got {evt:?}"
    );
    join.await.expect("actor task completes");
}

#[tokio::test]
async fn dropping_all_handles_exits_the_actor() {
    let (_server, client) = tokio::io::duplex(4096);
    let (handle, mut events, join) = Connection::spawn(client).expect("spawn under tokio runtime");

    let clone1 = handle.clone();
    let clone2 = clone1.clone();
    drop(handle);
    drop(clone1);
    drop(clone2);

    let evt = events.recv().await.expect("Shutdown event arrives");
    assert!(
        matches!(evt, Event::Shutdown(ShutdownReason::HandleDropped)),
        "no surviving sender → HandleDropped: got {evt:?}"
    );
    join.await.expect("actor task completes");
}

#[tokio::test]
async fn shutdown_helper_is_best_effort_noop_on_closed_actor() {
    let (server, client) = tokio::io::duplex(4096);
    let (handle, mut events, join) = Connection::spawn(client).expect("spawn under tokio runtime");

    drop(server); // actor sees EOF immediately
    let _ = events.recv().await; // drain Shutdown(Eof)
    join.await.expect("actor exits");

    // Calling shutdown after the actor has exited must be a
    // no-op, not a panic. The handle still exists but the receive
    // side of its channel is gone.
    handle.shutdown();
}

#[tokio::test]
async fn try_send_returns_full_when_command_channel_is_saturated() {
    // Capacity-1 command channel. We don't poll events (and don't
    // attach a server) so the actor parks on its first write.
    let (_server, client) = tokio::io::duplex(4096);
    let (handle, _events, _join) =
        Connection::spawn_with_capacities(client, 1, 8).expect("spawn under tokio runtime");

    // First command parks the actor's write loop (no reader on the
    // other side of `_server`, server buffer eventually fills).
    // The command channel itself holds 1 item; the second try_send
    // either succeeds (if the actor already drained item 1 in the
    // meantime) or returns Full. Either is acceptable behaviour;
    // we just want to confirm try_send NEVER blocks and never
    // panics.
    let _ = handle.try_send(Command::WriteFrame(vec![0u8; 1024]));
    let r = handle.try_send(Command::WriteFrame(vec![0u8; 1024]));
    // Both Ok and Err(TrySendError::Full) are valid here — the
    // schedule is racy. We just don't tolerate a panic or a hang.
    match r {
        Ok(_) | Err(tokio::sync::mpsc::error::TrySendError::Full(_)) => {}
        Err(other) => panic!("unexpected try_send error: {other:?}"),
    }
}

#[tokio::test]
async fn frame_helper_constructor_round_trips_header_fields() {
    // Doc-style sanity check: hand-build a Header via the proto
    // crate, wrap it, body it, and re-extract.
    let header = hxproto::parse::decode_header_full(&build_header(0x6c, 7, 1, 3, 0), 4096)
        .expect("test header decodes");
    let f = Frame::new(header, b"abc".to_vec());
    assert_eq!(f.header.type_, 0x6c);
    assert_eq!(f.header.trans, 7);
    assert_eq!(f.header.flag, 1);
    assert!(f.header.in_error_bit_check());
    assert_eq!(f.body, b"abc");
}

// Sanity check on HeaderDecoded::in_error semantics — surfaced
// as a free function for clarity (we don't actually have a
// method by that name; the bit is just flag&1). Renamed locally
// so the test reads naturally.
trait InErrorBitCheck {
    fn in_error_bit_check(&self) -> bool;
}
impl InErrorBitCheck for hxproto::parse::HeaderDecoded {
    fn in_error_bit_check(&self) -> bool {
        self.flag & 1 != 0
    }
}

/// A frame that arrives in pieces must survive a command sent while it is
/// half read. The actor races each frame read against the command queue;
/// a read that lost the race used to be dropped with the bytes it had
/// already taken, so the rest of the body was read as the next header.
#[tokio::test]
async fn a_command_mid_frame_does_not_lose_the_frame() {
    let (mut server, client) = tokio::io::duplex(64 * 1024);
    let (handle, mut events, _join) = Connection::spawn(client).expect("spawn under tokio runtime");

    let body: Vec<u8> = (0..4000u32).map(|i| i as u8).collect();
    let hdr = build_header(0x69, 1, 0, body.len() as u32, 0);

    for split in [5usize, 22, 23, 1000] {
        // Part of the frame: some of the header, or the header and part of
        // the body. Let the actor start reading it.
        let whole: Vec<u8> = hdr.iter().copied().chain(body.iter().copied()).collect();
        server.write_all(&whole[..split]).await.unwrap();
        tokio::time::sleep(std::time::Duration::from_millis(20)).await;

        // A command lands mid-frame; it goes out while the read waits.
        let out = build_header(0x6a, 2, 0, 0, 0).to_vec();
        handle.send(Command::WriteFrame(out.clone())).await.unwrap();
        let mut echoed = vec![0u8; out.len()];
        server.read_exact(&mut echoed).await.unwrap();
        assert_eq!(echoed, out, "the command went out");

        // The rest of the frame, then one more whole frame after it.
        server.write_all(&whole[split..]).await.unwrap();
        let next = build_header(0x6b, 3, 0, 4, 0);
        server.write_all(&next).await.unwrap();
        server.write_all(b"next").await.unwrap();

        for (want_type, want_body) in [(0x69u32, &body[..]), (0x6b, &b"next"[..])] {
            let evt = tokio::time::timeout(std::time::Duration::from_secs(2), events.recv())
                .await
                .unwrap_or_else(|_| panic!("split {split}: no frame"))
                .expect("event channel open");
            match evt {
                Event::Frame(f) => {
                    assert_eq!(f.header.type_, want_type, "split {split}: frame type");
                    assert_eq!(f.body, want_body, "split {split}: frame body");
                }
                other => panic!("split {split}: expected a frame, got {other:?}"),
            }
        }
    }
}

/// The connection must read while it writes. Both sides here send a frame
/// larger than the pipe between them, and neither reads until its own
/// write is done — so if the actor stops reading while it writes a
/// command, neither write ever finishes.
#[tokio::test]
async fn a_large_command_goes_out_while_a_large_frame_comes_in() {
    let (mut server, client) = tokio::io::duplex(1024);
    let (handle, mut events, _join) = Connection::spawn(client).expect("spawn under tokio runtime");

    let big: Vec<u8> = (0..256 * 1024u32).map(|i| i as u8).collect();
    let mut command = build_header(0x70, 9, 0, big.len() as u32, 0).to_vec();
    command.extend_from_slice(&big);
    handle
        .send(Command::WriteFrame(command.clone()))
        .await
        .unwrap();
    tokio::time::sleep(std::time::Duration::from_millis(20)).await;

    let incoming = big.clone();
    let expect = command.len();
    let server_side = tokio::spawn(async move {
        server
            .write_all(&build_header(0x71, 10, 0, incoming.len() as u32, 0))
            .await
            .unwrap();
        server.write_all(&incoming).await.unwrap();
        let mut got = vec![0u8; expect];
        server.read_exact(&mut got).await.unwrap();
        got
    });

    let evt = tokio::time::timeout(std::time::Duration::from_secs(5), events.recv())
        .await
        .expect("the frame came in while the command went out")
        .expect("event channel open");
    match evt {
        Event::Frame(f) => {
            assert_eq!(f.header.type_, 0x71);
            assert_eq!(f.body, big);
        }
        other => panic!("expected a frame, got {other:?}"),
    }
    let got = tokio::time::timeout(std::time::Duration::from_secs(5), server_side)
        .await
        .expect("the command went out")
        .unwrap();
    assert_eq!(got, command);
}

/// A command must go out while the consumer is slow to take frames: the
/// actor waiting for room in the event channel mustn't stop it writing.
#[tokio::test]
async fn a_command_goes_out_while_the_consumer_lags() {
    let (mut server, client) = tokio::io::duplex(64 * 1024);
    let (handle, _events, _join) =
        Connection::spawn_with_capacities(client, 8, 1).expect("spawn under tokio runtime");

    // More frames than the event channel holds, none of them taken.
    for i in 0..4u32 {
        server
            .write_all(&build_header(0x72, i, 0, 4, 0))
            .await
            .unwrap();
        server.write_all(b"full").await.unwrap();
    }
    tokio::time::sleep(std::time::Duration::from_millis(20)).await;

    let out = build_header(0x73, 11, 0, 0, 0).to_vec();
    handle.send(Command::WriteFrame(out.clone())).await.unwrap();
    let mut got = vec![0u8; out.len()];
    tokio::time::timeout(
        std::time::Duration::from_secs(2),
        server.read_exact(&mut got),
    )
    .await
    .expect("the command went out")
    .unwrap();
    assert_eq!(got, out);
}
