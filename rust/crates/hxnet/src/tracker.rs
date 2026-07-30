//! Tracker fetch engine — Phase R3 work item 8, phase T1
//! (`docs/rust/tracker-hxnet-scoping.md`).
//!
//! The per-connection protocol engine that drives one HTRK tracker
//! fetch over an already-connected (optionally TLS-wrapped) byte stream.
//! It replaces the hand-rolled C async state machine in
//! `network.c` (the `tracker_run_ctx` / `tracker_fetch_ctx` chain) — the
//! wire parsing already lives in `hotline-proto::parse`; this module is
//! the transport orchestration that calls those parsers.
//!
//! T1 scope: the per-connection engine + unit tests. No C FFI and no
//! connect/TLS/serial-walk yet — those are T2 (the connect + the walk
//! over `gtkhx_prefs.tracker[]` and the rustls wrap come with the FFI
//! bridge). Here the caller hands us a connected stream and we run one
//! attempt against it.
//!
//! # Protocol (mirrors network.c)
//!
//! Both versions open by writing a handshake and reading a 6-byte
//! response (`"HTRK"` + a `u16` version):
//!
//! - **v3 probe** ([`run_v3_probe`]): write the 8-byte v3 handshake
//!   (`"HTRK"` + `0x0003` + features), then read the 6-byte response
//!   under a watchdog. Real pre-spec v1 trackers `memcmp` the full
//!   `"HTRK\0\1"` magic and ignore the `0x0003` version byte — they send
//!   nothing — so a timeout means "fall back to v1" ([`Outcome::ProbeInconclusive`];
//!   the caller reopens and runs [`run_v1`]). A `0x0003` response reads
//!   the trailing 2 feature bytes and runs the v3 listing flow; a
//!   `0x0001`/`0x0002` response (a spec-compliant tracker that read only
//!   6 bytes) drops into the v1 record flow.
//! - **v1** ([`run_v1`]): write the 6-byte `"HTRK\0\1"` magic, read the
//!   6-byte response (no watchdog — we already know this endpoint), then
//!   the same version dispatch.
//!
//! v1 listing: read the rest of the 14-byte reply header (`nservers` at
//! offset 10), then per server: 8 bytes (IPv4 + port + nusers; a leading
//! zero byte marks a padding slot that's skipped without decrementing
//! the count), 3 bytes (2 reserved + `name_len`), the name, 1 byte
//! `desc_len`, the description.
//!
//! v3 listing: write the 4-byte listing request, read the 10-byte
//! response header (`total_size` + `record_count`), read the capped
//! records blob, walk it record-by-record.

use std::io;
use std::time::Duration;

use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

use hotline_proto::parse::{
    pack_tracker_v3_handshake, pack_tracker_v3_listing_request_simple, parse_tracker_header,
    parse_tracker_record_fixed, parse_tracker_v3_record, parse_tracker_v3_response_header,
    tracker_normalize_text, tracker_record_is_padding, tracker_v3,
};

/// `"HTRK"` magic + `u16` version that opens every tracker response.
const HTRK_MAGIC: [u8; 4] = *b"HTRK";
const HTRK_VERSION_V1: u16 = 0x0001;
const HTRK_VERSION_V2: u16 = 0x0002;
const HTRK_VERSION_V3: u16 = 0x0003;

/// Cap on the v3 records-blob we'll read. Mirror of the C
/// `HX_TRACKER_V3_MAX_PAYLOAD` (16 MiB) — the spec allows a `u32`
/// (4 GiB), but a hostile tracker shouldn't be able to make us allocate
/// + read that much. ~200k servers at ~80 bytes each.
pub const V3_MAX_PAYLOAD: u32 = 16 * 1024 * 1024;

/// Cap on padding slots in a v1 listing. Padding slots (records whose
/// first octet is 0 — an impossible IPv4 first octet) mark deleted-server
/// gaps and don't count against `nservers`, so a tracker that emits an
/// endless stream of them would otherwise hang the reader on unbounded
/// data. Real listings have at most a handful; 65_535 (the `nservers`
/// ceiling) is a generous bound that still terminates a hostile feed.
pub const MAX_V1_PADDING_SLOTS: u32 = 65_535;

/// IPv4 address-type byte (`tracker_v3::ADDR_IPV4`); v1 records are
/// always IPv4, tagged with this for a uniform [`TrackerRecord`] shape.
const ADDR_IPV4: u8 = tracker_v3::ADDR_IPV4;

/// What went wrong during a tracker fetch attempt.
///
/// `#[non_exhaustive]` (like [`crate::SpawnError`]) so new failure modes
/// can be added without a semver break as this becomes a stable surface.
#[derive(Debug)]
#[non_exhaustive]
pub enum TrackerError {
    /// Underlying transport error (not a clean truncation).
    Io(io::Error),
    /// EOF / truncation before a structurally-required field finished.
    ShortRead,
    /// Response didn't open with the `"HTRK"` magic.
    BadMagic,
    /// Response version outside the {v1, v2, v3} we know how to drive.
    UnsupportedVersion(u16),
    /// v3 listing-response header failed to parse (bad response type).
    BadV3Header,
    /// v3 header with exactly one of `total_size` / `record_count` zero
    /// — records promised with no payload, or payload with no records.
    MalformedV3Header { total_size: u32, record_count: u16 },
    /// v3 `total_size` exceeds [`V3_MAX_PAYLOAD`].
    PayloadTooLarge { total_size: u32, cap: u32 },
    /// A v3 record at index `index` of `count` failed to parse.
    MalformedV3Record { index: u16, count: u16 },
    /// v3 records walked cleanly but left `n` trailing bytes — the
    /// declared `record_count` is smaller than the payload's content.
    V3TrailingBytes(usize),
    /// A v1 listing sent more padding slots than [`MAX_V1_PADDING_SLOTS`]
    /// before completing its `nservers` records — a buggy or hostile
    /// tracker dribbling zero-prefixed slots forever. Bail rather than
    /// read unbounded data.
    ExcessivePadding,
}

