//! C-callable surface for hxnet.
//!
//! Connections are created by the connect entry points further down
//! (`hxnet_connection_open_tcp`, `hxnet_connection_open_plaintext`,
//! `_open_hope`, `_open_plaintext_tls`), which resolve + connect the
//! socket **inside Rust** — no OS socket fd ever crosses this FFI.
//! Once a handle exists it is driven through:
//!
//! - [`hxnet_connection_try_recv_frame`] — non-blocking poll for
//!   the next event. The C side drives this from a GLib idle / a
//!   poll loop in tests.
//! - [`hxnet_connection_send_frame`] — non-blocking enqueue of
//!   bytes to write.
//! - [`hxnet_connection_destroy`] — drop the handle; the actor
//!   gets HandleDropped and exits.
//! - [`hxnet_frame_free`] — release a frame returned by try_recv.
//!
//! The production paths use the callback-driven connect entries,
//! which route events through the hxbridge ferry to the GLib main
//! loop rather than the polling form above.
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

use hxbridge::runtime::Runtime;
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
    /// HOPE control-channel AEAD material slot. `Some` only on a HOPE
    /// connection; the lifecycle fills it when ChaCha20-Poly1305 is
    /// negotiated. Read by `hxnet_connection_hope_aead_material` so an
    /// HTXF subchannel can derive transfer keys in-process.
    hope_aead: Option<crate::lifecycle::HopeAeadSlot>,
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
    assert!(std::mem::align_of::<HxnetFrame>() == std::mem::align_of::<*mut u8>());
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
/// `handle` must be a non-NULL pointer previously returned by one
/// of the connect entry points and not yet passed to
/// [`hxnet_connection_destroy`]. `out_frame` and `out_reason`
/// must be valid writable locations.
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_try_recv_frame(
    handle: *mut HxnetConnection,
    out_frame: *mut HxnetFrame,
    out_reason: *mut c_int,
) -> c_int {
    if handle.is_null() || out_frame.is_null() || out_reason.is_null() {
        glib::g_critical!("hxnet", "hxnet_connection_try_recv_frame: NULL arg");
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
                // connection-state events on. No connect entry
                // currently produces a pollable handle (the connect
                // paths are all callback-driven), so State events
                // don't reach here in practice. If a future caller
                // wires the polling API through a connect path,
                // we'll grow a separate `try_recv_state` entry; for now
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

/// `size_t hxnet_build_login_frame(login, login_len, password, password_len,
/// name, name_len, icon, version, caps, trans, out, out_cap)` — build a
/// plaintext (legacy) `HTLC_HDR_LOGIN` frame into `out[..out_cap]`, returning the
/// frame length written (`0` on failure or a too-small buffer). Exposes the
/// production [`crate::login::build_login_frame`] so the C integration-test
/// harness can emit the same LOGIN packet production does (replaces the old
/// `src/login_packet.c`). Per-field rules are the builder's: LOGIN always emitted
/// (zero-length chunk when empty), PASSWORD omitted when empty, NAME / ICON /
/// VERSION / CAPABILITIES each omitted when empty / zero.
///
/// # Safety
/// Each `(ptr, len)` pair is NULL/0 or valid for `len` bytes; `out` is valid for
/// `out_cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn hxnet_build_login_frame(
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
    out: *mut u8,
    out_cap: usize,
) -> usize {
    // A NULL pointer with a non-zero length would silently drop the field —
    // a caller bug. Fail closed rather than treat it as empty.
    if out.is_null()
        || (login.is_null() && login_len != 0)
        || (password.is_null() && password_len != 0)
        || (name.is_null() && name_len != 0)
    {
        return 0;
    }
    let login_s: &[u8] = if login_len == 0 {
        &[]
    } else {
        std::slice::from_raw_parts(login, login_len)
    };
    let password_s: &[u8] = if password_len == 0 {
        &[]
    } else {
        std::slice::from_raw_parts(password, password_len)
    };
    let name_s: &[u8] = if name_len == 0 {
        &[]
    } else {
        std::slice::from_raw_parts(name, name_len)
    };
    let req = crate::login::LoginRequest {
        login: login_s,
        password: password_s,
        name: name_s,
        icon,
        version,
        caps,
        trans,
    };
    match crate::login::build_login_frame(&req) {
        Ok(frame) if frame.len() <= out_cap => {
            std::ptr::copy_nonoverlapping(frame.as_ptr(), out, frame.len());
            frame.len()
        }
        _ => 0,
    }
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
        glib::g_critical!("hxnet", "hxnet_connection_send_frame: NULL handle");
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
/// `handle` must be a non-NULL pointer previously returned by one
/// of the connect entry points and not yet destroyed.
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_destroy(handle: *mut HxnetConnection) {
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
pub type HxnetStateCallback =
    Option<unsafe extern "C" fn(conn: *mut HxnetConnection, state: c_uint, user_data: *mut c_void)>;

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
/// `port` is the TCP port. The three callbacks are wired through
/// the shared `wire_callback_state_with_on_state` helper;
/// `on_state` fires once per `ConnectionState`
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
        glib::g_critical!("hxnet", "hxnet_connection_open_tcp: NULL or empty host");
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

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(Runtime::global)) {
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
        match crate::connect::resolve_and_connect(&host_str, port, None, &evt_tx).await {
            Ok(stream) => {
                // Emit Connected here (vs. inside resolve_and_connect)
                // so the future post-connect setup (TLS / etc.)
                // gets a single state-event surface to thread
                // through. Phase A just spawns the actor right
                // after.
                let _ = evt_tx
                    .send(Event::State(crate::ConnectionState::Connected))
                    .await;
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
        rt,
        cmd,
        events,
        join,
        on_event,
        on_shutdown,
        on_state,
        user_data,
    )
}

/// Parse an optional proxy URI argument shared by the three lifecycle
/// `open_*` entry points into a [`ProxyConfig`].
///
/// - `NULL` pointer or zero length → `Ok(None)` (connect direct).
/// - A valid `socks5://` / `socks4://` URI → `Ok(Some(cfg))`.
/// - A malformed or unsupported URI (including `http(s)://`, which
///   tokio-socks can't tunnel) → `Err(msg)`. Callers treat this as a
///   hard failure and return NULL rather than silently connecting
///   direct past a configured proxy — the same fail-loud contract as the
///   feature-off path in `resolve_and_connect`.
///
/// The returned error never echoes the raw URI's userinfo (see
/// `ProxyConfig::from_uri`), so logging it can't leak a proxy password.
///
/// # Safety
///
/// `ptr` must point at `len` readable bytes or be `NULL`.
pub(crate) unsafe fn parse_proxy_arg(
    ptr: *const u8,
    len: usize,
) -> Result<Option<crate::connect::ProxyConfig>, String> {
    // NULL + zero length is the "no proxy" sentinel. A NULL pointer with a
    // non-zero length is a caller bug — fail fast (same contract as the
    // login / name slices above) rather than silently dropping a
    // configured proxy and connecting direct.
    if ptr.is_null() {
        if len != 0 {
            return Err("proxy_uri is NULL but proxy_uri_len is non-zero".to_string());
        }
        return Ok(None);
    }
    if len == 0 {
        return Ok(None);
    }
    if (len as u64) > (isize::MAX as u64) {
        return Err("proxy_uri length exceeds isize::MAX".to_string());
    }
    let bytes = std::slice::from_raw_parts(ptr, len);
    let uri = std::str::from_utf8(bytes).map_err(|_| "proxy_uri is not valid UTF-8".to_string())?;
    crate::connect::ProxyConfig::from_uri(uri).map(Some)
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
/// Callers who need either use the dedicated connect entries
/// (`hxnet_connection_open_hope`, `_open_plaintext_tls`).
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
    proxy_uri: *const u8,
    proxy_uri_len: usize,
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
    // then applies the per-field rule (the legacy Hotline 1.x LOGIN wire
    // shape): LOGIN is always emitted (a zero-length chunk when empty), PASSWORD
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

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(Runtime::global)) {
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

    let proxy = match parse_proxy_arg(proxy_uri, proxy_uri_len) {
        Ok(p) => p,
        Err(e) => {
            glib::g_critical!("hxnet", "hxnet_connection_open_plaintext: {}", e);
            return std::ptr::null_mut();
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
        proxy,
    };

    let join = rt.handle().spawn(async move {
        crate::lifecycle::run_plaintext_lifecycle(req, cmd_rx, evt_tx).await;
    });

    wire_callback_state_with_on_state(
        rt,
        cmd,
        events,
        join,
        on_event,
        on_shutdown,
        on_state,
        user_data,
    )
}

/// Polling-mode sibling of [`hxnet_connection_open_plaintext`]: runs
/// the same production plaintext lifecycle (DNS + TCP + magic + LOGIN
/// + LOGIN-reply + Option-B replay + actor), but exposes events
/// through the polling API ([`hxnet_connection_try_recv_frame`])
/// instead of the GLib-main-thread callback forwarder.
///
/// This is the foundation for routing the synchronous Tier 3 test
/// harness through the production connect path (increment 2): the
/// harness has no GLib main loop, so it drives the lifecycle by
/// polling for frames. The replayed LOGIN reply arrives as the first
/// `HXNET_RECV_FRAME`; subsequent server frames (SELFINFO, agreement,
/// chat, …) arrive as the actor reads them. Outbound frames go via
/// [`hxnet_connection_send_frame`]. State events are dropped by the
/// polling receiver (the harness keys off the frames, like the
/// production C dispatch does).
///
/// Same parameter / safety contract as
/// [`hxnet_connection_open_plaintext`], minus the callbacks — including
/// the optional `proxy_uri` / `proxy_uri_len` (NULL/0 = connect direct).
/// The proxy params let the Tier 3 harness drive a proxied connect
/// through the production path (the SOCKS-proxy integration test).
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_open_plaintext_polling(
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
    proxy_uri: *const u8,
    proxy_uri_len: usize,
) -> *mut HxnetConnection {
    if host.is_null() || host_len == 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext_polling: NULL or empty host"
        );
        return std::ptr::null_mut();
    }
    if trans == 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext_polling: trans=0 is reserved"
        );
        return std::ptr::null_mut();
    }
    // slice::from_raw_parts is UB for len * size_of::<T> > isize::MAX —
    // guard the same way hxnet_connection_open_plaintext does.
    if [host_len, login_len, password_len, name_len]
        .iter()
        .any(|&n| (n as u64) > (isize::MAX as u64))
    {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext_polling: a length argument \
             exceeds isize::MAX"
        );
        return std::ptr::null_mut();
    }

    let host_slice = std::slice::from_raw_parts(host, host_len);
    let host_str = match std::str::from_utf8(host_slice) {
        Ok(s) => s.to_string(),
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_plaintext_polling: host is not valid UTF-8"
            );
            return std::ptr::null_mut();
        }
    };

    let login_vec = if login_len == 0 || login.is_null() {
        Vec::new()
    } else {
        std::slice::from_raw_parts(login, login_len).to_vec()
    };
    let password_vec = if password_len == 0 || password.is_null() {
        Vec::new()
    } else {
        std::slice::from_raw_parts(password, password_len).to_vec()
    };
    let name_vec = if name_len == 0 || name.is_null() {
        Vec::new()
    } else {
        std::slice::from_raw_parts(name, name_len).to_vec()
    };

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(Runtime::global)) {
        Ok(rt) => rt,
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_plaintext_polling: Runtime::global \
                 panicked; aborting to avoid unwinding across the FFI boundary"
            );
            std::process::abort();
        }
    };

    let proxy = match parse_proxy_arg(proxy_uri, proxy_uri_len) {
        Ok(p) => p,
        Err(e) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_plaintext_polling: {}",
                e
            );
            return std::ptr::null_mut();
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
        proxy,
    };
    let join = rt.handle().spawn(async move {
        crate::lifecycle::run_plaintext_lifecycle(req, cmd_rx, evt_tx).await;
    });

    // Polling-mode handle: keep the event receiver for
    // try_recv_frame; no callback forwarder.
    let handle = Box::new(HxnetConnection {
        cmd,
        events: Some(events),
        _callback_state: None,
        _join: join,
        hope_aead: None,
    });
    Box::into_raw(handle)
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
    proxy_uri: *const u8,
    proxy_uri_len: usize,
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

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(Runtime::global)) {
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

    let proxy = match parse_proxy_arg(proxy_uri, proxy_uri_len) {
        Ok(p) => p,
        Err(e) => {
            glib::g_critical!("hxnet", "hxnet_connection_open_plaintext_tls: {}", e);
            return std::ptr::null_mut();
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
        proxy,
    };

    // Wrap the C verify callback in a Rust closure the lifecycle
    // calls post-handshake with the cert fingerprint. The opaque
    // user_data is shared with the other callbacks; the SendUserData
    // wrapper lets the closure move into the spawned task.
    let verify_closure: Option<Box<dyn Fn(&str) -> bool + Send>> = verify_cert.map(|cb| {
        let ud = SendUserData(user_data);
        let boxed: Box<dyn Fn(&str) -> bool + Send> = Box::new(move |fp: &str| {
            let ud = &ud;
            unsafe { cb(fp.as_ptr(), fp.len(), ud.0) != 0 }
        });
        boxed
    });

    let join = rt.handle().spawn(async move {
        crate::lifecycle::run_plaintext_tls_lifecycle(req, verify_closure, cmd_rx, evt_tx).await;
    });

    wire_callback_state_with_on_state(
        rt,
        cmd,
        events,
        join,
        on_event,
        on_shutdown,
        on_state,
        user_data,
    )
}

/// Polling-mode sibling of [`hxnet_connection_open_plaintext_tls`]: runs
/// the same production TLS-from-byte-zero lifecycle
/// ([`crate::lifecycle::run_plaintext_tls_lifecycle`]) but exposes events
/// through the polling API ([`hxnet_connection_try_recv_frame`]) instead
/// of the GLib callback forwarder. Unlike the plaintext / HOPE polling
/// opens, this one KEEPS the `verify_cert` callback: TLS trust is decided
/// during the handshake on the lifecycle task (WebPKI-first, then the
/// TOFU callback on WebPKI failure), so a self-signed test cert needs a
/// callback that accepts it. The callback must be thread-safe (it runs on
/// the tokio task, not a GLib main loop).
///
/// Lets the synchronous Tier 3 harness drive its TLS tests through the
/// **production** rustls transport instead of the GnuTLS GIOStream
/// harness. Same parameter / safety contract as
/// [`hxnet_connection_open_plaintext_tls`], minus the event/shutdown/state
/// callbacks.
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_open_plaintext_tls_polling(
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
    verify_cert: HxnetVerifyCertCallback,
    user_data: *mut c_void,
) -> *mut HxnetConnection {
    if host.is_null() || host_len == 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext_tls_polling: NULL or empty host"
        );
        return std::ptr::null_mut();
    }
    if trans == 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext_tls_polling: trans=0 is reserved"
        );
        return std::ptr::null_mut();
    }
    if [host_len, login_len, password_len, name_len]
        .iter()
        .any(|&n| (n as u64) > (isize::MAX as u64))
    {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext_tls_polling: a length argument exceeds isize::MAX"
        );
        return std::ptr::null_mut();
    }

    let host_slice = std::slice::from_raw_parts(host, host_len);
    let host_str = match std::str::from_utf8(host_slice) {
        Ok(s) => s.to_string(),
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_plaintext_tls_polling: host is not valid UTF-8"
            );
            return std::ptr::null_mut();
        }
    };

    // A NULL pointer with a non-zero length is a caller bug (it would
    // silently drop the credential / name). Reject it rather than treat
    // it as empty — same fail-fast contract as the callback sibling
    // hxnet_connection_open_plaintext_tls.
    if (login.is_null() && login_len != 0)
        || (password.is_null() && password_len != 0)
        || (name.is_null() && name_len != 0)
    {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_plaintext_tls_polling: NULL pointer with \
             non-zero length for login / password / name"
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

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(Runtime::global)) {
        Ok(rt) => rt,
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_plaintext_tls_polling: Runtime::global \
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
        // This TLS polling open is direct-only: it takes no proxy_uri
        // param (unlike hxnet_connection_open_plaintext_polling, which the
        // SOCKS integration test drives through a proxy). Add one here if a
        // proxied-TLS harness path is ever needed.
        proxy: None,
    };
    let verify_closure: Option<Box<dyn Fn(&str) -> bool + Send>> = verify_cert.map(|cb| {
        let ud = SendUserData(user_data);
        let boxed: Box<dyn Fn(&str) -> bool + Send> = Box::new(move |fp: &str| {
            let ud = &ud;
            unsafe { cb(fp.as_ptr(), fp.len(), ud.0) != 0 }
        });
        boxed
    });
    let join = rt.handle().spawn(async move {
        crate::lifecycle::run_plaintext_tls_lifecycle(req, verify_closure, cmd_rx, evt_tx).await;
    });

    // Polling-mode handle: keep the event receiver for try_recv_frame;
    // no callback forwarder.
    let handle = Box::new(HxnetConnection {
        cmd,
        events: Some(events),
        _callback_state: None,
        _join: join,
        hope_aead: None,
    });
    Box::into_raw(handle)
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
    proxy_uri: *const u8,
    proxy_uri_len: usize,
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
    // cipher_alg is OPTIONAL: NULL / empty means "no cipher" — the
    // server runs the HMAC secure-login over a plaintext transport
    // (mhxd's non-cipher_only mode). A NULL pointer with a non-zero
    // length is still a caller bug (it would silently drop the cipher).
    if cipher_alg.is_null() && cipher_alg_len != 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_hope: NULL cipher_alg with non-zero length"
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
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_hope: host is not valid UTF-8"
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
    let cipher_vec = if cipher_alg_len == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(cipher_alg, cipher_alg_len).to_vec()
    };

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(Runtime::global)) {
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

    let proxy = match parse_proxy_arg(proxy_uri, proxy_uri_len) {
        Ok(p) => p,
        Err(e) => {
            glib::g_critical!("hxnet", "hxnet_connection_open_hope: {}", e);
            return std::ptr::null_mut();
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
        // Empty cipher_vec → advertise an empty cipher list in step 1
        // ("no cipher offered"), not a one-entry list with an empty
        // string. The server reads an empty list as "negotiate no
        // cipher".
        cipher_algs: if cipher_vec.is_empty() {
            Vec::new()
        } else {
            vec![cipher_vec]
        },
        proxy,
    };

    let hope_slot: crate::lifecycle::HopeAeadSlot =
        std::sync::Arc::new(std::sync::Mutex::new(None));
    let lifecycle_slot = hope_slot.clone();
    let join = rt.handle().spawn(async move {
        crate::lifecycle::run_hope_lifecycle(req, cmd_rx, evt_tx, lifecycle_slot).await;
    });

    let handle = wire_callback_state_with_on_state(
        rt,
        cmd,
        events,
        join,
        on_event,
        on_shutdown,
        on_state,
        user_data,
    );
    // Attach the HOPE AEAD slot so an HTXF subchannel can later derive
    // transfer keys off the negotiated session material.
    if !handle.is_null() {
        unsafe {
            (*handle).hope_aead = Some(hope_slot);
        }
    }
    handle
}

/// Polling-mode sibling of [`hxnet_connection_open_hope`]: runs the
/// same production HOPE-Secure-Login lifecycle ([`run_hope_lifecycle`]:
/// magic + step-1 / step-2 + cipher transition, then the encrypted
/// actor), but exposes events through the polling API
/// ([`hxnet_connection_try_recv_frame`]) instead of the GLib callback
/// forwarder.
///
/// This lets the synchronous Tier 3 test harness drive its HOPE tests
/// (`test_hope_blowfish` / `_chacha20` / `_hmac` / `_banner` /
/// `_chat_history`, …) through the **production** crypto stack instead
/// of the harness's own C `hope.c` / `cipher.c` reimplementation — so
/// the tests verify the shipped client's wire format, not dead C code.
/// The replayed step-2 LOGIN reply arrives as the first
/// `HXNET_RECV_FRAME`; subsequent server frames follow as the actor
/// reads (and transparently decrypts) them. Outbound frames go via
/// [`hxnet_connection_send_frame`] and are encrypted by the actor.
///
/// `cipher_alg` is OPTIONAL exactly as in [`hxnet_connection_open_hope`]
/// (NULL / empty ⇒ no-cipher HMAC secure login over a plaintext
/// transport). Same parameter / safety contract as that function, minus
/// the callbacks.
///
/// [`run_hope_lifecycle`]: crate::lifecycle::run_hope_lifecycle
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_open_hope_polling(
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
) -> *mut HxnetConnection {
    if host.is_null() || host_len == 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_hope_polling: NULL or empty host"
        );
        return std::ptr::null_mut();
    }
    if trans == 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_hope_polling: trans=0 is reserved"
        );
        return std::ptr::null_mut();
    }
    // cipher_alg is OPTIONAL (see hxnet_connection_open_hope): NULL /
    // empty ⇒ no cipher. A NULL pointer with a non-zero length is a
    // caller bug that would silently drop the cipher.
    if cipher_alg.is_null() && cipher_alg_len != 0 {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_hope_polling: NULL cipher_alg with non-zero length"
        );
        return std::ptr::null_mut();
    }
    // slice::from_raw_parts is UB for len * size_of::<T> > isize::MAX.
    if [host_len, login_len, password_len, name_len, cipher_alg_len]
        .iter()
        .any(|&n| (n as u64) > (isize::MAX as u64))
    {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_hope_polling: a length argument exceeds isize::MAX"
        );
        return std::ptr::null_mut();
    }
    if (login.is_null() && login_len != 0)
        || (password.is_null() && password_len != 0)
        || (name.is_null() && name_len != 0)
    {
        glib::g_critical!(
            "hxnet",
            "hxnet_connection_open_hope_polling: NULL pointer with non-zero \
             length for login / password / name"
        );
        return std::ptr::null_mut();
    }

    let host_slice = std::slice::from_raw_parts(host, host_len);
    let host_str = match std::str::from_utf8(host_slice) {
        Ok(s) => s.to_string(),
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_hope_polling: host is not valid UTF-8"
            );
            return std::ptr::null_mut();
        }
    };

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
    let cipher_vec = if cipher_alg_len == 0 {
        Vec::new()
    } else {
        std::slice::from_raw_parts(cipher_alg, cipher_alg_len).to_vec()
    };

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(Runtime::global)) {
        Ok(rt) => rt,
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_connection_open_hope_polling: Runtime::global panicked; \
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
        cipher_algs: if cipher_vec.is_empty() {
            Vec::new()
        } else {
            vec![cipher_vec]
        },
        // This HOPE polling open is direct-only: it takes no proxy_uri
        // param. (Only hxnet_connection_open_plaintext_polling does, for
        // the SOCKS integration test.)
        proxy: None,
    };
    let hope_slot: crate::lifecycle::HopeAeadSlot =
        std::sync::Arc::new(std::sync::Mutex::new(None));
    let lifecycle_slot = hope_slot.clone();
    let join = rt.handle().spawn(async move {
        crate::lifecycle::run_hope_lifecycle(req, cmd_rx, evt_tx, lifecycle_slot).await;
    });

    // Polling-mode handle: keep the event receiver for try_recv_frame;
    // no callback forwarder.
    let handle = Box::new(HxnetConnection {
        cmd,
        events: Some(events),
        _callback_state: None,
        _join: join,
        hope_aead: Some(hope_slot),
    });
    Box::into_raw(handle)
}

