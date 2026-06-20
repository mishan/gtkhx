//! Rust ↔ GLib bridge for GtkHx (Phases R3.0 + R3.1, see
//! `docs/voice-chat-plan.md` §5, `docs/RUST-ROADMAP.md` §R3, and
//! `docs/rust-glib-interop.md` for the lifetime-model rationale).
//!
//! Phase R3.0 ships the C-owned-GObject wrapping helpers and a single
//! reference signal emit (this file). Phase R3.1 — the [`runtime`]
//! and [`channel`] submodules — adds the dedicated-thread tokio
//! runtime and the tokio→GLib event ferry that subsequent R3.x
//! worker ports (banner.c R3.2, hxnet Connection actor R3.3,
//! xfers.c R3.4) build on. Both R3.1 modules are gated behind the
//! `runtime` cargo feature so the default hxbridge build stays lean
//! until a consumer turns the feature on.
//!
//! # Lifetime model
//!
//! GtkHx's `GtkhxSession` is a C-owned GObject. It outlives any single
//! Rust-side reference to it (it's a singleton constructed at
//! `gtkhx_session_get_default` and held by the GtkApplication for the
//! process lifetime). The Rust side has two ways to take a borrow:
//!
//! - [`session_from_ptr`] uses `from_glib_borrow`: no ref-count change.
//!   Safe for **read-only access** (`get_property`, `is_type`, …) where
//!   the Rust handle's lifetime is strictly bounded by a single
//!   stack frame and **does not cross any code path that could
//!   `g_object_unref` the session**.
//!
//! - [`session_from_ptr_full`] uses `from_glib_none`: adds one ref,
//!   dropped when the Rust `glib::Object` goes out of scope. Required
//!   for **signal emit**, because GObject signal dispatch is
//!   synchronous and re-entrant — a connected C handler can call
//!   `g_object_unref` on the session before the emit returns, leaving
//!   a borrowed Rust handle dangling. The extra ref is one atomic
//!   increment per call (`g_object_ref` is `g_atomic_int_inc` on the
//!   refcount), which is cheap compared to the cost of a use-after-free
//!   crash.
//!
//! Rule of thumb: if the Rust handle ever calls `emit_by_name` (or any
//! other function that runs user code with a re-entrancy risk), use
//! the `_full` variant.

use glib::object::ObjectExt;
use glib::translate::Borrowed;
use glib::translate::FromGlibPtrBorrow;
use glib::translate::FromGlibPtrNone;
use std::ffi::{c_char, c_void, CStr};

// Phase R3.1+ submodules — gated behind the `runtime` feature so
// the default cargo-test build can still exercise the R3.0 wrapping
// helpers without dragging in tokio. R3.2 (banner.c) flips the
// feature on in the meson build.
#[cfg(feature = "runtime")]
pub mod blocking;
#[cfg(feature = "runtime")]
pub mod channel;
#[cfg(feature = "runtime")]
pub mod runtime;

// Phase 8.B forward-looking imports. These crates are listed in
// Cargo.toml as optional deps under the `voice` feature, so they
// resolve into Cargo.lock + cargo-sources.json vendoring at R3.0 time
// rather than at 8.B PR time. The default build doesn't compile them
// (the gstreamer-webrtc-sys build.rs needs GStreamer 1.26 dev libs
// that won't land in the Flatpak runtime until the GNOME 48 bump).
// The `as _` form pulls each crate into scope without binding a name
// when the feature is on, which keeps `unused_crate_dependencies`
// quiet and is the canonical idiom for "build-time dep, not yet a
// code dep."
#[cfg(feature = "voice")]
mod _voice_deps_imports {
    use gstreamer as _;
    use gstreamer_app as _;
    use gstreamer_audio as _;
    use gstreamer_rtp as _;
    use gstreamer_sdp as _;
    use gstreamer_webrtc as _;
    use gtk4 as _;
    use libadwaita as _;
}

