# S1 — consolidate banner + delete the HTXF C shims

> Cleanup tail of the xfer-worker migration
> (`docs/rust/ROADMAP.md`, the xfer-worker migration section). After W1–W3 moved the transfer
> logic to `hxnet::xfer` and S0 moved the `struct htxf_conn` lifecycle behind
> `hx_htxf_*`, two thin C shims over `hxnet`'s HTXF channel remain:
>
> - `src/htxf_subchannel.c` (68 LOC) — `hx_htxf_subchannel_pack_preamble`, the
>   16/24-byte HTXF handshake header packer callers build before
>   `hxnet_htxf_connect`.
> - `src/htxf_io.c` (157 LOC) — `htxf_io_{init,release,read,write,
>   set_read_timeout,abort_arm,abort}`, thin wrappers over `hxnet_htxf_*` that
>   add a cancel-boundary check + an ECANCELED-vs-EIO reclassification.
>
> Both are the last C between the transfer drivers and `hxnet`. S1 deletes them.

## What still calls the shims

- **Production C:** `banner.c` (transient stack htxf: init / pack_preamble /
  read / release), `network.c::htxf_connect` (pack_preamble / abort_arm),
  `xfers.c` (release / abort).
- **Tests:** `test_htxf_cancel` (drives read/write/timeout/abort directly),
  `test_real_htxf_connect`, the `*_banner` integration tests, and the
  file/folder Tier-3 tests (init / release / pack_preamble).

The `hxnet::xfer` loops (W1–W3) already call `hxnet_htxf_*` directly in-crate,
so they don't go through these shims. The cancel-boundary + ECANCELED logic in
`htxf_io_read/write` is only exercised by `banner.c` (which never cancels) and
`test_htxf_cancel` — production worker cancellation rides the `HtxfAbort` token,
which `hxnet_htxf_read` already observes.

## Increments

- **S1.1 — preamble pack → hxnet; delete `htxf_subchannel.c`. _(Shipped.)_**
  `hotline-proto` gained `build_htxf_preamble` (the `size64` framing — flag
  OR-ing, zeroed legacy length, trailing 8-byte BE size — over the existing
  `build_htxf_hdr`), exposed via `hxnet_htxf_pack_preamble` on the same C ABI
  the old packer had. The ~7 callers switched; `htxf_subchannel.{c,h}` deleted.
  `hxnet_htxf_connect`'s signature was left unchanged (callers still pre-pack)
  so the no-preamble passthrough path keeps working — simpler than folding the
  pack into `connect`.
- **S1.2 — read/write shims → direct `hxnet_htxf_*`; delete `htxf_io.c`.
  _(Shipped.)_** Chose **token-only cancellation**: the shim's canceled-flag
  pre-check + ECANCELED reclassification was dead in production (only banner
  reads and it never cancels; no production writers; the Rust xfer loops read
  `hxnet_htxf_read` directly and cancel via the `HtxfAbort` token — the aborted
  socket makes a parked read return `-1`). Every `htxf_io_*` call site (banner.c,
  xfers.c, network.c, the tests) now calls `hxnet_htxf_*` directly; banner's
  read loop became `hxnet_htxf_read_full`; xfers.c closes via a local
  `xfer_close_channel` (close + NULL, so worker+destructor closes don't
  double-free). `htxf_io.h` renamed to `hxnet_htxf.h` (pure FFI decls);
  `htxf_io.c` deleted. `test_htxf_cancel` retargeted at the token directly
  (obsolete canceled-flag subtest dropped; abort-wakes / abort-before-arm /
  null-safety / fan-out kept under ASan).

## Out of scope

- Opaque-ifying the `struct htxf_conn` scalar fields (an S0 follow-up).
- Any wire-format change — S1 only moves where bytes are built, not the bytes.

## Hard constraints

- Wire compat unchanged: the packed HTXF handshake bytes must be identical
  (a Tier-2 vector over the new `hxnet` path guards this).
- Preserve cancellation: a worker parked in a subchannel read must still wake on
  `xfer_delete` / app shutdown (the `HtxfAbort` path), and the errno the C
  drivers see must not regress in a way that reclassifies a cancel as a fault.