/// Opaque handle to a HOPE control-channel's retained AEAD material.
/// Obtained from [`hxnet_connection_hope_aead_material`] and passed to
/// [`hxnet_htxf_connect`] so an HTXF subchannel can derive its per-transfer
/// keys in-process. The session key never crosses the FFI as bytes —
/// only this opaque token does. Free with [`hxnet_hope_aead_free`].
pub struct HxnetHopeAead {
    pub(crate) material: crate::lifecycle::HopeAeadMaterial,
}

/// Return an opaque handle to `conn`'s HOPE control-channel AEAD
/// material, or NULL when `conn` is not a HOPE connection or did not
/// negotiate a ChaCha20-Poly1305 cipher (plaintext / Blowfish /
/// no-cipher leave the slot empty). The handle owns a copy of the
/// material, so it is independent of `conn`'s lifetime; free it with
/// [`hxnet_hope_aead_free`].
///
/// # Safety
/// `conn` must be a valid pointer from one of the open functions, not
/// yet destroyed.
#[no_mangle]
pub unsafe extern "C" fn hxnet_connection_hope_aead_material(
    conn: *mut HxnetConnection,
) -> *mut HxnetHopeAead {
    if conn.is_null() {
        return std::ptr::null_mut();
    }
    let conn = &*conn;
    let slot = match &conn.hope_aead {
        Some(s) => s,
        None => return std::ptr::null_mut(),
    };
    let material = match slot.lock() {
        Ok(g) => match g.as_ref() {
            Some(m) => m.clone(),
            None => return std::ptr::null_mut(),
        },
        Err(_) => return std::ptr::null_mut(),
    };
    Box::into_raw(Box::new(HxnetHopeAead { material }))
}