impl From<io::Error> for TrackerError {
    fn from(e: io::Error) -> Self {
        TrackerError::Io(e)
    }
}

impl std::fmt::Display for TrackerError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            TrackerError::Io(e) => write!(f, "tracker io error: {e}"),
            TrackerError::ShortRead => write!(f, "tracker response truncated"),
            TrackerError::BadMagic => write!(f, "tracker response: bad magic"),
            TrackerError::UnsupportedVersion(v) => {
                write!(f, "tracker response: unsupported HTRK version 0x{v:04x}")
            }
            TrackerError::BadV3Header => write!(f, "tracker: bad v3 response header"),
            TrackerError::MalformedV3Header {
                total_size,
                record_count,
            } => write!(
                f,
                "tracker: malformed v3 header (total_size={total_size}, record_count={record_count})"
            ),
            TrackerError::PayloadTooLarge { total_size, cap } => {
                write!(f, "tracker: v3 response too large ({total_size} bytes, cap {cap})")
            }
            TrackerError::MalformedV3Record { index, count } => {
                // 1-based for humans; widen before +1 so a record_count
                // of u16::MAX can't overflow.
                write!(f, "tracker: malformed v3 record {}/{count}", u32::from(*index) + 1)
            }
            TrackerError::V3TrailingBytes(n) => {
                write!(f, "tracker: {n} trailing bytes after v3 records")
            }
            TrackerError::ExcessivePadding => {
                write!(f, "tracker: too many v1 padding slots")
            }
        }
    }
}

impl std::error::Error for TrackerError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            TrackerError::Io(e) => Some(e),
            _ => None,
        }
    }
}

/// One server record from a tracker listing. Addresses and text are
/// raw wire bytes — the C side transcodes MacRoman → UTF-8 when it
/// builds the `HxTrackerServer` event; the name/desc here have already
/// had the C `tracker_normalize_text` (CR→LF + strip-ANSI) applied,
/// matching the legacy emit.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TrackerRecord {
    /// `0x04` IPv4 / `0x06` IPv6 / `0x48` hostname. v1 is always `0x04`.
    pub addr_type: u8,
    /// IPv4: 4 bytes, network order. IPv6: 16 bytes. Hostname: UTF-8.
    pub address: Vec<u8>,
    pub port: u16,
    pub nusers: u16,
    pub name: Vec<u8>,
    pub desc: Vec<u8>,
    /// 0 for v1 records (no TLV trailer).
    pub tlv_count: u16,
    /// Raw TLV blob; empty for v1.
    pub tlv_bytes: Vec<u8>,
}

/// A completed tracker listing — the batch metadata the view's
/// `tracker-batch-begin` signal carries, plus the records.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TrackerListing {
    /// Record-path shape, not the raw wire-version byte: `1` for the
    /// fixed-record listing flow (served by v1 *and* v2 trackers — a v2
    /// reply is routed through the same record path and reported as `1`),
    /// `3` for the v3 TLV listing flow. Downstream consumers use it to
    /// pick columns (v3 records can carry Country / Caps TLVs; the
    /// `1` path never does), so it reflects the parse shape rather than
    /// distinguishing v1 from v2 on the wire.
    pub version: u8,
    /// Record count the header declared (the batch size the view shows;
    /// equals `records.len()` on a well-formed listing).
    pub expected: u16,
    pub records: Vec<TrackerRecord>,
}

/// Result of a v3 probe attempt. `#[non_exhaustive]` (like
/// [`crate::SpawnError`]) so new probe outcomes can be added without a
/// semver break.
#[derive(Debug)]
#[non_exhaustive]
pub enum Outcome {
    /// The endpoint served a listing (v3, or v1/v2 if it read only the
    /// first 6 handshake bytes).
    Listing(TrackerListing),
    /// The v3 probe was *inconclusive* — the caller should reopen the
    /// connection and run [`run_v1`]. This covers every way an endpoint
    /// can fail to speak v3 at us, not just a watchdog timeout: the
    /// watchdog elapsing with no reply (the classic pre-spec v1 tracker
    /// that ignored the `0x0003` version byte), a short read / connection
    /// close mid-reply, a transport error during the probe read, or a
    /// 6-byte reply that isn't `HTRK` magic (junk from a v1 tracker that
    /// choked on the v3 handshake). Errors that occur *after* a valid
    /// `HTRK` reply is recognised are surfaced as [`TrackerError`]
    /// instead — they aren't fallback triggers.
    ProbeInconclusive,
}

