# syntax=docker/dockerfile:1
#
# hxd-ng for the GtkHx Tier 3 rig.
#
# hxd-ng (https://github.com/mishan/hxd-ng) is the only Hotline server
# that implements the video extension, so it is the target for every
# video test. Built from source at a pinned revision, like the other
# servers are pinned: an unpinned main has broken this rig before. Bump
# HXD_NG_REV deliberately, with the tests run against the new one.
ARG HXD_NG_REV=cba901bd22d6e4034e27f30888a9f8f4835e1767

FROM rust:1-trixie AS build
ARG HXD_NG_REV
# The voice SFU's DTLS stack reaches aws-lc-sys, which needs cmake.
RUN apt-get update \
 && apt-get install -y --no-install-recommends cmake git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
RUN git clone https://github.com/mishan/hxd-ng.git /src \
 && git -C /src checkout --detach "$HXD_NG_REV"
WORKDIR /src
RUN --mount=type=cache,target=/usr/local/cargo/registry \
    --mount=type=cache,target=/usr/local/cargo/git \
    --mount=type=cache,target=/src/target \
    cargo build --release --locked -p hxd \
 && install -D -m 0755 target/release/hxd -t /out/

FROM debian:trixie-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/hxd /usr/local/bin/hxd
COPY conf/hxd-ng.toml /etc/hxd-ng/hxd-ng.toml
COPY conf/accounts /var/lib/hxd-ng/accounts
COPY --chmod=0755 entrypoint.sh /usr/local/bin/hxd-ng-entrypoint
WORKDIR /var/lib/hxd-ng
HEALTHCHECK --interval=10s --timeout=5s --start-period=10s --retries=3 \
    CMD ["bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/5520"]
ENTRYPOINT ["hxd-ng-entrypoint"]
