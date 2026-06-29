# SOCKS / proxy support in hxnet (`tokio-socks`) — scoping

Scoping for adding central SOCKS-proxy support to the `hxnet` connect
path. This is **Phase R3 work item 9** (the one genuinely-deferred R3
item) — the prerequisite named in `connect.rs` for restoring transparent
SOCKS on the orchestrated control channel and the tracker (item 8, now in
Rust), and for the follow-on of moving the HTXF subchannel's C-side
connect into Rust too.

> **Status: DONE (S1 + S2 merged; S3 on `claude/socks-s3-tracker-htxf`,
> pending merge).** All three network paths — control channel, HTXF
> subchannel, and tracker — now connect through the same `GProxyResolver`-
> sourced SOCKS proxy via `resolve_and_connect`; the C `GSocketClient`
> connects are gone. The TL;DR + "current state" table below describe the
> *original* problem this work set out to fix and are kept for context;
> see the per-phase **Phasing** section for what shipped and **Deferred /
> optional follow-ups** for what's intentionally left.

## TL;DR

- **The main control channel already lost transparent SOCKS.** When the
  orchestrator became the default-and-only connect path, the control
  channel started connecting via `tokio::net::TcpStream` in
  `hxnet::connect::resolve_and_connect`, which never consults
  `GProxyResolver`. The legacy `GSocketClient` connect that *did* honour
  it is gone. So proxy support is currently **inconsistent**: control
  channel = no proxy (a regression vs. the pre-orchestrator client), while
  HTXF and the tracker still get it because their connect is C/
  `GSocketClient`.
- **Adding `tokio-socks` centrally does double duty:** it *restores*
  proxy support for the control channel and *unblocks* moving the
  remaining C-side connects (the HTXF subchannel; the tracker, item 8,
  already moved) fully into Rust without losing it. Worth doing first.
- **The transport change is small** — one connect primitive
  (`resolve_and_connect`, four in-crate callers). The real design question
  is **where the proxy config comes from**, since `GProxyResolver`'s
  gsettings / PAC / libproxy integration has no Rust equivalent.

## Current state — the three network paths

| Path | Connect | Proxy today |
|------|---------|-------------|
| Control channel | `network.c` → `hxnet_connection_open_{plaintext,plaintext_tls,hope}` → `lifecycle.rs` → `resolve_and_connect` (tokio) | **None** |
| HTXF (files/banner) | `xfers.c` / `banner.c` `GSocketClient` connect + fd-handoff into `hxnet::htxf` | Works (`GProxyResolver`) |
| Tracker | `network.c` tracker state machine, `GSocketClient` async | Works (`GProxyResolver`) |

The only TCP-connect primitive in Rust is
`hxnet::connect::resolve_and_connect` (`connect.rs`). Callers: `lifecycle.rs`
(plaintext / TLS / HOPE — three sites) and the `hxnet_connection_open_tcp`
FFI (one site). That's the single point the proxy branch plugs into.

## Scope — two distinct pieces

### 1. Transport mechanic (small, well-bounded)

- Add the `tokio-socks` crate (MIT; GPL-2.0-or-later compatible) as an
  **optional** dependency behind a non-default `socks` Cargo feature, so
  embedders that don't need proxying skip the extra deps. The `ProxyConfig`
  type + URI parsing are always compiled (cheap, no deps); only the
  `tokio-socks` tunnel and the SOCKS connect tests are feature-gated. The
  gtkhx build turns the feature on (`hxnet/socks` in `rust/meson.build`);
  CI tests with it on. A build without the feature that's nonetheless
  handed a proxy fails loudly (`ErrorKind::Unsupported`) rather than
  silently connecting direct.
- When a SOCKS proxy is configured, `resolve_and_connect` connects TCP to
  the proxy, runs the SOCKS5 (or SOCKS4) handshake targeting `(host,
  port)`, then calls `Socks5Stream::into_inner()` to recover a plain
  `TcpStream`. **Everything downstream is unchanged** — rustls TLS, magic,
  LOGIN, and HOPE all layer on top of the tunnelled stream exactly as they
  do over a direct `TcpStream`.
- **Remote DNS (socks5h semantics):** in the proxied branch, pass the
  hostname to the proxy instead of resolving locally, so proxy-only
  hostnames resolve and DNS doesn't leak. The direct branch keeps the
  existing `lookup_host` + v4-first fallback.
- `resolve_and_connect` grows an `Option<ProxyConfig>` argument; the
  proxy connect + handshake reuse the existing `HANDSHAKE_TIMEOUT_SECS`
  timeout and cancellation semantics.

### 2. Proxy configuration sourcing (the actual decision)

