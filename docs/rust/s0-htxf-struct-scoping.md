# S0 — flip `struct htxf_conn` to Rust (Arc + atomics)

> The lifecycle keystone of the xfer-worker migration
> (`docs/rust/xfer-worker-to-rust-scoping.md`). W1–W3 moved the *transfer logic*
> (solo + folder, both directions) into `hxnet::xfer`; the worker already
> references no C symbols and drives everything through `HxnetXferParams` /
> `HxnetFolderParams`. What stays in C is the **refcounted, cross-thread
> `struct htxf_conn`** and its GTK worker shell (`xfers.c`). S0 moves the struct
> storage + lifecycle behind a Rust-owned handle.

## Why it's delicate

`htxf_conn` is the one place with a genuine cross-thread ownership knot:

- `xfers[]` (main thread) holds 1 ref.
- the transfer worker (tokio blocking pool) holds 1 ref.
- every pending `post_file_update` / cleanup idle (queued from the worker, run
  on main) holds 1 ref.

`refcount` and `canceled` are already `gint` mutated only via `g_atomic_int_*`;
`total_pos` is bumped by the worker and read by the main-thread task view. Get
the refcount/atomics wrong and it's a use-after-free, not a visible test
failure — so S0 must preserve the exact ownership model, not "improve" it.

## The two design axes

### 1. Refcount: keep intrusive-atomic, don't reach for `Arc` yet

The scoping bullet said "Arc-based refcount (clones *are* the refs)". But the
"N pending idles each hold a ref" pattern maps badly onto `Arc<T>` across FFI:
each idle is a `void*` C closure payload, so we'd be doing `Arc::into_raw` /
`from_raw` by hand at every `post_*` site and clone — which is exactly the
manual `g_atomic_int_inc/dec` we have now, minus the type safety, plus the
footgun of a raw-pointer `Arc` whose strong count you can't inspect.

**Decision:** keep the intrusive `refcount: AtomicI32` inside the handle and
expose `hx_htxf_ref` / `hx_htxf_unref` (returns "was-last" so C can run the
destructor tail). This is a faithful 1:1 port of the current
`g_atomic_int_inc` / `g_atomic_int_dec_and_test`, with the allocation and the
free moving into Rust. Revisit `Arc` only if a later phase makes the handle
purely Rust-side (no C field access), when `Arc`'s ergonomics actually pay off.

### 2. Field access: `#[repr(C)]` mirror, not full opaque

9 C files touch `htxf->…` directly (82 sites): `htxf_io.c`, `network.c`,
`tasks.c`, `files.c`, `gtkhx.c`, `preview.c`, `rcv.c`,
`files_remote_provider.c`, `files_ops.c` — on top of the 18-function
`hx_htxf_*` accessor seam already in `htxf_accessors.c`. Fully opaque-ifying
all 82 sites in one commit is a large, error-prone churn with no behavioural
payoff.

**Decision:** move the struct into a self-contained Rust crate (`hxhtxf`) as a
`#[repr(C)]` mirror, layout pinned by `_Static_assert`s on the C side (the
`gtkhx-boxed` pattern). `hx_htxf_new` allocates it Rust-side and returns
`*mut HtxfConn`; C keeps direct field access through the unchanged
`struct htxf_conn` declaration. The lifecycle fields (`refcount`, `canceled`,
`total_pos`) become the crate's atomics, mutated through
`hx_htxf_{ref,unref,cancel,is_canceled,add_total_pos}` so no C site does a raw
`g_atomic_int_*` on them anymore. The remaining scalar/string fields stay
plain and C-visible; opaque-ifying them is a *later* cosmetic phase, explicitly
out of S0 scope.

This keeps S0 to: (a) Rust owns alloc/free + the atomic lifecycle; (b) the
`htxf_io_abort_*` token bookkeeping collapses into the handle's `new`/`unref`;
(c) `xfers.c`'s hand-rolled `htxf_ref`/`htxf_unref`/`g_free` become calls into
the crate. Consumer field reads/writes are untouched.

## Increments

- **S0.1 — crate + layout pin.** New `rust/crates/hxhtxf`: `#[repr(C)]
  HtxfConn` mirroring `struct htxf_conn` field-for-field, `hx_htxf_new` /
  `hx_htxf_free` (raw, no refcount yet), and `_Static_assert`s in a C TU (or
  `proto_helpers.c`) pinning `sizeof` + every offset. `xfers.c` allocates via
  `hx_htxf_new` instead of `g_new0`. No behaviour change; a Tier-1 layout test
  guards the ABI.
- **S0.2 — atomic lifecycle.** Move `refcount` / `canceled` / `total_pos` to
  crate atomics behind `hx_htxf_ref` (→ new count) / `hx_htxf_unref` (→ bool
  was-last) / `hx_htxf_cancel` / `hx_htxf_is_canceled` / `hx_htxf_add_total_pos`
  / `hx_htxf_total_pos`. Rewrite `xfers.c`'s `htxf_ref`/`htxf_unref` +
  `htxf_io.c`'s `g_atomic_int_get(&htxf->canceled)` cancel-boundary checks +
  the worker's `total_pos` bumps onto them. The struct's `gint refcount,
  canceled` become `_Atomic`-shaped in the mirror (C never touches them
  directly again — grep gate).
- **S0.3 — fold in the abort token + free tail.** `hx_htxf_new` creates the
  `HtxfAbort` (today `htxf_io_abort_init`); `hx_htxf_unref`'s last-ref tail runs
  the `hx_preview_unref` + `htxf_io_release` + `htxf_io_abort_free` +
  free that `xfers.c::htxf_unref` does today (via a C-side destructor callback
  the crate invokes, so the GTK/preview couplings stay in C). `htxf_accessors.c`
  shrinks to the pure field getters/setters.

Each increment is independently testable and revertible; S0.1 is pure
scaffolding, S0.2 is the behavioural core (guard it hardest — the Tier-3
`file_get`/`folder_roundtrip` cancel paths + a fresh unit test on the
ref/unref/cancel transitions), S0.3 is cleanup.

## Out of scope (later)

- Opaque-ifying the scalar/string fields (the 82 direct sites) — cosmetic,
  no lifecycle payoff, huge churn.
- `Arc<HtxfConn>` — only worth it once C no longer touches fields.
- S1 (banner consolidation + deleting `htxf_io.c` / `htxf_subchannel.c`).

## Hard constraints

- Preserve the ownership model exactly (see the lifecycle comment over
  `refcount` in `protocol.h`). The bug class here is UAF, invisible to a green
  test run — reason about every ref/unref pairing.
- Wire compat unchanged (S0 touches no bytes on the wire).
- `hxhtxf` is a leaf-ish crate; it must not reference C symbols it can't resolve
  at link time (same rule as `hxnet`). The destructor tail that needs GTK /
  preview runs through a C-registered callback, not an upward extern.
