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
- **Protocol compat:** Full backward compatibility with Hotline 1.2 and 1.5 is a **hard requirement**. The wire-format and HOPE negotiation code is not to be modernized in a way that breaks legacy servers — modern transport security is a separate Phase ∞ project that requires server-side cooperation.
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

## Phase 0 — Hygiene & baseline

**Goal:** Get the tree into a state where modern tooling can actually look at it without choking, and where every later phase has a clean starting point.

**Work items**

1. **Strip CVS / editor cruft.** `find . -type d -name CVS` (and the matching `Entries`/`Repository`/`Root` files), the `#…#` and `.#…` files in `src/`, and the stale `.cvsignore` files. These are all in git history if anyone wants them back.
2. **Untrack autotools-generated files.** `configure`, `aclocal.m4`, `Makefile.in`, `config.guess`, `config.sub`, `config.h.in`, `depcomp`, `install-sh`, `missing`, `mkinstalldirs`, `autom4te.cache/`, `intl/` (gettext shipped sources), `ABOUT-NLS`. Add to `.gitignore`. Keep `autogen.sh`, `configure.in`, `Makefile.am`, `acconfig.h`, `*.spec`, `debian/`.
3. **Add a CLAUDE.md / contributor notes file** capturing the architecture overview I sketched above, the threading model, and the wire protocol locations (`hotline.h`). This pays for itself on every later phase.
4. **Set up CI scaffolding** — even just a GitHub Actions matrix that runs `./autogen.sh && ./configure && make` inside a Docker image with whatever GTK version each branch targets. Catching breakage cheaply matters more as the surface area grows.
5. **Pick a license stance.** Code says "GPL v2 or later"; `LICENSE` in the root is already GPL. No work, just confirm intent.

**Exit criteria:** `git status` is clean after a build; CI green on `main` for whatever Phase 1 targets.

---

## Phase 1 — Modernize C and build, freeze GTK 1.2 baseline

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

## Phase 2 — Port to GTK+ 2

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

## Phase 3 — Port to GTK+ 3

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

