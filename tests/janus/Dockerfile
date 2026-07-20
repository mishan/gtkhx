# GtkHx test overlay for Janus.
#
# The base image (published from hotline-docker) pulls + verifies the
# Janus binary and ships a general config. This thin layer re-applies
# GtkHx's test config + fixtures + HOPE-seeded accounts on top, so CI
# doesn't re-pull the tarball every run.
#
# Requires the base image to be published + pullable (public, or CI logged
# in to GHCR). Pin BASE_TAG to a digest/tag for reproducibility.
ARG BASE_REGISTRY=ghcr.io/mishan
ARG BASE_TAG=latest
FROM ${BASE_REGISTRY}/janus:${BASE_TAG}

USER root

# openssl (self-signed cert) + curl (seed-hope API calls, banner fetch);
# the base image ships neither.
RUN apt-get update \
 && apt-get install --no-install-recommends -y openssl curl ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Test config: HOPE / chat-history / voice ON, rate limits off, admin API
# on (for the build-time seed), matrix ports (5510/5610/5514), banner.
COPY conf/config.yaml /opt/janus/Server/config.yaml

# Self-signed cert the test config's TLSCert/TLSKey point at (must exist
# before the seed step boots Janus).
RUN mkdir -p /opt/janus/Server/tls \
 && openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout /opt/janus/Server/tls/key.pem \
        -out    /opt/janus/Server/tls/cert.pem \
        -days 3650 -subj "/CN=localhost" \
        -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
 && chmod 600 /opt/janus/Server/tls/key.pem

# Seed HOPE password blobs onto the bundled accounts (boots Janus + PATCHes
# via the admin API; the base image already pre-created Server/*).
COPY seed-hope-passwords.sh /usr/local/bin/seed-hope-passwords
RUN chmod +x /usr/local/bin/seed-hope-passwords \
 && /usr/local/bin/seed-hope-passwords

# Deterministic Files/ fixtures + a banner so HTLS_HDR_BANNER fires.
RUN printf 'hello world\n' > /opt/janus/Server/Files/test.txt \
 && printf 'tier-3 integration suite test data\n' \
        > /opt/janus/Server/Files/integration_seed.txt \
 && mkdir -p /opt/janus/Server/Files/Uploads /opt/janus/Server/Files/test_folder \
 && printf 'folder-leaf-A\n' > /opt/janus/Server/Files/test_folder/leaf_a.txt \
 && printf 'folder-leaf-B\n' > /opt/janus/Server/Files/test_folder/leaf_b.txt \
 && curl -fsSL --retry 3 "https://placehold.co/468x60.gif?text=GtkHx+HTXF+GIF" \
        -o /opt/janus/Server/banner.gif

RUN chown -R janus:janus /opt/janus

USER janus
# ENTRYPOINT (tracker registration from TRACKERS) inherited from the base.
