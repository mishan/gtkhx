# mhxd test server for GtkHx integration / smoke testing.
#
# Builds a fresh mhxd from upstream master at image-build time, then
# overlays our checked-in config + patched user accounts so the
# container comes up in a known-good state. The set of customisations
# used to live as a sed/dd shell pipeline in this file (~200 LOC of
# shell and commentary); replacing those with concrete files in
# tests/mhxd/conf/ and tests/mhxd/patches/ keeps the source-of-truth
# auditable in git.
#
# Build:    docker build -t gtkhx-mhxd tests/mhxd
# Run:      docker run --rm -p 5500:5500 -p 5501:5501 gtkhx-mhxd
# Connect:  point GtkHx at localhost:5500
#
# The mhxd clone is unpinned (`--depth 1` master). If a future mhxd
# rev breaks our checked-in config (e.g. by adding a required section
# to hxd.conf), pin the clone to a specific commit here and bump
# deliberately.

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
        curl \
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

# Pull mhxd master and apply our local fixes.
#
# patches/folder-xfer-size.patch — fixes two copy/paste typos in
# folder_getpaths (htxf.c) and rcv_folder_get (files.c) that compute
# the per-file size formula incorrectly, breaking multi-file folder
# transfers when files have no resource fork. See the patch's commit
# message for the full root-cause writeup.
WORKDIR /build
RUN git clone --depth 1 https://github.com/kangsterizer/mhxd.git mhxd
COPY patches/folder-xfer-size.patch /build/patches/folder-xfer-size.patch
RUN cd /build/mhxd && git apply /build/patches/folder-xfer-size.patch

# Configure with the feature set GtkHx exercises:
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
#   --with-pthreads       — thread model (mhxd's default is fork/clone)
#   --enable-hope         — secure login handshake; GtkHx negotiates this
#   --enable-cipher       — Blowfish/RC4 link encryption
#   --enable-compress     — zlib link compression
#   --enable-hxtrackd     — tracker server (port 5498); compile but
#                           skip exposing by default
#   --enable-hldump       — server-side wire-protocol dump for
#                           debugging; mirrors GTKHX_DEBUG=proto.
#   --disable-exec        — compile out CONFIG_EXEC entirely.
#                           Otherwise mhxd's cmd_exec forks
#                           unconditionally on login and on certain
#                           chat commands, producing [hxd] <defunct>
#                           zombies. (Setting paths.luser="" in the
#                           config does NOT work — the fork happens
#                           before the empty-path guard.)
#
# `make -j$(nproc) || make` because mhxd's autotools graph isn't
# fully parallel-safe — fall back to single-job if the parallel run
# stumbled.
RUN cd /build/mhxd \
    && ./autogen.sh \
    && ./configure \
        --prefix=/opt/mhxd \
        --enable-hxd \
        --with-pthreads \
        --enable-hope \
        --enable-cipher \
        --enable-compress \
        --enable-hxtrackd \
        --enable-hldump \
        --disable-exec \
    && (make -j"$(nproc)" || make) \
    && mkdir -p /opt/mhxd \
    && make install

# Lay down the runtime config tree.
#
# 1. Copy the upstream `run/hxd/` skeleton as the base — gives us
#    accounts/, news/, banlist, MOTD, etc.
# 2. Overlay our checked-in conf/ tree on top:
#      - hxd.conf            — same as upstream except ident=0
#                              (no identd reachable in containers),
#                              version=185 (modern login-reply flow
#                              with SELFINFO, instead of the
#                              version=0 degraded path), and
#                              nospam=no (defeat reconn_time spam
#                              protection that rejects 2nd+
#                              connections from 127.0.0.1).
#      - accounts/guest/UserData — pre-modified binary blob with
#                              the right access bits: clears
#                              rename/delete/move/create/etc bits
#                              on byte 4 (only download + upload),
#                              sets UPLOAD_FOLDERS + DOWNLOAD_FOLDERS
#                              on byte 8. mhxd's HTLC dispatcher
#                              gates GETFOLDER/PUTFOLDER on those.
# 3. Seed files/test.txt and files/integration_seed.txt with known
#    content so file_list / file_get / folder_get have something
#    to find. Upstream `run/hxd/` ships without a files/ directory.
# 4. The banner-block fields (type/file/url) are still patched at
#    runtime via docker-entrypoint.sh so BANNER_MODE / BANNER_URL /
#    BANNER_FILE env vars remain configurable across container
#    starts.
RUN cp -a /build/mhxd/run/hxd /opt/mhxd/run
COPY conf/hxd.conf /opt/mhxd/run/hxd.conf
COPY conf/accounts/guest/UserData /opt/mhxd/run/accounts/guest/UserData
RUN mkdir -p /opt/mhxd/run/files \
    && printf 'hello world\n' > /opt/mhxd/run/files/test.txt \
    && printf 'tier-3 integration suite test data\n' \
        > /opt/mhxd/run/files/integration_seed.txt \
    && mkdir -p /opt/mhxd/run/files/Uploads \
    && mkdir -p /opt/mhxd/run/files/test_folder \
    && printf 'folder-leaf-A\n' \
        > /opt/mhxd/run/files/test_folder/leaf_a.txt \
    && printf 'folder-leaf-B\n' \
        > /opt/mhxd/run/files/test_folder/leaf_b.txt

# Seed default banner images for file-mode HTXF tests. Fetched
# from placehold.co at build time so the running container can
# serve them without needing internet access at runtime — and so
# the test fixture is deterministic across container starts.
# Two formats so BANNER_MODE=GIFf and BANNER_MODE=JPEG both work
# out of the box; users can override by mounting a custom file
# and setting BANNER_FILE.
RUN curl -fsSL --retry 3 \
        "https://placehold.co/468x60.jpg?text=GtkHx+HTXF+JPEG" \
        -o /opt/mhxd/run/banner.jpg \
 && curl -fsSL --retry 3 \
        "https://placehold.co/468x60.gif?text=GtkHx+HTXF+GIF" \
        -o /opt/mhxd/run/banner.gif

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

# Entrypoint script — patches hxd.conf's banner block based on
# the BANNER_MODE / BANNER_URL / BANNER_FILE env vars at startup,
# then execs hxd. With no env vars set, behaviour matches the
# URL-mode default committed in conf/hxd.conf.
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod 755 /usr/local/bin/docker-entrypoint.sh

# Hotline ports:
#   5498/tcp — HTRK tracker (only used if you launch hxtrackd)
#   5500/tcp — HTLS server (clients connect here)
#   5501/tcp — HTXF file transfer
EXPOSE 5498/tcp 5500/tcp 5501/tcp

USER mhxd
WORKDIR /opt/mhxd/run

# The entrypoint script patches hxd.conf in /opt/mhxd/run (which
# the mhxd user owns from the chown above) and exec's hxd in the
# foreground. We don't pass `-d` so stdout/stderr stay attached
# to `docker logs`.
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD []
