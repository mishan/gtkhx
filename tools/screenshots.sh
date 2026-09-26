#!/bin/sh
# Take the README and AppStream screenshots, the same pixels on any machine.
#
#   tools/screenshots.sh                 every scene, into data/screenshots/
#   tools/screenshots.sh chat news       just these
#   tools/screenshots.sh --check         take them afresh and compare with
#                                        the committed ones; fails on a change
#
# Everything runs in the image tools/screenshots/Dockerfile describes, which
# pins the toolkit, the fonts and the servers. GtkHx is built there from this
# working tree, into a Docker volume that keeps the build between runs.
# The pictures are taken with shotbox, from a checkout beside this one
# (../shotbox) or wherever SHOTBOX_DIR points. See docs/screenshots.md.

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
shotbox=${SHOTBOX_DIR:-$root/../shotbox}
out=$root/data/screenshots
volume=gtkhx-screenshots-work
check=

if [ "${1:-}" = --check ]; then
    check=1
    shift
fi
if [ ! -x "$shotbox/bin/shotbox" ]; then
    echo "$0: no shotbox at $shotbox; clone https://github.com/mishan/shotbox" \
        "there, or set SHOTBOX_DIR" >&2
    exit 2
fi

docker build -q -t gtkhx-hxd-ng "$root/tests/hxd-ng" >/dev/null
docker build -q -t gtkhx-screenshots "$root/tools/screenshots" >/dev/null

mkdir -p "$out"
fresh=$(mktemp -d)
trap 'rm -rf "$fresh"' EXIT

# seccomp and AppArmor unconfined: glycin decodes images in a bubblewrap
# sandbox, and Docker's default profile refuses the namespaces it needs.
# tracker.example.org is the fake tracker the tracker scene lists.
status=0
docker run --rm \
    --security-opt seccomp=unconfined --security-opt apparmor=unconfined \
    --add-host tracker.example.org:127.0.0.1 \
    -e GTKHX_DEBUG="${GTKHX_DEBUG:-}" -e EXPLORE="${EXPLORE:-}" \
    -v "$root:/src:ro" -v "$shotbox:/shotbox:ro" -v "$volume:/work" \
    -v "$fresh:/out" -v "$out:/ref:ro" \
    gtkhx-screenshots sh -euc '
        owner=$(stat -c %u:%g /out)
        trap "chown -R $owner /out" EXIT
        export CARGO_HOME=/work/cargo-home
        [ -d /work/build ] || meson setup /work/build /src -Dtests=false \
            -Dcargo_target_dir=/work/cargo-target >/work/setup.log
        meson compile -C /work/build >/work/compile.log ||
            { tail -40 /work/compile.log; exit 1; }
        python3 /src/tools/screenshots/scenes.py /out "$@"
        if [ -n "'"$check"'" ]; then
            python3 /src/tools/screenshots/scenes.py --compare /ref /out "$@"
        fi
    ' sh "$@" || status=$?

if [ "$status" != 0 ]; then
    # Keep what was taken for a look: the differences a check found, or
    # the picture of the screen a scene took when it gave up.
    rm -rf "$root/build-screenshots"
    cp -r "$fresh" "$root/build-screenshots"
    echo "screenshots: failed; see build-screenshots/" >&2
    exit "$status"
elif [ -n "$check" ]; then
    echo "screenshots: the committed pictures are current"
else
    for f in "$fresh"/*.png; do
        case $f in
        */explore-*.png | *-failed.png)
            # Not pictures for the README: for looking at.
            mkdir -p "$root/build-screenshots"
            cp "$f" "$root/build-screenshots/"
            echo "screenshots: build-screenshots/$(basename "$f")"
            ;;
        *-diff.png) ;;
        *)
            cp "$f" "$out/"
            echo "screenshots: ${out#"$root"/}/$(basename "$f")"
            ;;
        esac
    done
fi
