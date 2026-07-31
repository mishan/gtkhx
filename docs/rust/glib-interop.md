# Rust ↔ GLib interop — the GtkHx convention

> Companion to `docs/voice.md` §5 and `ROADMAP.md` (this
> directory).
>
> The tokio runtime and the tokio→GLib event ferry live in this same
> `hxbridge` crate, behind its `runtime` Cargo feature — which the
> `gtkhx-ffi` façade turns on for the binary's build. See the
> [tokio runtime](#tokio-runtime) and
> [event ferry](#tokio--glib-event-ferry) sections below.

## The problem

GtkHx's GObjects (the `GtkhxSession` singleton, individual `PanelFrame`s,
the `webrtcbin` instances from gstreamer-rs in `hxvoice-runtime`) are
constructed and owned by C. Rust code needs to drive them — read
properties, emit signals, attach handlers — without confusing GLib about
who holds which reference.

GLib's ref-counting rules are simple in isolation: every `g_object_ref`
must be paired with exactly one `g_object_unref`. They get harder when:

1. Both languages can call `g_object_unref`.
2. Signal dispatch is synchronous and re-entrant — a handler can
   `g_object_unref` the object the outer code is still working with.
3. The Rust handle's lifetime is implicit (RAII), while the C
   reference's lifetime is explicit (manual `g_object_ref` /
   `g_object_unref` calls).

This document is the GtkHx-wide convention for resolving (1)–(3). The
`hxbridge` crate (`rust/crates/hxbridge/`) is its reference
implementation; every future Rust↔GLib boundary in the project
inherits the same rules.

## The two wrapping forms

### `session_from_ptr` — borrow form (`from_glib_borrow`)

Use for: **read-only access** that does not cross a re-entrancy
boundary.

```rust
let handle = unsafe { hxbridge::session_from_ptr(ptr) };
let title: String = handle.property::<String>("title");
// `handle` drops here; refcount unchanged across the call.
```

Properties this satisfies:

- The Rust handle does NOT add a ref to the underlying GObject.
  GLib's terminology: the handle "borrows" the existing C-side ref.
- The C-side caller is responsible for keeping at least one ref alive
  for the entire duration of the borrow.
- Cheap: zero atomic operations on the wrapping call.

Properties this does NOT satisfy:

- **Re-entrancy safety.** If the handle calls any code path that
  ultimately runs a registered signal handler, and that handler
  calls `g_object_unref` on the object — possibly because the
  handler closed the last user-visible reference — the borrowed
  handle becomes dangling for the rest of the outer function.

Rule of thumb: if the handle ever calls `emit_by_name` (or `notify`,
or `set_property` on a property with a custom setter, or anything else
that runs user code), do NOT use the borrow form. Use the next one.

### `session_from_ptr_full` — full form (`from_glib_none`)

Use for: **signal emit** and any other call path that runs re-entrant C
or Rust code.

```rust
let handle = unsafe { hxbridge::session_from_ptr_full(ptr) };
handle.emit_by_name_with_values("task-update", &[v0, v1]);
// `handle` drops here; the ref it took is released.
```

Properties this satisfies:

- The Rust handle adds one ref on wrap, drops it on Rust drop.
- The session stays alive across every code path the Rust handle
  reaches, including all synchronous re-entrant emits.
- Pinned by the `full_form_survives_handler_unref_during_emit`
  test in `hxbridge::tests`, which sets up the exact scenario above
  (extra C-side ref, handler reads the live refcount via a fresh
  borrow, then drops the extra ref) and asserts the handler observed
  the shim's wrap-ref. A borrow-form regression fails that assertion
  with a clear message naming the function.
  `refcount_stable_across_many_emits` (no leak across 1000 emits)
  and `reentrant_emit_is_safe` (nested emits unwind cleanly) cover
  adjacent but distinct properties — neither would catch the unref
  regression on its own.

Cost:

- One `g_object_ref` (atomic increment) on wrap, one `g_object_unref`
  (atomic decrement + maybe-finalize check) on drop.
- That's two atomic ops per call. Negligible compared to:
  - The hundreds of atomic ops a single signal dispatch already does
    inside GLib (handler-list walk, return-value accumulator, etc.).
  - The use-after-free crash the borrow form would suffer if a
    handler dropped the last ref.

**TL;DR: borrow for reads, full for emits.**

## `glib::MainContext::default().spawn_local`

Async tasks that run on the GLib main loop go through this entry
point. There is intentionally **no `hxbridge::spawn_local` wrapper**:
glib-rs's canonical spelling is the documented form, and wrapping it
would just hide the canonical name without adding any policy or type
safety.

```rust
use glib::MainContext;
MainContext::default().spawn_local(async move {
    // … any future returning ()
});
```

