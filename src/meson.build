gtkhx_sources = files(
  # about.c ported to Rust (gtkhx-ui about.rs); about.h keeps the C ABI decl.
  'algo_list.c',
  'banner.c',
  'banner_dispatch.c',
  'bookmark_cipher.c',
  'bookmark_rc4_dialog.c',
  'bookmarks.c',
  'bookmarks_io.c',
  'cipher_vocab.c',
  'chat.c',
  'chat_history.c',
  'chat_tabs.c',
  'cicn.c',
  'gif_avatar.c',
  'gif_icons.c',
  'commands.c',
  # connect.c ported to Rust (gtkhx-ui connect.rs); connect.h keeps the C ABI
  # decls. The shared cipher/compress vocab moved to cipher_vocab.c.
  'debug.c',
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
  'dock_layout.c',
  'dock_layout_parse.c',
  'gtkhx.c',
  'gtkhx_icon.c',
  'gtkhx_log.c',
  # gtkhx_session.c removed in Phase R4 — GtkhxSession is now the Rust
  # glib::subclass crate `gtkhx-session` (rust_gtkhx_session_dep below).
  'gtkhx_theme.c',
  'hx_panel.c',
  'hx_panel_frame.c',
  'hx_split.c',
  'panel_registry.c',
  'gtkurl.c',
  'gtkutil.c',
  'hfs.c',
  'history.c',
  'host_port.c',
  'hotline_url.c',
  'htxf_io.c',
  'htxf_subchannel.c',
  'inline_media.c',
  'inline_media_attach.c',
  'inline_media_decode.c',
  'inline_media_dialog.c',
  'inline_media_download.c',
  'inline_media_upload.c',
  'hl_code.c',
  'hl_date.c',
  'human_readable.c',
  'log.c',
  'login_packet.c',
  'agreement_packet.c',
  'macres.c',
  'msg.c',
  'network.c',
  'hxnet_bridge.c',
  'news.c',
  'news_browser.c',
  'notify.c',
  'news15.c',
  'options.c',
  'path_hldir.c',
  'path_util.c',
  'pict_embed.c',
  'pict_magick.c',
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
  'tls_trust.c',
  'tls_trust_dialog.c',
  'toolbar.c',
  # tracker.c + tracker_row.c ported to Rust (gtkhx-ui crate, Phase R5).
  # tracker_bridge.c is the thin session/prefs shim the Rust window calls.
  'tracker_bridge.c',
  # gtkhx_ui_bridge.c — session shim for the other Rust windows (agreement /
  # user editor). usermod.c is now the wire senders + access table only.
  'gtkhx_ui_bridge.c',
  'tracker_event.c',
  'tracker_parser.c',
  'tracker_v3.c',
  'tracker_v3_meta.c',
  'tray.c',
  'uniquify_path.c',
  'usermod.c',
  'users.c',
  'users_row.c',
  'users_view.c',
  'xfers.c',
  'xtext.c',
)

# Voice chat (fogWraith WebRTC extension) C sources. Compiled in only
# when voice_enabled (see -Dvoice / the top-level meson.build). When off,
# these files are dropped, the HAVE_VOICE define below is unset, and every
# call site in the always-compiled files is guarded by #ifdef HAVE_VOICE.
# voice.c            — wire-out helpers (hx_send_voice_*).
# voice_model.c      — per-uid voice presence/state GObject (no GStreamer).
# voice_panel.c      — per-chat-tab Join/Leave/Mute toolbar + runtime wire.
# voice_ptt.c        — push-to-talk key controller.
# voice_ptt_keyspec.c— pure PTT key-spec vocabulary / canonicalisation.
voice_sources = files(
  'voice.c',
  'voice_model.c',
  'voice_panel.c',
  'voice_ptt.c',
  'voice_ptt_keyspec.c',
)
if voice_enabled
  gtkhx_sources += voice_sources
endif

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
# them unconditionally. Previously these were also load-bearing for the
# vendored dfa.c's `#ifdef HAVE_FOO` gates — that file has been replaced
# by GLib's GRegex (PCRE2) so the constraint is gone, but the defines are
# still useful for the few HAVE_LIBINTL_H consumers etc.
conf.set('STDC_HEADERS',    1)
conf.set('HAVE_STRING_H',   1)
conf.set('HAVE_STDLIB_H',   1)
conf.set('HAVE_LIMITS_H',   1)
conf.set('HAVE_LIBINTL_H',  1)

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

# Voice chat. Gates the voice_*.c sources (above) and every voice call
# site in the always-compiled files (gtkhx.c, chat.c, toolbar.c,
# options.c, users_view.c, network.c, rcv.c). Driven by -Dvoice +
# the GStreamer probe in the top-level meson.build.
if voice_enabled
  conf.set('HAVE_VOICE', 1)
endif

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

# The hxvoice-runtime staticlib is linked only when voice is compiled in;
# with voice off the crate isn't built (rust/meson.build), so referencing
# -lhxvoice_runtime here would fail at link time.
rust_deps = [
  rust_hxcrypto_hash_dep,
  rust_hxcrypto_aead_dep,
  rust_hxcrypto_stream_dep,
  rust_hxcompress_dep,
  rust_hotline_proto_dep,
  rust_hxbridge_dep,
  rust_gtkhx_session_dep,
  # gtkhx-ui (Phase R5 windows) must precede gtkhx-boxed on the link line:
  # it has undefined refs to hx_tracker_v3_meta_copy/_free which the later
  # gtkhx-boxed archive resolves.
  rust_gtkhx_ui_dep,
  rust_gtkhx_boxed_dep,
  rust_hxnet_dep,
  rust_hx_image_decode_dep,
]
if voice_enabled
  rust_deps += rust_hxvoice_runtime_dep
endif

executable('gtkhx',
  gtkhx_sources,
  gtkhx_resources,
  dependencies        : deps + rust_deps,
  include_directories : include_directories('.'),
  install             : true,
)