/// Clone a HOPE AEAD material handle into a new, independently owned
/// handle. The copy carries its own `HopeAeadMaterial`, so its lifetime
/// is decoupled from the source — a caller can hand the clone to a
/// worker that outlives the original (e.g. banner.c's HTXF fetch, where
/// the control connection's `htlc->hope_aead` may be freed on disconnect
/// while the blocking transfer is still in flight). Returns NULL when
/// `h` is NULL. Free with [`hxnet_hope_aead_free`].
///
/// # Safety
/// `h` must be NULL or a live pointer from one of the handle-producing
/// functions, not yet freed.
#[no_mangle]
pub unsafe extern "C" fn hxnet_hope_aead_clone(
    h: *const HxnetHopeAead,
) -> *mut HxnetHopeAead {
    if h.is_null() {
        return std::ptr::null_mut();
    }
    let src = &*h;
    Box::into_raw(Box::new(HxnetHopeAead {
        material: src.material.clone(),
    }))
}

/// Free a handle from [`hxnet_connection_hope_aead_material`] or
/// [`hxnet_hope_aead_clone`]. NULL is a no-op; double-free is undefined.
///
/// # Safety
/// `h` must be NULL or a live pointer from one of those functions.
#[no_mangle]
pub unsafe extern "C" fn hxnet_hope_aead_free(h: *mut HxnetHopeAead) {
    if !h.is_null() {
        drop(Box::from_raw(h));
    }
}

