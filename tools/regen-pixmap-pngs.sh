#!/usr/bin/env bash
#
# regen-pixmap-pngs.sh — rebuild src/pixmaps/*.png from the matching
# .xpm sources.
#
# The XPMs are the source-of-truth pixel art (hand-edited, human-
# readable, version-controlled diffs render as text). The PNGs are
# the deployed form, bundled into gtkhx.gresource so callers can use
# gdk_pixbuf_new_from_resource without depending on the GdkPixbuf XPM
# loader — which isn't included in the Flatpak GNOME runtime by
# default (legacy-format loaders have been getting dropped upstream).
#
# Re-run this script whenever you change any .xpm in src/pixmaps,
# then commit both the .xpm and the regenerated .png. Both forms
# live in the tree so anyone with classic Mac/X11 icon tools can
# round-trip the XPM and have the PNG follow.
#
# Quirk: a handful of historical XPMs in this tree use an empty
# transparent-color spec (`X<tab>c "` with nothing after the c),
# which is legal-ish XPM but trips ImageMagick's parser. The awk
# preprocess below substitutes "None" for the missing color name —
# matches what gdk-pixbuf's loader does internally.

set -euo pipefail

cd "$(dirname "$0")/../src/pixmaps"

for xpm in *.xpm; do
    png="${xpm%.xpm}.png"
    tmp="$(mktemp --suffix=.xpm)"
    awk '
        # Replace `<glyph>\tc "` (empty transparent-color spec) with
        # `<glyph>\tc None"` so ImageMagick can parse it.
        /\tc "/ { sub(/c "/, "c None\"") }
        { print }
    ' "$xpm" > "$tmp"
    magick "$tmp" "$png"
    rm -f "$tmp"
done

echo "Regenerated $(ls *.png | wc -l) PNGs from $(ls *.xpm | wc -l) XPMs."
