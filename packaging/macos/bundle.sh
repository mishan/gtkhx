#!/usr/bin/env bash
#
# Build a relocatable macOS .app bundle of GtkHx from a Homebrew build.
#
# Run after `meson setup` + `ninja`, with Homebrew's dylibbundler installed
# (`brew install dylibbundler`):
#
#     packaging/macos/bundle.sh [BUILD_DIR] [OUT_DIR]
#
# Produces OUT_DIR/GtkHx.app and OUT_DIR/GtkHx-macos-<arch>.zip.
#
# Layout: Contents/MacOS/gtkhx-bin is the real binary; Contents/MacOS/GtkHx is a
# launcher (the CFBundleExecutable) that points GTK/GStreamer at the bundled
# Resources/Frameworks and then execs it. dylibbundler rewrites the binary's
# linked dylibs into Frameworks with @executable_path install names; the GStreamer
# plugins (dlopen'd, so not linked) are copied in and a DYLD fallback path in the
# launcher backstops any of their deps that didn't get rewritten.
#
# Best-effort collector — treat the first CI runs as the real test. Most likely
# to need iteration: the curated GStreamer plugin list. (Alert sounds resolve via
# the launcher's XDG_DATA_DIRS — see sound_resolve in src/sound.c.)
set -euo pipefail

BUILD_DIR="${1:-build}"
OUT_DIR="${2:-dist}"
BREW="$(brew --prefix)"
ARCH="$(uname -m)"

BIN="$BUILD_DIR/src/gtkhx"
[ -f "$BIN" ] || { echo "error: $BIN not found — build first" >&2; exit 1; }
command -v dylibbundler >/dev/null || { echo "error: dylibbundler missing (brew install dylibbundler)" >&2; exit 1; }

APP="$OUT_DIR/GtkHx.app"
MACOS="$APP/Contents/MacOS"
RES="$APP/Contents/Resources"
FW="$APP/Contents/Frameworks"

