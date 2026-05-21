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
#   - **HOPE-Secure-Login + ChaCha20-Poly1305 AEAD.** The Dockerfile
#     seeds HOPE-compatible password hashes into the bundled
#     accounts at build time (see seed-hope-passwords.sh) — see
#     `What's enabled` in README.md for the gating details.
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

# Pinned Janus build (current as of May 2026). Upstream's public
# URL (https://get.vespernet.net/janus-linux-amd64.tar.gz, sha
# 2a2a2aedf98ea941b1f26ea190bec3eb4c5213aeecdb794fa3dc543d0103373f)
# rate-limits incoming connections aggressively, which the Tier 3
# suite trips. fogWraith provided a build with the rate limit
# disabled; Misha hosts it under misha.nasledov.com until upstream
# updates the public binary. Same sha256-pin model — any change at
# the URL produces a checksum mismatch here and we bump deliberately.
ARG JANUS_URL=https://misha.nasledov.com/janus-linux-amd64.tar.gz
ARG JANUS_SHA256=0a79f8c6fc724229a7f013ad227c285698027c1eb0eb569226012ba5b50670d4

WORKDIR /build
RUN curl -fsSL "$JANUS_URL" -o janus.tar.gz \
 && echo "$JANUS_SHA256  janus.tar.gz" | sha256sum -c - \
 && tar xzf janus.tar.gz \
 && mv janus-linux-amd64 /opt/janus

# Replace the upstream config wholesale with our checked-in copy.
# The checked-in conf/config.yaml encodes every test-targeted edit
# in one auditable place (vs. a stack of brittle in-place seds in
# previous revisions of this Dockerfile). What we changed from
# upstream and why:
#
#   ChatHistoryEnabled: true       — Janus is the chat-history
#                                    target.
#   EnableHOPE: true               — HOPE-Secure-Login on; the
#                                    seed script below populates
#                                    HOPEPassword fields onto the
#                                    bundled YAMLs so Step 2
#                                    actually validates.
#   APIAddr: ":8973"               — temporarily exposes the
#                                    admin REST API. Needed by
#                                    the seed step; left enabled
#                                    at runtime because this is
#                                    a localhost test container
#                                    and the key is in git
#                                    anyway.
#   APIKeys[0].Key: "build-seed-key"
#   MaxConnectionsPerIP: 0         — disable per-IP throttle (Tier
#                                    3 hammers 127.0.0.1 with many
#                                    short-lived connections).
#   ConnectionRateLimit: 0,
#   ConnectionRateBanThreshold: 0  — same, the other two limiter
#                                    knobs that aren't in upstream
#                                    config; the rate-limit-
#                                    disabled binary reads them
#                                    when present.
COPY conf/config.yaml /opt/janus/Server/config.yaml

# Seed HOPE-compatible password hashes onto the bundled accounts.
# Standalone script — see its preamble for the full background;
# short version: Janus stores bcrypt + a separately-encrypted
# HOPEPassword blob per account, and the only documented way to
# generate the HOPE half is by PATCH-ing the password through the
# admin API after the master key has been generated on first
# startup. The script launches Janus, PATCHes the passwords, and
# sanity-checks that HOPEPassword: ends up in both YAMLs before
# letting the build succeed.
COPY seed-hope-passwords.sh /usr/local/bin/seed-hope-passwords
RUN chmod +x /usr/local/bin/seed-hope-passwords \
 && /usr/local/bin/seed-hope-passwords

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
