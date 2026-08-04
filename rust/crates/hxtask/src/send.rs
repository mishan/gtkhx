//! `hlwrite_chunks` — the control-channel send primitive (ported from the send
//! orchestration in `network.c`).
//!
//! Every outbound Hotline request is packed into a frame and handed to hxnet for
//! transmission. That orchestration — snapshot the trans id, pack the frame
//! (stamping + advancing `htlc->trans`), emit the wire trace, then ship the
//! bytes through the hxnet send bridge (tearing the connection down if the actor
//! refuses) — used to live in `network.c::hlwrite_chunks`. It lives here now,
//! behind the exact C ABI the ~14 caller files link against. It's a natural
//! neighbour of the transaction table in this crate: every caller pairs a
//! [`task_new`] with a send, so registering an outbound request and putting it
//! on the wire are the two halves of the same idiom.
//!
//! [`task_new`]: crate::task_new
//!
//! The frame is packed **natively in Rust** via `hotline_proto` (the same
//! builder the old C `hlpack_chunks` wrapper delegated to), so nothing bounces
//! back out to C to build bytes — and the wire format is byte-for-byte unchanged
//! (1.2 / 1.5 / 1.9 compat is a hard requirement). The only C this reaches into
//! is the wire trace, the hxnet send bridge, and the status-log / close on a
//! send failure.
//!
//! The old variadic `hlwrite` front door is gone (it had no production callers):
//! a C variadic can't be an `extern "C"` Rust definition, and there was nothing
//! left to keep it for. Callers build a `struct hx_chunk[]` and call
//! `hlwrite_chunks` directly.

use std::os::raw::{c_char, c_int};

use hotline_proto::build::HxChunk;
// Native pack (no C detour): the same builder the C `hlpack_chunks` wrapper
// called, reached directly as a Rust function.
use hotline_proto::ffi::{gtkhx_proto_pack_message, gtkhx_proto_pack_message_size};

use crate::HtlcConn;

// The C environment this drives. Resolved at the final C link in the real build
// (leaf-up, via the gtkhx-ffi façade); the #[cfg(test)] doubles below let
// `cargo test` run headless — same shape the rest of this crate uses. The
// `hx_conn_*` accessors are Rust (gtkhx-core) reached over their C ABI so the
// doubles can stand in for them against a fake htlc.
#[cfg(not(test))]
extern "C" {
    /// gtkhx-core — `htlc->fd` (0 = no live socket).
    fn hx_conn_fd(h: *const HtlcConn) -> c_int;
    /// gtkhx-core — `htlc->trans` snapshot (no increment), for the trace id.
    fn hx_conn_trans(h: *const HtlcConn) -> u32;
    /// gtkhx-core — return `htlc->trans`, then increment it (the frame stamp).
    fn hx_conn_trans_post_inc(h: *mut HtlcConn) -> u32;
    /// proto_trace.c — Hotline wire trace (`GTKHX_DEBUG=proto`).
    fn proto_trace_send_begin(ty: u32, trans: u32, hc: u16);
    fn proto_trace_send_chunk(ty: u16, len: u16, data: *const u8);
    fn proto_trace_send_end();
    /// hxnet_bridge.c — the new-path send gate + frame enqueue.
    fn hx_bridge_is_installed(htlc: *const HtlcConn) -> glib::ffi::gboolean;
    fn hx_bridge_send_frame(htlc: *mut HtlcConn, data: *const u8, len: u32) -> c_int;
    /// gtkhx_log.c — status line, used to report a send failure before closing.
    fn hx_printf_prefix(
        htlc: *mut HtlcConn,
        cid: u32,
        prefix: *const c_char,
        fmt: *const c_char,
        ...
    );
    /// network.c — tear the connection down when hxnet refuses the send.
    fn hx_htlc_close(htlc: *mut HtlcConn, expected: c_int);
    /// debug.c — non-variadic categorised log (the `net` category).
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
}

