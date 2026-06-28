# SOCKS5 proxy for GtkHx Tier 3 integration testing.
#
# Builds microsocks (rofl0r/microsocks) — a tiny, MIT-licensed,
# forwarding SOCKS5 server — and runs it on port 1080. The SOCKS Tier 3
# test (tests/integration/test_integration_socks.c) points the
# production hxnet connect path at this proxy and asks it to reach the
# mhxd container, exercising resolve_and_connect's proxy branch ->
# tokio-socks -> the proxy -> mhxd end to end.
#
# Build:  docker build -t gtkhx-socks tests/socks-proxy
# Run:    docker run --rm --network host gtkhx-socks
#         (host networking so the proxy can reach mhxd's published
#          127.0.0.1:5500 the same way the test runner does)
# Use:    GTKHX_TEST_SOCKS=socks5://127.0.0.1:1080
#
# microsocks is a no-auth forwarding proxy: it connects to whatever
# target the client's CONNECT request names (here 127.0.0.1:5500), which
# is exactly the "system has a SOCKS proxy" shape the client handles.
#
# Pinned to a release tag (MICROSOCKS_REV) for reproducibility — bump
# the line below for a newer microsocks. Same pinning rationale as
# tests/mhxd/Dockerfile.

FROM debian:bookworm-slim AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install --no-install-recommends -y \
        ca-certificates \
        git \
        build-essential \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
ARG MICROSOCKS_REV=v1.0.5
RUN git clone https://github.com/rofl0r/microsocks.git microsocks \
    && git -C microsocks checkout --quiet "$MICROSOCKS_REV" \
    && make -C microsocks \
    && install -m 0755 microsocks/microsocks /usr/local/bin/microsocks

# -----------------------------------------------------------------------------
FROM debian:bookworm-slim AS runtime

RUN useradd --system --home-dir /nonexistent --shell /usr/sbin/nologin socks
COPY --from=build /usr/local/bin/microsocks /usr/local/bin/microsocks

# SOCKS5 listen port.
EXPOSE 1080/tcp

USER socks

# -i 0.0.0.0: listen on all interfaces (under --network host this is the
# runner's loopback the test reaches at 127.0.0.1:1080). -p 1080: the
# conventional SOCKS port the test defaults to. No -1, so no auth is
# required — the test connects with a credential-less socks5:// URI.
ENTRYPOINT ["/usr/local/bin/microsocks", "-i", "0.0.0.0", "-p", "1080"]
