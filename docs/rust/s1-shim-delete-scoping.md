# S1 — consolidate banner + delete the HTXF C shims

> Cleanup tail of the xfer-worker migration
> (`docs/rust/xfer-worker-to-rust-scoping.md`). After W1–W3 moved the transfer
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

- **S1.1 — preamble pack → `hxnet_htxf_connect`; delete `htxf_subchannel.c`.**
  `hotline-proto` already has `build_htxf_hdr` (the 16-byte header) +
  `HTXF_MAGIC` / `HTXF_HDR_SIZE`. Move the `size64` framing (flag OR-ing,
  zeroing the legacy 32-bit length, the trailing 8-byte BE size) into
  `hxnet_htxf_connect`, changing its FFI from `(preamble, preamble_len,
  xfer_ref, …)` to `(xfer_ref, total_size, type, flags, size64, …)` and packing
  the handshake internally. Update the ~7 callers; delete
  `htxf_subchannel.{c,h}`. Self-contained (touches only the preamble).
- **S1.2 — read/write shims → direct `hxnet_htxf_*`; delete `htxf_io.c`.**
  Decide where the cancel-boundary check lives: fold the
  canceled-flag-before-read + ECANCELED reclassification into `hxnet_htxf_read/
  write` (they already observe the token; add the pre-check reading the handle's
  `canceled` atomic), or accept token-only cancellation and drop the C
  reclassification. Replace every `htxf_io_{init,release,read,write,
  set_read_timeout,abort_arm,abort}` call site (banner.c, xfers.c, network.c,
  and the tests) with the `hxnet_htxf_*` equivalent, then delete
  `htxf_io.{c,h}`. Bigger, and touches the cancel path — guard with
  `test_htxf_cancel` (migrated) + the Tier-3 transfer matrix.

## Out of scope

- Opaque-ifying the `struct htxf_conn` scalar fields (an S0 follow-up).
- Any wire-format change — S1 only moves where bytes are built, not the bytes.

## Hard constraints

- Wire compat unchanged: the packed HTXF handshake bytes must be identical
  (a Tier-2 vector over the new `hxnet` path guards this).
- Preserve cancellation: a worker parked in a subchannel read must still wake on
  `xfer_delete` / app shutdown (the `HtxfAbort` path), and the errno the C
  drivers see must not regress in a way that reclassifies a cancel as a fault.
