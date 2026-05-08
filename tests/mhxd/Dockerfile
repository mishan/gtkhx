# mhxd test server for GtkHx integration / smoke testing.
#
# Builds a fresh mhxd from upstream master at image-build time, sets up
# a minimal account file that the GtkHx test paths can connect to
# (guest, no password; admin, well-known password), and runs hxd in
# the foreground on port 5500.
#
# Build:    docker build -t gtkhx-mhxd tests/mhxd
# Run:      docker run --rm -p 5500:5500 -p 5501:5501 gtkhx-mhxd
# Connect:  point GtkHx at localhost:5500
#
# The container is intentionally rebuildable from upstream every time
# (no pinned commit) so this same Dockerfile gives us whatever mhxd's
# main branch offers today. If mhxd ever ships a release tag we want
# to pin to, swap the `git clone` for `git clone --branch <tag>`.

FROM debian:bookworm-slim AS build

# Build deps. Most are obvious; the less-obvious ones:
#   libssl-dev   — mhxd's --enable-cipher uses OpenSSL crypto for
#                  Blowfish / RC4 + the HOPE handshake.
#   libpcre2-dev — pulled in by some optional features; harmless if
#                  unused.
#   pkg-config   — autotools macros lean on it for SSL detection.
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
    && rm -rf /var/lib/apt/lists/*

# Pull mhxd master and build with the feature set GtkHx exercises:
#
#   --with-pthreads       — thread model (mhxd's default is fork/clone)
#   --enable-hope         — secure login handshake; GtkHx negotiates this
#   --enable-cipher       — Blowfish/RC4 link encryption
#   --enable-compress     — zlib link compression
#   --enable-ipv6         — getaddrinfo path; GtkHx is built with this
#   --enable-hxtrackd     — tracker server (port 5498); skip exposing
#                           by default but compile so it's available
#   --enable-hldump       — wire-protocol dump for debugging on the
#                           server side, mirrors what GtkHx's
#                           GTKHX_DEBUG=proto does on the client.
#
# `make -j$(nproc) || make` because mhxd's autotools graph isn't
# fully parallel-safe — fall back to single-job if the parallel run
# stumbled.
WORKDIR /build
RUN git clone --depth 1 https://github.com/kangsterizer/mhxd.git mhxd \
    && cd mhxd \
    && ./autogen.sh \
    && ./configure \
        --prefix=/opt/mhxd \
        --with-pthreads \
        --enable-hope \
        --enable-cipher \
        --enable-compress \
        --enable-ipv6 \
        --enable-hxtrackd \
        --enable-hldump \
    && make -j"$(nproc)" || make \
    && make install

# Runtime config skeleton. mhxd wants a directory with hxd.conf,
# accounts/, news/, banlist, etc. Copy the upstream `run/hxd/` tree
# into /opt/mhxd/run as the seed.
RUN cp -a /build/mhxd/run/hxd /opt/mhxd/run

# -----------------------------------------------------------------------------
# Runtime stage. Slim down to just the binary + runtime libs + config tree.
# -----------------------------------------------------------------------------
FROM debian:bookworm-slim AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install --no-install-recommends -y \
        libssl3 \
        zlib1g \
        libpcre2-8-0 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --home-dir /var/lib/mhxd \
               --shell /usr/sbin/nologin mhxd

# Bring the built binaries + the run/ skeleton from the build stage.
COPY --from=build /opt/mhxd /opt/mhxd
RUN chown -R mhxd:mhxd /opt/mhxd/run

# Hotline ports:
#   5498/tcp — HTRK tracker (only used if you launch hxtrackd)
#   5500/tcp — HTLS server (clients connect here)
#   5501/tcp — HTXF file transfer
EXPOSE 5498/tcp 5500/tcp 5501/tcp

USER mhxd
WORKDIR /opt/mhxd/run

# Run hxd in foreground (-d would daemonize; we want stdout in
# `docker logs`). The cwd holds hxd.conf so no -f flag needed.
CMD ["/opt/mhxd/sbin/hxd"]
