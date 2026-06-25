# HTXF (file transfer) → Rust migration — scoping

> Sibling to `docs/rust/phase-g-migration.md`. Phase G moved the **control
> channel** onto the hxnet orchestrator. This doc scopes moving the
> **HTXF data subchannel** (file transfers + banner fetch) off its
> remaining C transport glue onto Rust.

## Correction: the crypto is already in Rust

An earlier draft of this doc (and the C-crypto-cleanup section of
`phase-g-migration.md`) framed HTXF as keeping a C **AEAD
implementation** alive. That was wrong — verified by reading the
sources:

- `cipher_aead.c` is a **182-line pass-through shim**: every function
  (`cipher_aead_seal` / `open` / `peek_frame_size` /
  `derive_transfer_keys` / `derive_session_keys` / `hkdf`) just calls
  the matching `gtkhx_aead_*` symbol in the **`hxcrypto-aead`** Rust
  crate. Zero crypto in the C file.
- `cipher.c` (380 lines) is likewise C **glue**: the control-channel
  `cipher_encode` / `cipher_decode` qbuf framing calls the Rust
  Blowfish primitive `gtkhx_blowfish_ofb64_crypt` (`hxcrypto-stream`).
- HTXF (`htxf_io.c`) uses **only** the AEAD — no Blowfish — so its
  crypto is *already* `hxcrypto-aead`, reached through the
  `cipher_aead.c` shim.

So there is **no HTXF crypto to port**. What's still in C is the
**transport glue**: the subchannel connect, the preamble, the
GIOStream byte-pump framing in `htxf_io.c`, and the pthread workers.
The remaining payoff of an HTXF→Rust move is therefore (a) deleting
the `cipher_aead.c` shim, and (b) moving the transport so the HTXF
TLS path can use rustls instead of the shared C `GTlsConnection`
accept-cert handler — which is the entanglement that blocks a clean
`delete-old-connect` (see `phase-g-migration.md`).

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

## What's already in Rust (more than the earlier draft assumed)

- **`hxcrypto-aead`** owns the entire ChaCha20-Poly1305 seal/open
  framing **and** both key derivations (`gtkhx_aead_seal` /
  `gtkhx_aead_open` / `gtkhx_aead_peek_frame_size` /
  `gtkhx_aead_derive_transfer_keys` / `gtkhx_aead_derive_session_keys`).
  `cipher_aead.c` already calls straight through to these. HTXF's
  crypto *is* this crate today — there is nothing to write or port.
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

Since the crypto is already Rust, the phases are about transport
glue and shim removal, not crypto.

### Phase H1 — delete the `cipher_aead.c` shim (optional, low value)

The only "crypto migration" left is cosmetic: re-point the
`cipher_aead_*` callers at `gtkhx_aead_*` directly and delete the
182-line shim. But the shim is a clean, stable C ABI over the Rust
FFI, and most of its callers (`rcv.c` / `hope.c` / `network.c` /
`network_decode.c` / `hxnet_bridge.c`) are control-channel code that
`delete-old-connect` will gut anyway — so re-pointing them now is
throwaway churn. **Recommendation: skip H1.** Leave the shim; it
isn't crypto debt, just one indirection. (If it bugs you, fold the
shim removal into `delete-old-connect`, which already touches those
files.)

### Phase H2 — Rust subchannel transport (the real work)

Move the connect + preamble + the read/write byte pump into a Rust
`HtxfChannel` the C worker drives over FFI (`htxf_open`,
`htxf_read`, `htxf_write`, `htxf_close`). The worker stays a C
pthread calling blocking FFI; Rust owns the socket + AEAD. This
deletes `htxf_io.c` + `htxf_subchannel.c` and the
`htxf_connect` / `hx_sync_connect_to_host` pair, and gives the
subchannel the same rustls TLS path the control channel uses.

The payoff here is **not crypto** (already Rust) — it's that the
HTXF TLS path stops sharing the C `GTlsConnection` accept-cert
handler (`on_socket_client_event`) and uses rustls instead. That
shared handler is exactly what blocks a clean `delete-old-connect`
(see `phase-g-migration.md`). So H2 is the step that makes the
legacy-connect deletion a clean cut.

**Decision point:** keep the C pthread worker calling blocking Rust
FFI (lowest risk, preserves the throughput profile), or move the
worker itself onto a tokio task and marshal progress through the
existing ferry. The former is recommended first — the folder
protocol and resume logic in `xfers.c` are a lot of fiddly C that
doesn't need to move.

### Phase H3 — (stretch) folder protocol in Rust

Only if there's appetite: port the GETFOLDER/PUTFOLDER framing +
resume logic into Rust. Large, no crypto/transport payoff, high
behavior-risk (resume, partial transfers, name encoding). Almost
certainly not worth it — listed for completeness.

## What this unlocks for the C cleanup

The crypto is already Rust, so the table is about C **glue/shim**
files, not crypto:

| File | What it is | Deletable when |
|------|-----------|----------------|
| `compress.c` | control-channel zlib glue | `delete-old-connect` (no non-control consumer) |
| `cipher_aead.c` | 182-line AEAD pass-through shim | callers re-pointed at `gtkhx_aead_*` — fold into `delete-old-connect` (control callers) once HTXF is the only other user |
| `cipher.c` | control-channel Blowfish-OFB-64 qbuf glue (calls `gtkhx_blowfish_ofb64_*`) | `delete-old-connect` (HTXF doesn't use it — confirmed `htxf_io.c` has zero Blowfish refs) |
| hash primitives (`md5`/`sha`/`haval`/`hmac`) | C glue over `hxcrypto-hash`? — audit | after their last C caller goes |

Note: HTXF only ever arms **AEAD** (ChaCha20) — the subchannel is
encrypted only when the control channel negotiated
`CIPHER_MODE_AEAD`. There is no Blowfish-on-HTXF (`htxf_io.c` has no
`gtkhx_blowfish_*` / `cipher_encode` refs), so `cipher.c` is purely
control-channel and dies with `delete-old-connect`.

## Risks / open questions

- **Throughput.** Benchmark a large transfer before/after H2 — the
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

The crypto is already Rust, so there's no "retire the C crypto"
step to do — that part is done. Concretely:

- **Skip H1.** The `cipher_aead.c` shim isn't crypto debt; leave it,
  or fold its removal into `delete-old-connect` (which touches the
  same control-channel callers anyway).
- **H2 is the real HTXF→Rust work**, and its payoff is *transport*,
  not crypto: moving the subchannel onto rustls removes the shared
  C TLS accept-cert handler, which is the entanglement blocking a
  clean `delete-old-connect`. If the goal is to unblock the legacy
  deletion, H2 is the step — scope it as "C pthread worker drives a
  blocking Rust `HtxfChannel` over FFI; Rust owns the socket + TLS +
  AEAD," keeping the folder/resume logic in C.
- **H3 (folder protocol in Rust)** stays a non-goal.

Net: the HTXF crypto migration the earlier draft imagined was
already complete. The remaining decision is whether H2's transport
move is worth doing now (to unblock `delete-old-connect`) or to keep
the legacy deletion on hold until there's appetite for H2.
