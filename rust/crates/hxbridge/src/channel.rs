//! Tokio task → GLib main loop event ferry (Phase R3.1).
//!
//! The R3 architectural shape: worker code runs as tokio tasks on
//! the [`Runtime`](crate::runtime::Runtime), produces typed events,
//! and forwards them to UI code running on
//! `glib::MainContext::default()`. This module is the ferry — a
//! Send-friendly mpmc channel whose receiver is awaited on the
//! GLib executor, calling a user-supplied handler for each item.
//!
//! # Why `async_channel`, not `tokio::sync::mpsc`
//!
//! `tokio::sync::mpsc::Receiver` polls fine on a tokio executor;
//! on the GLib main loop it requires `blocking_recv` (which blocks
//! the UI) or threading through a tokio `LocalSet` (which complicates
//! every consumer). `async_channel::Receiver::recv` is just a future
//! that's polled by whatever executor is currently running it, so
//! `glib::MainContext::default().spawn_local` drains it cleanly with
//! no per-call ceremony. The crate also lives in the workspace
//! transitively via glycin, so the dep already costs us nothing.
//!
//! # Shutdown semantics
//!
//! Both ends are cancellable by drop. When every `Sender<T>` is
//! dropped (the tokio side finishes), `Receiver::recv` returns
//! `Err(RecvError)`; the [`forward_to_main`] loop sees the error
//! and the future resolves. The GLib executor reclaims the task at
//! the next iteration. There is no explicit "stop" message.
//!
//! Conversely, if the GLib side wants to stop draining (e.g. window
//! closes), dropping the [`MainForwarder`] handle returned by
//! [`forward_to_main`] aborts the spawn_local future at its next
//! poll. The forwarder owns the `Receiver<T>` inside the
//! spawn_local future's closure, so aborting drops the receiver
//! and the channel closes from the receive end. Surviving senders
//! see `Err(SendError(_))` on subsequent `send` /
//! `send_blocking` — the typical call-site pattern is to drop
//! senders alongside the forwarder; otherwise wrap `send` results
//! in `let _ = ...;` to swallow the harmless error.
//! See [`MainForwarder`] for the full rationale and the
//! `dropping_forwarder_stops_delivery` test that pins this.

use async_channel::{bounded, Receiver, Sender};
use glib::MainContext;
use std::future::Future;

/// Construct a bounded channel pair. `capacity` is the number of
/// in-flight items the channel will buffer before `Sender::send`
/// `.await`s on receiver progress.
///
/// Backpressure recipe: a worker producing events faster than the
/// GLib main loop can drain them will park inside `send().await`
/// once the buffer fills. This is the intended behaviour — it
/// prevents a slow UI from drowning in stale events. Pick a
/// capacity large enough to absorb a typical event burst but small
/// enough that backpressure kicks in before memory pressure does.
/// 64 is a reasonable default for chat-style traffic; 256 for the
/// initial USER_LIST flood on join.
pub fn channel<T: 'static>(capacity: usize) -> (Sender<T>, Receiver<T>) {
    bounded(capacity)
}

/// Handle for a forward task spawned on the GLib main context.
/// Drop the handle to stop the forwarder (the spawn_local future
/// resolves at its next poll).
///
/// `glib::JoinHandle`'s own `Drop` *detaches* the future rather
/// than aborting it (matches tokio's semantics: dropping a
/// `JoinHandle` leaves the task running). Production callers want
/// abort-on-drop here so a closed window stops draining stale
/// events into a no-longer-relevant handler; we implement that
/// explicitly via the `MainForwarder` Drop impl below.
///
/// # Side effect: closes the channel from the receiver end
///
/// The forwarder owns the [`Receiver<T>`] inside the spawn_local
/// future's closure. Aborting that future drops the closure, which
/// drops the receiver, which closes the channel. Senders that
/// outlive the forwarder will see `Err(SendError(_))` on subsequent
/// `send` / `send_blocking` — there's no receiver left to deliver
/// to. Production callers should drop their senders alongside (or
/// before) the forwarder; otherwise wrap `send` results in
/// `let _ = ...;` to swallow the harmless error.
pub struct MainForwarder {
    join: Option<glib::JoinHandle<()>>,
}

