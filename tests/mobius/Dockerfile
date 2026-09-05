# GtkHx test overlay for Mobius (github.com/jhalter/mobius).
#
# Unlike mhxd and Janus, Mobius publishes its own server image, so there
# is no hotline-docker base layer to build first — this Dockerfile is the
# whole rig. The upstream image is FROM scratch: no shell, no package
# manager, nothing to RUN. Everything the rig needs is therefore a COPY
# of a file the server reads at startup.
#
# Pin policy matches the rest of the rig: bump MOBIUS_TAG explicitly with
# a test run, never silently. Mobius makes wire-format-affecting changes
# between minor releases, and the news transactions in particular have
# moved. Upstream publishes one tag per git tag plus `edge` for master;
# there is no floating `latest` to drift under us.
ARG MOBIUS_TAG=v0.23.1
FROM ghcr.io/jhalter/mobius-hotline-server:${MOBIUS_TAG}

# The base image ships its default config tree at this path; COPY merges
# over it, so Agreement.txt / banner.jpg / Files/ survive untouched and
# only the four files below are ours.
COPY conf/ /usr/local/var/mobius/config/

EXPOSE 5520 5521

# The base ENTRYPOINT is ["/app/server"]; -config is required because the
# flag's default is the relative path "config" and a scratch image has no
# working directory to resolve it against. -bind moves the server off the
# conventional 5500 so it can share host networking with mhxd (5500) and
# Janus (5510); the file-transfer port follows as bind+1.
CMD ["-config", "/usr/local/var/mobius/config", "-bind", "5520"]
