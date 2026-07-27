# S0 — flip `struct htxf_conn` to Rust (Arc + atomics)

> The lifecycle keystone of the xfer-worker migration
> (`docs/rust/ROADMAP.md`, the xfer-worker migration section). W1–W3 moved the *transfer logic*
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
expose `hx_htxf_ref` / `hx_htxf_unref`. This is a faithful 1:1 port of the
current `g_atomic_int_inc` / `g_atomic_int_dec_and_test`, with the allocation +
free moving into Rust. As shipped (S0.3), `hx_htxf_unref` is void and does not
hand the "was-last" verdict back to C — on the last ref it runs the C-registered
destructor (`hx_htxf_set_destructor`) itself, then frees, so the whole teardown
tail lives behind the ABI. Revisit `Arc` only if a later phase makes the handle
purely Rust-side (no C field access), when `Arc`'s ergonomics actually pay off.

### 2. Field access: `#[repr(C)]` mirror, not full opaque

9 C files touch `htxf->…` directly (82 sites): `htxf_io.c`, `network.c`,
`tasks.c`, `files.c`, `gtkhx.c`, `preview.c`, `rcv.c`,
`files_remote_provider.c`, `files_ops.c` — on top of the 18-function
`hx_htxf_*` accessor seam already in `htxf_accessors.c`. Fully opaque-ifying
all 82 sites in one commit is a large, error-prone churn with no behavioural
payoff.

**Decision:** put the storage in the existing `hxnet` crate (new
`xfer_handle` module), not a new crate — `hxnet` already owns the HTXF domain
(the handle's `hx` field *is* an `hxnet::htxf::HtxfConn` transport channel, and
the transfer loops in `hxnet::xfer` drive the handle), it's already linked into
the binary, and C→`hxnet` symbol references resolve fine (that's how the
`hxnet_xfer_*` calls already work). The struct is a `#[repr(C)]` mirror
(`HtxfHandle`, named apart from the channel `HtxfConn`), layout pinned at
runtime by `tests/unit/test_htxf_layout.c` against `sizeof`/`offsetof` on the
real struct — chosen over compile-time `_Static_assert`s because `MAXPATHLEN`
et al. would make a hard-coded size fragile. `hx_htxf_new` allocates it
Rust-side and returns `*mut struct htxf_conn`; C keeps direct field access
through the unchanged `struct htxf_conn` declaration. The lifecycle fields (`refcount`, `canceled`,
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

- **S0.1 — storage module + layout pin. _(Shipped.)_** New
  `hxnet::xfer_handle`: `#[repr(C)] HtxfHandle` mirroring `struct htxf_conn`
  field-for-field, `hx_htxf_new` / `hx_htxf_free` (raw, no refcount move yet),
  plus `hx_htxf_sizeof` / `_alignof` / `_offsetof_*` introspection. `xfers.c`
  allocates via `hx_htxf_new` instead of `g_malloc0` and frees via
  `hx_htxf_free`. `tests/unit/test_htxf_layout.c` pins the ABI at runtime — it
  immediately caught that `compat.h` clamps `MAXPATHLEN` to a fixed 4095 (not
  the host `PATH_MAX`). No behaviour change.
- **S0.2 — atomic lifecycle. _(Shipped.)_** `refcount` / `canceled` /
  `total_pos` are now `AtomicI32` / `AtomicI32` / `AtomicU64` in the mirror
  (layout-identical to the C `gint`/`guint64`, so offsets are unchanged),
  driven behind `hx_htxf_ref` (→ new count) / `hx_htxf_unref` (→ was-last) /
  `hx_htxf_cancel` / `hx_htxf_is_canceled` / `hx_htxf_add_total_pos` /
  `hx_htxf_set_total_pos` / `hx_htxf_total_pos` — a 1:1 port of the old
  `g_atomic_int_*` (SeqCst). `xfers.c`'s `htxf_ref`/`htxf_unref`, the init
  seed, the cancel setters + `fu_dispatch` check, and the `total_pos` bump /
  completion clamp all route through them, as do `htxf_io.c`'s four
  cancel-boundary checks and the `total_pos` reads in `tasks.c` / `gtkhx.c` /
  `files_browser.c`. No C site touches the three fields directly anymore (grep
  gate). Covered by a `ref_unref_cancel_total_pos_transitions` crate unit test
  plus the existing `htxf_cancel` shim test + the Tier-3 transfer paths.
- **S0.3 — fold in the abort token + free tail. _(Shipped.)_** `hx_htxf_new`
  now creates the `HtxfAbort` token (was `xfers.c`'s `htxf_io_abort_init`) and
  `hx_htxf_free` frees it. `hx_htxf_unref` became void and, on the last ref,
  runs a C-registered destructor callback (`hx_htxf_set_destructor`, wired to
  `xfers.c::htxf_destructor` = `hx_preview_unref` + `htxf_io_release` — the
  GTK/preview + channel teardown that stays in C) then `hx_htxf_free`. So
  `xfers.c::htxf_unref` collapsed away — its callers call `hx_htxf_unref`
  directly — and the retired `htxf_io_abort_init` / `htxf_io_abort_free` shims
  are deleted (the `test_htxf_cancel` unit test drives the token via the same
  `hxnet_htxf_abort_new`/`_free` primitives; `htxf_io_abort_arm` / `htxf_io_abort`
  stay). The teardown ordering is preserved: the destructor closes the channel
  (dropping the channel's token ref) before `hx_htxf_free` drops the creation ref.

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