`GProxyResolver` gave gsettings + libproxy + PAC + per-host rules for
free. Replacing it:

- **(A) Query `GProxyResolver` in C, pass the result into hxnet.** At
  connect time C calls `g_proxy_resolver_lookup()` for the target and
  passes the chosen `socks5://…` URI (or NULL for direct) into the open
  FFI. Preserves the full desktop integration (gsettings / PAC / per-host).
  Cost: thread a proxy-URI argument through the `hxnet_connection_open_*`
  surface (and later the htxf / tracker opens). **Recommended** — it's the
  only option that matches the pre-orchestrator behaviour.
- **(B) Env-only in Rust.** Parse `ALL_PROXY` / `SOCKS_PROXY` /
  `NO_PROXY` inside hxnet. Near-zero C changes, but ignores GNOME proxy
  settings and PAC. Fine as an MVP or as a headless/CLI fallback, not
  parity.
- **(C) A gtkhx pref.** An explicit "SOCKS proxy host:port" row in
  Settings — simple and predictable, but a third config source. Could
  complement (A).
- **Recommendation:** (A) for parity, optionally falling back to (B)'s env
  vars when `GProxyResolver` returns direct, so headless use still works.

**HTTP-CONNECT gap:** `GProxyResolver` can also hand back `http://`
proxies; `tokio-socks` only does SOCKS4/5. HTTP CONNECT proxy support
would be separate work (a small CONNECT shim, or a crate). Decide up front
whether to support it or document the new path as SOCKS-only.

## Design / change points

- `connect.rs`: `resolve_and_connect(host, port, proxy: Option<ProxyConfig>,
  evt_tx)`. Direct branch unchanged; proxy branch uses `tokio-socks` +
  remote DNS.
- A `ProxyConfig` type — `{ scheme: Socks4 | Socks5, addr, auth:
  Option<(user, pass)> }` — parsed from a `socks5://[user:pass@]host:port`
  URI.
- FFI: add a nullable `proxy_uri: *const u8` to the
  `hxnet_connection_open_*` family (NULL = direct); `lifecycle.rs` threads
  it into `resolve_and_connect`.
- C: one helper that runs `g_proxy_resolver_lookup` for a target and
  returns the chosen URI (or NULL). Reused by the control-channel connect
  now (and by the tracker, which has since moved to Rust — item 8), and by
  the HTXF opens once their fd-handoff connect moves to Rust (the
  follow-on cleanup noted under item 9).
- Net effect: the tracker + HTXF pass the proxy URI to Rust and drop their
  `GSocketClient` connects — the C connect goes away entirely.

## Phasing

- **S1 — Rust transport, no wiring. DONE.** Added `tokio-socks` (opt-in
  behind a non-default `socks` feature), the proxy branch in
  `resolve_and_connect`, and `ProxyConfig` parsing. Unit-tested against a
  loopback mock SOCKS5 server. Callers pass `None`, so it was a pure
  addition with zero behaviour change.
- **S2 — Config sourcing + control channel. DONE.**
  Added the C `bridge_lookup_socks_proxy` helper (`GProxyResolver`, queried
  with the `none://host:port` scheme that `GSocketClient` itself uses for a
  raw TCP connect) and threaded `proxy_uri` / `proxy_uri_len` through the
  three control-channel open FFIs (plaintext / TLS / HOPE) + their request
  structs. This **restores SOCKS for the main connection.** Config source
  is GProxyResolver-only (no env-var fallback in C — `GProxyResolver`'s
  default already honours `all_proxy` / GNOME settings). Only SOCKS proxy
  results are honoured; a non-SOCKS (e.g. `http://`) result is warned and
  skipped. Tier 3 coverage landed too: a microsocks proxy container
  (`tests/socks-proxy/`) + `tests/integration/test_integration_socks.c`
  drives the production connect through the proxy to mhxd (via-proxy login
  + dead-proxy negative control), wired into the integration CI job.
- **S3 — Rides the connect moves. DONE.** Both remaining paths now honour
  the same proxy:
  - **Tracker:** `hxnet_tracker_fetch_open` gained a `proxy_uri`;
    `TcpTlsConnector` threads it into `resolve_and_connect` for every
    tracker connect. A single proxy applies to the whole walk, resolved
    once in C (`hx_bridge_lookup_socks_proxy`) for the first tracker —
    the uniform-proxy case `all_proxy` / GNOME settings target anyway.
  - **HTXF:** the subchannel connect moved into Rust
    (`hxnet_htxf_connect`: `resolve_and_connect` driven from the blocking
    worker via the runtime + a channel), so the C `GSocketClient` connect
    + fd dup + `hxnet_htxf_open(fd)` adoption are gone (the fd-adopting
    `hxnet_htxf_open` survives only as a socketpair test entry). The dead
    `hx_sync_connect_to_host` + its `GSocketClient` TLS accept-cert
    plumbing were deleted.
  - A shared IPv6-aware `src/host_port.{c,h}` (`gtkhx_parse_host_port` /
    `gtkhx_join_host_port`) replaced the hand-rolled `strrchr(':')` splits
    across network/bookmarks/connect/tls_trust/tracker/bridge.
  - Tested end to end against the live mhxd + Argus + hxtrackd matrix
    (file/folder transfers, banner, HOPE banners, tracker v1/v3/TLS), with
    the proxy paths exercised through a real SOCKS5 proxy (an
    `hxnet_htxf_connect`-through-proxy unit test + `tracker_fetch` via
    `GTKHX_TEST_SOCKS`).