/// Borrow a C-owned `GtkhxSession` as a Rust `glib::Object` for the
/// duration of the call. The returned [`Borrowed<glib::Object>`] does
/// **not** add a ref to the underlying GObject — zero atomic
/// operations on the refcount on wrap, zero on drop. The caller's
/// C-side reference must outlive every use of the returned handle.
///
/// Use the result via deref: `borrow.property::<T>(name)`,
/// `borrow.is::<T>()`, etc. — `Borrowed<glib::Object>` derefs to
/// `&glib::Object`.
///
/// Use this for read-only access. For signal emit, use
/// [`session_from_ptr_full`] instead — the borrow form is unsafe under
/// re-entrant emit because a connected C handler can `g_object_unref`
/// the session before the emit returns, leaving the borrowed handle
/// dangling.
///
/// # Safety
///
/// `ptr` must be a valid `GObject*` (or subclass thereof) pointer that
/// remains live for the entire lifetime of the returned
/// `Borrowed<glib::Object>`. Passing `NULL` is undefined behaviour —
/// callers are expected to reject NULL before this call. Passing a
/// pointer whose type is not a subclass of `GObject` is undefined
/// behaviour.
#[inline]
pub unsafe fn session_from_ptr(ptr: *mut c_void) -> Borrowed<glib::Object> {
    debug_assert!(!ptr.is_null(), "hxbridge::session_from_ptr: NULL ptr");
    // `from_glib_borrow` returns a `Borrowed<T>` that wraps the raw
    // pointer without adding a ref. The wrapper derefs to `&T`, so
    // callers get all `ObjectExt` methods via auto-deref — but the
    // Borrowed itself cannot be cloned (which would add a ref) and
    // its lifetime is bounded by the caller's stack frame.
    glib::Object::from_glib_borrow(ptr as *mut glib::gobject_ffi::GObject)
}

/// Wrap a C-owned `GtkhxSession` as a Rust `glib::Object`, adding a
/// reference that is dropped when the returned handle goes out of
/// scope. Use this for signal emit and any other call that might run
/// re-entrant C handlers.
///
/// The extra `g_object_ref` is one atomic increment; the cost is
/// negligible compared to the use-after-free window the borrow form
/// would open.
///
/// # Safety
///
/// `ptr` must be a valid `GObject*` (or subclass thereof) pointer.
/// Passing `NULL` is undefined behaviour. Passing a pointer whose type
/// is not a subclass of `GObject` is undefined behaviour.
#[inline]
pub unsafe fn session_from_ptr_full(ptr: *mut c_void) -> glib::Object {
    debug_assert!(!ptr.is_null(), "hxbridge::session_from_ptr_full: NULL ptr");
    glib::Object::from_glib_none(ptr as *mut glib::gobject_ffi::GObject)
}

// ---- Reference signal emit ---------------------------------------------