/// Read exactly `buf.len()` bytes, mapping a premature EOF to
/// [`TrackerError::ShortRead`] (a 0-length buf is a no-op).
async fn read_exact_or_short<S: AsyncRead + Unpin>(
    s: &mut S,
    buf: &mut [u8],
) -> Result<(), TrackerError> {
    match s.read_exact(buf).await {
        Ok(_) => Ok(()),
        Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => Err(TrackerError::ShortRead),
        Err(e) => Err(TrackerError::Io(e)),
    }
}

/// Run a v3 probe over `stream`: send the 8-byte v3 handshake, then read
/// the 6-byte response under `probe_timeout`.
///
/// The probe is treated as *inconclusive* — returning [`Outcome::ProbeInconclusive`]
/// so the caller retries with [`run_v1`] on a fresh connection — for every
/// way the endpoint can fail to speak v3 at us:
///
///   - the watchdog elapses with no response (`timeout`);
///   - the peer hangs up or sends a short reply before 6 bytes
///     (`ShortRead` / `Io`); or
///   - it sends 6 bytes that aren't an `HTRK` reply at all (a pre-spec v1
///     tracker that wedged on our v3 magic and dribbled junk).
///
/// This mirrors the legacy C state machine
/// (`network.c::on_tracker_handshake_response`), where all of these route
/// to `tracker_fetch_retry_v1`. Only once a recognisable `HTRK` magic is
/// in hand do we commit to the v3/v1 dispatch and surface its errors hard
/// (an unsupported *version* after valid magic is a real failure, not a
/// fallback trigger — matching the C bail).
pub async fn run_v3_probe<S: AsyncRead + AsyncWrite + Unpin>(
    stream: &mut S,
    features: u16,
    probe_timeout: Duration,
) -> Result<Outcome, TrackerError> {
    let mut hs = [0u8; tracker_v3::HANDSHAKE_LEN];
    // Infallible: HANDSHAKE_LEN-sized buffer.
    let _ = pack_tracker_v3_handshake(&mut hs, features);
    stream.write_all(&hs).await?;
    stream.flush().await?;

    let mut resp6 = [0u8; 6];
    match tokio::time::timeout(probe_timeout, read_exact_or_short(stream, &mut resp6)).await {
        // Watchdog elapsed — no (complete) response in time.
        Err(_elapsed) => return Ok(Outcome::ProbeInconclusive),
        // Short read / connection close / transport error during the probe:
        // the endpoint isn't speaking v3. Inconclusive → fall back to v1.
        Ok(Err(TrackerError::ShortRead)) | Ok(Err(TrackerError::Io(_))) => {
            return Ok(Outcome::ProbeInconclusive);
        }
        Ok(Err(e)) => return Err(e),
        Ok(Ok(())) => {}
    }

    // A 6-byte reply that isn't HTRK-magic means junk from a v1 tracker that
    // choked on our v3 handshake — also inconclusive, retry with v1.
    if resp6[0..4] != HTRK_MAGIC {
        return Ok(Outcome::ProbeInconclusive);
    }

    let listing = dispatch_after_response(stream, &resp6).await?;
    Ok(Outcome::Listing(listing))
}

/// Run a v1 fetch over `stream`: send the 6-byte `"HTRK\0\1"` magic, read
/// the 6-byte response (no watchdog — the caller already decided this is
/// a v1 endpoint), and dispatch. Normally yields a v1 listing, but the
/// version dispatch is shared, so a v3-speaking endpoint here would still
/// be handled.
pub async fn run_v1<S: AsyncRead + AsyncWrite + Unpin>(
    stream: &mut S,
) -> Result<TrackerListing, TrackerError> {
    let mut magic = [0u8; 6];
    magic[0..4].copy_from_slice(&HTRK_MAGIC);
    magic[4..6].copy_from_slice(&HTRK_VERSION_V1.to_be_bytes());
    stream.write_all(&magic).await?;
    stream.flush().await?;

    let mut resp6 = [0u8; 6];
    read_exact_or_short(stream, &mut resp6).await?;
    dispatch_after_response(stream, &resp6).await
}

/// Shared post-handshake dispatch: validate the magic, read the version,
/// and run the matching listing flow. `resp6` is the 6-byte response
/// already read by the caller.
async fn dispatch_after_response<S: AsyncRead + AsyncWrite + Unpin>(
    stream: &mut S,
    resp6: &[u8; 6],
) -> Result<TrackerListing, TrackerError> {
    if resp6[0..4] != HTRK_MAGIC {
        return Err(TrackerError::BadMagic);
    }
    let version = u16::from_be_bytes([resp6[4], resp6[5]]);
    match version {
        HTRK_VERSION_V3 => read_v3_listing(stream).await,
        HTRK_VERSION_V1 | HTRK_VERSION_V2 => read_v1_listing(stream, resp6).await,
        other => Err(TrackerError::UnsupportedVersion(other)),
    }
}

