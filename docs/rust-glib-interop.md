# Rust ↔ GLib interop — the GtkHx convention

> Companion to `docs/voice-chat-plan.md` §5 (Phase R3.0) and
> `RUST-ROADMAP.md` §R3.
>
> **Status note (June 2026):** the Phase 8.x references throughout this
> document were written when those phases were upcoming. All of 8.A
> through 8.G have since shipped, so wherever this doc says "eventual"
> or "Phase 8.B will" the actual code already exists in
> `rust/crates/hxvoice-runtime/`. The conventions described below still
> apply; the tense just hasn't been swept.
>
> **R3.1 addendum:** the tokio runtime + tokio→GLib event ferry land
> in this same `hxbridge` crate behind the `runtime` cargo feature.
> See the [tokio runtime](#tokio-runtime-r31) and
> [event ferry](#tokio--glib-event-ferry-r31) sections below for the
> shipped APIs. Phase R3.2 (banner.c port) is the first planned
> consumer that flips the feature on in the meson build.

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

Use cases in Phase 8.B+:

- gstreamer-rs bus watch (`gst::Bus::add_watch`/`add_watch_local`
  wrap this internally).
- webrtcbin signal handlers that need to do follow-up async work
  (e.g. send the ICE candidate over the Hotline wire).

Pinned by the `spawn_local_actually_polls_future` test in
`hxbridge::tests`.

## Tokio runtime (R3.1)

The `hxbridge::runtime` module (Cargo feature `runtime`) ships
the single, shared, dedicated-thread tokio runtime that every R3+
worker port schedules against. Locked-in decisions are encoded in
the module's rustdoc; the short version:

- **One runtime, process-wide.** Lazily started on the first call
  to `Runtime::global()`. `OnceLock` guarantees the singleton.
  Subsequent calls hand back the existing instance — no
  per-connection or per-window runtimes.
- **Dedicated OS thread.** `tokio::runtime::Builder::new_multi_thread()
  .worker_threads(1)`. The GLib main loop owns the calling thread;
  any tokio work needs its own. `worker_threads(1)` is fine for the
  R3 workload (socket I/O, not CPU-bound); bump it when we have
  evidence the single worker is saturated.
- **No `spawn_local` on the runtime side.** Tokio's `spawn` requires
  `Send + 'static` because the multi-thread scheduler can move
  tasks across worker threads. The GLib half of the bridge is where
  `!Send` UI state lives; the tokio half stays `Send`-clean.
- **`enable_io` + `enable_time`.** I/O is on for R3.3 (`hxnet`
  TcpStream); time is on for the ping keepalive port (R3.2+) that
  replaces `network.c::ping_tick` (`g_timeout_add_seconds`).

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

## Tokio → GLib event ferry (R3.1)

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
  Rust-side ref that needs explicit lifetime management — that's
  Phase R4 territory (where `GtkhxSession` becomes a
  `glib::subclass`-derived Rust GObject and Rust ownership is
  first-class).

## Cross-references to the post-R2 conventions

These were established in R2 and apply here too:

- **`unsafe` is scoped to FFI entry points and the GValue plumbing,
  not the safe Rust API.** `hxbridge::lib.rs` follows the R2 shape:
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
  example and `src/proto_helpers.c::hl_htxf_hdr_pack` for the R2 use.
- **C-side hand-declared `extern` blocks** for FFI symbols, not
  cbindgen-generated headers. Drift surfaces as link-time undefined
  symbols. The `gtkhx_bridge_emit_pointer_pair_signal` declaration
  in `src/gtkhx_session.c` follows this pattern.

## Forward pointers

Phases R3.0 (wrapping helpers) and R3.1 (tokio runtime + ferry) are
the foothold. The same rules apply to:

- **Phase 8.B (gstreamer-rs pipeline).** `webrtcbin` is a GObject;
  emit calls on it go through `session_from_ptr_full` shape; bus
  watches use `spawn_local`. The C-side runtime ownership lives in
  `hxvoice-runtime::Runtime` (Rust-owned), not C, so the wrapping
  shape differs slightly — but the re-entrancy rules are identical.
- **Phase R3 proper (`hxnet` Connection actor).** The tokio↔GLib
  bridge uses these wrapping helpers to forward events into the
  C-side `GtkhxSession`. Same lifetime model.
- **Phase R4 (`GtkhxSession` in Rust).** When the session moves to
  `glib::subclass`-derived Rust, the FFI shim layer (`hxbridge`)
  stays — C-side callers continue to use it, just the underlying
  GObject's implementation has moved. The wrapping helpers continue
  to work because they target `glib::Object`, which is the base
  class for both C-defined and Rust-defined subclasses.
- **Phase R5 (UI windows in Rust).** Rust UI code talks to
  GtkhxSession directly without going through hxbridge — it has its
  own typed handle from the glib::subclass derivation. hxbridge's
  reason for existing fades away as the C-side UI shrinks.

## GStreamer runtime floor

Phase 8.0 (R3.0) pulls the `gstreamer-rs` 0.25 family into
`workspace.dependencies` but does not consume the bindings yet. Phase
8.B is when consumers go live, and that's when the runtime version
floor matters:

- gstreamer-rs 0.25 tracks GStreamer **1.26**.
- GNOME 47 Flatpak runtime ships GStreamer 1.24.
- GNOME 48 brings 1.26.

Action item for Phase 8.B (not 8.0): verify `com.nasledov.gtkhx.yml`'s
runtime version, and bump to GNOME 48 or later before consuming the
gstreamer-webrtc bindings. Phase 8.0 itself only requires that the
crate graph builds (which works on any Rust target — no GStreamer
runtime needed at build time).
