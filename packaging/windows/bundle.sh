#!/usr/bin/env bash
#
# Build a portable Windows ZIP of GtkHx from an MSYS2 UCRT64 build.
#
# Run inside the MSYS2 UCRT64 shell after `meson setup` + `ninja`:
#
#     packaging/windows/bundle.sh [BUILD_DIR] [OUT_DIR]
#
# Produces OUT_DIR/GtkHx-win64/ (a self-contained prefix — bin/ + share/ + lib/)
# and OUT_DIR/GtkHx-win64.zip. Double-clicking bin\gtkhx.exe runs the app with
# no MSYS2 install: GTK resolves ../share and ../lib relative to the exe's bin/
# directory (g_win32_get_package_installation_directory_of_module strips the
# trailing \bin), so mirroring the MSYS2 prefix layout is what makes it portable.
#
# This is a best-effort collector; treat the first CI runs as the real test.
# The gaps most likely to need iteration: the curated GStreamer plugin list
# (voice), and the PREFIX-relative sound-file lookup in src/sound.c (a relocated
# install can't find $PREFIX/share/gtkhx/sounds — see the note near the end).
set -euo pipefail

BUILD_DIR="${1:-build}"
OUT_DIR="${2:-dist}"
MINGW_PREFIX="${MINGW_PREFIX:-/ucrt64}"

APP="GtkHx-win64"
STAGE="$OUT_DIR/$APP"
BIN="$STAGE/bin"
LIB="$STAGE/lib"
SHARE="$STAGE/share"

EXE="$BUILD_DIR/src/gtkhx.exe"
[ -f "$EXE" ] || { echo "error: $EXE not found — build first" >&2; exit 1; }

echo ">> staging into $STAGE"
rm -rf "$STAGE"
mkdir -p "$BIN" "$LIB" "$SHARE"
cp "$EXE" "$BIN/"

