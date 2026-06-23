//! C-callable surface for hxnet (Phase R3.3.b).
//!
//! Five symbols, all in this module:
//!
//! - [`hxnet_connection_spawn_fd`] — adopt an already-connected OS
//!   socket fd. Returns an opaque handle; the actor begins reading
//!   frames on the shared tokio runtime immediately.
//! - [`hxnet_connection_try_recv_frame`] — non-blocking poll for
//!   the next event. The C side drives this from a GLib idle / a
//!   poll loop in tests.
//! - [`hxnet_connection_send_frame`] — non-blocking enqueue of
//!   bytes to write.
//! - [`hxnet_connection_destroy`] — drop the handle; the actor
//!   gets HandleDropped and exits.
//! - [`hxnet_frame_free`] — release a frame returned by try_recv.
//!
//! R3.3.b ships the non-blocking, polling-style API only. The
//! callback-driven variant that routes events through the hxbridge
//! ferry to the GLib main loop arrives in a later phase
//! (R3.3.e) alongside the production switch in network.c. The
//! polling form is easier to reason about for the smoke-test
//! goal, and the FFI surface stays additive when the callback
//! path lands.
//!
//! # Lifetime
//!
//! The handle owns the tokio task that's reading the fd. Calling
//! [`hxnet_connection_destroy`] aborts that task and drops the
//! [`ConnectionHandle`]. A running actor that's idling on the
//! command channel would see the dropped sender as "handle
//! dropped" and flush before exiting, but the abort means a
//! blocked / mid-handshake actor is cancelled at its next await
//! without that graceful flush. Either way the fd is closed when
//! the wrapped `tokio::net::TcpStream` drops.
//!
//! # Safety contracts
//!
//! All five FFI entry points NULL-check their pointer parameters
//! before dereferencing them; a NULL handle / NULL out-param / NULL
//! data-with-nonzero-len gets a `g_critical` log and a failure
//! return code rather than a segfault. Rust **cannot** validate
//! non-NULL pointers — passing a freed handle, a stack address that
//! has gone out of scope, or any other dangling-but-non-NULL value
//! is undefined behaviour exactly as it would be in C. Every entry
//! point's `# Safety` section spells out the caller-side
//! preconditions that have to hold for the pointer arguments to be
//! safe to dereference.
//!
//! # Thread-safety: not Send / not Sync
//!
//! A given [`HxnetConnection`] pointer **must** be used from a
//! single thread, or with external synchronization that prevents
//! concurrent calls. The polling FFI's
//! `hxnet_connection_try_recv_frame`, `hxnet_connection_send_frame`,
//! and `hxnet_connection_destroy` all dereference the handle as
//! `&mut HxnetConnection` (try_recv mutably borrows the event
//! receiver; send mutably borrows the command sender via try_send;
//! destroy reclaims the box). Concurrent calls on the same
//! handle from different threads would create overlapping
//! `&mut` / `&mut` (or `&mut` / `&`) borrows, which is undefined
//! behaviour even when each individual operation is "morally
//! independent." The intended usage is exactly the
//! Connection-actor shape: production callers drive the FFI from
//! the GLib main thread; the actor itself runs on the tokio
//! runtime thread and uses the channels' internal locking. No
//! HxnetConnection pointer ever needs to live on more than one
//! thread.

use std::ffi::c_void;
use std::os::raw::{c_int, c_uint};
use std::os::unix::io::FromRawFd;

use hxbridge::runtime::Runtime;
use tokio::net::TcpStream;
use tokio::sync::mpsc;
use tokio::task::JoinHandle;

use crate::{Command, Connection, ConnectionHandle, Event, Frame, ShutdownReason};

/// Opaque handle type. C sees `struct hxnet_connection;` and only
/// uses pointers.
///
/// Holds state for both FFI modes:
///
/// - **Polling mode** (R3.3.b): `events` is `Some(receiver)`;
///   `_callback_state` is `None`. `try_recv_frame` drains the
///   receiver synchronously.
/// - **Callback mode** (R3.3.e): `events` is `None`; the actor's
///   event receiver has been moved into a tokio pump task that
///   forwards into an async_channel; the GLib main context drains
///   that channel via [`hxbridge::channel::forward_to_main`] and
///   invokes the C callback per frame. `_callback_state` holds
///   the pump's JoinHandle + the MainForwarder; both are kept
///   alive for the connection's lifetime and dropped here on
///   destroy.
///
/// `_callback_state` makes this type **not Send** (MainForwarder
/// holds a `glib::JoinHandle` which is ThreadGuard-protected).
/// That matches the documented FFI contract: HxnetConnection
/// pointers are single-threaded.
pub struct HxnetConnection {
    cmd: ConnectionHandle,
    events: Option<mpsc::Receiver<Event>>,
    /// Callback-mode state. None in polling mode.
    _callback_state: Option<CallbackState>,
    /// JoinHandle for the spawned actor task. Dropping a tokio
    /// `JoinHandle` only detaches the task (the task keeps
    /// running on the runtime until it exits naturally); this
    /// field exists so future FFI growth has the option to call
    /// `.abort()` synchronously on destroy, or `.await` the
    /// handle from a tokio-context shutdown path. R3.3.b doesn't
    /// use either today — destroy relies on dropping the
    /// `ConnectionHandle` to signal the actor and letting it
    /// flush + exit — so the underscore prefix marks it
    /// reserved-for-future-use.
    _join: JoinHandle<()>,
}

/// Callback-mode FFI state. Holds the tokio→async_channel pump
/// task and the GLib main-thread forwarder that invokes the C
/// callback per event. Both are dropped on destroy: pump's
/// JoinHandle drop detaches the task (which exits on its own
/// when the actor's event stream closes); MainForwarder's drop
/// aborts the spawn_local future on the GLib main context.
struct CallbackState {
    _pump: JoinHandle<()>,
    _forwarder: hxbridge::channel::MainForwarder,
}

/// Plain-old-data frame the C side reads after try_recv. Mirrors
/// the union of [`hotline_proto::parse::HeaderDecoded`] (header
/// fields the C side cares about) plus an owned body buffer.
///
/// `body_ptr` + `body_len` are owned by Rust. The C side must
/// call [`hxnet_frame_free`] when done with the frame; calling
/// `free()` directly is undefined behaviour.
#[repr(C)]
pub struct HxnetFrame {
    pub type_: u32,
    pub trans: u32,
    pub flag: u32,
    pub hc: u16,
    pub _pad: u16,
    pub body_len: u32,
    /// NULL if `body_len == 0`. Owned by Rust.
    pub body_ptr: *mut u8,
}

// Pin the cross-language ABI layout from the Rust side. Same
// pattern as `HeaderDecodedOut` / `HistoryEntryOut` /
// `TrackerRecordFixedOut` in hotline-proto. The fixed-size
// prefix is stable across targets; `body_ptr`'s offset and the
// struct's total size depend on pointer alignment (8 bytes on
// 64-bit targets, 4 on 32-bit) so we express those in terms of
// `align_of::<*mut u8>()` rather than hardcoding a 64-bit
// number. The C side mirrors this layout via _Static_assert in
// tests/unit/test_hxnet_ffi.c — drift on either side trips a
// compile error before any byte hits the wire.
const _: () = {
    assert!(std::mem::offset_of!(HxnetFrame, type_) == 0);
    assert!(std::mem::offset_of!(HxnetFrame, trans) == 4);
    assert!(std::mem::offset_of!(HxnetFrame, flag) == 8);
    assert!(std::mem::offset_of!(HxnetFrame, hc) == 12);
    assert!(std::mem::offset_of!(HxnetFrame, _pad) == 14);
    assert!(std::mem::offset_of!(HxnetFrame, body_len) == 16);
    // body_ptr follows body_len; the compiler inserts
    // alignment padding to align_of::<*mut u8>(). On 64-bit
    // targets that's 8 bytes of trailing-padding before the
    // pointer (offset 24); on 32-bit it's none (offset 20).
    let expected_body_ptr_off = if std::mem::align_of::<*mut u8>() == 8 {
        24
    } else {
        20
    };
    assert!(std::mem::offset_of!(HxnetFrame, body_ptr) == expected_body_ptr_off);
    assert!(
        std::mem::align_of::<HxnetFrame>() == std::mem::align_of::<*mut u8>()
    );
    // Total size: body_ptr offset + pointer size. Pinning this
    // catches trailing-padding drift (e.g. someone adding a u32
    // after body_ptr that bumps the size by 8 on 64-bit due to
    // alignment rules). Pairs with the C-side sizeof
    // _Static_assert in tests/unit/test_hxnet_ffi.c.
    let expected_size = expected_body_ptr_off + std::mem::size_of::<*mut u8>();
    assert!(std::mem::size_of::<HxnetFrame>() == expected_size);
};

// Event return codes from try_recv_frame. Mirrored on the C side
// by `tests/unit/test_hxnet_ffi.c` (kept in sync by hand — the
// FFI drift discipline matches the other hxbridge / hotline-proto
// surfaces).

/// No event was available right now. Try again later.
pub const HXNET_RECV_EMPTY: c_int = 0;
/// A frame was returned via `out_frame`. The C side owns it
/// until it calls [`hxnet_frame_free`].
pub const HXNET_RECV_FRAME: c_int = 1;
/// The actor stopped. After this code is returned, no more
/// events will arrive — the C side should call
/// [`hxnet_connection_destroy`] to clean up. The reason is
/// signalled via the matching `HXNET_SHUTDOWN_*` constants.
pub const HXNET_RECV_SHUTDOWN: c_int = 2;

/// Shutdown reason codes, written into `*out_reason` when
/// try_recv returns `HXNET_RECV_SHUTDOWN`.
pub const HXNET_SHUTDOWN_EOF: c_int = 0;
pub const HXNET_SHUTDOWN_STREAM_ERROR: c_int = 1;
pub const HXNET_SHUTDOWN_FRAME_TOO_LARGE: c_int = 2;
pub const HXNET_SHUTDOWN_HANDLE_DROPPED: c_int = 3;

/// Send a write command to the actor. Return codes:
///
/// - `0` success — the command was enqueued.
/// - `-1` channel full — try again later.
/// - `-2` channel closed — the actor exited.
/// - `-3` invalid args (NULL handle / NULL data with nonzero len).
pub const HXNET_SEND_OK: c_int = 0;
pub const HXNET_SEND_FULL: c_int = -1;
pub const HXNET_SEND_CLOSED: c_int = -2;
pub const HXNET_SEND_INVALID: c_int = -3;

