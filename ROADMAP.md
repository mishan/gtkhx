# GtkHx Roadmap

GtkHx was last released in May 2003, as version 0.9.4, against GTK+ 1.2. The revival
carried it forward to GTK 4 + libadwaita and has been rewriting it in Rust ever since.

**This document is the product roadmap: what is left to build, and the decisions that
constrain how.** The port itself — which C files remain and in what order they move to
Rust — lives in [docs/rust/ROADMAP.md](docs/rust/ROADMAP.md). Subsystem references are in
[docs/](docs/README.md); working notes for the codebase are in [CLAUDE.md](CLAUDE.md).

---

## The constraint everything else bends around

**Full backward compatibility with the Hotline 1.2 and 1.5 wire protocols is a hard
requirement at every stage.** There are only a handful of servers left in the wild and
some of them have not been touched in twenty years. A feature that needs the server to
change is not a feature we can ship unilaterally; it is a proposal, and it belongs in the
"needs ecosystem cooperation" bucket at the bottom of this file.

This is why the extensions GtkHx has adopted are all either negotiated (a capability bit
that legacy servers simply never set) or probed with a timeout and silently abandoned. No
extension is allowed to degrade the legacy path.

---

## Where things stand

| | |
|---|---|
| **Toolkit** | GTK 4 + libadwaita + libpanel. Light/dark/system theme tracking, and consistent `AdwHeaderBar` chrome. Chat, users, tasks, news, threaded news and files are dockable panels in a persistable split layout; the tracker and the file preview are standalone windows. |
| **Build** | Meson + Cargo. Autotools, the RPM spec and the old `debian/` tree are gone. |
| **Language** | Hybrid C + Rust, with Rust now the larger half. See [docs/rust/ROADMAP.md](docs/rust/ROADMAP.md). |
| **Protocol** | 1.2 / 1.5 / 1.9 compatible. Connect, HOPE negotiation and the ciphers (Blowfish OFB-64, ChaCha20-Poly1305 AEAD) all run in Rust. RC4 retired. Compression is implemented but not currently negotiated — see the defects below. |
| **Transport security** | TLS on a dedicated port, TOFU trust with fingerprint pinning. [docs/tls.md](docs/tls.md) |
| **Extensions** | Voice chat, inline media, GIF icons, chat history, colored nicknames, emoji shortcodes, tracker v3. |
| **Platforms** | Linux, macOS, Windows. Flatpak manifest + AppStream metadata are Flathub-ready. |
| **Testing** | Three tiers — unit, wire-fixture, and end-to-end integration against a Docker rig. Static analysis and sanitizers run in CI. |

The GTK climb (1.2 → 2 → 3 → 4), the build-system replacement, the crypto rewrite, the
widget replacements (`gtk_hlist` → `GtkColumnView`, `xtext` → a purpose-built Rust chat
widget), the preferences rewrite, packaging, and the test suite are all done. Git history
is the record of how; the subsystem docs are the record of what.

---

## Open work

### Multi-server connections

The one long-promised feature that has never landed. The original code shipped a
`MAX_CONN` array that always returned element zero; that fiction has been replaced with an
honest single-session model plus a routing seam, so "which session" is now an explicit
question at every call site rather than an assumption.

Two decisions remain genuinely open: **when** this lands relative to the Rust port, and
**how** per-connection panels relate to the docking layout. The second is the expensive
one — one of the two candidate layouts forces the panel registry's key to grow a dimension
and the layout file's grammar to grow with it.

Full survey, including the panel-scope trichotomy and the voice-exclusivity arbiter:
[docs/multi-connection.md](docs/multi-connection.md).

### Chat and message logging

Still pending, and further from done than it looks. The original `log.c` was deleted rather
than carried forward; the preference survives but both its change hook and the one call
site that would have written a line are inside `#if 0`. `$CONFIG/logs/` is named in a
comment and nowhere else — nothing resolves or creates it. This is a fresh implementation.

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