/// Common post-Connection-spawn callback wiring, with an
/// `on_state` callback that routes `Event::State(...)` through
/// instead of dropping it. Builds the HxnetConnection box, spawns
/// the pump task, acquires the MainContext, and attaches the
/// forwarder. Reuses the same
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
        hope_aead: None,
    });
    let handle_ptr = Box::into_raw(handle_box);

    let (ferry_tx, ferry_rx) = async_channel::bounded::<Event>(CALLBACK_FERRY_CAPACITY);
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

    let forwarder = hxbridge::channel::forward_to_main(&main_ctx, ferry_rx, move |evt| match evt {
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
                    on_state(state_cb.handle_ptr, state as c_uint, state_cb.user_data);
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

// ===================================================================
// Tracker fetch FFI (R3 item 8, T2)
// ===================================================================
//
// The C bridge (network.c::hx_tracker_list_async) opens a walk with a
// list of tracker URLs, then drains events on the GLib main loop via a
// timeout source, re-emitting the existing tracker-batch-begin /
// tracker-server-create signals. Mirrors the hxnet_connection_*_polling
// handle/drain shape: spawn run_fetch on the global runtime, keep the
// event receiver for poll, drop + abort on close.

use crate::tracker_fetch::{
    run_fetch, TcpTlsConnector, TrackerEvent, VerdictCache, VerifyFn,
};

/// Opaque handle for an in-flight tracker fetch walk. Created by
/// [`hxnet_tracker_fetch_open`], drained by [`hxnet_tracker_fetch_poll`],
/// freed by [`hxnet_tracker_fetch_close`].
pub struct HxnetTrackerFetch {
    events: mpsc::Receiver<TrackerEvent>,
    join: JoinHandle<()>,
    /// Backing store for the borrowed pointers handed out by the last
    /// poll. Replaced on the next poll (invalidating the prior
    /// pointers) and dropped on close.
    current: Option<TrackerEvent>,
}

/// `kind` discriminants in [`HxnetTrackerEvent`]. Mirrored by the C
/// bridge (hand-synced, like the HXNET_STATE_* constants).
pub const HXNET_TRK_KIND_BEGIN: u32 = 0;
pub const HXNET_TRK_KIND_RECORD: u32 = 1;
pub const HXNET_TRK_KIND_ERROR: u32 = 2;
pub const HXNET_TRK_KIND_DONE: u32 = 3;

/// [`hxnet_tracker_fetch_poll`] return codes.
pub const HXNET_TRK_POLL_EMPTY: c_int = 0;
pub const HXNET_TRK_POLL_EVENT: c_int = 1;
pub const HXNET_TRK_POLL_CLOSED: c_int = -1;

/// Host-aware TOFU verify callback for the tracker walk. Unlike the
/// connection FFI's [`HxnetVerifyCertCallback`] (one connection = one
/// host), a tracker walk spans many endpoints through a single callback,
/// so the tracker's `host` and `port` are passed alongside the
/// `"sha256:<hex>"` leaf fingerprint. Returns non-zero to accept. The C
/// side keys the trust decision on `(host, port)` via `tls_trust_decide`,
/// so different ports on one host pin (and prompt) independently.
pub type HxnetTrackerVerifyCallback = Option<
    unsafe extern "C" fn(
        host: *const u8,
        host_len: usize,
        port: u16,
        fp: *const u8,
        fp_len: usize,
        user_data: *mut c_void,
    ) -> c_int,
>;

/// Plain-old-data view of one [`TrackerEvent`], filled by
/// [`hxnet_tracker_fetch_poll`]. Pointer fields BORROW the handle's
/// `current` event and are valid only until the next `poll` or `close`
/// on that handle; the C side copies what it needs immediately (it
/// builds an `HxTrackerServer` whose constructor g_strndups the text).
/// Unused fields for a given `kind` are zeroed / NULL.
#[repr(C)]
pub struct HxnetTrackerEvent {
    pub kind: u32,
    /// BEGIN: record-path version (1 or 3).
    pub version: u8,
    /// RECORD: address type (0x04 / 0x06 / 0x48).
    pub addr_type: u8,
    /// BEGIN: record count for the batch.
    pub count: u16,
    /// RECORD: batch total (== the batch's BEGIN count).
    pub total: u16,
    /// RECORD: TCP port.
    pub port: u16,
    /// RECORD: user count.
    pub nusers: u16,
    /// RECORD: number of TLV entries in `tlv_*`.
    pub tlv_count: u16,
    /// BEGIN / RECORD / ERROR: tracker URL.
    pub url_ptr: *const u8,
    pub url_len: usize,
    /// RECORD: address bytes (IPv4 = 4, IPv6 = 16, hostname = UTF-8).
    pub address_ptr: *const u8,
    pub address_len: usize,
    /// RECORD: server name (MacRoman wire bytes, already normalised).
    pub name_ptr: *const u8,
    pub name_len: usize,
    /// RECORD: server description.
    pub desc_ptr: *const u8,
    pub desc_len: usize,
    /// RECORD: raw v3 TLV blob (empty for v1).
    pub tlv_ptr: *const u8,
    pub tlv_len: usize,
    /// ERROR: human-readable failure message.
    pub message_ptr: *const u8,
    pub message_len: usize,
}

// Pin the cross-language ABI layout from the Rust side, same discipline
// as HxnetFrame. The 16-byte scalar prefix is followed by 6 (ptr, len)
// pairs — all pointer-sized fields — at successive `P`-byte offsets,
// where `P = size_of::<usize>()`. Pinning EVERY field offset (not just
// up to url_ptr) plus the total size catches a reorder or type change of
// any field on either side at compile time rather than corrupting the
// decode at runtime. Expressed in terms of `P` so the asserts hold on
// 32- and 64-bit targets. The C bridge mirrors this with _Static_assert
// (src/network.c on T2; src/tracker_fetch_ffi.h once extracted).
const _: () = {
    let p = std::mem::size_of::<*const u8>();
    assert!(std::mem::offset_of!(HxnetTrackerEvent, kind) == 0);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, version) == 4);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, addr_type) == 5);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, count) == 6);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, total) == 8);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, port) == 10);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, nusers) == 12);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, tlv_count) == 14);
    // Pointer/len pairs, in declared order, at 16 + k*P.
    assert!(std::mem::offset_of!(HxnetTrackerEvent, url_ptr) == 16);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, url_len) == 16 + p);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, address_ptr) == 16 + 2 * p);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, address_len) == 16 + 3 * p);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, name_ptr) == 16 + 4 * p);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, name_len) == 16 + 5 * p);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, desc_ptr) == 16 + 6 * p);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, desc_len) == 16 + 7 * p);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, tlv_ptr) == 16 + 8 * p);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, tlv_len) == 16 + 9 * p);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, message_ptr) == 16 + 10 * p);
    assert!(std::mem::offset_of!(HxnetTrackerEvent, message_len) == 16 + 11 * p);
    assert!(std::mem::size_of::<HxnetTrackerEvent>() == 16 + 12 * p);
    assert!(std::mem::align_of::<HxnetTrackerEvent>() == std::mem::align_of::<*const u8>());
};