/// Emit a `(G_TYPE_POINTER, G_TYPE_POINTER)` signal on a C-owned
/// `GtkhxSession`. This is the canonical Phase R3.0 reference for
/// "how do I drive a C-side signal emit from Rust" — see
/// `docs/rust-glib-interop.md` for the rationale.
///
/// Wraps `session_ptr` via [`session_from_ptr_full`] so the session
/// stays alive across the emit even if a connected C handler runs
/// `g_object_unref` on it (the re-entrant-safe path).
///
/// `signal_name` is a NUL-terminated UTF-8 string (the signal as
/// registered in `gtkhx_session.c::class_init`). `arg0` and `arg1`
/// are opaque pointers that ride through the signal as
/// `G_TYPE_POINTER` — the spec is that the emitter owns the lifetime
/// of whatever they point to for the duration of the emit; the
/// handlers must not retain.
///
/// # Safety
///
/// `session_ptr` must be a valid non-NULL `GtkhxSession*`.
/// `signal_name` must be a valid NUL-terminated UTF-8 C string.
/// `arg0`/`arg1` lifetimes must outlast the synchronous emit call.
pub unsafe fn emit_pointer_pair_signal(
    session_ptr: *mut c_void,
    signal_name: &CStr,
    arg0: *mut c_void,
    arg1: *mut c_void,
) {
    let session = session_from_ptr_full(session_ptr);
    let name = match signal_name.to_str() {
        Ok(s) => s,
        Err(e) => {
            // Caller-side bug: a non-UTF-8 signal name can never match a
            // registered GLib signal (names are ASCII identifiers). Route
            // through GLib's critical-logging channel so the message
            // shows up alongside other GtkHx diagnostics and so
            // `G_LOG_FATAL_CRITICAL=hxbridge` in CI/tests can promote
            // it to an abort — otherwise a typo at the FFI boundary
            // would look like a silently-missing signal emission.
            glib::g_critical!(
                "hxbridge",
                "emit_pointer_pair_signal: signal name is not valid UTF-8 \
                 ({} bytes valid before error: {}); emission skipped",
                e.valid_up_to(),
                e,
            );
            return;
        }
    };
    // Construct G_TYPE_POINTER Values for the two opaque pointer
    // args. glib-rs doesn't expose a safe `From<*mut T>` because the
    // pointer's pointee is unknown to the type system; we go through
    // the raw `g_value_set_pointer` path. The Values are dropped at
    // the end of the call, freeing the GValue boxes but NOT touching
    // what they point at — G_TYPE_POINTER is borrow semantics.
    let v0 = pointer_value(arg0);
    let v1 = pointer_value(arg1);
    let _ret = session.emit_by_name_with_values(name, &[v0, v1]);
}

/// Build a `G_TYPE_POINTER`-typed `glib::Value` holding the raw
/// pointer. The Value box itself is owned by the caller and freed on
/// drop; the pointer's pointee is borrowed (G_TYPE_POINTER doesn't ref
/// or copy).
fn pointer_value(p: *mut c_void) -> glib::Value {
    use glib::translate::ToGlibPtrMut;
    let mut v = unsafe { glib::Value::from_type_unchecked(glib::Type::POINTER) };
    unsafe {
        glib::gobject_ffi::g_value_set_pointer(v.to_glib_none_mut().0, p);
    }
    v
}

/// FFI shim for [`emit_pointer_pair_signal`]. C side calls this from
/// the legacy `gtkhx_session_emit_*` wrappers for any
/// `(G_TYPE_POINTER, G_TYPE_POINTER)`-shaped signal.
///
/// `signal_name` is `*const c_char` (rather than `*const i8`) so the
/// signature is correct on targets where `char` is unsigned (e.g.
/// aarch64). Matches the existing R1/R2 FFI shim style.
///
/// # Safety
///
/// Same as [`emit_pointer_pair_signal`], plus:
/// - `signal_name` must be a valid NUL-terminated C string pointer.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_bridge_emit_pointer_pair_signal(
    session_ptr: *mut c_void,
    signal_name: *const c_char,
    arg0: *mut c_void,
    arg1: *mut c_void,
) {
    if session_ptr.is_null() || signal_name.is_null() {
        // Caller-side bug. Route through GLib's logging so the message
        // appears alongside other GtkHx diagnostics (and so the
        // G_LOG_FATAL_CRITICAL env var can promote it to abort in
        // tests) rather than going to plain stderr.
        glib::g_critical!(
            "hxbridge",
            "gtkhx_bridge_emit_pointer_pair_signal: NULL session_ptr or signal_name"
        );
        return;
    }
    let name = CStr::from_ptr(signal_name);
    emit_pointer_pair_signal(session_ptr, name, arg0, arg1);
}

#[cfg(test)]
mod tests {
    use super::*;
    use glib::object::ObjectExt;
    use std::cell::Cell;
    use std::rc::Rc;