impl MainForwarder {
    /// Abort the forwarder immediately. The spawn_local future
    /// owns the `Receiver<T>`, so aborting it drops the receiver
    /// and the channel closes from the receive end — surviving
    /// senders' subsequent `send` / `send_blocking` calls return
    /// `Err(SendError(_))`. Identical to dropping the handle;
    /// provided as an explicit form for call sites where
    /// readability matters. See the [`MainForwarder`] type docs
    /// for the full shutdown story.
    pub fn abort(mut self) {
        if let Some(j) = self.join.take() {
            j.abort();
        }
    }
}

impl Drop for MainForwarder {
    fn drop(&mut self) {
        if let Some(j) = self.join.take() {
            j.abort();
        }
    }
}

/// Spawn a future on `ctx` that pulls items out of `rx` and invokes
/// `handler(item)` for each. Returns a [`MainForwarder`] handle
/// that cancels the future on drop.
///
/// Pass `glib::MainContext::default()` in production code; the
/// parameter form exists so unit tests can use a private context
/// (sharing `default()` across parallel tests trips glib's
/// ThreadGuard).
///
/// Must be called from the same OS thread that iterates `ctx`.
/// `spawn_local` enforces this — it panics if called off-thread.
///
/// The handler runs on `ctx`'s owning thread and may freely call
/// GTK, emit GtkhxSession signals, etc. The handler is `FnMut` so
/// it can keep state across calls; it must not block (anything
/// that wants to await goes back through tokio, not the handler).
///
/// # Lifetime
///
/// Continues forwarding until either:
/// - every `Sender<T>` in the corresponding pair has been dropped
///   (`recv` returns `Err`, the loop exits, the future resolves);
/// - the returned [`MainForwarder`] is dropped (glib aborts the
///   future at its next poll point).
pub fn forward_to_main<T, F>(
    ctx: &MainContext,
    rx: Receiver<T>,
    mut handler: F,
) -> MainForwarder
where
    T: 'static,
    F: FnMut(T) + 'static,
{
    let join = ctx.spawn_local(async move {
        while let Ok(item) = rx.recv().await {
            handler(item);
        }
    });
    MainForwarder { join: Some(join) }
}

/// Spawn a future on the main context, returning the join handle
/// directly. Thin wrapper over `MainContext::default().spawn_local`
/// that the crate's own consumers (e.g. [`forward_to_main`]) use;
/// production callers should write the canonical form themselves
/// per `docs/rust-glib-interop.md`'s "no `hxbridge::spawn_local`
/// wrapper" rule. This is `pub(crate)` for that reason.
#[allow(dead_code)] // Reserved for follow-up R3.x crate-internal use.
pub(crate) fn spawn_local<F>(fut: F) -> glib::JoinHandle<F::Output>
where
    F: Future + 'static,
{
    MainContext::default().spawn_local(fut)
}

#[cfg(test)]
mod tests {
    // Tests run in parallel by default; sharing `MainContext::default()`
    // across parallel test threads trips glib's per-thread ThreadGuard
    // checks. Each test builds a private `MainContext::new()` and
    // installs it as the thread-default for the test's duration. The
    // forward_to_main API takes a context parameter precisely so this
    // is straightforward.

    use super::*;
    use crate::runtime::Runtime;
    use glib::MainContext;
    use std::cell::RefCell;
    use std::rc::Rc;
    use std::time::Duration;

    /// RAII guard that pushes `ctx` as the thread-default for the
    /// lifetime of the guard. Glib 0.22 exposes a scoped
    /// `with_thread_default(closure)` API only; for tests that have
    /// to pump-loop alongside the future we built this around the
    /// raw ffi push/pop pair.
    struct ThreadDefaultGuard {
        ctx_ptr: *mut glib::ffi::GMainContext,
    }

    impl Drop for ThreadDefaultGuard {
        fn drop(&mut self) {
            // SAFETY: paired with the push_thread_default in
            // `private_ctx`. The context pointer is the one we
            // pushed; pop_thread_default expects exactly that
            // pointer in LIFO order, which holds inside a single
            // test (we don't nest).
            unsafe {
                glib::ffi::g_main_context_pop_thread_default(self.ctx_ptr);
            }
        }
    }

    /// Build a fresh private context and install it as the
    /// thread-default for the test's lifetime. The
    /// [`ThreadDefaultGuard`] pops on drop. Calling forward_to_main
    /// with the same ctx ties the spawn_local future and any
    /// JoinHandles it produces to this thread, so parallel tests on
    /// other threads don't trip glib's per-object ThreadGuard.
    fn private_ctx() -> (MainContext, ThreadDefaultGuard) {
        use glib::translate::ToGlibPtr;
        let ctx = MainContext::new();
        let ctx_ptr: *mut glib::ffi::GMainContext = ctx.to_glib_none().0;
        // SAFETY: ctx outlives the guard (we return it by value).
        // push/pop is the documented way to layer a non-default
        // main context onto the current thread.
        unsafe {
            glib::ffi::g_main_context_push_thread_default(ctx_ptr);
        }
        (ctx, ThreadDefaultGuard { ctx_ptr })
    }

    /// Pump `ctx` until `pred()` is true or `deadline` elapses.
    /// Non-blocking iterations plus a 1 ms sleep so the loop
    /// doesn't burn through its iteration budget faster than the
    /// tokio side can park/unpark — a real concern when other
    /// parallel tests are loading the CPU (the `many_concurrent_*`
    /// sibling tests can starve the tokio worker for several ms
    /// before it polls a backpressured send).
    fn pump_until(
        ctx: &MainContext,
        pred: impl Fn() -> bool,
        deadline: Duration,
    ) -> bool {
        let start = std::time::Instant::now();
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
    fn forward_delivers_items_in_order() {
        let (ctx, _owner) = private_ctx();

        let (tx, rx) = channel::<u32>(8);
        let seen = Rc::new(RefCell::new(Vec::<u32>::new()));
        let s = seen.clone();
        let _forwarder = forward_to_main(&ctx, rx, move |v| {
            s.borrow_mut().push(v);
        });

        // Push from a tokio task — exercises the real cross-thread
        // path (tokio worker thread sends; GLib main drains).
        let tx_clone = tx.clone();
        let handle = Runtime::global().spawn(async move {
            for v in 1..=4u32 {
                tx_clone.send(v).await.expect("send succeeds");
            }
        });
        Runtime::global().block_on(handle).expect("sender completes");
        drop(tx); // closes the channel so the forwarder loop ends

        let drained = pump_until(
            &ctx,
            || seen.borrow().len() == 4,
            Duration::from_secs(5),
        );
        assert!(drained, "main loop drained all 4 items");
        assert_eq!(*seen.borrow(), vec![1, 2, 3, 4], "FIFO order preserved");
    }

    #[test]
    fn forward_resolves_when_all_senders_drop() {
        let (ctx, _owner) = private_ctx();

        let (tx, rx) = channel::<()>(1);
        let counter = Rc::new(RefCell::new(0u32));
        let c = counter.clone();
        let _forwarder = forward_to_main(&ctx, rx, move |()| {
            *c.borrow_mut() += 1;
        });

        // No sender activity, just immediate close.
        drop(tx);

        for _ in 0..32 {
            ctx.iteration(false);
        }
        assert_eq!(*counter.borrow(), 0, "no items were delivered");
    }

    #[test]
    fn handler_runs_on_main_thread() {
        let (ctx, _owner) = private_ctx();

        let (tx, rx) = channel::<()>(1);
        let main_tid = std::thread::current().id();
        let observed = Rc::new(RefCell::new(None::<std::thread::ThreadId>));
        let o = observed.clone();
        let _forwarder = forward_to_main(&ctx, rx, move |()| {
            *o.borrow_mut() = Some(std::thread::current().id());
        });

        let tx_clone = tx.clone();
        let handle = Runtime::global().spawn(async move {
            tx_clone.send(()).await.expect("send succeeds");
        });
        Runtime::global().block_on(handle).expect("sender completes");
        drop(tx);

        let _ = pump_until(
            &ctx,
            || observed.borrow().is_some(),
            Duration::from_secs(5),
        );
        assert_eq!(
            observed.borrow().expect("handler ran"),
            main_tid,
            "handler must run on the main (GLib) thread, not a tokio worker"
        );
    }

    #[test]
    fn dropping_forwarder_stops_delivery() {
        // Aborting the spawn_local future drops the closure and the
        // receiver it owns; that closes the channel from the receiver
        // end. Two observable consequences we pin here:
        //
        //  1. The handler never sees items posted after the abort.
        //  2. Subsequent `send_blocking` on a surviving sender fails
        //     with SendError (no receiver left). This is the
        //     correct, deliberate behaviour — call sites should drop
        //     senders alongside the forwarder.
        let (ctx, _owner) = private_ctx();

        let (tx, rx) = channel::<u32>(4);
        let seen = Rc::new(RefCell::new(Vec::<u32>::new()));
        let s = seen.clone();
        let forwarder = forward_to_main(&ctx, rx, move |v| {
            s.borrow_mut().push(v);
        });

        tx.send_blocking(1).expect("send 1 succeeds");
        let _ = pump_until(
            &ctx,
            || seen.borrow().len() == 1,
            Duration::from_secs(5),
        );
        assert_eq!(*seen.borrow(), vec![1]);

        drop(forwarder);

        // Pump a few iterations so any racing in-flight item would
        // have a chance to arrive at the (now-aborted) handler.
        for _ in 0..32 {
            ctx.iteration(false);
        }
        assert_eq!(
            *seen.borrow(),
            vec![1],
            "handler must not observe new items after forwarder drop"
        );

        // Confirm the channel is closed on the receive side: a
        // surviving sender now sees SendError.
        assert!(
            tx.send_blocking(2).is_err(),
            "channel must close on receiver drop after forwarder abort"
        );
        assert_eq!(*seen.borrow(), vec![1], "still no new items");
    }

    #[test]
    fn handler_dropping_sender_closes_loop_cleanly() {
        // Re-entrancy regression: the handler holds the only
        // surviving Sender, drops it on the first item, and the
        // forwarder's recv() returns Err on the next poll. No panic,
        // no second invocation, no hang.
        let (ctx, _owner) = private_ctx();

        let (tx, rx) = channel::<u32>(4);
        let calls = Rc::new(RefCell::new(0u32));
        let c = calls.clone();
        let tx_cell: Rc<RefCell<Option<Sender<u32>>>> =
            Rc::new(RefCell::new(Some(tx.clone())));
        let tc = tx_cell.clone();
        let _forwarder = forward_to_main(&ctx, rx, move |_v| {
            *c.borrow_mut() += 1;
            // Drop the handler's sender. Combined with the test
            // dropping its own `tx` below, that's the last Sender,
            // so the receiver should close.
            tc.borrow_mut().take();
        });

        tx.send_blocking(99).expect("send succeeds");
        drop(tx);

        for _ in 0..64 {
            ctx.iteration(false);
        }
        assert_eq!(*calls.borrow(), 1, "handler ran exactly once");
        assert!(
            tx_cell.borrow().is_none(),
            "handler observed and dropped its sender"
        );
    }

    #[test]
    fn channel_applies_backpressure_at_capacity() {
        // Capacity-1 channel; a second blocking send must park
        // until the receiver drains item 1. We use a tokio task to
        // do the parking and a short timeout to confirm it actually
        // makes progress only after the GLib side pumps.
        let (ctx, _owner) = private_ctx();

        let (tx, rx) = channel::<u32>(1);
        let seen = Rc::new(RefCell::new(Vec::<u32>::new()));
        let s = seen.clone();
        let _forwarder = forward_to_main(&ctx, rx, move |v| {
            s.borrow_mut().push(v);
        });

        let tx_clone = tx.clone();
        let handle = Runtime::global().spawn(async move {
            tx_clone.send(1).await.expect("first send succeeds");
            tokio::time::timeout(Duration::from_secs(2), tx_clone.send(2))
                .await
                .expect("send did not deadlock")
                .expect("second send succeeds");
        });

        let _ = pump_until(
            &ctx,
            || seen.borrow().len() == 2,
            Duration::from_secs(10),
        );
        Runtime::global().block_on(handle).expect("sender completes");
        drop(tx);
        assert_eq!(*seen.borrow(), vec![1, 2]);
    }
}
