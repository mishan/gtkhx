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
  - **`xtext.c`** — XChat 1.8.5's text widget (~3500 LOC). Immediate-mode GDK drawing.
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
6. **Replace `xtext.c`.** **Decision: drop it, use GtkTextView.** The mIRC color codes and transparency are not priorities, which removes the only two features GtkTextView would have struggled with — making this a clean win. Keep the message-formatting code in `chat.c` / `commands.c`; only the rendering substrate changes. URL highlighting and per-user nick coloring are straightforward with GtkTextTag + a click event controller. (HexChat's modern xtext fork — `src/fe-gtk/xtext.c` in the hexchat/hexchat repo — uses cairo + Pango and could be vendored if a feature gap turns up later, but it's ~6000 lines of someone else's code to own. GtkTextView is the smaller surface.)
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
- `xtext.c`'s mIRC color parser (`\003NN`) and the `g_user_colors[]` table need to survive the rewrite — those are wire-protocol-shaped, not toolkit-shaped.
- `news15.c` and `chat.c` both reach into widget internals (`GTK_TEXT(w)->vadj`). Replace with `gtk_scrolled_window_get_vadjustment()` / TextView's adjustments.

**Exit criteria:** `gtkhx` builds against GTK 2, launches, connects to a Hotline server, joins chat, downloads a file, posts news. UI looks crufty but works.

---

## Phase 3 — Port to GTK+ 3

**Goal:** Modern theming, cairo drawing, no deprecated APIs. After Phase 2 this is mostly cleanup.

**Work items**

1. **Drawing → cairo.** Anywhere `gdk_gc_*`, `gdk_draw_*`, or `expose_event` survived Phase 2 (custom drawing in `cicn.c` for icon rendering, anything left in xtext if you kept it), convert to cairo via `draw` signal handlers. `cicn.c` is the main custom-draw site outside the widgets you've already replaced.
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

**Goal:** Land on the current toolkit. The breaking changes here are conceptual, not just renames.

**Work items**

1. **Event controllers replace event signals.** `button_press_event`, `key_press_event`, `motion_notify_event` are gone. Use `GtkGestureClick`, `GtkEventControllerKey`, `GtkEventControllerMotion`. Affects every dialog and the chat/users widgets.
2. **No more `GtkContainer`.** Widgets are now parents of widgets directly; `gtk_container_add(window, child)` → `gtk_window_set_child(window, child)`. Boxes use `gtk_box_append`. Touches every UI file.
3. **No more `gtk_widget_show_all`.** Widgets are visible by default; explicit `gtk_widget_set_visible` where needed.
4. **GtkBuilder + UI XML.** Strongly recommended for GTK 4 — describe layouts in `.ui` files, bind callbacks in code. Pays for itself when you want HeaderBar, popovers, etc. Big rewrite of dialog construction code in `connect.c`, `options.c`, `usermod.c`.
5. **GtkColumnView replaces GtkTreeView** for the user list, file list, tracker list, news list. (GtkTreeView is technically still there in 4 but deprecated; use GListModel + GtkColumnView for new work.)
6. **Drag-and-drop is completely new.** If file-list DnD ever worked, it'll need the new `GtkDropTarget` / `GtkDragSource` API.
7. **HeaderBar replaces toolbar window.** The main toolbar in `toolbar.c` becomes a headerbar on the chat window, or stays as a sidebar — UX call. Either way, `GtkToolbar` is gone.
8. **No more X11 assumptions.** Wayland is the default. `USE_XLIB` and any `gdk/gdkx.h` paths get deleted (or properly conditionalized).
9. **GIO for file I/O and async.** Many of the hand-rolled pthread + queue patterns in `xfers.c` and `network.c` can become `GTask` + `GSocketClient` + `GFile`. Big simplification, but a real refactor.
10. **`GtkApplication` from Phase 3 stays, with adjustments.**

**Gotchas**

- The protocol-side code (`hotline.h`, `network.c`'s wire handling, `compress.c`, `cipher.c`) shouldn't change at all in this phase. If a Phase 4 patch touches `hotline.h`, something's wrong.
- The pthread-based xfer code can stay if you want; converting to GTask is a quality-of-life improvement, not a requirement.
- File dialogs change: `GtkFileChooserDialog` → `GtkFileDialog` (async, returns via callback).

**Exit criteria:** Runs natively on Wayland. Looks like a 2026 GNOME app. No deprecation warnings against current GTK 4.

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

- **Flatpak manifest.** Once on GTK 4, this is straightforward and is the path of least resistance for end users.
- **AppStream metadata** (`gtkhx.appdata.xml`) so it shows up in software centers.
- **`gtkhx.desktop`** with proper categories.
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
3. **Custom widget strategy → shim for `gtk_hlist`, in-place rewrite for `xtext`** (drop in favor of GtkTextView).
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
