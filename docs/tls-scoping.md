# TLS Client Support — Scoping Notes for GtkHx

Status: scoped, decisions locked (§12). **Phase 2's load-bearing
prerequisite — the HTXF GIOStream port — shipped 2026-05-27** as
branch `claude/htxf-giostream` (PR #122, merged). The remaining
Phase 2 work is just flowing the TLS flag through the now-stream-
shaped HTXF connect helpers. Written 2026-05-26 against
<https://github.com/jhalter/mobius/blob/master/docs/tls.md> at fetch
time, refreshed 2026-05-27 after the HTXF port landed. Janus /
VesperNet TLS status is unconfirmed (memory note [[gtkhx_janus]] —
closed-source binary, no public docs).

This supersedes the "Phase ∞ — Modernized Hotline protocol" entry in
ROADMAP.md, which assumed transport security required inventing a new
HOPE-style negotiation layer AND server-side cooperation we don't
have. Mobius's TLS shipped with neither: it's plain TLS-from-byte-zero
on a separate port. We can adopt it as a client now and call it
**Phase 7**, not Phase ∞.

What stays out of scope: server-side TLS (we're a client),
TLS-for-the-Hotline-tracker (Mobius doesn't do it, and the tracker
protocol is a separate UDP/TCP wire format we'd need a separate
scoping pass for), TLS for the Hotline 1.x in-band cipher (the HOPE
ChaCha20 / RC4 / Blowfish stack stays exactly as-is — they coexist
with TLS, they don't compete with it).

---

## 1. What Mobius actually shipped

Quoting the docs (and condensed):

- TLS runs on **separate ports** alongside plaintext: control
  defaults to 5600 (vs 5500), file-transfer to 5601 (vs 5501).
- TLS-enabled is a server-side flag (`-tls-cert` + `-tls-key`); both
  ports run simultaneously so legacy clients keep working.
- The protocol on the TLS port is the **unchanged Hotline 1.x wire
  protocol** — TLS just wraps the TCP socket from the first byte.
  No STARTTLS, no HOPE-style in-band negotiation, no protocol bump.
- File transfer follows the same model: the HTXF subchannel that
  normally goes to port 5501 goes to 5601 instead when the control
  connection was TLS.
- Self-signed certs are explicitly anticipated ("clients may require
  trust prompt"). Let's Encrypt is the suggested production path.

That's the entire spec. No new handshake, no new framing, no new
opcodes. It's about as simple as TLS adoption can be.

The same model is what stunnel / `openssl s_client` would expose for
any TCP protocol — which is convenient, because it means a third-party
TLS-fronting proxy can also turn any legacy Hotline server into a
TLS-speaking one without server cooperation. That's worth mentioning
in the user-visible UI: "TLS works against Mobius servers and any
server fronted by an `stunnel`-style TLS terminator." We don't depend
on every server adopting Mobius's flag.

---

## 2. Current state of GtkHx's network layer

The connect path is async and lives in `network.c`:

- `hx_connect` → `g_socket_client_new()` →
  `g_socket_client_connect_to_host_async(...)`. Returns a
  `GSocketConnection` whose `GIOStream` becomes `htlc->in` /
  `htlc->out`. Everything downstream reads / writes through that
  stream — `rcv.c`, `commands.c`, `cipher.c` HOPE wrap.
- Sync helper `hx_sync_connect_to_host` for HTXF worker threads.
  Returns a **`GSocketConnection`** since the GIOStream port
  (PR #122). `xfers.c` and `banner.c` cast it to GIOStream and
  feed it to `htxf_io_read` / `htxf_io_write` (`src/htxf_io.{c,h}`),
  which handle both the plaintext path and the HOPE+ChaCha20 AEAD
  framing layer.
- Async tracker connect (same `GSocketClient` shape as the main
  connect path).

Three pieces matter for TLS:

1. **Async control connect** — works against GIO streams, easy to
   TLS-ify (`g_socket_client_set_tls(client, TRUE)` and the rest is
   transparent).
2. **Sync HTXF connect** — now also `GSocketConnection`-shaped.
   The worker call sites already feed a GIOStream into htxf_io_*,
   so wrapping it in a `GTlsClientConnection` only needs a single
   `g_socket_client_set_tls(client, TRUE)` call inside
   `hx_sync_connect_to_host` (gated on the per-htxf TLS flag). One
   small lingering item: `xfers.c::htxf_io_get_socket` walks
   through `G_IS_SOCKET_CONNECTION` directly to reach the underlying
   GSocket for `g_socket_condition_timed_wait`; under TLS that
   helper needs a `G_IS_TLS_CONNECTION` branch that fetches the
   `base-io-stream` property. Maybe ~5 LOC.
3. **Tracker connect** — unchanged. Mobius doesn't TLS its tracker
   listener, and we have no evidence anyone else does either.
   Re-scope if a TLS-tracker spec emerges.

Zero TLS code exists today (`grep -i tls src/` is empty modulo the
unrelated `HTLS_` Hotline-Server opcode prefix). No TLS dep is wired
up in `meson.build`.

---

## 3. Dependency story

GLib's GIO already ships TLS support via the **glib-networking** GIO
module. We don't link a TLS library directly; we use GIO's
`g_socket_client_set_tls(...)`, `GTlsClientConnection`, and the
`accept-certificate` signal, and at runtime glib-networking picks
GnuTLS or OpenSSL depending on the build/distro.

No new compile-time dep — `glib-networking` is already a runtime
dep of `gtk4` on every distro we care about, and it ships in the
GNOME 49 Flatpak runtime we already use. We may want to add an
`appstreamcli`-visible note that TLS is available, but no link
flags or pkg-config additions.

If we eventually need raw GnuTLS in the HTXF path (see §4 option B),
that does add a build dep. The GIO-only path doesn't.

---

## 4. The HTXF problem — resolved

This was the load-bearing question mark in the original scoping.
Three options were on the table — port the HTXF workers to
`GIOStream` (option A), add a `tls_fd_t` shim (B), or run a
stunnel-style proxy thread (C). Option A was the recommendation
and it shipped on `claude/htxf-giostream` (PR #122, merged
2026-05-27) in six phased commits:

- Phase A: `htxf_connect` and `hx_sync_connect_to_host` return
  `GSocketConnection` instead of raw fd
- Phase B: `htxf_io_read` / `htxf_io_write` (new `src/htxf_io.{c,h}`)
  take `GIOStream *io`; internals use `g_input_stream_read` /
  `g_output_stream_write_all`
- Phase C: `xfers.c::rd_wr` split into `rd_wr_recv` / `rd_wr_send`
  to route through `htxf_io_*` (incidentally fixed a latent AEAD
  bulk-transfer bug); `preview_get` ported to GIOStream
- Phase D: `select(s+1, ...)` drain in the folder-receive path
  replaced with `g_socket_condition_timed_wait` on the underlying
  GSocket
- Phase E: `banner.c` worker ported; `read_n`/`write_n` static
  helpers deleted
- Phase F: Tier 2 round-trip tests for `htxf_io_*` against
  `GMemoryInputStream` + `GMemoryOutputStream` wrapped in
  `GSimpleIOStream` (15 subtests covering plaintext, AEAD round-
  trip, NULL-io rejection, oversized-length-prefix rejection,
  tag tampering, etc.)

The HTXF path now flows through `GIOStream` end-to-end. Wrapping
it in `GTlsClientConnection` is a one-line change in
`hx_sync_connect_to_host` (set the client's TLS flag before
`g_socket_client_connect_to_host`).

This branch was driven by the TLS scoping but it also delivered
two related wins:

- **SOCKS proxying works for HTXF for free**, since GIO routes
  socket-client connects through `GProxyResolver` automatically.
- **Latent AEAD bulk-transfer bug fixed**: rd_wr's raw read()/write()
  loop bypassed the AEAD frame codec on the bulk data fork. The
  routing change in Phase C funnels everything through `htxf_io_*`,
  closing the bypass.

The original (B) and (C) options are obsolete — kept here as
historical context for why we picked A.

---

## 5. Certificate trust UX

Mobius's docs flag this directly: self-signed is anticipated. The
behaviour we need:

1. **First connect** — validate against the system CA bundle. If
   valid, just connect, no prompt.
2. **CA failure** — show an Adwaita `AdwAlertDialog` with the cert
   details (subject CN, issuer, fingerprint SHA-256, expiry, the
   specific `GTlsCertificateFlags` bits that failed). Options:
   - **Cancel** — abort the connect.
   - **Connect this time** — accept for this session only.
   - **Trust this server** — pin the fingerprint per host:port and
     skip the prompt on future connects.
3. **Subsequent connects** — if pinned, validate against the pinned
   fingerprint instead of the CA. If the cert rotated, prompt
   again with a "this cert changed since last time" notice (similar
   to OpenSSH's `WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!`).

That's a small new module — call it `tls_trust.{c,h}`, ~200 LOC —
plus a persistence file at `$CONFIG/tls-known-hosts` (one
fingerprint per line, host:port:sha256). Mirrors the SSH
known-hosts mental model that anyone running a Hotline server is
already familiar with.

The dialog wiring belongs in `connect.c` because that's where the
`hx_connect` flow lives, but the trust DB and the cert formatter
should live in `tls_trust.c` so they're unit-testable without
spinning up a real TLS connection.

---

## 6. Bookmark / Connect dialog UI

Two new fields per bookmark:

- `tls` (boolean) — TLS on/off.
- `tls_port` (u16, optional) — defaults to **5600** (the Mobius
  convention) when `tls` is true and the user hasn't picked one
  explicitly. Or, more elegantly: a single `port` field that the
  user fills in, defaulting to 5500 for plaintext and 5600 for TLS.
  This avoids the "two port fields" UI awkwardness — pick one
  representation, surface a tooltip explaining the default.

Connect dialog: a "Use TLS (encrypted)" toggle near the existing
HOPE / cipher / compress controls. When on, port auto-flips
5500→5600 (and the user can override). HOPE + cipher + compress
should grey out — they're redundant with TLS (TLS already provides
confidentiality + integrity; HOPE's ChaCha20-Poly1305 is the same
primitive used twice). The Mobius docs don't say anything about
forbidding HOPE-over-TLS, but it's wasted CPU and the cipher
negotiation could fail in confusing ways. Make them mutually
exclusive in the UI; if a bookmark sets both on, prefer TLS and
silently drop the HOPE bits.

For the HTXF subchannel: derive `htxf_tls_port` from `tls_port + 1`
(again the Mobius convention). No separate UI field; that's
internal.

---

## 7. Bookmark file format

Today (HTsc 460-byte layout):

```
"HTsc"<ver><129 zeros><login len + login + zeros>
<pass len + pass + zeros>
<server len + server + ':port' + flags[3] + zeros>
```

Where `flags[0..2]` is `secure`, `compress`, `cipher`. **The trailing
zero pad runs for hundreds of bytes**, and new clients reading old
bookmarks see those zeros as legitimate `tls=0` data.

Cleanest extension: claim `flags[3] = tls` (currently zeroed). No
magic version bump needed; backward compat is automatic in both
directions. Old GtkHx reading a TLS-flagged bookmark would silently
ignore the new flag and connect plaintext to the bookmark's port —
which is *wrong* (would try plaintext on 5600 and fail), but at
least doesn't crash or corrupt. We can guard against this in the
loader by also flagging `tls=1` bookmarks with a sentinel byte the
old loader would refuse to parse, but that's more drama than it's
worth — the failure mode is "connect fails with a clear error".

Skip a separate `tls_port` byte if we go with the single-port
representation in §6. The existing port field already takes a
string up to 252 bytes; that's plenty.

---

## 8. Server compatibility matrix

| Server | TLS? | Notes |
|---|---|---|
| Mobius | **Yes** | Confirmed via docs. Ports 5600/5601 default. Self-signed common. |
| Janus (VesperNet) | Unknown | Closed-source binary. Worth probing: `openssl s_client -connect hotline.vespernet.net:5600`. If it answers, confirmed. |
| mhxd | No | C codebase, no TLS code in the tree. Could add it (it's the natural mhxd contribution), but out of scope here. |
| Hotline 1.0–1.9 Mac clients | No | None of the original Mac clients spoke TLS. |
| hlserver.com | No | Behaves like a 1.0/1.2 server; no TLS evidence. |
| Badmoon (1.9) | No | No evidence of TLS support. |

Adding TLS to GtkHx therefore unlocks: Mobius servers, possibly
Janus, and any legacy server fronted by `stunnel`. Doesn't break
anything else (legacy plaintext stays the default).

---

## 9. Phasing

Five landable pieces, each with its own merge:

**Phase 1 — Control-channel TLS, no UI.**
- Add `g_socket_client_set_tls()` call gated on a new `tls` field on
  the connect/session struct (no UI plumbing yet — flip it via a
  pref or hardcoded test build).
- Wire `GTlsConnection::accept-certificate` to a stub that accepts
  everything (allows testing against a self-signed Mobius without
  the trust UI yet).
- Tier 3 integration test: spin up Mobius with TLS in Docker
  Compose, point GtkHx at it, exercise login + chat round-trip.
- Estimated diff: ~100 LOC src + ~150 LOC tests.

**Phase 2 — HTXF over TLS.**
- _Prerequisite already shipped_: the HTXF GIOStream port landed
  on `claude/htxf-giostream` (PR #122) — option (A) from §4. The
  ~250 LOC src + ~250 LOC tests estimate from this scope doc was
  the prerequisite work, not Phase 2 itself. See §4 for the
  delivered phasing (A–F).
- Remaining work: thread the `tls` flag from the session struct
  through `htxf_connect` and `hx_sync_connect_to_host` to the
  `GSocketClient`. Both helpers already use GSocketClient
  internally, so it's a `g_socket_client_set_tls(client, TRUE)`
  call before `g_socket_client_connect_to_host` plus the
  per-htxf flag plumbing.
- Grow `xfers.c::htxf_io_get_socket` to handle the TLS-wrapped
  case: when the GIOStream is a GTlsConnection, fetch the
  `base-io-stream` property to reach the underlying
  GSocketConnection / GSocket (the `accept-certificate` plumbing
  and trust prompt come in from Phase 3, but the GSocket fetch
  is independent and small).
- Tier 3: TLS file transfer + TLS banner fetch round-trips.
- Estimated diff: ~30 LOC src + ~100 LOC tests.

**Phase 3 — Cert trust UX.**
- `tls_trust.{c,h}` module: fingerprint pinning, known-hosts DB.
- Adwaita prompt dialog wired from `connect.c`.
- Tier 1 unit tests for the trust DB; Tier 3 covers the dialog
  paths under accept / pin / reject flows.
- Estimated diff: ~250 LOC src + ~150 LOC tests.

**Phase 4 — Connect dialog + bookmark UI.**
- "Use TLS" toggle in `connect.c`; port auto-flip; HOPE+cipher
  grey-out when TLS on.
- Bookmark format extension (claim `flags[3]`); bookmarks dialog
  TLS toggle row.
- Tier 1: bookmark round-trip with the new flag; pre-existing
  bookmarks still load with `tls=0`.
- Estimated diff: ~150 LOC src + ~50 LOC tests.

**Phase 5 — Documentation + matrix entry.**
- README / man page mention.
- Add the TLS-enabled Mobius container to the Tier 3 matrix as a
  new entry (parallel to the Janus / mhxd ones).
- ROADMAP.md: mark Phase 7 done, retire the "Phase ∞" entry.

Order matters: Phase 1 has to land before 2, 3, 4 can be tested
end-to-end. Phases 2, 3, 4 are otherwise independent and could
land in any order.

---

## 10. Open questions

- **Janus TLS support.** Easy to confirm with `openssl s_client`
  against `hotline.vespernet.net:5600`. Worth doing before Phase 1
  so the test matrix can include it.
- **Trust DB on-disk format detail.** SSH known-hosts is the
  decided shape (see §12), but Hotline doesn't have a "host key
  fingerprint" concept independent of the cert — we'd be pinning
  the X.509 fingerprint, which rotates whenever the cert does.
  Open: "always prompt on rotation" (annoying for production
  servers that rotate every 90 days) vs. "pin the issuer +
  subject when the cert chains to a real CA, and only pin the
  fingerprint for self-signed" (browser-like). Punt to Phase 3
  implementation — the trust DB schema can absorb either.
- **TLS for the tracker.** Out of scope here (Mobius doesn't TLS
  the tracker, neither does anyone else we know of). If a tracker
  spec emerges, separate scoping doc.
- **Cert verification edge cases.** Hotline servers commonly run on
  numeric IPs (no DNS hostname). X.509 verification against an IP
  requires the cert's SAN to include the IP — Let's Encrypt won't
  issue those. So for the common case ("user types
  `192.168.1.5:5600`"), the user will hit the trust prompt and pin
  the cert. That's expected and fine under the TOFU model from
  §12, but worth documenting in user-facing release notes.

---

## 11. Effort estimate

Rough numbers, assuming smooth code review. Phase 2's prerequisite
HTXF GIOStream port has already shipped (PR #122) so the remaining
TLS work is considerably smaller than the original ~750 LOC src
estimate.

| Phase | Code LOC | Test LOC | Calendar time | Status |
|---|---|---|---|---|
| 0 — HTXF GIOStream port | ~600 | ~430 | shipped | done (PR #122) |
| 1 — Control-channel TLS | ~100 | ~150 | 1–2 days | pending |
| 2 — HTXF over TLS | ~30 | ~100 | half day | pending (prereq done) |
| 3 — Cert trust UX | ~250 | ~150 | 2–3 days | pending |
| 4 — Connect + bookmark UI | ~150 | ~50 | 1–2 days | pending |
| 5 — Docs + matrix | — | — | half day | pending |

**Remaining: ~530 LOC src, ~450 LOC tests, ~1 week of focused work.**

The biggest risk used to be Phase 2's HTXF GIOStream port — that
ended up taking six commits (Phases A–F on `claude/htxf-giostream`),
incidentally caught a latent AEAD bulk-transfer bug, and grew Tier
2 round-trip coverage. With that done, Phase 1 (the control-channel
TLS) is now the riskiest piece — but it's a single
`g_socket_client_set_tls` call plus the `accept-certificate` stub,
backed by a Tier 3 test that exercises a TLS-enabled Mobius
container. Hard to get wrong.

---

## 12. Decisions locked (2026-05-26)

1. **Port representation: single port field.** The Connect dialog
   and bookmarks dialog have one port input. Toggling the TLS
   checkbox auto-flips the default (5500 → 5600) if the field
   hasn't been hand-edited; once the user has typed a port, the
   toggle leaves it alone. No separate "TLS port" field.
2. **Trust model: SSH-style TOFU.** First connect with an unknown
   cert prompts the user; accept-once and trust-this-server are
   both offered. Trusted certs pin per host:port in
   `$CONFIG/tls-known-hosts`. Cert rotation surfaces a "this cert
   changed" prompt — the open detail in §10 is whether to pin the
   fingerprint or the issuer+subject for CA-chained certs; that
   gets decided during Phase 3 implementation.
3. **Default port on new bookmarks: 5500 (plaintext).** TLS is
   opt-in via the checkbox. Don't probe-and-prefer; surface TLS
   as a deliberate user choice.
4. **HOPE-over-TLS: forbid.** When TLS is on, the Connect dialog
   greys out HOPE / cipher / compress. On disk, a bookmark with
   both TLS and HOPE flags set has the HOPE bits silently dropped
   at load (debug-logged via `GTKHX_DEBUG=login`); the connect
   still goes through, TLS-only.

These pin the v1 behaviour. All four are reversible if real-world
usage suggests otherwise after shipping; explicitly out of scope
for re-debate during Phases 1–5.
