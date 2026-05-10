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
        --disable-exec \
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
# (3) operation.nospam yes → operation.nospam no
#     The default config enables nospam, whose check_connections()
#     in src/hxd/spam.c rejects any connection from an IP that
#     already has an entry in the recent-connections table.
#     reconn_time was the obvious knob to adjust, but the
#     implementation has a quirk:
#         while (tv_secdiff(&ct[i].tv, &now) > reconn_time) i++;
#     With reconn_time=0 the cleanup never runs (`0 > 0` is false),
#
# (4) Seed files/test.txt and files/integration_seed.txt with
#     known content. mhxd's run/hxd/ skeleton ships without a
#     files/ directory, so the file_list test sees a task-error
#     (ENOENT) and the file_get test wouldn't have any target.
#     Two tiny text files cover both flows.
#     so the table holds onto every connection forever and every
#     second connection from the same IP gets rejected.
#     The integration suite fires several connections per second
#     from 127.0.0.1, which trips this on every test after the
#     first. Disable nospam outright via the operation section —
#     the check at hlserver.c:401 short-circuits when nospam=no
#     and the spam.* settings stop mattering.
#
# (5) NOTE: --disable-exec at configure time (above) removes mhxd's
#     CONFIG_EXEC code path entirely — both the rcv_login luser
#     hook (mhxd/src/hxd/rcv.c:161) and the chat-driven shell
#     command paths (commands.c:915, 984). An earlier attempt
#     (commit 34803b1) at fixing this from the config side by
#     sed-ing paths.luser to "" did NOT work, because mhxd's
#     cmd_exec function forks unconditionally — its strlen(cmdpath)
#     > 0 guard only gates the LOGGING block, not the fork+execve
#     itself. So passing an empty path still forked, the execve
#     resolved to "./exec/(null)", failed, child _exit(1)'d, and
#     [hxd] <defunct> piled up exactly like before. Compiling out
#     CONFIG_EXEC is the only way to actually skip the fork.
RUN cp -a /build/mhxd/run/hxd /opt/mhxd/run \
    && sed -i 's/^\([[:space:]]*\)ident 1;/\1ident 0;/' \
        /opt/mhxd/run/hxd.conf \
    && sed -i 's/^\([[:space:]]*\)version 0;/\1version 185;/' \
        /opt/mhxd/run/hxd.conf \
    && sed -i 's/^\([[:space:]]*\)nospam yes;/\1nospam no;/' \
        /opt/mhxd/run/hxd.conf \
    && mkdir -p /opt/mhxd/run/files \
    && printf 'hello world\n' > /opt/mhxd/run/files/test.txt \
    && printf 'tier-3 integration suite test data\n' \
        > /opt/mhxd/run/files/integration_seed.txt \
    && mkdir -p /opt/mhxd/run/files/Uploads

# (6) Lock down guest's file-mutation bits in the binary UserData
#
#     mhxd's struct hl_user_data starts with a 4-byte magic followed
#     by struct hl_access_bits (8 bytes of bitfields). On little-
#     endian hosts (mhxd's only target in practice), byte 4 of
#     UserData is the first 8 access bits in this order:
#
#       bit 0: rename_folders
#       bit 1: delete_folders
#       bit 2: create_folders
#       bit 3: move_files
#       bit 4: rename_files
#       bit 5: download_files
#       bit 6: upload_files
#       bit 7: delete_files
#
#     The upstream guest UserData has all of these set, which lets
#     a logged-in guest run HTLC_HDR_FILE_MKDIR / FILE_DELETE /
#     FILE_MOVE etc. — that's not the security posture a real
#     server would ship, and it lets a privileged-opcode test pass
#     for the wrong reason ('mkdir succeeded for guest' instead of
#     'mkdir was rejected because guest lacks the bit').
#
#     Mask byte 4 with 0x60 = 0b01100000: keep only download_files
#     (bit 5) and upload_files (bit 6). file_get and file_put need
#     those for their existing tests; everything else gets cleared.
#
#     Done with dd + a tiny shell pipeline so we don't need to
#     install python or perl into the build stage. Read byte 4 via
#     `dd | od`, mask in shell arithmetic, format the masked result
#     as a printf octal escape (\NNN) and write back via dd
#     seek+notrunc.
#
#     Why octal not hex: dash's printf (the /bin/sh on Debian) does
#     NOT interpret '\xHH' escapes — it writes the literal text
#     '\x20' instead of the byte 0x20. POSIX printf is only
#     guaranteed to handle '\NNN' octal. The double-printf trick
#     below builds a format string '\NNN' from the masked decimal
#     byte and feeds it to a second printf that emits the actual
#     byte.
#
#     Backslash count matters: the inner printf format is in
#     SINGLE quotes (preserves literally), so we want exactly two
#     backslashes — '\\' — feeding into printf, which collapses to
#     a single backslash on output, giving '\NNN'. Four backslashes
#     would output '\\NNN' instead, and the outer printf would
#     then emit a literal '\' followed by digits. (Spent a build
#     fighting that.) Verified against both the upstream guest
#     seed and an all-bits-set synthetic input.
RUN b=$(dd if=/opt/mhxd/run/accounts/guest/UserData \
            bs=1 count=1 skip=4 status=none \
          | od -An -vtu1 | tr -d ' ') \
 && m=$(( b & 0x60 )) \
 && fmt=$(printf '\\%03o' "$m") \
 && printf "$fmt" \
      | dd of=/opt/mhxd/run/accounts/guest/UserData \
            bs=1 count=1 seek=4 conv=notrunc status=none

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
