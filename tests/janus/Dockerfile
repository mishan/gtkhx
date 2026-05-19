# Janus test server for GtkHx integration / smoke testing.
#
# Janus is VesperNet's closed-source Hotline server — single-binary
# Go executable, ~24 MB unpacked, statically linked. The Linux amd64
# tarball lives at get.vespernet.net.
#
# Why we want it as a test target:
#   - **Only public server that ships the chat-history extension.**
#     SQLite-backed, cursor pagination, exactly the fogWraith spec
#     GtkHx Phase 5 will implement.
#   - Implements native TLS on a separate port (cert-pinning tests
#     once we land TLS support).
#   - Implements large-file (>4 GiB) transfers with capability
#     negotiation matching what we send today.
#   - Generally the richest feature set of any Hotline server we
#     have access to.
#
# Build:    docker build -t gtkhx-janus tests/janus
# Run:      docker run --rm -p 5510:5500 -p 5511:5501 gtkhx-janus
# Connect:  point GtkHx at localhost:5510
#
# Note the host-side port mapping (5510:5500): mhxd is conventionally
# on host port 5500, so when both containers run side-by-side via
# Compose, Janus answers on 5510.
#
# The binary is not redistributed — the Dockerfile pulls it fresh
# at image-build time and sha256-verifies. Same legal posture as the
# placehold.co banner fetches in tests/mhxd/.

FROM debian:bookworm-slim AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install --no-install-recommends -y \
        ca-certificates \
        curl \
    && rm -rf /var/lib/apt/lists/*

# Pinned Janus 2.0.7 (commit a4046cf, May 13 2026). The upstream URL
# carries no version suffix — agora.vespernet.net always serves the
# current build — so we pin by sha256 instead of version string. Any
# change at the URL produces a checksum mismatch here and we bump
# deliberately.
ARG JANUS_URL=https://get.vespernet.net/janus-linux-amd64.tar.gz
ARG JANUS_SHA256=2a2a2aedf98ea941b1f26ea190bec3eb4c5213aeecdb794fa3dc543d0103373f

WORKDIR /build
RUN curl -fsSL "$JANUS_URL" -o janus.tar.gz \
 && echo "$JANUS_SHA256  janus.tar.gz" | sha256sum -c - \
 && tar xzf janus.tar.gz \
 && mv janus-linux-amd64 /opt/janus

# Tweaks to the upstream config. We keep most defaults — Janus's
# config.yaml is well-commented and the defaults are sensible for a
# fresh install — and only flip the bits the integration tests need:
#
#   - ChatHistoryEnabled: enable the SQLite-backed chat history that
#     is the headline reason we're using Janus as a test target.
#   - Bind to all interfaces (Interface: "") is already the default;
#     no change.
#   - Tracker registration stays off so this container doesn't
#     accidentally announce itself.
#
# HOPE and TLS are intentionally LEFT OFF for the first revision:
#   - HOPE requires re-setting account passwords so Janus can derive
#     the HOPE hash; the upstream guest/admin yamls don't ship with
#     HOPE-compatible hashes. Worth doing in a follow-up.
#   - TLS needs a generated cert + key; the Extras/generate_cert.sh
#     dance is straightforward but adds runtime config we haven't
#     audited yet.
RUN sed -i 's|^ChatHistoryEnabled: false|ChatHistoryEnabled: true|' \
        /opt/janus/Server/config.yaml \
 && grep -n -E '^ChatHistoryEnabled|^EnableHOPE|^TLSCert' \
        /opt/janus/Server/config.yaml

# Seed a deterministic content tree so integration tests don't have
# to assume what's in Files/. Mirrors the mhxd container seeding.
RUN mkdir -p /opt/janus/Server/Files \
 && printf 'hello world\n' \
        > /opt/janus/Server/Files/test.txt \
 && printf 'tier-3 integration suite test data\n' \
        > /opt/janus/Server/Files/integration_seed.txt \
 && mkdir -p /opt/janus/Server/Files/Uploads \
 && mkdir -p /opt/janus/Server/Files/test_folder \
 && printf 'folder-leaf-A\n' \
        > /opt/janus/Server/Files/test_folder/leaf_a.txt \
 && printf 'folder-leaf-B\n' \
        > /opt/janus/Server/Files/test_folder/leaf_b.txt

# -----------------------------------------------------------------------------
# Runtime stage. The Janus binary is statically linked Go (verified
# with `file`: "statically linked"), so we don't need any runtime
# libs beyond ca-certificates (for the optional outbound HTTPS to
# trackers and the NNTP gateway — neither used in tests, but Janus
# may still init the TLS subsystem at startup).
# -----------------------------------------------------------------------------
FROM debian:bookworm-slim AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install --no-install-recommends -y \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --home-dir /var/lib/janus \
               --shell /usr/sbin/nologin janus

COPY --from=build /opt/janus /opt/janus
RUN chown -R janus:janus /opt/janus

# Hotline ports:
#   5500/tcp — HTLS server (clients connect here)
#   5501/tcp — HTXF file transfer subchannel
#   5600/tcp — HTLS over TLS (reserved; TLS not enabled in v1)
#   5601/tcp — HTXF over TLS
EXPOSE 5500/tcp 5501/tcp 5600/tcp 5601/tcp

USER janus
WORKDIR /opt/janus

# Janus takes no required args. It looks for `Server/` next to the
# executable, which our COPY placed at /opt/janus/Server. Logs go to
# stdout/stderr so `docker logs` works without -d.
ENTRYPOINT ["/opt/janus/janus"]
CMD []
