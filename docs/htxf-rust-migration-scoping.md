# HTXF (file transfer) → Rust migration — scoping

> Sibling to `docs/phase-g-migration.md`. Phase G moved the **control
> channel** onto the hxnet orchestrator. This doc scopes moving the
> **HTXF data subchannel** (file transfers + banner fetch) onto the
> same Rust stack — the migration that finally lets `cipher_aead.c`
> (and, with it, the last of the C transport crypto) be deleted.

## Why

After `delete-old-connect`, the only thing keeping the C transport
crypto alive is HTXF:

- `cipher_aead.c` (ChaCha20-Poly1305) — the HTXF subchannel AEAD.
- the `cipher.h` / `cipher_aead.h` includes in `htxf_io.c`,
  `htxf_subchannel.c`, `banner.c`.

HTXF runs on its own socket, outside the orchestrator, on pthread
worker threads with a GIOStream byte pump. So it's a self-contained
migration: nothing about it is entangled with the control-channel
work, and finishing it collapses the C crypto surface to just the
hash primitives + whatever `hope.c`/`network_decode.c` legacy bits
remain.

## Current architecture

A transfer is a short-lived second TCP connection ("subchannel") to
the same server, opened per file (or per folder stream), driven by a
blocking pthread worker:

```
xfers.c worker (get_thread / put_thread / folder_get_thread /
                folder_put_thread)  +  banner.c banner worker
   │
   ├─ htxf_connect (network.c)
   │     └─ hx_sync_connect_to_host  → GSocketClient blocking connect
   │        (TLS-aware, SOCKS via GProxyResolver)  → GSocketConnection
   │
   ├─ hx_htxf_subchannel_pack_preamble (htxf_subchannel.c)
   │     16-byte HTXF header (+8 for 64-bit size when SIZE64)
   │
   ├─ hx_htxf_subchannel_arm_aead (htxf_subchannel.c)
   │     cipher_aead_derive_transfer_keys(ctrl keys → xfer keys)
   │     sets htxf->aead_active, htxf->xfer_encode / xfer_decode
   │
   └─ loop: htxf_io_read / htxf_io_write (htxf_io.c)
         AEAD-aware framed read/write over the GIOStream:
           aead_read  → cipher_aead_peek_frame_size + cipher_aead_open
           aead_write → cipher_aead_seal
         (plaintext passthrough when !aead_active)
```

File sizes (LOC): `xfers.c` 1879, `banner.c` 842, `htxf_io.c` 368,
`htxf_subchannel.c` 87, plus the `htxf_connect` / `hx_sync_connect_to_host`
pair in `network.c`.

Key properties:

- **Throughput-sensitive.** This is the bulk-data path; the
  control channel is tiny by comparison. Any rewrite must not
  regress transfer speed (the AEAD frames are pre-sized to avoid
  per-call reallocation; `htxf_io` loops `write_all`).
- **pthread, not async.** Each transfer is a blocking worker;
  progress marshals to the GLib main thread via `g_idle_add` /
  `gtkhx_post_to_main`. This is *different* from the control
  channel's tokio-actor model.
- **Folder transfers** (`folder_get_thread` / `folder_put_thread`)
  speak the Hotline 1.5 GETFOLDER/PUTFOLDER mini-protocol *inside*
  the subchannel byte stream (per-file headers, name lengths,
  resume markers) — that framing logic is non-trivial and lives in
  `xfers.c`, not in the transport layer.

## What's already reusable in Rust

The control-channel work left exactly the pieces HTXF needs:

- **`hxcrypto-aead`** implements the ChaCha20-Poly1305 seal/open
  framing that `cipher_aead_seal` / `cipher_aead_open` /
  `cipher_aead_peek_frame_size` implement in C — *the same wire
  frame format*. HTXF and the control channel share
  `cipher_aead.c`'s primitives today; they differ only in key
  derivation (`derive_transfer_keys` vs `derive_session_keys`).
  So the Rust side needs a `derive_transfer_keys` equivalent
  (small — mirrors the existing `derive_aead_keys`) and can reuse
  `AeadState` + the framing verbatim.
- **`hxnet`'s transform stack** (`AeadStream` over an
  `AsyncDuplex`) already *is* an AEAD-framed read/write over a
  byte stream — structurally what `htxf_io.c` hand-rolls.
- **The connect path.** `resolve_and_connect` (or a
  `GSocketClient`-backed connect, if we want SOCKS for free on the
  subchannel too — see the open question below) already exists.
- **The GIOStream→Rust bridge pattern** from `hxnet_bridge` shows
  how to hand a socket to Rust and marshal events back to GLib.

So this is largely *reuse + re-point*, not a from-scratch crypto
or framing implementation.

## Migration design (phased)