/// Spawn a Connection actor on the shared tokio runtime, reading
/// from the supplied **connected TCP socket fd**. The fd is
/// wrapped as a `tokio::net::TcpStream`; passing a non-TCP socket
/// (e.g. an `AF_UNIX` `socketpair(2)` fd) is rejected up front
/// with a NULL return and a `g_critical` log — see the `peer_addr`
/// probe in the body for the runtime gate.
///
/// # fd ownership
///
/// Rust **always** takes ownership of `fd` when `fd >= 0`. On
/// success it lives inside the spawned `tokio::net::TcpStream`
/// and is closed when the actor exits. On the NULL-return failure
/// paths (set_nonblocking errored, peer_addr rejected the fd,
/// TcpStream::from_std rejected the fd, or Connection::spawn
/// couldn't find a runtime), the `std::net::TcpStream` we built
/// from `from_raw_fd` falls out of scope and Rust closes the fd
/// via the standard Drop. Either way, the C side **must not**
/// `close(2)` the fd after handing it to this function — that
/// would double-close and likely tear down an unrelated reused
/// fd value. The only fd the C side keeps responsibility for is
/// one passed with `fd < 0` (NULL return, no ownership transfer
/// attempted).
///
/// # Errors
///
/// Returns NULL (after a `g_critical` log) for the recoverable
/// failure modes: negative `fd`, `set_nonblocking` errored,
/// `peer_addr` failed (fd is not a connected TCP socket),
/// `TcpStream::from_std` rejected the fd, or
/// `Connection::spawn` couldn't find a tokio runtime context.
/// Caller-side recovery is to retry the connect — the failed fd
/// is already closed by Rust, so the caller starts fresh with a
/// new socket.
///
/// # Aborts
///
/// If `Runtime::global()` panics during lazy initialisation —
/// typically because the OS refuses to spawn the runtime's
/// worker thread — this function calls `std::process::abort()`
/// rather than unwinding across the C ABI boundary, which would
/// be undefined behaviour. The panic is logged via `g_critical`
/// before the abort. This is a process-fatal failure mode; the
/// C side never observes it as a return.
///
/// # Safety
///
/// `fd` must be a valid, **already-connected TCP socket fd**.
/// Passing a closed fd, a listening socket, a connected UDP
/// socket, or a non-AF_INET/AF_INET6 socket (such as an
/// `AF_UNIX` socketpair fd) is undefined behaviour — Rust
/// adopts the fd via `TcpStream::from_raw_fd` which assumes the
/// underlying file is a connected TCP socket. The `peer_addr`
/// probe in the body catches the most common misuse (non-TCP /
/// not-yet-connected) but is not a substitute for the caller
/// passing the right kind of fd to begin with.
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_spawn_fd(
    fd: c_int,
) -> *mut HxnetConnection {
    if fd < 0 {
        glib::g_critical!("hxnet", "hxnet_connection_spawn_fd: negative fd");
        return std::ptr::null_mut();
    }

    // Resolve the runtime singleton behind catch_unwind — same
    // pattern as hxbridge::blocking::spawn_blocking_with_idle so
    // a runtime init panic can't unwind across the C ABI.
    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(
        Runtime::global,
    )) {
        Ok(rt) => rt,
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_spawn_fd: Runtime::global panicked; \
                 aborting to avoid unwinding across the FFI boundary"
            );
            std::process::abort();
        }
    };

    // tokio::net::TcpStream::from_std requires the fd to be
    // already in non-blocking mode AND for the calling code to
    // have a runtime context (enter the handle below). We adopt
    // the fd via std::net::TcpStream::from_raw_fd, switch to
    // non-blocking, then hand it over.
    let std_stream = std::net::TcpStream::from_raw_fd(fd);
    if let Err(e) = std_stream.set_nonblocking(true) {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_spawn_fd: set_nonblocking failed: {}",
            e
        );
        // std_stream's Drop closes the fd here — Rust owns it now.
        return std::ptr::null_mut();
    }
    // Probe peer_addr() to reject non-TCP / not-yet-connected
    // fds. peer_addr returns ENOTCONN for unconnected sockets
    // and EOPNOTSUPP / EINVAL for non-stream / non-TCP types.
    // We don't need the address itself — just the success of the
    // syscall. A successful probe confirms (a) the fd is a
    // socket, (b) it supports the SOCK_STREAM getpeername
    // semantics, and (c) it's connected. This catches the
    // common smoke-test misuse (AF_UNIX socketpair, listening
    // socket) before the downstream tokio code wraps the fd as
    // TcpStream and produces confusing errors later.
    if let Err(e) = std_stream.peer_addr() {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_spawn_fd: peer_addr failed ({}); fd is not a \
             connected TCP socket",
            e
        );
        return std::ptr::null_mut();
    }

    let _guard = rt.handle().enter();
    let tcp = match TcpStream::from_std(std_stream) {
        Ok(s) => s,
        Err(e) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_spawn_fd: TcpStream::from_std failed: {}",
                e
            );
            return std::ptr::null_mut();
        }
    };

    let (cmd, events, join) = match Connection::spawn(tcp) {
        Ok(triple) => triple,
        Err(e) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_spawn_fd: Connection::spawn failed: {}",
                e
            );
            return std::ptr::null_mut();
        }
    };

    let handle = Box::new(HxnetConnection {
        cmd,
        events: Some(events),
        _callback_state: None,
        _join: join,
    });
    Box::into_raw(handle)
}

/// Non-blocking poll for the next event. Writes a frame (if one
/// arrived) into `*out_frame` and returns `HXNET_RECV_FRAME`.
///
/// If the actor has exited, writes the shutdown reason into
/// `*out_reason` and returns `HXNET_RECV_SHUTDOWN`. Otherwise
/// returns `HXNET_RECV_EMPTY`.
///
/// `out_frame` and `out_reason` must both be non-NULL. The
/// function fills only the matching one based on its return code;
/// the other is left untouched.
///
/// # Safety
///
/// `handle` must be a non-NULL pointer previously returned by
/// [`hxnet_connection_spawn_fd`] and not yet passed to
/// [`hxnet_connection_destroy`]. `out_frame` and `out_reason`
/// must be valid writable locations.
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_try_recv_frame(
    handle: *mut HxnetConnection,
    out_frame: *mut HxnetFrame,
    out_reason: *mut c_int,
) -> c_int {
    if handle.is_null() || out_frame.is_null() || out_reason.is_null() {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_try_recv_frame: NULL arg"
        );
        return HXNET_RECV_EMPTY;
    }

    let h = &mut *handle;
    let events = match h.events.as_mut() {
        Some(rx) => rx,
        None => {
            // Polling try_recv called on a callback-mode handle.
            // No receiver to drain — the actor's events go
            // straight to the C callback. Return EMPTY so any
            // stray polling consumer doesn't see corrupted
            // state; the right pattern is to use callback-mode
            // OR polling-mode, never both.
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_try_recv_frame: handle is in callback \
                 mode; no receiver to poll"
            );
            return HXNET_RECV_EMPTY;
        }
    };
    loop {
        match events.try_recv() {
            Ok(Event::Frame(frame)) => {
                write_frame_to_out(frame, out_frame);
                return HXNET_RECV_FRAME;
            }
            Ok(Event::Shutdown(reason)) => {
                *out_reason = shutdown_code(reason);
                return HXNET_RECV_SHUTDOWN;
            }
            Ok(Event::State(_)) => {
                // Polling API has no surface to expose
                // connection-state events on. The polling
                // spawn (`spawn_fd`) is only used by R3.3.b's
                // smoke test which adopts a pre-connected fd
                // and never sees connect-time state events. If
                // a future caller wires the polling API
                // through the Phase A connect path, we'll grow
                // a separate `try_recv_state` entry; for now
                // silently drop state events and continue the
                // try_recv loop so the caller still gets to
                // see Frame / Shutdown.
                continue;
            }
            Err(mpsc::error::TryRecvError::Empty) => return HXNET_RECV_EMPTY,
            Err(mpsc::error::TryRecvError::Disconnected) => {
                // The actor finished without emitting Shutdown (we
                // try our best to ensure it always does, but defend
                // anyway).
                *out_reason = HXNET_SHUTDOWN_HANDLE_DROPPED;
                return HXNET_RECV_SHUTDOWN;
            }
        }
    }
}

fn shutdown_code(reason: ShutdownReason) -> c_int {
    match reason {
        ShutdownReason::Eof => HXNET_SHUTDOWN_EOF,
        ShutdownReason::StreamError(_) => HXNET_SHUTDOWN_STREAM_ERROR,
        ShutdownReason::FrameTooLarge { .. } => HXNET_SHUTDOWN_FRAME_TOO_LARGE,
        ShutdownReason::HandleDropped => HXNET_SHUTDOWN_HANDLE_DROPPED,
    }
}

unsafe fn write_frame_to_out(frame: Frame, out: *mut HxnetFrame) {
    let mut body = frame.body.into_boxed_slice();
    let body_len = body.len() as u32;
    let body_ptr = if body.is_empty() {
        std::ptr::null_mut()
    } else {
        body.as_mut_ptr()
    };
    // Hand ownership to C. C must call hxnet_frame_free to drop.
    std::mem::forget(body);

    std::ptr::write(
        out,
        HxnetFrame {
            type_: frame.header.type_,
            trans: frame.header.trans,
            flag: frame.header.flag,
            hc: frame.header.hc,
            _pad: 0,
            body_len,
            body_ptr,
        },
    );
}

/// Free a frame previously returned via [`hxnet_connection_try_recv_frame`].
///
/// # Safety
///
/// `frame` must be a non-NULL pointer to an `HxnetFrame` populated
/// by [`hxnet_connection_try_recv_frame`] returning `HXNET_RECV_FRAME`.
///
/// Callers **must not** modify `body_ptr` or `body_len` between
/// receiving the frame and calling this function. The free path
/// reconstructs the original boxed slice allocation from those
/// exact two values; if `body_len` has been shrunk (or grown),
/// `Box::from_raw` would deallocate with the wrong layout, which
/// is undefined behaviour. The defensive `body_len > isize::MAX`
/// ceiling inside the function catches one egregious misuse but
/// is not a substitute for "leave the fields alone." The other
/// integer fields (`type_`, `trans`, `flag`, `hc`, `body_len`,
/// `_pad`) are fine to read; they aren't load-bearing for the
/// free.
///
/// Double-free is undefined behaviour. After this call, the
/// fields' meanings (especially `body_ptr`) are no longer valid
/// and must not be read.
#[no_mangle]
pub unsafe extern "C" fn hxnet_frame_free(frame: *mut HxnetFrame) {
    if frame.is_null() {
        return;
    }
    let f = &mut *frame;
    if !f.body_ptr.is_null() && f.body_len > 0 {
        // Defensive ceiling check: `std::slice::from_raw_parts_mut`
        // is documented UB when `len * size_of::<T>() > isize::MAX`.
        // A buggy/malicious C caller could corrupt `body_len`
        // between our return from try_recv_frame and the free call;
        // the actor's MAX_BODY_LEN cap means we never produce a
        // body that large ourselves, so any value past the ceiling
        // implies caller-side corruption. Log + leak the body
        // rather than reach into UB territory — same discipline as
        // hotline-proto's `as_slice` helper.
        if (f.body_len as u64) > (isize::MAX as u64) {
            glib::g_critical!(
                "hxnet",
                "hxnet_frame_free: body_len {} exceeds isize::MAX; \
                 refusing to construct slice, leaking body to avoid UB",
                f.body_len
            );
            f.body_ptr = std::ptr::null_mut();
            f.body_len = 0;
            return;
        }
        // Reconstitute the boxed slice we leaked in
        // write_frame_to_out, then let it drop.
        let slice = std::slice::from_raw_parts_mut(f.body_ptr, f.body_len as usize);
        drop(Box::from_raw(slice as *mut [u8]));
    }
    f.body_ptr = std::ptr::null_mut();
    f.body_len = 0;
}