# Search paths passed to EVERY dylibbundler call. GStreamer plugins/libs
# reference their deps via @rpath / relative names dylibbundler can't resolve on
# its own; without a search path it drops to an interactive "specify the
# directory where this library is located" prompt that hangs a CI run forever
# (no TTY). Cover Homebrew's main lib dir plus every keg-only formula's lib.
DB_SEARCH=(--search-path "$BREW/lib")
for _d in "$BREW"/opt/*/lib; do [ -d "$_d" ] && DB_SEARCH+=(--search-path "$_d"); done

# Hang-proof wrapper: even with the search paths above, an unexpected unresolved
# library would drop dylibbundler into its stdin prompt and spin forever in CI.
# Feeding it an endless "quit" makes such a case abort the pass (non-zero) rather
# than block. The process-substitution keeps dylibbundler's own exit status (a
# plain `yes | ` pipe would mask it / trip pipefail on SIGPIPE).
run_dylibbundler() { dylibbundler "$@" < <(yes quit); }

echo ">> staging $APP"
rm -rf "$APP"
mkdir -p "$MACOS" "$RES" "$FW"
cp "$BIN" "$MACOS/gtkhx-bin"

# ---- linked dylib closure --------------------------------------------------
# dylibbundler follows the binary's load commands, copies every non-system dylib
# into Frameworks, and rewrites install names to @executable_path/../Frameworks.
echo ">> bundling linked dylibs"
run_dylibbundler --create-dir --overwrite-files --bundle-deps \
  --fix-file "$MACOS/gtkhx-bin" \
  --dest-dir "$FW" \
  --install-path "@executable_path/../Frameworks/" \
  "${DB_SEARCH[@]}"

# ---- GStreamer plugins (voice: -Dvoice=enabled) ----------------------------
# dlopen'd, so dylibbundler didn't see them. Copy the voice pipeline's elements
# and fix each plugin's own dylib deps into Frameworks. The launcher also sets a
# DYLD fallback path as a backstop for anything not rewritten.
GST_SRC="$BREW/lib/gstreamer-1.0"
GST_DST="$FW/gstreamer-1.0"
GST_PLUGINS=(
  coreelements playback autodetect typefindfunctions
  audioconvert audioresample audiomixer volume level
  opus rtp rtpmanager srtp dtls sctp webrtc webrtcnice nice
  osxaudio applemedia
)
if [ -d "$GST_SRC" ]; then
  echo ">> collecting GStreamer plugins"
  mkdir -p "$GST_DST"
  for p in "${GST_PLUGINS[@]}"; do
    dylib="$GST_SRC/libgst${p}.dylib"
    [ -f "$dylib" ] && cp "$dylib" "$GST_DST/"
  done
  # gst-plugin-scanner is spawned to build the registry; ship it too.
  scanner="$BREW/libexec/gstreamer-1.0/gst-plugin-scanner"
  [ -f "$scanner" ] && { mkdir -p "$FW/gstreamer-1.0-libexec"; cp "$scanner" "$FW/gstreamer-1.0-libexec/"; }
  # Fix each plugin's deps into Frameworks (best-effort; DYLD fallback backstops).
  for dylib in "$GST_DST"/*.dylib; do
    [ -f "$dylib" ] || continue   # skip an unmatched glob (would abort set -e)
    run_dylibbundler --overwrite-files --bundle-deps \
      --fix-file "$dylib" --dest-dir "$FW" \
      --install-path "@executable_path/../Frameworks/" \
      "${DB_SEARCH[@]}" --search-path "$FW" 2>/dev/null || \
      echo "::warning:: dylibbundler pass failed for $(basename "$dylib")"
  done
  # Rewrite the scanner's own load commands too. As shipped it references its
  # deps via absolute Homebrew-prefix paths, which don't exist on an end user's
  # machine — copy them into Frameworks and repoint the scanner at them (it runs
  # from Frameworks/gstreamer-1.0-libexec/, so its siblings sit one level up).
  scanner_dst="$FW/gstreamer-1.0-libexec/gst-plugin-scanner"
  if [ -f "$scanner_dst" ]; then
    run_dylibbundler --overwrite-files --bundle-deps \
      --fix-file "$scanner_dst" --dest-dir "$FW" \
      --install-path "@executable_path/../" \
      "${DB_SEARCH[@]}" --search-path "$FW" 2>/dev/null || \
      echo "::warning:: dylibbundler pass failed for gst-plugin-scanner"
  fi
else
  echo "::warning:: $GST_SRC missing — voice plugins not bundled"
fi

# ---- gdk-pixbuf loaders ----------------------------------------------------
PIXBUF_SRC="$BREW/lib/gdk-pixbuf-2.0/2.10.0"
if [ -d "$PIXBUF_SRC/loaders" ]; then
  echo ">> collecting gdk-pixbuf loaders"
  PIXBUF_DST="$RES/lib/gdk-pixbuf-2.0/2.10.0"
  mkdir -p "$PIXBUF_DST/loaders"
  cp "$PIXBUF_SRC"/loaders/*.so "$PIXBUF_DST/loaders/"
  for so in "$PIXBUF_DST"/loaders/*.so; do
    [ -f "$so" ] || continue   # skip an unmatched glob (would abort set -e)
    run_dylibbundler --overwrite-files --bundle-deps --fix-file "$so" \
      --dest-dir "$FW" --install-path "@executable_path/../Frameworks/" \
      "${DB_SEARCH[@]}" 2>/dev/null || true
  done
  ( cd "$PIXBUF_DST" && GDK_PIXBUF_MODULEDIR=loaders \
      gdk-pixbuf-query-loaders loaders/*.so > loaders.cache )
fi

# ---- GSettings schemas + icons + app data ----------------------------------
echo ">> collecting schemas, icons, sounds"
mkdir -p "$RES/share/glib-2.0/schemas"
cp "$BREW"/share/glib-2.0/schemas/*.xml "$RES/share/glib-2.0/schemas/" 2>/dev/null || true
glib-compile-schemas "$RES/share/glib-2.0/schemas"

mkdir -p "$RES/share/icons"
for theme in Adwaita hicolor; do
  [ -d "$BREW/share/icons/$theme" ] && cp -R "$BREW/share/icons/$theme" "$RES/share/icons/"
done
gtk4-update-icon-cache -q -t -f "$RES/share/icons/Adwaita" 2>/dev/null || true

if [ -d sounds ]; then
  mkdir -p "$RES/share/gtkhx/sounds"
  cp sounds/*.wav "$RES/share/gtkhx/sounds/" 2>/dev/null || true
fi

# ---- app icon (.icns from src/pixmaps) -------------------------------------
echo ">> building app icon"
ICONSET="$(mktemp -d)/GtkHx.iconset"
mkdir -p "$ICONSET"
cp src/pixmaps/gtkhx_icon_16.png  "$ICONSET/icon_16x16.png"
cp src/pixmaps/gtkhx_icon_32.png  "$ICONSET/icon_16x16@2x.png"
cp src/pixmaps/gtkhx_icon_32.png  "$ICONSET/icon_32x32.png"
cp src/pixmaps/gtkhx_icon_64.png  "$ICONSET/icon_32x32@2x.png"
cp src/pixmaps/gtkhx_icon_128.png "$ICONSET/icon_128x128.png"
cp src/pixmaps/gtkhx_icon_256.png "$ICONSET/icon_128x128@2x.png"
cp src/pixmaps/gtkhx_icon_256.png "$ICONSET/icon_256x256.png"
iconutil -c icns "$ICONSET" -o "$RES/GtkHx.icns" || echo "::warning:: iconutil failed"

# ---- launcher + Info.plist -------------------------------------------------
echo ">> writing launcher + Info.plist"
cat > "$MACOS/GtkHx" <<'LAUNCH'
#!/bin/bash
# Point GTK / GStreamer / gdk-pixbuf at the bundled copies, then exec the binary.
HERE="$(cd "$(dirname "$0")" && pwd)"
RES="$HERE/../Resources"
FW="$HERE/../Frameworks"
export DYLD_FALLBACK_LIBRARY_PATH="$FW:${DYLD_FALLBACK_LIBRARY_PATH:-}"
export GST_PLUGIN_SYSTEM_PATH=""
export GST_PLUGIN_PATH="$FW/gstreamer-1.0"
export GST_PLUGIN_SCANNER="$FW/gstreamer-1.0-libexec/gst-plugin-scanner"
export GDK_PIXBUF_MODULE_FILE="$RES/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
export GSETTINGS_SCHEMA_DIR="$RES/share/glib-2.0/schemas"
export XDG_DATA_DIRS="$RES/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "$HERE/gtkhx-bin" "$@"
LAUNCH
chmod +x "$MACOS/GtkHx"

# Version from meson if available, else a dev placeholder.
VERSION="$(awk -F\' '/version *:/ {print $2; exit}' meson.build 2>/dev/null || echo 0.0.0-dev)"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>              <string>GtkHx</string>
  <key>CFBundleDisplayName</key>       <string>GtkHx</string>
  <key>CFBundleIdentifier</key>        <string>com.nasledov.gtkhx</string>
  <key>CFBundleVersion</key>           <string>$VERSION</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleExecutable</key>        <string>GtkHx</string>
  <key>CFBundleIconFile</key>          <string>GtkHx.icns</string>
  <key>CFBundlePackageType</key>       <string>APPL</string>
  <key>LSMinimumSystemVersion</key>    <string>11.0</string>
  <key>NSHighResolutionCapable</key>   <true/>
</dict>
</plist>
PLIST

# ---- zip -------------------------------------------------------------------
echo ">> zipping"
( cd "$OUT_DIR" && ditto -c -k --keepParent GtkHx.app "GtkHx-macos-$ARCH.zip" )
echo ">> done: $OUT_DIR/GtkHx-macos-$ARCH.zip"
