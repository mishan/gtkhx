# GtkHx test SOCKS5 proxy.
#
# microsocks has no GtkHx-test-specific tuning, so this is just a
# passthrough to the published gtkhx-socks image (built once in the
# hotline-docker repo). Kept as a Dockerfile so the compose file and CI
# build step stay uniform across the five test images; the build is a
# no-op pull.
#
# Requires the base image to be published + pullable (public, or CI logged
# in to GHCR). Pin BASE_TAG to a digest/tag for reproducibility.
ARG BASE_REGISTRY=ghcr.io/mishan
ARG BASE_TAG=latest
FROM ${BASE_REGISTRY}/gtkhx-socks:${BASE_TAG}
