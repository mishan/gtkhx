# GtkHx Rust Roadmap

This is the plan for replacing the C codebase of GtkHx with Rust,
**incrementally and leaf-up**, while keeping a working GTK 4 + libadwaita
binary every step of the way. It is a sibling to the root `ROADMAP.md` (the
product / feature roadmap) — start there for what the client is supposed to
*do*; this document is about what it is written in.

The two roadmaps share an exit criterion: **full backward compatibility with
the Hotline 1.2 and 1.5 wire protocols is a hard requirement at every step**.
We don't get to break the handful of legacy servers still in the wild.

Companion documents in this directory:
[`crate-layout.md`](crate-layout.md) (how the Rust crate graph is arranged and
why), [`glib-interop.md`](glib-interop.md) (the Rust ↔ GLib ref-counting and
async conventions), and the per-subsystem scoping notes
([`../docking.md`](../docking.md),
[`preview-porting.md`](preview-porting.md), and friends).

---

## Why incremental, why leaf-up

Three motivations, locked in during the kickoff conversation:

1. **Memory safety / robustness.** The cipher state machine, the receive-side
   wire parser, and the file-transfer worker threads were the highest-risk C in
   the tree — manual buffer management, hand-written byte-swap macros,
   pthread/`g_idle` marshalling. Rust eliminates the categories of bug that
   regularly cost time during Tier 3 debugging.