    /// Get the raw `*mut GObject` for an `Object` without consuming it
    /// or changing its ref-count. The owning Rust handle keeps the
    /// underlying GObject alive for the duration of any test that uses
    /// the returned pointer.
    fn raw_gobject<T: glib::object::ObjectType>(obj: &T) -> *mut c_void {
        obj.as_ptr() as *mut c_void
    }

    // ---- Test signaller: minimal GObject subclass with a single
    //      (G_TYPE_POINTER, G_TYPE_POINTER) signal so we can exercise
    //      the emit path against a known type. ----

    glib::wrapper! {
        pub struct TestSignaller(ObjectSubclass<imp::TestSignaller>);
    }

    impl TestSignaller {
        fn new() -> Self {
            glib::Object::new::<Self>()
        }
    }

    mod imp {
        use glib::subclass::prelude::*;
        use glib::subclass::Signal;
        use std::sync::OnceLock;

        #[derive(Default)]
        pub struct TestSignaller;

        #[glib::object_subclass]
        impl ObjectSubclass for TestSignaller {
            const NAME: &'static str = "HxBridgeTestSignaller";
            type Type = super::TestSignaller;
            type ParentType = glib::Object;
        }

        impl ObjectImpl for TestSignaller {
            fn signals() -> &'static [Signal] {
                static SIGNALS: OnceLock<Vec<Signal>> = OnceLock::new();
                SIGNALS.get_or_init(|| {
                    vec![Signal::builder("test-pair")
                        .param_types([glib::Type::POINTER, glib::Type::POINTER])
                        .build()]
                })
            }
        }
    }

    // ---- W2 ref-count tests (kept from skeleton) ----

    #[test]
    fn session_from_ptr_borrow_does_not_change_refcount() {
        let obj = glib::Object::new::<glib::Object>();
        let raw = raw_gobject(&obj);
        let before = obj.ref_count();
        // session_from_ptr now returns Borrowed<Object>, which is a
        // zero-cost wrapper over the raw pointer. The borrow form
        // promises ZERO refcount change — not "net zero across the
        // call", but no atomic op at all. Check that the refcount is
        // unchanged while the borrow is alive, not just after drop.
        {
            let borrow = unsafe { session_from_ptr(raw) };
            // Use the borrow via deref so the test exercises the real
            // call shape. ObjectExt methods auto-deref through
            // Borrowed<Object>.
            assert_eq!(
                borrow.ref_count(),
                before,
                "borrow form added a ref while the handle was alive"
            );
        }
        let after = obj.ref_count();
        assert_eq!(after, before, "borrow form changed refcount on drop");
    }

    #[test]
    fn session_from_ptr_full_holds_a_ref_while_alive() {
        let obj = glib::Object::new::<glib::Object>();
        let raw = raw_gobject(&obj);
        let before = obj.ref_count();
        let handle = unsafe { session_from_ptr_full(raw) };
        let during = obj.ref_count();
        assert_eq!(during, before + 1);
        drop(handle);
        let after = obj.ref_count();
        assert_eq!(after, before);
    }

    // ---- W5 emit-path tests ----

    #[test]
    fn emit_observed_by_connected_handler() {
        let signaller = TestSignaller::new();
        let saw = Rc::new(Cell::new(0u32));
        let saw_clone = saw.clone();
        signaller.connect_local("test-pair", false, move |_| {
            saw_clone.set(saw_clone.get() + 1);
            None
        });

        let raw = raw_gobject(&signaller);
        let sig_name = CStr::from_bytes_with_nul(b"test-pair\0").unwrap();
        let arg0: *mut c_void = 0xdead_beef as *mut c_void;
        let arg1: *mut c_void = 0xcafe_babe as *mut c_void;
        unsafe { emit_pointer_pair_signal(raw, sig_name, arg0, arg1) };

        assert_eq!(saw.get(), 1, "handler observed exactly one emit");
    }

    #[test]
    fn refcount_stable_across_many_emits() {
        // The session_from_ptr_full inside the emit path takes one ref,
        // drops it on Rust handle drop. Across N emits, refcount returns
        // to its starting value — no leak.
        let signaller = TestSignaller::new();
        signaller.connect_local("test-pair", false, |_| None);

        let raw = raw_gobject(&signaller);
        let sig_name = CStr::from_bytes_with_nul(b"test-pair\0").unwrap();
        let before = signaller.ref_count();
        for _ in 0..1000 {
            unsafe {
                emit_pointer_pair_signal(
                    raw,
                    sig_name,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                );
            }
        }
        let after = signaller.ref_count();
        assert_eq!(after, before, "no ref leak across 1000 emits");
    }

    #[test]
    fn reentrant_emit_is_safe() {
        // Re-entrant emit (nested emit, no unref): an outer emit's
        // handler triggers another emit on the same signal. Confirms
        // dispatch tolerates re-entrancy and refcount remains stable.
        // Does NOT pin the from_glib_none decision on its own — for
        // that, see `full_form_survives_handler_unref_during_emit`
        // below.
        let signaller = TestSignaller::new();
        let depth = Rc::new(Cell::new(0u32));
        let max_depth = Rc::new(Cell::new(0u32));

        let depth_inner = depth.clone();
        let max_depth_inner = max_depth.clone();
        let signaller_ptr = raw_gobject(&signaller);
        signaller.connect_local("test-pair", false, move |_| {
            let d = depth_inner.get() + 1;
            depth_inner.set(d);
            if d > max_depth_inner.get() {
                max_depth_inner.set(d);
            }
            // Re-enter once — emit again from inside the handler.
            if d < 2 {
                let sig_name = CStr::from_bytes_with_nul(b"test-pair\0").unwrap();
                unsafe {
                    emit_pointer_pair_signal(
                        signaller_ptr,
                        sig_name,
                        std::ptr::null_mut(),
                        std::ptr::null_mut(),
                    );
                }
            }
            depth_inner.set(d - 1);
            None
        });

        let raw = raw_gobject(&signaller);
        let sig_name = CStr::from_bytes_with_nul(b"test-pair\0").unwrap();
        let before = signaller.ref_count();
        unsafe {
            emit_pointer_pair_signal(
                raw,
                sig_name,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            );
        }
        let after = signaller.ref_count();

        assert_eq!(max_depth.get(), 2, "handler re-entered exactly once");
        assert_eq!(depth.get(), 0, "depth counter unwound cleanly");
        assert_eq!(after, before, "refcount stable across re-entrant emit");
    }

    #[test]
    fn full_form_survives_handler_unref_during_emit() {
        // Pins the load-bearing reason `emit_pointer_pair_signal` uses
        // `session_from_ptr_full` (= `from_glib_none`, +1 ref on wrap)
        // instead of `session_from_ptr` (= `from_glib_borrow`, no ref
        // change). The full form's ref is a defensive measure: any
        // post-emit code path that reads from the session handle (a
        // property, an `is::<T>` check, etc.) would UAF under the
        // borrow form if a handler closed the last user-side ref —
        // and convention is that future maintainers extending this
        // function shouldn't need to re-derive the safety analysis.
        //
        // Scenario:
        //   1. Build a signaller; take an extra C-side ref so the
        //      object survives the Rust binding's drop. refcount = 2.
        //   2. Drop the Rust binding. refcount = 1 (extra only).
        //   3. Handler reads the live refcount via a fresh borrow
        //      (no perturbation), then drops the extra ref.
        //
        // Observable refcount inside the handler:
        //   - full form  (current code): refcount = 3
        //   - borrow form (regression):  refcount = 2
        //
        // The two +1s the handler sees with the full form are:
        //   1. The shim's `session_from_ptr_full` ref (the load-bearing
        //      one this test pins).
        //   2. glib-rs's `emit_by_name_with_values` wraps the instance
        //      in a `Value` to pass to `g_signal_emitv`, and
        //      `g_value_set_object` refs the object for the lifetime
        //      of that Value. This +1 is present under both forms, so
        //      it doesn't change the delta — but it does mean the
        //      borrow-form regression value is 2, not 1.
        let signaller = TestSignaller::new();
        let raw = raw_gobject(&signaller);

        // Extra C-side ref so refcount survives the Rust binding drop.
        unsafe {
            glib::gobject_ffi::g_object_ref(raw as *mut glib::gobject_ffi::GObject);
        }

        let observed = Rc::new(Cell::new(0u32));
        let flag = observed.clone();
        // Smuggle the raw pointer past the closure's send-bound check
        // (raw pointers are !Send but usize is).
        let captured_raw = raw as usize;

        signaller.connect_local("test-pair", false, move |_args| {
            let ptr = captured_raw as *mut glib::gobject_ffi::GObject;
            // Snapshot refcount via a borrow (no ref change).
            let snapshot = unsafe {
                let borrow: Borrowed<glib::Object> =
                    glib::Object::from_glib_borrow(ptr);
                borrow.ref_count()
            };
            flag.set(snapshot);
            // Now drop the extra external ref. With the full form, the
            // shim's wrap keeps refcount > 0 across this drop; with the
            // borrow form, this would finalize the object mid-handler.
            unsafe {
                glib::gobject_ffi::g_object_unref(ptr);
            }
            None
        });

        // Drop the Rust binding: refcount = 2 → 1 (extra ref only).
        drop(signaller);

        let sig_name = CStr::from_bytes_with_nul(b"test-pair\0").unwrap();
        unsafe {
            emit_pointer_pair_signal(
                raw,
                sig_name,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            );
        }
        // After the emit returns:
        //   full form: shim wrap drops → refcount = 0 → finalize.
        //   borrow form: already finalized inside the handler (UAF).

        assert_eq!(
            observed.get(),
            3,
            "Expected handler to see refcount = 3 (extra ref + shim's \
             from_glib_none wrap + emit_by_name_with_values's instance \
             Value ref). Got {}. A value of 2 means the FFI shim has \
             regressed from session_from_ptr_full to session_from_ptr — \
             any post-emit handle access would risk use-after-free.",
            observed.get(),
        );
    }

    #[test]
    fn malformed_utf8_signal_name_logs_and_returns_cleanly() {
        // Caller-side bug regression check: passing a non-UTF-8 signal
        // name must NOT panic, abort, or invoke signal dispatch — it
        // must g_critical and return cleanly. We verify it returns
        // by observing that no handler ran and the session refcount
        // is unchanged after the call.
        let signaller = TestSignaller::new();
        let saw = Rc::new(Cell::new(0u32));
        let saw_clone = saw.clone();
        signaller.connect_local("test-pair", false, move |_| {
            saw_clone.set(saw_clone.get() + 1);
            None
        });

        let raw = raw_gobject(&signaller);
        // Build a CStr that holds raw bytes that aren't valid UTF-8.
        // 0xFF is invalid as the first byte of any UTF-8 sequence.
        let bad: &[u8] = b"\xff\x00";
        let bad_cstr = CStr::from_bytes_with_nul(bad).unwrap();

        let before = signaller.ref_count();
        unsafe {
            emit_pointer_pair_signal(
                raw,
                bad_cstr,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            );
        }
        let after = signaller.ref_count();

        assert_eq!(saw.get(), 0, "no handler should have run");
        assert_eq!(after, before, "refcount should be unchanged");
    }

    #[test]
    fn spawn_local_actually_polls_future() {
        let ctx = glib::MainContext::default();
        let _guard = ctx.acquire().expect("acquired default main context");

        let triggered = Rc::new(Cell::new(false));
        let t = triggered.clone();
        ctx.spawn_local(async move {
            t.set(true);
        });

        // Pump the main context until idle. The future is one-shot —
        // a few iterations should be enough.
        for _ in 0..16 {
            if !ctx.iteration(false) {
                break;
            }
        }

        assert!(
            triggered.get(),
            "spawn_local'd future ran on the default main context"
        );
    }
}
