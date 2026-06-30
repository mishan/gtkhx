# GtkHx Revival Roadmap

This document lays out a phased plan for reviving GtkHx (last released 0.9.4, May 2003) and modernizing it from GTK+ 1.2 → GTK 4, plus the cleanup and modernization work that naturally falls out along the way.

The roadmap is organized as phases that each end on something runnable. Each phase has a goal, the concrete work items, the gotchas worth flagging up front, and an "exit criteria" so we know when to move on.

---

## Snapshot of the codebase

A quick orientation, since everything below references it:

- **Size:** ~42,000 LOC across ~50 `.c`/`.h` files in `src/`, plus `plugins/` (sample, eliza).
- **Toolkit:** GTK+ 1.2, Glib 1.2, optional gdk-pixbuf 0.x.
- **Build:** autoconf (`configure.in`, not `.ac`) + automake; gettext (`fr` translation); `gtk-config`-era macros.
- **Threading:** custom `gtkthreads.c` — pipe + `pthread_cond` + `gdk_input_add`. Predates `g_main_context_invoke()` and friends.
- **Protocol compat:** Full backward compatibility with Hotline 1.2 and 1.5 is a **hard requirement**. The wire-format and HOPE negotiation code is not to be modernized in a way that breaks legacy servers. Modern transport security landed separately via the dedicated-TLS-port model (Phase 7), which needs no changes to the legacy wire format.
- **Reference server:** [mhxd](https://github.com/kangsterizer/mhxd) (2023 merge of three `hxd` forks) is the natural test target. Same codebase family as GtkHx's protocol stack.
- **Networking:** raw sockets, optional IPv6 via `getaddrinfo`. Connection runs on a pthread.
- **Crypto:** in-tree MD5, SHA, HAVAL, HMAC; OpenSSL only for `RAND_bytes` and the cipher state structs (Blowfish/RC4; IDEA disabled via `CONFIG_NO_IDEA`).
- **Compression:** zlib, optional.
- **Plugins:** dlopen-based, `MODULE_IFACE_VER 2`, signal-emit pattern. Sample + ELIZA included. *(Decision: ABI breaks; see "Decisions locked in".)*
- **Two huge custom widgets** are the porting bottleneck:
  - **`gtk_hlist.c`** — a fork of GtkCList (~3500 LOC). 392 call sites in `files.c`, `users.c`, `tracker.c`, `news15.c`, `options.c`.
  - **`xtext.c`** — XChat 1.8.5's text widget (~3500 LOC). Immediate-mode GDK drawing. **Plan: replace with HexChat's modern xtext fork** (`src/fe-gtk/xtext.c` from hexchat/hexchat) — same widget lineage, actively maintained against modern GTK.
- **Hygiene debt:** CVS metadata in every directory, emacs autosave/lock leftovers (`#about.c#`, `.#commands.c.1.36`, `.#news15.c.1.33`, `.#xfers.c.1.40`, `.#commands.c.1.36`, `#rcv.c#`, `#tracker.c#`), generated autotools artifacts (`configure`, `aclocal.m4`, `Makefile.in`, `autom4te.cache/`) checked into the import.
- **Known issues from the original TODO** (still applicable):
  - "Rewrite GtkHList to be a derived class of GtkCList" — i.e. the author already wanted to delete the fork.
  - "GTK+ 2.0 support" — never happened.
  - "Reimplement SOCKS support."
  - "store pref variables inside a union in cfgvar."
  - "fix preferences broken-ness."

---

## Phase 0 — Hygiene & baseline ✅

**Goal:** Get the tree into a state where modern tooling can actually look at it without choking, and where every later phase has a clean starting point.

**Work items**

1. **Strip CVS / editor cruft.** `find . -type d -name CVS` (and the matching `Entries`/`Repository`/`Root` files), the `#…#` and `.#…` files in `src/`, and the stale `.cvsignore` files. These are all in git history if anyone wants them back.
2. **Untrack autotools-generated files.** `configure`, `aclocal.m4`, `Makefile.in`, `config.guess`, `config.sub`, `config.h.in`, `depcomp`, `install-sh`, `missing`, `mkinstalldirs`, `autom4te.cache/`, `intl/` (gettext shipped sources), `ABOUT-NLS`. Add to `.gitignore`. Keep `autogen.sh`, `configure.in`, `Makefile.am`, `acconfig.h`, `*.spec`, `debian/`.
3. **Add a CLAUDE.md / contributor notes file** capturing the architecture overview I sketched above, the threading model, and the wire protocol locations (`hotline.h`). This pays for itself on every later phase.
4. **Set up CI scaffolding** — even just a GitHub Actions matrix that runs `./autogen.sh && ./configure && make` inside a Docker image with whatever GTK version each branch targets. Catching breakage cheaply matters more as the surface area grows.
5. **Pick a license stance.** Code says "GPL v2 or later"; `LICENSE` in the root is already GPL. No work, just confirm intent.

**Exit criteria:** `git status` is clean after a build; CI green on `main` for whatever Phase 1 targets.

---

## Phase 1 — Modernize C and build, freeze GTK 1.2 baseline ✅

**Goal:** Make the code compile under a modern C compiler with strict warnings, even if we still reference GTK 1.2 conceptually. Most of this work is GTK-version-agnostic and pays dividends on every subsequent phase.

The reality: **GTK+ 1.2 is gone from every modern distro.** You'd need a vintage container (Debian sarge / etch era) to even attempt the original build. So in practice Phase 1 is "clean up the C, mothball the GTK 1.2 build, and make the Phase 2 GTK 2 port the first thing that actually links." I'd skip trying to revive a working GTK 1.2 binary unless you specifically want a "this is what 2003 looked like" reference build.

**Work items**

1. **Replace autotools with Meson.** Write a top-level `meson.build` plus `src/meson.build`. Use `dependency('gtk+-2.0')` (later bumped per phase), `dependency('glib-2.0')`, `dependency('zlib')`, `dependency('nettle')`, `dependency('gnutls')`. `i18n.gettext('gtkhx')` for the French translation. Delete `configure.in`, `acconfig.h`, `aclocal.m4`, `autogen.sh`, the `intl/` in-tree gettext copy, and all the autotools shims (`config.guess`, `config.sub`, `depcomp`, `install-sh`, `missing`, `mkinstalldirs`).
2. **Modern C dialect.** Set `-std=gnu11` (or `c99` minimum). Add `-Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter`. Expect a deluge — `gtk_signal_*` callback prototypes alone will produce hundreds. Triage in batches.
3. **Drop dead replacement functions.** `snprintf.c`, `getopt_r.c` / `getopt1_r.c`, `localtime_r.c`, `strcasestr.c`, `basename.c` were portability shims for HP-UX / IRIX / pre-glibc. All POSIX standard now. Delete.
4. **Replace in-tree crypto.** Per the decision above: `md5.c` and `sha.c` → `g_checksum_*`. `hmac.c` → thin wrapper over `GHmac` (preserve the HMAC-HAVAL branch until we verify nothing advertises it). `cipher.c` → rewrite over Nettle. `rand.c` → `getrandom(2)`. This can land before any GTK porting starts and removes ~50K LOC of vendored crypto we don't want to maintain.
5. **Format-string and `sprintf` audit.** A 23-year-old C codebase will have `sprintf(buf, "...", user_input)` patterns. Convert to `g_snprintf` / `snprintf` with bounds.
6. **Header hygiene.** `hx.h` is 655 lines of mixed types, inline functions, macros, and `extern` decls — split it into `protocol.h` (wire-format structs/macros), `session.h` (session/htlc), `prefs.h`, `compat.h`. Same for `hotline.h`.
7. **Remove Win32 conditionals** unless you actively want to keep Windows support. Modern path is MSYS2/MinGW with the same GTK build, no `WIN32`-only branches.
8. **Multi-connection: defer, but stop pretending.** `MAX_CONN 1` and `sess_from_htlc()` returns `&sessions[0]`. The half-built abstraction is more confusing than helpful. Don't extend it during the GTK ports — collapse it to a single explicit session, then build real multi-conn against the modernized codebase in Phase 5.

**Exit criteria:** Codebase compiles to objects (no link required) under modern gcc/clang with `-Wall -Wextra` producing only the deprecation warnings you can't fix until the toolkit changes.

---

## Phase 2 — Port to GTK+ 2 ✅

**Goal:** Get a binary that runs on a modern Linux desktop, even if it looks dated. This is the **biggest single jump** because of the two custom widgets and the signal API change.

**Why GTK 2 first instead of straight to 4:** every API generation tightens semantics. Trying to leap directly to GTK 4 means rewriting against assumptions (no GtkContainer, event controllers instead of signals, no immediate-mode drawing, no global threads-enter/leave) while you're still untangling 1.2 idioms. Land on 2, where most of the toolkit-shaped problems are isolatable, then climb.

**Work items, in dependency order**

1. **Build system pkg-config swap.** `gtk-config` → `pkg-config gtk+-2.0`. `gtk+-1.2` and `glib-1.2` constants out, `gtk+-2.0` in. Glib 2 brings GObject, GError, GIO, GAsyncQueue.
2. **Type system cast macros.** `GTK_OBJECT(x)` → `G_OBJECT(x)` where appropriate (or just the right `GTK_*` macro). `GtkType` → `GType`. `GTK_WIDGET_FLAGS(w) & GTK_VISIBLE` → `gtk_widget_get_visible(w)`. ~475 sites, but most are mechanical.
3. **Signals:** `gtk_signal_connect(obj, "x", GTK_SIGNAL_FUNC(cb), data)` → `g_signal_connect(obj, "x", G_CALLBACK(cb), data)`. ~419 sites. Mostly sed-able but watch for `gtk_signal_connect_object` → `g_signal_connect_swapped`, and for the `_after` variants.
4. **Replace `gtk_hlist` with GtkTreeView.** This is the single biggest task in this phase. Strategy:
   - Build a thin `gtk_hlist_compat.[ch]` shim that exposes the same `gtk_hlist_*` API but is implemented over a GtkTreeView + GtkListStore. Migrate one consumer at a time (`tracker.c` is smallest, do it first; `users.c` and `files.c` last).
   - Once all five consumers are converted, delete the shim and inline the TreeView calls.
   - Expect the TreeView column model + cell renderer setup to be wordier than CList. That's the price of the real widget.
5. **Replace GtkCTree (news threading)** with GtkTreeView in tree mode + GtkTreeStore. Same playbook.
6. **Replace `xtext.c`.** **Decision: vendor HexChat's modern xtext** (`src/fe-gtk/xtext.c` from hexchat/hexchat — same XChat-derived lineage, but actively maintained, GTK 2/3-compatible, cairo + Pango drawing). Drop GtkHx's stale ~3500 LOC fork in favor of HexChat's vendored copy. Keep the message-formatting code in `chat.c` / `commands.c`; only the widget changes. The chat-specific behaviors (URL highlighting, mIRC color parsing, indent rendering, context menus) come for free instead of being reimplemented on GtkTextView. (Earlier draft said drop xtext entirely for GtkTextView; rejected after weighing the reimplementation cost.)
7. **GdkFont → Pango.** `gdk_font_load(name)` → `pango_font_description_from_string(name)`. `gtk_widget_modify_font()` to apply. Wherever the code measures text, switch to PangoLayout.
8. **GtkPixmap → GtkImage.** ~56 sites; most are toolbar/icon use. The XPM headers in `src/pixmaps/` still load fine via `gdk_pixbuf_new_from_xpm_data`.
9. **GtkOptionMenu → GtkComboBox** in `connect.c` (cipher/compression dropdowns).
10. **Threading rewrite — first cut.** Replace `gtkthreads.c` with the GTK 2 standard `gdk_threads_enter()` / `gdk_threads_leave()` (which is what your custom code is already approximating). Initialize with `g_thread_init(NULL)` + `gdk_threads_init()`. Plan to delete this entirely in Phase 3 because GTK 3 deprecated it; for now it gets you compiling.
11. **`gtk_widget_set_usize` → `gtk_widget_set_size_request`** (sed-able).
12. **`gtk_timeout_add` → `g_timeout_add`** (sed-able).
13. **`g_io_add_watch` keeps working** but the callback signature can be tightened. `hxd_fd_set` in `gtkhx.c` is the integration point for the Hotline protocol with the main loop — should still work.
14. **Drop the gettext `intl/` in-tree copy.** Modern systems ship libintl; just `AM_GNU_GETTEXT([external])`.

**Gotchas**

- The colormap dance in `init_colors()` (`gtkhx.c`) is pre-truecolor-era. With GTK 2 on any modern X server you can delete the `gdk_colormap_alloc_color` calls and just use `GdkColor` literals.
- `gtk_text_insert(GTK_TEXT(w), font, fg, bg, str, len)` (the old GtkText) becomes a `GtkTextBuffer` + `GtkTextTag` operation — the per-call font/color args go away in favor of named tags.
- The `g_user_colors[]` table and any chat-side color/format logic that reaches into the widget need to survive the swap — those are wire-protocol-shaped, not toolkit-shaped. HexChat's xtext brings its own mIRC color parser (`\003NN`); reuse that path instead of carrying ours.
- `news15.c` and `chat.c` both reach into widget internals (`GTK_TEXT(w)->vadj`). Replace with `gtk_scrolled_window_get_vadjustment()` / TextView's adjustments.

**Exit criteria:** `gtkhx` builds against GTK 2, launches, connects to a Hotline server, joins chat, downloads a file, posts news. UI looks crufty but works.

---

## Phase 3 — Port to GTK+ 3 ✅

**Goal:** Modern theming, cairo drawing, no deprecated APIs. After Phase 2 this is mostly cleanup.

**Work items**

1. **Drawing → cairo.** Anywhere `gdk_gc_*`, `gdk_draw_*`, or `expose_event` survived Phase 2 (custom drawing in `cicn.c` for icon rendering), convert to cairo via `draw` signal handlers. `cicn.c` is the main custom-draw site outside the widgets we've already replaced. (HexChat's xtext, vendored in Phase 2, already draws via cairo + Pango — no Phase 3 work there.)
2. **Drop `gdk_threads_enter` / `_leave` entirely.** GTK 3 deprecated them. Use `g_main_context_invoke()` to marshal from worker threads onto the main context. The Hotline I/O thread (`network.c`, `xfers.c`) is the main offender — all UI calls from those threads need to be wrapped.
3. **GtkBox replaces GtkVBox / GtkHBox.** `gtk_vbox_new(homog, spacing)` → `gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing)`.
4. **GtkGrid replaces GtkTable** if you used any (`options.c` likely has some).
5. **CSS theming.** Drop the manual `GdkColor` / `gtk_widget_modify_bg` calls; expose color/font preferences as a small CSS string applied via `GtkStyleContext`.
6. **GtkApplication** for the toplevel. One application instance, one main window, properly wired up to D-Bus single-instance, session save, etc.
7. **GResource** for icons/pixmaps/UI files instead of installing into `$prefix/share/gtkhx`.
8. **Input handling cleanup.** Migrate from raw key-press signals to `GtkEntryCompletion` for nick completion (`old_nickcompletion` pref hints at the existing custom one).
9. **Deprecation pass.** Build with `-DGDK_DISABLE_DEPRECATED -DGTK_DISABLE_DEPRECATED` and chase the warnings to zero. This is the gate to Phase 4 — anything still deprecated in 3 will be removed in 4.