/// Non-blocking enqueue of a write command. See the
/// `HXNET_SEND_*` constants for return codes.
///
/// # Safety
///
/// `handle` must be non-NULL and valid. `data` must point to at
/// least `len` bytes of readable memory. `len` may be zero, in
/// which case `data` is ignored.
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_send_frame(
    handle: *mut HxnetConnection,
    data: *const u8,
    len: c_uint,
) -> c_int {
    if handle.is_null() {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_send_frame: NULL handle"
        );
        return HXNET_SEND_INVALID;
    }
    if len > 0 && data.is_null() {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_send_frame: NULL data with nonzero len"
        );
        return HXNET_SEND_INVALID;
    }
    // `std::slice::from_raw_parts` is documented UB for
    // `len * size_of::<T>() > isize::MAX`. On 32-bit targets that
    // ceiling is 2 GiB; a malicious or buggy C caller passing a
    // very large `c_uint` could trip it. Reject explicitly with
    // INVALID — the actor's MAX_BODY_LEN cap (1 MiB) means no
    // legitimate frame ever approaches this bound anyway.
    if (len as u64) > (isize::MAX as u64) {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_send_frame: len {} exceeds isize::MAX",
            len
        );
        return HXNET_SEND_INVALID;
    }
    let h = &*handle;
    let bytes = if len == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(data, len as usize).to_vec()
    };
    match h.cmd.try_send(Command::WriteFrame(bytes)) {
        Ok(()) => HXNET_SEND_OK,
        Err(mpsc::error::TrySendError::Full(_)) => HXNET_SEND_FULL,
        Err(mpsc::error::TrySendError::Closed(_)) => HXNET_SEND_CLOSED,
    }
}

/// Drop the handle and abort its spawned task. The task is
/// cancelled at its next await point (see the body for why an
/// abort is required rather than just dropping the cmd sender), so
/// for a mid-handshake or otherwise-blocked actor it does NOT
/// necessarily observe `HandleDropped` or flush pending writes
/// before its transport is dropped and the socket closed. Any
/// frames still in the event channel are dropped too (the C side
/// already had its chance via [`hxnet_connection_try_recv_frame`]).
///
/// # Safety
///
/// `handle` must be a non-NULL pointer previously returned by
/// [`hxnet_connection_spawn_fd`] and not yet destroyed.
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_destroy(
    handle: *mut HxnetConnection,
) {
    if handle.is_null() {
        return;
    }
    let conn = Box::from_raw(handle);
    // Abort the spawned lifecycle/actor task. Dropping a JoinHandle
    // only *detaches* — the task keeps running. For a connection
    // that's mid-handshake (DNS / TCP connect / magic / LOGIN), the
    // task isn't yet polling the command channel, so dropping the
    // cmd sender below would NOT stop it: it would keep running,
    // holding the socket, and only self-terminate if/when it next
    // tried to emit an event (never, if it's blocked on a hung
    // connect or a silent server). That leak was the "hxnet in a bad
    // state after disconnecting a hung connect" bug. abort() forces
    // cancellation at the next await point and drops the task's
    // TcpStream/TlsStream, closing the socket. For an already-exited
    // actor (the normal shutdown path) abort() is a no-op.
    conn._join.abort();
    // Dropping the Box releases the cmd sender, the events receiver /
    // callback state (the forwarder aborts its spawn_local, the pump
    // detaches), all together.
    drop(conn);
}

// ============================================================
// Callback-driven FFI (R3.3.e)
// ============================================================
//
// The polling-form FFI above (R3.3.b) makes the C side
// responsible for draining events on a GLib idle. The
// callback form here flips that: hxnet drives the C side
// directly by invoking a registered callback on the GLib main
// thread per event.
//
// Wiring:
//   1. Connection actor (tokio task) emits Event into a
//      tokio::sync::mpsc.
//   2. Pump task (tokio task) forwards each Event into an
//      async_channel::Sender. The async_channel ferries the
//      data across the tokio↔GLib boundary because its
//      Receiver future polls cleanly on any executor.
//   3. forward_to_main (GLib main-thread spawn_local) reads
//      from the async_channel Receiver and invokes the C
//      callback per event. The closure also handles building
//      the HxnetFrame struct for Frame events.
//
// The handler closure captures the C function pointers and
// user_data inside a private SendCallbacks bundle so the
// `forward_to_main` closure has everything it needs in one
// place. Raw and function pointers are Send by default — the
// bundle picks up Send auto-derivation, no `unsafe impl Send`
// needed. See SendCallbacks's doc-comment for why we
// deliberately avoid the explicit marker impl.

/// C-side per-frame callback. Invoked on the GLib main thread
/// (the same thread `MainContext::ref_thread_default()` returned
/// at spawn time).
///
/// # Frame lifetime — read carefully
///
/// The `*mut HxnetFrame` argument points at a stack-allocated
/// `HxnetFrame` whose storage is destroyed the moment the
/// callback returns. The rule is: **don't keep the pointer past
/// return.** Two valid patterns:
///
/// 1. Process and free inside the callback — call
///    `hxnet_frame_free(frame)` on the supplied pointer before
///    returning. The pointer is valid for the duration of the
///    callback, so `hxnet_frame_free` here is safe; it only
///    touches the body allocation behind `body_ptr` (with
///    length `body_len`), not the stack-frame struct itself.
/// 2. Defer freeing by copying the integer fields plus the
///    `body_ptr`/`body_len` pair into a C-owned `HxnetFrame`
///    struct, then later call `hxnet_frame_free(&copy)` on
///    that copy. The body memory keeps the same address; only
///    the `HxnetFrame` *struct* needs to be the C-side copy.
///
/// What's forbidden is **storing the original `*mut HxnetFrame`
/// for use after the callback returns**, and *then* passing
/// that stale pointer to anything — at that point the stack
/// frame backing the struct is gone, so even though
/// `hxnet_frame_free` only touches `body_ptr` / `body_len`,
/// reading those fields through a dangling pointer is UB.
///
/// # Why `*mut`, not `*const`
///
/// `hxnet_frame_free` writes through the pointer (it zeroes
/// `body_ptr` and `body_len` on the supplied struct so a
/// follow-up double-free attempt is detectable). Marking the
/// argument `*const` here would force every correct caller
/// invoking `hxnet_frame_free(frame)` to cast away const — and
/// const-cast-then-write is UB in C even when the underlying
/// memory is writable. Taking `*mut` is the same ABI and
/// matches the ownership contract.
pub type HxnetEventCallback =
    Option<unsafe extern "C" fn(*mut HxnetConnection, *mut HxnetFrame, *mut c_void)>;

/// C-side per-shutdown callback. Invoked at most once, on the
/// GLib main thread, when the actor exits. After this callback
/// returns, no further callbacks will fire on this handle; the
/// C side should call `hxnet_connection_destroy` to clean up.
pub type HxnetShutdownCallback =
    Option<unsafe extern "C" fn(*mut HxnetConnection, c_int, *mut c_void)>;

/// Bundle of the C-side callback pointers + user_data we hand
/// to the forwarder closure. All fields are raw / function
/// pointers, so the compiler derives `Send` on its own — we
/// don't need (and deliberately don't add) an `unsafe impl
/// Send`. The reason: a future maintainer might add a non-Send
/// field here (a `Cell`, a `Rc`, anything) without realising
/// the marker impl would silently keep the type Send and hand
/// the non-Send value across a spawn boundary. Letting auto-
/// derivation do its job means such a field would correctly
/// trip a compile error and force a re-think.
///
/// In practice the spawn_local future runs on the same thread
/// as the FFI caller (per the documented thread-pinned
/// contract), so Send is structural rather than evidence of
/// cross-thread access — but that's an argument for being
/// careful about what we add to this struct, not an argument
/// for asserting Send manually.
struct SendCallbacks {
    on_event: HxnetEventCallback,
    on_shutdown: HxnetShutdownCallback,
    user_data: *mut c_void,
    handle_ptr: *mut HxnetConnection,
}

/// Default channel capacity for the tokio→GLib ferry. Big
/// enough to absorb a chat-history burst without blocking the
/// actor's read loop; small enough that the GLib main thread
/// can drain it predictably under load.
const CALLBACK_FERRY_CAPACITY: usize = 64;

/// Spawn a Connection actor like
/// [`hxnet_connection_spawn_fd`], but route events through a
/// pair of C callbacks invoked on the GLib main thread instead
/// of buffering them for polling retrieval. The fd contract
/// (TCP, connected, Rust takes ownership) and the abort-on-
/// runtime-panic semantics are identical.
///
/// `on_event` fires once per `Event::Frame` with a non-NULL
/// `frame` pointer; the C side owns the frame until calling
/// `hxnet_frame_free`. `on_shutdown` fires once with the
/// shutdown reason code (`HXNET_SHUTDOWN_*`); after it returns,
/// no further callbacks fire.
///
/// Both callback slots are required (NULL is rejected with
/// `g_critical` + NULL return). `user_data` is opaque and
/// passed through to both callbacks unchanged.
///
/// # Threading
///
/// The callbacks fire on the GLib main thread (whatever
/// `MainContext::ref_thread_default()` resolved to at spawn
/// time). C-side state touched from the callback should be
/// safe to access on that thread; the per-handle thread-pinned
/// contract documented in the module-level safety section
/// continues to apply.
///
/// # Safety
///
/// `fd` requirements identical to
/// [`hxnet_connection_spawn_fd`]. `on_event` and `on_shutdown`
/// must be valid C function pointers when non-NULL. `user_data`
/// must remain valid for the entire connection lifetime.
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_spawn_fd_with_callback(
    fd: c_int,
    on_event: HxnetEventCallback,
    on_shutdown: HxnetShutdownCallback,
    user_data: *mut c_void,
) -> *mut HxnetConnection {
    if on_event.is_none() || on_shutdown.is_none() {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_spawn_fd_with_callback: NULL callback"
        );
        return std::ptr::null_mut();
    }
    if fd < 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_spawn_fd_with_callback: negative fd"
        );
        return std::ptr::null_mut();
    }

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(
        Runtime::global,
    )) {
        Ok(rt) => rt,
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_spawn_fd_with_callback: Runtime::global \
                 panicked; aborting to avoid unwinding across the FFI \
                 boundary"
            );
            std::process::abort();
        }
    };

    // Same fd-prep as the polling variant — adopt as
    // std::net::TcpStream, switch to non-blocking, peer_addr
    // probe, then hand to tokio.
    let std_stream = std::net::TcpStream::from_raw_fd(fd);
    if let Err(e) = std_stream.set_nonblocking(true) {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_spawn_fd_with_callback: set_nonblocking \
             failed: {}",
            e
        );
        return std::ptr::null_mut();
    }
    if let Err(e) = std_stream.peer_addr() {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_spawn_fd_with_callback: peer_addr failed \
             ({}); fd is not a connected TCP socket",
            e
        );
        return std::ptr::null_mut();
    }

    let _guard = rt.handle().enter();
    let tcp = match TcpStream::from_std(std_stream) {
        Ok(s) => s,
        Err(e) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_spawn_fd_with_callback: TcpStream::\
                 from_std failed: {}",
                e
            );
            return std::ptr::null_mut();
        }
    };
    let (cmd, events, join) = match Connection::spawn(tcp) {
        Ok(triple) => triple,
        Err(e) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_spawn_fd_with_callback: \
                 Connection::spawn failed: {}",
                e
            );
            return std::ptr::null_mut();
        }
    };

    // Hand the post-spawn wiring (MainContext acquire,
    // Box::into_raw, pump spawn, forwarder attach,
    // callback_state stash) to the shared helper so this entry
    // and the transform-config variant below stay aligned. See
    // [`wire_callback_state`] for the rationale on ordering and
    // the cleanup-on-failure contract.
    wire_callback_state(rt, cmd, events, join, on_event, on_shutdown, user_data)
}

