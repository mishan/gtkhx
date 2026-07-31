# TLS

GtkHx speaks TLS to Hotline servers that offer it. This is a subject
reference for the model, the trust rules, and where the pieces live.

## The separate-port model

There is no in-band TLS negotiation in Hotline. Servers that support
TLS listen on a **dedicated pair of ports** alongside the plaintext
pair, and a client that connects there does the TLS handshake
immediately — TLS from byte zero — then speaks the ordinary,
completely unchanged Hotline 1.x wire protocol over the encrypted
stream.

| Channel | Plaintext | TLS |
|---|---|---|
| Control (HTLS) | 5500 | 5600 |
| File transfer (HTXF) | 5501 | 5601 |

No STARTTLS, no protocol bump, no new opcodes, no new framing. The
file-transfer subchannel follows the control channel: when the control
connection was TLS, the HTXF subchannel goes to the TLS transfer port.
The client derives it as **control port + 1**, so a non-default TLS
control port carries its transfer port with it and there is no separate
"TLS transfer port" concept in the UI or on disk.

The generality of the model is the point: because nothing about the
Hotline bytes changes, **any** legacy Hotline server can be fronted
with `stunnel` (or any TLS-terminating proxy) and become TLS-speaking
without touching the server. GtkHx's TLS support therefore isn't
contingent on a particular server implementation adopting a flag.

## Threat model — why TOFU is the expected path, not a degraded one

Hotline servers overwhelmingly run on numeric IP addresses with no DNS
name. X.509 validation against an IP requires the certificate to carry
an IP SAN, and public CAs will not issue those. So for the common case
— the user types `192.168.1.5:5600` — there is no path to a CA-valid
certificate at all, and self-signed certificates are the norm rather
than the exception.

The design follows from that: WebPKI first, trust-on-first-use as the
normal fallback rather than an error state. A prompt-and-pin on first
connect is the expected user experience for most servers, and the UI
treats it as such.

## Certificate trust

Two stages, mirroring the browser/SSH split:

1. **During the handshake.** The rustls verifier runs real WebPKI
   validation — chain to a native trust root plus hostname (SNI) match
   — against the system root store. It records the verdict and then
   *completes the handshake either way*, so a certificate that doesn't
   chain to a public root can still be offered to the trust gate
   instead of hard-failing the connection. Handshake-signature checks
   always run against the presented certificate; the verifier defers
   the *decision*, not the *check*.
2. **After the handshake.** If WebPKI validated, the certificate is
   trusted silently — no prompt, no store lookup. Only when WebPKI did
   not validate does the connection fall through to the TOFU gate.

The TOFU gate classifies the leaf certificate's SHA-256 fingerprint
against a per-user store and produces one of three states:

| State | Meaning | Behaviour |
|---|---|---|
| **Trusted** | Fingerprint matches a pin for this host:port | Accept silently |
| **Unknown** | No pin for this host:port | Prompt; pin on accept |
| **Mismatch** | A pin exists and the fingerprint differs | Prompt with a distinct, destructive-styled warning; re-pin on accept |

Mismatch is the OpenSSH `REMOTE HOST IDENTIFICATION HAS CHANGED`
case — rotation, server move, or interception — and gets its own copy
and button styling so it can't be clicked through by muscle memory.

One convenience carve-out: a strict-Unknown whose fingerprint is
already pinned for the *same host on another port* is accepted and
pinned silently. That is the control channel on :5600 followed by its
HTXF subchannel on :5601, which would otherwise prompt twice for one
certificate. It never overrides a Mismatch.

### The store

The pin store is `$CONFIG/known_hosts` — SSH-shaped, one entry per
non-comment line:

```text
<host>[:port] sha256:<64-hex> # added <ISO-8601 date>
```

Blank lines and `#` comments round-trip, so a hand edit survives the
next pin. A hostname-only entry (no `:port`) matches every port for
that host, the legacy SSH convention; GtkHx always *writes*
`host:port`. Pins are written atomically (temp file plus rename, mode
0600 where the platform has one). Reads are byte-lossy-decoded rather
than strict UTF-8, so a stray non-UTF-8 byte in a comment can't make
the whole store look empty and re-prompt for a valid pin.

`GTKHX_KNOWN_HOSTS` overrides the path.

## Where it lives

- **`rust/crates/hxnet`** — the connect lifecycle and the transport.
  TLS is `tokio-rustls`; **the GIO / glib-networking TLS path is gone
  entirely.** `src/tls.rs` holds the WebPKI-or-TOFU verifier and the
  client config; the lifecycle applies the trust decision
  post-handshake, before any credentials go out, and closes the stream
  on rejection. HTXF subchannels and the tracker fetch runner use the
  same stack.
- **`rust/crates/hxtls-trust`** — the trust brain, `std`-only so the
  headless test binaries link it without libadwaita. `lib.rs` is the
  pure store (every operation takes an explicit path); `ffi.rs` holds
  path resolution, the classify → accept/prompt/pin decision, and the
  C ABI.