**Exit criteria:** Builds clean against GTK 3 with deprecations disabled. UI follows the system theme.

---

## Phase 4 — Port to GTK 4 ✅

**Goal:** Land on the current toolkit. The breaking changes here are conceptual, not just renames. Following the Phase 3 pattern: bump the meson dep first, then drive the resulting punch list to zero with mostly-mechanical sub-phases, ending on a runnable binary.

**Pre-flight decisions** (lock these in before starting 4.1):

- **Drawing model for `xtext.c`:** keep cairo via `gtk_snapshot_append_cairo()`, don't rewrite around `GskRenderNode`s. Cheaper, preserves the Phase 3.4b cairo work.
- **`gtk_hlist_compat`:** stay on `GtkTreeView` through Phase 4, even though it's deprecated in 4.10. The five consumers and the `gtk_hlist_*` API surface are stable; migrating to `GtkColumnView`+`GListModel` is a separate axis of change and would double the effort. Defer to a Phase 4.x or Phase 5 sub-phase.
- **UI definition:** stay code-driven, do **not** introduce `.ui` XML + `GtkBuilder` in this phase. Mixing structural API changes with a description-language rewrite is asking for confusion. `.ui` migration can happen incrementally afterward, dialog-by-dialog.
- **Toolbar:** the `toolbar.c` window stays as a separate window of buttons — it's already drawn that way and the layout works. Defer the move to a `GtkHeaderBar` on the chat/main window to a Phase 5 UX sub-phase.
- **`gtk_widget_set_visible`:** widgets are visible by default in GTK 4, so most `gtk_widget_show_all` calls just disappear rather than being replaced.