// ============================================================
// Transform-config FFI (R3.3.e-2)
// ============================================================
//
// Lets the C side pass a HOPE-negotiated cipher + compression
// stack and have hxnet hand the resulting layered transport to
// the Connection actor. The shape is a flat `#[repr(C)]` struct
// — all per-direction key material is held inline, the cipher /
// compression kind tags pick which subset of fields are
// load-bearing on any given call.
//
// Behaviourally identical to `spawn_fd_with_callback` once the
// transports are built — the same SendCallbacks + pump +
// forward_to_main wiring runs underneath.

/// Cipher selection tag, matches [`crate::transform::CipherKind`].
pub const HXNET_CIPHER_NONE: c_uint = 0;
pub const HXNET_CIPHER_BLOWFISH: c_uint = 1;
pub const HXNET_CIPHER_CHACHA20_POLY1305: c_uint = 2;
/// R3.3.e-4g: Hotline-frame-aware Blowfish that mirrors the
/// legacy HOPE per-message rekey protocol
/// ([`crate::hope_blowfish::HopeBlowfishStream`]). Use this
/// for HOPE-Blowfish handshakes; the bare
/// [`HXNET_CIPHER_BLOWFISH`] kind is wire-incompatible with any
/// HOPE server that ever trips the rekey marker.
pub const HXNET_CIPHER_HOPE_BLOWFISH: c_uint = 3;

/// HMAC algorithm tag. Matches
/// [`crate::hope_blowfish::HopeMacAlg`].
pub const HXNET_MACALG_SHA256: u8 = 0;
pub const HXNET_MACALG_SHA1: u8 = 1;
pub const HXNET_MACALG_MD5: u8 = 2;

/// Maximum HOPE session-key length the FFI accepts. The wire
/// protocol caps the SESSIONKEY chunk at 64 bytes, which the C
/// `htlc->sessionkey` field also pins; we mirror exactly.
pub const HXNET_HOPE_SESSION_KEY_MAX: c_uint = 64;

/// Compression selection tag, matches
/// [`crate::transform::CompressionKind`].
pub const HXNET_COMPRESSION_NONE: c_uint = 0;
pub const HXNET_COMPRESSION_GZIP: c_uint = 1;
pub const HXNET_COMPRESSION_LZ4: c_uint = 2;
pub const HXNET_COMPRESSION_ZSTD: c_uint = 3;

/// Maximum Blowfish key length the cipher accepts (matches
/// `Blowfish::new_from_slice`'s 1..=56 range).
pub const HXNET_BLOWFISH_MAX_KEY_LEN: c_uint = 56;

/// C-facing config for the layered transport. Flat POD struct so
/// the C side mirrors it with an explicit `_Static_assert` on
/// size + the relevant field offsets — same ABI-pinning
/// discipline as [`HxnetFrame`].
///
/// Field usage depends on the kind tags:
///
/// - `cipher_kind == HXNET_CIPHER_BLOWFISH` reads
///   `blowfish_key[..blowfish_key_len]`, `blowfish_read_ivec`,
///   `blowfish_write_ivec`.
/// - `cipher_kind == HXNET_CIPHER_CHACHA20_POLY1305` reads
///   `aead_read_*` / `aead_write_*`.
/// - Either compression tag uses no extra fields — the codec
///   is fully described by the tag.
///
/// All fields not used by the active variant are ignored; the C
/// side may leave them zeroed.
#[repr(C)]
pub struct HxnetTransformConfig {
    pub cipher_kind: c_uint,
    pub compression_kind: c_uint,

    /// Per-direction Blowfish key lengths in bytes
    /// (1..=`HXNET_BLOWFISH_MAX_KEY_LEN`). The HOPE chain derives
    /// distinct keys for the two directions (client→server and
    /// server→client); a single-key shortcut would desynchronise
    /// the read keystream from anything the server sends — caught
    /// against VesperNet/Janus, where the keys clearly differ.
    pub blowfish_read_key_len: c_uint,
    pub blowfish_write_key_len: c_uint,
    /// Per-direction Blowfish key buffers; only the first
    /// `blowfish_*_key_len` bytes of each are used.
    pub blowfish_read_key: [u8; 56],
    pub blowfish_write_key: [u8; 56],
    /// Per-direction OFB ivecs. The legacy HOPE handshake
    /// derives these from the session keystream.
    pub blowfish_read_ivec: [u8; 8],
    pub blowfish_write_ivec: [u8; 8],
    /// Per-direction OFB byte-offset (0..7) into the current
    /// keystream block. The bridge install can fire mid-block
    /// — the C side may have decoded post-LOGIN-reply bytes (or
    /// encoded step-2 LOGIN bytes) before `install_check_idle`'s
    /// htlc->out drain wait completes, advancing `num` past 0.
    /// Restoring with `num=0` would re-encrypt the ivec block on
    /// the next crypt() and desynchronise the keystream. Caught
    /// by VesperNet/Janus + Blowfish: Janus ships an empty
    /// HTLS_DATA_CIPHER_IVEC and the post-LOGIN SELFINFO is
    /// decoded by the legacy path before install fires.
    pub blowfish_read_num: u8,
    pub blowfish_write_num: u8,

    /// R3.3.e-4g: HOPE-Blowfish per-message rekey inputs.
    /// `hope_macalg` is one of `HXNET_MACALG_*`; the session
    /// key (HOPE-Step-1 reply SESSIONKEY chunk, up to 64 bytes)
    /// is hashed against the current direction key for each
    /// HMAC iteration the marker triggers. Only consulted when
    /// `cipher_kind == HXNET_CIPHER_HOPE_BLOWFISH`.
    pub hope_macalg: u8,
    /// 1 byte of padding so `hope_session_key_len` lands on a
    /// 4-byte boundary.
    pub _pad_macalg: u8,
    pub hope_session_key_len: c_uint,
    pub hope_session_key: [u8; 64],

    /// ChaCha20-Poly1305 keys per direction.
    pub aead_read_key: [u8; 32],
    pub aead_write_key: [u8; 32],
    /// Frame counters per direction.
    pub aead_read_counter: u64,
    pub aead_write_counter: u64,
    /// Direction tags — `AEAD_DIR_SERVER_TO_CLIENT` /
    /// `AEAD_DIR_CLIENT_TO_SERVER` from hxcrypto-aead.
    pub aead_read_dir: u8,
    pub aead_write_dir: u8,

    /// Explicit trailing padding so the struct's total size is
    /// stable and the C side's `_Static_assert sizeof(...)`
    /// catches drift.
    pub _pad: [u8; 6],
}

// Pin the cross-language ABI layout from the Rust side; the C
// side mirrors with the same offsets. Drift on either side
// trips a compile error before any byte hits the wire.
const _: () = {
    assert!(std::mem::offset_of!(HxnetTransformConfig, cipher_kind) == 0);
    assert!(std::mem::offset_of!(HxnetTransformConfig, compression_kind) == 4);
    assert!(std::mem::offset_of!(HxnetTransformConfig, blowfish_read_key_len) == 8);
    assert!(std::mem::offset_of!(HxnetTransformConfig, blowfish_write_key_len) == 12);
    assert!(std::mem::offset_of!(HxnetTransformConfig, blowfish_read_key) == 16);
    assert!(std::mem::offset_of!(HxnetTransformConfig, blowfish_write_key) == 72);
    assert!(std::mem::offset_of!(HxnetTransformConfig, blowfish_read_ivec) == 128);
    assert!(std::mem::offset_of!(HxnetTransformConfig, blowfish_write_ivec) == 136);
    assert!(std::mem::offset_of!(HxnetTransformConfig, blowfish_read_num) == 144);
    assert!(std::mem::offset_of!(HxnetTransformConfig, blowfish_write_num) == 145);
    assert!(std::mem::offset_of!(HxnetTransformConfig, hope_macalg) == 146);
    assert!(std::mem::offset_of!(HxnetTransformConfig, _pad_macalg) == 147);
    assert!(std::mem::offset_of!(HxnetTransformConfig, hope_session_key_len) == 148);
    assert!(std::mem::offset_of!(HxnetTransformConfig, hope_session_key) == 152);
    // hope_session_key is 64 bytes → ends at 216. aead_read_key
    // ([u8; 32]) has 1-byte alignment so it falls right at 216
    // with no padding.
    assert!(std::mem::offset_of!(HxnetTransformConfig, aead_read_key) == 216);
    assert!(std::mem::offset_of!(HxnetTransformConfig, aead_write_key) == 248);
    // aead_read_counter is u64 — needs 8-byte alignment. The
    // aead_write_key array ends at 248+32=280, which is
    // already 8-aligned, so no padding is inserted before
    // the counter.
    assert!(std::mem::offset_of!(HxnetTransformConfig, aead_read_counter) == 280);
    assert!(std::mem::offset_of!(HxnetTransformConfig, aead_write_counter) == 288);
    assert!(std::mem::offset_of!(HxnetTransformConfig, aead_read_dir) == 296);
    assert!(std::mem::offset_of!(HxnetTransformConfig, aead_write_dir) == 297);
    assert!(std::mem::offset_of!(HxnetTransformConfig, _pad) == 298);
    // Total: 298 + 6 = 304, aligned to the struct's natural
    // 8-byte alignment.
    assert!(std::mem::size_of::<HxnetTransformConfig>() == 304);
    assert!(std::mem::align_of::<HxnetTransformConfig>() == 8);
};

