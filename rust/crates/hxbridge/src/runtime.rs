//! Tokio runtime hosted on a dedicated OS thread (Phase R3.1).
//!
//! Per `docs/rust/ROADMAP.md` §R3 work items 1–2: every R3+ worker —
//! the upcoming `hxnet` Connection actor, the banner.c HTXF port
//! (R3.2), the xfers.c transfer workers (R3.4) — needs a single
//! shared tokio runtime to schedule against. The roadmap's
//! locked-in decision (item 4 of "Locked-in decisions"):
//!
//! > **Async runtime: tokio in a dedicated thread, GLib MainContext
//! > on the UI.** […] `tokio::runtime::Runtime` lives on a worker
//! > thread, the main thread runs `glib::MainContext::default()` as
//! > the GTK event loop, communication crosses via
//! > `tokio::sync::mpsc` / `glib::MainContext::channel`.
//!
//! This module is the foothold. It exposes a [`Runtime`] type that
//! owns a `tokio::runtime::Runtime`, plus [`Runtime::global`] for the
//! shared process-lifetime instance used by all production R3+ code.
//!
//! # Why one runtime, not one-per-task
//!
//! The leaf-up port lands worker code piecemeal: banner first, then
//! the Connection actor, then transfers. Giving each a private
//! runtime would waste worker threads and complicate shutdown.
//! Instead, every R3+ consumer calls [`Runtime::global`] and spawns
//! against the shared scheduler. The first call lazily starts the
//! runtime; subsequent calls just hand back the handle.
//!
//! # Why a dedicated OS thread, not the main thread
//!
//! `glib::MainContext::default()` already owns the main OS thread —
//! it's the GTK event loop. Hoisting tokio onto the same thread would
//! either (a) block the UI whenever tokio is busy, or (b) require a
//! custom `MainContext`-aware reactor (which exists in `glib-rs` but
//! complicates every async fn's executor assumption). Spawning a
//! dedicated thread to host `tokio::runtime::Runtime` is the
//! well-trodden pattern from the gtk-rs book and from
//! [balena's rust-async-interop example][bal] — and exactly what
//! the roadmap calls for.
//!
//! [bal]: https://github.com/balena-io-experimental/rust-async-interop
//!
//! # Cross-thread marshalling
//!
//! Tokio-side tasks send events back to the GLib main thread via
//! [`crate::channel`] — the `async_channel::Sender<T>` /
//! `async_channel::Receiver<T>` pair this crate's
//! [`forward_to_main`](crate::channel::forward_to_main) drives.
//! This module owns the runtime; the channel module owns the ferry.

use std::future::Future;
use std::sync::OnceLock;
use tokio::runtime;
use tokio::task::JoinHandle;

/// Wrapper around a `tokio::runtime::Runtime` that lives on a
/// dedicated thread inside the multi-thread scheduler tokio creates
/// for us.
///
/// Construction is fallible (the runtime needs to spawn its worker
/// thread); production code uses [`Runtime::global`], which lazily
/// constructs the singleton and panics on failure — failure at that
/// point means the process is too broken to make progress. Tests can
/// use [`Runtime::new`] for private instances.
pub struct Runtime {
    inner: runtime::Runtime,
}

impl Runtime {
    /// Build a fresh runtime. Allocates one OS thread for the
    /// scheduler plus N worker threads (`worker_threads(1)` is
    /// enough for the R3 workload — most of the work is socket I/O
    /// gated on the kernel, not CPU-bound).
    ///
    /// Returns `Err` only if the OS refuses to spawn a thread.
    pub fn new() -> std::io::Result<Self> {
        let inner = runtime::Builder::new_multi_thread()
            .worker_threads(1)
            .thread_name("hxbridge-tokio")
            .enable_io()
            .enable_time()
            .build()?;
        Ok(Self { inner })
    }

