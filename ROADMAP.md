# GtkHx Roadmap

GtkHx was last released in May 2003, as version 0.9.4, against GTK+ 1.2. The revival
carried it forward to GTK 4 + libadwaita and has been rewriting it in Rust ever since.

**This document is the product roadmap: what is left to build, and the decisions that
constrain how.** The port itself — which C files remain and in what order they move to
Rust — lives in [docs/rust/ROADMAP.md](docs/rust/ROADMAP.md). Smaller known gaps and
review follow-ups are in [BACKLOG.md](BACKLOG.md). Subsystem references are in
[docs/](docs/README.md); working notes for the codebase are in [CLAUDE.md](CLAUDE.md).

---

## The constraint everything else bends around

**Full backward compatibility with the Hotline 1.2 and 1.5 wire protocols is a hard
requirement at every stage.** There are only a handful of servers left in the wild and
some of them have not been touched in twenty years. A feature that needs the server to
change is not a feature we can ship unilaterally; it is a proposal. The difference now is
that there is a server to prototype it in: [hxd-ng](https://github.com/mishan/hxd-ng), the
Rust server that shares GtkHx's protocol crates. A proposal can be built on both ends,
written up as a spec, and offered to the rest of the ecosystem with a working
implementation behind it.

This is why the extensions GtkHx has adopted are all either negotiated (a capability bit
that legacy servers simply never set) or probed with a timeout and silently abandoned. No
extension is allowed to degrade the legacy path.

---

## Where things stand

| | |
|---|---|
| **Toolkit** | GTK 4 + libadwaita, with mullion-gtk for the dock. Light/dark/system theme tracking, and consistent `AdwHeaderBar` chrome. Chat, users, tasks, news, threaded news, voice and video are dockable panels in a persistable split layout; the file browser (one per connection), the tracker and the file preview are standalone windows. |
| **Build** | Meson + Cargo. Autotools, the RPM spec and the old `debian/` tree are gone. |
| **Language** | Hybrid C + Rust, with Rust now the larger half. C no longer grows: a CI check fails any pull request that adds net lines of C. See [docs/rust/ROADMAP.md](docs/rust/ROADMAP.md). |
| **Shared code** | The wire protocol, the file-transfer codec and the HFS sidecars come from [hx-libs](https://github.com/mishan/hx-libs), shared with the hxd-ng server. |
| **Connections** | Several servers at once, one tab each. Settings and connections live in TOML files owned by Rust (`hxconfig`, `hxbookmarks`). |
| **Protocol** | 1.2 / 1.5 / 1.9 compatible. Connect, HOPE negotiation and the ciphers (Blowfish OFB-64, ChaCha20-Poly1305 AEAD) all run in Rust. RC4 retired. Compression (GZIP, LZ4, ZSTD) is negotiated when the user picks it. |
| **Transport security** | TLS on a dedicated port, TOFU trust with fingerprint pinning. [docs/tls.md](docs/tls.md) |
| **Extensions** | Voice chat, video chat and screen sharing, inline media, GIF icons, chat history, colored nicknames, emoji shortcodes, tracker v3. |
| **Platforms** | Linux, macOS, Windows. The Flatpak, for x86_64 and aarch64, is signed and served from GtkHx's own repository at dl.gtkhx.org, with stable and beta channels; it updates like any other Flatpak app. [docs/flatpak-repo.md](docs/flatpak-repo.md) |
| **Testing** | Three tiers — unit, wire-fixture, and end-to-end integration against a Docker rig. Static analysis and sanitizers run in CI. |

The GTK climb (1.2 → 2 → 3 → 4), the build-system replacement, the crypto rewrite, the
widget replacements (`gtk_hlist` → `GtkColumnView`, `xtext` → a purpose-built Rust chat
widget), the preferences rewrite, packaging, and the test suite are all done. Git history
is the record of how; the subsystem docs are the record of what.

---

## Open work

### Multi-connection follow-through

Several simultaneous connections shipped in 1.3.0, under the tab-switched layout
(Model A): a tab per connection, each with its own panel set, identity overrides from
Settings → Connections, and an arbiter deciding which connection owns the microphone.
The transport, the signals and the connection-scoped keys all carry the connection now.

What is left is making the rest of the UI stop asking "which connection has focus?" when
it means "which connection is this for?":

- **Readers that still route through `hx_active_session()`.** The Rust UI reaches its
  connection through `gtkhx_active_htlc()` and friends in the news dialogs, the user
  editor, chat input, file info, the voice panel and the compose windows.
- **`thread_local` singletons in `gtkhx-ui`** that hold per-connection state: the
  threaded news browser and its in-flight fetches, the flat news view, the banner and the
  create-post window. `useredit.rs`, an id-keyed map, is the shape they all want.

Coexisting per-connection panels (Model B) remain undecided. Full survey:
[docs/multi-connection.md](docs/multi-connection.md).

### Chat and message logging

Still pending, and a fresh implementation: the original `log.c` was deleted rather than
carried forward, and its preference was dropped in the move to `hxconfig`. Nothing
resolves or creates a log directory.

Note this is a different feature from the chat-history extension, which replays
server-held scrollback on join. Local logging persists what *this* client saw, including
from servers that have no history support, and the two need to compose without duplicating
lines. That interaction is the interesting part of the design.

Unrelated to the `GTKHX_DEBUG` categorised logger, which is stderr instrumentation.

### New-user experience

First run is detected — the absence of a preferences file pops the settings dialog — but
what it pops is the raw settings dialog, which is a poor first impression and asks for
things a new user has no opinion about yet. A welcome dialog with an optional short wizard
(identity, pick a server, done) is the obvious shape.

Open questions, and the reason this hasn't been built: what tone to strike, whether to
ship a curated server list, and whether to ping those servers for liveness before showing
them — a list of dead servers is worse than no list.

### Accessibility

An accessibility review in October 2026 found the gaps below, in the order they are
being fixed. Settled along the way: Escape on the Agreement window disagrees (and so
disconnects), only private messages and mentions are announced, and these fixes may grow
the C side, with a `C-Growth` trailer, rather than wait for a port.

1. **Icon-only buttons have accessible names.** *Done.* `gtkhx_icon_button_new` /
   `gtkhx_set_name_tip` in C and `icon_button` / `set_name_tip` in `gtkhx-ui` set the
   name with the tooltip. `tools/check-icon-buttons.sh` holds buttons built from an icon
   name to them; it can't see menu buttons, toggles or buttons with an image child.
2. **The user list.** *Done.* The name cell, which draws the nick itself, is named with
   the nick and its away and admin states (`hx_user_row_name_cell`); the voice icons
   are named; and the per-user menu opens on Menu and Shift+F10 too, with focusable
   items.
3. **Keyboard reach.** *Done.* In the files window Tab and Backspace switch panels and go
   up only from a list; a link in a text view opens its menu on Menu, Shift+F10 or Enter
   at the cursor (F7 shows it), and the server banner is a focusable link; news
   compose, File Info and Agreement close on Escape; F10 opens the main menu; Enter in
   the Trackers address adds it.
4. **Labels.** *Done.* Chat views and text inputs are named (chat, private message,
   subjects, find, paths, tracker search, the bookmark name, news compose); transfer
   progress bars are labeled by their file; PDF pages, the media seek bar, file/folder
   and news icons, the previewed picture, the About logo, icon-picker cells and the
   inline-media dialog say what they are; and `preview.c`, `notify.c` and the tab-title
   fallbacks are translated. Still unnamed: the avatar preview in Settings, inline
   pictures in chat, and transfer thumbnails.
5. **Announcements.** *Done.* `announce.rs` (`gtkhx_announce` / `gtkhx_announce_line`
   from C) announces incoming private messages and mentions, a lost connection, an
   empty server address on Connect, a rejected push-to-talk key, find results and the
   emoji suggestion being picked. `gtk_accessible_announce` is 4.14, so it is built
   with the `v4_14` feature meson sets from the GTK it finds; below 4.14 nothing is
   announced, and the chat view's text isn't exposed either.
6. **Contrast.** *Done.* Under the system's high-contrast setting a theme's colors step
   aside for the default palette and the system chrome (`gtkhx_theme_set_high_contrast`),
   and the markdown delimiters keep the text color; links are always underlined, in the
   accent's text color rather than a fixed blue; and the nickname color's Clear is live
   only while a color is set, which tells "not set" apart from black.
7. **Later.** *Done.* F6 and Shift+F6 step between panes, from a text field too
   (mullion-gtk 0.3, which also brought the dock's own names, announcements and
   translations). A clicked notification opens the conversation it announced, which took
   GtkHx becoming one instance that owns its app id, as GNOME and the portal deliver a
   notification's action through it. A later launch hands its `--server` or `--bookmark`
   to the running one, which connects in a new tab. Ctrl+1 and on show the panels, and a
   Keyboard Shortcuts window (Ctrl+?) lists every key, bound from the same table
   (`shortcuts.rs`).

### Help

There is a `/help` command that prints the slash-command list into chat, and a Keyboard
Shortcuts window (Ctrl+?, and in the main menu) listing every key GtkHx binds. What is
left is help for the rest: what the panels are for, and how the protocol's quirks show up.

### Flatpak repository

Releases publish themselves to dl.gtkhx.org, and a release's metainfo shows that
release's screenshots.

### Plugin system

The original dlopen ABI was deleted rather than ported, and nothing replaced it. The
decision to make is whether a plugin system comes back at all, and in what form —
GIRepository-based, or a small embedded scripting hook (Lua, or GJS) for chat triggers,
auto-replies and command aliases. The old ELIZA plugin is not a reason to keep an ABI, but
"can I script a bot" is a real request from this ecosystem.

### Tracker search and pagination

Tracker v3 defines server-side text search and pagination that GtkHx does not yet use. The
design splits into a network-level tracker query and a client-side list filter; the client
filter stays regardless, because v1 trackers cannot search at all. See
[docs/tracker-protocol.md](docs/tracker-protocol.md).

A "tracker of trackers" — a server-discovered metadirectory rather than a user-managed
list of tracker addresses — is a bigger swing and remains undecided.

### Working with hxd-ng

hxd-ng is a Hotline server in Rust. It shares GtkHx's protocol crates through hx-libs and
implements most of the same extensions from the other end. It is in the test rig, but
only the video tests use it so far. What is left:

- **Point more of Tier 3 at it**: chat history, inline media, GIF icons, TLS, tracker
  registration, and voice alongside Janus. hxd-ng's own roadmap asks for the chat-history
  suite in particular.
- **Keep its pin current.** The rig builds a fixed hxd-ng revision, and it falls behind.
- **Share more code**: the tracker codec first, then HOPE. The plan and the order are in
  [docs/rust/ROADMAP.md](docs/rust/ROADMAP.md#shared-code-with-hxd-ng).
- **One home for extension specs**, and a wire-fixture corpus both projects run.

### Code modernization

Ongoing, opportunistic rather than swept: better module boundaries and more reuse, and
defining string and resource constants once instead of repeating them at each site.

---

## Known open defects

These are real, reproduced, and unfixed. Each is described in full in its subject doc.

- **Two voice defects against Janus's SFU**, both diagnosed as server-side. One is a
  publish-before-answer race, only reproducible with two real GUI processes — a client-side
  delay was tried and reverted. The other omits a spec-required attribute on renegotiation,
  breaking the first joiner's voice-activity indication until they rejoin.
  [docs/voice.md](docs/voice.md)
- **Window position is not restored on Wayland.** Size restores from preferences; position
  only restores if the compositor volunteers. GTK had a public API for this, then pulled it
  back for redesign — don't restart this work until it reappears upstream.

---

## Decisions locked in

These are settled. Don't reopen them without a strong new reason.

1. **Build system: Meson + Cargo.** Autotools is gone.
2. **Hard 1.2/1.5 wire compatibility**, as above. This outranks every other consideration.
3. **License: GPL-2.0-or-later.** Not v2-only, not v3-only. The "or later" clause stays.
   Some Rust crates are line-by-line derivative of the GPL reference server and therefore
   cannot be relicensed even if we wanted to; see [docs/rust/crate-layout.md](docs/rust/crate-layout.md).
4. **The plugin ABI was broken deliberately.** If a plugin system returns it will be a new
   design, not a revival of `MODULE_IFACE_VER 2`.
5. **Single-session during the ports.** *(Superseded, kept for the record.)* The ports
   were done against one session and refactored to N afterwards; multi-connection shipped
   in 1.3.0 under the tab-switched layout.
6. **RC4 is retired.** Removed from the cipher offer list and the connect dropdown.
   Advertising it under a "Secure" label gave users false confidence. Its protocol slot
   stays reserved so the integer is never reused.

   On-disk bookmark compatibility is handled by a stable cipher-byte vocabulary kept
   independent of the dropdown's ordering, so reordering the UI can never change the
   meaning of a byte already on disk. Retired ciphers keep their byte; new ciphers get new
   ones. A bookmark still holding the RC4 byte prompts for a replacement and is rewritten in
   place, so it only asks once.
7. **Crypto lives in Rust.** The original plan was GnuTLS + Nettle for ciphers and GLib's
   `GChecksum`/`GHmac` for hashes; what actually shipped is the `hxcrypto` crate (now in
   hx-libs, beside `hxhope`) plus `tokio-rustls` for TLS, and the C crypto dispatchers
   were deleted. The reasoning that
   picked a clean-licensed, non-OpenSSL stack still holds — the implementation just landed
   a layer over.
8. **The vendored chat widget was replaced, not ported.** *(Superseded decision, kept for
   the record.)* The original plan was to vendor HexChat's maintained xtext fork and keep
   it forever. That was the right call in its moment — it bought the GTK 2 → 3 → 4 climb
   cheaply — but a line-uniform layout model could not carry inline media or variable-height
   rows. It was replaced by a purpose-built Rust widget, since split out as
   [Rotulus](https://github.com/mishan/rotulus), and deleted. See
   [docs/chat-view.md](docs/chat-view.md) and the
   [xtext benchmark](https://github.com/mishan/rotulus/blob/main/docs/xtext-benchmark.md).

---

## Longer term: Hotline-ng

Every so often the question came up of designing a successor wire protocol — one with
real transport security, sane framing, and Unicode by construction, instead of extensions
bolted onto a 1996 format. It stayed parked for a social reason, not a technical one: a
new protocol needs servers, and the server population is small, mostly unmaintained, and
partly closed-source.

That has changed. hxd-ng serves **Hotline-ng**, a JSON protocol over WebSocket, next to
the legacy wire. GtkHx speaking it is a real possibility and a longer-term goal, but not
current work: Hotline-ng client development is focused on
[hx-ng](https://github.com/mishan/hx-ng), the browser client, because a client that runs
anywhere with nothing to install is what best shows what the protocol is for.

When GtkHx does take it on, the plan has to respect:

- **The legacy wire stays the priority.** Hotline-ng would be a second protocol GtkHx
  offers, never a replacement, and nothing in the 1.2/1.5 path may depend on it.
- **What it would unlock** that the extension route cannot: detach and resume, push,
  and the identity and moderation work hxd-ng is building.
- **Where the code lives.** A Rust Hotline-ng client library would belong in hx-libs,
  beside the legacy codec, with hx-ng's TypeScript client as the reference behavior.
- **The port comes first.** It is far cheaper to add a second protocol to a Rust
  connection layer than to one still threaded through `rcv.c` and `network.c`, so the
  remaining model-side port is effectively a prerequisite.

The extension route remains the right one for anything the legacy servers can adopt: TLS on
a dedicated port needed no protocol changes at all, and the capability-negotiated
extensions degrade cleanly on servers that never heard of them.