/// The `[hx]` status-line prefix sigil (gtkhx.c global). Wrapped in a helper so
/// the test build can substitute a NULL (the log double ignores it) without
/// tripping the `*const c_char: !Sync` rule a bare `static` double would hit.
#[cfg(not(test))]
unsafe fn infoprefix() -> *const c_char {
    extern "C" {
        static INFOPREFIX: *const c_char;
    }
    INFOPREFIX
}

/// `void hlwrite_chunks(struct htlc_conn *htlc, guint32 type, guint32 flag,
/// const struct hx_chunk *chunks, int hc)` — the chunk-array send primitive
/// (ported from network.c). Packs the frame in Rust, traces it, and hands the
/// bytes to hxnet's send; on a send-actor refusal the connection is torn down
/// rather than leaving protocol state to desync. A faithful port of the C
/// orchestration — no wire bytes change.
///
/// # Safety
/// `htlc` is a live `struct htlc_conn *`; `chunks` points at `hc` valid
/// `hx_chunk`s (or is NULL when `hc == 0`). Same contract the C callers already
/// honour.
#[no_mangle]
pub unsafe extern "C" fn hlwrite_chunks(
    htlc: *mut HtlcConn,
    ty: u32,
    flag: u32,
    chunks: *const HxChunk,
    hc: c_int,
) {
    // Public-API guardrails (mirror the C `g_return_if_fail` set): a caller bug
    // fails before we open a proto trace block or write zero bytes.
    if htlc.is_null() || hc < 0 || (hc != 0 && chunks.is_null()) {
        return;
    }

    if hx_conn_fd(htlc) == 0 {
        return;
    }

    // Snapshot the trans id for the trace (no increment yet); the counter is
    // advanced only when we actually pack, matching the old C ordering where a
    // pack-guard trip left `htlc->trans` untouched.
    let my_trans = hx_conn_trans(htlc);

    // Pack the frame. `pack_message_size` predicts the exact byte count, so a
    // zero size (too many chunks / overflow) or a write that isn't exactly
    // `needed` is a caller bug, not a runtime condition — treated as fatal
    // below. We never truncate a short pack onto the wire.
    let needed = gtkhx_proto_pack_message_size(chunks, hc as usize);
    let packed: Result<Vec<u8>, ()> = if needed == 0 {
        Err(())
    } else {
        let trans = hx_conn_trans_post_inc(htlc); // == my_trans; advances the counter
        let mut buf = vec![0u8; needed];
        let written = gtkhx_proto_pack_message(
            buf.as_mut_ptr(),
            needed,
            ty,
            trans,
            flag,
            chunks,
            hc as usize,
        );
        if written == needed {
            Ok(buf)
        } else {
            Err(())
        }
    };

    // Trace after the pack (so a pack-guard trip can't leave an open trace
    // block), walking the caller's original chunk array — the attempt is visible
    // in a proto trace even when the pack was rejected below.
    proto_trace_send_begin(ty, my_trans, hc as u16);
    for i in 0..hc as isize {
        let c = &*chunks.offset(i);
        proto_trace_send_chunk(c.tag, c.len, c.data);
    }
    proto_trace_send_end();

    let frame = match packed {
        Ok(f) => f,
        Err(()) => {
            // A rejected or short pack means the caller handed us a bad chunk
            // array. Don't silently drop a control-channel request — that
            // desyncs protocol state and is near-impossible to diagnose. Report
            // it and tear the connection down (never a truncated frame on the
            // wire). Message pre-formatted so the log call takes no varargs.
            let msg = std::ffi::CString::new(format!(
                "hlwrite_chunks: frame pack failed (type={ty:#x}, {hc} chunks); closing.\n"
            ))
            .unwrap_or_default();
            hx_printf_prefix(htlc, 0, infoprefix(), msg.as_ptr());
            hx_htlc_close(htlc, /*expected=*/ 0);
            return;
        }
    };

    // When the bridge is installed, hxnet's transform stack handles the
    // negotiated cipher / compression, so we ship PLAINTEXT bytes.
    // Per-connection: the transport handle lives on the connection, so this
    // asks whether *this* one can send rather than whether any connection can.
    if hx_bridge_is_installed(htlc) != 0 {
        let rc = hx_bridge_send_frame(htlc, frame.as_ptr(), frame.len() as u32);
        if rc != 0 {
            // hxnet refused the send (channel full / actor exited / bug /
            // uninstalled mid-call). In every case the connection is
            // effectively dead — close rather than drop the bytes and let
            // protocol state desync. hx_bridge_send_frame already logged the
            // specific FFI code via g_critical.
            let msg = std::ffi::CString::new(format!("hxnet send failed (rc={rc}); closing.\n"))
                .unwrap_or_default();
            hx_printf_prefix(htlc, 0, infoprefix(), msg.as_ptr());
            hx_htlc_close(htlc, /*expected=*/ 0);
        }
    } else {
        // No bridge installed → no connection. The orchestrator installs the
        // bridge synchronously at connect time and sends only run on a live
        // session, so reaching here is a stray send racing teardown. Drop the
        // just-packed bytes rather than queue them on a dead socket.
        if let Ok(msg) = std::ffi::CString::new(format!(
            "hlwrite_chunks: no bridge installed; dropping {} packed bytes",
            frame.len()
        )) {
            debug_log_str(c"net".as_ptr(), msg.as_ptr());
        }
    }
    // `frame` (a Vec) frees itself here — no g_malloc / g_free round-trip.
}

