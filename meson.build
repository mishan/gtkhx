project('gtkhx', 'c',
  version : '0.9.5-dev',
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
# audio file fire-and-forget, in-process, without spawning a player. If
# the build host doesn't have it, sound.c falls back to the legacy
# fork+exec path automatically — see HAVE_GSOUND below.
gsound_dep = dependency('gsound', required : false)
if gsound_dep.found()
  add_project_arguments('-DHAVE_GSOUND=1', language : 'c')
endif

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
# Summary
# -----------------------------------------------------------------------------
summary(
  {
    'GTK'        : gtk_dep.version(),
    'libadwaita' : adw_dep.version(),
    'GLib'       : glib_dep.version(),
    'Nettle'     : nettle_dep.version(),
    'zlib'       : zlib_dep.version(),
    'GSound'     : gsound_dep.found() ? gsound_dep.version() : 'no (sounds will fork an external player)',
  },
  section : 'Dependencies',
)
