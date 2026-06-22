//! Wire-protocol trace for the orchestrator's handshake phase —
//! the Rust-side counterpart to `src/proto_trace.c`.
//!
//! `proto_trace.c` traces frames once they reach the C `rcv` path
//! (`hx_rcv_hdr`) or go out through `hlwrite`. But the Phase G
//! orchestrator does DNS + TCP + magic + LOGIN + the login-reply
//! receive entirely in Rust, *before* any frame is handed to the C
//! side, so those handshake-phase frames are invisible to
//! `proto_trace.c`. That's a real gap: "the server rejected our
//! LOGIN" / "the server sent X before the TASK reply" bugs live
//! exactly in that window. This module fills it.
//!
//! Same gate as the C trace — `GTKHX_DEBUG=proto` (or `all`) — and the
//! same arrow format (`→` outgoing, `←` incoming) so a full session
//! trace reads continuously across the Rust handshake and the C
//! session. Output goes to stderr (where the C `debug_log` category
//! sink also writes), prefixed `[proto]`.

use std::sync::OnceLock;

use hotline_proto::parse::decode_header_full;
use hotline_proto::wire::ChunkIter;

/// Cheap once-checked gate: enabled when `GTKHX_DEBUG` contains the
/// `proto` (or `all`) category, matching `src/debug.c`'s parsing.
fn enabled() -> bool {
    static ENABLED: OnceLock<bool> = OnceLock::new();
    *ENABLED.get_or_init(|| {
        std::env::var("GTKHX_DEBUG")
            .map(|v| {
                v.split(',')
                    .map(str::trim)
                    .any(|cat| cat == "proto" || cat == "all")
            })
            .unwrap_or(false)
    })
}

/// Direction of a traced frame.
#[derive(Clone, Copy)]
pub enum Dir {
    /// Client → server.
    Out,
    /// Server → client.
    In,
}

/// Symbolic name for a wire opcode (header `type` field). Mirrors the
/// subset of `src/proto_trace.c::proto_hdr_name` that the handshake
/// path can see; unknown opcodes print as `?` (the hex is always
/// shown alongside, so an unnamed opcode is still identifiable).
fn hdr_name(type_: u32) -> &'static str {
    match type_ {
        // Client → server (the ones the orchestrator sends).
        0x0000_006b => "LOGIN",
        0x0000_006c => "MSG",
        0x0000_0079 => "AGREEMENTAGREE",
        0x0000_01f4 => "PING",
        // Server → client.
        0x0001_0000 => "TASK",
        0x0000_006d => "AGREEMENT",
        0x0000_0162 => "USER_SELFINFO",
        0x0000_012d => "USER_CHANGE",
        0x0000_012e => "USER_PART",
        0x0000_0163 => "MSG_BROADCAST",
        0x0000_0068 => "MSG",
        0x0000_006a => "CHAT",
        0x0000_007a => "BANNER",
        0x0000_0066 => "NEWS_POST",
        0x0000_006f => "POLITEQUIT",
        0x0000_00d3 => "QUEUE",
        _ => "?",
    }
}

/// Trace one full wire frame (22-byte header + body). No-op unless
/// `GTKHX_DEBUG=proto`. `raw` is the complete frame as it goes on / came
/// off the wire; the header is decoded here and the chunk list walked,
/// matching the C trace's per-chunk lines.
pub fn trace(dir: Dir, raw: &[u8]) {
    if !enabled() {
        return;
    }
    let Some(h) = decode_header_full(raw, crate::MAX_BODY_LEN + 2) else {
        eprintln!("[proto] (hxnet) <undecodable frame, {} bytes>", raw.len());
        return;
    };
    match dir {
        Dir::Out => eprintln!(
            "[proto] (hxnet) \u{2192} trans={} type={} (0x{:06x}) hc={}",
            h.trans,
            hdr_name(h.type_),
            h.type_,
            h.hc
        ),
        Dir::In => eprintln!(
            "[proto] (hxnet) \u{2190} trans={} type={} (0x{:06x}) flag={} len={}",
            h.trans,
            hdr_name(h.type_),
            h.type_,
            h.flag,
            // Log the raw wire `len` (matches src/proto_trace.c), so an
            // odd/oversized length claim is visible in the trace rather
            // than hidden behind the post-decode body_len.
            h.wire_len
        ),
    }
    for chunk in ChunkIter::over_message(raw, raw.len()) {
        // Redact credential chunks. LOGIN (0x0069) and PASSWORD
        // (0x006a) are only XOR-0xFF "obfuscated" on the wire —
        // trivially reversible — so previewing them would leak
        // credentials into the log. Still report tag + len.
        let body = match chunk.tag {
            0x0069 | 0x006a => "<redacted>".to_string(),
            _ => preview(chunk.data),
        };
        eprintln!(
            "[proto] (hxnet)   chunk type=0x{:04x} len={} {}",
            chunk.tag,
            chunk.data.len(),
            body
        );
    }
}

/// A short preview of a chunk's data, mirroring
/// `src/proto_trace.c::data_preview` byte-for-byte so the Rust and C
/// traces are diffable. Empty data is `""` (empty string). A window
/// that is entirely printable — `isprint()` plus `\t` / `\n` / `\r`,
/// with a single trailing NUL tolerated — is rendered quoted with
/// `"`, `\`, `\n`, `\r`, `\t` and any mid-string NUL escaped;
/// otherwise the bytes are hex-dumped (space-separated, no brackets).
/// Long chunks are truncated at `PREVIEW_MAX` with a `... (+N bytes)`
/// suffix. The caller supplies the leading space.
fn preview(data: &[u8]) -> String {
    // Matches PREVIEW_MAX in src/proto_trace.c.
    const MAX: usize = 48;
    if data.is_empty() {
        return String::new();
    }
    let show = data.len().min(MAX);
    let window = &data[..show];

    // Printable if every byte is isprint() or \t / \n / \r. A NUL that
    // is the final byte of the whole (untruncated) chunk is tolerated;
    // a mid-string NUL is not.
    let printable = window.iter().enumerate().all(|(i, &c)| {
        if c == 0 && i == show - 1 && i == data.len() - 1 {
            return true; // trailing NUL
        }
        (0x20..0x7f).contains(&c) || c == b'\t' || c == b'\n' || c == b'\r'
    });

    let mut s = String::with_capacity(MAX * 4 + 16);
    if printable {
        s.push('"');
        for &c in window {
            match c {
                b'"' => s.push_str("\\\""),
                b'\\' => s.push_str("\\\\"),
                b'\n' => s.push_str("\\n"),
                b'\r' => s.push_str("\\r"),
                b'\t' => s.push_str("\\t"),
                0 => s.push_str("\\0"), // mid-string NUL
                _ => s.push(c as char),
            }
        }
        s.push('"');
    } else {
        for (i, &c) in window.iter().enumerate() {
            if i > 0 {
                s.push(' ');
            }
            s.push_str(&format!("{c:02x}"));
        }
    }
    if data.len() > MAX {
        s.push_str(&format!("... (+{} bytes)", data.len() - MAX));
    }
    s
}
