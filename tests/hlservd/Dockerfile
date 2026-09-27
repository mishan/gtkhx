# GtkHx test overlay for hlservd.
#
# hlservd is the Hotline Server 1.9.5 as a headless POSIX daemon, ported
# from the Hotsprings 2003 GPL release: the rig's stand-in for the
# original 1.9 server. The base image (published from hotline-docker) pulls
# and verifies the binary; this layer adds the rig's settings (port 5530,
# clear of mhxd, Janus and hxd-ng on the shared host network, and the flood
# ban off for loopback) and a fixture file, so the root isn't empty.
#
# Requires the base image to be published + pullable (public, or CI logged
# in to GHCR). Pin BASE_TAG to a digest/tag for reproducibility.
ARG BASE_REGISTRY=ghcr.io/mishan
ARG BASE_TAG=latest
FROM ${BASE_REGISTRY}/hlservd:${BASE_TAG}

COPY --chown=hlservd:hlservd conf/Settings.ini /var/lib/hlservd/Settings.ini
COPY --chown=hlservd:hlservd files/ /var/lib/hlservd/Files/

HEALTHCHECK --interval=10s --timeout=5s --start-period=10s --retries=3 \
    CMD ["bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/5530"]
