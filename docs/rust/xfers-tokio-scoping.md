# `xfers.c` transfer worker → tokio scoping (Phase R3, work item 4)

> **Status: SHIPPED (June 2026), phases X1–X5.** The transfer worker runs
> on the tokio blocking pool with cooperative cancellation
> (`hxnet_htxf_abort_*`); the last `pthread_create` and `gtkthreads.c` are
> gone. Cancellation is covered by the unit test
> `tests/unit/test_htxf_cancel.c`; Tier 3 (`test_real_htxf_connect`)
> drives the transfer matrix on the happy path. The text below is the
> original (pre-implementation) scoping, retained for rationale — it
> describes the starting state in the present tense.

Scoping for converting the file-transfer worker off its `pthread` and onto
the tokio runtime. This was the **R3 exit-criterion item**: `xfers.c` held
the last `pthread_create` in the tree and the last callers of
`gtkthreads.c`.

## TL;DR

- **Converting `xfers.c` retires both the last `pthread_create` and
  `gtkthreads.c`.** `gtkhx_post_to_main` has exactly two callers, both in
  `xfers.c` (`post_file_update`, `post_xfer_cleanup`) — `preview.c` does
  *not* use it (the ROADMAP's "preview.c keeps it until R5" note is stale).
  So this one conversion satisfies "no `pthread_create` outside vendored
  xtext" and deletes `gtkthreads.c` outright.
- **This is not an async rewrite.** The byte transport already moved to
  Rust (`htxf_io_read` / `htxf_io_write` over `hxnet_htxf_open`, AEAD +
  rustls inside). The job is to lift the existing four C worker functions
  onto **tokio's blocking pool** via `gtkhx_bridge_spawn_blocking_with_idle`
  — exactly the pattern `banner.c` already uses.
- **The one genuinely new problem is cancellation.** Today a stalled
  transfer is killed with `pthread_cancel`. Blocking-pool tasks can't be
  force-cancelled, so cancel becomes cooperative: a flag plus a
  socket-abort to unblock a parked read/write, with the existing read
  timeout as backstop. This is the bulk of the work.

## Current shape

- `xfer_ready_write` (`xfers.c` ~L1654) `pthread_create`s one of four
  `void *(*)(void *)` workers: `get_thread` / `put_thread` (solo file) and
  `folder_get_thread` / `folder_put_thread` (folder stream). This is the
  only `pthread_create` in `src/`.
- Each worker opens the local file fd(s), runs a **blocking** copy loop
  calling `htxf_io_read` / `htxf_io_write` (sync Rust, AEAD/TLS-aware),
  handles folder framing + hfs/macres resource forks + `uniquify_path` +
  preview triggering, posts progress via `post_file_update` →
  `gtkhx_post_to_main(fu_dispatch)`, and at exit posts `post_xfer_cleanup`
  → `gtkhx_post_to_main(cleanup_dispatch)`.
- Lifecycle: refcounted `htxf_conn` (xfers[] ref + worker ref + one ref per
  queued idle). Cancellation = `htxf->canceled` flag **plus**
  `pthread_cancel(htxf->tid)` (`xfer_delete`, `xfers_delete_all`).
- `xfer_ready_write` masks SIGTSTP/SIGCONT around the spawn so the worker
  thread doesn't catch terminal stop signals.

## Precedent: `banner.c`

`banner.c` already runs its HTXF fetch on the blocking pool via
`gtkhx_bridge_spawn_blocking_with_idle(worker_run, completion_run, ctx)`:
the shim runs `worker_run` on a blocking-pool thread, then posts
`completion_run` on the GLib main loop when it returns (panic-safe —
`catch_unwind` → abort). `xfers.c` follows the same shape. The deltas vs.
banner: (a) four workers, not one; (b) long-lived with **streaming**
progress (banner is one-shot); (c) a real **cancellation** requirement
(banner just bumps a generation counter).

## What moves vs. what stays

**Stays C, logic unchanged** — the four worker bodies (copy loop, folder
framing, hfs/macres resource forks, `uniquify_path`, preview triggering).
They run on a blocking-pool thread instead of a `pthread`. No Rust rewrite.

**Already Rust, untouched** — the byte transport (`htxf_io.c` →
`hxnet_htxf_*`).

**Changes:**

- `xfer_ready_write`: `pthread_create` → `gtkhx_bridge_spawn_blocking_with_idle`;
  delete the signal-mask dance and the `htxf->tid` field.
- Terminal cleanup: fold `cleanup_dispatch` logic into the bridge
  **completion callback** (already runs on the main loop — no manual post).
- Mid-transfer progress: move `post_file_update` off `gtkhx_post_to_main`
  (see Design §1).
- Cancellation: `pthread_cancel` → cooperative (see Design §2).

**Deletes when done:** `gtkthreads.c` + `gtkthreads.h` and their includes;
the `pthread_t tid` field; the signal-mask helpers (`ignore_signals` /
`unignore_signals`) if unused elsewhere.

## Design

### 1. Mid-transfer progress delivery (the `gtkthreads.c` retirement)

`spawn_blocking_with_idle` marshals only one *terminal* completion, but the
workers post progress many times mid-loop. Two ways to deliver those to the
main loop without `gtkhx_post_to_main`:

- **(a) Per-transfer event channel** *(matches the roadmap's "progress
  flows over a per-transfer channel" wording).* The worker sends
  `Progress{done,total}` / `Done` on an mpsc; a `GSource` (or a bridge
  drain) consumes it on the main loop and emits `file-update`. Ordered,
  backpressure-aware, and cancellation falls out of dropping the receiver.
  More plumbing.
- **(b) A bridge post-idle helper.** Add `gtkhx_bridge_post_to_main(fn,
  data)` to `hxbridge` — the same one-liner `gtkthreads.c` provides today,
  relocated onto the runtime's captured main context. Smallest diff; keeps
  the existing `fu_dispatch` shape; still deletes `gtkthreads.c`.

**Recommendation:** (b) for the first cut — smallest change that hits the
exit criterion — with (a) as the cleaner end-state if the transfer window
later wants structured events.

### 2. Cancellation (the real work)

Replace `pthread_cancel` with cooperative cancellation:

- Keep `htxf->canceled`; the worker loops already check it.
- **Unblock a parked worker:** on cancel, shut down / abort the hxnet htxf
  channel's socket so an in-flight blocking `htxf_io_read` / `htxf_io_write`
  returns an error promptly. This needs a **thread-safe hxnet FFI to abort
  the channel** from the main thread while the worker owns it — a cancel
  token inside the Rust htxf handle that read/write observe, or a
  half-close. (Confirm the safest shape during X1.)
- **Backstop:** `hxnet_htxf_set_read_timeout` already exists, so even
  without an abort the loop wakes periodically and notices `canceled`.
- Net: prompt cancel via socket-abort, guaranteed-eventually via the
  timeout. Slightly less immediate than `pthread_cancel`'s hard kill, but
  cancellation now only happens at well-defined loop boundaries — no torn
  state, which is *safer*, not just different.

### 3. Refcount / shutdown

- Map the worker ref onto the blocking task: take it before
  `spawn_blocking`, drop it in the completion callback (mirrors today's
  `cleanup_dispatch` hand-off).
- App shutdown (`xfers_delete_all`): set `canceled` + abort the sockets;
  blocking tasks unwind. At process exit, best-effort is acceptable — same
  property `banner.c` has today.

## Phasing

Ordered so the hardest, riskiest piece lands first and independently:

- **X1 — cooperative-cancel foundation.** Add the hxnet channel-abort FFI;
  make every worker loop tolerate a mid-flight read/write error from an
  abort and check `canceled` at each boundary. Still on `pthread` +
  `pthread_cancel` — this just makes cooperative cancel *sufficient* on its
  own. De-risks the core change in isolation.
- **X2 — progress/cleanup off `gtkhx_post_to_main`.** Add the delivery
  mechanism (Design §1) and switch `post_file_update` / cleanup to it.
  Still on `pthread`; the post path is migrated but `pthread_create`
  remains.
- **X3 — flip the spawn.** `pthread_create` →
  `gtkhx_bridge_spawn_blocking_with_idle`; move cleanup into the completion
  callback; delete the signal dance, `htxf->tid`, and `pthread_cancel`
  calls. The last `pthread_create` is now gone.
- **X4 — delete `gtkthreads.c`.** Remove the file, its header, and the
  include sites; update CLAUDE.md / ROADMAP.
- **X5 — Tier 3 validation** (below).

## Testing

- **Existing Tier 3 transfer tests** (file get/put, folder get/put, plus
  the TLS variants) against mhxd / Janus are the regression guard — they
  drive `xfer_ready_write` + the worker loops + progress + completion
  end-to-end.
- **New cancel-mid-transfer cases:** start a large transfer, cancel both
  server-side and client-side, assert prompt teardown and no leak / UAF
  under ASan. This is the behaviour the conversion most changes.
- **Concurrency:** multiple simultaneous transfers (blocking-pool
  fan-out); assert per-transfer progress ordering.
- **Shutdown-with-active-transfer** (`xfers_delete_all`) under ASan.

## Risks / open questions

1. **Cancel mechanism / latency** — the central question. Needs the hxnet
   channel-abort FFI; confirm it can safely interrupt a blocking read/write
   owned by another thread.
2. **Blocking-pool sizing** — N concurrent transfers = N blocking threads;
   tokio's default `max_blocking_threads` is 512. Fine for a desktop
   client, but note it (and that it's tunable).
3. **Progress-delivery choice** (channel vs. bridge post-idle) — gates how
   much new plumbing X2 carries.
4. **Worker-side GTK isolation** — the hfs/macres + preview code stays C
   and runs on a pool thread; confirm none of it touches GTK directly
   (workers must marshal to main — verified clean in the Phase 3 audit per
   CLAUDE.md).
5. **Event ordering** — the terminal completion must run *after* the last
   progress event (channel ordering, or same-context post ordering).

## Effort

Larger than the banner conversion because of cancellation and the four
workers, but bounded — no copy-loop rewrite, byte transport already Rust.
The cooperative-cancel FFI + pass (X1) is the bulk; the spawn flip and
`gtkthreads.c` deletion (X3–X4) are mechanical once cancel is solid.
Roughly the upper end of the R-phase "weeks" scale for one item, dominated
by cancel correctness and the Tier 3 cancel/shutdown coverage. Completing
it closes the R3 exit criterion.
