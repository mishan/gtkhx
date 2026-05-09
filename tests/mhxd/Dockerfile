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
        libncurses-dev \
    && rm -rf /var/lib/apt/lists/*

# Pull mhxd master and build with the feature set GtkHx exercises:
#
#   --enable-hxd          — the actual server binary. Despite the help
#                           text saying --disable-hxd, the default is
#                           OFF: there's an auto-enable-if-nothing-else
#                           in configure.in, but it's defeated as soon
#                           as we pass --enable-hxtrackd. Explicit is
#                           safer than implicit, so we pass it. This
#                           ALSO trips the configure.in:779 guard that
#                           adds -I../cipher to COMMONINCLUDES, which
#                           is what src/common needs to find cipher.h.
#                           (--enable-hx would trip the same guard but
#                           pulls in a libreadline build dep we don't
#                           need; we only want the server here.)
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
# Without --enable-hxd, configure leaves the `-I../cipher`
# substitution out of COMMONINCLUDES, and src/common fails to compile
# with `cipher.h: No such file or directory`. The guard at
# configure.in:779 reads
#   if test $enable_hxd = yes -o $enable_hx = yes; then
#     ... COMMONINCLUDES="$COMMONINCLUDES -I../cipher"
# Either alone is sufficient; --enable-hxd is what we actually want
# anyway since it builds the server we run as the test target.
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
        --enable-hxd \
        --with-pthreads \
        --enable-hope \
        --enable-cipher \
        --enable-compress \
#        --enable-ipv6 \
        --enable-hxtrackd \
        --enable-hldump \
    && (make -j"$(nproc)" || make) \
    && mkdir -p /opt/mhxd \
    && make install

# Runtime config skeleton. mhxd wants a directory with hxd.conf,
# accounts/, news/, banlist, etc. Copy the upstream `run/hxd/` tree
# into /opt/mhxd/run as the seed, then patch the bits that bite a
# containerised test environment:
#
# (1) ident 1 → ident 0
#     mhxd's default config does an identd lookup against every
#     connecting client BEFORE sending HTLS_MAGIC (see
#     src/hxd/hlserver.c:312, `if (hxd_cfg.options.ident) start_ident
#     (htlc); else hxd_fd_set(s, FDR);`). In a Docker container
#     with no ident server reachable, this hangs the handshake
#     until the ident query times out (several seconds), and even
#     then subsequent connections behave non-deterministically.
#     The conf file itself acknowledges this: `# disable if you
#     have problems with firewalls`. Turn it off.
#
# (2) version 0 → version 185
#     The version=0 path in mhxd's rcv_login suppresses
#     HTLS_HDR_USER_SELFINFO entirely (see src/hxd/rcv.c:133,
#     gated on `if (hxd_cfg.options.version != 0)`), and the
#     loginreply degenerates to a single-chunk TASK with just our
#     UID. GtkHx — and our Tier 3 integration tests — expect the
#     modern flow: loginreply with uid+version+banner+name, then
#     AGREEMENT, then SELFINFO. Switching to 185 (act as Hotline
#     1.8.5) makes mhxd take that path and lets the
#     test_login.c assertions actually fire.
#
# (3) reconn_time 2 → reconn_time 0   (and conn_max 5 → 100)
#     `nospam` is enabled by default. Its reconn_time setting
#     drops any connection from the same address within 2 seconds
#     of the previous one. Our integration tests fire connections
#     from 127.0.0.1 milliseconds apart (handshake/smoke followed
#     by handshake/two_connections, plus parallel test runs); the
#     second connection always gets dropped before it completes
#     the handshake. Turn the per-IP reconnect cooldown off, and
#     bump the per-IP concurrent-connection cap so parallel test
#     runs aren't capped out at 5.
RUN cp -a /build/mhxd/run/hxd /opt/mhxd/run \
    && sed -i 's/^\([[:space:]]*\)ident 1;/\1ident 0;/' \
        /opt/mhxd/run/hxd.conf \
    && sed -i 's/^\([[:space:]]*\)version 0;/\1version 185;/' \
        /opt/mhxd/run/hxd.conf \
    && sed -i 's/^\([[:space:]]*\)reconn_time 2;/\1reconn_time 0;/' \
        /opt/mhxd/run/hxd.conf \
    && sed -i 's/^\([[:space:]]*\)conn_max 5;/\1conn_max 100;/' \
        /opt/mhxd/run/hxd.conf

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
CMD ["/opt/mhxd/run/bin/hxd"]