**Why a sub-phased plan, not a big-bang rewrite:** Phase 3 worked the same way and the discipline of "every commit ends on a clean build" caught real regressions early (the cicn `sizeof(void *)` bug, the toolbar `delete-event` callback typo, the chat-window-doesn't-show race). Same approach here.

**Sub-phases**

1. **4.1 — Bump meson dep, first GTK 4 build attempt.** Switch `dependency('gtk+-3.0')` → `dependency('gtk4')`. Don't fix anything yet — just look at the error wall. Expected categories: `gtk_container_add` / `GTK_CONTAINER` (~167 sites), `gtk_box_pack_start` / `_end` (~221 sites), `gtk_widget_show_all` (~37 sites), `gtk_widget_destroy` (~50 sites), `gtk_widget_get_window` / `GdkWindow` (~46 sites), event signal connections + `GdkEventXxx` struct field access (~25 sites), `gtk_image_new_from_pixbuf` (~51 sites — most still work, some need attention), the entire `xtext.c` draw signal path. Expect 1000+ compile errors — that's the punch list for the rest of the phase. Add `-Wno-error=deprecated-declarations` temporarily so the bulk of work is just the breaking changes, not deprecations.
2. **4.2 — Container / box API sweep (the big mechanical one).**
   - `gtk_container_add(window, child)` → `gtk_window_set_child(window, child)`
   - `gtk_container_add(scrolled, child)` → `gtk_scrolled_window_set_child(scrolled, child)`
   - `gtk_container_add(frame, child)` → `gtk_frame_set_child(frame, child)`
   - `gtk_container_add(button, child)` → `gtk_button_set_child(button, child)`
   - `gtk_box_pack_start(box, w, expand, fill, padding)` → `gtk_box_append(box, w)` plus `gtk_widget_set_hexpand(w, expand)` / margin properties for padding (the pattern we already established for the table → grid migration in Phase 3.9c).
   - `gtk_box_pack_end` → `gtk_box_append` after a `gtk_widget_set_halign(GTK_ALIGN_END)`, or a small wrapper that walks the box backwards.
   - This is ~400 sites total. Worth a `gtkhx_box_append_packed(box, w, expand, fill, padding)` helper to keep the diff narrow and the per-call expansion uniform.
3. **4.3 — Show / destroy / realize sweep.**
   - `gtk_widget_show_all(w)` → drop in most cases (widgets visible by default in GTK 4); promote to `gtk_widget_set_visible(w, TRUE)` only where the widget's `visible` property was explicitly cleared.
   - `gtk_widget_destroy(window)` → `gtk_window_destroy(window)` for toplevels.
   - `gtk_widget_destroy(widget)` for non-window widgets → unparent / `g_object_unref`.
   - Scrub the leftover `gtk_widget_realize` + `gtk_widget_get_style` dead-code pattern again — Phase 3 hit it in five places, expect a couple more in odd corners.
4. **4.4 — GdkWindow → GdkSurface.**
   - `gtk_widget_get_window(w)` → `gtk_native_get_surface(gtk_widget_get_native(w))` (with NULL-safety).
   - `gdk_window_get_width` / `_get_height` → `gdk_surface_get_width` / `_get_height`.
   - `gdk_window_get_root_origin` is gone — under Wayland there is no absolute root origin to get. The `gtkhx_save_window_positions` Wayland-skip from Phase 3.x becomes the only path.
   - `gdk_window_raise` → `gtk_window_present` for toplevels (the use sites in `create_*_window` early-out paths).
5. **4.5 — Event signals → `GtkEventController`s (the biggest conceptual jump).** GTK 4 widgets no longer emit `button_press_event`, `key_press_event`, `motion_notify_event`, `enter_notify_event`, `leave_notify_event`, `configure_event` directly. Each becomes an event controller you attach to the widget:
   - `button-press-event` / `button-release-event` → `GtkGestureClick` (bound to "pressed" / "released" signals).
   - `key-press-event` / `key-release-event` → `GtkEventControllerKey` (bound to "key-pressed" / "key-released").
   - `motion-notify-event` / `enter-notify-event` / `leave-notify-event` → `GtkEventControllerMotion`.
   - `configure-event` → no replacement on widgets; toplevel size changes come via `GtkWindow::default-width` notify or `GtkWidget::size-allocate`. Position has already gone away on Wayland.
   - `delete-event` → `GtkWindow::close-request`.
   - The event-handler functions also need their signatures updated (no `GdkEventXxx *event` param; the controller signal carries x/y/state directly).
   - Roughly 25 handlers to convert across `chat.c`, `users.c`, `files.c`, `news15.c`, `xtext.c`. The `chat_input_key_press` handler in `chat.c` is the most complex (Tab nick completion + Return to send + Up/Down history) — it's the one that'll be most painful.
6. **4.6 — GdkEvent struct access → accessor functions.** Where event handlers still get a `GdkEvent *` (e.g. for popup menus where the activating event matters), `event->x`, `event->y`, `event->button`, `event->state`, `event->keyval` are gone — all have `gdk_event_get_*` accessor functions in GTK 4.
7. **4.7 — Menus, popups, file dialogs.**
   - `GtkMenu` is gone — the `users.c` user popup menu becomes a `GtkPopoverMenu` driven by a `GMenuModel`. The handful of submenu / item helpers (`menu_quick_sub`, `menu_quick_item`) need rewriting around `GMenu` items.
   - `GtkFileChooserDialog` → `GtkFileDialog` (async, callback-based; the `upload_file_response` pattern in `files.c` becomes a `GtkFileDialog::open` callback).
   - `gtk_dialog_get_action_area` is finally gone in GTK 4. The `gtkhx_dialog_action_area` wrapper from Phase 3.9b becomes either a refactor onto `gtk_dialog_add_button` (for response-based buttons) or rewrite the dialogs that pack pre-made widgets to use a custom `GtkBox` instead of the action area.
8. **4.8 — Drag and drop.** Four sites (`files.c` and the news folder/catalog browsers in `news15.c`). `gtk_drag_dest_set` → `GtkDropTarget`, `gtk_drag_source_set` → `GtkDragSource`. Both attach as event controllers.
9. **4.9 — `xtext.c` custom widget — snapshot rewrite.** The `draw` signal is gone; widgets implement a `snapshot` vfunc instead. The Phase 3.4b cairo work survives: replace the `draw` handler with a `snapshot` vfunc that calls `gtk_snapshot_append_cairo()` to get a `cairo_t` and runs the existing draw code unchanged. Realize/unrealize handlers no longer create their own `GdkWindow` (widgets don't have one in GTK 4); the cursor / selection-targets setup migrates to event controllers (Phase 4.5/4.8). Watch out for `gtk_xtext_set_font`'s no-realize-needed property — already done in Phase 3.4b, should carry over fine.
10. **4.10 — `gtk_hlist_compat` deprecation containment.** Wrap the implementation file in `G_GNUC_BEGIN_IGNORE_DEPRECATIONS` / `G_GNUC_END_IGNORE_DEPRECATIONS` so the GtkTreeView usage compiles cleanly under GTK 4's deprecation flags. Add a TODO/Phase-5 comment pointing at the `GtkColumnView`+`GListModel` migration. The five consumers don't change.
11. **4.11 — Toolbar, accelerators, GtkApplication adjustments.**
   - `GtkToolbar` is gone — the `toolbar.c` window's content is already a `GtkBox` of buttons, so no real change beyond removing any leftover toolbar-specific styling.
   - `gtk_window_add_accel_group` is gone → `GtkShortcutController` attached to each window. The Ctrl+K (connect) and Ctrl+Q (quit) accelerators in `gtkutil.c init_keyaccel` get reimplemented as `GtkShortcut` instances.
   - `GtkApplication` from Phase 3.6 mostly survives. The `gtk_application_add_window` walk in `gtkhx_activate` should still work; verify the activate semantics under GTK 4.
   - `gtk_main_iteration` / `gtk_events_pending` (2 sites) → `g_main_context_iteration` / `g_main_context_pending` on the default context.
12. **4.12 — First boot + bug-finding round.** This is where the unknowns surface. Phase 3 hit several show-stoppers in this window (the `Ptr` 64-bit struct layout bug in cicn, the toolbar `delete-event` callback typo, the chat window not appearing on Wayland because `gtk_application_add_window` only ran for the toolbar, the `GResource` doubled-prefix path). Expect similar surprises here. Plan for several iterative debugging commits before the binary is reliably useful.
13. **4.13 — Lock in.** `-Werror=deprecated-declarations` again, this time targeting GTK 4. Anything that survives goes into Phase 5 follow-up tasks.

**Gotchas worth flagging up front**

- The protocol-side code (`hotline.h`, `network.c`'s wire handling, `compress.c`, `cipher.c`, `commands.c`, `rcv.c`) shouldn't change at all in this phase. If a Phase 4 patch touches `hotline.h`, something's wrong.
- The pthread-based xfer code can stay; converting `xfers.c` to `GTask` + `GSocketClient` is a quality-of-life improvement, not a port requirement. Defer to Phase 5.
- The `gtkthreads.c` recursive-mutex + custom poll function from Phase 3.3 is GTK-version-agnostic — it should survive Phase 4 unchanged. The cleaner long-term answer is `g_main_context_invoke()` at every worker→UI boundary, but that's a per-call-site refactor (~56 sites in `network.c` and `xfers.c`) and is also Phase 5 territory.
- File dialogs change: `GtkFileChooserDialog` → `GtkFileDialog` is async (callback-based, no `gtk_dialog_run`). Existing dialog use sites become a request + a response handler.
- `gtk_widget_set_size_request` still exists in GTK 4 with the same semantics — the Phase 3.x `-2` → `-1` cleanup carries over.
- `GdkPixmap`/`GdkBitmap` typedef aliases (in `session.h` and `gtk_hlist_compat.h`) survive Phase 4 — they're our own types, not GTK's. Keep until the consumers can be unwound (Phase 5 cleanup).

**Exit criteria:** Runs natively on Wayland. Connects to mhxd / hlserver.com, joins chat, browses files, lists tracker servers. Builds with `-Werror=deprecated-declarations` against current GTK 4. UX is no worse than the Phase 3 binary; HeaderBar and other GTK-4-native UX touches are explicitly Phase 5 work.

---

## Reference server: mhxd

For meaningful end-to-end testing — once anything is runnable — we need a Hotline server we control. **mhxd** ([github.com/kangsterizer/mhxd](https://github.com/kangsterizer/mhxd)) is the right pick:

- It's a 2023-era merge of three forks of the original `hxd` HotlineX server — same codebase family that GtkHx grew out of (the original GtkHx README points at `hx.fortyoz.org` and the SourceForge HotlineX project; this is the surviving descendant).
- GPL-2.0-or-later, so license-compatible if we end up trading patches.
- The repo also builds an `hxtrackd` tracker and a console `hx` client — useful for testing tracker code and as a reference protocol implementation when our `rcv.c` parsing disagrees with reality.
- Default port 5500. Has SOCKS5 client support, optional pthreads, optional SQL backend for accounts — all useful test surfaces.
- Active CI as of 2023, so it presumably builds on something modern. (Worth confirming once we get to needing it.)

**When this becomes important:** Once Phase 2 produces a runnable binary, we need *something* to connect to. Standing up a local mhxd in a container (or just locally) is the obvious move — gives us a deterministic, reset-able test target instead of depending on whatever public Hotline servers may or may not still be up. Worth a `docker/mhxd/` directory in the GtkHx repo with a `Dockerfile` and a sample `hxd.conf`.

**Worth checking when we get there:**
- Whether mhxd has any protocol fixes in its commit history that GtkHx should mirror (e.g., bug fixes to packet parsing, extensions to HOPE negotiation). Diff its `hotline.h` and `rcv.c` analogues against ours.
- Whether mhxd's GTK client (`ghx`, built with `--enable-gtk`) has been ported further than GtkHx — same lineage, possibly farther along the porting path. If so, cherry-picking patches saves work.
- Its SOCKS5 implementation when we tackle the SOCKS TODO item.

---

## Phase 5 — Modernization beyond the toolkit

These are independent of the GTK climb and can be slotted in earlier (some of them really should be) or saved for last.

**Protocol & networking**

- ✅ **Full backward compat with Hotline 1.2 and 1.5** — maintained throughout the GTK ports. Verified end-to-end against mhxd (1.2/legacy), hlserver.com (1.0/1.2-style), and Badmoon / Mobius / Janus (1.9). Tier 3 integration suite covers chat, news, file transfers, banner, HOPE+stream/AEAD ciphers, colored nicknames. Modern transport security via TLS is tracked as Phase 7 below; Phase 1 (control-channel TLS) shipped on `claude/tls-phase1-control-channel`.
- ✅ **SOCKS support** — comes for free with the GSocketClient + GProxyResolver migration. `GSocketClient` honours `GProxyResolver`'s lookup, so `http_proxy`/`socks_proxy` env vars and GNOME's network proxy settings just work for both the Hotline control connection (network.c) and HTXF transfers (xfers.c). No reimplementation of the 2003 TODO needed.
- ✅ **Wire-level int handling audit** — `HN16`/`HN32` byte-swap macros remain (they're terse and the call sites are dense), but the structural correctness audit landed: `proto_helpers.c` for cursor-based chunk reads, `network_decode.c` for bounded TLV walking, and the Tier 2 wire-fixture harness pins layout under both -O0 and ASan. Strict-aliasing / alignment bugs on ARM64 would now surface as test failures rather than silent data corruption.

**Plugin system**

- The dlopen ABI in `plugins/plugin_h.h` is a `MODULE_IFACE_VER 2` from 2002. Decide: keep, deprecate, or replace with GIRepository + scriptable plugins (Lua / Python / JS via GJS). The ELIZA chat plugin is fun-but-niche; not a strong reason to keep the API.
- A modern alternative is to expose a small in-process scripting hook (e.g., an embedded Lua interpreter) for chat triggers, auto-reply, command aliases.

**Packaging & distribution**

- ✅ **Flatpak manifest.** `com.nasledov.gtkhx.yml` at the repo root, GNOME 49 runtime, local-source variant for development. To submit to Flathub, swap the `dir` source for a `git` source pointing at a tagged commit and add screenshots to `data/screenshots/`.
- ✅ **AppStream metadata** (`data/com.nasledov.gtkhx.metainfo.xml`) — full Flathub-ready: id, name, summary, description, screenshots, releases, OARS-1.1 content_rating, branding colors. Validated at `meson test` time via `appstreamcli validate --pedantic`.
- ✅ **`com.nasledov.gtkhx.desktop`** with proper categories (Network/Chat/FileTransfer), validated at test time via `desktop-file-validate`.
- ✅ **RPM spec + `debian/` removed.** Both directories deleted during Phase 0–1 cleanup (the 2003-era files were referencing pre-Glib-2 paths and APIs). Distro packaging story is now Flatpak (com.nasledov.gtkhx on Flathub-ready manifest); native distro maintainers can rebuild from the tagged tarball when there's demand.

**Quality / process**

- ✅ **Test suite.** Three tiers under `tests/`: Tier 1 unit tests for pure-glib modules (~20 covering text utilities, byte-swap, hash collections, prefs parser, HMAC, HTsc bookmark format, etc.), Tier 2 wire-fixture tests for protocol parsers / encoders (HOPE handshake KATs, FILELIST walkers, opcode round-trips), Tier 3 integration tests run end-to-end against Dockerized mhxd / Mobius / Janus servers. Coverage reporting via `tools/coverage.sh` → `coverage/index.html`. CI runs every push, full Tier 3 matrix included.
- ✅ **Static analysis.** `.github/workflows/analyze.yml` runs GCC `-fanalyzer` and `clang-tidy` (against the existing `.clang-tidy` config) on every push and PR. Both `continue-on-error` for now — they upload findings as CI artifacts and produce a categorised summary in the build log, but don't block merges. Flip individual categories to mandatory as the tree gets cleaned up. `tools/analyze.sh` runs the same pipeline locally.
- ✅ **AddressSanitizer / UBSan in CI.** Same workflow runs a `meson -Db_sanitize=address,undefined` build and exercises the full unit + proto test suite under instrumentation. Real run with detect_leaks=0 (GtkHx doesn't tear every alloc down on shutdown), halt_on_error=0 to surface multiple findings per run. Catches use-after-free, OOB, double-free, undefined integer overflow, NULL deref before they reach the legacy GTK paths.
- **Use `g_autoptr` / `g_autofree` aggressively.** Precedent landed in `bookmarks_io.c` + `bookmarks.c` + `test_bookmarks.c`: `g_autofree` for local strings, `g_autoptr (HxBookmark)` via the cleanup-func declared in `bookmarks.h`. The rest of the tree still uses explicit `g_free` and `goto out` cleanup tails. Not worth a sweep — convert opportunistically when touching a function. The bookmarks files are the in-tree pattern to copy.

**UX features the original always wanted**

- **Multi-server connections** (`MAX_CONN > 1`, the abstraction is half-built). Still pending — the plan is a tabbed UI for multi-conn rather than the current per-window layout. Single-conn was deliberately preserved through the GTK ports to keep blast radius small; revisit against the modernized codebase.
- ✅ **Real preferences UI** — Settings is now an `AdwPreferencesDialog` with nine pages (General, Identity, Chat, Sound, Notifications, Trackers, Misc, and the icon picker / theme controls), backed by GKeyFile-on-disk persistence at `$CONFIG/gtkhxrc`. The original "fix preferences broken-ness" TODO is closed.
- ✅ **Emoji shortcodes** — emoji survive servers that don't speak UTF-8 by riding the wire as Slack/Discord-style `:shortcode:` text (e.g. 😂 → `:joy:`) instead of the Mac Roman `?` fallback, with the inverse decode on display (on every server, so hand-typed shortcodes render too). Includes an inline `:prefix` typeahead popup in the chat / pchat / PM inputs. Conversion + typeahead each have a Settings → Chat → Emoji toggle (default on). The emoji↔shortcode table is generated into the `hotline-proto` Rust crate; the C side is thin wiring at the existing `gtkhx_text_for_wire` (send) and `hx_chat_event_new` / `hx_msg_event_new` (display) chokepoints. Full design + sub-phase log in `docs/emoji-shortcodes-plan.md`.
- **Chat/message logging.** Still pending — `log.c` is still `#if 0`'d-out in the build. The `$CONFIG/logs/` directory is reserved (prefs / path machinery resolves it), but nothing actually writes chat transcripts there yet. Separate from the categorised debug logger in `debug.c` / `proto_trace.c`, which is for stderr instrumentation (`GTKHX_DEBUG=proto,news,login,…`) and is unrelated to user-facing chat history persistence.
- **Tracker tracker (tracker of trackers).** Still undecided. The current Trackers settings page lets the user manage a list of tracker addresses; a server-discovered metadirectory is a bigger swing — see `docs/tracker-v3-scoping.md` for one direction.
- **New-user experience.** Still pending — `docs/new-user-experience-scoping.md` lays out a first-run welcome dialog with an optional 3-page wizard (identity / server pick / done) for users without a `gtkhxrc`. Detects first run by absence of the prefs file. Open questions on tone, curated server list, dead-server pinging.

---

## Phase 7 — TLS (separate-port model, no protocol changes) ✅

Mobius (and now Janus) shipped plain TLS on a dedicated port (5600 HTLS-TLS, 5601 HTXF-TLS) with no in-band negotiation — connect, TLS handshake, then speak the regular Hotline 1.x protocol over the encrypted stream. It needs no new protocol layer and gives us real modern transport security against existing servers without ecosystem-wide cooperation. See `docs/tls-scoping.md` for the full plan.

Sub-phases:

- ✅ **Phase 1 — control-channel TLS.** `hx_connect` takes a `tls` parameter that flips `g_socket_client_set_tls`; accept-everything cert stub at the `GTlsConnection::accept-certificate` signal. Tier 3 coverage exercises the state-machine progression (`test_real_tls`) plus a full LOGIN + chat round-trip over the wrapped socket (`test_real_tls_login`) against Janus's TLS port. Matrix carries `HX_TEST_CAP_TLS` + `tls_port` / `tls_xfer_port` fields. Shipped on `claude/tls-phase1-control-channel`.
- ✅ **Phase 2 — HTXF over TLS.** `htlc->tls` field threaded through `htxf_connect` and `hx_sync_connect_to_host` so HTXF subchannels (xfers.c + banner.c workers) mirror the control channel's TLS mode. `htxf_io_get_socket` grew the GTlsConnection branch via `base-io-stream` property access. Tier 3: `test_real_tls_file_get` (FILE_GET round-trip over TLS HTXF) + `test_real_tls_banner` (file-mode banner HTXF over TLS) against Janus's tls_xfer_port. Shipped on `claude/tls-phase1-control-channel` (extended onto the Phase 1 branch).
- ✅ **Phase 3 — Cert trust UX.** TOFU trust DB (`tls_trust.{c,h}`) — SHA-256 fingerprint over cert DER, SSH known_hosts file shape, TRUSTED/UNKNOWN/MISMATCH lookup. Adwaita `AdwAlertDialog` prompt (`tls_trust_dialog.{c,h}`) with destructive styling on MISMATCH, wired into `network.c::tls_accept_certificate`. Pin writes are dispatched via `g_idle_add` (calling `g_mkdir_with_parents` from inside the accept-certificate signal handler wedges the TLS handshake on glib-networking + GnuTLS). Tier 1 covers the trust DB (`test_tls_trust`, 8 subtests); existing Tier 3 TLS tests now exercise the real pin path with `GTKHX_TLS_AUTO_ACCEPT=1`. Shipped on `claude/tls-phase1-control-channel`.
- ✅ **Phase 4 — Connect-dialog + bookmark UI.** "Use TLS" `AdwSwitchRow` in the Connect dialog with port auto-flip (5500↔5600, custom ports preserved) and HOPE+cipher+compress grey-out when TLS is on. `connect_with_args` enforces the same coupling at the data layer for bookmark / programmatic paths. Bookmark format extended with a 4th flag byte for TLS — pre-TLS files read with `tls=0` via zero-padding (no version bump needed). Bookmarks management dialog gained a matching TLS toggle row. Tier 1 covers round-trip + pre-TLS back-compat. Shipped on `claude/tls-phase1-control-channel`.
- ✅ **Phase 5 — Docs.** `docs/tls-scoping.md` and this ROADMAP entry updated; README / man page mention deferred to the next release-notes pass. Mobius matrix entry tracked separately in [[gtkhx_multi_server_test_plan]]; Janus alone covers the TLS test target for now.

---

## Phase 8 — Voice chat (fogWraith capability extension) ✅

The fogWraith spec adds voice chat to Hotline via a server-side SFU: clients negotiate `CAPABILITY_VOICE` (bit 2 of `DATA_CAPABILITIES`, already reserved as `HTLC_CAP_VOICE` in `src/hotline.h`), then run a WebRTC session against the server on UDP base+4. PCMU only, DTLS+SRTP, ICE — all handled by the WebRTC stack. Seven new TRAN opcodes (600–606) and five new data fields (0x01F5–0x01F9) ride the existing TCP control channel for signaling. Full plan at `docs/voice-chat-plan.md`.

**Status**: Phase 8.A through 8.G have all shipped. End-to-end voice works against the Janus VoiceRoom container with DTLS-SRTP audio + the seven new opcodes wired through the Rust runtime, plus the per-uid voice indicator column in the user list. The post-R3-R4 follow-up below also already shipped — the C-side GStreamer glue replaced itself with `hxvoice-runtime` (gstreamer-rs + gstreamer-webrtc-rs) early in 8.C rather than waiting for R3/R4.

**Voice is now an optional build feature.** The `-Dvoice` meson option (`auto` / `enabled` / `disabled`, default `auto`) gates the whole extension on the GStreamer 1.20+ stack. With `auto` a build host without GStreamer drops voice silently; `enabled` makes the missing stack a hard error. When off, the `hxvoice` / `hxvoice-runtime` crates aren't compiled (no GStreamer needed at build time), the GStreamer libs aren't linked, `HTLC_CAP_VOICE` isn't advertised, and all voice C sources / call sites / tests are compiled out behind `HAVE_VOICE`. See the "Voice chat (Phase 8) is an optional build feature" note in `CLAUDE.md` for the exact gate plumbing.

Locked-in choices (kept for the historical record):

- **WebRTC stack: gstreamer-rs + gstreamer-webrtc-rs** in the `hxvoice-runtime` crate. The original plan staged a C `webrtcbin` first with a Rust runtime as the post-R3/R4 follow-up; in practice the all-Rust runtime landed during 8.C without needing the broader R3/R4 prerequisites. See voice-chat-plan §3 and §11.
- **Hybrid Rust/C split** per `docs/rust/ROADMAP.md` Phase R2 + the `hxvoice` and `hxvoice-runtime` crates:
  - **Wire protocol → Rust** (`rust/crates/hotline-proto/src/voice.rs`): typed builders/parsers, SDP-summary + ICE-JSON + participant-blob walkers, mid-label decoder. Same shape chat-history / tracker-v3 / news use.
  - **Session state machine → Rust** (`rust/crates/hxvoice/`): pure `SessionMachine` with `step(Event) -> Vec<Action>`. No GLib, no GStreamer, no GTK — just typed events in, typed actions out. Owns the renegotiation queue, mid→UID map, mute flag, timeout policy, implicit-leave logic. Tested against the spec's annotated lifecycle examples + targeted regressions.
  - **GStreamer pipeline + webrtcbin runtime → Rust** (`rust/crates/hxvoice-runtime/`): owns the `gst::Pipeline` / `webrtcbin` instances, runs the SDP / ICE / pad-added / connection-state dispatch, threads the per-pad RTP-activity counter through the receive bins for the voice indicator. Talks to C via the `gtkhx_voice_runtime_*` FFI surface.
  - **UI → C** (`src/voice.{c,h}`, `src/voice_panel.{c,h}`, `src/voice_model.{c,h}`, `src/users_view.c`): per-chat voice toolbar, settings page, the `HxVoiceModel` canonical per-uid state, the voice indicator column.
- **Test target: Janus**, in the Tier 3 matrix on host networking with `HX_TEST_CAP_VOICE` + `voice_port` 5514. Seven integration binaries plus state-machine property tests in `hxvoice` and wire-fixture tests in `hotline-proto::voice`.

Sub-phases — all shipped (full detail in `docs/voice-chat-plan.md`):

- **8.A** — capability + signaling.
- **8.B** — GStreamer dependency + bare pipeline.
- **8.C** — state machine + webrtcbin runtime (the hard one — five debugging weeks).
- **8.D** — UI: chat-tab toolbar + signal bridge + bridge backend.
- **8.E** — Settings device pickers.
- **8.F** — Tier 3 integration matrix vs Janus.
- **8.G** — Per-uid voice indicator column in the user list. In-voice + muted ship; "actively speaking" plumbing is in place but demoted to IN_VOICE pending real VAD (GStreamer `level` or RFC 6464). See voice-chat-plan §12 step 4.

Phase 8 follow-ups (small):

- "Start muted" toggle, PTT keybind capture, "Auto-join voice when joining a chat room" toggle in `settings_page_voice()`.
- Real volume-graded speaker detection (the §12 step 4 flip).
- Flatpak mic-capture permission — partially shipped on `claude/flatpak-pipewire-mic`: kept `--socket=pulseaudio` and added `--filesystem=xdg-run/pipewire-0` for native PipeWire access. The dedicated Audio portal (would give a per-app permission prompt) is still upstream-discussion-only.

---

## Phase 9 — Inline media (fogWraith capability extension) ✅

The fogWraith spec adds inline images to chat via a server-validated upload/download pipeline: capable clients send `TranUploadMedia` (750) to get an opaque media handle, attach the handle + canonical MIME to a normal `TranChatSend` / `TranSendInstantMsg`, and capable recipients fetch the canonical bytes via `TranDownloadMedia` (751). Capability bit 3 (`HTLC_CAP_INLINE_MEDIA = 0x0008`) is already reserved in `src/hotline.h`. Full plan at `docs/inline-media-plan.md`.

**Status**: shipped. All sub-phases (9.A–9.F) landed, including the inline render path via multi-subline padding in xtext; the deferred items below (animated GIF, etc.) remain as noted.

The honest design risk is the receive-render path: xtext's vertical layout is line-uniform everywhere (`fontsize × subline_count`), and no existing patch in HexChat's lineage carries inline images. The plan splits "spec conformance" from "true inline render" so the wire stack ships against real servers before any xtext surgery lands.

Sub-phases:

- **9.A** — Wire protocol foundation. `hotline-proto::inline_media` module: typed builders/parsers for 750/751 (single-shot + chunked), `LimitsAdvertisement` parser for the new `DATA_CHAT_MEDIA_MAX_*` LOGIN-reply fields (0x020C–0x0211), `MediaErrorCode` enum (0–5). C-side dispatcher hookup. Tier 2 wire fixtures. No UI yet.
- **9.B** — Bounded decoder. New `media_decode` module that magic-byte sniffs + bounds-checks before decoding via `GdkPixbufLoader`. Allowlist is JPEG/PNG/GIF only; SVG/WebP/AVIF/HEIC rejected at sniff time per the spec. Reuses the `src/preview.{c,h}` worker-thread loader pipeline. Animated GIF deferred to v2 (first-frame still in v1).
- **9.C** — Send UX. Paperclip in chat / pchat / PM input bars; paste-from-clipboard for `GdkTexture` clipboard content; drag-and-drop image files via `GtkDropTarget`. Pre-flight against server-advertised limits with a resize/recompress offer. Chunked upload state machine cancellable mid-upload.
- **9.D** — Receive UX (placeholder + dialog). Inbound media renders as a styled placeholder row (`[image · PNG · 800×600 · 124 KB · click to view]`). Click opens an in-app dialog backed by the existing image-preview pipeline. Right-click context menu: Save As, Copy Image, Open in External Viewer. End-of-9.D, the client is spec-conformant with zero xtext changes.
- **9.E** — Inline render via multi-subline padding. Extend `textentry` with a media-kind discriminator + `GdkTexture *`. Reserve `ceil(img_h / fontsize)` blank sublines and paint the texture into the band during `gtk_xtext_render_line`. Reuses existing scroll/click/calc math; selection over media is all-or-nothing with alt text on copy. ~200–400 LOC focused on xtext.c. Variable-height xtext (Option 4) remains a possible follow-up if the line-grid compromises chafe.
- **9.F** — Tier 3 integration against Janus. Janus is the fogWraith reference server and ships inline-media support alongside the spec (same pattern as chat-history and voice). Add `HX_TEST_CAP_INLINE_MEDIA` to the Janus matrix entry, write end-to-end binaries for send (single + chunked), receive, error-code surfacing, authorization, and handle expiry. Legacy-fallback / mixed-audience cases are covered by mixing a capable Janus member with mhxd or Mobius observers in the existing multi-server Tier 3 setup — no mock server needed. (If the supports-check at the start of 9.F finds Janus doesn't actually implement it, fall back to a Go mock under `tests/integration/mock-server/inline-media/` per the alternate plan in `docs/inline-media-plan.md`.)

Locked-in choices:

- **Animated GIF deferred to v2** — v1 decodes the first frame and renders as a still.
- **In-app dialog for click-to-view**, reusing `src/preview.{c,h}`. External viewer is available via the right-click context menu, not the default click.
- **Right-click context menu** on every media surface (placeholder in 9.D, inline row in 9.E): Save As, Copy Image, Open in External Viewer.
- **JPEG / PNG / GIF only.** Spec-mandated; explicit reject of SVG / WebP / AVIF / HEIC at sniff time.

Janus is the inline-media Tier 3 target. Confirming Janus's actual support is the first step of 9.F (look for capability echo of bit 3 + the `DATA_CHAT_MEDIA_MAX_*` advisory fields in the LOGIN reply); the fallback if support isn't there is a Go mock server under `tests/integration/mock-server/inline-media/`.

---

## Phase 10 — GIF icons (fogWraith capability extension)

The fogWraith spec adds per-user custom **GIF avatars** to Hotline, independent of the standard 16-bit icon ID. The design is **pull-based**: the server stores GIF data per session but never pushes it. When a user sets or clears their icon, the server broadcasts an **Icon Change (1864)** notification carrying only the user's ID; clients re-fetch the image on demand via **Get Icon (1863)**. Four transactions (1861–1864 / 0x0745–0x0748) and two new field IDs (0x0300 GIF data, 0x0301 packed list entry) ride the existing TCP control channel. Full plan at `docs/gif-icons-plan.md`.

**Status**: ✅ shipped (10.A–10.D); 10.E is this docs pass. End-to-end against mhxd + Janus: set / get / get-list round-trip a real `GIF89a`, the 1864 broadcast reaches other clients, avatars render (animated) in the user list, and the Settings → Identity picker sets / clears your own. **Janus support confirmed** (2026-06-28). No Go mock server needed.

Structural difference from the other fogWraith extensions: **the spec defines no `DATA_CAPABILITIES` bit.** Per the locked-in decision, GtkHx does **not** invent one — support is discovered by **probe-and-fallback**. Critically, Janus (and presumably others) *silently drops* unknown opcodes with no reply, so the probe must use a **timeout watchdog**, not an error-code check — the same pattern as the tracker-v3 probe (`hx_tracker_v3_probe_ms()`).

Locked-in choices:

- **No capability bit** — probe Get Icon List (1861) after login with a ~2 s watchdog; silent fallback on timeout. Safe against every legacy server.
- **Animated avatars, with a pause control** — render animated GIFs in the user list, plus an "Animate GIF avatars" pref that falls back to a still first frame when off, and a per-user pause (click the avatar / right-click menu). (Inline media chose first-frame-only for v1; GIF avatars go further by request.) *As shipped (10.D): a single shared frame timer drives all avatars — simpler than per-cell frame clocks and fine at user-list scale — and it stops when nothing is animating.*
- **Reuse the inline-media bounded decoder** (`inline_media_decode_async`, magic-byte sniff + dimension/pixel/byte caps, sandboxed), narrowed to GIF.
- **Legacy CICN icons and GIF are one feature, two payloads.** The icon transactions (0x0745–0x0748) are shared; legacy carried a Mac cicn resource in field 0x0e90, fogWraith carries a GIF in 0x0300 (+ packed list 0x0301). cicn-over-wire is **vestigial** — no reachable server serves it (mhxd and Janus both discard a 0x0e90 payload; verified June 2026), so GtkHx implements the GIF payload only and leaves 0x0e90 reserved. The standard 16-bit icon-ID system (0x0068) is separate and already rendered today.
- **Header bug fixed during scoping.** `HTLC_HDR_ICON_GET` in `src/hotline.h` had been mis-defined as `0x0e90` (the cicn data-field number); the real opcode is `0x0747`. Dormant (proto_trace-only), now corrected. 10.A adds the remaining opcodes/fields using mhxd's exact constant names (`HTLC_HDR_ICON_GETLIST/_SET/_GET`, `HTLS_HDR_ICON_CHANGE`, `HTLS_DATA_ICON_GIF/_LIST`). Fields 0x0300/0x0301 coincide numerically with tracker-v3 TLV IDs but live in a separate namespace.

Sub-phases (detail in `docs/gif-icons-plan.md`):

- **10.A** ✅ — wire foundation: `hotline-proto::gif_icons` + FFI, opcodes/fields in `hotline.h`, `rcv.c` dispatch + senders in `src/gif_icons.{c,h}`, `GtkhxSession` `gif-icon-*` signals, probe-and-fallback. Tier 2 fixtures + Tier 3 (`tests/integration/test_gif_icons.c`).
- **10.B** ✅ — receive + per-uid avatar cache (`src/gif_avatar.{c,h}`, bounded sandboxed decode) + 1864→re-fetch + still avatar render in the user list (same cell path as cicn icons).
- **10.C** ✅ — send UX: GIF picker on Settings → Identity (choose / preview / clear). **Decoupled from capability** — the choice is persisted (`$CONFIG/avatar.gif`) and auto-sent on the next connect to a capable server, so the picker is never gated. Downscale deferred (gdk-pixbuf has no GIF encoder).
- **10.D** ✅ — animation: all frames decoded + played by a shared frame timer; global "Animate GIF avatars" pref (`CFG_ANIMATE_AVATARS`); per-user pause via click-on-avatar + a right-click "Pause/Resume Animation" menu item.
- **10.E** ✅ — docs: this entry, the effort table, `docs/gif-icons-plan.md`, README, CHANGELOG, and the man page.

---

## Decisions locked in

These were the open questions on the first draft. Answers:

1. **Build system → Meson.** Replacing autotools in Phase 1.
2. **GTK 1.2 baseline → skip.** First runnable target is GTK 2 (Phase 2).
3. **Custom widget strategy → shim for `gtk_hlist`, vendor HexChat's xtext to replace `xtext.c`** (in-tree XChat 1.8.5 fork is too far behind to keep maintaining).
4. **Multi-connection → commit to it.** Plan it as a Phase 5 deliverable, but in the meantime: stop pretending `MAX_CONN > 1` works (it doesn't — `sess_from_htlc()` returns `&sessions[0]`), and treat the single-session shape as a known temporary state. Do *not* add abstractions for multi-conn during the GTK ports; do the ports against single-session, then refactor to N sessions in Phase 5 against a smaller, modern codebase. The original half-built abstraction is more confusing than helpful and should be straightened out, not extended.
5. **Plugin API → break it.** Delete `plugins/sample`, keep `plugins/eliza` if it amuses anyone, but don't preserve the `MODULE_IFACE_VER 2` ABI. If a plugin system comes back, it'll be a Phase 5 redesign (e.g., embedded Lua or GJS) rather than the current dlopen pattern.
6. **Crypto stack → GnuTLS + Nettle for ciphers, GLib's `GChecksum` / `GHmac` for hashes.** Rationale captured in the next section. OpenSSL is the fallback if Nettle ergonomics turn out worse than expected once we're elbows-deep in `cipher.c`.
7. **License → keep GPL-2.0-or-later** (the existing "version 2... or any later version" header text). Not switching to v2-only, not upgrading to v3-only.
8. **RC4 retired.** Removed from `valid_ciphers[]`, the HOPE cipher offer list, and `cipher.c`'s dispatch in `claude/remove-rc4`. RC4 is a known-broken stream cipher; advertising it under a "Secure (HOPE)" label gave users a false sense of security. Plaintext, Blowfish (still acceptable), or ChaCha20-Poly1305 are all preferable. `CIPHER_RC4 = 1` stays reserved as a protocol-slot integer so it doesn't get re-used by accident.

    On-disk bookmark compatibility is handled via a stable cipher-byte vocabulary in `src/bookmark_cipher.{c,h}` — independent of `valid_ciphers[]` so reordering the connect dropdown can never shift the meaning of any byte already on disk. Byte assignments: `0=off, 1=RC4 (retired-but-named), 2=BLOWFISH, 3=CHACHA20-POLY1305`. New ciphers get new bytes at the tail; bytes don't get re-used when a cipher is retired. Bookmarks with non-RC4 ciphers load exactly as they were — no silent upgrades. A bookmark holding the legacy RC4 byte triggers `src/bookmark_rc4_dialog.{c,h}` at every connect entry point (toolbar SplitButton, Bookmarks dialog row-select); the dialog prompts the user for a replacement (ChaCha20-Poly1305 suggested, Blowfish, "Connect without encryption" destructive, Cancel) and rewrites the bookmark file in place so subsequent opens don't re-prompt. Cancel abandons the connection.

---

## Crypto stack rationale

Hotline's wire protocol needs MD5 (auth challenge/response), HMAC (with negotiable hash — including HAVAL, which `hmac.c` checks for as `"HMAC-HAVAL"`), and optionally Blowfish (RC4 was retired in `claude/remove-rc4` — see Decision 8 above). IDEA is disabled (`CONFIG_NO_IDEA`).

| Option | Pros | Cons |
|---|---|---|
| **OpenSSL 3.x (EVP)** | Already integrated in `cipher.c` / `rand.c`. Ubiquitous. Has every cipher Hotline needs. | Verbose API. Blowfish/RC4 moved to "legacy provider" in 3.x — needs explicit `OSSL_PROVIDER_load(NULL, "legacy")`. Apache 2.0 license is GPL-v3-compat but not v2-only-compat (moot for us since we're v2-or-later). |
| **GnuTLS + Nettle** *(picked)* | LGPL v2.1+, clean GPL story. Nettle has every legacy cipher Hotline needs, with a much cleaner API. GnuTLS gives us TLS for free in Phase 5. Aligns with GLib's `GTlsBackend`. | Two libraries on the dep list (Nettle for primitives, GnuTLS for TLS). Slightly less ubiquitous on macOS/Windows. |
| **libsodium** | Permissive ISC license, modern audited primitives, simple API. | **Disqualified for this protocol** — by design has no Blowfish, no RC4. Useful only for any net-new modern features (e.g., client-to-client E2E layered on top of Hotline chat). |
| **GLib's `GChecksum` / `GHmac`** *(picked, for hashes only)* | Already a dependency, zero new code, covers MD5/SHA1/SHA256/SHA512. | Hashes only — no ciphers, no HAVAL. |

**Plan:**

- **`md5.c`, `sha.c`** → delete, use `g_checksum_*`.
- **`hmac.c`** → reduce to a thin wrapper over `GHmac`, except for the HAVAL branch (see below).
- **`haval.c`** → keep for now, it's wired into `hmac.c`'s `HMAC-HAVAL` MAC negotiation. Verify whether any extant Hotline server still advertises HAVAL; if not, delete the whole branch and `haval.[ch]`.
- **`cipher.c`** → rewrite over Nettle (`nettle/blowfish.h`). This is **only** for the existing Hotline `HOPE` cipher negotiation (Blowfish over the legacy protocol; RC4 retired in `claude/remove-rc4`). Still useful for client-to-client privacy on the rare server that supports it; survives unchanged.
- **`rand.c`** → ✅ done (Phase 1.3): wraps `getrandom(2)` with a
  `/dev/urandom` fallback for old kernels. No OpenSSL dep, no extra
  library either. Nettle's `yarrow256_*` was considered and rejected:
  it's a userland PRNG that has to be seeded from the kernel CSPRNG
  anyway, so all it would add is state-management complexity.

**What this is *not*:** This crypto-stack work is about the legacy HOPE ciphers (Blowfish, the retired RC4), not transport security. Modern transport security shipped separately via the dedicated-TLS-port model — Mobius and Janus expose plain TLS on a separate port with no protocol changes, and GtkHx speaks it end-to-end. See Phase 7.

---

## Effort sketch (very rough)

| Phase | Scope | Rough effort | Status |
|---|---|---|---|
| 0 | Hygiene & CI | 1–2 days | ✅ |
| 1 | Build modernization, C cleanup | 1–2 weeks | ✅ |
| 2 | GTK 2 port (gtk_hlist, xtext, signals, threads) | 4–8 weeks — the big one | ✅ |
| 3 | GTK 3 port (cairo, threading, deprecations) | 2–3 weeks | ✅ |
| 4 | GTK 4 port (event controllers, GtkBuilder, GtkColumnView) | 3–5 weeks | ✅ |
| 5 | Modernizations (packaging, tests, prefs UX) | ongoing | mostly ✅ |
| 7 | TLS (separate-port model, sub-phases 1–5) | 2–3 weeks | ✅ |
| 8 | Voice chat (fogWraith, sub-phases A–G) | 6–10 weeks — Rust runtime was the long pole | ✅ |
| 9 | Inline media (fogWraith, sub-phases A–F) | 3–5 weeks | scoped |
| 10 | GIF icons (fogWraith, sub-phases A–E) | 2–3 weeks | ✅ |
| ∞ | Modernized Hotline protocol (Hotline-NG) | n/a — social problem | parked |

Done in evenings/weekends, multiply by ~3.

---

## Suggested next concrete step

Start with **Phase 0 step 1**: rip out the CVS dirs, the emacs autosave/lock files, and the regenerable autotools artifacts; commit. That gives us a clean tree to actually diff against in Phase 1, and it's the kind of work where the "before" state is actively misleading every grep for the rest of the project.

After that, the natural Phase 1 starter is replacing `configure.in` and `Makefile.am` with a `meson.build`, then getting CI to attempt a build. The build will fail loudly on missing GTK 1.2 — that failure is the trigger to start Phase 2 by switching the GTK pkg-config name to `gtk+-2.0`.
