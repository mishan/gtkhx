# SOCKS / proxy support in hxnet (`tokio-socks`) — scoping

Scoping for adding central SOCKS-proxy support to the `hxnet` connect
path. This is the prerequisite named in `connect.rs` and in Phase R3 work
items 8 (tracker → hxnet) and 9 (revisit HTXF's C-side connect).

## TL;DR

- **The main control channel already lost transparent SOCKS.** When the
  orchestrator became the default-and-only connect path, the control
  channel started connecting via `tokio::TcpStream` in
  `hxnet::connect::resolve_and_connect`, which never consults
  `GProxyResolver`. The legacy `GSocketClient` connect that *did* honour
  it is gone. So proxy support is currently **inconsistent**: control
  channel = no proxy (a regression vs. the pre-orchestrator client), while
  HTXF and the tracker still get it because their connect is C/
  `GSocketClient`.
- **Adding `tokio-socks` centrally does double duty:** it *restores*
  proxy support for the control channel and *unblocks* moving HTXF and the
  tracker fully into Rust (items 8/9) without losing it. Worth doing
  first, before those two.
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

- Add the `tokio-socks` crate (MIT; GPL-2.0-or-later compatible).
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
  now, and by the tracker (item 8) and HTXF (item 9) opens when they move
  to Rust.
- Net effect for items 8/9: they pass the proxy URI to Rust and drop their
  `GSocketClient` connects — the C connect goes away entirely.

## Phasing

- **S1 — Rust transport, no wiring.** Add `tokio-socks`, the proxy branch
  in `resolve_and_connect`, and `ProxyConfig` parsing. Unit-test against a
  loopback mock SOCKS5 server. Callers pass `None`, so this is a pure
  addition with zero behaviour change.
- **S2 — Config sourcing + control channel.** Add the C `GProxyResolver`
  helper and thread `proxy_uri` through the control-channel open FFI. This
  **restores SOCKS for the main connection.** Validate Tier 3 through a
  SOCKS proxy container.
- **S3 — Rides items 8/9.** Tracker + HTXF pass the same proxy URI when
  they move to Rust; their C `GSocketClient` connects are deleted.

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

1. **Config source (A/B/C).** The main decision; it gates the FFI shape.
   Recommend A, env fallback optional.
2. **HTTP CONNECT proxies.** Out of `tokio-socks` scope — support with a
   shim or document as unsupported. `GProxyResolver` returning an `http://`
   proxy must at least fail clearly, not silently connect direct.
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
independent of items 8/9 — and worth sequencing *first*, since it also
closes the current control-channel proxy regression rather than only
enabling cleanup.
