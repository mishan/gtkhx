gtkhx_sources = files(
  'about.c',
  'chat.c',
  'cicn.c',
  'cipher.c',
  'commands.c',
  'compress.c',
  'connect.c',
  'dfa.c',
  'files.c',
  'gtk_hlist.c',
  'gtkhx.c',
  'gtkthreads.c',
  'gtkutil.c',
  'hfs.c',
  'history.c',
  'hmac.c',
  'log.c',
  'macres.c',
  'msg.c',
  'network.c',
  'news.c',
  'news15.c',
  'options.c',
  'plugin.c',
  'preview.c',
  'rand.c',
  'rcv.c',
  'sound.c',
  'tasks.c',
  'toolbar.c',
  'tracker.c',
  'usermod.c',
  'users.c',
  'xfers.c',
  'xtext.c',
)

# -----------------------------------------------------------------------------
# Generated config.h — replaces autotools' AC_DEFINE machinery.
#
# The HAVE_* probes from the old configure.in are gone: snprintf, vsnprintf,
# localtime_r, basename, strcasestr, getopt_long, getaddrinfo are all POSIX
# now and present on every target we care about. dlopen is in libc on modern
# glibc; we still link -ldl conditionally for older systems.
# -----------------------------------------------------------------------------
conf = configuration_data()
conf.set_quoted('PACKAGE',            meson.project_name())
conf.set_quoted('VERSION',            meson.project_version())
conf.set_quoted('GETTEXT_PACKAGE',    meson.project_name())
conf.set_quoted('PACKAGE_LOCALE_DIR', get_option('prefix') / get_option('localedir'))

# Headers/feature defines historically probed via autoconf. All universally
# present on the targets we care about (any modern glibc/musl/BSD); we set
# them unconditionally so the legacy `#ifdef HAVE_FOO` gates in vendored
# code (notably dfa.c) take the right branch without us having to edit
# 2,500 lines of GNU regex.
conf.set('STDC_HEADERS',    1)
conf.set('HAVE_STRING_H',   1)
conf.set('HAVE_STDLIB_H',   1)
conf.set('HAVE_LIMITS_H',   1)
conf.set('HAVE_LIBINTL_H',  1)

# IPv6 was conditional via --disable-ipv6; getaddrinfo is universal now.
conf.set('USE_IPV6',       1)

# Compression and cipher were configure-time toggles; both are required now.
conf.set('CONFIG_COMPRESS', 1)
conf.set('CONFIG_CIPHER',   1)

# IDEA cipher was always disabled in the old build (patent encumbered at the
# time, never wired in to HOPE negotiation in the wild).
conf.set('CONFIG_NO_IDEA',  1)

# X11 assumptions baked into gtkhx.c colormap code etc. GTK 2/3 are X11;
# Phase 4 (GTK 4 / Wayland) revisits this define.
conf.set('USE_XLIB',        1)

# gdk-pixbuf was conditional in 1.2 era; required for any GTK >= 2.
conf.set('USE_GDK_PIXBUF',  1)

# Plugin loader (dlopen) compiles but won't find any plugins — the API is
# slated for removal, see ROADMAP. Set to 0 once plugin.c is excised.
conf.set('USE_PLUGIN',      1)

configure_file(
  output        : 'config.h',
  configuration : conf,
)

executable('gtkhx',
  gtkhx_sources,
  dependencies        : deps,
  include_directories : include_directories('.'),
  install             : true,
)