/// Translate the C-side config into a [`crate::transform::CipherLayer`].
/// Returns the layer plus a descriptive label for `g_critical`
/// messages on the failure path. This helper is the validation
/// boundary: it accepts a raw [`HxnetTransformConfig`], rejects
/// unknown `cipher_kind` tags, out-of-range `blowfish_key_len`,
/// failed Blowfish state init, and AEAD direction tags outside
/// the two canonical SERVER_TO_CLIENT / CLIENT_TO_SERVER values.
/// Each failure carries a static label that the caller in
/// [`hxnet_connection_spawn_fd_with_transforms_and_callback`]
/// routes into a `g_critical` and a NULL return.
fn cipher_layer_from_config(
    cfg: &HxnetTransformConfig,
) -> Result<crate::transform::CipherLayer, &'static str> {
    match cfg.cipher_kind {
        HXNET_CIPHER_NONE => Ok(crate::transform::CipherLayer::None),
        HXNET_CIPHER_BLOWFISH | HXNET_CIPHER_HOPE_BLOWFISH => {
            let read_key_len = cfg.blowfish_read_key_len as usize;
            let write_key_len = cfg.blowfish_write_key_len as usize;
            if read_key_len == 0
                || read_key_len > HXNET_BLOWFISH_MAX_KEY_LEN as usize
            {
                return Err("blowfish_read_key_len out of range (1..=56)");
            }
            if write_key_len == 0
                || write_key_len > HXNET_BLOWFISH_MAX_KEY_LEN as usize
            {
                return Err("blowfish_write_key_len out of range (1..=56)");
            }
            let read_key = &cfg.blowfish_read_key[..read_key_len];
            let write_key = &cfg.blowfish_write_key[..write_key_len];
            // Use the per-direction num verbatim. restore_ofb_state
            // already masks to 0..7 internally so an over-range
            // value can't index ivec out of bounds — same defense
            // the rollback path relies on. Threading num through
            // is the fix for the Janus + Blowfish disconnect: the
            // C-side may have advanced num past 0 by the time
            // install_check_idle fires; restoring num=0 here would
            // re-encrypt ivec on the next crypt() and desync the
            // keystream against the server. The per-direction key
            // split is the other half of the same fix — HOPE
            // derives distinct read/write Blowfish keys from the
            // session keystream, so loading the same key into both
            // states gives the read side a wrong keystream against
            // any server that doesn't happen to derive identical
            // keys (the Janus case).
            let read_state = match hxcrypto_stream::BlowfishOfb64State::new(read_key) {
                Some(mut s) => {
                    s.restore_ofb_state(&cfg.blowfish_read_ivec,
                                        cfg.blowfish_read_num as u32);
                    s
                }
                None => return Err("blowfish read state init failed"),
            };
            let write_state = match hxcrypto_stream::BlowfishOfb64State::new(write_key) {
                Some(mut s) => {
                    s.restore_ofb_state(&cfg.blowfish_write_ivec,
                                        cfg.blowfish_write_num as u32);
                    s
                }
                None => return Err("blowfish write state init failed"),
            };
            // Branch on the kind to pick the right wrapper. The
            // plain Blowfish path stays for non-HOPE use cases
            // (e.g. a future HTXF subchannel where there's no
            // rekey marker to worry about); the HOPE path
            // additionally captures the session key + HMAC alg
            // the per-message rekey rotation needs.
            if cfg.cipher_kind == HXNET_CIPHER_HOPE_BLOWFISH {
                let sk_len = cfg.hope_session_key_len as usize;
                if sk_len == 0
                    || sk_len > HXNET_HOPE_SESSION_KEY_MAX as usize
                {
                    return Err(
                        "hope_session_key_len out of range (1..=64)",
                    );
                }
                let macalg = match cfg.hope_macalg {
                    HXNET_MACALG_SHA256 => {
                        crate::hope_blowfish::HopeMacAlg::Sha256
                    }
                    HXNET_MACALG_SHA1 => {
                        crate::hope_blowfish::HopeMacAlg::Sha1
                    }
                    HXNET_MACALG_MD5 => {
                        crate::hope_blowfish::HopeMacAlg::Md5
                    }
                    _ => {
                        return Err("hope_macalg is not a known tag");
                    }
                };
                let session_key =
                    cfg.hope_session_key[..sk_len].to_vec();
                Ok(crate::transform::CipherLayer::HopeBlowfish {
                    read_state,
                    read_key: read_key.to_vec(),
                    write_state,
                    write_key: write_key.to_vec(),
                    session_key,
                    macalg,
                })
            } else {
                Ok(crate::transform::CipherLayer::Blowfish {
                    read_state,
                    write_state,
                })
            }
        }
        HXNET_CIPHER_CHACHA20_POLY1305 => {
            // The dir tag is one byte of the ChaCha20-Poly1305
            // nonce derivation; a value outside the defined set
            // would derive a non-interoperable (and potentially
            // unsafe) nonce. Reject anything other than the
            // two canonical SERVER_TO_CLIENT / CLIENT_TO_SERVER
            // tags before handing them to AeadState.
            if cfg.aead_read_dir != hxcrypto_aead::AEAD_DIR_SERVER_TO_CLIENT
                && cfg.aead_read_dir
                    != hxcrypto_aead::AEAD_DIR_CLIENT_TO_SERVER
            {
                return Err("aead_read_dir is not a defined direction tag");
            }
            if cfg.aead_write_dir != hxcrypto_aead::AEAD_DIR_SERVER_TO_CLIENT
                && cfg.aead_write_dir
                    != hxcrypto_aead::AEAD_DIR_CLIENT_TO_SERVER
            {
                return Err("aead_write_dir is not a defined direction tag");
            }
            // For a working bidirectional channel the read and
            // write directions must disagree — same direction
            // tag on both ends produces matching nonces, which
            // breaks the AEAD's per-direction counter
            // invariant. Refuse the obviously-broken config
            // up front.
            if cfg.aead_read_dir == cfg.aead_write_dir {
                return Err(
                    "aead_read_dir and aead_write_dir must disagree",
                );
            }
            Ok(crate::transform::CipherLayer::ChaCha20Poly1305 {
                read: hxcrypto_aead::AeadState {
                    key: cfg.aead_read_key,
                    counter: cfg.aead_read_counter,
                    dir: cfg.aead_read_dir,
                },
                write: hxcrypto_aead::AeadState {
                    key: cfg.aead_write_key,
                    counter: cfg.aead_write_counter,
                    dir: cfg.aead_write_dir,
                },
            })
        }
        _ => Err("unknown cipher_kind"),
    }
}

fn compression_kind_from_config(
    cfg: &HxnetTransformConfig,
) -> Result<crate::transform::CompressionKind, &'static str> {
    match cfg.compression_kind {
        HXNET_COMPRESSION_NONE => Ok(crate::transform::CompressionKind::None),
        HXNET_COMPRESSION_GZIP => Ok(crate::transform::CompressionKind::Gzip),
        HXNET_COMPRESSION_LZ4 => Ok(crate::transform::CompressionKind::Lz4),
        HXNET_COMPRESSION_ZSTD => Ok(crate::transform::CompressionKind::Zstd),
        _ => Err("unknown compression_kind"),
    }
}

/// Like [`hxnet_connection_spawn_fd_with_callback`], but wraps
/// the adopted TCP socket in a cipher + compression stack
/// described by `config` before handing it to the actor.
///
/// `config` is dereferenced read-only; the C side may free /
/// reuse its storage as soon as this function returns. NULL
/// `config` is rejected with `g_critical` + NULL.
///
/// All fd / callback / runtime semantics are identical to the
/// non-transform variant. The layered transport is built via
/// [`crate::transform::compose`]; the new failure modes this
/// surfaces beyond the base spawn path are:
///
/// - unknown `cipher_kind` or `compression_kind` tag,
/// - `blowfish_key_len` outside the 1..=56 range,
/// - Blowfish read / write state initialisation failure,
/// - `aead_read_dir` / `aead_write_dir` outside the canonical
///   `SERVER_TO_CLIENT` / `CLIENT_TO_SERVER` direction tags,
/// - Zstd decoder init failure.
///
/// All log via `g_critical` and return NULL.
///
/// # Safety
///
/// `config` must point at a valid, fully-initialised
/// `HxnetTransformConfig`. All other parameters carry the same
/// safety preconditions as
/// [`hxnet_connection_spawn_fd_with_callback`].
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_spawn_fd_with_transforms_and_callback(
    fd: c_int,
    config: *const HxnetTransformConfig,
    on_event: HxnetEventCallback,
    on_shutdown: HxnetShutdownCallback,
    user_data: *mut c_void,
) -> *mut HxnetConnection {
    if config.is_null() {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_spawn_fd_with_transforms_and_callback: NULL config"
        );
        return std::ptr::null_mut();
    }
    if on_event.is_none() || on_shutdown.is_none() {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_spawn_fd_with_transforms_and_callback: NULL \
             callback"
        );
        return std::ptr::null_mut();
    }
    if fd < 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_spawn_fd_with_transforms_and_callback: negative fd"
        );
        return std::ptr::null_mut();
    }

    let cfg = &*config;
    let cipher_layer = match cipher_layer_from_config(cfg) {
        Ok(l) => l,
        Err(msg) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_spawn_fd_with_transforms_and_callback: {}",
                msg
            );
            return std::ptr::null_mut();
        }
    };
    let compression_kind = match compression_kind_from_config(cfg) {
        Ok(k) => k,
        Err(msg) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_spawn_fd_with_transforms_and_callback: {}",
                msg
            );
            return std::ptr::null_mut();
        }
    };

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(
        Runtime::global,
    )) {
        Ok(rt) => rt,
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_spawn_fd_with_transforms_and_callback: \
                 Runtime::global panicked; aborting to avoid unwinding \
                 across the FFI boundary"
            );
            std::process::abort();
        }
    };

    // Same fd-prep + TCP wrap as the non-transform variant.
    let std_stream = std::net::TcpStream::from_raw_fd(fd);
    if let Err(e) = std_stream.set_nonblocking(true) {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_spawn_fd_with_transforms_and_callback: \
             set_nonblocking failed: {}",
            e
        );
        return std::ptr::null_mut();
    }
    if let Err(e) = std_stream.peer_addr() {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_spawn_fd_with_transforms_and_callback: \
             peer_addr failed ({}); fd is not a connected TCP socket",
            e
        );
        return std::ptr::null_mut();
    }

    let _guard = rt.handle().enter();
    let tcp = match TcpStream::from_std(std_stream) {
        Ok(s) => s,
        Err(e) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_spawn_fd_with_transforms_and_callback: \
                 TcpStream::from_std failed: {}",
                e
            );
            return std::ptr::null_mut();
        }
    };

    // Layer the cipher + compression on top of the raw TCP.
    let boxed = match crate::transform::compose(tcp, cipher_layer, compression_kind)
    {
        Ok(b) => b,
        Err(e) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_spawn_fd_with_transforms_and_callback: \
                 transform compose failed: {}",
                e
            );
            return std::ptr::null_mut();
        }
    };

    let (cmd, events, join) = match Connection::spawn_boxed(boxed) {
        Ok(triple) => triple,
        Err(e) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_spawn_fd_with_transforms_and_callback: \
                 Connection::spawn_boxed failed: {}",
                e
            );
            return std::ptr::null_mut();
        }
    };

    wire_callback_state(rt, cmd, events, join, on_event, on_shutdown, user_data)
}