The transfer worker is blocking and throughput-critical, so the
cleanest shape is a **Rust subchannel transport** that the existing
C worker drives synchronously through a thin FFI — NOT a wholesale
move of the folder-protocol logic into Rust (that's a much bigger,
riskier change with little crypto payoff). In other words: move the
*bytes + crypto*, leave the *file/folder orchestration* in C for now.

### Phase H1 — Rust AEAD framing primitive (no transport)

Add `hxcrypto-aead::derive_transfer_keys` + a small
`AeadFramer` (or reuse `AeadState` directly) exposed over FFI:
`hxnet_htxf_aead_seal` / `hxnet_htxf_aead_open` /
`hxnet_htxf_aead_peek_frame_size`. Pure functions over caller
buffers — no sockets, no threads, no runtime. Re-point
`htxf_io.c`'s `aead_read` / `aead_write` at these instead of
`cipher_aead.c`. Tier 2 round-trip tests against the C
implementation pin byte-for-byte parity.

**Unlocks:** `cipher_aead.c` can be deleted once nothing else
calls it (control channel already uses the Rust AEAD post-Phase G;
this removes the last C caller). Smallest, highest-value step —
it's the one that actually retires the C crypto.

### Phase H2 — Rust subchannel transport (optional, larger)

Move the connect + preamble + the read/write byte pump into a Rust
`HtxfChannel` the C worker drives over FFI (`htxf_open`,
`htxf_read`, `htxf_write`, `htxf_close`). The worker stays a C
pthread calling blocking FFI; Rust owns the socket + AEAD. This
deletes `htxf_io.c` + `htxf_subchannel.c` and the
`htxf_connect` / `hx_sync_connect_to_host` pair, and gives the
subchannel the same rustls TLS path the control channel uses.

**Decision point:** keep the C pthread worker calling blocking Rust
FFI (lowest risk, preserves the throughput profile), or move the
worker itself onto a tokio task and marshal progress through the
existing ferry. The former is recommended first — the folder
protocol and resume logic in `xfers.c` are a lot of fiddly C that
doesn't need to move to get the crypto win.

### Phase H3 — (stretch) folder protocol in Rust

Only if there's appetite: port the GETFOLDER/PUTFOLDER framing +
resume logic into Rust. Large, low crypto payoff, high
behavior-risk (resume, partial transfers, name encoding). Almost
certainly not worth it for this round — listed for completeness.

## What this unlocks for the C crypto cleanup

| File | After Phase G (`delete-old-connect`) | After HTXF H1/H2 |
|------|--------------------------------------|------------------|
| `compress.c` | deletable (no non-control consumer) | — |
| `cipher_aead.c` | **stays** (HTXF subchannel) | **deletable** (H1) |
| `cipher.c` Blowfish | control-channel uses go; audit `htxf_io.c`'s `cipher.h` include (appears unused for Blowfish — HTXF is AEAD-only) | deletable if the include is confirmed dead |
| hash primitives (`md5`/`sha`/`haval`/`hmac`) | used by remaining HOPE/HTXF crypto | re-point to `hxcrypto-hash`, then delete |

Note: HTXF only ever arms **AEAD** (ChaCha20) — the subchannel is
encrypted only when the control channel negotiated
`CIPHER_MODE_AEAD`. There is no Blowfish-on-HTXF, so `cipher.c`'s
survival is really about confirming the `htxf_io.c` `cipher.h`
include is vestigial.

## Risks / open questions

- **Throughput.** Benchmark a large transfer before/after H1 — the
  FFI call boundary per AEAD frame must not dominate. Frames are
  bulk-sized, so per-frame FFI overhead should be negligible, but
  measure.
- **SOCKS on the subchannel.** The current `hx_sync_connect_to_host`
  gets SOCKS for free via `GProxyResolver`. If H2 moves the connect
  to tokio, the subchannel loses proxy support (same gap the
  control channel has). Mitigation: keep the GSocketClient connect
  in C and hand the fd to Rust, or wire `tokio-socks`. (Leaning:
  keep the C connect for H2, hand off the connected fd — mirrors
  how the control-channel bridge adopts an fd.)
- **TLS subchannel.** H2 would let the subchannel use rustls
  instead of `GTlsClientConnection`; until then leave it on the C
  TLS path.
- **Banner fetch** (`banner.c`) is a second HTXF consumer — H1
  covers it for free (it goes through `htxf_io`); H2 must keep its
  worker shape working too.
- **Resume / partial transfers** must survive H2 unchanged — the
  byte-pump move can't perturb the offset bookkeeping the folder
  workers do.

## Recommendation

Do **Phase H1 only** as the next concrete step: re-point
`htxf_io.c`'s AEAD at `hxcrypto-aead` over a thin FFI, prove
byte-parity in Tier 2, and delete `cipher_aead.c`. That captures
the entire crypto-cleanup payoff with minimal behavior risk and
leaves the throughput-critical worker + folder logic untouched. H2
(transport move) and H3 (folder protocol) are separable follow-ups
to weigh on their own merits, not prerequisites for retiring the C
crypto.
