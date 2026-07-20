# GtkHx test overlay for Argus.
#
# The base image (published from hotline-docker) pulls + verifies the
# Argus binary and ships a general config (conventional 5498/5499, no
# stunnel). This thin layer re-applies GtkHx's test config (plain listener
# shifted to 5698 so it coexists with hxtrackd's 5498 under host
# networking, promoted-server fixtures, debug logging) and the stunnel
# TLS-on-6498 sidecar the Phase D tracker-v3-TLS test needs.
#
# Requires the base image to be published + pullable (public, or CI logged
# in to GHCR). Pin BASE_TAG to a digest/tag for reproducibility.
ARG BASE_REGISTRY=ghcr.io/mishan
ARG BASE_TAG=latest
FROM ${BASE_REGISTRY}/argus:${BASE_TAG}

USER root

# stunnel (TLS sidecar) + openssl/xxd (self-signed cert + fingerprint);
# the base image ships none of these.
RUN apt-get update \
 && apt-get install --no-install-recommends -y openssl stunnel4 xxd \
 && rm -rf /var/lib/apt/lists/*

# Test config: plain 5698 / udp 5699, promoted-server fixtures, debug logs.
COPY conf/config.yaml /opt/argus/config.yaml

# Self-signed cert for stunnel + its SHA-256 fingerprint (informational).
RUN openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout /opt/argus/tls.key \
        -out    /opt/argus/tls.crt \
        -days 3650 -subj "/CN=gtkhx-argus/O=GtkHx Test" \
 && openssl x509 -in /opt/argus/tls.crt -outform DER \
        | openssl dgst -sha256 -binary \
        | xxd -p -c 256 \
        | awk '{print "sha256:" $0}' \
        > /opt/argus/tls.fingerprint \
 && chmod 0640 /opt/argus/tls.key \
 && chown -R argus:argus /opt/argus

COPY conf/stunnel.conf /etc/stunnel/stunnel.conf
COPY entrypoint.sh /opt/argus/entrypoint.sh
RUN chmod +x /opt/argus/entrypoint.sh

EXPOSE 5698/tcp 6498/tcp 5699/udp
WORKDIR /opt/argus

# Override the base entrypoint (which just runs argus): this one launches
# argus (as the argus user) alongside stunnel, so the container runs as
# root to bind + drop.
USER root
ENTRYPOINT ["/opt/argus/entrypoint.sh"]
CMD []
