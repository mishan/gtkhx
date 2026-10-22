# The environment the README and AppStream screenshots are taken in.
#
# Everything that decides a pixel is pinned here, so the same scenes give
# the same PNGs on any machine: the toolkit, the fonts and the renderer
# (the CI base image, by digest), the Hotline servers the scenes connect
# to (by digest and revision), and the capture tools, shotbox among them
# (by version and hash). GtkHx itself is built from the working tree
# inside this image; see tools/screenshots.sh.
#
# Bumping a digest here changes pictures, and so can bumping shotbox's
# version: treat it the same way. Regenerate them in the same commit.

ARG CI_BASE=ghcr.io/mishan/gtkhx-ci-base@sha256:be4942d9de06a79319a4c7528cf0115658a8dba7218ccc4a4933068ae130fecd
ARG JANUS=ghcr.io/mishan/janus@sha256:8afddb00c8b22965db0cc8fc09657eff01c7767aa829d64c190525188859b654
ARG HXD_NG=gtkhx-hxd-ng

FROM ${JANUS} AS janus
FROM ${HXD_NG} AS hxd-ng

FROM ${CI_BASE}
# ImageMagick and xwininfo for shotbox's pictures; PyGObject for its
# accessibility reader (libatspi and at-spi2-core are in the base); sqlite
# to seed the news and chat history; procps for the scene runner's process
# checks.
RUN dnf -y install --setopt=tsflags=nodocs \
        ImageMagick xwininfo sqlite procps-ng python3-pip python3-gobject \
    && dnf clean all \
    && rm -rf /var/cache/dnf
# shotbox takes the pictures, drives the display and reads the accessible
# tree. Its wheel declares no dependencies (PyGObject, for the tree, comes
# from dnf above), so pip's system install (into /usr/local; Fedora doesn't
# mark its Python externally managed) has nothing else to bring in.
RUN printf 'shotbox==0.9.0 --hash=sha256:%s\n' \
        9cbde77bc26401cd6563b1a4aa7cd6f53cbad0db9263c4e4a278222c80c591ef \
        >/tmp/shotbox.txt \
    && python3 -m pip install --no-cache-dir --require-hashes -r /tmp/shotbox.txt \
    && rm /tmp/shotbox.txt
COPY --from=janus /opt/janus /opt/janus
COPY --from=hxd-ng /usr/local/bin/hxd /usr/local/bin/hxd
