//! `spawn_blocking_with_idle` — the worker-pool replacement for the
//! `pthread_create` + `g_idle_add` pair (Phase R3.2).
//!
//! Per `docs/rust/ROADMAP.md` §R3 work items 3–5. The C side's
//! pre-R3 idiom for "run blocking I/O off the main thread, post the
//! result back" was a freshly-spawned detached pthread that called
//! `g_idle_add` on completion. R3.2 replaces both halves: the worker
//! runs on tokio's blocking-pool (shared across all R3+ consumers),
//! and the completion lands on whatever main context was thread-
//! default at the call site.
//!
//! # Why a blocking-pool task, not a regular `spawn`
//!
//! tokio's regular `spawn` is for tasks that drive async futures and
//! park / yield cooperatively. The banner HTXF worker (and the
//! upcoming xfers.c port) does synchronous GIO calls
//! (`g_input_stream_read_all`, `g_output_stream_write_all`, the
//! `htxf_io_read` AEAD-framing wrapper); none of those return
//! futures. Running them on a regular tokio worker would block the
//! single worker thread for the whole transfer, starving every
//! other R3 task. `spawn_blocking` is tokio's explicit escape hatch
//! for this: it runs the closure on a dedicated blocking-pool
//! thread that's sized for blocking work (default 512, set with
//! `Builder::max_blocking_threads`).
//!
//! When the blocking I/O paths migrate to tokio-native primitives
//! in R3.3 (`hxnet` Connection actor + `tokio::net::TcpStream`),
//! the consumers move off `spawn_blocking_with_idle` and onto
//! regular spawn + the [`crate::channel`] ferry. This helper stays
//! for the GIO-blocking consumers that haven't migrated yet.
//!
//! # Cancellation
//!
//! `spawn_blocking` tasks cannot be cancelled — same constraint
//! POSIX blocking sockets imposed on the legacy pthread path. The
//! C-side consumers handle this with generation counters (the
//! caller bumps a global generation on cancel; the completion
//! callback checks it and silently drops the result if the
//! generation moved). The shim doesn't try to abstract that — it's
//! a per-consumer concern. See `banner.c::htxf_generation` for the
//! reference shape.

use crate::runtime::Runtime;
use glib::MainContext;
use std::ffi::c_void;

/// Helper wrapper: `*mut c_void` isn't `Send`, so we wrap it in a
/// newtype with a manual `unsafe impl Send`. The C-side caller's
/// contract is that the pointee outlives both the worker and the
/// completion (the worker writes the result into a heap struct it
/// owns; the completion reads + frees it). The shim is opaque
/// about lifetime — that's per-consumer policy.
///
/// We deliberately store the raw pointer, NOT a `usize` round-trip.
/// Under Rust's strict-provenance model a `*ptr as usize as *ptr`
/// roundtrip loses the pointer's provenance — Miri flags any later
/// dereference (as the unit tests in this file do) as undefined
/// behaviour. Preserving the raw `*mut c_void` keeps provenance
/// intact end-to-end.
#[derive(Copy, Clone)]
struct SendPtr(*mut c_void);

// SAFETY: the C-side contract says `user_data` must outlive both
// callbacks and that neither callback's body races the other (the
// completion runs AFTER the worker, by construction). Wrapping the
// pointer in a Send marker so we can move it into the spawn_blocking
// closure is the standard pattern at FFI boundaries — the safety
// burden is on the C-side contract, not on the Send marker.
unsafe impl Send for SendPtr {}

impl SendPtr {
    #[inline]
    fn as_ptr(self) -> *mut c_void {
        self.0
    }
}

