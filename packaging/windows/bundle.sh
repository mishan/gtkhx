#!/usr/bin/env bash
#
# Build a portable Windows ZIP of GtkHx from an MSYS2 UCRT64 build.
#
# Run inside the MSYS2 UCRT64 shell after `meson setup` + `ninja`:
#
#     packaging/windows/bundle.sh [BUILD_DIR] [OUT_DIR]
#
# Produces OUT_DIR/GtkHx-win64/ (gtkhx.exe + DLLs at the root, with share/ and
# lib/ alongside) and OUT_DIR/GtkHx-win64.zip. Double-clicking gtkhx.exe runs the
# app with no MSYS2 install: GLib derives the install prefix from the module
# directory (g_win32_get_package_installation_directory_of_module), so it finds
# share/ + lib/ next to the exe — and the app's own relocatable data lookups
# (sounds, icons.rsrc) resolve via that same prefix.
#
# This is a best-effort collector; treat the first CI runs as the real test.
# The gap most likely to need iteration: the curated GStreamer plugin list.
set -euo pipefail

BUILD_DIR="${1:-build}"
OUT_DIR="${2:-dist}"
MINGW_PREFIX="${MINGW_PREFIX:-/ucrt64}"

APP="GtkHx-win64"
STAGE="$OUT_DIR/$APP"
# gtkhx.exe + its DLLs live at the archive root so the user double-clicks
# gtkhx.exe directly (no bin/ to descend into). GTK still resolves data because
# GLib derives the install prefix from the module directory, and share/ + lib/
# sit alongside the exe — the same prefix layout, just without the bin/ level.
BIN="$STAGE"
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
      # Skip anything that isn't a real file — notably an unmatched glob, which
      # bash leaves as the literal pattern (feeding it to ldd would abort the
      # script under `set -e`).
      [ -f "$f" ] || continue
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
# Source of truth: the element factories hxvoice-runtime creates (grep
# ElementFactory::make in rust/crates/hxvoice-runtime). mulaw (mulawenc/mulawdec)
# is the PCMU codec the servers negotiate — without it VoiceRuntime::new fails to
# build the send bin and the client leaves the room the instant it joins.
# audiotestsrc is the silence/fallback source autoaudiosrc drops to when there's
# no capture device.
GST_PLUGINS=(
  coreelements playback autodetect typefindfunctions
  audioconvert audioresample audiomixer volume level audiotestsrc
  opus mulaw rtp rtpmanager srtp dtls sctp webrtc webrtcnice nice
  wasapi wasapi2 directsound
)
if [ -d "$GST_SRC" ]; then
  echo ">> collecting GStreamer plugins"
  mkdir -p "$GST_DST"
  for p in "${GST_PLUGINS[@]}"; do
    dll="$GST_SRC/libgst${p}.dll"
    [ -f "$dll" ] && cp "$dll" "$GST_DST/"
  done
  # gst-plugin-scanner is spawned by the registry to introspect plugins on first
  # launch; ship it beside the plugins AND fold it into the dependency closure so
  # any DLL only it references is bundled (otherwise the scan fails at runtime).
  scanner="$MINGW_PREFIX/lib/gstreamer-1.0/gst-plugin-scanner.exe"
  [ -f "$scanner" ] && cp "$scanner" "$GST_DST/"
  copy_deps "$GST_DST"/*.dll "$GST_DST"/*.exe
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

# ---- app data (alert sounds + Hotline user icons live outside GResource) ---
# Both resolve at runtime relative to the module-derived prefix (sound_resolve /
# init_icons search g_get_system_data_dirs() + the Windows module path), so
# staging them at <prefix>/share/gtkhx/... is what makes them load.
if [ -d sounds ]; then
  mkdir -p "$SHARE/gtkhx/sounds"
  cp sounds/*.wav "$SHARE/gtkhx/sounds/" 2>/dev/null || true
fi
# icons.rsrc — the classic Hotline colour-icon (cicn) set for the user list.
if [ -f icons.rsrc ]; then
  mkdir -p "$SHARE/gtkhx/icons"
  cp icons.rsrc "$SHARE/gtkhx/icons/"
fi

# ---- zip -------------------------------------------------------------------
echo ">> zipping"
( cd "$OUT_DIR" && zip -qr "$APP.zip" "$APP" )
echo ">> done: $OUT_DIR/$APP.zip"
