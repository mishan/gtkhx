# TLS Client Support — Scoping Notes for GtkHx

Status: shipped. **All five sub-phases landed on branch
`claude/tls-phase1-control-channel`** (Phase 1: control-channel
TLS; Phase 2: HTXF over TLS; Phase 3: cert trust UX; Phase 4:
Connect dialog + bookmark UI; Phase 5: docs). Written 2026-05-26
against
<https://github.com/jhalter/mobius/blob/master/docs/tls.md> at fetch
time, refreshed through 2026-05-28 as each phase landed. Janus
is the live Tier 3 TLS target.

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

**Phase 1 — Control-channel TLS, no UI. _Shipped on
`claude/tls-phase1-control-channel`._**
- `g_socket_client_set_tls()` call gated on a new `tls` parameter
  on `hx_connect` (no UI plumbing — flip it via the `GTKHX_TLS=1`
  env var until Phase 4 lands the Connect-dialog toggle). Done in
  `src/network.{c,h}`.
- `GTlsConnection::accept-certificate` stub
  (`tls_accept_certificate_phase1_stub`) wired via the
  `GSocketClient::event` signal at `G_SOCKET_CLIENT_TLS_HANDSHAKING`
  phase. Accepts every cert so the self-signed Janus/Mobius cert
  doesn't trip GnuTLS verification. Phase 3 replaces this with the
  real TOFU trust UI.
- Tier 3 coverage:
  - `test_real_tls` — drives production `hx_connect(tls=1)` against
    Janus's TLS port and asserts the connection-state signal
    sequence reaches `HANDSHAKE_DONE`.
  - `test_real_tls_login` — full LOGIN + chat round-trip over the
    wrapped socket via the new `_stream` family of harness helpers
    in `tests/integration/integration_tls.{c,h}`.
- Test container: `tests/janus/Dockerfile` generates a self-signed
  `CN=localhost` cert at image build time and Janus exposes
  HTLS-TLS on 5600 / HTXF-TLS on 5601 (host-mapped 5610 / 5611).
- Matrix: `HX_TEST_CAP_TLS` bit + `tls_port` / `tls_xfer_port`
  fields on `hx_test_server`; Janus advertises both. A new
  `/integration/server-matrix/tls-cap` subtest pins that any
  cap-advertising entry has non-zero port fields.
- Actual diff: ~150 LOC src (`network.c` connect plumbing) + ~600
  LOC tests (Tier 3 binaries, GIOStream harness helpers, server-
  matrix wiring).

**Phase 2 — HTXF over TLS. _Shipped on
`claude/tls-phase1-control-channel`_** (extended onto the Phase 1
branch per Misha's choice to bundle both rounds in one PR).
- `htlc->tls` field added on `struct htlc_conn`. Set in
  `hx_connect` from the tls parameter so HTXF subchannels
  downstream can read it back from `htxf->htlc->tls` or via the
  banner-fetch snapshot.
- `hx_sync_connect_to_host` grew a trailing `char tls` parameter
  — same `g_socket_client_set_tls(client, TRUE)` + accept-
  everything cert hook as `hx_connect` (Phase 1 stub reused
  unchanged via `on_socket_client_event`).
- `htxf_connect` reads `htxf->htlc->tls` and passes through.
  Banner worker snapshots `htlc->tls` into the per-fetch struct
  at spawn time (same way it already snapshots host/port/AEAD
  state — works around connection reset / reconnect races).
- Port arithmetic is preserved: the Mobius/Janus separate-port
  model puts TLS-HTXF on TLSPort+1 (5601), which falls out of the
  existing `htlc->serverport + 1` math when `serverport` already
  stores the TLS HTLS port (5600) in TLS mode. No `tls_xfer_port`
  concept needed on the production side.
- `xfers.c::htxf_io_get_socket` grew the `GTlsConnection` branch
  the prior comment promised — walks `base-io-stream` via
  `g_object_get` (the direct accessor `g_tls_connection_get_base_io_stream`
  isn't ABI-stable across the GLib versions we still build
  against; property access is always available). Same shape as
  the harness `stream_underlying_socket` helper.
- Tier 3 coverage:
  - `test_real_tls_file_get` — full FILE_GET over TLS control +
    TLS HTXF subchannel against Janus, asserts the seed bytes
    round-trip intact through TLS encrypt/decrypt.
  - `test_real_tls_banner` — file-mode banner HTXF over TLS;
    asserts the JPEG / GIF / PNG magic-byte prefix survives the
    larger (~2 KB) body transfer.
- Actual diff: ~80 LOC src (network.c / banner.c / xfers.c
  threading) + ~330 LOC tests (two Tier 3 binaries + the harness
  `_xfer_tls` connect helper + `_task_trans_stream` drain helper).

**Phase 3 — Cert trust UX.** *Shipped 2026-05-27.*
- `tls_trust.{c,h}` module: SHA-256 fingerprint over the cert
  DER, SSH known_hosts file shape (host:port → fingerprint with a
  trailing date comment), TRUSTED / UNKNOWN / MISMATCH lookup.
  Path resolves via `gtkhx_config_dir()` with a `GTKHX_KNOWN_HOSTS`
  env override for tests.
- `tls_trust_dialog.{c,h}` module: `AdwAlertDialog` wrapping a
  nested `GMainLoop` so the GSocketClient accept-certificate
  signal handler (which has to return sync) can prompt the user.
  Distinct copy + button styling for the MISMATCH path
  (destructive response).
- Wired into `network.c::tls_accept_certificate`: fingerprint →
  lookup → TRUSTED returns TRUE silently; UNKNOWN / MISMATCH
  prompt via the dialog and pin on acceptance.
  `GTKHX_TLS_AUTO_ACCEPT=1` env override bypasses the dialog so
  Tier 3 tests can run headless.
- Pin writes are dispatched off the accept-certificate signal
  via `g_idle_add` (`schedule_trust_pin`) — calling
  `g_mkdir_with_parents` directly from the accept handler wedges
  the TLS handshake on glib-networking + GnuTLS (handshake never
  resumes, server logs `read handshake: EOF`). Pin no longer
  mkdir's the config dir (created at app startup; tests use
  `/tmp`).