- **`rust/crates/gtkhx-ui/src/tls_trust_dialog.rs`** — the Adwaita
  prompt, and only that. The decision runs on the hxnet worker thread
  and reaches the dialog through a callback registered at UI init;
  that callback hops to the GTK main thread and spins a nested main
  loop, because the verify path has to answer synchronously.
- **`src/network.h`** — the surviving C ABI. `hx_connect(..., char tls)`
  is the entry point; `hx_tls_orchestrator_verify_cert` and
  `hx_tls_verify_subchannel_cert` are the two trampolines the Rust
  verifier calls (the connection-keyed one for the control channel, the
  host:port-keyed one for subchannel workers that snapshot an endpoint
  rather than hold a connection).

The C modules that used to implement this — the trust DB, the trust
dialog, and the `GTlsConnection` accept-certificate plumbing — are all
deleted.

### Test seams

The verify callback runs on a tokio worker, so the test overrides are
process-global setters rather than environment reads (a `setenv` would
race a worker reading `environ`). Each falls back to its environment
variable when unset: `hx_tls_test_set_known_hosts` /
`GTKHX_KNOWN_HOSTS`, `hx_tls_test_set_auto_accept` /
`GTKHX_TLS_AUTO_ACCEPT`, `hx_tls_test_set_force_tls` / `GTKHX_TLS`,
`hx_tls_test_set_prompt_verdict` / `GTKHX_TLS_TEST_PROMPT`.
Auto-accept over a Mismatch logs loudly — a silent override must never
hide a real fingerprint change. With no prompt callback registered
(headless tests), the prompt path rejects.

## HOPE over TLS is rejected

TLS already provides confidentiality and integrity; HOPE's
ChaCha20-Poly1305 layered on top is the same primitive applied twice,
for nothing, and the cipher negotiation against a TLS endpoint fails in
confusing ways. The combination is rejected in two places:

- **UI.** Turning on "Use TLS" in the Connect dialog forces the HOPE
  switch off and greys out HOPE, cipher, and compression.
- **Data layer.** `hx_connect` rejects `secure && tls` up front, before
  dispatching to the connect orchestrator — it cancels any in-flight
  connect, logs to the session, and shows an error dialog. That is the
  backstop for bookmarks and programmatic callers that never went
  through the dialog.

## Bookmarks: the fourth flag byte

The on-disk `HTsc` bookmark is a 460-byte fixed layout. The server
block is a length byte, the `host` / `host:port` string, then flag
bytes:

```text
  203        server length byte = L
  204..204+L "host" / "host:port"
  +0         secure (HOPE) flag
  +1         compress index
  +2         cipher stable byte
  +3         tls flag            <- added for TLS
  ...        zero padding out to a 256-byte server block
```

TLS claimed `flags[3]`, which was previously part of the zero padding.
**The trailing zero pad is what makes this backward compatible in both
directions.** A pre-TLS file has a zero there, which reads as `tls=0` —
correct, with no version bump and no migration. The reader is also
lenient about the byte being absent entirely (a truncated legacy file
reads as TLS off).

A sentinel byte that the old loader would refuse to parse was
considered and **deliberately rejected**. The failure mode it would
guard against is an old client reading a TLS bookmark, ignoring the
flag, and attempting plaintext against the TLS port — which fails with
a clear connect error. That is not worth the drama of an
intentionally-unparseable file.

There is one port field, not two. Toggling TLS in the Connect dialog
flips the default 5500 ↔ 5600; a port the user has already typed is
left alone. New bookmarks default to plaintext on 5500 — TLS is a
deliberate opt-in, not something the client probes for and prefers.

## Tracker connections

The tracker fetch runner is TLS-first with a plain-TCP fallback, over
the same rustls stack and the same `known_hosts` store. A TLS
*handshake* failure records a per-URL "plain-only" verdict and retries
plain; a *transport* failure does not; and a **trust rejection is
hard** — no fallback, because silently downgrading to plaintext after
the store or the user rejected a certificate would be a security
downgrade. See `docs/tracker-protocol.md`.

## Server compatibility

| Server | TLS |
|---|---|
| Janus (VesperNet) | Yes — the live integration-test target for the whole TLS path |
| Mobius | Yes, per its documentation |
| mhxd | No TLS code in the tree |
| Original Hotline 1.0–1.9 Mac clients/servers | No |
| Any server fronted by `stunnel` | Yes, without server cooperation |

Adding TLS broke nothing: plaintext remains the default and every
legacy path is untouched.

## Open

- **Certificate rotation policy.** Today a rotation on a self-signed
  server surfaces a Mismatch prompt every time the certificate changes.
  For a CA-chained certificate the browser-like alternative would be to
  pin issuer plus subject rather than the fingerprint, so a routine
  90-day renewal doesn't prompt. The store schema can absorb either;
  the current behaviour is "always prompt on rotation".
- **Server-side TLS** is out of scope — GtkHx is a client.
