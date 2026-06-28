# SOCKS5 proxy test container

A minimal [microsocks](https://github.com/rofl0r/microsocks) SOCKS5
forwarding proxy used by the Tier 3 SOCKS test
(`tests/integration/test_integration_socks.c`).

The test drives GtkHx's production connect path (the hxnet orchestrator,
via `hxnet_connection_open_plaintext_polling`) *through* this proxy to the
mhxd container, so `resolve_and_connect`'s proxy branch + `tokio-socks`
run end to end against a real SOCKS5 server.

It passes the proxy URI to the FFI directly rather than going through
`src/hxnet_bridge.c`'s `GProxyResolver` lookup — that C plumbing
(`bridge_lookup_socks_proxy`) is the production source of the URI but is
not exercised by this test.

```sh
docker build -t gtkhx-socks tests/socks-proxy
# host networking so the proxy reaches mhxd's published 127.0.0.1:5500
docker run -d --name gtkhx-socks --network host gtkhx-socks
GTKHX_TEST_SOCKS=socks5://127.0.0.1:1080 \
GTKHX_TEST_HOST=127.0.0.1 GTKHX_TEST_PORT=5500 \
  meson test -C build --suite integration
```

The proxy is no-auth (the test connects with a credential-less
`socks5://` URI) and forwards each CONNECT to the target the client
names — here mhxd. microsocks is pinned to a release tag in the
Dockerfile (`MICROSOCKS_REV`); bump that line for a newer version.

The test also runs a negative control against a dead proxy
(`socks5://127.0.0.1:1`): because mhxd is directly reachable in the
matrix, a connection that ignored the proxy and went direct would
wrongly succeed, so requiring failure there proves the proxy is on the
path. This stands in for a blocked-direct-egress netns.
