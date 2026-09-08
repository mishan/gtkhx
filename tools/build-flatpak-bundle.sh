#!/bin/bash
# Build a self-contained .flatpak bundle for hand-off to a tester.
#
# Usage:
#   tools/build-flatpak-bundle.sh              # build with defaults
#   tools/build-flatpak-bundle.sh --clean      # nuke build-flatpak/
#                                              # and repo/ first
#   tools/build-flatpak-bundle.sh -o foo.flatpak  # custom output name
#
# What this does (the two flatpak commands you'd otherwise type by
# hand):
#
#   1. flatpak-builder           builds the app into a local OSTree
#                                repo (./repo) using
#                                com.nasledov.gtkhx.yml
#   2. flatpak build-bundle      exports the app from the repo into
#                                a single .flatpak file
#
# The output file is what you email / USB-stick to a tester. They
# install it with:
#
#   flatpak install --user gtkhx.flatpak
#
# First-install on their end pulls org.gnome.Platform//49 (~600 MB)
# from Flathub. If they haven't added Flathub yet:
#
#   flatpak remote-add --user --if-not-exists flathub \
#           https://flathub.org/repo/flathub.flatpakrepo
#
# The bundle itself does NOT include the runtime — including the
# runtime balloons the file to ~700 MB and is almost never what you
# want. The Flathub-pulls-runtime flow is the standard.

set -euo pipefail

# Resolve repo root so the script works from any cwd.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

MANIFEST="com.nasledov.gtkhx.yml"
# App-id is parsed from the manifest so we don't drift if it ever
# changes.
APP_ID=$(awk '/^app-id:/ {print $2; exit}' "$MANIFEST")
if [[ -z "${APP_ID:-}" ]]; then
    echo "error: couldn't parse app-id from $MANIFEST" >&2
    exit 1
fi

BUILD_DIR="build-flatpak"
REPO_DIR="repo"
OUTPUT="gtkhx.flatpak"
CLEAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)
            CLEAN=1
            shift
            ;;
        -o|--output)
            OUTPUT="$2"
            shift 2
            ;;
        -h|--help)
            sed -n 's/^# \?//;2,30p' "$0"
            exit 0
            ;;
        *)
            echo "error: unknown arg: $1" >&2
            echo "use --help for usage" >&2
            exit 1
            ;;
    esac
done

if ! command -v flatpak-builder >/dev/null 2>&1; then
    echo "error: flatpak-builder not found in PATH" >&2
    echo "  Debian / Ubuntu: sudo apt install flatpak-builder" >&2
    echo "  Fedora:          sudo dnf install flatpak-builder" >&2
    echo "  Arch:            sudo pacman -S flatpak-builder" >&2
    exit 1
fi

# The runtime + SDK must be installed on the host before the build
# can succeed. flatpak-builder will print a verbose error if they're
# missing, but checking up front gives a friendlier message.
RUNTIME_VER=$(awk '/^runtime-version:/ {gsub(/"/,"",$2); print $2; exit}' \
              "$MANIFEST")
if ! flatpak info --user "org.gnome.Sdk/$(uname -m)/${RUNTIME_VER}" \
        >/dev/null 2>&1 \
   && ! flatpak info "org.gnome.Sdk/$(uname -m)/${RUNTIME_VER}" \
        >/dev/null 2>&1; then
    echo "warning: org.gnome.Sdk//${RUNTIME_VER} doesn't look" \
         "installed." >&2
    echo "         Install with:" >&2
    echo "           flatpak install --user flathub" \
         "org.gnome.Sdk//${RUNTIME_VER}" >&2
    echo "         (flatpak-builder will fail in a few seconds" \
         "without it.)" >&2
    echo >&2
fi

if [[ $CLEAN -eq 1 ]]; then
    printf 'cleaning %s/ and %s/...\n' "$BUILD_DIR" "$REPO_DIR"
    rm -rf "$BUILD_DIR" "$REPO_DIR"
fi

# --force-clean wipes the build dir before each run; --repo writes
# the built artifact into our local OSTree repo. --user keeps it all
# in $XDG_DATA_HOME so we don't need sudo. --disable-rofiles-fuse
# avoids the FUSE-mount step, which fails on some kernels and
# inside containers and isn't needed for a single-app build.
printf 'building %s into %s/ ...\n' "$APP_ID" "$REPO_DIR"
flatpak-builder \
    --user \
    --force-clean \
    --disable-rofiles-fuse \
    --repo="$REPO_DIR" \
    "$BUILD_DIR" \
    "$MANIFEST"

# Nothing may install into /app/lib64. Flatpak puts only /app/lib on
# LD_LIBRARY_PATH, and the app carries no rpath, so a library in
# /app/lib64 resolves only via the ld.so.cache flatpak regenerates on
# the *user's* machine — it depends on their runtime commit and
# flatpak version. A bundle built that way starts here and fails on
# someone else's desktop with "cannot open shared object file". The
# manifest pins libdir to /app/lib; this catches a buildsystem that
# ignores it. Shipping a broken bundle is much worse than a red build.
#
# Test for the directory before searching it. `find` on a path that
# isn't there exits 1; under `set -o pipefail` that status survives the
# `head`, and `set -e` then kills the script at the assignment — with
# nothing on stderr, because it was redirected away. That is the
# *passing* case, so writing the check as a bare `find ... | head`
# aborted every clean build right after flatpak-builder's last line,
# before the bundle was ever exported. It only looked fine when it was
# written because the bug it guards against was still present and the
# directory existed.
STRAY_LIBS=""
if [[ -d "$BUILD_DIR/files/lib64" ]]; then
    # No 2>/dev/null and no `head`: the directory is known to exist, so
    # a non-zero find here is a real error and should be loud, and a
    # truncated list of misplaced libraries helps nobody.
    STRAY_LIBS=$(find "$BUILD_DIR/files/lib64" -name '*.so*')
fi
if [[ -n "$STRAY_LIBS" ]]; then
    echo "error: libraries installed into /app/lib64:" >&2
    echo "$STRAY_LIBS" | sed 's|^|  |' >&2
    echo >&2
    echo "  These are not on the runtime library search path. Give the" >&2
    echo "  offending module an explicit libdir in $MANIFEST." >&2
    exit 1
fi

# build-bundle pulls the just-built app out of repo/ and writes a
# single self-contained .flatpak file. No runtime inside — the
# end-user pulls org.gnome.Platform//$RUNTIME_VER from their
# configured remote (Flathub, usually) at install time.
printf 'exporting bundle to %s ...\n' "$OUTPUT"
flatpak build-bundle "$REPO_DIR" "$OUTPUT" "$APP_ID"

# Summary. Size matters when emailing the bundle; sha256 is for
# the recipient to verify integrity if you publish the bundle
# somewhere unauthenticated.
SIZE_HUMAN=$(du -h "$OUTPUT" | awk '{print $1}')
SHA=$(sha256sum "$OUTPUT" | awk '{print $1}')
printf '\n'
printf 'bundle:  %s\n' "$OUTPUT"
printf 'size:    %s\n' "$SIZE_HUMAN"
printf 'sha256:  %s\n' "$SHA"
printf '\n'
printf 'recipient install:\n'
printf '  flatpak remote-add --user --if-not-exists flathub \\\n'
printf '          https://flathub.org/repo/flathub.flatpakrepo\n'
printf '  flatpak install --user %s\n' "$OUTPUT"
printf '  flatpak run %s\n' "$APP_ID"