Use cases:

- gstreamer-rs bus watch (`gst::Bus::add_watch`/`add_watch_local`
  wrap this internally).
- webrtcbin signal handlers that need to do follow-up async work
  (e.g. send the ICE candidate over the Hotline wire).

Pinned by the `spawn_local_actually_polls_future` test in
`hxbridge::tests`.

## Tokio runtime

The `hxbridge::runtime` module (Cargo feature `runtime`) ships
the single, shared, dedicated-thread tokio runtime that every Rust
worker schedules against. Locked-in decisions are encoded in
the module's rustdoc; the short version:

- **One runtime, process-wide.** Lazily started on the first call
  to `Runtime::global()`. `OnceLock` guarantees the singleton.
  Subsequent calls hand back the existing instance — no
  per-connection or per-window runtimes.
- **Dedicated OS thread.** `tokio::runtime::Builder::new_multi_thread()
  .worker_threads(1)`. The GLib main loop owns the calling thread;
  any tokio work needs its own. `worker_threads(1)` is fine for the
  workload (socket I/O, not CPU-bound); bump it when we have
  evidence the single worker is saturated.
- **No `spawn_local` on the runtime side.** Tokio's `spawn` requires
  `Send + 'static` because the multi-thread scheduler can move
  tasks across worker threads. The GLib half of the bridge is where
  `!Send` UI state lives; the tokio half stays `Send`-clean.
- **`enable_io` + `enable_time`.** I/O for `hxnet`'s sockets; time so
  `tokio::time` (sleeps, timeouts) works under the runtime. The ping
  keepalive deliberately stayed a GLib `g_timeout_add_seconds` in
  `network.c` — it drives C-side connection state, so a tokio
  `Interval` would buy nothing. See the timer audit in
  `docs/rust/ROADMAP.md`.

```rust
use hxbridge::runtime::Runtime;

let handle = Runtime::global().spawn(async move {
    do_some_io().await
});
```

Pinned by:

- `runtime::tests::new_runtime_starts_and_runs_a_task`
- `runtime::tests::spawn_returns_join_handle_that_resolves`
- `runtime::tests::global_runtime_is_a_singleton`
- `runtime::tests::global_runtime_actually_runs_tasks`
- `runtime::tests::time_is_enabled_so_sleep_works`
- `runtime::tests::spawn_runs_on_worker_thread_not_caller`

## Tokio → GLib event ferry

The `hxbridge::channel` module pairs a `tokio::task` producing
events with a GLib-main-thread handler consuming them. Two pieces:

```rust
use glib::MainContext;
use hxbridge::channel::{channel, forward_to_main};
use hxbridge::runtime::Runtime;

let (tx, rx) = channel::<HotlineEvent>(64);

// On the main thread, install the ferry. The handler runs on the
// main thread; it can freely touch GTK widgets, emit GtkhxSession
// signals, etc. Holding the returned `MainForwarder` keeps the
// ferry running; dropping it aborts (and closes the channel from
// the receiver side, so producers see SendError on subsequent
// sends — typical pattern is to drop senders + forwarder together).
let _forwarder = forward_to_main(&MainContext::default(), rx, |evt| {
    dispatch_to_ui(evt);
});

// On the tokio side, send events. `send` returns a future; awaiting
// it parks under backpressure once the channel buffer fills.
Runtime::global().spawn(async move {
    while let Some(evt) = read_next_event().await {
        if tx.send(evt).await.is_err() {
            break; // forwarder went away
        }
    }
});
```

### Why `async_channel` instead of `tokio::sync::mpsc`

`tokio::sync::mpsc::Receiver` only polls cleanly inside a tokio
executor; on the GLib main loop it requires `blocking_recv` (which
blocks the UI) or a tokio `LocalSet`. `async_channel::Receiver` is
just a future that runs under whatever executor polls it — the GLib
executor drains it with no extra ceremony. `async-channel` also
arrives in the workspace transitively via glycin, so the dep
doesn't grow.

### Backpressure and capacity

`channel(capacity)` builds an `async_channel::bounded` pair. When
the buffer is full, `Sender::send().await` parks until the GLib
main loop drains an item. Pick a capacity that absorbs a typical
event burst but kicks in before memory pressure does — 64 is a
reasonable default for chat-style traffic; 256 for the initial
USER_LIST flood on join.

### `forward_to_main(ctx, rx, handler)` takes a context

The context parameter is what lets unit tests use a private
`MainContext::new()` instead of polluting `MainContext::default()`
across parallel test runs. Production callers pass
`MainContext::default()` and the panel of test cases in
`channel::tests` exercises the cross-thread, backpressure,
re-entrancy, and shutdown paths against a private context per test.

Pinned by:

- `channel::tests::forward_delivers_items_in_order`
- `channel::tests::forward_resolves_when_all_senders_drop`
- `channel::tests::handler_runs_on_main_thread`
- `channel::tests::dropping_forwarder_stops_delivery`
- `channel::tests::handler_dropping_sender_closes_loop_cleanly`
- `channel::tests::channel_applies_backpressure_at_capacity`

## What's NOT allowed

These patterns will be caught in review:

- **`from_glib_full` on a borrowed pointer.** `from_glib_full` takes
  ownership of a ref that was already added for you (the GLib idiom
  for "function returns a new ref"). Calling it on a pointer where
  the caller still holds the ref leaks a ref permanently. Use
  `from_glib_none` (the "full" form in our convention — confusingly
  named, but the glib-rs name predates this convention).
- **`from_glib_borrow` across an emit boundary.** This is the
  re-entrancy footgun. The
  `hxbridge::tests::full_form_survives_handler_unref_during_emit`
  test is the regression check — it exercises a handler that drops
  an external ref mid-emit and verifies the shim's own ref keeps
  the session live for the remainder of the call.
- **Storing the wrapped handle past the call site.** Both wrapping
  forms are intended for local use within a single function. Storing
  the handle in a struct field or static creates a long-lived
  Rust-side ref that needs explicit lifetime management. Where Rust
  genuinely owns the object, use the Rust-side type instead: the
  `glib::subclass`-derived `GtkhxSession` in `gtkhx-core` gives you a
  typed handle with first-class Rust ownership, no shim required.

## Cross-references to the wire-protocol-crate conventions

These were established when the protocol crate landed and apply here
too:

- **`unsafe` is scoped to FFI entry points and the GValue plumbing,
  not the safe Rust API.** `hxbridge::lib.rs` follows that shape:
  the wrapping helpers (`session_from_ptr`, `session_from_ptr_full`)
  are `unsafe fn` because they take raw pointers from C; the
  `emit_pointer_pair_signal` entry is `unsafe fn` for the same
  reason; and the `pointer_value` helper has a narrow `unsafe { … }`
  block around `g_value_set_pointer` (the safe API doesn't expose a
  way to construct a `G_TYPE_POINTER` `Value` because the pointee
  type is unknown to the borrow checker — that's intrinsic to the
  C ABI, not a hxbridge choice). The crate has no "wide" unsafe; the
  unsafe blocks are localized to single FFI calls.
- **`g_error` not `g_assert`** on the C side. `g_assert` is gated on
  `G_DISABLE_ASSERT`, which downstream packagers can set without the
  project realising it; safety-critical invariants must use
  `g_error` (always fatal) or `g_critical` + graceful skip. The
  convention applies to any C-side check on a path that calls into
  Rust. See `src/rcv.c::rcv_task_login` for the original convention
  example and `src/proto_helpers.c::hl_htxf_hdr_pack` for a later one.
- **C-side hand-declared `extern` blocks** for FFI symbols, not
  cbindgen-generated headers. Drift surfaces as link-time undefined
  symbols. `src/hxnet_bridge.c`'s `extern` declarations of the
  `hxnet_connection_*` entry points follow this pattern.

## GStreamer runtime floor

The workspace pins the `gstreamer-rs` 0.24 family in
`workspace.dependencies`. (It originally landed at 0.25 alongside the
gtk-rs 0.22 family; both were downgraded one minor when the rustc 1.92
MSRV the 0.22 / 0.25 line carried turned out to be incompatible with
Debian trixie's stock rustc 1.85 — see `rust-toolchain.toml` and the
workspace `Cargo.toml` comments for the full rationale.)

The runtime version floor:

- gstreamer-rs 0.24 has a build-time floor of GStreamer 1.14 (per
  gstreamer-webrtc-sys 0.24's `system-deps` probe) but project meson
  pins it harder at >= 1.20, which is where webrtcbin stabilised.
- Debian trixie ships GStreamer **1.24** — the natural pair for
  gstreamer-rs 0.24.
- GNOME 49 Flatpak runtime ships GStreamer **1.26** — also fine
  against 0.24 bindings; the bindings just don't expose any 1.26-only
  API surface to our code, and we use none of it.

No GNOME-runtime bump is required for voice with this dep tree.
The earlier "must bump to GNOME 48 for GStreamer 1.26" action item was
specific to the gstreamer-rs 0.25 family and is no longer load-bearing.
If a future voice or media feature needs a 1.26-binding-level API
(versus 1.26-runtime behaviour) we'd need to step the whole gtk-rs /
gstreamer-rs / rustc trio back up — but nothing currently shipped does.