/// `(ptr, len)` for a byte slice, using a NULL pointer for an empty
/// slice rather than `as_ptr()`'s non-NULL dangling pointer — C consumers
/// commonly gate on `ptr != NULL`.
fn slice_ptr_len(b: &[u8]) -> (*const u8, usize) {
    if b.is_empty() {
        (std::ptr::null(), 0)
    } else {
        (b.as_ptr(), b.len())
    }
}

/// Fill `out` from `ev`, borrowing `ev`'s buffers. `out` must be
/// non-NULL. Unused fields are zeroed; empty buffers get a NULL pointer.
unsafe fn fill_tracker_event(out: *mut HxnetTrackerEvent, ev: &TrackerEvent) {
    // Start from an all-zero struct so every unused field is NULL / 0.
    std::ptr::write_bytes(out, 0, 1);
    let o = &mut *out;
    match ev {
        TrackerEvent::BatchBegin { url, version, count } => {
            o.kind = HXNET_TRK_KIND_BEGIN;
            o.version = *version;
            o.count = *count;
            (o.url_ptr, o.url_len) = slice_ptr_len(url.as_bytes());
        }
        TrackerEvent::Record { total, record } => {
            // url_ptr stays NULL for records — the C bridge uses the
            // batch URL it stashed on BatchBegin.
            o.kind = HXNET_TRK_KIND_RECORD;
            o.total = *total;
            o.addr_type = record.addr_type;
            o.port = record.port;
            o.nusers = record.nusers;
            o.tlv_count = record.tlv_count;
            (o.address_ptr, o.address_len) = slice_ptr_len(&record.address);
            (o.name_ptr, o.name_len) = slice_ptr_len(&record.name);
            (o.desc_ptr, o.desc_len) = slice_ptr_len(&record.desc);
            (o.tlv_ptr, o.tlv_len) = slice_ptr_len(&record.tlv_bytes);
        }
        TrackerEvent::BatchError { url, message } => {
            o.kind = HXNET_TRK_KIND_ERROR;
            (o.url_ptr, o.url_len) = slice_ptr_len(url.as_bytes());
            (o.message_ptr, o.message_len) = slice_ptr_len(message.as_bytes());
        }
        TrackerEvent::Done => {
            o.kind = HXNET_TRK_KIND_DONE;
        }
    }
}

