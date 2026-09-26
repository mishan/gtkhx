# syntax=docker/dockerfile:1
#
# hxd-ng for the GtkHx Tier 3 rig.
#
# hxd-ng (https://github.com/mishan/hxd-ng) is the only Hotline server
# that implements the video extension, so it is the target for every
# video test. The server itself comes from the image hxd-ng's own CI
# publishes to GHCR for every commit on main, so nothing compiles here;
# this layer adds only the rig's config, accounts and entrypoint. Pinned
# by commit tag and digest, like the other servers are pinned: an
# unpinned main has broken this rig before. Bump both deliberately, with
# the tests run against the new one.
FROM ghcr.io/mishan/hxd-ng:sha-cba901b@sha256:8b4b76a86b63d45285051e0efcf8293a97e28ac99b36ea98599cae1caf435a5b

# The base image runs as the unprivileged hxd user (10001), which owns
# /var/lib/hxd-ng and /run/hxd-ng; the accounts go in owned by it too.
COPY conf/hxd-ng.toml /etc/hxd-ng/hxd-ng.toml
COPY --chown=10001:10001 conf/accounts /var/lib/hxd-ng/accounts
COPY --chmod=0755 entrypoint.sh /usr/local/bin/hxd-ng-entrypoint
HEALTHCHECK --interval=10s --timeout=5s --start-period=10s --retries=3 \
    CMD ["bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/5520"]
ENTRYPOINT ["hxd-ng-entrypoint"]
CMD []
