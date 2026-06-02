# mhxd's hxtrackd as a Tier 3 test target — the canonical pre-spec
# v1 Hotline tracker, the kind of target the probe-then-fallback
# code path in network.c needs to keep working against.
#
# Why we want it as a Tier 3 target:
#   - The default tracker most users connect to (hltracker.com,
#     hxd-derived community trackers, mhxd's own bundled tracker)
#     speaks v1 and ONLY v1, with no spec-compliant 6-byte read
#     for v3 magic. The Phase A code assumed otherwise and
#     produced empty listings against every real v1 tracker —
#     this container is the regression net so that bug can't
#     recur silently.
#   - mhxd's bundled hxtrackd is the cleanest reference v1
#     implementation we have. Built from the same upstream tree
#     as the mhxd container next to it; we just compile + run
#     the hxtrackd binary instead of hxd.
#
# Build:    docker build -t gtkhx-hxtrackd tests/hxtrackd
# Run:      docker run --rm -p 5598:5498 -p 5599:5499/udp gtkhx-hxtrackd
# Connect:  point GtkHx tracker fetcher at localhost:5598
#
# Host port mapping: 5598/5599 (not 5498/5499) because Argus
# already claims the conventional tracker ports in the Compose
# matrix. The tracker_matrix.c entry for "hxtrackd" hard-codes
# 5598 as the host-side port.

FROM debian:bookworm-slim AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install --no-install-recommends -y \
        ca-certificates \
        git \
        build-essential \
        autoconf \
        automake \
        libtool \
        pkg-config \
        libssl-dev \
        zlib1g-dev \
        libpcre2-dev \
        libncurses-dev \
    && rm -rf /var/lib/apt/lists/*

# Pull mhxd at a pinned revision, kept in lockstep with
# tests/mhxd/Dockerfile (MHXD_REV). Both used to clone master
# unpinned; that let upstream churn break the build (a merged patch
# stopped applying) and let a stale local clone-layer cache diverge
# from CI. Bump both Dockerfiles together when you want a newer mhxd.
WORKDIR /build
ARG MHXD_REV=c16f038452bbb406b226bcdd5a5510b26ec526c0
RUN git clone https://github.com/kangsterizer/mhxd.git mhxd \
    && git -C mhxd checkout --quiet "$MHXD_REV"

# Same configure feature set as tests/mhxd/, with the same
# rationale (see tests/mhxd/Dockerfile for the per-flag comments).
# The point of building all of these is mostly that mhxd's
# autotools graph defaults to "auto-enable hxd" when no other
# subprogram is asked for, and we want a clean explicit set.
#
# --enable-hope is REQUIRED whenever --enable-cipher is set
# (configure.in:476 gates the cipher subsystem on hope). The
# tracker itself doesn't actually USE cipher/hope at runtime,
# but configure rejects --enable-cipher without it, and dropping
# --enable-cipher breaks src/common's compile path (it pulls
# cipher.h via include surface even though no cipher symbols
# are reached from the tracker). Easiest stable path: enable
# them all, same as the tests/mhxd/ Dockerfile.
RUN cd /build/mhxd \
    && ./autogen.sh \
    && ./configure \
        --prefix=/opt/hxtrackd \
        --enable-hxd \
        --with-pthreads \
        --enable-hope \
        --enable-cipher \
        --enable-compress \
        --enable-hxtrackd \
        --disable-exec \
    && (make -j"$(nproc)" || make) \
    && mkdir -p /opt/hxtrackd \
    && make install

# Lay down the runtime tree. Upstream's run/hxtrackd skeleton has
# hxtrackd.conf + tracker_banlist; we overlay our own copy of
# hxtrackd.conf with the timing knobs tuned for the test rig.
RUN cp -a /build/mhxd/run/hxtrackd /opt/hxtrackd/run
COPY conf/hxtrackd.conf /opt/hxtrackd/run/hxtrackd.conf

# -----------------------------------------------------------------------------
# Runtime stage. hxtrackd + python3 (for the seed-registration
# helper that primes the tracker with deterministic test content).
# -----------------------------------------------------------------------------
FROM debian:bookworm-slim AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install --no-install-recommends -y \
        libssl3 \
        zlib1g \
        libpcre2-8-0 \
        python3-minimal \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --home-dir /var/lib/hxtrackd \
               --shell /usr/sbin/nologin hxtrackd

COPY --from=build /opt/hxtrackd /opt/hxtrackd
RUN chown -R hxtrackd:hxtrackd /opt/hxtrackd/run

# Entrypoint script: start hxtrackd, send a seed UDP registration
# from localhost so the tracker has at least one deterministic
# server to serve, then loop sending heartbeats every (interval/2)
# seconds so the entry never expires.
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
COPY seed-tracker.py /usr/local/bin/seed-tracker.py
RUN chmod 755 /usr/local/bin/docker-entrypoint.sh \
              /usr/local/bin/seed-tracker.py

# Tracker ports — the conventional 5498/tcp client-listing port +
# 5499/udp server-registration port. Container-internal; host-side
# mapping (5598/5599) is set at `docker run` time so Argus on
# 5498/5499 doesn't collide in the multi-target rig.
EXPOSE 5498/tcp 5499/udp

USER hxtrackd
WORKDIR /opt/hxtrackd/run

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD []
