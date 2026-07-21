gtkhx_sources = files(
  # about.c ported to Rust (gtkhx-ui about.rs); about.h keeps the C ABI decl.
  'algo_list.c',
  'banner.c',
  'banner_dispatch.c',
  'bookmark_cipher.c',
  # bookmark_rc4_dialog.c ported to Rust (gtkhx-ui rc4_dialog.rs).
  # bookmarks.c (management dialog) ported to Rust (gtkhx-ui bookmarks.rs);
  # bookmarks.h keeps the C ABI decls. CRUD stays in bookmarks_io.c.
  'bookmarks_io.c',
  # cipher_vocab.c folded into the Rust gtkhx-ui crate (cipher_vocab.rs) when
  # the Bookmarks dialog joined the Connect dialog in Rust (R5.3).
  'chat.c',
  # chat_send_bridge.c — model/caps shims for the Rust chat wire-senders
  # (hxchat-send crate; the hx_send_chat / hx_chat_* senders moved out of
  # chat.c). Links via rust_hxchat_send_dep below.
  'chat_send_bridge.c',
  'chat_history.c',
  'cicn.c',
  'gif_avatar.c',
  'gif_icons.c',
  'commands.c',
  # connect.c ported to Rust (gtkhx-ui connect.rs); connect.h keeps the C ABI
  # decls. The shared cipher/compress vocab is in the Rust crate too
  # (cipher_vocab.rs), as is the Bookmarks dialog (bookmarks.rs).
  'debug.c',
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
  # history.c (GNU readline command-line history) retired — the chat + PM input
  # line history is now the Rust InputHistory (hxchat-model), reached via
  # hx_input_history_* in chat_members.rs.
  'host_port.c',
  'hotline_url.c',
  'htxf_io.c',
  'htxf_subchannel.c',
  'inline_media.c',
  'inline_media_attach.c',
  'inline_media_decode.c',
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
  # news.c + news15.c + news_browser.c + news_send_bridge.c all retired — the
  # 1.5 news wire senders live in the hxnews-send Rust crate, the reply
  # handlers + carriers in hxnews-recv, and the browser UI in the gtkhx-ui
  # `news_browser` module. news_recv_bridge.c is the last C leaf: htlc->in
  # bytes, htlc version+access, the post-date formatter, and the row-icon loader.
  'news_recv_bridge.c',
  'notify.c',
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
  'sound_events.c',
  'tasks.c',
  'tasks_table.c',
  # text_util.c ported to Rust (hxtext crate, Phase R5); text_util.h keeps the
  # C ABI decls the ~16 callers include. Links via rust_hxtext_dep below.
  'tls_trust.c',
  # tls_trust_dialog.c ported to Rust (gtkhx-ui tls_trust_dialog.rs);
  # tls_trust_dialog.h keeps the C ABI decl network.c calls.
  'toolbar.c',
  # tracker.c + tracker_row.c ported to Rust (gtkhx-ui crate, Phase R5).
  # tracker_bridge.c is the thin session/prefs shim the Rust window calls.
  'tracker_bridge.c',
  # dock_bridge.c — generic libpanel dock-embed shim for the docked Rust
  # window ports (Users first). See docs/rust/dock-porting-scoping.md.
  'dock_bridge.c',
  # users_bridge.c — C content leaves (HxUserListView + action buttons +
  # voice panel) for the Rust Users window shell (gtkhx-ui `users`).
  'users_bridge.c',
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
  # users_row.c ported to Rust (gtkhx-ui `user_row`, Phase R5.8); its
  # hx_user_row_* C ABI now resolves against the crate.
  # users_view.c ported to Rust (gtkhx-ui `users_view`, Phase R5.9); its
  # hx_user_list_view_* C ABI resolves against the crate. The custom Name
  # cell stays C; the voice column moved to gtkhx-ui's `voice` feature
  # (users_voice_col.rs).
  'users_cell.c',
  'xfers.c',
  'xfers_recv.c',
  'xfers_send.c',
  'xtext.c',
)

# Voice chat (fogWraith WebRTC extension) C sources. Compiled in only
# when voice_enabled (see -Dvoice / the top-level meson.build). When off,
# these files are dropped, the HAVE_VOICE define below is unset, and every
# call site in the always-compiled files is guarded by #ifdef HAVE_VOICE.
# voice_bridge.c     — session/htlc field accessors for the Rust voice UI +
#                      senders (users_voice_col + voice_panel + voice_ptt to
#                      gtkhx-ui's `voice` feature; the hx_send_voice_* senders
#                      to the hxvoice-send crate).
# voice_ptt_keyspec.c— pure PTT key-spec vocabulary / canonicalisation (keeps
#                      its standalone C unit test; voice_ptt.rs externs _parse).
# (voice_panel.c retired — ported to rust/crates/gtkhx-ui/src/voice_panel.rs.
#  voice_model.c retired — ported to the hxvoice-model Rust crate.
#  voice_ptt.c retired — ported to rust/crates/gtkhx-ui/src/voice_ptt.rs.
#  voice.c retired — ported to the hxvoice-send Rust crate.)
voice_sources = files(
  'voice_bridge.c',
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
  # chat wire-out senders (Rust port of chat.c's send path). Must precede
  # rust_hxtext_dep on the link line: hxchat-send has an undefined ref to
  # gtkhx_text_for_wire that the later hxtext archive resolves. Same
  # static-archive rule (consumer before provider) as gtkhx-ui → gtkhx-boxed
  # above; the C callers of gtkhx_text_for_wire happen to pull it first today,
  # but don't rely on that as they get ported.
  rust_hxchat_send_dep,
  # news wire senders (flat 1.0/1.2 + 1.5 threaded) — Rust port of news.c +
  # news15.c. Before rust_hxtext_dep on the link line (undefined ref to
  # gtkhx_text_for_wire), same consumer-before-provider rule as hxchat-send.
  rust_hxnews_send_dep,
  # 1.5 news receive handlers (rcv_task_news* moved out of rcv.c). After
  # hxnews-send: it resolves the rcv_task_newscat_list ref hxnews-send registers.
  rust_hxnews_recv_dep,
  # pure post-threading layout for the 1.5 news browser (news_browser.c).
  rust_hxnews_model_dep,
  # files-subsystem model: file-type -> icon-id map behind
  # files.c::icon_of_ftype_and_name.
  rust_hxfiles_model_dep,
  # files-subsystem transfer codec: FFO fork-header + info-block byte
  # math behind xfers.c::file_recv_one.
  rust_hxfiles_xfer_dep,
  # cicn colour-icon decoder (cicn.c's decode path retired) — hxcicn_decode,
  # wrapped into a GdkPixbuf by the thin cicn.c.
  rust_hxcicn_dep,
  # text encoding + emoji-shortcode helpers (Rust port of text_util.c).
  rust_hxtext_dep,
]
if voice_enabled
  rust_deps += rust_hxvoice_runtime_dep
  # HxVoiceModel moved to the hxvoice-model Rust crate (voice_model.c retired).
  rust_deps += rust_hxvoice_model_dep
  # hx_send_voice_* moved to the hxvoice-send Rust crate (voice.c retired).
  rust_deps += rust_hxvoice_send_dep
endif

executable('gtkhx',
  gtkhx_sources,
  gtkhx_resources,
  dependencies        : deps + rust_deps,
  include_directories : include_directories('.'),
  install             : true,
)
