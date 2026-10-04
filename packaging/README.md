# Packaging

Scripts and notes for producing distributable GtkHx builds on each platform.
Driven by `.github/workflows/release.yml` (tag pushes + manual snapshots), but
runnable locally too.

## Windows — portable ZIP

`windows/bundle.sh`, run from an MSYS2 UCRT64 shell after a build:

```sh
meson setup build -Dvoice=enabled -Dtests=false -Dcargo_target_dir="$(pwd)/rust-target"
ninja -C build
packaging/windows/bundle.sh build dist
# -> dist/GtkHx-win64/  and  dist/GtkHx-win64.zip
```

Collects `gtkhx.exe`, its transitive UCRT64 DLLs (via `ldd`, closed over), the
GStreamer voice plugins + their deps, gdk-pixbuf loaders, GSettings schemas, and
the Adwaita/hicolor icons into a mirrored `bin/ share/ lib/` prefix. GTK resolves
data relative to the exe's `bin/`, so it runs from wherever it's unzipped.

## macOS — .app bundle

`macos/bundle.sh`, run after a build with `dylibbundler` installed:

```sh
brew install dylibbundler
meson setup build -Dvoice=enabled -Dtests=false -Dcargo_target_dir="$(pwd)/rust-target"
ninja -C build
packaging/macos/bundle.sh build dist
# -> dist/GtkHx.app  and  dist/GtkHx-macos-<arch>.zip
```

`dylibbundler` rewrites the binary's linked dylibs into `Contents/Frameworks`;
GStreamer plugins (dlopen'd) are copied in and a `DYLD_FALLBACK_LIBRARY_PATH` in
the launcher backstops their deps. A launcher script is the bundle executable and
points GTK/GStreamer at the bundled Resources before exec'ing the real binary.

## Linux — Flatpak

`tools/build-flatpak-bundle.sh` (unchanged) builds `gtkhx.flatpak` from
`com.nasledov.gtkhx.yml`. The runtime is pulled from Flathub on install.

## Version and update notices

`-Dbuild_version=<version>` sets the version a build reports, in About and
to the update check; empty falls back to the project version. The package
workflow passes the version it was given, so a beta says `1.4.1b1`, not
`1.4.1-dev`. For the Flatpak it adds the option to the manifest, since
flatpak-builder takes options from nowhere else.

The macOS `Info.plist` and the Windows executable's numeric version fields
still take the project version.

The Windows and macOS builds check for updates by default (`-Dupdate_check`
is `auto`), and the manifest passes `enabled`. A Homebrew, MacPorts or MSYS2
package should pass `-Dupdate_check=disabled`: its package manager does that
job. See `docs/updates.md`.

## Status / known gaps

These collectors are a first cut; expect a CI round or two to settle. Known
follow-ups:

- **Unsigned.** No code signing / notarization yet (chosen for snapshots).
  macOS users bypass Gatekeeper (right-click → Open); Windows users click
  through SmartScreen. Revisit for real releases (needs an Apple Developer cert
  + a Windows code-signing cert).
- **GStreamer plugin lists are curated** (`GST_PLUGINS` in each script) for the
  voice pipeline. If a voice element is missing at runtime, add it there.
- **Relocatable sound path.** `src/sound.c::sound_resolve` searches, in order,
  `$CONFIG/sounds`, every `g_get_system_data_dirs()` entry's `gtkhx/sounds`, the
  Windows module-relative `share/gtkhx/sounds`, then the compiled-in
  `$PREFIX/share/gtkhx/sounds`. So the staged sounds resolve in a relocated
  bundle: the macOS launcher points `XDG_DATA_DIRS` at `Contents/Resources/share`,
  and on Windows GLib derives the data dirs from the exe location.
