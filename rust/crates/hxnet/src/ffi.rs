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
//! [`hxnet_connection_destroy`] drops the [`ConnectionHandle`]
//! which drops the command sender, which the actor sees as
//! "handle dropped" — it flushes any pending writes and exits.
//! The fd is closed when the wrapped `tokio::net::TcpStream`
//! drops at the end of the actor's loop.
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
    match events.try_recv() {
        Ok(Event::Frame(frame)) => {
            write_frame_to_out(frame, out_frame);
            HXNET_RECV_FRAME
        }
        Ok(Event::Shutdown(reason)) => {
            *out_reason = shutdown_code(reason);
            HXNET_RECV_SHUTDOWN
        }
        Err(mpsc::error::TryRecvError::Empty) => HXNET_RECV_EMPTY,
        Err(mpsc::error::TryRecvError::Disconnected) => {
            // The actor finished without emitting Shutdown (we
            // try our best to ensure it always does, but defend
            // anyway).
            *out_reason = HXNET_SHUTDOWN_HANDLE_DROPPED;
            HXNET_RECV_SHUTDOWN
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

/// Drop the handle. The actor sees `HandleDropped` and exits;
/// pending writes are flushed best-effort. Any frames still in
/// the event channel are dropped (the C side already had its
/// chance via [`hxnet_connection_try_recv_frame`]).
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
    // Reclaim the Box; its Drop drops the cmd sender (actor
    // exits), the events receiver (any in-flight events
    // discarded), and the JoinHandle (the actor task continues
    // to completion but we don't await it — it gets detached
    // and dropped naturally on the runtime).
    drop(Box::from_raw(handle));
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
    let (cmd, mut events, join) = match Connection::spawn(tcp) {
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

    // forward_to_main is going to call MainContext::spawn_local,
    // which **panics** if the calling thread doesn't own the
    // context (see hxbridge::channel::forward_to_main and the
    // upstream gtk-rs requirement). A panic across this
    // `extern "C"` boundary is UB, so we defensively `acquire()`
    // ownership ourselves and fail with `g_critical` + NULL on
    // an Err. The guard binding must be a named local — letting
    // it drop at the end of a boolean expression releases the
    // context before forward_to_main attaches. Same pattern as
    // hxvoice-runtime's bus-watch attach.
    //
    // We do this BEFORE Box::into_raw so the failure path
    // doesn't leak the box: a returned NULL with `cmd`,
    // `events`, and `join` going out of scope is clean — `cmd`
    // dropping signals HandleDropped to the actor, which exits;
    // `events` dropping discards any in-flight buffered events;
    // the actor task runs to completion on the runtime and the
    // TcpStream's Drop closes the fd.
    let main_ctx = glib::MainContext::ref_thread_default();
    let _acquire_guard = if main_ctx.is_owner() {
        None
    } else {
        match main_ctx.acquire() {
            Ok(g) => Some(g),
            Err(_) => {
                glib::g_critical!(
                    "hxnet",
                    "hxnet_connection_spawn_fd_with_callback: thread-default \
                     MainContext is owned by another thread; cannot acquire \
                     for spawn_local. The Connection was spawned on the \
                     tokio runtime but is being dropped because the GLib \
                     wiring cannot attach."
                );
                return std::ptr::null_mut();
            }
        }
    };

    // Build the box first so we can capture its raw pointer in
    // the callback closure (so the C callback receives a stable
    // handle pointer it can correlate to its own state).
    let handle_box = Box::new(HxnetConnection {
        cmd,
        events: None,
        _callback_state: None,
        _join: join,
    });
    let handle_ptr = Box::into_raw(handle_box);

    // tokio→GLib ferry: pump task forwards every Event from
    // the actor's mpsc receiver into the async_channel; the
    // GLib forward_to_main drains the async_channel and
    // invokes the C callback.
    let (ferry_tx, ferry_rx) =
        async_channel::bounded::<Event>(CALLBACK_FERRY_CAPACITY);
    let pump = rt.handle().spawn(async move {
        while let Some(evt) = events.recv().await {
            if ferry_tx.send(evt).await.is_err() {
                // GLib side dropped (handle destroyed). Stop
                // pumping; the actor will eventually exit when
                // its cmd channel closes too.
                break;
            }
        }
    });

    // Capture the C callbacks behind the SendCallbacks marker.
    let cb = SendCallbacks {
        on_event,
        on_shutdown,
        user_data,
        handle_ptr,
    };

    // forward_to_main runs on the current thread's
    // MainContext, which in production is the GLib main loop.
    // The handler runs on that thread and is free to touch GTK
    // state, call the C callback, etc. We've already acquired
    // ownership above, so the spawn_local inside forward_to_main
    // is guaranteed not to panic.
    let forwarder =
        hxbridge::channel::forward_to_main(&main_ctx, ferry_rx, move |evt| {
            match evt {
                Event::Frame(frame) => {
                    // Allocate the HxnetFrame on the stack so
                    // the C callback receives a stable pointer
                    // for the duration of its call. After the
                    // callback returns, the C side either has
                    // called hxnet_frame_free already (in which
                    // case the body has been moved into Rust's
                    // free path) or will eventually call it;
                    // either way the stack HxnetFrame is fine
                    // to drop here because body_ptr ownership
                    // has been formally transferred to C by the
                    // callback contract.
                    let mut out = std::mem::MaybeUninit::<HxnetFrame>::uninit();
                    // SAFETY: write_frame_to_out fully
                    // initialises *out.
                    unsafe { write_frame_to_out(frame, out.as_mut_ptr()) };
                    // Hand the raw *mut pointer to the callback —
                    // hxnet_frame_free writes through the struct
                    // to zero body_ptr / body_len, so a *mut
                    // here matches the callback's documented
                    // contract and saves the C side a const-cast.
                    let frame_ptr = out.as_mut_ptr();
                    if let Some(on_event) = cb.on_event {
                        // SAFETY: caller guarantees the
                        // function pointer is valid and that
                        // user_data outlives the connection.
                        unsafe {
                            on_event(cb.handle_ptr, frame_ptr, cb.user_data);
                        }
                    }
                }
                Event::Shutdown(reason) => {
                    let code = shutdown_code(reason);
                    if let Some(on_shutdown) = cb.on_shutdown {
                        // SAFETY: caller guarantees the
                        // function pointer is valid.
                        unsafe {
                            on_shutdown(cb.handle_ptr, code, cb.user_data);
                        }
                    }
                }
            }
        });

    // Now stuff the callback state into the handle. Re-borrow
    // through the raw pointer.
    let handle_ref = unsafe { &mut *handle_ptr };
    handle_ref._callback_state = Some(CallbackState {
        _pump: pump,
        _forwarder: forwarder,
    });

    handle_ptr
}

// Silence unused import warning when the runtime feature is on
// but no test exercises the c_void path. The body of this module
// uses c_int / c_uint exclusively; c_void is reserved for future
// FFI growth (e.g. user_data parameters on callback variants).
#[allow(dead_code)]
fn _silence_unused_c_void(_p: *mut c_void) {}