/// Common post-Connection-spawn callback wiring. Builds the
/// HxnetConnection box, spawns the pump task, acquires the
/// MainContext, attaches the forwarder. Returns the C-side
/// handle pointer or NULL on the MainContext-acquisition
/// failure path (with `g_critical` already logged). Same
/// shape used by both
/// [`hxnet_connection_spawn_fd_with_callback`] and the
/// transform-config variant above so the wiring stays in one
/// place.
fn wire_callback_state(
    rt: &Runtime,
    cmd: ConnectionHandle,
    mut events: mpsc::Receiver<Event>,
    join: JoinHandle<()>,
    on_event: HxnetEventCallback,
    on_shutdown: HxnetShutdownCallback,
    user_data: *mut c_void,
) -> *mut HxnetConnection {
    // Acquire MainContext before Box::into_raw so a failure
    // doesn't leak the handle — see the matching comment in
    // hxnet_connection_spawn_fd_with_callback for the rationale.
    let main_ctx = glib::MainContext::ref_thread_default();
    let _acquire_guard = if main_ctx.is_owner() {
        None
    } else {
        match main_ctx.acquire() {
            Ok(g) => Some(g),
            Err(_) => {
                glib::g_critical!(
                    "hxnet",
                    "wire_callback_state: thread-default MainContext is owned \
                     by another thread; cannot acquire for spawn_local. The \
                     Connection was spawned on the tokio runtime but is being \
                     dropped because the GLib wiring cannot attach."
                );
                return std::ptr::null_mut();
            }
        }
    };

    let handle_box = Box::new(HxnetConnection {
        cmd,
        events: None,
        _callback_state: None,
        _join: join,
    });
    let handle_ptr = Box::into_raw(handle_box);

    let (ferry_tx, ferry_rx) =
        async_channel::bounded::<Event>(CALLBACK_FERRY_CAPACITY);
    let pump = rt.handle().spawn(async move {
        while let Some(evt) = events.recv().await {
            if ferry_tx.send(evt).await.is_err() {
                break;
            }
        }
    });

    let cb = SendCallbacks {
        on_event,
        on_shutdown,
        user_data,
        handle_ptr,
    };

    let forwarder =
        hxbridge::channel::forward_to_main(&main_ctx, ferry_rx, move |evt| {
            match evt {
                Event::Frame(frame) => {
                    let mut out = std::mem::MaybeUninit::<HxnetFrame>::uninit();
                    unsafe { write_frame_to_out(frame, out.as_mut_ptr()) };
                    // Hand the raw *mut pointer to the callback —
                    // hxnet_frame_free writes through the struct
                    // to zero body_ptr / body_len, so a *mut
                    // here matches the callback's documented
                    // contract and saves the C side a const-cast.
                    let frame_ptr = out.as_mut_ptr();
                    if let Some(on_event) = cb.on_event {
                        unsafe {
                            on_event(cb.handle_ptr, frame_ptr, cb.user_data);
                        }
                    }
                }
                Event::Shutdown(reason) => {
                    // Log the *full* reason — including the
                    // StreamError's inner io::Error string —
                    // before we collapse it to an integer code
                    // for the FFI callback. The integer alone
                    // tells the C side which BUCKET the actor
                    // died in (StreamError vs FrameTooLarge vs
                    // EOF) but not which specific failure
                    // produced it. Without this stderr line
                    // the actual cause of a mid-burst death is
                    // unrecoverable from user logs — the C
                    // side's bridge_on_shutdown_cb may also
                    // get pre-empted by a synchronous send
                    // failure path that uninstalls the bridge
                    // and swallows the deferred event entirely.
                    eprintln!("hxnet: actor shutting down: {reason:?}");
                    let code = shutdown_code(reason);
                    if let Some(on_shutdown) = cb.on_shutdown {
                        unsafe {
                            on_shutdown(cb.handle_ptr, code, cb.user_data);
                        }
                    }
                }
                Event::State(state) => {
                    // The post-HOPE callback FFI from R3.3.e-1
                    // only carries Frame + Shutdown; the spawn
                    // path adopts an already-connected fd so no
                    // connect-time state events ever fire here.
                    // When Phase A's connect-in-Rust spawn
                    // function lands, it'll carry its own
                    // on_state callback and route state events
                    // through that. For now log + drop so any
                    // future regression that wires a state-
                    // event-emitting transform under the
                    // existing spawn surfaces in dev logs
                    // rather than silently disappearing.
                    eprintln!(
                        "hxnet: unexpected State({state:?}) on post-HOPE \
                         callback path; ignoring (no consumer)"
                    );
                }
            }
        });

    let handle_ref = unsafe { &mut *handle_ptr };
    handle_ref._callback_state = Some(CallbackState {
        _pump: pump,
        _forwarder: forwarder,
    });

    handle_ptr
}

// ============================================================ *
// Phase A — hxnet does the TCP connect itself                  *
// ============================================================ *

/// Callback fired on each connection-state transition. The C
/// side typically maps these onto the existing
/// `gtkhx_session_emit_connection_state` GtkhxSession signals
/// the toolbar / chat windows already listen to for throbber /
/// status text.
///
/// `state` is the integer discriminant of
/// [`crate::ConnectionState`] — see that enum's
/// `#[repr(u32)]` for the exact values. Fired on the GLib main
/// thread (forwarded through `forward_to_main`), same
/// shape as `on_event` / `on_shutdown`.
pub type HxnetStateCallback = Option<
    unsafe extern "C" fn(conn: *mut HxnetConnection, state: c_uint, user_data: *mut c_void),
>;

/// TLS certificate trust callback (TOFU bridge). Invoked at most once,
/// on the tokio lifecycle task, right after the TLS handshake completes
/// and before any LOGIN is sent — but ONLY when WebPKI validation
/// against the native roots failed. A CA-valid cert is trusted silently
/// and this callback is never called. Gets the peer leaf cert's SHA-256
/// fingerprint as a `"sha256:<hex>"` byte string (NOT NUL-terminated
/// — use `fp_len`). Returns non-zero to accept the connection, zero
/// to reject (the lifecycle then closes the stream before LOGIN).
///
/// The C side keys this to its known-hosts TOFU store
/// (`hx_tls_orchestrator_verify_cert` → `hx_tls_trust_lookup`) and
/// may prompt the user (marshalled to the GLib main thread). It runs
/// on the tokio thread, so it may block on that prompt — fine during
/// a single connection's handshake.
pub type HxnetVerifyCertCallback =
    Option<unsafe extern "C" fn(fp: *const u8, fp_len: usize, user_data: *mut c_void) -> c_int>;

/// `Send` wrapper for the opaque C `user_data` pointer so the TLS
/// verify closure (which captures it) can move into the spawned
/// lifecycle task. Raw pointers aren't `Send`; the FFI contract
/// already requires `user_data` to outlive the connection and be
/// used single-threaded, and the verify closure only ever runs on
/// the one lifecycle task.
struct SendUserData(*mut c_void);
unsafe impl Send for SendUserData {}

/// Connection-state mirror constants for the C side, matching the
/// `#[repr(u32)]` discriminants on [`crate::ConnectionState`]. The
/// `const _` block below asserts that match at compile time, so a
/// Rust-side reorder that forgets to bump a constant fails the
/// build. NOTE: that assert only ties these constants to the Rust
/// enum — the C consumer's own copy of these values (in
/// `src/hxnet_bridge.c`) is hand-maintained with no automated
/// cross-language check, so keep it in sync by hand.
pub const HXNET_STATE_RESOLVING: c_uint = 0;
pub const HXNET_STATE_CONNECTING: c_uint = 1;
pub const HXNET_STATE_CONNECTED: c_uint = 2;
pub const HXNET_STATE_TLS_HANDSHAKING: c_uint = 3;
pub const HXNET_STATE_MAGIC_EXCHANGE: c_uint = 4;
pub const HXNET_STATE_LOGIN_SENDING: c_uint = 5;
pub const HXNET_STATE_LOGIN_REPLY_WAIT: c_uint = 6;
pub const HXNET_STATE_HOPE_STEP1: c_uint = 7;
pub const HXNET_STATE_HOPE_STEP2: c_uint = 8;
pub const HXNET_STATE_CIPHER_TRANSITION: c_uint = 9;
pub const HXNET_STATE_HANDSHAKE_DONE: c_uint = 10;

const _: () = {
    // Pin the discriminant mapping at compile time. Any reorder
    // of ConnectionState variants without a matching update to
    // the constants above trips a build error here rather than
    // a runtime ABI surprise.
    assert!(crate::ConnectionState::Resolving as u32 == HXNET_STATE_RESOLVING);
    assert!(crate::ConnectionState::Connecting as u32 == HXNET_STATE_CONNECTING);
    assert!(crate::ConnectionState::Connected as u32 == HXNET_STATE_CONNECTED);
    assert!(crate::ConnectionState::TlsHandshaking as u32 == HXNET_STATE_TLS_HANDSHAKING);
    assert!(crate::ConnectionState::MagicExchange as u32 == HXNET_STATE_MAGIC_EXCHANGE);
    assert!(crate::ConnectionState::LoginSending as u32 == HXNET_STATE_LOGIN_SENDING);
    assert!(crate::ConnectionState::LoginReplyWait as u32 == HXNET_STATE_LOGIN_REPLY_WAIT);
    assert!(crate::ConnectionState::HopeStep1 as u32 == HXNET_STATE_HOPE_STEP1);
    assert!(crate::ConnectionState::HopeStep2 as u32 == HXNET_STATE_HOPE_STEP2);
    assert!(crate::ConnectionState::CipherTransition as u32 == HXNET_STATE_CIPHER_TRANSITION);
    assert!(crate::ConnectionState::HandshakeDone as u32 == HXNET_STATE_HANDSHAKE_DONE);
};

