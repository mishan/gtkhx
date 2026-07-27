gtkhx_sources = files(
  # about.c ported to Rust (gtkhx-ui about.rs); about.h keeps the C ABI decl.
  'algo_list.c',
  'banner.c',
  'banner_dispatch.c',
  # bookmark_cipher.c + bookmarks_io.c retired — bookmark storage (TOML model,
  # legacy HTsc import/export, cipher vocab) is the hxbookmarks Rust crate,
  # reached directly from the Rust Connect + Bookmarks dialogs. bookmarks.h
  # keeps only the create_bookmarks_window decl the toolbar calls.
  # bookmark_rc4_dialog.c ported to Rust (gtkhx-ui rc4_dialog.rs).
  # bookmarks.c (management dialog) ported to Rust (gtkhx-ui bookmarks.rs).
  # cipher_vocab.c folded into the Rust gtkhx-ui crate (cipher_vocab.rs) when
  # the Bookmarks dialog joined the Connect dialog in Rust (R5.3).
  'chat.c',
  # chat_send_bridge.c — model/caps shims for the Rust chat wire-senders
  # (hxchat-send crate; the hx_send_chat / hx_chat_* senders moved out of
  # chat.c). Links via the gtkhx-ffi façade archive.
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
  # filelist_walker.c retired — the FILE_LIST walk + per-entry decode moved
  # into the hxfiles-entry Rust crate (gtkhx_files_populate_from_reply).
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
  # hfs.c retired — the HFS metadata sidecar (CAP / AppleDouble / Netatalk) is
  # now the Rust hxhfs crate (rust_hxhfs_dep below); hfs.h keeps the struct /
  # macro / C-ABI decls its callers include.
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
  'human_readable.c',
  'login_packet.c',
  'agreement_packet.c',
  'msg.c',
  'network.c',
  'hxnet_bridge.c',
  # hxconn.c retired — struct htlc_conn's accessors + lifecycle are the Rust
  # hxconn crate now (rust_hxconn_dep below); the layout is pinned by the
  # _Static_assert in protocol.h against Rust's HtlcConn.
  # news.c + news15.c + news_browser.c + news_send_bridge.c all retired — the
  # The 1.5 news domain is fully Rust: wire senders in hxnews-send, reply
  # handlers + carriers in hxnews-recv, the browser UI + post-date formatting +
  # row-icon loading in the gtkhx-ui `news_browser` / `news_render` / `hl_date`
  # modules. (news_recv_bridge.c, the last C leaf, is gone.)
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
  'rcv.c',
  'sound.c',
  'sound_events.c',
  'tasks.c',
  # tasks_table.c + the struct task model in tasks.c ported to the Rust hxtask
  # crate (via the gtkhx-ffi façade); tasks_bridge.c is its C field-accessor shim.
  'tasks_bridge.c',
  # text_util.c ported to Rust (hxtext crate, Phase R5); text_util.h keeps the
  # C ABI decls the ~16 callers include. Links via rust_hxtext_dep below.
  # tls_trust.c + tls_trust_dialog.c ported to Rust: the trust brain is the
  # hxtls-trust crate (rust_hxtls_trust_dep below) and the Adwaita prompt is
  # gtkhx-ui's tls_trust_dialog.rs; tls_trust.h keeps the surviving C ABI.
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
  # Rust-facing accessor seam over the still-C-owned htxf_conn: the field
  # getters/setters + fs-size helper the Rust file-transfer receive handlers
  # (hxxfer-recv) reach transfer state through, so the transfer logic lives in
  # Rust while the refcounted, cross-thread struct stays in xfers.c.
  'htxf_accessors.c',
  'xtext.c',
)

# /exec (chat command) + its Unix process / fd-watch / signal machinery.
# fork/pipe/dup2/execve + POSIX signals + a GIOChannel over a raw fd —
# Unix-only, so it's compiled out on Windows (where /exec is absent).
if host_machine.system() != 'windows'
  gtkhx_sources += files('cmd_exec.c')
endif

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

conf = configuration_data()
conf.set_quoted('PACKAGE',            meson.project_name())
conf.set_quoted('VERSION',            meson.project_version())
conf.set_quoted('GETTEXT_PACKAGE',    meson.project_name())
conf.set_quoted('PACKAGE_LOCALE_DIR', get_option('prefix') / get_option('localedir'))

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

# Every Rust crate reaches the binary through one archive: the gtkhx-ffi
# façade (rust/crates/gtkhx-ffi). It bundles the whole rlib graph, so each
# #[no_mangle] symbol is defined exactly once and link order is irrelevant.
#
# This replaces a 37-entry hand-ordered list. Static archives resolve
# left-to-right, so every crate had to sit ahead of the crates providing its
# externs, and each entry carried a comment justifying its position
# ("must precede gtkhx-boxed", "consumer before provider", ...). Adding a
# crate now means adding a dependency in gtkhx-ffi/Cargo.toml — nothing here.
#
# Voice needs no special case either: the voice crates enter the archive via
# gtkhx-ffi's `voice` Cargo feature, which rust/meson.build turns on with
# -Dvoice. There is no -lhxvoice_runtime to conditionally omit any more.
rust_deps = [rust_gtkhx_ffi_dep]

# Windows resource: file icon + embedded application manifest (asInvoker, no UAC)
# + version metadata. windres compiles gtkhx.rc into an object linked below.
# Guarded to Windows so nothing here touches the Linux/macOS build.
gtkhx_win_res = []
if host_machine.system() == 'windows'
  _ver = meson.project_version().split('-')[0].split('.')
  _rc_conf = configuration_data()
  _rc_conf.set('VER_MAJOR',   _ver.get(0, '0'))
  _rc_conf.set('VER_MINOR',   _ver.get(1, '0'))
  _rc_conf.set('VER_PATCH',   _ver.get(2, '0'))
  _rc_conf.set('VERSION_STR', meson.project_version())
  _gtkhx_rc = configure_file(
    input         : 'gtkhx.rc.in',
    output        : 'gtkhx.rc',
    configuration : _rc_conf,
  )
  gtkhx_win_res = import('windows').compile_resources(
    _gtkhx_rc,
    # windres reads the .ico / .manifest the .rc references from here.
    include_directories : include_directories('.'),
    depend_files        : files('gtkhx.ico', 'gtkhx.manifest'),
  )
endif

executable('gtkhx',
  gtkhx_sources,
  gtkhx_resources,
  gtkhx_win_res,
  dependencies        : deps + rust_deps,
  include_directories : include_directories('.'),
  install             : true,
  # GUI subsystem on Windows so double-clicking the exe doesn't pop a console
  # window (mingw still enters through main()). Ignored on non-PE targets.
  win_subsystem       : 'windows',
)
