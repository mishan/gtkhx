# GtkHx test overlay for hxtrackd.
#
# The base image (published from hotline-docker) compiles hxtrackd from
# the mhxd source and runs it plain. This thin layer re-applies GtkHx's
# test config (24h drop interval) + the UDP seed helper that primes the
# tracker with one deterministic server for the v1-fallback Tier 3 test.
#
# Requires the base image to be published + pullable (public, or CI logged
# in to GHCR). Pin BASE_TAG to a digest/tag for reproducibility.
ARG BASE_REGISTRY=ghcr.io/mishan
ARG BASE_TAG=latest
FROM ${BASE_REGISTRY}/hxtrackd:${BASE_TAG}

USER root

# python3 for the seed-registration helper (base image ships no python).
RUN apt-get update \
 && apt-get install --no-install-recommends -y python3-minimal \
 && rm -rf /var/lib/apt/lists/*

# Test config (long drop interval) + seed helper + the seed-loop entrypoint.
COPY conf/hxtrackd.conf /opt/hxtrackd/run/hxtrackd.conf
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
COPY seed-tracker.py /usr/local/bin/seed-tracker.py
RUN chmod 755 /usr/local/bin/docker-entrypoint.sh /usr/local/bin/seed-tracker.py \
 && chown -R hxtrackd:hxtrackd /opt/hxtrackd/run

USER hxtrackd
WORKDIR /opt/hxtrackd/run

# Override the base entrypoint (which just runs hxtrackd): this one also
# starts the UDP seed loop.
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD []
