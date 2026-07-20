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
# Run:      docker run --rm -p 5698:5698 -p 5699:5699/udp gtkhx-argus
# Connect:  point GtkHx tracker fetcher at localhost:5698 (the
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

# stunnel + openssl land here for the Phase D TLS test target.
# Argus itself has no native TLS support (its config.yaml has no
# tls/ssl/cert section — verified against the upstream 1.0.2
# configuration.md included in the tarball). To exercise the
# tracker client's TLS-first path against a real cert + handshake
# we wrap Argus with stunnel:
#
#   stunnel terminates TLS on 0.0.0.0:6498, forwards to argus on
#   127.0.0.1:5698 (the same plain port the existing Tier 3 test
#   already uses). One container, two ports — plain on 5698, TLS
#   on 6498 — so the existing test stays unchanged and the new
#   Phase D test points at 6498.
#
# Cert: generated at image-build time, self-signed, SHA-256
# fingerprint is captured into /opt/argus/tls.fingerprint so the
# Tier 3 harness can pre-pin it via GTKHX_KNOWN_HOSTS rather than
# prompting. The Tier 3 test today goes the simpler route of
# inline-accepting any cert (no GMainLoop to drive the production
# TOFU prompt), so the fingerprint file is informational for now.
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install --no-install-recommends -y \
        ca-certificates \
        openssl \
        stunnel4 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --home-dir /var/lib/argus \
               --shell /usr/sbin/nologin argus

COPY --from=build /opt/argus /opt/argus
RUN chown -R argus:argus /opt/argus

# Generate the self-signed cert for the stunnel TLS wrapper. CN =
# the docker-compose / test-host hostname; -nodes makes the key
# unencrypted (we'd otherwise need a passphrase pipe into
# stunnel). 10-year validity so a rebuild is the only reason the
# cert would change. The fingerprint file is what the Tier 3
# test reads to pre-pin trust.
RUN openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout /opt/argus/tls.key \
        -out    /opt/argus/tls.crt \
        -days 3650 \
        -subj "/CN=gtkhx-argus/O=GtkHx Test" \
    && openssl x509 -in /opt/argus/tls.crt -outform DER \
        | openssl dgst -sha256 -binary \
        | xxd -p -c 256 \
        | awk '{print "sha256:" $0}' \
        > /opt/argus/tls.fingerprint \
    && cat /opt/argus/tls.fingerprint \
    && chmod 0640 /opt/argus/tls.key \
    && chown argus:argus /opt/argus/tls.key /opt/argus/tls.crt \
        /opt/argus/tls.fingerprint

COPY conf/stunnel.conf /etc/stunnel/stunnel.conf
COPY entrypoint.sh /opt/argus/entrypoint.sh
RUN chmod +x /opt/argus/entrypoint.sh

# Tracker ports:
#   5698/tcp — client listing requests over plain TCP (HTRK over
#              TCP — what GtkHx speaks against; v1/v2/v3 all
#              multiplex here)
#   6498/tcp — TLS-wrapped client listing requests (stunnel
#              terminates TLS here and forwards to 127.0.0.1:5698).
#   Note: the plain listener is 5698 (not the conventional 5498) so it
#   doesn't collide with hxtrackd's hardcoded 5498 under host networking.
#   5699/udp — server registration heartbeats (not driven by tests
#              today)
EXPOSE 5698/tcp 6498/tcp 5699/udp

WORKDIR /opt/argus

# The entrypoint launches argus as the argus user under stunnel
# (which runs as root briefly to bind the privileged-ish port,
# then drops). Both processes log to stdout so `docker logs`
# captures the merged stream. argus' SIGINT propagates through
# the shell wrapper so `docker stop` shuts down cleanly.
ENTRYPOINT ["/opt/argus/entrypoint.sh"]
CMD []