// ---- test doubles for the C environment ------------------------------------
//
// The pack itself runs for real (hotline-proto is a normal Cargo dependency, so
// `cargo test` links the true builder). Only the C environment is doubled: each
// test drives the branch matrix through a thread-local `Env`, setting the inputs
// (fd liveness, whether the bridge is installed, the send return code) and
// reading back what the orchestration did (trace / send / close call counts).

#[cfg(test)]
mod doubles {
    use super::*;
    use std::cell::RefCell;

    pub struct Env {
        // inputs
        pub fd: c_int,
        pub trans: u32,
        pub installed: bool,
        pub send_rc: c_int,
        // observations
        pub trace_begin_trans: Option<u32>,
        pub trace_chunks: usize,
        pub send_calls: u32,
        pub last_send_len: u32,
        /// Which connection the send was routed to. The transport handle is
        /// per-connection now, so "did it reach the bridge" is only half the
        /// question — the other half is whether it reached the right one.
        pub last_send_htlc: *mut HtlcConn,
        pub close_calls: u32,
    }

    // Hand-written rather than `#[derive(Default)]`: `*mut T` only gained a
    // `Default` impl in a rustc newer than the workspace MSRV floor (1.85 —
    // Debian stable's stock toolchain, `rust-version` in rust/Cargo.toml), so
    // the derive fails to resolve `*mut HtlcConn: Default` there. It builds on
    // Debian unstable's newer rustc but not on the floor. A null default is
    // exactly what the derive would produce anyway.
    impl Default for Env {
        fn default() -> Self {
            Self {
                fd: 0,
                trans: 0,
                installed: false,
                send_rc: 0,
                trace_begin_trans: None,
                trace_chunks: 0,
                send_calls: 0,
                last_send_len: 0,
                last_send_htlc: std::ptr::null_mut(),
                close_calls: 0,
            }
        }
    }

    thread_local! {
        pub static ENV: RefCell<Env> = RefCell::new(Env::default());
    }

    pub fn reset(fd: c_int, trans: u32, installed: bool, send_rc: c_int) {
        ENV.with(|e| {
            *e.borrow_mut() = Env {
                fd,
                trans,
                installed,
                send_rc,
                ..Env::default()
            };
        });
    }

