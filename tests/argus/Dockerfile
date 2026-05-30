# Argus test tracker for GtkHx integration / Tier 3 testing.
#
# Argus is VesperNet's closed-source Hotline tracker — same family as
# Janus (see tests/janus/), single statically-linked Go binary,
# downloadable as a Linux amd64 tarball from get.vespernet.net.
#
# Why we want it as a Tier 3 target:
#   - **Only public v3-capable tracker we have access to.** Speaks v1
#     + v2 + v3 simultaneously on the same TCP port with automatic
#     version detection. Mock alternatives would be busywork; Argus
#     is the real thing.
#   - Operationally clean: single binary, YAML config, no auth needed,
#     no TLS to wire up for the listing path.
#   - **Promoted-servers config** lets us seed deterministic content
#     into every listing without running an actual Hotline server
#     that registers via UDP. Three entries baked into our config
#     yield three records in every listing, in deterministic order,
#     forever. No background scrape, no flake.
#
# Note: Argus emits ALL promoted_servers entries as v3 0x48 hostname
# records on the wire, even when the YAML address is a literal IP.
# The client parser handles all three address types cleanly; the
# Phase A view-emit path drops non-IPv4 records. Tier 3 tests
# therefore assert at the parser layer. See README.md.
#
# Build:    docker build -t gtkhx-argus tests/argus
# Run:      docker run --rm -p 5498:5498 -p 5499:5499/udp gtkhx-argus
# Connect:  point GtkHx tracker fetcher at localhost:5498 (the
#           default tracker port), or set in Settings → Tracker.
#
# The binary is not redistributed — the Dockerfile pulls it fresh at
# image-build time and sha256-verifies. Same legal posture as Janus.

FROM debian:bookworm-slim AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install --no-install-recommends -y \
        ca-certificates \
        curl \
    && rm -rf /var/lib/apt/lists/*

# Pinned Argus build (current as of May 2026). sha256 verified
# against the tarball Misha pulled from get.vespernet.net on
# 2026-05-29 — any change at the URL produces a checksum mismatch
# here and we bump the SHA deliberately, same model as Janus.

#ARG ARGUS_URL=https://get.vespernet.net/argus-linux-amd64.tar.gz
# Use tarball hosted on misha.nasledov.com for now as get.vespernet.net responds with HTTP 403 when run on GitHub Actions
ARG ARGUS_URL=https://misha.nasledov.com/argus-linux-amd64.tar.gz
ARG ARGUS_SHA256=8759cb59383c2b1bb7f210ea3bf47912f4cca6f9e46c1b28974f01937eed5628

# get.vespernet.net 403s curl from GitHub Actions runner IP ranges
# consistently (succeeds from desktops, fails from cloud egress
# pools). The 403 isn't transient — retry can't fix it — and we
# only pull one file from this host, so curl --retry is pure
# wasted CI time. The one cheap probe worth trying is a real-
# browser User-Agent, in case vespernet's filter is UA-based
# (some CDN rules short-circuit curl's default UA to 403). If
# that still fails on GHA the proven mitigation is mirroring at
# a friendly host — that's the route Janus took (see
# tests/janus/Dockerfile preamble).
WORKDIR /build
RUN curl -fsSL \
        -A "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36" \
        "$ARGUS_URL" -o argus.tar.gz \
 && echo "$ARGUS_SHA256  argus.tar.gz" | sha256sum -c - \
 && tar xzf argus.tar.gz \
 && mv argus-linux-amd64 /opt/argus

# Replace the upstream config wholesale with our checked-in copy.
# What we changed from upstream and why:
#
#   listen.bind_address: 0.0.0.0       — explicit (upstream's "" also
#                                         binds all interfaces; we
#                                         spell it out).
#   registration.reject_private_ips: false
#                                      — Docker bridge IPs are RFC
#                                         1918; the default true
#                                         would reject any UDP
#                                         registration from inside
#                                         the test rig.
#   logging.level: debug               — per-connection v3 negotiation
#                                         lines aid Tier 3
#                                         diagnostics.
#   promoted_servers: [3 pinned]       — deterministic listing content
#                                         without needing a real
#                                         Hotline server to register.
#
# Auth, federation, API, Mnemosyne all left disabled.
COPY conf/config.yaml /opt/argus/config.yaml

# -----------------------------------------------------------------------------
# Runtime stage. Argus is statically linked Go (`file argus` reports
# "statically linked, Go BuildID=..."), so we don't need any runtime
# libs beyond ca-certificates.
# -----------------------------------------------------------------------------
FROM debian:bookworm-slim AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install --no-install-recommends -y \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --home-dir /var/lib/argus \
               --shell /usr/sbin/nologin argus

COPY --from=build /opt/argus /opt/argus
RUN chown -R argus:argus /opt/argus

# Tracker ports:
#   5498/tcp — client listing requests (HTRK over TCP — what GtkHx
#              speaks against; v1/v2/v3 all multiplex here)
#   5499/udp — server registration heartbeats (we don't drive this
#              in tests today, but expose it so a future
#              registration-round-trip test doesn't need an image
#              rebuild)
EXPOSE 5498/tcp 5499/udp

USER argus
WORKDIR /opt/argus

# Argus has no required CLI args. It looks for config.yaml,
# accounts.yaml, bans.txt, and federation.key in the directory
# containing the binary — our COPY placed them at /opt/argus/.
# Logs go to stdout (JSON in non-TTY environments like Docker), so
# `docker logs` works without -d.
ENTRYPOINT ["/opt/argus/argus"]
CMD []