/// v1 listing flow. `resp6` is the first 6 of the 14-byte reply header;
/// read the trailing 8, parse `nservers`, then stream records.
async fn read_v1_listing<S: AsyncRead + AsyncWrite + Unpin>(
    stream: &mut S,
    resp6: &[u8; 6],
) -> Result<TrackerListing, TrackerError> {
    let mut hdr = [0u8; 14];
    hdr[0..6].copy_from_slice(resp6);
    read_exact_or_short(stream, &mut hdr[6..14]).await?;
    // Infallible at 14 bytes; defensively map None to ShortRead.
    let nservers = parse_tracker_header(&hdr).ok_or(TrackerError::ShortRead)?;

    // Don't pre-size from the untrusted `nservers` (up to 65_535) — that
    // would allocate up front before a single record is validated. Let
    // the Vec grow against the bytes actually delivered.
    let mut records = Vec::new();
    let mut remaining = nservers;
    let mut padding_slots: u32 = 0;
    while remaining > 0 {
        // IPv4(4) + port(2) + nusers(2). A leading zero byte is a
        // padding slot (IPs can't start with 0) — skip it without
        // decrementing the counter, exactly the bytes the C reader
        // consumes for a padding entry.
        let mut head8 = [0u8; 8];
        read_exact_or_short(stream, &mut head8).await?;
        if tracker_record_is_padding(&head8) {
            padding_slots += 1;
            if padding_slots > MAX_V1_PADDING_SLOTS {
                return Err(TrackerError::ExcessivePadding);
            }
            continue;
        }
        // 2 reserved + name_len.
        let mut rest3 = [0u8; 3];
        read_exact_or_short(stream, &mut rest3).await?;
        let mut fixed = [0u8; 11];
        fixed[0..8].copy_from_slice(&head8);
        fixed[8..11].copy_from_slice(&rest3);
        // Infallible at 11 bytes.
        let rec = parse_tracker_record_fixed(&fixed).ok_or(TrackerError::ShortRead)?;

        let mut name = vec![0u8; rec.name_len as usize];
        read_exact_or_short(stream, &mut name).await?;
        tracker_normalize_text(&mut name);

        let mut desc_len = [0u8; 1];
        read_exact_or_short(stream, &mut desc_len).await?;
        let mut desc = vec![0u8; desc_len[0] as usize];
        read_exact_or_short(stream, &mut desc).await?;
        tracker_normalize_text(&mut desc);

        records.push(TrackerRecord {
            addr_type: ADDR_IPV4,
            // addr_be is the raw 4 wire bytes in network order; surface
            // them verbatim, matching the C event's in_addr.
            address: fixed[0..4].to_vec(),
            port: rec.port,
            nusers: rec.nusers,
            name,
            desc,
            tlv_count: 0,
            tlv_bytes: Vec::new(),
        });
        remaining -= 1;
    }

    Ok(TrackerListing {
        version: 1,
        expected: nservers,
        records,
    })
}