# ---- transitive DLL closure ------------------------------------------------
# ldd resolves the full dependency graph of a PE image (following imports). We
# copy every dependency that lives under the MSYS2 prefix (skipping C:\Windows
# system DLLs), then repeat over the DLLs we just copied — GStreamer plugins
# (added below) pull DLLs the exe alone doesn't reference.
copy_deps() {
  # $@ = PE files to scan. Copies MinGW-prefix deps into $BIN.
  local changed=1
  local scan=("$@")
  while [ "$changed" = 1 ]; do
    changed=0
    local next=()
    for f in "${scan[@]}"; do
      while read -r dll; do
        # ldd line: "name.dll => /ucrt64/bin/name.dll (0x...)"
        case "$dll" in
          "$MINGW_PREFIX"/*)
            local base; base=$(basename "$dll")
            if [ ! -f "$BIN/$base" ]; then
              cp "$dll" "$BIN/"
              next+=("$BIN/$base")
              changed=1
            fi
            ;;
        esac
      done < <(ldd "$f" 2>/dev/null | awk '{print $3}')
    done
    # Re-scan only the newly copied DLLs next pass (bash 4.4+ expands an empty
    # array cleanly under `set -u`). The `changed` flag ends the loop.
    scan=("${next[@]}")
  done
}

echo ">> collecting DLL dependencies of gtkhx.exe"
copy_deps "$BIN/gtkhx.exe"

# ---- GStreamer plugins (voice: -Dvoice=enabled) ----------------------------
# GStreamer plugins are dlopen()ed, so ldd never sees them — copy the elements
# the voice pipeline needs explicitly, then close over their own DLL deps.
# webrtcbin (webrtc) + nice (ICE) + dtls/srtp/sctp are the WebRTC core;
# rtp/rtpmanager carry the media; audioconvert/resample/opus/level are the audio
# path; wasapi2 is the OS capture/render; coreelements/playback/autodetect glue.
GST_SRC="$MINGW_PREFIX/lib/gstreamer-1.0"
GST_DST="$LIB/gstreamer-1.0"
GST_PLUGINS=(
  coreelements playback autodetect typefindfunctions
  audioconvert audioresample audiomixer volume level
  opus rtp rtpmanager srtp dtls sctp webrtc webrtcnice nice
  wasapi wasapi2 directsound
)
if [ -d "$GST_SRC" ]; then
  echo ">> collecting GStreamer plugins"
  mkdir -p "$GST_DST"
  for p in "${GST_PLUGINS[@]}"; do
    dll="$GST_SRC/libgst${p}.dll"
    [ -f "$dll" ] && cp "$dll" "$GST_DST/"
  done
  # gst-plugin-scanner is spawned by the registry; ship it beside the plugins.
  scanner="$MINGW_PREFIX/lib/gstreamer-1.0/gst-plugin-scanner.exe"
  [ -f "$scanner" ] && cp "$scanner" "$GST_DST/"
  copy_deps "$GST_DST"/*.dll
else
  echo "::warning:: $GST_SRC missing — voice plugins not bundled"
fi

# ---- gdk-pixbuf loaders ----------------------------------------------------
# Copy the loader DLLs and a loaders.cache rewritten to relative paths so it
# resolves inside the bundle regardless of where the user unzips it.
PIXBUF_SRC="$MINGW_PREFIX/lib/gdk-pixbuf-2.0/2.10.0"
if [ -d "$PIXBUF_SRC/loaders" ]; then
  echo ">> collecting gdk-pixbuf loaders"
  PIXBUF_DST="$LIB/gdk-pixbuf-2.0/2.10.0"
  mkdir -p "$PIXBUF_DST/loaders"
  cp "$PIXBUF_SRC"/loaders/*.dll "$PIXBUF_DST/loaders/"
  copy_deps "$PIXBUF_DST"/loaders/*.dll
  # Emit a cache with bundle-relative loader paths.
  ( cd "$PIXBUF_DST" \
    && GDK_PIXBUF_MODULEDIR=loaders gdk-pixbuf-query-loaders loaders/*.dll \
       > loaders.cache )
fi

# ---- GSettings schemas (GTK / libadwaita read org.gtk.* + org.gnome.*) -----
echo ">> compiling GSettings schemas"
mkdir -p "$SHARE/glib-2.0/schemas"
cp "$MINGW_PREFIX"/share/glib-2.0/schemas/*.xml "$SHARE/glib-2.0/schemas/" 2>/dev/null || true
glib-compile-schemas "$SHARE/glib-2.0/schemas"

# ---- icons (GTK/Adwaita stock icons: window controls, symbolic widget art) --
echo ">> collecting icon themes"
mkdir -p "$SHARE/icons"
for theme in Adwaita hicolor; do
  [ -d "$MINGW_PREFIX/share/icons/$theme" ] \
    && cp -r "$MINGW_PREFIX/share/icons/$theme" "$SHARE/icons/"
done
gtk4-update-icon-cache -q -t -f "$SHARE/icons/Adwaita" 2>/dev/null || true

# ---- app data (alert sounds live outside GResource) ------------------------
# NOTE: src/sound.c looks in $PREFIX/share/gtkhx/sounds, where $PREFIX is baked
# in at configure time and won't match the unzip location — so on a relocated
# install the sounds aren't found today. We stage them at the mirrored path so
# they're present; making the lookup relocatable (derive the prefix from the
# module dir on Windows) is a follow-up in src/sound.c.
if [ -d sounds ]; then
  mkdir -p "$SHARE/gtkhx/sounds"
  cp sounds/*.wav "$SHARE/gtkhx/sounds/" 2>/dev/null || true
fi

# ---- zip -------------------------------------------------------------------
echo ">> zipping"
( cd "$OUT_DIR" && zip -qr "$APP.zip" "$APP" )
echo ">> done: $OUT_DIR/$APP.zip"
