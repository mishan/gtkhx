# SOCKS / proxy support in hxnet (`tokio-socks`) — scoping

Scoping for adding central SOCKS-proxy support to the `hxnet` connect
path. This is **Phase R3 work item 9** (the one genuinely-deferred R3
item) — the prerequisite named in `connect.rs` for restoring transparent
SOCKS on the orchestrated control channel and the tracker (item 8, now in
Rust), and for the follow-on of moving the HTXF subchannel's C-side
connect into Rust too.

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
- **S3 — Rides the connect moves.** The tracker (item 8, done) + the
  HTXF subchannel pass the same proxy URI once their connects are in Rust;
  their C `GSocketClient` connects are deleted.

## Testing

- **Rust unit:** a minimal loopback SOCKS5 responder (or `tokio-socks`
  test utilities) — assert connect-through-proxy, remote-DNS target
  selection, and authenticated vs. anonymous handshakes.
- **Tier 3:** stand up a SOCKS5 proxy (dante / 3proxy / `ssh -D`) in the
  matrix and assert the control channel (then HTXF / tracker) reach
  mhxd / Janus *only* via the proxy — ideally with direct egress blocked
  in the test netns so a proxy bypass fails loudly rather than silently
  succeeding.
- **Regression:** the `proxy = None` path stays byte-identical to today.

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
3. **Remote vs. local DNS.** Go socks5h (remote) when proxied; note the
   behaviour change from today's local resolve, and that the direct path
   is unaffected.
4. **TLS over the tunnel.** Unchanged in principle (rustls wraps the
   tunnelled stream), but verify the SNI / cert verification still sees the
   real target hostname, not the proxy's.
5. **Proxy auth secrets.** If authenticated proxies are in scope, creds
   come from the URI / env only for now — no pref storage of secrets.

## Effort

The Rust core is the "~50 LOC" the `connect.rs` comment predicted. The
bulk is S2: the C `GProxyResolver` helper, threading `proxy_uri` through
the open FFIs, and a Tier 3 proxy container. Roughly a few focused days,
independent of the tracker / HTXF connect moves — and worth sequencing
*first*, since it also closes the current control-channel proxy
regression rather than only enabling cleanup.