2. **Better concurrency.** The old pthread + `g_main_context_invoke` pattern
   worked, but it was the wrong shape for the multi-connection tabbed UI: each
   connection owned one global `htlc_conn`. tokio (in a dedicated thread, with
   GLib's main context as the UI side) is the modern, well-trodden pattern; the
   migration also let us drop the last `pthread_create` call sites.
3. **Modernization for contributors.** Rust + gtk4-rs is what a GNOME
   contributor expects to encounter when they file an issue and want to fix it.
   The C of 2003 is not.

**Notably not a motivation: shipping a reusable `libhotline` crate for other
clients.** We are producing one structurally — the leaf-up extraction naturally
yielded a clean protocol crate in `hotline-proto` — but we do not optimize for
external consumers and do not freeze APIs for them. If a TUI client ever wants
it, it can vendor the version it likes.

That remains the accurate description of today's intent, and it is a real
constraint on the rest of the plan: it is why `hotline-proto` carries its C ABI
unconditionally, why every crate is `publish = false` at `0.1.0`, and why we
refactor `pub` signatures freely. `crate-layout.md` §5 sets out what would have
to change to reverse it — a Cargo feature gating the C ABI, a rename away from
the internal `hx*` prefix, a `missing_docs` pass, a semver commitment at `0.x`,
and (for the crates that are hxd-derived and must stay GPL) an honest read of
who the audience actually is. **If that work is ever taken up, this paragraph
is the first thing to change** — otherwise the next person reading the roadmap
will make choices that undo it.

The leaf-up strategy comes from
[the librsvg precedent](https://blogs.gnome.org/alatiera/category/librsvg/):
keep a working binary throughout, push the C ↔ Rust boundary outward from the
leaves, and let the public C "API" of each replaced file become the FFI surface
of the new Rust crate. librsvg finished its port over five years while
remaining a shipping GNOME library the whole time. That cadence is realistic
for us too.

---

## Locked-in decisions

These were settled before planning began. Re-litigating them mid-port costs
more than the gain. Two have since been superseded by events and are marked as
such rather than deleted, because the reasoning is still worth knowing.

1. **Build system: Meson stays primary, invokes Cargo for the Rust workspace.**
   The Rust code lives under `rust/` as a Cargo workspace; `rust/meson.build`
   runs `cargo build --release` via a `custom_target` and links the produced
   archive into the C binary. We accept that
   [this combination is notoriously chimeric](https://discourse.gnome.org/t/projects-with-rust-code-should-not-mix-meson-and-cargo-building/28612)
   — librsvg, Fractal, GNOME Loupe and every other modern GNOME-Rust app
   already pay this tax, and the patterns are well-trodden. Not using
   `corrosion-rs` (CMake-specific).

2. **FFI direction: C → Rust only, with hand-declared `extern` blocks on the C
   side.** Rust crates expose `#[no_mangle] pub extern "C"` functions and C ABI
   types; the C translation unit that calls them declares the prototypes
   itself. Signature drift surfaces at link time as an undefined symbol, which
   is enough for opaque-pointer APIs and keeps the build simpler. We don't use
   `rust-bindgen` because the dependency only flows one way, leaf-up.

   > **Amended.** The original decision said the headers would be
   > cbindgen-generated. In practice the generated headers were never included
   > by C code, so the crates skipped cbindgen entirely rather than carry build
   > machinery for an unused output. The hand-declared form is what the tree
   > does everywhere today (see `src/hxnet_bridge.c` for a representative
   > example) and what `rust/meson.build` documents.

3. **UI toolkit: gtk4-rs + libadwaita-rs.** Same widget set as the C code, so
   the AdwHeaderBar / AdwToast / AdwPreferencesDialog choices carry across
   unchanged. The workspace pins the gtk-rs-core 0.21 family (glib/gio 0.21,
   gtk4 0.10, libadwaita 0.8) — see `rust/Cargo.toml` for the Debian-stable
   rustc floor that keeps us off the next line up.

4. **Async runtime: tokio in a dedicated thread, GLib MainContext on the UI.**
   The documented gtk-rs pattern. Heavy IO (transfers, banner fetch, tracker
   fetch) runs as tokio tasks; light async work runs on the GLib executor via
   `glib::spawn_future_local`. Conventions, capacities and the re-entrancy
   rules are in [`glib-interop.md`](glib-interop.md).

5. **Crypto: RustCrypto crates.** `md-5`, `sha1`, `sha2`, `hmac`, `blowfish`,
   `chacha20poly1305`. All pure-Rust, audited, MIT/Apache-2.0 (GPL-compatible).
   HAVAL — historically advertised but unused — was deleted before the port
   began; RC4 was retired separately and is not on the list.

6. ~~**Custom widgets stay vendored C: xtext does not get rewritten.**~~
   **Superseded.** The original reasoning was that HexChat's xtext fork was
   thousands of lines of cairo + Pango + mIRC colour parsing that upstream
   maintained and we benefited from for free, so wrapping it as a gtk-rs
   subclass was acceptable but rewriting it was not in scope.

   It was rewritten anyway, and the vendored widget is deleted. The chat output
   surface is now `hxchat-layout` (a dependency-free layout engine — spans,
   wrapping, a chunked prefix-sum height index, scroll anchoring, selection,
   search) plus `hxchat-view` (the GTK4 widget). `src/chat_view.h` is a
   declaration header; there is no `chat_view.c`. See
   `docs/chat-view.md` for the case that overturned this decision and
   `docs/chat-view-benchmark.md` for the measurements.

7. **`gtk_hlist_compat` dies on the way through, not separately.** *(Done.)*
   Its consumers were each rewritten directly against `GtkColumnView` /
   `gio::ListStore`, and the shim disappeared with its last consumer. Every
   Rust list widget is a `GtkColumnView` / `GtkListView` / `GtkListBox` from
   the start.

8. **Single-connection during the port.** `MAX_CONN > 1` and the tabbed UI wait
   until the port is far enough along that a multi-conn refactor lands against
   mostly-Rust code, not half-Rust-half-C state machines. See
   [multi-connection](#multi-connection--tabbed-ui) below.

9. **License stays GPL-2.0-or-later.** Every Rust crate we pull in must be
   GPL-2-compatible: MIT, Apache-2.0, BSD, LGPL. RustCrypto is dual
   MIT/Apache, gtk-rs is MIT, tokio is MIT — clean. The provenance side (which
   of *our* crates could ever be relicensed) is in `crate-layout.md` §4.

10. **Crate layout: one staticlib façade; crates split on design, not link
    graph.** `gtkhx-ffi` is the workspace's only `staticlib` and the only
    archive on the C link line. Full rationale, plus the three constraints that
    keep the surviving crate boundaries, in
    [`crate-layout.md`](crate-layout.md).

---

## What has already moved to Rust

Roughly leaf-up, in the order it happened. Git history has the detail; this is
the map.

| Area | Where it lives now | What it replaced |
|---|---|---|
| Build plumbing | `rust/meson.build` + the Cargo workspace | — |
| Crypto + transport compression | `hxcrypto` (hash / stream / aead / compress) | `hmac.c`, `cipher.c`, `cipher_aead.c`, `compress.c`, `md5.c`, `sha.c`, `haval.c` |
| Wire protocol — parsers, builders, framing, Mac Roman text, dates | `hotline-proto` | the byte-twiddling half of `rcv.c` / `proto_helpers.c` / the `hlwrite` send path |
| Connection lifecycle: connect, magic, LOGIN, HOPE, ciphers, compression, TLS | `hxnet` + `hxbridge` (tokio runtime + GLib ferry) | `network.c`'s connect/decode state machine, `hope.c`, `network_decode.c`, `connect_magic.c` |
| HTXF file transfers — subchannel transport, the recv/send/folder byte loops, `htxf_conn` storage and lifecycle, the worker shell | `hxnet::{htxf,xfer,xfer_handle}` + `hxhandlers::xfer` | `xfers.c`, `xfers_recv.c`, `xfers_send.c`, `htxf_io.c`, `htxf_subchannel.c`, `gtkthreads.c` |
| HFS sidecar / resource-fork I/O; FFO+FILP fork-header codec | `hxhfs`, `hxfiles-xfer` | `hfs.c` and the fiddly byte math in `xfers.c` |
| Tracker fetch (HTRK v1 + v3, TLS, probe-fallback) | `hxnet::tracker` | `network.c`'s `GSocketClient` tracker state machine |
| Banner fetch (URL mode over `ureq`, file mode over HTXF) | `hxnet` + `gtkhx-ui::banner` | `banner.c`, `banner_dispatch.c`, the `libsoup` dependency |
| `GtkhxSession` GObject + its boxed signal payloads + `htlc_conn` accessors | `gtkhx-core` | `gtkhx_session.c`, the boxed types in `proto_helpers.c` / `tracker_event.c`, `hxconn.c` |
| Receive- and send-side protocol handlers | `hxhandlers::{recv,send}` | the per-opcode handler bodies in `rcv.c` and the scattered `hlwrite` call sites |
| Task registry + the send primitive | `hxtask` | `tasks_table.c`, the variadic `hlwrite` |
| Client-side models: chat / membership / conversation registry, news, files | `hxmodel` | `struct chat` + `gchats`, the news GUI structs, `filelist_walker.c` |
| Chat output surface | `hxchat-layout` + `hxchat-view` | vendored `xtext.c` |
| Windows and dialogs | `gtkhx-ui`, module per window | see below |
| TLS trust store (TOFU + SHA-256 pinning) | `hxtls-trust` | `tls_trust.c`, `tls_trust_dialog.c` |
| Bookmarks (HTsc format, legacy import, cipher vocabulary) | `hxbookmarks` | `bookmarks_io.c`, `bookmark_rc4_dialog.c`, `cipher_vocab.c` |
| Voice chat, end to end | `hxvoice` (state machine), `hxvoice-runtime` (webrtcbin), `hxvoice-model`, `hxvoice-send` | `voice.c`, `voice_panel.c`, `voice_model.c`, `voice_ptt.c` |
| Text encoding + emoji shortcodes; Mac resource fork + cicn decode; image decode; sound playback | `hxtext`, `hxmacres`, `hx-image-decode`, `hxsound` | `text_util.c`, `macres.c`, the decode half of `cicn.c`, GSound |

**Windows and dialogs.** Every window's *shell* — its dock registration or
top-level construction and lifecycle — is Rust. Fully content-ported: the
Tracker; About / Agreement / User Editor; Connect and Bookmarks; the Settings
form (all but the two custom-widget pages, which keep C draw functions); the
TLS-trust prompt; the user list view and row; the private-message and
private-chat tab content; the whole threaded 1.5 news browser and the flat
1.0/1.2 news viewer; the standalone dialogs (User Info, Create Post, Broadcast,
inline-media view, emoji picker and `:shortcode:` typeahead); and the voice UI.
What is still C behind a Rust shell is the [inventory](#inventory--whats-still-c)
below.

**Concurrency.** There is no `pthread_create` in the tree. Worker threads are
tokio tasks (or blocking-pool tasks) that marshal to the main thread through
the `hxbridge` ferry or `g_idle_add`; the GLib timers that remain are the ones
that drive C-side state and gain nothing from a tokio `Interval` — the ping
keepalive, the post-login SELFINFO fallback, the fetch drains, UI debounce.

---

## Durable findings

Things that cost real time to learn and would cost it again.

### Crypto

- **Rust crate panics are a bug class at the FFI boundary.** The first
  extraction had constructors panicking on an invalid key length and
  `.expect()`-ing on compressor init failure — both turn a malformed server
  reply into a client abort. The rule since: fallible construction returns
  `Option`/`bool` from Rust, NULL across the FFI, and the C side fails closed.
- **A shared cipher-state struct is asserted at compile time on both sides.**
  The AEAD state was the first: `hxcrypto`'s
  `const _: () = assert!(size_of::<AeadState>() == …)` was paired with a
  `_Static_assert` on the same size in the C header, so a field reorder on
  either side tripped a build error rather than a misalignment at decrypt time.
  (The C half went away with the C crypto dispatchers; the Rust assert and its
  reasoning stayed, because it also documents *why* a size pin is equivalent to
  a field-offset pin for that struct shape.) Every cross-language struct added
  since follows the same pattern — C `_Static_assert` against Rust `size_of` /
  `align_of` / `offset_of` consts. `tasks_bridge.c` and `inline_media_decode.c`
  are current examples, the latter pinning enum discriminants as well as
  layout.
- **Blowfish rollback snapshots state; it does not clone it.** Speculative
  `cipher_decode` needs to be able to roll the cipher back. Cloning the whole
  Rust state meant a key-schedule-sized allocation per Hotline transaction. The
  shipped form exposes save/restore entry points that snapshot only the OFB
  feedback state into a stack buffer.
- **The legacy `key||text` hash branches are pinned byte for byte.** Tier 1
  tests assert the hand-computed digests of the concatenated form for each
  supported hash, so a future "consistency fix" can't quietly rewrite the
  branch into RFC 2104 HMAC and silently break HOPE login against legacy
  servers.

### Wire protocol

- **Endianness.** Every multi-byte integer on the Hotline wire is big-endian.
  RustCrypto's APIs are byte-oriented and don't care, but the byte-swap macros
  lived alongside cipher code in places. Don't drop the swap.
- **Mac Roman ↔ UTF-8 conversion belongs to the protocol layer.** It lives in
  `hotline-proto`'s `text` module and matches glibc's `iconv` `MACINTOSH` table
  byte for byte. (Not to be confused with `hl_code.c`, the unrelated XOR-0xff
  obfuscation of LOGIN/PASSWORD chunks.)
- **The HOPE rekey marker is wire-format-critical.** A random nibble in the
  type field's high byte triggers N rounds of HMAC-stretching the cipher key.
  It was ported byte for byte rather than refactored on the way, and the
  frame-aware Rust adapter mirrors the original's read-side parse and write-
  side probability exactly. Don't tidy it.
- **Frame the read stream by `DataSize`, not `TotalSize`.** Getting this wrong
  desynced against a fragmenting server and surfaced as "unknown header type".
- **`#[non_exhaustive]` on the opcode enum.** The 1.9 additions live alongside
  the 1.2/1.5 ones and servers occasionally add more.

### Boxed signal payloads

The `GtkhxSession` signals whose payloads aren't scalars — the chat event and
its attached media, the message event, the tracker server record and its v3
metadata, the chat-history entry, the inline-media handle table — are glib
boxed types. They live in `gtkhx-core::boxed`, in the **same crate as the
session GObject that emits them**.

They spent a period in a crate of their own, and the reason is worth
remembering because it was a link-graph artefact rather than a design one: when
every crate produced its own `staticlib`, two archives could each bundle the
boxed types' `_copy`/`_free` and collide at the final link, and a proto unit
test that pulled one `_copy` would drag in the session crate's dangling
externs. The single-façade architecture dissolved both problems — one archive,
each `#[no_mangle]` symbol defined exactly once — and the crate merged in.

**One constraint survives and still shapes the crate.** `gtkhx-core` must stay
free of undefined external symbols, because the Tier 2 proto tests link its
standalone archive *alone*. That is why the per-session task registry did not
merge in with the rest. See [`crate-layout.md`](crate-layout.md) §2b.

The mechanics, which are the part to copy when adding a new payload type:

- **Only the boxed type moved; the struct layout stays C-visible.** C producers
  still allocate and fill the struct, and C consumers still read fields
  directly. So each Rust type is a `#[repr(C)]` mirror with its byte layout
  pinned on both sides — `_Static_assert(sizeof(...) == N)` in C against
  `const _: () = assert!(size_of::<…>() == N)` (and `offset_of!` where field
  positions matter) in Rust.
- **Copy and free go through glib's allocator** (`g_malloc0` + `g_strndup` /
  `g_free`), so a value made by a C `hx_*_new` and one made by a Rust `_copy`
  release through the same path.
- **A wide struct can be modelled opaquely.** The tracker v3 metadata carries
  dozens of fields; rather than transcribe them, `gtkhx-core` models it as a
  correctly-sized, correctly-aligned buffer whose copy/free fix up only its
  owned `char *` fields **by byte offset**, with the size and every offset
  pinned by `_Static_assert`s on the C side. The UI crate separately carries a
  fully typed mirror of the same memory, because the tracker window needs to
  *read* the fields. Two intentional views, both const-asserted.

### Cross-thread lifecycle and cancellation

The file-transfer handle is the one object genuinely shared between the GLib
main thread and a worker, and getting it right produced three findings that
generalize:

- **An intrusive atomic refcount, not `Arc`, across the FFI.** The lifetime
  pattern is "N pending idle callbacks each hold a reference", which maps badly
  onto `Arc` over a C boundary. The handle keeps `AtomicI32` refcount, cancel
  flag and byte counter as fields of a `#[repr(C)]` mirror, with a registered
  last-unref destructor, behind explicit `ref`/`unref` entry points.
- **Blocking-pool tasks cannot be force-cancelled.** There is no
  `pthread_cancel` equivalent, so cancellation is cooperative: an abort token
  shuts the subchannel socket down to wake a parked blocking read, which
  returns an error and unwinds the loop. Critically, the token is published
  into the handle **unconditionally**, with the socket shutdown as a
  best-effort extra — so a cancel is still observed by the read's pre-check
  even when the socket can't be duplicated. Never leaving the handle unarmed is
  what keeps a transfer cancellable at all.
- **Layout assertions find portability bugs, not just refactoring bugs.** The
  runtime layout test for that handle immediately caught that `compat.h`
  hard-clamps `MAXPATHLEN` to a fixed value rather than the host's `PATH_MAX`
  — so every `#[repr(C)]` mirror of a struct containing a path buffer has to
  use the clamped size, not the platform's.

There is also a standing ordering rule that came out of a transfer-completion
hang: the completion cleanup runs at `G_PRIORITY_DEFAULT_IDLE`, **below** the
progress-update idles, so the updates drain before the object they describe is
torn down.

### TLS integration — the option taken, and the two that weren't

TLS runs through `tokio-rustls`: the connection is wrapped from byte zero, with
a WebPKI-first verifier that falls back to the trust-on-first-use known-hosts
store only when WebPKI validation fails. HOPE-on-TLS is rejected up front
(redundant double encryption). HTXF subchannels use the same path.

For the record, the options that were **not** taken:

- **TLS terminates in C, plaintext bridged over a `socketpair`.** A tactical
  stepping stone only — it would have left a permanent extra hop.
- **A permanent split, leaving TLS on the legacy `GIOStream` path.** Rejected
  because it would have blocked deleting the C stream helpers, which was most
  of the value of the migration.

### gtk4-rs traps

Two that every window has to respect:

- **The app initializes GTK from C, so gtk4-rs's own init flag is unset.** Call
  `gtk::set_initialized()` at each construction site or every widget/model
  constructor aborts.
- **Never write qdata (`set_data`) onto `GtkColumnView`'s internal cell or row
  widgets.** It corrupts GTK's cell recycling and frees a live cell — the
  symptom is a first-row use-after-free surfacing as a `GTK_IS_ACCESSIBLE`
  failure. Right-click row detection stashes the row position on the cell's own
  label instead.

Two smaller ones worth carrying:

- **`GtkTreeListModel` decides expandable-vs-leaf once.** Attach children to a
  node before appending the node; the child-model function fires once and the
  verdict sticks.
- **Templates are a per-window choice.** gtk4-rs supports
  `gtk::CompositeTemplate`, which turns a long run of `child.set_parent()` into
  XML. Small windows stay code-driven; big ones are worth a template.

### Permanent seams, not TODOs

Each shell port leaves a thin C `gtkhx_<win>_build_content` (plus an optional
`_after_embed`); each content port leaves a small accessor/setter seam
(`hx_msgwin_*`, `hx_gchat_*`, the `HxUserListView` FFI). These are the leaf-up
boundary. They stay until the corresponding deeper layer (model, wire, dock) is
itself ported — they are not churn to be removed.

### The chat model's end state

The per-chat model and window were reshaped into Rust rather than ported field
for field. The original tangle kept two lockstep per-session hashtables plus a
god-object mixing a model back-pointer, several live widget handles, command
history, render cursors and an inline-media table — with membership stored
twice. That is now three separate concerns: a pure, testable
`Conversation`/`Member` model with no GTK (nick completion and tab-cycle are
methods on it, unit-tested without a display); a `gio::ListModel` of members
that the user list binds to directly, as the single source of truth; and a
per-conversation view object. The single per-conversation registry moved to
Rust as well and is what the multi-connection design builds on. Wire compat was
untouched throughout — this was client-side state shape only.

**What remains is the irreducible C view leaf**: `struct gtkhx_chat`, now
opaque (defined privately in `chat.c`, reached through `hx_gchat_*`
accessors), holding the GTK widget handles — window, scrollbar, output, input,
subject entry, voice panel, media-attach button — plus the user-list widget,
the view's own `cid`, and the chat-history render cursors. The two Rust *data*
handles that were conversation state rather than view state (input history, the
media table) moved into the model, so a private chat's typed history now
survives closing and reopening its window. `cid` deliberately stayed in the
view: it is the view's self-identity for its own lookup, not a redundant
back-pointer.

**This is a permanent seam by design, not a TODO.** It closes when the chat
window's content itself ports, not before.

---

## Inventory — what's still C

With every window's shell in Rust, the remaining surface is (A) a few
standalone windows, (B) the *content* still living behind the shells, and (C)
shared infrastructure. This is the honest ledger.

### A. Standalone windows

The small self-contained pool is drained; three larger items remain.

- **Preview window** (`preview.c`) — text / image / PDF / source viewers plus
  HTXF-worker marshalling. Deferred: it hinges on `sourceview5` and a poppler
  crate aligning with the pinned gtk4 family, gated behind Cargo features the
  way the existing `HAVE_POPPLER` / `HAVE_GTKSOURCEVIEW` gates work. See
  [preview-porting.md](preview-porting.md). It is a plain
  `GtkWindow` with no dock involvement, so it is free of the libpanel question.
- **System tray** (`tray.c`).
- **Files path-completion popover** (`files_complete.c`).

### B. Content still C inside a Rust window shell

Each of these is a *content* port of the same shape as the user-list, private
message and private chat ports: build the widget tree in gtk4-rs and keep
genuinely-C leaves behind FFI. This is the big remaining category.

- **Files browser** — the largest content port left: the two `files_panel`
  `GtkColumnView`s, drag-and-drop (`GtkDropTarget`), the three providers
  (`files_provider.c`, `files_local_provider.c`, `files_remote_provider.c`),
  `files_ops.c` / `files_entry.c`, transfer integration, the Norton-style
  shortcut set, and the rename / mkdir / move sub-dialogs. The model and
  wire halves are already Rust (`hxmodel::files`, the FILE_LIST populate and
  decode, the Get Info dialog).
- **Chat content** — the render and output path in `chat.c` (`xprintline*`,
  `output_chat_from_event`, the history batch renderer, word-click handling),
  window construction, the private-chat leaf, and the wire senders. The tab
  strip, the input key handler and the chat-invitation dialog are already Rust,
  as is the output widget itself. The model side is described above.
- **Users controller glue** — the action-button handlers, the right-click user
  popover and its `GAction`s, the `user_create` / `delete` / `change` /
  `user_list` model↔view glue, the colour helpers, and the wire senders. The
  list view and row are already Rust. Deliberately deferred: this is
  controller and wire glue tied to the remaining C session structs, not a clean
  UI leaf.
- **Custom cells** — `users_cell.c`, the snapshot-rendered Name cell, stays C
  behind the `HxUserListView` FFI.
- **Tasks content** — the `gtask` row build, progress and queue-badge updates,
  the up/down queue reorder, and the transfer-progress handlers in `tasks.c`.
- **Private-message model + broadcast rendering** — `msg.c` keeps the `msgwin`
  struct and its lifecycle, the input handlers, and the message / broadcast
  render path; the tab content tree is already Rust.
- **Inline media** — the attach / upload / download paths
  (`inline_media*.c`). The view dialog is Rust, and the decode itself is the
  `hx-image-decode` crate behind a thin C shim (`inline_media_decode.c`, which
  also carries the `_Static_assert`s pinning the Rust enum discriminants).
- **Settings** — `options.c` keeps the change hooks that re-apply prefs across
  live widgets, the prefs parser, and the dialog shell: the window, the sidebar
  and the `settings_entries[]` table. Every *page* is a Rust builder called
  through that table's `.draw` pointers; retiring the indirection is what is
  left. The values themselves moved to the `hxconfig` crate — the `cfgvars[]`
  registry is gone.

### C. Shared infrastructure

Ports late; some of it may never need to.

- `gtkutil.c` — themed pixmap buttons, dialog helpers, `init_keyaccel`, the
  `.gtkhx-*` style appliers. Pervasive; each helper migrates when its last C
  caller does.
- `notify.c` (desktop notifications), `gtkurl.c` (URL click handling),
  `sound.c` (the thin shim over `hxsound`), `gtkhx_theme.c` / `gtkhx_icon.c`
  (theming singletons), `gtkhx_log.c` (the `hx_printf` → session-signal shim; not a
  transcript logger).
- The remaining model-side C: `rcv.c` (now the generic dispatch plus the
  post-LOGIN state machine), `network.c`, `commands.c` (the slash-command
  parser — never a wire-protocol file), `proto_helpers.c`, `proto_trace.c`
  (debug-only, deliberately deferred), `hxnet_bridge.c`, and the small bridge
  shims each Rust module reaches C through.
- `gtkhx.c` — `main()`, `GtkApplication` init, and the `GtkhxSession`
  signal→view adapters.
- `toolbar.c` plus the libpanel dock infrastructure (`hx_panel*.c`,
  `panel_registry.c`, `hx_split.c`, `dock_layout*.c`, `dock_bridge.c`). **The
  dock stays C by design** — gtk4-rs has no libpanel bindings, so Rust shells
  register through `dock_bridge.c` without ever naming a libpanel type; see
  [../docking.md](../docking.md). The toolbar ports with or
  after `main()`, because it owns the `PanelDock` every shell registers into.

> One wrinkle worth remembering from the shell ports: windows that treat the
> panel as their window object should point their `window` field at the content
> box (a widget inside the panel's tree once embedded) rather than the dock
> panel the shell owns. The content `"destroy"` teardown disconnects any
> session handler on the embed-failure path — but must **not** free the backing
> struct there, because `destroy` fires at the *start* of teardown and child
> callbacks may still read it.

---

## `main()` and `GtkApplication` in Rust

**Goal:** delete the last meaningful C and ship a Rust binary. `gtkhx.c`'s
`main()`, the `GtkApplication` activate handler, the signal-connect calls and
the GIOChannel-based fd watches all move into a new application crate.

Work items:

1. An app crate with `main.rs`: initialize adw, build the application id
   `com.nasledov.gtkhx`, wire the activate handler.
2. `GtkApplication` becomes `AdwApplication`. The hamburger-menu GActions
   migrate to `ActionEntry::builder()`. Style-manager (light/dark/system)
   tracking is straightforward in libadwaita-rs.
3. Resources (the gresource bundle, the AppStream metadata path) load via
   `gio::Resource::load()` or, better, the `gtk4-macros::gresource` proc-macro
   for compile-time embedding.
4. Replace Meson's `executable()` with `cargo build --release --bin gtkhx` plus
   an install rule — or keep the meson-driven C build for one more cycle and
   have a small C `main.c` call into a Rust staticlib. Pick the lower-risk
   option at the time.
5. CI adds a standalone `cargo build` step to catch crate-only breakage early.

Gotchas:

- AppStream / `.desktop` / icon installation must keep working. These are data
  files, not code; meson keeps installing them, and the post-install
  `gtk-update-icon-cache` / `update-desktop-database` hooks stay.
- The Flatpak manifest needs a Rust SDK extension —
  `org.freedesktop.Sdk.Extension.rust-stable`, enabled via `sdk-extensions` and
  `prepend-path` for `/usr/lib/sdk/rust-stable/bin`.
- Translation: `po/`'s French strings need to keep being extracted. `xgettext`
  understands Rust with some flag fiddling; validate that the strings still
  round-trip **before** starting, not after.

**Exit criteria:** `src/` contains only the small C seams the inventory above
calls permanent (the dock infrastructure, the `build_content` hooks, the bridge
shims), `gtkhx` is built by cargo, and everything that worked before still
works: launches on Wayland under GTK 4 / libadwaita, connects to mhxd / Janus /
Badmoon, chat / files / news / tracker all functional, Tier 3 green.

---

## Multi-connection & tabbed UI

**Goal:** honour the long-deferred `MAX_CONN > 1` ask, now that the codebase is
amenable to it. Tabs across the top of the main window (`AdwTabView`), one
connection per tab, independent transfer queues, shared preferences and
bookmarks.

This is the root `ROADMAP.md`'s multi-conn work, finally done — and cheap,
because the networking work made a connection a struct, the UI work made the
interface a tree of widgets, and the protocol work made the wire format
reusable across instances. Detailed design lives in
`docs/multi-connection.md`.

Work items:

1. The app owns a collection of connection tabs. Each pairs an `hxnet`
   connection actor with a UI subtree (chat, users, news, files) for that
   connection.
2. `AdwTabView` in the toolbar window holds the tabs. "Connect" opens a new
   tab; closing a tab disconnects the underlying connection.
3. The transfer window stays global — it lists every connection's in-flight
   work — with a per-row tag for which connection.
4. Bookmarks gain an "open in new tab" affordance.
5. The connection-loss banner becomes per-tab.

Gotchas:

- Each tab needs its own `GtkhxSession` instance, not the global singleton. We
  either reify the session into one-per-connection at that point, or keep the
  global and add a connection id to every signal payload. The former is
  cleaner, and that is the right moment for it.
- `AdwTabView`'s reordering and detach affordances are good, but the
  persistence story (restore tabs across launches) needs design.

**Exit criteria:** open two tabs to two different servers, chat in both at
once, transfer a file in each, see both progress, switch tabs, close one
without disrupting the other.

---

## Suggested next concrete step

The frontier is the **content behind the shells**, plus the remaining larger
standalone windows. Two pools, both leaf-up:

1. **Content ports** (inventory §B) — the main pool, in rough order of value:
   the **Files** two-panel browser (biggest by a distance, and the model half
   is already Rust so it is a view port rather than a rewrite), the **Chat**
   render/output path and window construction, the **Tasks** `gtask` list, and
   the `msg.c` private-message model. Each mirrors the user-list port: build
   the tree in gtk4-rs, keep genuinely-C leaves behind FFI.

2. **The larger standalone windows** (inventory §A) — **`tray.c`** and
   **`files_complete.c`** are both self-contained and unblocked. **`preview.c`**
   waits on the poppler / sourceview crate alignment.

Deliberately deferred, and why: the **Users controller glue** waits for the
remaining C session structs it is tied to; **`toolbar.c` and the dock infra**
port with or after `main()`, since the toolbar owns the `PanelDock` every shell
registers into and the dock itself stays C by design.

There is also a standing cleanup item worth folding into whatever touches it:
the crate boundaries that still talk over `extern "C"` where a Cargo dependency
would do — see `crate-layout.md` §3 for which ones those are and which are
irreducible.

---

## Out of scope — things we're explicitly not doing

Keeping the "if it ever happens" pile separate from the actual plan.

- **A reusable `libhotline` crate API.** We're producing one structurally, but
  we are not committing to API stability for outside consumers. See the
  motivations section above and `crate-layout.md` §5 for what would have to
  change.
- **Plugin system reincarnation.** The dlopen ABI stays dead. If we reintroduce
  scripting hooks (Lua / Wasmtime), that's a fresh design conversation, not a
  port goal.
- **Windows / macOS / iOS targets.** The current binary is Linux-Wayland, and
  full cross-platform support is not a committed phase. Some groundwork has
  landed opportunistically as the Rust rewrite makes it cheap:
  `.github/workflows/ports.yml` probes Windows (MSYS2 UCRT64) and macOS builds,
  and the leaf crates are being kept compilable off-Linux — `hx-image-decode`
  gained a pure-Rust `image` backend for the glycin-less platforms, and
  `hxnet`'s raw-fd FFI surface was removed. On the latter: the production
  connect paths already resolve and connect **inside Rust**, so no OS socket fd
  ever crossed the FFI in the running client; all that remained was test
  support — a few fd-adopting entries and some unconditional
  `std::os::unix::io` imports that broke the Windows compile — and those are
  gone (the tests inject via loopback or the real connect entry points).
  **Remaining `hxnet` follow-up:** it still depends on `glib` (via `hxbridge`'s
  shared tokio runtime and the FFI logging), so it is validated through
  `ports.yml`'s full-app build rather than the bare no-GTK portability
  tripwire; decoupling that logging and runtime seam behind injected callbacks
  would let it join the tripwire. The whole-app port (the C GIOChannel
  plumbing, the Linux-only `libseccomp` dependency) remains an open question
  for later.

---

## References

- **librsvg's incremental C → Rust precedent** — five-year port, kept shipping
  the whole time. The architectural pattern (public Rust API in a library
  crate, public C API as a thin shim) is the model. See Federico's
  [Replacing C library code with Rust (GUADEC 2017)](https://viruta.org/docs/fmq-porting-c-to-rust.pdf)
  and the
  [librsvg architecture docs](https://gnome.pages.gitlab.gnome.org/librsvg/devel-docs/architecture.html).
- **gtk-rs book chapters**:
  [Meson](https://gtk-rs.org/gtk4-rs/stable/latest/book/meson.html),
  [Main event loop](https://gtk-rs.org/gtk4-rs/stable/latest/book/main_event_loop.html),
  [Libadwaita](https://gtk-rs.org/gtk4-rs/stable/latest/book/libadwaita.html).
- **The Tokio + GLib bridge pattern**:
  [balena-io rust-async-interop](https://github.com/balena-io-experimental/rust-async-interop)
  and the
  [Rust forum thread](https://users.rust-lang.org/t/using-gtk-rs-and-tokio/100539).
- **RustCrypto coverage**: `md-5`, `sha1`, `hmac`, `blowfish`,
  `chacha20poly1305` are all in
  [RustCrypto/hashes](https://github.com/RustCrypto/hashes) /
  [RustCrypto/block-ciphers](https://github.com/RustCrypto/block-ciphers).