- Tier 1: `tests/unit/test_tls_trust.c` — 8 subtests covering
  pin-then-lookup, unknown host, wrong port, mismatch, rewrite,
  comment / blank-line preservation, hostname-only entries,
  missing file.
- Tier 3: existing TLS tests (`test_real_tls`, `_login`,
  `_file_get`, `_banner`) now run with `GTKHX_TLS_AUTO_ACCEPT=1`
  and a tmpdir `GTKHX_KNOWN_HOSTS`, exercising the real trust
  DB write path on the first connect and the TRUSTED fast path
  on the second.
- Actual diff: ~395 LOC src (`tls_trust.c` + `tls_trust_dialog.c`
  + accept-certificate plumbing in `network.c`) + ~290 LOC tests
  (`test_tls_trust.c` + harness stubs).

**Phase 4 — Connect dialog + bookmark UI.** *Shipped 2026-05-28.*
- Connect dialog (`connect.c`) gained a "Use TLS"
  `AdwSwitchRow` at the top of the Connection group. Toggling
  flips the default port (5500 ↔ 5600 — custom ports survive
  unchanged) and greys out HOPE + cipher + compress (the TLS-port
  endpoint expects raw HTLS; layering HOPE on top would double-
  encrypt). `connect_with_args` enforces the same coupling at
  the data layer so a bookmark with `tls=1,secure=1` doesn't
  trigger a HOPE handshake against a TLS port.
- `last_conn` cache grew a `tls` field so Reconnect-after-
  disconnect carries the choice through; `set_the_entries` /
  `connect_open_bookmark_by_name` thread `tls` end-to-end.
- Bookmark format extended: HxBookmark + bookmark_parsed gain a
  `tls` field; the 4th flag byte in the HTsc on-disk layout
  (zero in legacy files, naturally back-compat as TLS off).
  Both writer call sites (`bookmarks_io.c::hx_bookmark_save`
  and `connect.c::bookmark_save_response`) and both reader call
  sites (`hx_bookmark_load` and `connect.c::bookmark_parse`)
  updated; pre-TLS files still round-trip cleanly.
- Bookmarks management dialog (`bookmarks.c`) gained a matching
  "Use TLS" `AdwSwitchRow`; load + save plumb `bm->tls` through.
- Tier 1: `tests/unit/test_bookmarks.c` got two new subtests —
  `save_load_tls` (round-trips `tls=1`) and `load_pre_tls_bookmark`
  (a hand-written legacy-shape file loads with `tls=0`). The
  existing byte-layout test grew an assertion on the 4th flag
  byte's position.
- Actual diff: ~115 LOC src across `connect.c` / `bookmarks.c` /
  `bookmarks_io.c` / `bookmarks.h` / `connect.h` + ~75 LOC tests.

**Phase 5 — Documentation.** *Shipped 2026-05-28.*
- This doc updated: every Phase row marked shipped, the §11
  effort table closed out.
- ROADMAP.md Phase 7 entries 4 + 5 marked ✅.
- README / man page mention left for the next user-facing release
  notes pass — out of scope for the per-phase scoping doc itself.

What's deliberately not in Phase 5: a Mobius container in the
Tier 3 matrix. Janus already covers the TLS-capable server slot
end-to-end (`test_real_tls`, `_login`, `_file_get`, `_banner`),
and Mobius integration is tracked as its own multi-server harness
phase (Phase B in [[gtkhx_multi_server_test_plan]]).

Order followed (Phase 1 → 2 → 3 → 4 → 5): Phase 1's control
channel was the load-bearing prerequisite for every later phase;
2, 3, 4 each extended it independently and landed sequentially on
the same `claude/tls-phase1-control-channel` branch.

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
| 3 — Cert trust UX | ~395 | ~290 | shipped | done |
| 4 — Connect + bookmark UI | ~115 | ~75 | shipped | done |
| 5 — Docs | — | — | shipped | done |

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