/// Spawn `worker(user_data)` on tokio's blocking pool. When it
/// returns, `completion(user_data)` runs on the GLib main loop's
/// thread (specifically `MainContext::ref_thread_default()` —
/// whatever was thread-default at the moment of this call).
///
/// Production callers run on the main thread, where
/// `ref_thread_default()` is the global default context. Tests can
/// push a private thread-default before calling and the completion
/// will land on that ctx instead — that's how the unit tests in
/// this file isolate from each other.
///
/// # Lifetimes / safety
///
/// - `worker` and `completion` are NUL-checked; the shim
///   `g_critical`s and returns without spawning if either is NULL.
/// - `user_data` is opaque — the C-side caller owns the pointee
///   and must keep it alive across both callbacks. Typical pattern
///   is to allocate a heap struct in the calling function, hand
///   the pointer here, and free it inside the completion callback.
/// - The blocking-pool thread the worker runs on is NOT the
///   thread that called this function. Anything the worker touches
///   must be safe to use off-main-thread. GtkSession's signals
///   (via [`crate::emit_pointer_pair_signal`]) ARE NOT safe to
///   emit from worker threads — emit them from the completion
///   callback instead.
///
/// # Safety
///
/// `worker` and `completion`, when non-null, must be valid C
/// function pointers with the documented signature. `user_data`
/// must remain valid for the lifetime of both callbacks.
pub unsafe fn spawn_blocking_with_idle(
    worker: Option<unsafe extern "C" fn(*mut c_void)>,
    completion: Option<unsafe extern "C" fn(*mut c_void)>,
    user_data: *mut c_void,
) {
    let Some(worker) = worker else {
        glib::g_critical!("hxbridge", "spawn_blocking_with_idle: NULL worker");
        return;
    };
    let Some(completion) = completion else {
        glib::g_critical!("hxbridge", "spawn_blocking_with_idle: NULL completion");
        return;
    };

    // Capture the thread-default ctx at the call site. The C side
    // is typically on the main thread where this is the default
    // main context; tests push a private ctx so they don't share
    // state across parallel runs.
    let ctx = MainContext::ref_thread_default();
    let raw = SendPtr(user_data);

    // Resolve the runtime singleton OUTSIDE the spawn_blocking
    // closure and behind catch_unwind. Runtime::global() panics
    // if the OS refuses to spawn the runtime's worker thread (or
    // any other OnceLock initialisation failure). A panic
    // unwinding here would cross the `extern "C"` boundary
    // (gtkhx_bridge_spawn_blocking_with_idle below) and trigger
    // undefined behaviour. Catch + abort keeps the unwind off the
    // FFI boundary.
    //
    // We catch here rather than at the FFI entry point because
    // Runtime::global() is the only call on this path that can
    // panic — the rest of the function is infallible. AssertUnwindSafe
    // is correct: we don't have mutable state that could be left
    // in an invalid intermediate state by the panic; we're just
    // calling a static accessor.
    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(Runtime::global)) {
        Ok(rt) => rt,
        Err(_) => {
            glib::g_critical!(
                "hxbridge",
                "spawn_blocking_with_idle: Runtime::global panicked; \
                 aborting to avoid unwinding across the FFI boundary"
            );
            std::process::abort();
        }
    };

    rt.spawn_blocking(move || {
        // Worker runs on a tokio blocking-pool thread. NOT the
        // GLib main thread. Anything that touches GTK state must
        // wait for the completion callback.
        //
        // SAFETY: caller's contract holds across both callbacks.
        unsafe {
            worker(raw.as_ptr());
        }

        // Post the completion onto the captured main context. The
        // closure moves the SendPtr (Send) and the completion fn
        // pointer (Send by virtue of being `unsafe extern "C" fn`,
        // a `Copy` type that's plain-old-data). `MainContext::invoke`
        // takes `FnOnce + Send + 'static`.
        let ctx_clone = ctx.clone();
        ctx_clone.invoke(move || {
            // SAFETY: same contract — user_data outlives this call.
            unsafe {
                completion(raw.as_ptr());
            }
        });
    });
}

/// FFI shim — C-callable form of [`spawn_blocking_with_idle`].
///
/// # Safety
///
/// See [`spawn_blocking_with_idle`].
#[no_mangle]
pub unsafe extern "C" fn gtkhx_bridge_spawn_blocking_with_idle(
    worker: Option<unsafe extern "C" fn(*mut c_void)>,
    completion: Option<unsafe extern "C" fn(*mut c_void)>,
    user_data: *mut c_void,
) {
    spawn_blocking_with_idle(worker, completion, user_data);
}