/// Process-global TLS verdict cache. Tracker walks are serialized (a new
/// `hx_tracker_list_async` cancels any in-flight one), so a snapshot at
/// walk start + a store at walk end keeps the cache across Refreshes
/// without holding a lock across the walk's `.await`s.
fn tracker_verdicts() -> &'static std::sync::Mutex<VerdictCache> {
    static V: std::sync::OnceLock<std::sync::Mutex<VerdictCache>> = std::sync::OnceLock::new();
    V.get_or_init(|| std::sync::Mutex::new(VerdictCache::new()))
}

fn tracker_verdicts_snapshot() -> VerdictCache {
    tracker_verdicts()
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .clone()
}

fn tracker_verdicts_store(v: VerdictCache) {
    *tracker_verdicts().lock().unwrap_or_else(|e| e.into_inner()) = v;
}

/// Open a tracker fetch walk over `n` NUL-terminated URL strings
/// (`host` or `host:port`; default port 5498). `features` is the v3
/// handshake feature bitmask, `probe_ms` the v3-probe watchdog in
/// milliseconds. `verify_cert` (with `user_data`) is the TOFU trust
/// check consulted for non-WebPKI TLS certs; NULL accepts any
/// non-WebPKI cert (probe/test use only).
///
/// Returns an owned handle (free with [`hxnet_tracker_fetch_close`]) or
/// NULL on bad arguments.
///
/// # Safety
///
/// `urls` must point at `n` readable `*const c_char`, each a valid
/// NUL-terminated UTF-8 string living for the duration of this call.
/// `proxy_uri` is an optional NUL-terminated `socks5://…` URI to tunnel
/// the whole walk through (NULL = direct); a malformed / unsupported URI
/// fails the open (returns NULL). `user_data` must outlive the handle.
#[no_mangle]
pub unsafe extern "C" fn hxnet_tracker_fetch_open(
    urls: *const *const std::os::raw::c_char,
    n: usize,
    features: u16,
    probe_ms: u32,
    proxy_uri: *const std::os::raw::c_char,
    verify_cert: HxnetTrackerVerifyCallback,
    user_data: *mut c_void,
) -> *mut HxnetTrackerFetch {
    if urls.is_null() && n != 0 {
        glib::g_critical!("hxnet", "hxnet_tracker_fetch_open: NULL urls with n != 0");
        return std::ptr::null_mut();
    }
    // `urls.add(i)` for i in 0..n is UB if n * size_of::<*const c_char>()
    // overruns isize::MAX. Bound n the same way the other FFI entrypoints
    // bound their length arguments.
    if (n as u64).saturating_mul(std::mem::size_of::<*const c_void>() as u64)
        > (isize::MAX as u64)
    {
        glib::g_critical!(
            "hxnet",
            "hxnet_tracker_fetch_open: url count {} is implausibly large",
            n
        );
        return std::ptr::null_mut();
    }

    let mut url_vec: Vec<String> = Vec::with_capacity(n);
    for i in 0..n {
        let p = *urls.add(i);
        if p.is_null() {
            glib::g_critical!("hxnet", "hxnet_tracker_fetch_open: NULL url at index {}", i);
            return std::ptr::null_mut();
        }
        match std::ffi::CStr::from_ptr(p).to_str() {
            Ok(s) => {
                // Reject empty / whitespace-only entries at the boundary
                // so they fail deterministically here rather than turning
                // into an empty host + odd resolver / trust-cb inputs
                // deeper in the walk.
                if s.trim().is_empty() {
                    glib::g_critical!(
                        "hxnet",
                        "hxnet_tracker_fetch_open: url at index {} is empty / whitespace",
                        i
                    );
                    return std::ptr::null_mut();
                }
                url_vec.push(s.to_owned());
            }
            Err(_) => {
                glib::g_critical!(
                    "hxnet",
                    "hxnet_tracker_fetch_open: url at index {} is not valid UTF-8",
                    i
                );
                return std::ptr::null_mut();
            }
        }
    }

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(Runtime::global)) {
        Ok(rt) => rt,
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_tracker_fetch_open: Runtime::global panicked; aborting"
            );
            std::process::abort();
        }
    };

    let verify: Option<VerifyFn> = verify_cert.map(|cb| {
        let ud = SendUserData(user_data);
        let boxed: VerifyFn = Box::new(move |host: &str, port: u16, fp: &str| {
            let ud = &ud;
            unsafe { cb(host.as_ptr(), host.len(), port, fp.as_ptr(), fp.len(), ud.0) != 0 }
        });
        boxed
    });

    // Optional single SOCKS proxy for the whole walk (sourced once in C).
    // A malformed / unsupported URI fails the open rather than silently
    // connecting direct past a configured proxy.
    let proxy = if proxy_uri.is_null() {
        None
    } else {
        let bytes = std::ffi::CStr::from_ptr(proxy_uri).to_bytes();
        match parse_proxy_arg(bytes.as_ptr(), bytes.len()) {
            Ok(p) => p,
            Err(e) => {
                glib::g_critical!("hxnet", "hxnet_tracker_fetch_open: {}", e);
                return std::ptr::null_mut();
            }
        }
    };

    let probe_timeout = std::time::Duration::from_millis(probe_ms as u64);
    let (tx, rx) = mpsc::channel::<TrackerEvent>(64);
    let join = rt.handle().spawn(async move {
        let mut connector = TcpTlsConnector { verify, proxy };
        // Snapshot the process-global verdict cache so a Refresh doesn't
        // re-pay a known-failing TLS handshake, then write the result
        // back when the walk completes. Snapshot/restore bracket the
        // `.await` (no lock held across it); a cancelled walk that never
        // reaches the writeback just loses its updates, which is fine.
        let mut verdicts = tracker_verdicts_snapshot();
        run_fetch(&mut connector, &url_vec, features, probe_timeout, &mut verdicts, &tx).await;
        tracker_verdicts_store(verdicts);
    });

    Box::into_raw(Box::new(HxnetTrackerFetch {
        events: rx,
        join,
        current: None,
    }))
}