    /// Process-lifetime shared runtime. Lazily started on the first
    /// call. Subsequent calls return the same instance.
    ///
    /// Panics if the OS refuses to spawn the worker thread; this is
    /// the same failure mode as `pthread_create` returning ENOMEM in
    /// the legacy C path (`xfers.c::xfer_dispatch` logs and
    /// gracefully fails the transfer). Since this is *process*-wide
    /// startup, not a per-transfer call, the failure mode is "the
    /// program cannot run" — abort is the honest response.
    pub fn global() -> &'static Runtime {
        static INSTANCE: OnceLock<Runtime> = OnceLock::new();
        INSTANCE.get_or_init(|| {
            Runtime::new().expect(
                "hxbridge::Runtime::global: failed to start tokio runtime \
                 (cannot create OS thread)",
            )
        })
    }

    /// Spawn `fut` onto the runtime's worker pool. Returns a
    /// `JoinHandle` that resolves with the future's output when it
    /// completes; dropping the handle does NOT cancel the task
    /// (tokio's documented semantics) — call `JoinHandle::abort` if
    /// the task should stop.
    pub fn spawn<F>(&self, fut: F) -> JoinHandle<F::Output>
    where
        F: Future + Send + 'static,
        F::Output: Send + 'static,
    {
        self.inner.spawn(fut)
    }

    /// Spawn a synchronous closure onto tokio's dedicated blocking
    /// pool. Returns a `JoinHandle<R>` that resolves with the
    /// closure's return value when it completes.
    ///
    /// The blocking pool is sized separately from the regular
    /// worker pool — it can hold many more threads (default 512)
    /// because they spend most of their time parked in syscalls.
    /// This is the right knob for GIO blocking I/O, file-system
    /// calls, and anything else that does long-running synchronous
    /// work without yielding back to the executor.
    ///
    /// See [`crate::blocking::spawn_blocking_with_idle`] for the
    /// FFI-shaped wrapper that R3.2 consumers use.
    pub fn spawn_blocking<F, R>(&self, f: F) -> JoinHandle<R>
    where
        F: FnOnce() -> R + Send + 'static,
        R: Send + 'static,
    {
        self.inner.spawn_blocking(f)
    }

    /// Borrow the tokio `Handle`. Useful for cases where the caller
    /// wants `Handle::clone()` to hand a runtime reference into a
    /// closure or task that outlives the borrowing scope.
    pub fn handle(&self) -> &runtime::Handle {
        self.inner.handle()
    }

    /// Block the calling thread on `fut`. Provided for tests; the
    /// production path always calls [`spawn`](Self::spawn) and never
    /// blocks the calling (main) thread.
    pub fn block_on<F: Future>(&self, fut: F) -> F::Output {
        self.inner.block_on(fut)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};
    use std::sync::Arc;
    use std::time::Duration;

    #[test]
    fn new_runtime_starts_and_runs_a_task() {
        let rt = Runtime::new().expect("runtime starts");
        let result = rt.block_on(async { 1 + 1 });
        assert_eq!(result, 2);
    }

    #[test]
    fn spawn_returns_join_handle_that_resolves() {
        let rt = Runtime::new().expect("runtime starts");
        let handle = rt.spawn(async { 42u32 });
        let result = rt.block_on(handle).expect("join handle resolves");
        assert_eq!(result, 42);
    }

    #[test]
    fn global_runtime_is_a_singleton() {
        // Both calls return the same address — `OnceLock` gives us
        // pointer equality on the contained value.
        let a = Runtime::global() as *const Runtime;
        let b = Runtime::global() as *const Runtime;
        assert_eq!(a, b, "global() returned distinct instances");
    }

    #[test]
    fn global_runtime_actually_runs_tasks() {
        // Independently of the singleton check, prove the global
        // can schedule work. We poll a counter from a spawned task
        // and `block_on` the resulting JoinHandle.
        let counter = Arc::new(AtomicU32::new(0));
        let c = counter.clone();
        let handle = Runtime::global().spawn(async move {
            c.fetch_add(1, Ordering::SeqCst);
        });
        Runtime::global().block_on(handle).expect("task completes");
        assert_eq!(counter.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn time_is_enabled_so_sleep_works() {
        // R3.2+ relies on `tokio::time::interval` for ping
        // keepalive. Smoke-test that `enable_time` actually got
        // wired up.
        let rt = Runtime::new().expect("runtime starts");
        let start = std::time::Instant::now();
        rt.block_on(async {
            tokio::time::sleep(Duration::from_millis(10)).await;
        });
        assert!(
            start.elapsed() >= Duration::from_millis(10),
            "tokio::time::sleep did not honor the requested duration"
        );
    }

    #[test]
    fn spawn_runs_on_worker_thread_not_caller() {
        // The dedicated-thread design: the future MUST execute on a
        // tokio worker thread, never on the thread that called
        // `spawn`. If this ever regressed (e.g. someone swapped in
        // `new_current_thread`), we'd accidentally run async work on
        // the GLib main thread.
        let rt = Runtime::new().expect("runtime starts");
        let caller_tid = std::thread::current().id();
        let handle = rt.spawn(async move {
            // Identify which thread actually polled this future.
            std::thread::current().id()
        });
        let worker_tid = rt.block_on(handle).expect("task completes");
        assert_ne!(
            worker_tid, caller_tid,
            "spawned future ran on the calling thread (multi-thread scheduler not in effect)"
        );
    }
}
