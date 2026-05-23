gtkhx_sources = files(
  'about.c',
  'algo_list.c',
  'banner.c',
  'chat.c',
  'chat_history.c',
  'cicn.c',
  'cipher.c',
  'cipher_aead.c',
  'commands.c',
  'compress.c',
  'connect.c',
  'debug.c',
  'dfa.c',
  'emoji.c',
  'filelist_walker.c',
  'files.c',
  'files_browser.c',
  'files_complete.c',
  'files_entry.c',
  'files_local_provider.c',
  'files_ops.c',
  'files_panel.c',
  'files_provider.c',
  'files_remote_provider.c',
  'gtk_hlist_compat.c',
  'gtkhx.c',
  'gtkhx_log.c',
  'gtkhx_session.c',
  'gtkthreads.c',
  'gtkurl.c',
  'gtkutil.c',
  'hfs.c',
  'history.c',
  'hope.c',
  'htxf_io.c',
  'htxf_subchannel.c',
  'hl_code.c',
  'hl_date.c',
  'human_readable.c',
  'hmac.c',
  'log.c',
  'login_packet.c',
  'agreement_packet.c',
  'macres.c',
  'msg.c',
  'network.c',
  'news.c',
  'news_browser.c',
  'notify.c',
  'news15.c',
  'options.c',
  'path_hldir.c',
  'path_util.c',
  'pict_embed.c',
  'pict_magick.c',
  'plugin.c',
  'prefs_parser.c',
  'preview.c',
  'proto_helpers.c',
  'proto_trace.c',
  'qbuf.c',
  'rand.c',
  'rcv.c',
  'sound.c',
  'tasks.c',
  'tasks_table.c',
  'text_util.c',
  'toolbar.c',
  'tracker.c',
  'tray.c',
  'uniquify_path.c',
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

# libsoup-3 is optional. When found, banner.c builds the URL-mode
# fetcher that decodes server banners inline; without it, URL-mode
# banners render as a clickable line that opens in the user's
# default browser. File-mode banners (HTXF subchannel) are
# unaffected either way.
if soup_dep.found()
  conf.set('HAVE_LIBSOUP', 1)
endif

# Phase 5+: optional preview viewers. Each define gates a single
# struct hx_viewer in src/preview.c; without it the corresponding
# file type falls through to the catch-all text viewer.
if poppler_dep.found()
  conf.set('HAVE_POPPLER', 1)
endif
if sourceview_dep.found()
  conf.set('HAVE_GTKSOURCEVIEW', 1)
endif
if imagemagick_dep.found()
  conf.set('HAVE_IMAGEMAGICK', 1)
endif

# HOPE-Secure-Login modern compression algorithms (Janus/Klein
# server extension to the original GZIP). When the library is
# present we advertise it in the algorithm-negotiation list and
# dispatch into the corresponding src/compress.c branch; when
# absent the negotiation list just doesn't include it and the
# dispatcher entries compile out.
if lz4_dep.found()
  conf.set('HAVE_LZ4', 1)
endif
if zstd_dep.found()
  conf.set('HAVE_ZSTD', 1)
endif

# Plugin loader is GTK-1.2-bound (gtk_clist_*, gtk_file_selection_*,
# gtk_signal_connect, gtk_widget_set_usize). Per ROADMAP, the dlopen
# plugin API is slated for removal, so rather than rewrite it for GTK 3
# we leave USE_PLUGIN undefined. The contents of plugin.c and plugin.h
# are entirely under #ifdef USE_PLUGIN and compile to a no-op.
# conf.set('USE_PLUGIN',      1)

configure_file(
  output        : 'config.h',
  configuration : conf,
)

# -----------------------------------------------------------------------------
# Phase 3.7: bundled pixmaps as a GResource. The XML manifest lists every
# .png under src/pixmaps; glib-compile-resources turns them into a .c file
# whose registered constructor is run automatically at program load, so no
# explicit g_resources_register() call is needed in main(). Call sites use
# gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/foo.png", ...).
# -----------------------------------------------------------------------------
gnome = import('gnome')
gtkhx_resources = gnome.compile_resources(
  'gtkhx-resources',
  'gtkhx.gresource.xml',
  source_dir : '.',
  c_name     : 'gtkhx',
)

executable('gtkhx',
  gtkhx_sources,
  gtkhx_resources,
  dependencies        : deps,
  include_directories : include_directories('.'),
  install             : true,
)
