#!/bin/sh
# tests/mhxd/docker-entrypoint.sh — patch hxd.conf banner block at
# container startup based on env vars, then exec hxd.
#
# Variables (all optional, defaults preserve existing URL-mode
# behaviour for compatibility with the integration suite):
#
#   BANNER_MODE   "URL", "GIFf", or "JPEG"
#                 Default: "URL"
#                 "URL "/"URL" advertises a URL the client fetches
#                 itself via HTTP. "GIFf"/"JPEG" puts mhxd in
#                 file-mode: it sends HTLS_HDR_BANNER with the
#                 type but no URL, and replies to
#                 HTLC_HDR_DOWNLOAD_BANNER (212) by serving the
#                 bytes from BANNER_FILE over the HTXF subchannel
#                 (base port + 1).
#
#   BANNER_URL    URL string for URL mode.
#                 Default: https://placehold.co/468x60/png?text=GtkHx+Test+Banner
#
#   BANNER_FILE   Path inside the container to the banner image
#                 for file modes. The runtime user is `mhxd`, so
#                 the file must be readable by that user.
#                 Default for GIFf: /opt/mhxd/run/banner.gif
#                 Default for JPEG: /opt/mhxd/run/banner.jpg
#
# Examples:
#
#   # URL mode (current default; integration suite expects this)
#   docker run --rm -p 5500:5500 -p 5501:5501 gtkhx-mhxd
#
#   # File mode, JPEG, served from the seeded /opt/mhxd/run/banner.jpg
#   docker run --rm -p 5500:5500 -p 5501:5501 \
#       -e BANNER_MODE=JPEG \
#       gtkhx-mhxd
#
#   # File mode, GIF, custom banner mounted at runtime
#   docker run --rm -p 5500:5500 -p 5501:5501 \
#       -e BANNER_MODE=GIFf \
#       -e BANNER_FILE=/opt/mhxd/run/custom.gif \
#       -v $PWD/my_banner.gif:/opt/mhxd/run/custom.gif:ro \
#       gtkhx-mhxd

set -eu

CONF=/opt/mhxd/run/hxd.conf

# Tracker registration target(s). hxd.conf ships with `trackers
# 127.0.0.1;` (the standalone default — harmless when nothing is
# listening on the host's 5499/udp). The docker-compose rig overrides
# this with the tracker service names so mhxd's UDP heartbeats land on
# the Argus + hxtrackd containers:
#
#   TRACKERS="argus, hxtrackd"
#
# Accepts the same comma-separated host list hxd.conf's `trackers`
# directive does (bare host, host:port-not-applicable — registration
# always targets 5499/udp; ids/passwords via id:password@host also pass
# through verbatim). `tracker_register yes` is already set in conf, so
# patching the host list is all that's needed.
TRACKERS="${TRACKERS:-127.0.0.1}"
# Escape characters special to sed's replacement side before splicing the
# value in: backslash, the '|' we use as the s/// delimiter, and '&'
# (which sed expands to the whole match). Without this, an id:password
# value containing any of those — which the conf's `trackers` syntax
# permits — could corrupt the replacement or break the s/// expression.
TRACKERS_ESC=$(printf '%s' "$TRACKERS" | sed -e 's/[\\&|]/\\&/g')
sed -i \
	-e "s|^\\([[:space:]]*\\)trackers .*;|\\1trackers ${TRACKERS_ESC};|" \
	"$CONF"
echo "docker-entrypoint: trackers=\"$TRACKERS\""

MODE="${BANNER_MODE:-URL}"
URL_DEFAULT="https://placehold.co/468x60/png?text=GtkHx+Test+Banner"
URL="${BANNER_URL:-$URL_DEFAULT}"

# Pick a sensible file default based on mode if BANNER_FILE wasn't
# explicitly set. mhxd cares about the extension only insofar as
# the type field tells the client what's on the wire — file bytes
# are served as-is.
#
# Also clear the "other" field (FILE in URL mode, URL in file
# modes) — mhxd'"'"'s banner-send logic in src/hxd/rcv.c doesn'"'"'t
# gate the optional fields on the type, so without this it would
# happily include a stale URL chunk inside a JPEG-type banner.
# Empty string in the config = no chunk on the wire.
case "$MODE" in
	JPEG)
		FILE="${BANNER_FILE:-/opt/mhxd/run/banner.jpg}"
		URL=""
		# mhxd's type strings are exactly 4 bytes — the docs note
		# "GIFf" is the GIF type and "JPEG" is JPEG; URL is the
		# special pseudo-type. Pass through verbatim.
		TYPE="JPEG"
		;;
	GIFf|GIF)
		FILE="${BANNER_FILE:-/opt/mhxd/run/banner.gif}"
		URL=""
		TYPE="GIFf"
		;;
	URL|"URL ")
		FILE=""
		# URL kept from the BANNER_URL var above.
		TYPE="URL"
		;;
	*)
		echo "docker-entrypoint: unknown BANNER_MODE=$MODE" >&2
		echo "  supported: URL, GIFf, JPEG" >&2
		exit 64
		;;
esac

# In file modes, fail early if the file isn't there — otherwise
# mhxd will read 0 bytes at runtime and clients will get an empty
# transfer with no diagnostic.
if [ "$MODE" != "URL" ]; then
	if [ ! -r "$FILE" ]; then
		echo "docker-entrypoint: BANNER_FILE=$FILE not readable" >&2
		exit 65
	fi
fi

# Patch the three fields in the banner block. The hxd.conf format
# is forgiving — values can be any string in double quotes, and
# leading whitespace varies — so anchor on the field name and
# preserve the indentation prefix.
#
# Wrap the URL and FILE values in '|' delimiters because URLs and
# absolute paths both contain '/' which is sed's default delimiter.
sed -i \
	-e "s|^\\([[:space:]]*\\)type \"[^\"]*\";|\\1type \"$TYPE\";|" \
	-e "s|^\\([[:space:]]*\\)file \"[^\"]*\";|\\1file \"$FILE\";|" \
	-e "s|^\\([[:space:]]*\\)url \"[^\"]*\";|\\1url \"$URL\";|" \
	"$CONF"

echo "docker-entrypoint: banner mode=$TYPE file=\"$FILE\" url=\"$URL\""

exec /opt/mhxd/run/bin/hxd "$@"
