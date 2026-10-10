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
# ImageMagick and xwininfo for shotbox's pictures; sqlite to seed the news
# and chat history; procps for the scene runner's process checks.
RUN dnf -y install --setopt=tsflags=nodocs \
        ImageMagick xwininfo sqlite procps-ng python3-pip \
    && dnf clean all \
    && rm -rf /var/cache/dnf
# shotbox takes the pictures and drives the display. It is the standard
# library only, so pip's system install (into /usr/local; Fedora doesn't
# mark its Python externally managed) has nothing else to bring in.
RUN printf 'shotbox==0.3.0 --hash=sha256:%s\n' \
        86dab4b038c790c44d97a487acca927539d614973068ff6cff9cc4eebb151599 \
        >/tmp/shotbox.txt \
    && python3 -m pip install --no-cache-dir --require-hashes -r /tmp/shotbox.txt \
    && rm /tmp/shotbox.txt
COPY --from=janus /opt/janus /opt/janus
COPY --from=hxd-ng /usr/local/bin/hxd /usr/local/bin/hxd
