project('gtkhx', 'c',
  version : '1.0.0',
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
#
# GTK pin advances per phase: 2 -> 3 -> 4. Phase 4 is in progress; floor is
# 4.6 (the GtkApplication and snapshot APIs we lean on are stable from there).
# Per ROADMAP: Nettle for legacy Hotline ciphers (Blowfish/RC4) + GLib hashes;
# no GnuTLS (Phase ∞ work, requires server-side cooperation).
# -----------------------------------------------------------------------------
gtk_dep     = dependency('gtk4',      version : '>= 4.6')
# Phase 5: libadwaita gives us AdwApplication (color-scheme tracking),
# AdwPreferencesWindow (modern Settings layout), AdwHeaderBar, AdwToast,
# AdwAboutDialog, AdwAlertDialog. 1.6 is the floor that ships the new
# adaptive AdwDialog family (AdwAboutDialog / AdwAlertDialog /
# AdwPreferencesDialog) — the old AdwAboutWindow / AdwMessageDialog
# became deprecated in 1.6 and we pick up -Werror=deprecated-declarations
# from the build flags. 1.6 was released April 2024; widely available.
adw_dep     = dependency('libadwaita-1', version : '>= 1.6')
glib_dep    = dependency('glib-2.0',  version : '>= 2.56')
gthread_dep = dependency('gthread-2.0')
zlib_dep    = dependency('zlib')
nettle_dep  = dependency('nettle',    version : '>= 3.0')
threads_dep = dependency('threads')

# Phase 5: GSound replaces the legacy fork+execlp("play") path in sound.c.
# It's a thin GLib-style wrapper over libcanberra that plays an arbitrary
# audio file fire-and-forget, in-process. Required — Misha confirmed
# GSound is the canonical path; the fallback fork+exec code was retired.
gsound_dep = dependency('gsound')

# Phase 5: libsoup-3 powers the URL-mode banner fetcher in banner.c.
# Modern Hotline servers can advertise a banner via HTLS_HDR_BANNER as
# either an http(s) URL (URL-mode) or as bytes pulled over the HTXF
# subchannel (file-mode). Both modes display in the toolbar's banner
# row; URL-mode needs an HTTP client to actually retrieve the bytes.
#
# Optional: when libsoup-3 isn't on the build host, banner.c falls
# back to a click-to-open caption for URL-mode banners — the URL is
# shown as a clickable line that opens in the user's default browser.
# File-mode (HTXF) is unaffected. The HAVE_LIBSOUP define gates the
# fetch / decode code path inside banner.c.
soup_dep = dependency('libsoup-3.0', version : '>= 3.0',
                      required : false)

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

# Optional: LZ4 and Zstandard — HOPE-Secure-Login transport
# compression algorithms (Janus/Klein hx-family server extension).
# Negotiated alongside GZIP during the HOPE identification phase;
# servers that don't speak either fall through to GZIP or none. The
# HAVE_LZ4 / HAVE_ZSTD defines below gate the dispatcher entries
# inside src/compress.c and the algorithm entries in
# src/connect.c::valid_compressors[].
#
# Both available as Debian/Ubuntu liblz4-dev / libzstd-dev and
# Fedora lz4-devel / libzstd-devel. Neither ships in the GNOME 49
# Flatpak runtime / SDK — com.nasledov.gtkhx.yml builds both as
# inline modules so the Flatpak binary speaks LZ4 and ZSTD too.
lz4_dep  = dependency('liblz4',  required : false)
zstd_dep = dependency('libzstd', required : false)

# Optional: only present on systems that don't fold libintl into libc.
intl_dep    = dependency('intl', required : false)
# libdl is in libc on glibc >= 2.34; harmless if missing there.
dl_dep      = cc.find_library('dl', required : false)
math_dep    = cc.find_library('m',  required : false)

deps = [
  gtk_dep,
  adw_dep,
  glib_dep,
  gthread_dep,
  zlib_dep,
  nettle_dep,
  threads_dep,
  gsound_dep,
  soup_dep,
  poppler_dep,
  sourceview_dep,
  imagemagick_dep,
  lz4_dep,
  zstd_dep,
  intl_dep,
  dl_dep,
  math_dep,
]

# -----------------------------------------------------------------------------
# Subdirs
# -----------------------------------------------------------------------------
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
# entry are visible to the desktop session without a logout. No-ops if
# the tools are missing.
gnome = import('gnome')
gnome.post_install(
  gtk_update_icon_cache    : true,
  update_desktop_database  : true,
)

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
summary(
  {
    'GTK'           : gtk_dep.version(),
    'libadwaita'    : adw_dep.version(),
    'GLib'          : glib_dep.version(),
    'Nettle'        : nettle_dep.version(),
    'zlib'          : zlib_dep.version(),
    'GSound'        : gsound_dep.version(),
    'libsoup-3.0'   : soup_dep.found()       ? soup_dep.version()       : 'no',
    'poppler-glib'  : poppler_dep.found()    ? poppler_dep.version()    : 'no',
    'gtksourceview-5' : sourceview_dep.found() ? sourceview_dep.version() : 'no',
    'ImageMagick'   : imagemagick_dep.found() ? imagemagick_dep.version() : 'no',
    'liblz4'        : lz4_dep.found()         ? lz4_dep.version()         : 'no',
    'libzstd'       : zstd_dep.found()        ? zstd_dep.version()        : 'no',
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