/// Queue `func(user_data)` to run on the GLib main loop, callable from
/// any thread. Behaviourally identical to the legacy
/// `gtkhx_post_to_main` (`g_main_context_invoke(NULL, func, data)`) it
/// replaces — `func` is a `GSourceFunc`, run on the **global default**
/// main context (the one the UI iterates), and its `G_SOURCE_REMOVE` /
/// `G_SOURCE_CONTINUE` return value is honoured exactly as
/// `g_main_context_invoke` honours it.
///
/// This exists so the xfers.c transfer worker's progress / cleanup
/// posts (`post_file_update` / `post_xfer_cleanup`) marshal back to the
/// main thread through hxbridge rather than through `gtkthreads.c` —
/// the last step before `gtkthreads.c` (whose only remaining export is
/// `gtkhx_post_to_main`) can retire (Phase R3 X2 → X4,
/// `docs/rust/ROADMAP.md`). It is intentionally a pure-GLib
/// shim with no tokio dependency; it lives beside
/// `spawn_blocking_with_idle` because both are the worker→main
/// marshalling surface the transfer worker uses.
///
/// # Safety
///
/// `func`, when non-null, must be a valid `GSourceFunc`. `user_data`
/// must remain valid until `func` runs on the main loop (the C-side
/// caller's dispatcher frees it).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_bridge_post_to_main(
    func: glib::ffi::GSourceFunc,
    user_data: *mut c_void,
) {
    if func.is_none() {
        glib::g_critical!("hxbridge", "gtkhx_bridge_post_to_main: NULL func");
        return;
    }
    // NULL context = the global default main context, exactly as the
    // legacy gtkhx_post_to_main passed. g_main_context_invoke is
    // thread-safe: a worker thread enqueues, the main thread runs it.
    glib::ffi::g_main_context_invoke(std::ptr::null_mut(), func, user_data);
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};
    use std::sync::Arc;
    use std::time::{Duration, Instant};

    // The tests need to pass typed Rust closures' worth of state
    // through opaque `user_data`. The trick is to allocate a heap
    // `Box<TestState>` on the calling side, leak it into the FFI
    // call, and reclaim it inside the completion closure.

    struct TestState {
        worker_ran: AtomicU32,
        completion_ran: AtomicU32,
        worker_tid: std::sync::OnceLock<std::thread::ThreadId>,
        completion_tid: std::sync::OnceLock<std::thread::ThreadId>,
    }

    impl TestState {
        fn new() -> Arc<Self> {
            Arc::new(Self {
                worker_ran: AtomicU32::new(0),
                completion_ran: AtomicU32::new(0),
                worker_tid: std::sync::OnceLock::new(),
                completion_tid: std::sync::OnceLock::new(),
            })
        }
    }

    // C-shaped callbacks. `user_data` is a `*mut Arc<TestState>` —
    // the test leaks an `Arc` into the FFI call and consumes it
    // back in the completion. We use Arc rather than Box so the
    // test's outer scope can also hold a clone for assertions.
    unsafe extern "C" fn test_worker(user_data: *mut c_void) {
        // SAFETY: the caller passes a valid `*mut Arc<TestState>`.
        let state = &*(user_data as *const Arc<TestState>);
        state.worker_ran.fetch_add(1, Ordering::SeqCst);
        let _ = state.worker_tid.set(std::thread::current().id());
    }

    unsafe extern "C" fn test_completion(user_data: *mut c_void) {
        // SAFETY: the worker has already run; the pointer is still
        // valid because the test outer scope holds a clone of the
        // Arc.
        let state = &*(user_data as *const Arc<TestState>);
        state.completion_ran.fetch_add(1, Ordering::SeqCst);
        let _ = state.completion_tid.set(std::thread::current().id());
    }

    /// Push a private MainContext as the thread-default for the
    /// test's scope. Pops on Drop. Same pattern as the channel
    /// tests — see that module's `private_ctx` for full rationale.
    struct ThreadDefaultGuard {
        ctx_ptr: *mut glib::ffi::GMainContext,
    }

    impl Drop for ThreadDefaultGuard {
        fn drop(&mut self) {
            unsafe {
                glib::ffi::g_main_context_pop_thread_default(self.ctx_ptr);
            }
        }
    }

    fn private_ctx() -> (MainContext, ThreadDefaultGuard) {
        use glib::translate::ToGlibPtr;
        let ctx = MainContext::new();
        let ctx_ptr: *mut glib::ffi::GMainContext = ctx.to_glib_none().0;
        unsafe {
            glib::ffi::g_main_context_push_thread_default(ctx_ptr);
        }
        (ctx, ThreadDefaultGuard { ctx_ptr })
    }

    /// Pump `ctx` until `pred()` is true or `deadline` elapses.
    /// Uses non-blocking iterations plus a 1 ms sleep so the loop
    /// doesn't spin-burn through 1000 iterations before the
    /// blocking-pool task even gets a chance to run.
    fn pump_until_deadline(ctx: &MainContext, pred: impl Fn() -> bool, deadline: Duration) -> bool {
        let start = Instant::now();
        while start.elapsed() < deadline {
            if pred() {
                return true;
            }
            ctx.iteration(false);
            if !pred() {
                std::thread::sleep(Duration::from_millis(1));
            }
        }
        pred()
    }

    #[test]
    fn worker_and_completion_both_run_in_order() {
        let (ctx, _guard) = private_ctx();

        let state = TestState::new();
        let state_for_ffi = state.clone();
        let raw_arc = Box::into_raw(Box::new(state_for_ffi)) as *mut c_void;

        unsafe {
            spawn_blocking_with_idle(Some(test_worker), Some(test_completion), raw_arc);
        }

        let drained = pump_until_deadline(
            &ctx,
            || state.completion_ran.load(Ordering::SeqCst) == 1,
            Duration::from_secs(5),
        );
        assert!(drained, "completion callback did not arrive within bound");

        assert_eq!(
            state.worker_ran.load(Ordering::SeqCst),
            1,
            "worker ran exactly once"
        );
        assert_eq!(
            state.completion_ran.load(Ordering::SeqCst),
            1,
            "completion ran exactly once"
        );

        // Reclaim the Arc box. SAFETY: we owned this with into_raw
        // above; nothing else freed it (the FFI shim treats it as
        // opaque).
        unsafe {
            drop(Box::from_raw(raw_arc as *mut Arc<TestState>));
        }
    }

    #[test]
    fn worker_runs_on_blocking_pool_thread() {
        let (ctx, _guard) = private_ctx();

        let main_tid = std::thread::current().id();
        let state = TestState::new();
        let state_for_ffi = state.clone();
        let raw_arc = Box::into_raw(Box::new(state_for_ffi)) as *mut c_void;

        unsafe {
            spawn_blocking_with_idle(Some(test_worker), Some(test_completion), raw_arc);
        }

        // Assert the completion landed before reclaiming the
        // leaked Box. If pump_until_deadline timed out, the
        // completion is still in flight on the captured main
        // context (MainContext::invoke queued it; only the test's
        // pump can run it). Reclaiming the Box while a
        // completion-firing-on-stale-user_data is pending is a
        // use-after-free. Panic before the reclaim — better to
        // leak the test's Arc than UAF. Same fix applies to
        // completion_runs_on_main_thread below.
        let drained = pump_until_deadline(
            &ctx,
            || state.completion_ran.load(Ordering::SeqCst) == 1,
            Duration::from_secs(5),
        );
        assert!(
            drained,
            "completion did not arrive within deadline; cannot reclaim Box without risking UAF"
        );

        let worker_tid = state
            .worker_tid
            .get()
            .copied()
            .expect("worker recorded tid");
        assert_ne!(
            worker_tid, main_tid,
            "worker must NOT run on the caller's (main) thread — that would defeat the blocking-pool purpose"
        );

        unsafe {
            drop(Box::from_raw(raw_arc as *mut Arc<TestState>));
        }
    }

    #[test]
    fn completion_runs_on_main_thread() {
        let (ctx, _guard) = private_ctx();

        let main_tid = std::thread::current().id();
        let state = TestState::new();
        let state_for_ffi = state.clone();
        let raw_arc = Box::into_raw(Box::new(state_for_ffi)) as *mut c_void;

        unsafe {
            spawn_blocking_with_idle(Some(test_worker), Some(test_completion), raw_arc);
        }

        let drained = pump_until_deadline(
            &ctx,
            || state.completion_ran.load(Ordering::SeqCst) == 1,
            Duration::from_secs(5),
        );
        assert!(
            drained,
            "completion did not arrive within deadline; cannot reclaim Box without risking UAF"
        );

        let comp_tid = state
            .completion_tid
            .get()
            .copied()
            .expect("completion recorded tid");
        assert_eq!(
            comp_tid, main_tid,
            "completion MUST run on the captured main context's thread — it touches UI"
        );

        unsafe {
            drop(Box::from_raw(raw_arc as *mut Arc<TestState>));
        }
    }

    #[test]
    fn null_worker_is_a_clean_noop() {
        let (ctx, _guard) = private_ctx();

        let state = TestState::new();
        let state_for_ffi = state.clone();
        let raw_arc = Box::into_raw(Box::new(state_for_ffi)) as *mut c_void;

        unsafe {
            // Passing NULL worker: shim must g_critical and return,
            // NOT spawn anything, NOT run completion either.
            spawn_blocking_with_idle(None, Some(test_completion), raw_arc);
        }

        // Pump some iterations to give a misbehaving impl a chance
        // to run something.
        for _ in 0..64 {
            ctx.iteration(false);
        }

        assert_eq!(
            state.worker_ran.load(Ordering::SeqCst),
            0,
            "no worker should have run"
        );
        assert_eq!(
            state.completion_ran.load(Ordering::SeqCst),
            0,
            "no completion should have run"
        );

        unsafe {
            drop(Box::from_raw(raw_arc as *mut Arc<TestState>));
        }
    }

    #[test]
    fn null_completion_is_a_clean_noop() {
        let (ctx, _guard) = private_ctx();

        let state = TestState::new();
        let state_for_ffi = state.clone();
        let raw_arc = Box::into_raw(Box::new(state_for_ffi)) as *mut c_void;

        unsafe {
            spawn_blocking_with_idle(Some(test_worker), None, raw_arc);
        }

        // No worker should run — the shim rejects NULL completion
        // BEFORE spawning. Pump to confirm.
        for _ in 0..64 {
            ctx.iteration(false);
        }

        assert_eq!(
            state.worker_ran.load(Ordering::SeqCst),
            0,
            "no worker should have run (shim rejected NULL completion first)"
        );

        unsafe {
            drop(Box::from_raw(raw_arc as *mut Arc<TestState>));
        }
    }

    #[test]
    fn many_concurrent_spawns_all_complete() {
        // The blocking pool's scheduling has to be robust across
        // dozens of small jobs. Fire 32 spawns at once and verify
        // every completion arrives.
        let (ctx, _guard) = private_ctx();

        let n = 32;
        let states: Vec<Arc<TestState>> = (0..n).map(|_| TestState::new()).collect();
        let raw_arcs: Vec<*mut c_void> = states
            .iter()
            .map(|s| Box::into_raw(Box::new(s.clone())) as *mut c_void)
            .collect();

        for raw in &raw_arcs {
            unsafe {
                spawn_blocking_with_idle(Some(test_worker), Some(test_completion), *raw);
            }
        }

        let deadline = Instant::now() + Duration::from_secs(5);
        let all_done = || {
            states
                .iter()
                .all(|s| s.completion_ran.load(Ordering::SeqCst) == 1)
        };
        while Instant::now() < deadline && !all_done() {
            ctx.iteration(false);
        }
        assert!(all_done(), "all 32 completions must arrive");

        for raw in raw_arcs {
            unsafe {
                drop(Box::from_raw(raw as *mut Arc<TestState>));
            }
        }
    }

    #[test]
    fn post_to_main_runs_func_on_default_context() {
        // gtkhx_bridge_post_to_main queues onto the GLOBAL default main
        // context (not a thread-default). Serialise against every other
        // default-context test (here + lib.rs) via the crate-wide lock,
        // then acquire — otherwise parallel acquire() can fail.
        let _ser = crate::DEFAULT_CTX_TEST_LOCK
            .lock()
            .unwrap_or_else(|p| p.into_inner());
        let ctx = MainContext::default();
        let _guard = ctx.acquire().expect("acquire default main context");

        static RAN: AtomicU32 = AtomicU32::new(0);
        RAN.store(0, Ordering::SeqCst);

        unsafe extern "C" fn cb(_data: *mut c_void) -> glib::ffi::gboolean {
            RAN.fetch_add(1, Ordering::SeqCst);
            glib::ffi::GFALSE // G_SOURCE_REMOVE — run once
        }

        unsafe {
            gtkhx_bridge_post_to_main(Some(cb), std::ptr::null_mut());
        }

        let deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < deadline && RAN.load(Ordering::SeqCst) == 0 {
            ctx.iteration(false);
        }
        assert_eq!(
            RAN.load(Ordering::SeqCst),
            1,
            "posted GSourceFunc ran exactly once on the default main context"
        );
    }

    #[test]
    fn post_to_main_null_func_is_a_clean_noop() {
        // NULL GSourceFunc must g_critical and return without queuing
        // anything — no panic across the FFI boundary.
        let _ser = crate::DEFAULT_CTX_TEST_LOCK
            .lock()
            .unwrap_or_else(|p| p.into_inner());
        let ctx = MainContext::default();
        let _guard = ctx.acquire().expect("acquire default main context");
        unsafe {
            gtkhx_bridge_post_to_main(None, std::ptr::null_mut());
        }
        // Nothing to assert beyond "did not crash"; pump a few times to
        // give any erroneously-queued source a chance to misbehave.
        for _ in 0..16 {
            ctx.iteration(false);
        }
    }
}