/// Open a Hotline connection with hxnet driving the entire pre-
/// frame lifecycle. Phase A scope: DNS resolution + TCP connect
/// only. The actor enters frame mode immediately after the TCP
/// handshake completes — subsequent phases (B-F) layer TLS /
/// magic / LOGIN / HOPE on top before the frame-mode transition.
///
/// `host` is a non-NUL-terminated UTF-8 slice of length
/// `host_len`. It can be a DNS name or an IP literal (v4 or v6).
/// `port` is the TCP port. The three callbacks are wired exactly
/// the same way as in `hxnet_connection_spawn_fd_with_callback`;
/// `on_state` is new and fires once per `ConnectionState`
/// transition.
///
/// The returned handle is fully functional from the moment of
/// return — `hxnet_connection_send_frame` calls queue in the
/// command channel and get processed once the actor comes
/// online (i.e. once the TCP connect completes). DNS / connect
/// failures surface as `Event::Shutdown(StreamError("..."))`
/// before any frames flow; the on_shutdown callback fires
/// with the matching reason code.
///
/// # Safety
///
/// `host` must point at `host_len` readable bytes of valid
/// UTF-8 (or an IP literal — which is also valid UTF-8). The
/// three callback pointers and `user_data` must remain valid
/// for the connection's lifetime; production scopes them to
/// the `htlc_conn` lifetime which outlives the actor.
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_open_tcp(
    host: *const u8,
    host_len: usize,
    port: u16,
    on_event: HxnetEventCallback,
    on_shutdown: HxnetShutdownCallback,
    on_state: HxnetStateCallback,
    user_data: *mut c_void,
) -> *mut HxnetConnection {
    if host.is_null() || host_len == 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_tcp: NULL or empty host"
        );
        return std::ptr::null_mut();
    }
    if on_event.is_none() || on_shutdown.is_none() {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_tcp: NULL on_event / on_shutdown"
        );
        return std::ptr::null_mut();
    }
    // on_state is optional — consumers that don't care about
    // state transitions (e.g. R3.3.b's smoke test before it
    // gets ported) can pass NULL.

    // slice::from_raw_parts is documented UB for
    // `len * size_of::<T>() > isize::MAX`. Reject explicitly — same
    // defensive guard the other FFI entry points use — so a bogus
    // host_len from C is a logged error, not UB.
    if (host_len as u64) > (isize::MAX as u64) {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_tcp: host_len {} exceeds isize::MAX",
            host_len
        );
        return std::ptr::null_mut();
    }

    let host_slice = std::slice::from_raw_parts(host, host_len);
    let host_str = match std::str::from_utf8(host_slice) {
        Ok(s) => s.to_string(),
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_tcp: host is not valid UTF-8"
            );
            return std::ptr::null_mut();
        }
    };

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(
        Runtime::global,
    )) {
        Ok(rt) => rt,
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_tcp: Runtime::global panicked; \
                 aborting to avoid unwinding across the FFI boundary"
            );
            std::process::abort();
        }
    };

    // Create the channels + handle up front so the C side gets a
    // working ConnectionHandle immediately. Buffered sends queue
    // in the channel until the actor comes online post-connect.
    let (cmd, events, cmd_rx, evt_tx) = Connection::make_channels();

    let join = rt.handle().spawn(async move {
        match crate::connect::resolve_and_connect(&host_str, port, &evt_tx).await {
            Ok(stream) => {
                // Emit Connected here (vs. inside resolve_and_connect)
                // so the future post-connect setup (TLS / etc.)
                // gets a single state-event surface to thread
                // through. Phase A just spawns the actor right
                // after.
                let _ = evt_tx.send(Event::State(crate::ConnectionState::Connected)).await;
                Connection::run_actor(stream, cmd_rx, evt_tx).await;
            }
            Err(e) => {
                let _ = evt_tx
                    .send(Event::Shutdown(crate::ShutdownReason::StreamError(
                        format!("connect failed: {e}"),
                    )))
                    .await;
            }
        }
    });

    wire_callback_state_with_on_state(
        rt, cmd, events, join, on_event, on_shutdown, on_state, user_data,
    )
}

/// Open a Hotline connection with hxnet driving the full
/// plaintext-login lifecycle (Phase G of
/// `hxnet-owns-the-whole-lifecycle`).
///
/// This is the next step up from
/// [`hxnet_connection_open_tcp`]: instead of stopping after
/// TCP connect, the spawned task walks magic exchange + LOGIN
/// send + LOGIN reply receive before handing the stream to
/// the frame-mode actor. State events fire at every transition
/// (Resolving → Connecting → Connected → MagicExchange →
/// LoginSending → LoginReplyWait → HandshakeDone), so the C
/// side can drive the toolbar throbber off the same `on_state`
/// callback wiring it already uses.
///
/// All input slices are non-NUL-terminated:
///
/// - `host` (length `host_len`) — DNS name or IP literal,
///   UTF-8.
/// - `login` (length `login_len`) — user login. Empty allowed
///   ("guest").
/// - `password` (length `password_len`) — empty allowed.
/// - `name` (length `name_len`) — display name. Empty omits
///   the chunk.
///
/// `icon`, `version`, and `caps` are host-endian `u16` values (NOT
/// pre-swapped) — hxnet encodes them big-endian with `to_be_bytes()`
/// when building the LOGIN chunks, so passing network-order here would
/// double-swap on little-endian hosts. 0 omits the chunk. `caps` is
/// the `HTLC_CAP_*` bitmask — pass the same bits the legacy LOGIN
/// advertises so extensions (chat-history / inline-media / voice)
/// negotiate. `trans` is the transaction id for the LOGIN frame —
/// pick a non-zero value (production uses a counter).
///
/// **Plaintext only**: this FFI does NOT speak TLS or HOPE.
/// Callers who need either route through the legacy
/// `spawn_fd_*` path until Phase F (HOPE) and Phase B (TLS)
/// are folded into the orchestrator.
///
/// # Safety
///
/// Every `*_ptr` / `*_len` pair must point at `len` readable
/// bytes (or be `NULL` / 0 to indicate "omit"). UTF-8 validity
/// is required for `host`; `login` / `password` / `name` are
/// treated as opaque byte strings (production sends them
/// XOR-0xFF obfuscated for the credentials, raw bytes for the
/// name).
///
/// Callback pointers and `user_data` must remain valid for
/// the connection's lifetime.
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_open_plaintext(
    host: *const u8,
    host_len: usize,
    port: u16,
    login: *const u8,
    login_len: usize,
    password: *const u8,
    password_len: usize,
    name: *const u8,
    name_len: usize,
    icon: u16,
    version: u16,
    caps: u16,
    trans: u32,
    on_event: HxnetEventCallback,
    on_shutdown: HxnetShutdownCallback,
    on_state: HxnetStateCallback,
    user_data: *mut c_void,
) -> *mut HxnetConnection {
    if host.is_null() || host_len == 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext: NULL or empty host"
        );
        return std::ptr::null_mut();
    }
    if on_event.is_none() || on_shutdown.is_none() {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext: NULL on_event / on_shutdown"
        );
        return std::ptr::null_mut();
    }
    if trans == 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext: trans=0 is reserved; pick a non-zero id"
        );
        return std::ptr::null_mut();
    }

    // slice::from_raw_parts is documented UB for
    // `len * size_of::<T>() > isize::MAX`. Guard every length we turn
    // into a slice below — same defensive discipline as the other FFI
    // entry points — so a bogus length from C is a logged error.
    if [host_len, login_len, password_len, name_len]
        .iter()
        .any(|&n| (n as u64) > (isize::MAX as u64))
    {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext: a length argument exceeds isize::MAX"
        );
        return std::ptr::null_mut();
    }

    let host_slice = std::slice::from_raw_parts(host, host_len);
    let host_str = match std::str::from_utf8(host_slice) {
        Ok(s) => s.to_string(),
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_plaintext: host is not valid UTF-8"
            );
            return std::ptr::null_mut();
        }
    };

    // A NULL pointer with a non-zero length is a caller bug (it would
    // silently drop the credential / name). Reject it rather than treat
    // it as empty — same fail-fast contract as hxnet_connection_send_frame.
    if (login.is_null() && login_len != 0)
        || (password.is_null() && password_len != 0)
        || (name.is_null() && name_len != 0)
    {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext: NULL pointer with non-zero \
             length for login / password / name"
        );
        return std::ptr::null_mut();
    }

    // Empty credential / name slices land as Vec<u8>::new(). login.rs
    // then applies the per-field rule (matching src/login_packet.c):
    // LOGIN is always emitted (a zero-length chunk when empty), PASSWORD
    // is omitted entirely when empty (guest login — NOT a zero-length
    // chunk), and NAME / ICON / VERSION / CAPABILITIES are each omitted
    // when empty / zero.
    let login_vec = if login_len == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(login, login_len).to_vec()
    };
    let password_vec = if password_len == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(password, password_len).to_vec()
    };
    let name_vec = if name_len == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(name, name_len).to_vec()
    };

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(
        Runtime::global,
    )) {
        Ok(rt) => rt,
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_plaintext: Runtime::global panicked; \
                 aborting to avoid unwinding across the FFI boundary"
            );
            std::process::abort();
        }
    };

    let (cmd, events, cmd_rx, evt_tx) = Connection::make_channels();

    let req = crate::lifecycle::PlaintextOpenRequest {
        host: host_str,
        port,
        login: login_vec,
        password: password_vec,
        name: name_vec,
        icon,
        version,
        caps,
        trans,
    };

    let join = rt.handle().spawn(async move {
        crate::lifecycle::run_plaintext_lifecycle(req, cmd_rx, evt_tx).await;
    });

    wire_callback_state_with_on_state(
        rt, cmd, events, join, on_event, on_shutdown, on_state, user_data,
    )
}

/// Open a plaintext-Hotline-over-TLS connection (the Mobius / Janus
/// separate-port model: TLS-from-byte-zero on a dedicated port, then
/// the ordinary plaintext Hotline protocol over the encrypted
/// stream). The TLS sibling of [`hxnet_connection_open_plaintext`];
/// runs [`crate::lifecycle::run_plaintext_tls_lifecycle`].
///
/// Certificate trust is WebPKI-first: rustls validates the cert
/// against the native trust roots, and a CA-valid cert (e.g. Let's
/// Encrypt) is trusted silently — `verify_cert` is NOT consulted. Only
/// when WebPKI validation fails is `verify_cert` invoked
/// (post-handshake, with the leaf cert's `"sha256:<hex>"` fingerprint,
/// before any LOGIN) to make the TOFU decision; it may reject. Passing
/// `verify_cert = NULL` therefore leaves only *non-WebPKI* certs
/// accept-any (CA-valid certs are still validated); callers that want
/// TOFU enforcement on self-signed certs must supply it. See
/// [`crate::tls`] and [`HxnetVerifyCertCallback`].
///
/// Other parameters and safety are identical to
/// [`hxnet_connection_open_plaintext`].
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_open_plaintext_tls(
    host: *const u8,
    host_len: usize,
    port: u16,
    login: *const u8,
    login_len: usize,
    password: *const u8,
    password_len: usize,
    name: *const u8,
    name_len: usize,
    icon: u16,
    version: u16,
    caps: u16,
    trans: u32,
    on_event: HxnetEventCallback,
    on_shutdown: HxnetShutdownCallback,
    on_state: HxnetStateCallback,
    verify_cert: HxnetVerifyCertCallback,
    user_data: *mut c_void,
) -> *mut HxnetConnection {
    if host.is_null() || host_len == 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext_tls: NULL or empty host"
        );
        return std::ptr::null_mut();
    }
    if on_event.is_none() || on_shutdown.is_none() {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext_tls: NULL on_event / on_shutdown"
        );
        return std::ptr::null_mut();
    }
    if trans == 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext_tls: trans=0 is reserved"
        );
        return std::ptr::null_mut();
    }

    // slice::from_raw_parts is documented UB for
    // `len * size_of::<T>() > isize::MAX`. Guard every length we turn
    // into a slice below — same defensive discipline as the other FFI
    // entry points — so a bogus length from C is a logged error.
    if [host_len, login_len, password_len, name_len]
        .iter()
        .any(|&n| (n as u64) > (isize::MAX as u64))
    {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext_tls: a length argument exceeds isize::MAX"
        );
        return std::ptr::null_mut();
    }

    let host_slice = std::slice::from_raw_parts(host, host_len);
    let host_str = match std::str::from_utf8(host_slice) {
        Ok(s) => s.to_string(),
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_plaintext_tls: host is not valid UTF-8"
            );
            return std::ptr::null_mut();
        }
    };

    // A NULL pointer with a non-zero length is a caller bug (it would
    // silently drop the credential / name). Reject it rather than treat
    // it as empty — same fail-fast contract as hxnet_connection_send_frame.
    if (login.is_null() && login_len != 0)
        || (password.is_null() && password_len != 0)
        || (name.is_null() && name_len != 0)
    {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext_tls: NULL pointer with non-zero \
             length for login / password / name"
        );
        return std::ptr::null_mut();
    }

    let login_vec = if login_len == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(login, login_len).to_vec()
    };
    let password_vec = if password_len == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(password, password_len).to_vec()
    };
    let name_vec = if name_len == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(name, name_len).to_vec()
    };

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(
        Runtime::global,
    )) {
        Ok(rt) => rt,
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_plaintext_tls: Runtime::global \
                 panicked; aborting to avoid unwinding across the FFI boundary"
            );
            std::process::abort();
        }
    };

    let (cmd, events, cmd_rx, evt_tx) = Connection::make_channels();

    let req = crate::lifecycle::PlaintextOpenRequest {
        host: host_str,
        port,
        login: login_vec,
        password: password_vec,
        name: name_vec,
        icon,
        version,
        caps,
        trans,
    };

    // Wrap the C verify callback in a Rust closure the lifecycle
    // calls post-handshake with the cert fingerprint. The opaque
    // user_data is shared with the other callbacks; the SendUserData
    // wrapper lets the closure move into the spawned task.
    let verify_closure: Option<Box<dyn Fn(&str) -> bool + Send>> =
        verify_cert.map(|cb| {
            let ud = SendUserData(user_data);
            let boxed: Box<dyn Fn(&str) -> bool + Send> = Box::new(move |fp: &str| {
                let ud = &ud;
                unsafe { cb(fp.as_ptr(), fp.len(), ud.0) != 0 }
            });
            boxed
        });

    let join = rt.handle().spawn(async move {
        crate::lifecycle::run_plaintext_tls_lifecycle(
            req,
            verify_closure,
            cmd_rx,
            evt_tx,
        )
        .await;
    });

    wire_callback_state_with_on_state(
        rt, cmd, events, join, on_event, on_shutdown, on_state, user_data,
    )
}

