project('gtkhx', 'c',
  version : '1.3.2',
  license : 'GPL-2.0-or-later',
  meson_version : '>= 0.60.0',
  default_options : [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

# -----------------------------------------------------------------------------
# Compiler / flags
# -----------------------------------------------------------------------------
cc = meson.get_compiler('c')

add_project_arguments(
  '-DHAVE_CONFIG_H=1',
  '-D_GNU_SOURCE',
  '-D_REENTRANT',
  '-DGETTEXT_PACKAGE="@0@"'.format(meson.project_name()),
  '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
  '-DPREFIX="@0@"'.format(get_option('prefix')),
  language : 'c',
)

# Warnings worth turning on now. The codebase has 23 years of accumulated
# style; expect noise. Drop a flag back to -Wno- here if a single phase needs
# breathing room — better to keep the strict floor visible than hide it in
# per-file pragmas.
extra_warnings = [
  '-Wshadow',
  '-Wpointer-arith',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wno-unused-parameter',
  # Phase 3.9: lock in the deprecation pass — the tree is clean of GTK 3
  # deprecations as of the wmclass / table-to-grid sweeps. Promote any
  # future use of a deprecated GTK / GLib / GDK symbol to a hard error
  # so we don't silently accumulate the next round. The few sites we
  # have to keep using deprecated API for (gtk_dialog_get_action_area)
  # live behind the G_GNUC_BEGIN_IGNORE_DEPRECATIONS pragma in
  # gtkutil.c — the pragma is honored even with -Werror.
  #
  # Phase 4.1: temporarily downgrade to a warning so the GTK 4 port can
  # focus on breaking-change errors first; deprecations come back as
  # errors in Phase 4.13.
  # Phase 4.13: re-enabled. The GTK 4 port is clean of deprecated
  # symbols outside gtk_hlist_compat.c, which wraps its GtkTreeView
  # usage in G_GNUC_BEGIN/END_IGNORE_DEPRECATIONS pending the Phase 5
  # GtkColumnView migration.
  '-Werror=deprecated-declarations',
]
add_project_arguments(cc.get_supported_arguments(extra_warnings), language : 'c')

# -----------------------------------------------------------------------------
# Dependencies
# -----------------------------------------------------------------------------
gtk_dep     = dependency('gtk4',      version : '>= 4.10')
# Phase 5: libadwaita gives us AdwApplication (color-scheme tracking),
# AdwPreferencesWindow (modern Settings layout), AdwHeaderBar, AdwToast,
# AdwAboutDialog, AdwAlertDialog. 1.6 is the floor that ships the new
# adaptive AdwDialog family (AdwAboutDialog / AdwAlertDialog /
# AdwPreferencesDialog) — the old AdwAboutWindow / AdwMessageDialog
# became deprecated in 1.6 and we pick up -Werror=deprecated-declarations
# from the build flags. 1.6 was released April 2024; widely available.
adw_dep     = dependency('libadwaita-1', version : '>= 1.6')
# libpanel-1 is GNOME Builder's docking widget library — PanelDock /
# PanelGrid / PanelFrame / PanelWidget / PanelToggleButton. See
# docs/docking.md. The dependency was de-risked against
# Debian trixie (1.10.0) and the
# org.gnome.Platform/49 Flatpak runtime (which ships it). Floor
# pinned at 1.4: that's where create-frame on PanelDock/PanelGrid
# landed (1.2) and where panel_workbench_foreach_workspace landed
# (1.4) — anything older predates the API surface the docking work
# leans on.
panel_dep   = dependency('libpanel-1', version : '>= 1.4')
glib_dep    = dependency('glib-2.0',  version : '>= 2.56')
gthread_dep = dependency('gthread-2.0')
threads_dep = dependency('threads')

# Alert-sound playback (src/sound.c) is now the Rust `hxsound` crate
# (rust/crates/hxsound), which plays WAV files via rodio/cpal on Linux,
# macOS, and Windows — replacing the Linux-only GSound/libcanberra path. Its
# per-platform audio system libs are wired to rust_gtkhx_ffi_dep in
# rust/meson.build.

# Phase 8.B: GStreamer system libraries. The Rust hxvoice-runtime
# crate wraps these via gstreamer-sys (from the gstreamer-rs 0.24
# family pinned in rust/Cargo.toml's workspace.dependencies). The
# system libs still need to be on the build host so cargo's build
# script can find the .pc files and the final linker can resolve the
# shared library symbols.
#
# Version floor: 1.20 ships webrtcbin's stable API surface. The
# gstreamer-rs 0.24 family's build-time floor for gstreamer-webrtc-sys
# is 1.14, but the project locks the >= 1.20 floor here because that
# is where webrtcbin stabilised. Debian trixie ships 1.24 and the
# GNOME 49 Flatpak runtime ships 1.26 — both clear the floor and
# both pair cleanly with gstreamer-rs 0.24. (The earlier rust pin
# at the gstreamer-rs 0.25 family targeted 1.26 specifically; the
# 0.24 downgrade undoes that coupling so Debian-native builds with
# the distro's 1.24 just work.) Tracked in docs/voice.md
# §5.B and docs/rust/glib-interop.md "GStreamer runtime floor".
#
# Plugin set:
#   gstreamer-1.0       — core pipeline / element framework
#   gstreamer-app-1.0   — appsrc/appsink for in-process data exchange
#   gstreamer-audio-1.0 — audio caps, GstAudioInfo, format helpers
#   gstreamer-webrtc-1.0— webrtcbin (Phase 8.C consumer)
#
# Phase 8.B's loopback test only exercises core + audio (audiotestsrc
# ! mulawenc ! mulawdec ! fakesink). The other two pkg-config probes
# fail early if the Phase 8.C dep graph isn't present on this host;
# better a clear "package not found" at configure time than a cryptic
# Rust link error mid-build.
#
# Optional since the -Dvoice feature option (default 'auto'): each
# probe is `required : voice_opt`, so with -Dvoice=auto a missing
# GStreamer stack just resolves voice off (not-found, no error), with
# -Dvoice=enabled it's a hard configure error, and with -Dvoice=disabled
# the probes are skipped entirely. `voice_enabled` below is the single
# boolean every voice-gated build decision keys off — it drives the
# HAVE_VOICE define (src/meson.build), the hxvoice/hxvoice-runtime crate
# build (rust/meson.build), and the voice C sources + test targets.
voice_opt = get_option('voice')
gstreamer_dep        = dependency('gstreamer-1.0',        version : '>= 1.20', required : voice_opt)
gstreamer_app_dep    = dependency('gstreamer-app-1.0',    version : '>= 1.20', required : voice_opt)
gstreamer_audio_dep  = dependency('gstreamer-audio-1.0',  version : '>= 1.20', required : voice_opt)
# gstreamer-webrtc-1.0 lives in gst-plugins-bad on most distros and on
# the GNOME 49 Flatpak runtime. Phase 8.B's loopback pipeline
# (audiotestsrc ! mulawenc ! mulawdec ! fakesink) doesn't actually
# consume webrtcbin, but the runtime crate's Cargo.toml pulls in
# gstreamer-sdp / gstreamer-webrtc anyway so the resolver locks
# their Cargo.lock entries — the Phase 8.C build that flips
# webrtcbin on doesn't churn dependent crate versions. Build host
# needs gst-plugins-bad-dev (Debian/Ubuntu) or
# gstreamer1-plugins-bad-free-devel (Fedora) by configure time.
gstreamer_sdp_dep    = dependency('gstreamer-sdp-1.0',    version : '>= 1.20', required : voice_opt)
gstreamer_webrtc_dep = dependency('gstreamer-webrtc-1.0', version : '>= 1.20', required : voice_opt)

# Voice is on only when the whole stack resolved. With -Dvoice=enabled
# the dependency() calls above would already have errored out on any
# missing piece, so this is just the 'auto' all-or-nothing fold.
voice_enabled = gstreamer_dep.found() and gstreamer_app_dep.found() \
                and gstreamer_audio_dep.found() and gstreamer_sdp_dep.found() \
                and gstreamer_webrtc_dep.found()

# Glycin migration G.2: the hx-image-decode Rust crate pulls in
# glycin → gufo / lcms2 / libseccomp / fontconfig as transitive
# system deps. The Rust crate's build emits cargo:rustc-link-lib
# directives for these, but the Meson custom_target that produces
# the staticlib doesn't propagate those flags to the C-side
# executable link line — so the C-side `dependencies` list has to
# include them explicitly. Versions follow the Debian/Ubuntu LTS
# floor + the GNOME 48+ Flatpak runtime.
#
# All three are Linux-only: hx-image-decode links glycin ONLY on Linux (glycin
# is Linux-only — glycin-common uses the memfd syscall + a seccomp sandbox), and
# off-Linux it decodes with the pure-Rust `image` crate, which needs none of
# these C libraries. So require them on Linux and skip them elsewhere (a
# not-found placeholder the deps list below ignores, same as the optional
# viewers). This is what lets the ports.yml macOS/Windows app build configure.
if host_machine.system() == 'linux'
  fontconfig_dep = dependency('fontconfig', version : '>= 2.13')
  lcms2_dep      = dependency('lcms2',      version : '>= 2.14')
  libseccomp_dep = dependency('libseccomp', version : '>= 2.5')
else
  fontconfig_dep = dependency('fontconfig', required : false)
  lcms2_dep      = dependency('lcms2',      required : false)
  libseccomp_dep = dependency('libseccomp', required : false)
endif

# R3 item 5 (June 2026): the URL-mode banner fetcher moved off
# libsoup-3 to a Rust hxnet banner fetch (a blocking ureq GET on the
# tokio blocking pool — see rust/crates/hxnet/src/banner_http.rs and
# banner.c). The HTTP client now rides the hxnet staticlib the binary
# already links, so libsoup is no longer a dependency and URL-mode is
# unconditional (no HAVE_LIBSOUP gate / click-to-open fallback).

# Phase 5+: optional preview viewers driven by the type-aware
# dispatcher in src/preview.c.
#
#   poppler-glib   — PDF preview. Used to decode a PDF byte stream
#                    into rendered cairo surfaces; we display the
#                    first few pages as stacked GtkPicture rows.
#   gtksourceview  — Syntax-highlighted source-code preview for
#                    Markdown, C, Python, Rust, JavaScript, JSON,
#                    etc. GtkSourceBuffer drives a GtkSourceView
#                    (a GtkTextView subclass), language guessed
#                    from filename via GtkSourceLanguageManager.
#
# Both are present in the GNOME 49 Flatpak runtime — no Flatpak
# module needed. Both optional: if the library isn't on the build
# host, the corresponding viewer is compiled out and the file
# would fall through to the plain-text viewer. The HAVE_POPPLER /
# HAVE_GTKSOURCEVIEW defines below gate the dispatcher entries
# inside src/preview.c.
poppler_dep    = dependency('poppler-glib',     required : false)
sourceview_dep = dependency('gtksourceview-5',  required : false)

# Optional: ImageMagick (MagickWand 7) — used by src/pict_magick.c
# as the heavy fallback for QuickDraw PICT files that the cheap
# embedded-image sniff in src/pict_embed.c can't handle (i.e.,
# PICTs that use classic PackBits/DirectBits raster opcodes rather
# than wrapping a JPEG/PNG/GIF/TIFF). PICT files are still common
# on Hotline servers — they're what classic Mac OS screencapture
# produced for ~20 years.
#
# When not installed, PICT preview degrades to "Failed to decode
# image" for non-embedded PICTs; embedded-image PICTs still work
# via the sniff path. The HAVE_IMAGEMAGICK define gates the
# pict_magick.c source out of the build entirely.
#
# Not in the GNOME 49 Flatpak runtime; the Flatpak manifest builds
# ImageMagick as a module. Distro packages on Debian/Ubuntu/Fedora
# are libmagickwand-dev / ImageMagick-devel respectively.
imagemagick_dep = dependency('MagickWand', required : false)

# Optional: only present on systems that don't fold libintl into libc.
intl_dep    = dependency('intl', required : false)
# libdl is in libc on glibc >= 2.34; harmless if missing there.
dl_dep      = cc.find_library('dl', required : false)
math_dep    = cc.find_library('m',  required : false)

deps = [
  gtk_dep,
  adw_dep,
  panel_dep,
  glib_dep,
  gthread_dep,
  threads_dep,
  poppler_dep,
  sourceview_dep,
  imagemagick_dep,
  intl_dep,
  dl_dep,
  math_dep,
  # G.2 glycin migration: pulled in by the hx-image-decode Rust
  # staticlib via cargo:rustc-link-lib directives the meson
  # custom_target doesn't surface to the executable link line.
  fontconfig_dep,
  libseccomp_dep,
  lcms2_dep,
]

# macOS: hxnet's TLS stack loads the system trust store via
# rustls-native-certs, whose security-framework / core-foundation crates
# bind Apple's Security and CoreFoundation frameworks. The crates emit
# cargo:rustc-link-lib framework=... directives, but — same
# non-propagation as the glycin libs above — those never reach the C-side
# executable link line, so the frameworks have to be listed here. Security
# also provides the Authorization* symbols security-framework references.
if host_machine.system() == 'darwin'
  deps += dependency('appleframeworks', modules : ['Security', 'CoreFoundation'])
endif

# Voice GStreamer system libs link into the executable only when voice
# is compiled in. The Rust hxvoice-runtime staticlib references their
# symbols; with voice off the crate isn't built (rust/meson.build) so
# nothing needs them on the link line.
if voice_enabled
  deps += [
    gstreamer_dep,
    gstreamer_app_dep,
    gstreamer_audio_dep,
    gstreamer_sdp_dep,
    gstreamer_webrtc_dep,
  ]
endif

# Phase R1: zlib, liblz4, and libzstd are no longer linked into the
# main binary — the HOPE transport compression dispatcher moved to
# the Rust hxcrypto::compress module (rust/crates/hxcrypto/src/compress.rs) which bundles
# flate2 + lz4_flex + zstd internally.

# -----------------------------------------------------------------------------
# Subdirs
# -----------------------------------------------------------------------------
subdir('rust')
subdir('src')
subdir('po')

# -----------------------------------------------------------------------------
# Data: sounds and man page
# -----------------------------------------------------------------------------
install_data(
  files(
    'sounds/chatinvite.wav',
    'sounds/chatpost.wav',
    'sounds/error.wav',
    'sounds/filedone.wav',
    'sounds/join.wav',
    'sounds/logged-in.wav',
    'sounds/message.wav',
    'sounds/newspost.wav',
    'sounds/part.wav',
    'sounds/voice_join.wav',
    'sounds/voice_leave.wav',
  ),
  install_dir : get_option('datadir') / 'gtkhx' / 'sounds',
)

# Phase 5: the legacy autotools build dropped icons.rsrc at the top of
# datadir/gtkhx/. Auto-discovery in init_icons() scans datadir/gtkhx/icons/
# now, so install the resource into the new directory. Pre-existing
# installs at the legacy top-level location also still work because
# init_icons() scans datadir/gtkhx/ as a fallback.
install_data(
  files('icons.rsrc'),
  install_dir : get_option('datadir') / 'gtkhx' / 'icons',
)

install_man('gtkhx.1')

# -----------------------------------------------------------------------------
# Desktop integration: .desktop file + hicolor app icon.
#
# Together with the GApplication app-id ("com.nasledov.gtkhx") set in
# gtkhx.c::loop, these let GNOME Shell / KWin / etc. associate running
# windows with the .desktop entry — the Wayland dock / alt-tab / window
# list pick up the icon automatically.
#
# The same XPM is also bundled in the gresource (see
# src/gtkhx.gresource.xml) so the in-process gtk_icon_theme_add_
# resource_path lookup in gtkhx_activate works without `meson install`,
# i.e. in dev runs straight from the build dir.
# -----------------------------------------------------------------------------
desktop_file = files('data/com.nasledov.gtkhx.desktop')

install_data(
  desktop_file,
  install_dir : get_option('datadir') / 'applications',
)

# Install the app icon at every size we have a PNG for. PNG (not
# XPM) because most modern Wayland docks / GNOME Shell only honor
# PNG and SVG even though the hicolor spec technically allows XPM.
# rename: keeps the in-tree filenames decoupled from the install
# names — the install side has to be com.nasledov.gtkhx.png to match
# the .desktop file's Icon= field. Sizes 16-256 cover menus through
# the GNOME activities overview; the dock picks the closest match.
foreach sz : ['16', '32', '48', '64', '128', '256']
  install_data(
    files('src/pixmaps/gtkhx_icon_' + sz + '.png'),
    install_dir : get_option('datadir') / 'icons' / 'hicolor'
                  / (sz + 'x' + sz) / 'apps',
    rename : 'com.nasledov.gtkhx.png',
  )
endforeach

# Validate the .desktop file at test time if the freedesktop validator
# is available (it ships with the desktop-file-utils package on most
# distros). Catches typos in Categories / Keywords / KeyName casing
# before they ship.
desktop_validate = find_program('desktop-file-validate', required : false)
if desktop_validate.found()
  test(
    'validate-desktop',
    desktop_validate,
    args : [desktop_file],
  )
endif

# AppStream metainfo. Software centers (GNOME Software, KDE
# Discover, elementary AppCenter) and Flathub's review pipeline
# read this file to render the in-store listing — name, summary,
# description, screenshots, releases, OARS content rating. The
# id MUST match com.nasledov.gtkhx; the launchable's desktop-id
# MUST match the installed .desktop filename above. Mismatches
# get silently dropped from search results, which is hard to
# diagnose after the fact.
metainfo_file = files('data/com.nasledov.gtkhx.metainfo.xml')

install_data(
  metainfo_file,
  install_dir : get_option('datadir') / 'metainfo',
)

# Validate the metainfo at test time if appstreamcli is on the
# build host. Catches schema typos, missing required fields, OARS
# mistakes, and broken release entries before submission.
appstreamcli = find_program('appstreamcli', required : false)
if appstreamcli.found()
  test(
    'validate-metainfo',
    appstreamcli,
    args : ['validate', '--no-net', '--pedantic', metainfo_file],
  )
endif

# Refresh the gtk-update-icon-cache and update-desktop-database state
# after `meson install` so the freshly-installed icon and .desktop
# entry are visible to the desktop session without a logout.
#
# Each action is gated on its tool actually being present: meson's
# gnome.post_install() resolves the helper program at configure time and
# hard-errors when it's missing (it does NOT silently no-op). desktop-file-utils
# (update-desktop-database) and gtk-update-icon-cache aren't packaged on macOS /
# MSYS2, so probe with required:false and only ask post_install to run what
# exists. On Linux both are present and behavior is unchanged.
gnome = import('gnome')
update_desktop_db_prog = find_program('update-desktop-database', required : false)
update_icon_cache_prog = find_program(
  'gtk4-update-icon-cache', 'gtk-update-icon-cache', required : false,
)
gnome.post_install(
  gtk_update_icon_cache    : update_icon_cache_prog.found(),
  update_desktop_database  : update_desktop_db_prog.found(),
)

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
summary(
  {
    'GTK'           : gtk_dep.version(),
    'GLib'          : glib_dep.version(),
    'libadwaita'    : adw_dep.version(),
    'libpanel'      : panel_dep.version(),
    'Voice (GStreamer)' : voice_enabled ? gstreamer_dep.version() : 'no',
    'poppler-glib'  : poppler_dep.found()    ? poppler_dep.version()    : 'no',
    'gtksourceview-5' : sourceview_dep.found() ? sourceview_dep.version() : 'no',
    'ImageMagick'   : imagemagick_dep.found() ? imagemagick_dep.version() : 'no',
  },
  section : 'Dependencies',
)

# -----------------------------------------------------------------------------
# Test suite. See tests/PROPOSAL.md for the overall plan.
# Default-on; pass -Dtests=false to skip.
# -----------------------------------------------------------------------------
if get_option('tests')
  subdir('tests')
endif

summary(
  {
    'Tests'      : get_option('tests'),
  },
  section : 'Build',
)
