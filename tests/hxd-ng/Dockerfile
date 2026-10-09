# syntax=docker/dockerfile:1
#
# hxd-ng for the GtkHx Tier 3 rig.
#
# hxd-ng (https://github.com/mishan/hxd-ng) implements the video
# extension, and every video test runs against it as well as Janus. The
# server itself comes from the image hxd-ng's own CI publishes to GHCR
# for every commit on main, so nothing compiles here;
# this layer adds only the rig's config, accounts and entrypoint. Pinned
# by commit tag and digest, like the other servers are pinned: an
# unpinned main has broken this rig before. Bump both deliberately, with
# the tests run against the new one.
FROM ghcr.io/mishan/hxd-ng:sha-6dc8be3@sha256:87fe6ee6f0ad99cfab58c94002aa21b72b3985957f5243561e3795b609a6ad6c

# The base image runs as the unprivileged hxd user (10001), which owns
# /var/lib/hxd-ng and /run/hxd-ng; the accounts go in owned by it too.
COPY conf/hxd-ng.toml /etc/hxd-ng/hxd-ng.toml
COPY --chown=10001:10001 conf/accounts /var/lib/hxd-ng/accounts
COPY --chown=10001:10001 conf/files /srv/hxd-ng/files
# The folder tests upload here; git keeps no empty folder.
RUN mkdir /srv/hxd-ng/files/Uploads
COPY --chmod=0755 entrypoint.sh /usr/local/bin/hxd-ng-entrypoint
# The file-mode banner the HOPE banner tests fetch over HTXF.
ADD --chown=10001:10001 "https://placehold.co/468x60.gif?text=GtkHx+HTXF+GIF" /etc/hxd-ng/banner.gif
HEALTHCHECK --interval=10s --timeout=5s --start-period=10s --retries=3 \
    CMD ["bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/5520"]
ENTRYPOINT ["hxd-ng-entrypoint"]
CMD []