    pub unsafe fn hx_conn_fd(_h: *const HtlcConn) -> c_int {
        ENV.with(|e| e.borrow().fd)
    }
    pub unsafe fn hx_conn_trans(_h: *const HtlcConn) -> u32 {
        ENV.with(|e| e.borrow().trans)
    }
    pub unsafe fn hx_conn_trans_post_inc(_h: *mut HtlcConn) -> u32 {
        ENV.with(|e| {
            let mut env = e.borrow_mut();
            let t = env.trans;
            env.trans = t.wrapping_add(1);
            t
        })
    }
    pub unsafe fn proto_trace_send_begin(_ty: u32, trans: u32, _hc: u16) {
        ENV.with(|e| e.borrow_mut().trace_begin_trans = Some(trans));
    }
    pub unsafe fn proto_trace_send_chunk(_ty: u16, _len: u16, _data: *const u8) {
        ENV.with(|e| e.borrow_mut().trace_chunks += 1);
    }
    pub unsafe fn proto_trace_send_end() {}
    pub unsafe fn hx_bridge_is_installed(_htlc: *const HtlcConn) -> glib::ffi::gboolean {
        ENV.with(|e| if e.borrow().installed { 1 } else { 0 })
    }
    pub unsafe fn hx_bridge_send_frame(htlc: *mut HtlcConn, _data: *const u8, len: u32) -> c_int {
        ENV.with(|e| {
            let mut env = e.borrow_mut();
            env.send_calls += 1;
            env.last_send_len = len;
            env.last_send_htlc = htlc;
            env.send_rc
        })
    }
    // Fixed-arity stand-in for the variadic real symbol: hlwrite_chunks
    // pre-formats its message, so it only ever calls this with the four fixed
    // args (no varargs).
    pub unsafe fn hx_printf_prefix(
        _htlc: *mut HtlcConn,
        _cid: u32,
        _prefix: *const c_char,
        _fmt: *const c_char,
    ) {
    }
    pub unsafe fn hx_htlc_close(_htlc: *mut HtlcConn, _expected: c_int) {
        ENV.with(|e| e.borrow_mut().close_calls += 1);
    }
    pub unsafe fn debug_log_str(_cat: *const c_char, _msg: *const c_char) {}
    pub unsafe fn infoprefix() -> *const c_char {
        std::ptr::null()
    }
}

#[cfg(test)]
use doubles::{
    debug_log_str, hx_bridge_is_installed, hx_bridge_send_frame, hx_conn_fd, hx_conn_trans,
    hx_conn_trans_post_inc, hx_htlc_close, hx_printf_prefix, infoprefix, proto_trace_send_begin,
    proto_trace_send_chunk, proto_trace_send_end,
};

#[cfg(test)]
mod tests {
    use super::*;
    use doubles::ENV;

    // A non-NULL htlc for the tests that get past the null guard; the doubles
    // never dereference it.
    fn fake_htlc() -> *mut HtlcConn {
        std::ptr::dangling_mut::<HtlcConn>()
    }

    fn one_chunk() -> [HxChunk; 1] {
        static BODY: &[u8] = b"hi";
        [HxChunk {
            tag: 101,
            len: 2,
            data: BODY.as_ptr(),
        }]
    }

    #[test]
    fn null_htlc_is_noop() {
        doubles::reset(
            /*fd=*/ 1, /*trans=*/ 7, /*installed=*/ true, /*rc=*/ 0,
        );
        unsafe {
            hlwrite_chunks(std::ptr::null_mut(), 200, 0, std::ptr::null(), 0);
        }
        ENV.with(|e| {
            let env = e.borrow();
            assert_eq!(env.send_calls, 0);
            assert!(env.trace_begin_trans.is_none());
        });
    }

    #[test]
    fn negative_hc_is_noop() {
        doubles::reset(1, 7, true, 0);
        unsafe {
            hlwrite_chunks(fake_htlc(), 200, 0, std::ptr::null(), -1);
        }
        ENV.with(|e| assert_eq!(e.borrow().send_calls, 0));
    }

    #[test]
    fn positive_hc_with_null_chunks_is_noop() {
        doubles::reset(1, 7, true, 0);
        unsafe {
            hlwrite_chunks(fake_htlc(), 200, 0, std::ptr::null(), 2);
        }
        ENV.with(|e| assert_eq!(e.borrow().send_calls, 0));
    }

    #[test]
    fn dead_fd_is_noop() {
        doubles::reset(/*fd=*/ 0, 7, true, 0);
        unsafe {
            hlwrite_chunks(fake_htlc(), 200, 0, std::ptr::null(), 0);
        }
        ENV.with(|e| {
            let env = e.borrow();
            assert_eq!(env.send_calls, 0);
            assert!(env.trace_begin_trans.is_none());
        });
    }

    #[test]
    fn success_sends_traces_stamps_trans_and_does_not_close() {
        doubles::reset(
            /*fd=*/ 1, /*trans=*/ 42, /*installed=*/ true, /*rc=*/ 0,
        );
        let chunks = one_chunk();
        unsafe {
            hlwrite_chunks(fake_htlc(), 200, 0, chunks.as_ptr(), 1);
        }
        ENV.with(|e| {
            let env = e.borrow();
            // trans snapshot taken before the pack, reached the trace
            assert_eq!(env.trace_begin_trans, Some(42));
            assert_eq!(env.trace_chunks, 1);
            assert_eq!(env.send_calls, 1);
            // real pack produced a non-empty frame (22-byte hdr + chunk)
            assert!(env.last_send_len > 22);
            // and it went out on the connection it was asked for, not
            // whichever one happened to be installed last
            assert_eq!(env.last_send_htlc, fake_htlc());
            assert_eq!(env.close_calls, 0);
            // counter advanced by exactly one
            assert_eq!(env.trans, 43);
        });
    }

    #[test]
    fn zero_chunk_opcode_sends() {
        // DOWNLOAD_BANNER-shaped call: hc == 0, chunks NULL, still a valid send.
        doubles::reset(1, 3, true, 0);
        unsafe {
            hlwrite_chunks(fake_htlc(), 212, 0, std::ptr::null(), 0);
        }
        ENV.with(|e| {
            let env = e.borrow();
            assert_eq!(env.trace_chunks, 0);
            assert_eq!(env.send_calls, 1);
            assert_eq!(env.close_calls, 0);
            assert_eq!(env.trans, 4);
        });
    }

    #[test]
    fn send_failure_closes_the_connection() {
        doubles::reset(
            /*fd=*/ 1, 42, /*installed=*/ true, /*rc=*/ -1,
        );
        let chunks = one_chunk();
        unsafe {
            hlwrite_chunks(fake_htlc(), 200, 0, chunks.as_ptr(), 1);
        }
        ENV.with(|e| {
            let env = e.borrow();
            assert_eq!(env.send_calls, 1);
            assert_eq!(env.close_calls, 1);
        });
    }

    #[test]
    fn no_bridge_drops_without_send_or_close() {
        doubles::reset(
            /*fd=*/ 1, 42, /*installed=*/ false, /*rc=*/ 0,
        );
        let chunks = one_chunk();
        unsafe {
            hlwrite_chunks(fake_htlc(), 200, 0, chunks.as_ptr(), 1);
        }
        ENV.with(|e| {
            let env = e.borrow();
            // packed + traced, but nothing sent and no close
            assert_eq!(env.trace_begin_trans, Some(42));
            assert_eq!(env.send_calls, 0);
            assert_eq!(env.close_calls, 0);
            // still stamped one frame's trans
            assert_eq!(env.trans, 43);
        });
    }

    #[test]
    fn pack_rejection_is_fatal_and_never_sends() {
        // hc past MAX_PACK_CHUNKS (64) makes pack_message_size reject with a
        // zero size — a caller bug. It must close the connection, not silently
        // drop the request, and never put a (truncated) frame on the wire.
        doubles::reset(/*fd=*/ 1, 42, /*installed=*/ true, /*rc=*/ 0);
        let chunks = [HxChunk::EMPTY; 65];
        unsafe {
            hlwrite_chunks(fake_htlc(), 200, 0, chunks.as_ptr(), 65);
        }
        ENV.with(|e| {
            let env = e.borrow();
            assert_eq!(env.send_calls, 0);
            assert_eq!(env.close_calls, 1);
            // trans not advanced — the pack was rejected before the stamp.
            assert_eq!(env.trans, 42);
        });
    }
}
