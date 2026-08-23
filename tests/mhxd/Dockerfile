# GtkHx test overlay for mhxd.
#
# The heavy build (compiling mhxd from source) now lives in the
# hotline-docker repo, which publishes a general `mhxd` base image. This
# Dockerfile is a thin layer on top that re-applies GtkHx's test-specific
# config + fixtures — so CI builds it in seconds instead of recompiling
# mhxd every run.
#
# Requires the base image to be published + pullable (public, or CI logged
# in to GHCR). Pin BASE_TAG to a digest/tag for reproducibility.
ARG BASE_REGISTRY=ghcr.io/mishan
ARG BASE_TAG=latest
FROM ${BASE_REGISTRY}/mhxd:${BASE_TAG}

USER root

# Test config (nospam off, test access-bit UserData) + the deterministic
# file/folder fixtures the Tier 3 file/folder tests assert on.
COPY conf/hxd.conf /opt/mhxd/run/hxd.conf
COPY conf/accounts/guest/UserData /opt/mhxd/run/accounts/guest/UserData
# The 1.5 threaded-news tree. The base image creates this now; kept here so a
# build against an older cached base still gets threaded news rather than an
# empty root and "No such file or directory" on folder creation. Empty on
# purpose — seeding it would give the Tier 3 news assertions more content to
# drift against.
RUN mkdir -p /opt/mhxd/run/newsdir \
 && mkdir -p /opt/mhxd/run/files/Uploads /opt/mhxd/run/files/test_folder \
 && printf 'hello world\n' > /opt/mhxd/run/files/test.txt \
 && printf 'tier-3 integration suite test data\n' \
        > /opt/mhxd/run/files/integration_seed.txt \
 && printf 'folder-leaf-A\n' > /opt/mhxd/run/files/test_folder/leaf_a.txt \
 && printf 'folder-leaf-B\n' > /opt/mhxd/run/files/test_folder/leaf_b.txt

# Banner fixtures for the file-mode HTXF banner tests (the base image
# doesn't ship curl, so pull them here).
RUN apt-get update \
 && apt-get install --no-install-recommends -y curl ca-certificates \
 && curl -fsSL --retry 3 "https://placehold.co/468x60.jpg?text=GtkHx+HTXF+JPEG" \
        -o /opt/mhxd/run/banner.jpg \
 && curl -fsSL --retry 3 "https://placehold.co/468x60.gif?text=GtkHx+HTXF+GIF" \
        -o /opt/mhxd/run/banner.gif \
 && rm -rf /var/lib/apt/lists/*

RUN chown -R mhxd:mhxd /opt/mhxd/run

USER mhxd
# WORKDIR + ENTRYPOINT (banner/tracker patching) are inherited from the base.