### Ship useful defaults

Related, and cheaper. The bookmark list is bootstrapped with a single built-in and the
tracker list defaults to a single address — enough that neither list is empty, not enough
to be useful, and both point at hosts that may not have survived. Ship a handful of each,
and check they still answer before shipping them.

### Help

There is a `/help` command that prints the slash-command list into chat, and that is all.
There is no shortcuts window. At minimum the keyboard shortcuts need to be discoverable —
several are not guessable, and some function-key bindings in the file browser get stolen by
the desktop compositor, which a shortcuts window could at least explain.

### Publish to Flathub

The manifest, AppStream metadata and desktop file are all validated at test time and
Flathub-ready. What's left is swapping the local source for a tagged git source and adding
screenshots.

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

### Code modernization

Ongoing, opportunistic rather than swept: better module boundaries and more reuse, and
defining string and resource constants once instead of repeating them at each site.

---

## Known open defects

These are real, reproduced, and unfixed. Each is described in full in its subject doc.

- **Compression is never negotiated.** The Rust connect orchestrator offers an empty
  compression-algorithm list, so zlib compression is available in the implementation but
  never turned on against a server that would accept it. [docs/rust/networking.md](docs/rust/networking.md)
- **Animated media has no offscreen gating.** The chat view installs a frame tick whenever
  any media in the buffer is animated, with no visibility test, so scrolled-away GIFs keep
  costing frames. An earlier design had this gating; it did not survive the chat-view
  rewrite. [docs/image-decoding.md](docs/image-decoding.md)
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
5. **Single-session during the ports.** Do not add multi-connection abstractions
   speculatively; do the ports against one session and refactor to N against a smaller
   codebase.
6. **RC4 is retired.** Removed from the cipher offer list and the connect dropdown.
   Advertising it under a "Secure" label gave users false confidence. Its protocol slot
   stays reserved so the integer is never reused.

   On-disk bookmark compatibility is handled by a stable cipher-byte vocabulary kept
   independent of the dropdown's ordering, so reordering the UI can never change the
   meaning of a byte already on disk. Retired ciphers keep their byte; new ciphers get new
   ones. A bookmark still holding the RC4 byte prompts for a replacement and is rewritten in
   place, so it only asks once.
7. **Crypto lives in Rust.** The original plan was GnuTLS + Nettle for ciphers and GLib's
   `GChecksum`/`GHmac` for hashes; what actually shipped is the `hxcrypto` crate plus
   `tokio-rustls` for TLS, and the C crypto dispatchers were deleted. The reasoning that
   picked a clean-licensed, non-OpenSSL stack still holds — the implementation just landed
   a layer over.
8. **The vendored chat widget was replaced, not ported.** *(Superseded decision, kept for
   the record.)* The original plan was to vendor HexChat's maintained xtext fork and keep
   it forever. That was the right call in its moment — it bought the GTK 2 → 3 → 4 climb
   cheaply — but a line-uniform layout model could not carry inline media or variable-height
   rows. It was replaced by a purpose-built Rust widget and deleted. See
   [docs/chat-view.md](docs/chat-view.md) and
   [docs/chat-view-benchmark.md](docs/chat-view-benchmark.md).

---

## Parked: a modernized Hotline protocol

Every so often the question comes up of designing a successor wire protocol — one with
real transport security, sane framing, and Unicode by construction, instead of extensions
bolted onto a 1996 format.

It stays parked, and the reason is not technical. A new protocol needs servers to
implement it, and the server population is small, mostly unmaintained, and partly
closed-source. This is a social problem, not an engineering one. The productive version of
this instinct has been the extension route: TLS on a dedicated port needed no protocol
changes at all and works against real servers today, and the capability-negotiated
extensions degrade cleanly on servers that never heard of them.

If it ever becomes worth attempting, the reference server is the natural first target.