/// v3 listing flow. The 8-byte handshake response has been consumed down
/// to its first 6 bytes; read the trailing 2 feature bytes, send the
/// listing request, read the response header + payload, walk records.
async fn read_v3_listing<S: AsyncRead + AsyncWrite + Unpin>(
    stream: &mut S,
) -> Result<TrackerListing, TrackerError> {
    // Trailing 2 feature bytes of the 8-byte handshake response. We
    // don't act on the feature bitmask in T1 (Phase A requested no
    // optional fields), but they must be drained off the wire before the
    // listing request. Named `_features` since the value is only drained.
    let mut _features = [0u8; 2];
    read_exact_or_short(stream, &mut _features).await?;

    let mut req = [0u8; 4];
    // Infallible: 4-byte buffer.
    let _ = pack_tracker_v3_listing_request_simple(&mut req);
    stream.write_all(&req).await?;
    stream.flush().await?;

    let mut hdr = [0u8; tracker_v3::RESP_HDR_LEN];
    read_exact_or_short(stream, &mut hdr).await?;
    let h = parse_tracker_v3_response_header(&hdr).ok_or(TrackerError::BadV3Header)?;

    // Empty listing — a clean finish with no records (the view still
    // shows the section, so the user sees the tracker WAS reached).
    if h.total_size == 0 && h.record_count == 0 {
        return Ok(TrackerListing {
            version: 3,
            expected: 0,
            records: Vec::new(),
        });
    }
    // Exactly one of the two zero: malformed (records with no payload, or
    // payload with no records).
    if h.total_size == 0 || h.record_count == 0 {
        return Err(TrackerError::MalformedV3Header {
            total_size: h.total_size,
            record_count: h.record_count,
        });
    }
    if h.total_size > V3_MAX_PAYLOAD {
        return Err(TrackerError::PayloadTooLarge {
            total_size: h.total_size,
            cap: V3_MAX_PAYLOAD,
        });
    }

    let mut payload = vec![0u8; h.total_size as usize];
    read_exact_or_short(stream, &mut payload).await?;

    // Grow as records parse rather than pre-sizing from the untrusted
    // record_count (a tiny payload could still claim 65_535 records).
    let mut records = Vec::new();
    let mut off = 0usize;
    for i in 0..h.record_count {
        let (rec, consumed) =
            parse_tracker_v3_record(&payload, off).ok_or(TrackerError::MalformedV3Record {
                index: i,
                count: h.record_count,
            })?;
        records.push(TrackerRecord {
            addr_type: rec.addr_type,
            address: rec.address.to_vec(),
            port: rec.port,
            nusers: rec.nusers,
            name: {
                let mut n = rec.name.to_vec();
                tracker_normalize_text(&mut n);
                n
            },
            desc: {
                let mut d = rec.desc.to_vec();
                tracker_normalize_text(&mut d);
                d
            },
            tlv_count: rec.tlv_count,
            tlv_bytes: rec.tlv_bytes.to_vec(),
        });
        off += consumed;
    }

    // Symmetric with the C strict-leftover contract: after record_count
    // records the cursor must land exactly at the payload end. Leftover
    // bytes mean the declared count understated the content — surface it
    // rather than silently dropping wire data.
    if off != payload.len() {
        return Err(TrackerError::V3TrailingBytes(payload.len() - off));
    }

    Ok(TrackerListing {
        version: 3,
        expected: h.record_count,
        records,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The 8-byte v3 client handshake the engine should write
    /// (`"HTRK"` + version 0x0003 + features).
    fn v3_client_handshake(features: u16) -> Vec<u8> {
        let mut h = Vec::with_capacity(8);
        h.extend_from_slice(&HTRK_MAGIC);
        h.extend_from_slice(&HTRK_VERSION_V3.to_be_bytes());
        h.extend_from_slice(&features.to_be_bytes());
        h
    }

    /// The 6-byte v1 client magic the engine should write
    /// (`"HTRK\0\1"`).
    fn v1_client_magic() -> Vec<u8> {
        let mut h = Vec::with_capacity(6);
        h.extend_from_slice(&HTRK_MAGIC);
        h.extend_from_slice(&HTRK_VERSION_V1.to_be_bytes());
        h
    }

    /// Drive `fut` (the engine) on one end of an in-memory duplex while a
    /// scripted "server" runs on the other: it reads the bytes the engine
    /// writes and asserts they exactly equal `expect_handshake` (so a
    /// regression that writes the wrong handshake / version fails here,
    /// not just on a misaligned reply), then writes `script` for the
    /// engine to read. Returns the engine's result.
    async fn with_server<F, Fut, T>(expect_handshake: &[u8], script: Vec<u8>, run: F) -> T
    where
        F: FnOnce(tokio::io::DuplexStream) -> Fut,
        Fut: std::future::Future<Output = T>,
    {
        let (client, mut server) = tokio::io::duplex(64 * 1024);
        let expect = expect_handshake.to_vec();
        // The server task writes the script, then holds the connection
        // open (blocked on `rx_done`) until the engine result is in. This
        // keeps the silence-probe test from seeing a premature EOF before
        // the watchdog fires, without an arbitrary sleep that would slow
        // the suite.
        let (tx_done, rx_done) = tokio::sync::oneshot::channel::<()>();
        let server_task = tokio::spawn(async move {
            // Read + assert the engine's handshake. Fails loudly on a
            // short write (read_exact) OR wrong bytes (assert), so a
            // broken handshake can't slip through on a matching reply.
            if !expect.is_empty() {
                let mut hs = vec![0u8; expect.len()];
                server
                    .read_exact(&mut hs)
                    .await
                    .expect("engine handshake bytes");
                assert_eq!(hs, expect, "engine wrote an unexpected handshake");
            }
            let _ = server.write_all(&script).await;
            let _ = server.flush().await;
            let _ = rx_done.await; // hold `server` open until the test releases us
        });
        let out = run(client).await;
        let _ = tx_done.send(());
        // Propagate a server-task panic/cancel so a broken scripted server
        // fails the test loudly instead of silently green.
        server_task.await.expect("scripted server task");
        out
    }

    /// Like [`with_server`], but the server writes `script` and then
    /// *closes* (drops its half) instead of holding open. The engine sees
    /// the script bytes followed by EOF — used to exercise the short-read /
    /// connection-close probe path without waiting on the watchdog.
    async fn with_closing_server<F, Fut, T>(expect_handshake: &[u8], script: Vec<u8>, run: F) -> T
    where
        F: FnOnce(tokio::io::DuplexStream) -> Fut,
        Fut: std::future::Future<Output = T>,
    {
        let (client, mut server) = tokio::io::duplex(64 * 1024);
        let expect = expect_handshake.to_vec();
        let server_task = tokio::spawn(async move {
            if !expect.is_empty() {
                let mut hs = vec![0u8; expect.len()];
                server
                    .read_exact(&mut hs)
                    .await
                    .expect("engine handshake bytes");
                assert_eq!(hs, expect, "engine wrote an unexpected handshake");
            }
            let _ = server.write_all(&script).await;
            let _ = server.flush().await;
            // Fall out of scope → `server` drops → EOF on the client side.
        });
        let out = run(client).await;
        // Propagate a server-task panic/cancel rather than masking it.
        server_task.await.expect("scripted server task");
        out
    }

    fn v1_header(nservers: u16) -> Vec<u8> {
        let mut h = vec![0u8; 14];
        h[0..4].copy_from_slice(&HTRK_MAGIC);
        h[4..6].copy_from_slice(&HTRK_VERSION_V1.to_be_bytes());
        h[10..12].copy_from_slice(&nservers.to_be_bytes());
        h
    }

    fn v1_record(ip: [u8; 4], port: u16, nusers: u16, name: &[u8], desc: &[u8]) -> Vec<u8> {
        let mut r = Vec::new();
        r.extend_from_slice(&ip);
        r.extend_from_slice(&port.to_be_bytes());
        r.extend_from_slice(&nusers.to_be_bytes());
        r.extend_from_slice(&[0, 0]); // 2 reserved
        r.push(name.len() as u8);
        r.extend_from_slice(name);
        r.push(desc.len() as u8);
        r.extend_from_slice(desc);
        r
    }

    #[tokio::test]
    async fn v1_listing_round_trip_with_padding() {
        // nservers = 2; a padding slot (8 zero bytes) precedes the two
        // real records and must NOT count against nservers.
        let mut script = v1_header(2);
        script.extend_from_slice(&[0u8; 8]); // padding slot
        script.extend_from_slice(&v1_record([1, 2, 3, 4], 5500, 3, b"srv1", b"abc"));
        script.extend_from_slice(&v1_record([5, 6, 7, 8], 6000, 0, b"", b""));

        let listing = with_server(&v1_client_magic(), script, |mut c| async move {
            run_v1(&mut c).await
        })
        .await
        .expect("v1 listing");

        assert_eq!(listing.version, 1);
        assert_eq!(listing.expected, 2);
        assert_eq!(listing.records.len(), 2);

        let a = &listing.records[0];
        assert_eq!(a.addr_type, ADDR_IPV4);
        assert_eq!(a.address, vec![1, 2, 3, 4]);
        assert_eq!(a.port, 5500);
        assert_eq!(a.nusers, 3);
        assert_eq!(a.name, b"srv1");
        assert_eq!(a.desc, b"abc");
        assert_eq!(a.tlv_count, 0);
        assert!(a.tlv_bytes.is_empty());

        let b = &listing.records[1];
        assert_eq!(b.address, vec![5, 6, 7, 8]);
        assert_eq!(b.port, 6000);
        assert_eq!(b.nusers, 0);
        assert!(b.name.is_empty());
        assert!(b.desc.is_empty());
    }

    fn v3_handshake_response(features: u16) -> Vec<u8> {
        let mut r = Vec::new();
        r.extend_from_slice(&HTRK_MAGIC);
        r.extend_from_slice(&HTRK_VERSION_V3.to_be_bytes());
        r.extend_from_slice(&features.to_be_bytes());
        r
    }

    fn v3_ipv4_record(ip: [u8; 4], port: u16, nusers: u16, name: &[u8], desc: &[u8]) -> Vec<u8> {
        let mut r = Vec::new();
        r.push(tracker_v3::ADDR_IPV4);
        r.extend_from_slice(&ip);
        r.extend_from_slice(&port.to_be_bytes());
        r.extend_from_slice(&nusers.to_be_bytes());
        r.extend_from_slice(&(name.len() as u16).to_be_bytes());
        r.extend_from_slice(name);
        r.extend_from_slice(&(desc.len() as u16).to_be_bytes());
        r.extend_from_slice(desc);
        r.extend_from_slice(&0u16.to_be_bytes()); // tlv_count = 0
        r
    }

    #[tokio::test]
    async fn v3_probe_listing_round_trip() {
        let rec_a = v3_ipv4_record([10, 0, 0, 1], 5500, 7, b"alpha", b"first");
        let rec_b = v3_ipv4_record([10, 0, 0, 2], 5501, 0, b"beta", b"");
        let mut payload = Vec::new();
        payload.extend_from_slice(&rec_a);
        payload.extend_from_slice(&rec_b);

        let mut script = v3_handshake_response(0);
        // 10-byte response header: type=1, total_size, total_servers=2,
        // record_count=2.
        script.extend_from_slice(&tracker_v3::RESP_LIST.to_be_bytes());
        script.extend_from_slice(&(payload.len() as u32).to_be_bytes());
        script.extend_from_slice(&2u16.to_be_bytes());
        script.extend_from_slice(&2u16.to_be_bytes());
        script.extend_from_slice(&payload);

        // Handshake the engine writes: 8-byte v3 handshake, then the
        // 4-byte listing request (10 bytes upstream of the response). We
        // only need to drain the first 8 before writing the handshake
        // response; the listing request is drained by the server task's
        // trailing read window. To keep with_server simple, drain just
        // the 8-byte handshake and let the 4-byte request sit in the
        // buffer (the engine never blocks on us reading it).
        let outcome = with_server(&v3_client_handshake(0), script, |mut c| async move {
            run_v3_probe(&mut c, 0, Duration::from_secs(5)).await
        })
        .await
        .expect("v3 probe");

        let listing = match outcome {
            Outcome::Listing(l) => l,
            Outcome::ProbeInconclusive => panic!("unexpected probe timeout"),
        };
        assert_eq!(listing.version, 3);
        assert_eq!(listing.expected, 2);
        assert_eq!(listing.records.len(), 2);
        assert_eq!(listing.records[0].addr_type, tracker_v3::ADDR_IPV4);
        assert_eq!(listing.records[0].address, vec![10, 0, 0, 1]);
        assert_eq!(listing.records[0].port, 5500);
        assert_eq!(listing.records[0].nusers, 7);
        assert_eq!(listing.records[0].name, b"alpha");
        assert_eq!(listing.records[0].desc, b"first");
        assert_eq!(listing.records[1].name, b"beta");
        assert!(listing.records[1].desc.is_empty());
    }

    #[tokio::test]
    async fn v3_probe_times_out_on_silence() {
        // Server reads the handshake and sends nothing — a pre-spec v1
        // tracker ignoring the 0x0003 byte. The watchdog fires.
        let outcome = with_server(&v3_client_handshake(0), Vec::new(), |mut c| async move {
            run_v3_probe(&mut c, 0, Duration::from_millis(80)).await
        })
        .await
        .expect("probe returns Ok(ProbeInconclusive)");
        assert!(matches!(outcome, Outcome::ProbeInconclusive));
    }

    #[tokio::test]
    async fn v3_probe_falls_into_v1_path_on_v1_response() {
        // A spec-compliant tracker that read only 6 handshake bytes and
        // replied v1: the probe should drive the v1 record flow.
        let mut script = v1_header(1);
        script.extend_from_slice(&v1_record([192, 168, 0, 1], 5500, 1, b"v1srv", b"d"));

        let outcome = with_server(&v3_client_handshake(0), script, |mut c| async move {
            run_v3_probe(&mut c, 0, Duration::from_secs(5)).await
        })
        .await
        .expect("probe");
        let listing = match outcome {
            Outcome::Listing(l) => l,
            Outcome::ProbeInconclusive => panic!("unexpected timeout"),
        };
        assert_eq!(listing.version, 1);
        assert_eq!(listing.records.len(), 1);
        assert_eq!(listing.records[0].name, b"v1srv");
    }

    #[tokio::test]
    async fn bad_magic_is_rejected() {
        let script = vec![b'N', b'O', b'P', b'E', 0, 1];
        let err = with_server(&v1_client_magic(), script, |mut c| async move {
            run_v1(&mut c).await
        })
        .await
        .unwrap_err();
        assert!(matches!(err, TrackerError::BadMagic));
    }

    #[tokio::test]
    async fn unsupported_version_is_rejected() {
        let mut script = Vec::new();
        script.extend_from_slice(&HTRK_MAGIC);
        script.extend_from_slice(&0x0009u16.to_be_bytes()); // not v1/v2/v3
        let err = with_server(&v1_client_magic(), script, |mut c| async move {
            run_v1(&mut c).await
        })
        .await
        .unwrap_err();
        assert!(matches!(err, TrackerError::UnsupportedVersion(0x0009)));
    }

    #[tokio::test]
    async fn v3_probe_falls_back_on_junk_reply() {
        // A pre-spec v1 tracker wedges on our v3 magic and dribbles 6
        // non-HTRK bytes. That's inconclusive — the probe must report
        // ProbeInconclusive so the caller retries v1, NOT a hard BadMagic.
        let script = vec![b'N', b'O', b'P', b'E', 0, 1];
        let outcome = with_server(&v3_client_handshake(0), script, |mut c| async move {
            run_v3_probe(&mut c, 0, Duration::from_secs(5)).await
        })
        .await
        .expect("probe returns Ok");
        assert!(matches!(outcome, Outcome::ProbeInconclusive));
    }

    #[tokio::test]
    async fn v3_probe_falls_back_on_short_read() {
        // Server sends 3 bytes then hangs up before the full 6-byte
        // response. A connection close mid-reply is inconclusive → v1
        // retry, even though the watchdog never fired (generous timeout).
        let outcome = with_closing_server(
            &v3_client_handshake(0),
            vec![b'H', b'T', b'R'],
            |mut c| async move { run_v3_probe(&mut c, 0, Duration::from_secs(5)).await },
        )
        .await
        .expect("probe returns Ok");
        assert!(matches!(outcome, Outcome::ProbeInconclusive));
    }

    #[tokio::test]
    async fn v3_probe_unsupported_version_is_hard_error() {
        // Once valid HTRK magic is in hand, an unsupported *version* is a
        // real failure — the legacy C bails here rather than retrying v1.
        let mut script = Vec::new();
        script.extend_from_slice(&HTRK_MAGIC);
        script.extend_from_slice(&0x0009u16.to_be_bytes());
        let err = with_server(&v3_client_handshake(0), script, |mut c| async move {
            run_v3_probe(&mut c, 0, Duration::from_secs(5)).await
        })
        .await
        .unwrap_err();
        assert!(matches!(err, TrackerError::UnsupportedVersion(0x0009)));
    }

    #[tokio::test]
    async fn v3_empty_listing_is_clean() {
        let mut script = v3_handshake_response(0);
        // total_size = 0, record_count = 0 → clean empty.
        script.extend_from_slice(&tracker_v3::RESP_LIST.to_be_bytes());
        script.extend_from_slice(&0u32.to_be_bytes());
        script.extend_from_slice(&0u16.to_be_bytes());
        script.extend_from_slice(&0u16.to_be_bytes());

        let outcome = with_server(&v3_client_handshake(0), script, |mut c| async move {
            run_v3_probe(&mut c, 0, Duration::from_secs(5)).await
        })
        .await
        .expect("v3 empty");
        let listing = match outcome {
            Outcome::Listing(l) => l,
            Outcome::ProbeInconclusive => panic!("unexpected timeout"),
        };
        assert_eq!(listing.version, 3);
        assert_eq!(listing.expected, 0);
        assert!(listing.records.is_empty());
    }

    #[tokio::test]
    async fn v3_malformed_header_one_zero() {
        let mut script = v3_handshake_response(0);
        // total_size > 0 but record_count = 0 → malformed.
        script.extend_from_slice(&tracker_v3::RESP_LIST.to_be_bytes());
        script.extend_from_slice(&16u32.to_be_bytes());
        script.extend_from_slice(&0u16.to_be_bytes());
        script.extend_from_slice(&0u16.to_be_bytes());

        let err = with_server(&v3_client_handshake(0), script, |mut c| async move {
            run_v3_probe(&mut c, 0, Duration::from_secs(5)).await
        })
        .await
        .unwrap_err();
        assert!(matches!(
            err,
            TrackerError::MalformedV3Header {
                total_size: 16,
                record_count: 0
            }
        ));
    }

    /// 10-byte v3 listing-response header with an explicit response type.
    fn v3_resp_header(response_type: u16, total_size: u32, record_count: u16) -> Vec<u8> {
        let mut h = Vec::new();
        h.extend_from_slice(&response_type.to_be_bytes());
        h.extend_from_slice(&total_size.to_be_bytes());
        h.extend_from_slice(&record_count.to_be_bytes()); // total_servers
        h.extend_from_slice(&record_count.to_be_bytes());
        h
    }

    #[tokio::test]
    async fn v3_bad_response_type_is_rejected() {
        // A response-type byte that isn't RESP_LIST fails the header parse.
        let mut script = v3_handshake_response(0);
        script.extend_from_slice(&v3_resp_header(0x0099, 16, 1));
        let err = with_server(&v3_client_handshake(0), script, |mut c| async move {
            run_v3_probe(&mut c, 0, Duration::from_secs(5)).await
        })
        .await
        .unwrap_err();
        assert!(matches!(err, TrackerError::BadV3Header));
    }

    #[tokio::test]
    async fn v3_payload_too_large_is_rejected() {
        // total_size over the cap is rejected from the header alone — no
        // payload is sent, the engine bails before trying to read it.
        let mut script = v3_handshake_response(0);
        script.extend_from_slice(&v3_resp_header(
            tracker_v3::RESP_LIST,
            V3_MAX_PAYLOAD + 1,
            1,
        ));
        let err = with_server(&v3_client_handshake(0), script, |mut c| async move {
            run_v3_probe(&mut c, 0, Duration::from_secs(5)).await
        })
        .await
        .unwrap_err();
        assert!(matches!(
            err,
            TrackerError::PayloadTooLarge { cap, .. } if cap == V3_MAX_PAYLOAD
        ));
    }

    #[tokio::test]
    async fn v3_malformed_record_is_rejected() {
        // record_count=1 but the payload's first byte is an unknown
        // address type, so the record parse fails at index 0.
        let payload = vec![0xFFu8; 8];
        let mut script = v3_handshake_response(0);
        script.extend_from_slice(&v3_resp_header(
            tracker_v3::RESP_LIST,
            payload.len() as u32,
            1,
        ));
        script.extend_from_slice(&payload);
        let err = with_server(&v3_client_handshake(0), script, |mut c| async move {
            run_v3_probe(&mut c, 0, Duration::from_secs(5)).await
        })
        .await
        .unwrap_err();
        assert!(matches!(
            err,
            TrackerError::MalformedV3Record { index: 0, count: 1 }
        ));
    }

    #[tokio::test]
    async fn v3_trailing_bytes_is_rejected() {
        // One valid record but total_size declares 4 extra bytes the
        // record_count of 1 doesn't account for → strict trailing check.
        let rec = v3_ipv4_record([1, 2, 3, 4], 5500, 1, b"x", b"");
        let mut payload = rec.clone();
        payload.extend_from_slice(&[0, 0, 0, 0]);
        let mut script = v3_handshake_response(0);
        script.extend_from_slice(&v3_resp_header(
            tracker_v3::RESP_LIST,
            payload.len() as u32,
            1,
        ));
        script.extend_from_slice(&payload);
        let err = with_server(&v3_client_handshake(0), script, |mut c| async move {
            run_v3_probe(&mut c, 0, Duration::from_secs(5)).await
        })
        .await
        .unwrap_err();
        assert!(matches!(err, TrackerError::V3TrailingBytes(4)));
    }

    #[tokio::test]
    async fn v1_excessive_padding_is_rejected() {
        // A tracker that dribbles padding slots forever (never a real
        // record) is bounded — past MAX_V1_PADDING_SLOTS the reader bails
        // rather than consuming unbounded data.
        let mut script = v1_header(1); // promises 1 server, never delivered
        let slots = (MAX_V1_PADDING_SLOTS as usize) + 1;
        script.extend_from_slice(&vec![0u8; slots * 8]); // all-zero = padding
        let err = with_server(&v1_client_magic(), script, |mut c| async move {
            run_v1(&mut c).await
        })
        .await
        .unwrap_err();
        assert!(matches!(err, TrackerError::ExcessivePadding));
    }

    #[tokio::test]
    async fn truncated_v1_record_is_short_read() {
        // Header promises 1 server but the stream ends mid-record. Use
        // with_closing_server: the server drops after the partial bytes
        // so the engine's read_exact sees EOF (→ ShortRead). with_server
        // would instead hold the connection open, leaving the engine
        // blocked forever waiting for the 2 missing bytes (the hang that
        // showed up as a CI timeout).
        let mut script = v1_header(1);
        script.extend_from_slice(&[1, 2, 3, 4, 0x15, 0x7c]); // only 6 of the 8 head bytes
        let err = with_closing_server(&v1_client_magic(), script, |mut c| async move {
            run_v1(&mut c).await
        })
        .await
        .unwrap_err();
        assert!(matches!(err, TrackerError::ShortRead));
    }
}