/// Drain one event into `out`. Returns [`HXNET_TRK_POLL_EVENT`] (a new
/// event was written, its borrowed pointers valid until the next poll /
/// close), [`HXNET_TRK_POLL_EMPTY`] (nothing ready now — try again),
/// or [`HXNET_TRK_POLL_CLOSED`] (the walk task finished and the channel
/// is drained — stop polling and close the handle).
///
/// # Safety
///
/// `handle` must be a live pointer from [`hxnet_tracker_fetch_open`] and
/// `out` must point at a writable [`HxnetTrackerEvent`].
#[no_mangle]
pub unsafe extern "C" fn hxnet_tracker_fetch_poll(
    handle: *mut HxnetTrackerFetch,
    out: *mut HxnetTrackerEvent,
) -> c_int {
    if handle.is_null() || out.is_null() {
        glib::g_critical!(
            "hxnet",
            "hxnet_tracker_fetch_poll: NULL handle or out pointer"
        );
        return HXNET_TRK_POLL_CLOSED;
    }
    let h = &mut *handle;
    match h.events.try_recv() {
        Ok(ev) => {
            h.current = Some(ev);
            fill_tracker_event(out, h.current.as_ref().unwrap());
            HXNET_TRK_POLL_EVENT
        }
        Err(mpsc::error::TryRecvError::Empty) => HXNET_TRK_POLL_EMPTY,
        Err(mpsc::error::TryRecvError::Disconnected) => HXNET_TRK_POLL_CLOSED,
    }
}

/// Cancel (if still running) and free a tracker fetch handle. NULL-safe.
/// Aborts the walk task and drops the receiver, which also wakes the
/// task if it's parked on a send.
///
/// # Safety
///
/// `handle` must be NULL or a live pointer from
/// [`hxnet_tracker_fetch_open`], not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn hxnet_tracker_fetch_close(handle: *mut HxnetTrackerFetch) {
    if handle.is_null() {
        return;
    }
    let h = Box::from_raw(handle);
    h.join.abort();
    drop(h);
}