## Phase 4 — Port to GTK 4

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
- Wayland is the only backend that matters in GTK 4 (the X11 backend exists but you can't assume it). The `USE_XLIB` define in `config.h` becomes a no-op; remove it.
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

- **Full backward compat with Hotline 1.2 and 1.5 is a hard requirement.** Don't break it. The wire-format code in `rcv.c`, `commands.c`, `hotline.h`, and the cipher/compress negotiation in `cipher.c`/`compress.c` should not change shape during the GTK ports — only the surrounding C and the libraries it leans on. If a refactor accidentally drops 1.2 compatibility, that's a regression.
- **Reimplement SOCKS support** (TODO item from 2003). mhxd has SOCKS5 client support too — worth seeing how they did it.
- **IPv6 cleanup.** Code already supports it via `getaddrinfo` but with `#ifdef USE_IPV6` everywhere — make it unconditional.
- **Audit the wire-level int handling.** The `HN16`/`HN32` byte-swap macros and `PACKED` structs in `hotline.h` work but should be replaced with explicit `g_ntohl` / `GUINT32_FROM_BE` and proper buffer-cursor reads. Reduces the chance of strict-aliasing or alignment bugs on ARM64.

**Plugin system**

- The dlopen ABI in `plugins/plugin_h.h` is a `MODULE_IFACE_VER 2` from 2002. Decide: keep, deprecate, or replace with GIRepository + scriptable plugins (Lua / Python / JS via GJS). The ELIZA chat plugin is fun-but-niche; not a strong reason to keep the API.
- A modern alternative is to expose a small in-process scripting hook (e.g., an embedded Lua interpreter) for chat triggers, auto-reply, command aliases.

**Packaging & distribution**

- ✅ **Flatpak manifest.** `com.nasledov.gtkhx.yml` at the repo root, GNOME 48 runtime, local-source variant for development. To submit to Flathub, swap the `dir` source for a `git` source pointing at a tagged commit and add screenshots to `data/screenshots/`.
- ✅ **AppStream metadata** (`data/com.nasledov.gtkhx.metainfo.xml`) — full Flathub-ready: id, name, summary, description, screenshots, releases, OARS-1.1 content_rating, branding colors. Validated at `meson test` time via `appstreamcli validate --pedantic`.
- ✅ **`com.nasledov.gtkhx.desktop`** with proper categories (Network/Chat/FileTransfer), validated at test time via `desktop-file-validate`.
- **Update or remove the RPM spec and `debian/`.** The `debian/` dir is 2003-era; most distros want a Flatpak / Snap these days, or rebuild from upstream tarball.

**Quality / process**

- **A test suite.** Even a tiny one. The protocol parsing in `rcv.c` (~1100 lines) is the natural place to start — pure functions, well-defined inputs. Add a `tests/` directory and a `make check` target.
- **Static analysis.** clang-tidy + scan-build + `-fanalyzer`. This codebase has 23 years of accumulated UB potential.
- **AddressSanitizer / UBSan** in CI for the test runs.
- **Use `g_autoptr` / `g_autofree` aggressively** once on Glib 2.44+ — eliminates a huge class of leak/cleanup bugs in the dialog code.

**UX features the original always wanted**

- Multi-server connections (`MAX_CONN > 1`, the abstraction is half-built).
- Real preferences UI (TODO calls out "fix preferences broken-ness").
- Logging (`log.c` exists but is `#if 0`'d out in `gtkhx.c`).
- Tracker tracker (tracker of trackers) — keep, drop, or replace with a curated list.

---

## Phase ∞ — Modernized Hotline protocol (joint with mhxd)

This is the back-of-the-roadmap "if we ever want real modern crypto" section. Calling it out separately because it's a fundamentally different kind of work.

The existing Hotline 1.x protocol is plaintext over TCP, with optional Blowfish/RC4 negotiated via HOPE — neither of which is meaningful security in 2026 (RC4 is broken, Blowfish-CBC has small-block issues). To get real modern transport security we'd have to:

1. **Design a new protocol layer** — call it Hotline-NG, or just rev the HOPE handshake to negotiate "TLS 1.3 from here on." Either works; the latter is less disruptive.
2. **Implement it on the server side.** mhxd is the obvious target since we'd be working with it as our reference server anyway. This is where contributing back upstream matters.
3. **Implement it on the client side in GtkHx**, gated behind a server capability flag so legacy 1.2/1.5 servers continue to work.
4. **Get other clients/servers to adopt it.** This is the part that makes this Phase ∞ — the Hotline ecosystem is a handful of users on a handful of servers, and convincing any of them to upgrade is a social problem, not a technical one.

**Realistic framing:** treat this as "nice if it happens, don't block on it." The crypto-stack choice (Nettle for Hotline ciphers, GLib for hashes) deliberately leaves room to add GnuTLS later as a separate dependency *only* if we get to the point where there's a server that supports it.

**A more pragmatic intermediate:** if you just want client↔server traffic off the wire in plaintext, a VPN/WireGuard tunnel between client and server is an ops-level fix that doesn't require touching either codebase.

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

---

## Crypto stack rationale

Hotline's wire protocol needs MD5 (auth challenge/response), HMAC (with negotiable hash — including HAVAL, which `hmac.c` checks for as `"HMAC-HAVAL"`), and optionally Blowfish / RC4 ciphers. IDEA is disabled (`CONFIG_NO_IDEA`).

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
- **`cipher.c`** → rewrite over Nettle (`nettle/blowfish.h`, `nettle/arcfour.h`). This is **only** for the existing Hotline `HOPE` cipher negotiation (Blowfish/RC4 over the legacy protocol). Still useful for client-to-client privacy on the rare server that supports it; survives unchanged.
- **`rand.c`** → replace with `getrandom(2)` directly on Linux, fall back to `/dev/urandom`. Or use Nettle's `yarrow256_*`. No need to drag in OpenSSL just for random bytes.

**What this is *not*:** This is *not* a path to TLS. There is no Hotline server in the wild speaking TLS — the protocol predates that ever being a thing, and Hotline is dead enough that nobody's adding it. Real modern transport security is a separate, much bigger initiative: see "Phase ∞ — Modernized Hotline protocol" below.

---

## Effort sketch (very rough)

| Phase | Scope | Rough effort |
|---|---|---|
| 0 | Hygiene & CI | 1–2 days |
| 1 | Build modernization, C cleanup | 1–2 weeks |
| 2 | GTK 2 port (gtk_hlist, xtext, signals, threads) | 4–8 weeks — the big one |
| 3 | GTK 3 port (cairo, threading, deprecations) | 2–3 weeks |
| 4 | GTK 4 port (event controllers, GtkBuilder, GtkColumnView) | 3–5 weeks |
| 5 | Modernizations (TLS, packaging, tests) | ongoing |

Done in evenings/weekends, multiply by ~3.

---

## Suggested next concrete step

Start with **Phase 0 step 1**: rip out the CVS dirs, the emacs autosave/lock files, and the regenerable autotools artifacts; commit. That gives us a clean tree to actually diff against in Phase 1, and it's the kind of work where the "before" state is actively misleading every grep for the rest of the project.

After that, the natural Phase 1 starter is replacing `configure.in` and `Makefile.am` with a `meson.build`, then getting CI to attempt a build. The build will fail loudly on missing GTK 1.2 — that failure is the trigger to start Phase 2 by switching the GTK pkg-config name to `gtk+-2.0`.
