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
  '-Werror=deprecated-declarations',
]
add_project_arguments(cc.get_supported_arguments(extra_warnings), language : 'c')

# -----------------------------------------------------------------------------
# Dependencies
#
# GTK pin advances per phase: 2 -> 3 -> 4. Phase 3 is in progress; the floor is
# 3.22 (the last LTS-grade release of GTK 3, ships in every supported distro).
# Per ROADMAP: Nettle for legacy Hotline ciphers (Blowfish/RC4) + GLib hashes;
# no GnuTLS (Phase ∞ work, requires server-side cooperation).
# -----------------------------------------------------------------------------
gtk_dep     = dependency('gtk+-3.0',  version : '>= 3.22')
glib_dep    = dependency('glib-2.0',  version : '>= 2.56')
gthread_dep = dependency('gthread-2.0')
zlib_dep    = dependency('zlib')
nettle_dep  = dependency('nettle',    version : '>= 3.0')
threads_dep = dependency('threads')

# Optional: only present on systems that don't fold libintl into libc.
intl_dep    = dependency('intl', required : false)
# libdl is in libc on glibc >= 2.34; harmless if missing there.
dl_dep      = cc.find_library('dl', required : false)
math_dep    = cc.find_library('m',  required : false)

deps = [
  gtk_dep,
  glib_dep,
  gthread_dep,
  zlib_dep,
  nettle_dep,
  threads_dep,
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
    'sounds/chatinvite.aiff',
    'sounds/chatpost.aiff',
    'sounds/error.aiff',
    'sounds/filedone.aiff',
    'sounds/join.aiff',
    'sounds/logged-in.aiff',
    'sounds/message.aiff',
    'sounds/newspost.aiff',
    'sounds/part.aiff',
  ),
  install_dir : get_option('datadir') / 'gtkhx' / 'sounds',
)

install_man('gtkhx.1')

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
summary(
  {
    'GTK'    : gtk_dep.version(),
    'GLib'   : glib_dep.version(),
    'Nettle' : nettle_dep.version(),
    'zlib'   : zlib_dep.version(),
  },
  section : 'Dependencies',
)