/// Open a HOPE-Secure-Login connection with hxnet driving the full
/// handshake — magic + step-1 + step-2 + cipher transition — and the
/// post-handshake encrypted stream. The HOPE sibling of
/// [`hxnet_connection_open_plaintext`]; runs
/// [`crate::lifecycle::run_hope_lifecycle`].
///
/// `cipher_alg` (length `cipher_alg_len`) is the wire cipher label to
/// advertise — `b"BLOWFISH"` or `b"CHACHA20-POLY1305"`. HOPE always
/// negotiates a cipher, so an empty `cipher_alg` is rejected.
///
/// `trans` is the **step-1** transaction id; the orchestrator sends
/// step 2 as `trans + 1`, and the step-2 reply (which gets replayed
/// to the C side as `Event::Frame`) carries `trans + 1`. The C caller
/// registers its login task under that value.
///
/// `caps` is advertised in the step-2 LOGIN (BE u16 `HTLC_CAP_*`);
/// `icon` / `version` likewise. `name` is the display name sent in
/// step 2 (HOPE includes it, unlike the plaintext path which defers
/// the name to a later USER_CHANGE).
///
/// # Safety
///
/// Same as [`hxnet_connection_open_plaintext`], plus `cipher_alg`
/// must point at `cipher_alg_len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_open_hope(
    host: *const u8,
    host_len: usize,
    port: u16,
    login: *const u8,
    login_len: usize,
    password: *const u8,
    password_len: usize,
    name: *const u8,
    name_len: usize,
    icon: u16,
    version: u16,
    caps: u16,
    trans: u32,
    cipher_alg: *const u8,
    cipher_alg_len: usize,
    on_event: HxnetEventCallback,
    on_shutdown: HxnetShutdownCallback,
    on_state: HxnetStateCallback,
    user_data: *mut c_void,
) -> *mut HxnetConnection {
    if host.is_null() || host_len == 0 {
        glib::g_critical!("hxnet", "hxnet_connection_open_hope: NULL or empty host");
        return std::ptr::null_mut();
    }
    if on_event.is_none() || on_shutdown.is_none() {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_hope: NULL on_event / on_shutdown"
        );
        return std::ptr::null_mut();
    }
    if trans == 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_hope: trans=0 is reserved; pick a non-zero id"
        );
        return std::ptr::null_mut();
    }
    if cipher_alg.is_null() || cipher_alg_len == 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_hope: HOPE requires a non-empty cipher_alg"
        );
        return std::ptr::null_mut();
    }

    // slice::from_raw_parts is documented UB for
    // `len * size_of::<T>() > isize::MAX`. Guard every length we turn
    // into a slice below — same defensive discipline as the other FFI
    // entry points — so a bogus length from C is a logged error.
    if [host_len, login_len, password_len, name_len, cipher_alg_len]
        .iter()
        .any(|&n| (n as u64) > (isize::MAX as u64))
    {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_hope: a length argument exceeds isize::MAX"
        );
        return std::ptr::null_mut();
    }

    let host_slice = std::slice::from_raw_parts(host, host_len);
    let host_str = match std::str::from_utf8(host_slice) {
        Ok(s) => s.to_string(),
        Err(_) => {
            glib::g_critical!("hxnet", "hxnet_connection_open_hope: host is not valid UTF-8");
            return std::ptr::null_mut();
        }
    };

    // A NULL pointer with a non-zero length is a caller bug (it would
    // silently drop the credential / name). Reject it rather than treat
    // it as empty — same fail-fast contract as hxnet_connection_send_frame.
    if (login.is_null() && login_len != 0)
        || (password.is_null() && password_len != 0)
        || (name.is_null() && name_len != 0)
    {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_hope: NULL pointer with non-zero \
             length for login / password / name"
        );
        return std::ptr::null_mut();
    }

    let login_vec = if login_len == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(login, login_len).to_vec()
    };
    let password_vec = if password_len == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(password, password_len).to_vec()
    };
    let name_vec = if name_len == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(name, name_len).to_vec()
    };
    let cipher_vec = std::slice::from_raw_parts(cipher_alg, cipher_alg_len).to_vec();

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(
        Runtime::global,
    )) {
        Ok(rt) => rt,
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_hope: Runtime::global panicked; \
                 aborting to avoid unwinding across the FFI boundary"
            );
            std::process::abort();
        }
    };

    let (cmd, events, cmd_rx, evt_tx) = Connection::make_channels();

    let req = crate::lifecycle::HopeOpenRequest {
        host: host_str,
        port,
        login: login_vec,
        password: password_vec,
        name: name_vec,
        icon,
        version,
        caps,
        trans,
        cipher_algs: vec![cipher_vec],
    };

    let join = rt.handle().spawn(async move {
        crate::lifecycle::run_hope_lifecycle(req, cmd_rx, evt_tx).await;
    });

    wire_callback_state_with_on_state(
        rt, cmd, events, join, on_event, on_shutdown, on_state, user_data,
    )
}

/// Variant of `wire_callback_state` that routes
/// `Event::State(...)` through an additional `on_state`
/// callback instead of dropping it. Reuses the same
/// pump→forward_to_main shape as the older wiring helper —
/// only the per-event dispatch differs.
fn wire_callback_state_with_on_state(
    rt: &Runtime,
    cmd: ConnectionHandle,
    mut events: mpsc::Receiver<Event>,
    join: JoinHandle<()>,
    on_event: HxnetEventCallback,
    on_shutdown: HxnetShutdownCallback,
    on_state: HxnetStateCallback,
    user_data: *mut c_void,
) -> *mut HxnetConnection {
    let main_ctx = glib::MainContext::ref_thread_default();
    let _acquire_guard = if main_ctx.is_owner() {
        None
    } else {
        match main_ctx.acquire() {
            Ok(g) => Some(g),
            Err(_) => {
                glib::g_critical!(
                    "hxnet",
                    "wire_callback_state_with_on_state: thread-default \
                     MainContext is owned by another thread; cannot acquire \
                     for spawn_local"
                );
                return std::ptr::null_mut();
            }
        }
    };

    let handle_box = Box::new(HxnetConnection {
        cmd,
        events: None,
        _callback_state: None,
        _join: join,
    });
    let handle_ptr = Box::into_raw(handle_box);

    let (ferry_tx, ferry_rx) =
        async_channel::bounded::<Event>(CALLBACK_FERRY_CAPACITY);
    let pump = rt.handle().spawn(async move {
        while let Some(evt) = events.recv().await {
            if ferry_tx.send(evt).await.is_err() {
                break;
            }
        }
    });

    let cb = SendCallbacks {
        on_event,
        on_shutdown,
        user_data,
        handle_ptr,
    };
    let state_cb = SendStateCallback {
        on_state,
        user_data,
        handle_ptr,
    };

    let forwarder =
        hxbridge::channel::forward_to_main(&main_ctx, ferry_rx, move |evt| match evt {
            Event::Frame(frame) => {
                let mut out = std::mem::MaybeUninit::<HxnetFrame>::uninit();
                unsafe { write_frame_to_out(frame, out.as_mut_ptr()) };
                let frame_ptr = out.as_mut_ptr();
                if let Some(on_event) = cb.on_event {
                    unsafe {
                        on_event(cb.handle_ptr, frame_ptr, cb.user_data);
                    }
                }
            }
            Event::Shutdown(reason) => {
                eprintln!("hxnet: actor shutting down: {reason:?}");
                let code = shutdown_code(reason);
                if let Some(on_shutdown) = cb.on_shutdown {
                    unsafe {
                        on_shutdown(cb.handle_ptr, code, cb.user_data);
                    }
                }
            }
            Event::State(state) => {
                if let Some(on_state) = state_cb.on_state {
                    unsafe {
                        on_state(
                            state_cb.handle_ptr,
                            state as c_uint,
                            state_cb.user_data,
                        );
                    }
                }
            }
        });

    let handle_ref = unsafe { &mut *handle_ptr };
    handle_ref._callback_state = Some(CallbackState {
        _pump: pump,
        _forwarder: forwarder,
    });

    handle_ptr
}

/// Send-shaped copy of the state callback fields for use inside
/// the `forward_to_main` closure. Same shape as `SendCallbacks`
/// — the closure captures it by move and the C pointer
/// invariants stay the same (single-threaded use, lifetime
/// scoped to the connection).
struct SendStateCallback {
    on_state: HxnetStateCallback,
    user_data: *mut c_void,
    handle_ptr: *mut HxnetConnection,
}
unsafe impl Send for SendStateCallback {}

// Silence unused import warning when the runtime feature is on
// but no test exercises the c_void path. The body of this module
// uses c_int / c_uint exclusively; c_void is reserved for future
// FFI growth (e.g. user_data parameters on callback variants).
#[allow(dead_code)]
fn _silence_unused_c_void(_p: *mut c_void) {}