## Testing (as shipped)

- **Rust unit:** a loopback SOCKS5 responder in `connect.rs` asserts
  connect-through-proxy, remote-DNS target selection, and authenticated
  vs. anonymous handshakes; an `hxnet_htxf_connect`-through-proxy test in
  `htxf.rs` covers the HTXF path.
- **Tier 3:** a microsocks SOCKS5 proxy container (`tests/socks-proxy/`) +
  `tests/integration/test_integration_socks.c` drive the production
  connect through the proxy to mhxd. Instead of a netns with direct egress
  blocked, a **dead-proxy negative control** proves the proxy isn't
  bypassed (a connect that ignored the proxy would wrongly succeed against
  the directly-reachable mhxd). The tracker proxy path is exercised by
  pointing `tracker_fetch` at a real proxy via `GTKHX_TEST_SOCKS`.
  *Possible hardening:* a true blocked-egress netns is stricter still, but
  the negative control covers the main bypass risk.
- **Regression:** the `proxy = None` path stays byte-identical to direct.

## Risks / open questions

1. **Config source (A/B/C). RESOLVED → GProxyResolver-only (C).** No
   env-var fallback in C; `GProxyResolver`'s default backend already reads
   `all_proxy` / `ALL_PROXY` / GNOME settings. The FFI takes a parsed
   `socks5://` URI string.
2. **HTTP CONNECT proxies.** Out of `tokio-socks` scope. An explicit
   `http(s)://` URI passed to `ProxyConfig::from_uri` is rejected loudly;
   a `GProxyResolver` *result* of `http://` is warned and skipped (we
   can't tunnel a raw Hotline stream through HTTP-CONNECT). Revisit if a
   network that only offers an HTTP proxy turns up.
3. **Remote vs. local DNS. RESOLVED.** The proxied branch passes the
   target as a domain tuple so the proxy resolves it (socks5h / socks4a
   remote DNS); the direct path keeps its local `lookup_host` + v4-first
   fallback.
4. **TLS over the tunnel. RESOLVED.** rustls wraps the tunnelled stream
   and `wrap_tls` / `connect_tls` use the real target hostname for SNI +
   the TOFU fingerprint, not the proxy's — verified by the live TLS
   tracker listing test through the proxy.
5. **Proxy auth secrets.** Authenticated proxies work (the `user:pass@`
   from the resolver's URI); the password is redacted in `Debug` / logs /
   error paths. Still no in-app pref storage of proxy creds — they come
   from the system / env config only.

## Deferred / optional follow-ups

- **Async proxy lookup.** `g_proxy_resolver_lookup` is synchronous on the
  GLib main thread (`bridge_lookup_socks_proxy`). The default backends
  (GNOME GSettings, the env-var `GSimpleProxyResolver`) match rules in
  memory and don't block, but a PAC/WPAD backend could stall the UI during
  connect. The fix (`g_proxy_resolver_lookup_async` with the `open_*` call
  deferred into the completion callback) restructures the synchronous
  "`bridge_handle` set before return" install contract, so it was
  deliberately deferred. See the note in `hxnet_bridge.c`.
- **In-app proxy pref.** Config is GProxyResolver-only, so a proxy comes
  from OS / GNOME settings or `all_proxy`. A "SOCKS proxy host:port" row in
  Settings (the old option C) would let users set one without touching
  system / env config — UX-only, not required.
- **HTTP-CONNECT proxies.** Still out of scope (item 2); add a CONNECT
  shim only if an HTTP-only-proxy network turns up.

## Effort

The Rust core is the "~50 LOC" the `connect.rs` comment predicted. The
bulk is S2: the C `GProxyResolver` helper, threading `proxy_uri` through
the open FFIs, and a Tier 3 proxy container. Roughly a few focused days,
independent of the tracker / HTXF connect moves — and worth sequencing
*first*, since it also closes the current control-channel proxy
regression rather than only enabling cleanup.
