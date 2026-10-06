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

**Shared protocol code is now an explicit goal.** The leaf-up extraction yielded
`hxproto`, `hxfiles-xfer` and `hxhfs`, and GtkHx and hxd-ng consume the same
revision of each from [hx-libs](https://github.com/mishan/hx-libs). What moves
there next is in [Shared code with hxd-ng](#shared-code-with-hxd-ng). The
dependency remains `publish = false` at `0.1.0`: git revisions, coordinated
changes, and both
projects' CI are the compatibility contract while the public API is still
evolving. Publishing and a semver commitment remain separate decisions.

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
more than the gain. Some have since been amended or superseded by events and
are marked as such rather than deleted, because the reasoning is still worth
knowing.

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
   unchanged. The workspace uses the current gtk-rs-core family (glib/gio
   0.22, gtk4 0.11, libadwaita 0.9) and moves up with it; the Rust floor is
   that family's MSRV (see `rust-toolchain.toml`).

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
   surface is now Rotulus, which lives in
   [its own repository](https://github.com/mishan/rotulus) and which GtkHx
   takes from crates.io: `rotulus-layout` (a dependency-free layout engine — spans,
   wrapping, a chunked prefix-sum height index, scroll anchoring, selection,
   search) plus `rotulus` (the GTK4 widget). The crate's `rotulus.h` is a
   declaration header; there is no `chat_view.c`. See the widget's
   [design doc](https://github.com/mishan/rotulus/blob/main/docs/design.md)
   for how it works,
   [its xtext benchmark](https://github.com/mishan/rotulus/blob/main/docs/xtext-benchmark.md)
   for the measurements that overturned this decision, and
   [chat-view.md](../chat-view.md) for how GtkHx consumes it.

7. **`gtk_hlist_compat` dies on the way through, not separately.** *(Done.)*
   Its consumers were each rewritten directly against `GtkColumnView` /
   `gio::ListStore`, and the shim disappeared with its last consumer. Every
   Rust list widget is a `GtkColumnView` / `GtkListView` / `GtkListBox` from
   the start.

8. ~~**Single-connection during the port.**~~ **Superseded.** The plan was for
   `MAX_CONN > 1` and the tabbed UI to wait until the port was far enough along
   that a multi-conn refactor landed against mostly-Rust code, not
   half-Rust-half-C state machines. Multi-connection shipped in 1.3.0 anyway,
   with the C still in place; see
   [multi-connection](#multi-connection--tabbed-ui) below for what that leaves
   the port to clean up.

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
| Crypto + transport compression | hx-libs' `hxcrypto` (hash / stream / aead) and `hxhope` (the HOPE transport's ciphers and compression) | `hmac.c`, `cipher.c`, `cipher_aead.c`, `compress.c`, `md5.c`, `sha.c`, `haval.c` |
| Wire protocol — parsers, builders, framing, Mac Roman text, dates | `hxproto` | the byte-twiddling half of `rcv.c` / `proto_helpers.c` / the `hlwrite` send path |
| Connection lifecycle: connect, magic, LOGIN, HOPE, ciphers, compression, TLS | `hxnet` + `hxbridge` (tokio runtime + GLib ferry), over hx-libs' `hxsession` and `hxhope` | `network.c`'s connect/decode state machine, `hope.c`, `network_decode.c`, `connect_magic.c` |
| HTXF file transfers — subchannel transport, the recv/send/folder byte loops, `htxf_conn` storage and lifecycle, the worker shell | `hxnet::{htxf,xfer,xfer_handle}` + `hxhandlers::xfer` | `xfers.c`, `xfers_recv.c`, `xfers_send.c`, `htxf_io.c`, `htxf_subchannel.c`, `gtkthreads.c` |
| HFS sidecar / resource-fork I/O; FFO+FILP fork-header codec | `hxhfs`, `hxfiles-xfer` | `hfs.c` and the fiddly byte math in `xfers.c` |
| Tracker fetch (HTRK v1 + v3, TLS, probe-fallback) | `hxnet::tracker` | `network.c`'s `GSocketClient` tracker state machine |
| Banner fetch (URL mode over `ureq`, file mode over HTXF) | `hxnet` + `gtkhx-ui::banner` | `banner.c`, `banner_dispatch.c`, the `libsoup` dependency |
| `GtkhxSession` GObject + its boxed signal payloads + `htlc_conn` accessors | `gtkhx-core` | `gtkhx_session.c`, the boxed types in `proto_helpers.c` / `tracker_event.c`, `hxconn.c` |
| Receive- and send-side protocol handlers | `hxhandlers::{recv,send}` | the per-opcode handler bodies in `rcv.c` and the scattered `hlwrite` call sites |
| Task registry + the send primitive | `hxtask` | `tasks_table.c`, the variadic `hlwrite` |
| Client-side models: chat / membership / conversation registry, news, files | `hxmodel` | `struct chat` + `gchats`, the news GUI structs, `filelist_walker.c` |
| Chat output surface | `rotulus` + `rotulus-layout`, from crates.io | vendored `xtext.c` |
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
that drive C-side state and gain nothing from a tokio `Interval` — the
post-login SELFINFO fallback, the fetch drains, UI debounce. The keepalive
is the session's, timed by `hxnet`'s actor.

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
  The AEAD state was the first: a Rust `size_of` assert paired with a
  `_Static_assert` on the same size in the C header, so a field reorder on
  either side tripped a build error rather than a misalignment at decrypt time.
  Both halves went once no cipher state crossed to C. Every cross-language
  struct since follows the same pattern — C `_Static_assert` against Rust
  `size_of` / `align_of` / `offset_of` consts. `tasks_bridge.c` and
  `inline_media_decode.c` are current examples, the latter pinning enum
  discriminants as well as layout.
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
  `hxproto`'s `text` module and matches glibc's `iconv` `MACINTOSH` table
  byte for byte. (Not to be confused with `hl_code.c`, the unrelated XOR-0xff
  obfuscation of LOGIN/PASSWORD chunks.)
- **The HOPE rekey marker is wire-format-critical.** A random nibble in the
  type field's high byte triggers N rounds of HMAC-stretching the cipher key.
  It was ported byte for byte rather than refactored on the way, and the
  frame-aware Rust adapter mirrors the original's read-side parse and write-
  side probability exactly. Don't tidy it.
- **Frame the read stream by `DataSize`, not `TotalSize`.** Getting this wrong
  desynced against a fragmenting server and surfaced as "unknown header type".
  The frame reader is now `hxsession`'s, which also joins a fragmented
  transaction rather than passing on its first frame and dropping the rest.
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
- **One typed mirror, not two views.** The tracker v3 metadata once lived as
  an opaque, offset-patched buffer in `gtkhx-core` beside a typed mirror in
  `gtkhx-ui`, because C built it and each crate needed a different slice of
  it. Once its constructor moved to Rust, the typed mirror moved into
  `gtkhx-core` and became the only one; copy and free read named fields.

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
  `GtkWindow` with no dock involvement.
- **System tray** (`tray.c`).

### B. Content still C inside a Rust window shell

Each of these is a *content* port of the same shape as the user-list, private
message and private chat ports: build the widget tree in gtk4-rs and keep
genuinely-C leaves behind FFI. This is the big remaining category.

- **Files browser** — the model, the wire and now the view are Rust
  (`hxmodel::files`, the FILE_LIST populate and decode, the senders in
  `hxhandlers::send::files`, `xfer_new`, the Get Info dialog, and
  `gtkhx-ui::files`); what remains is the providers under the view. It ports
  in the order below.
  Each step deletes C and none grows it:

  1. **Wire senders** — done. `files.c` is gone: its senders are
     `hxhandlers::send::files`, and the recursive-listing engine, the `dir_char`
     global and the icon / kind / basename C wrappers had no callers left.
  2. **Path-completion popover** — done. It is `gtkhx-ui::files::complete`.
  3. **The view** — done. Both panels, the shared chrome, the row menu, the
     dialogs, drag and drop and the shortcut set are `gtkhx-ui::files`
     (`browser`, `panel`, `dialogs`, `dnd`, `row`), driving the C providers
     through their `hx_files_provider_*` ABI from `files::provider`.
     `files_browser.c`, `files_panel.c` and `files_entry.c` are gone, and so
     are the `gtkhx_files_build_content` seam and the benchmark's panel FFI.
  4. **Providers and operations** — `files_provider*.c`, `files_ops.c`. With no
     C consumer left the provider stops being a GObject interface and becomes
     a Rust enum over the two sides. The FILE_LIST reply routes straight to
     the Rust remote provider, which deletes `on_file_list_signal`, the `cfl`
     provider carrier and `hx_remote_files_provider_handle_file_list`; the
     `safe_local_basename` unit test moves to `cargo test`.
     Rows must also keep each name's raw wire bytes and send those back: today
     a row holds only the decoded name, so a name whose Mac Roman bytes happen
     to be valid UTF-8 (`√©` is `C3 A9`) shows as `é`, goes back as `0x8E`,
     and the server reports it missing. The sends that take a name are in the
     providers and `files_ops.c`, so this goes with them.
  5. **The seam** — drop the `HxFileEntry`, `hx_cfl_*` and sender exports that
     have no C caller left, connect Get Info to the session signal from Rust
     (deleting `on_file_info_signal`), and measure the seam before and after.

  End-to-end coverage for each step goes in `rust/crates/hx-e2e`, which sends
  the `hxrequest` builders' requests to the rig's servers (see
  [tests/COMPOSE.md](../../tests/COMPOSE.md)).

  The view went before the providers because the providers already had a
  small, stable C ABI to lend it in between; the other order would have needed
  a Rust-implemented GInterface just to keep the C panel working.

- **Chat content** — the render and output path in `chat.c` (`xprintline*`,
  `output_chat_from_event`, the history batch renderer, the load-more and
  inline-media click handlers),
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
- **Settings** — the whole dialog is Rust: the window, the sidebar, the page
  table and every page (`gtkhx-ui/options_window.rs`, `options.rs`). The
  values live in the `hxconfig` crate; `options.c` is down to the change hooks
  that re-apply prefs across live widgets, the prefs parser and the by-name
  bridge the Rust rows read and write through. Both the `cfgvars[]` registry
  and the per-page FFI exports are gone.

### C. Shared infrastructure

Ports late; some of it may never need to.

- `gtkutil.c` — themed pixmap buttons, dialog helpers, `init_keyaccel`, the
  `.gtkhx-*` style appliers. Pervasive; each helper migrates when its last C
  caller does.
- `notify.c` (desktop notifications), `gtkurl.c` (URL click handling),
  `sound.c` (the thin shim over `hxsound`), `gtkhx_log.c` (the `hx_printf` →
  session-signal shim; not a transcript logger).
- `gtkhx_theme.c` / `gtkhx_icon.c` — the theming singletons. These grew
  substantially with whole-app theming. The theme file parser and the palette
  model need no GTK and can split out into a crate that tests without a
  display; only the code that applies a theme to live widgets has to stay near
  GTK.
- `tracker_event.c` — the `HxTrackerServer` signal payload's constructors
  (address formatting, the v1 Mac Roman transcode). The wire codec under it
  is `hxproto::tracker`, shared with hxd-ng.
- The remaining model-side C: `rcv.c` (now the generic dispatch plus the
  post-LOGIN state machine), `network.c`, `commands.c` (the slash-command
  parser — never a wire-protocol file), `proto_helpers.c`, `proto_trace.c`
  (debug-only, deliberately deferred), `hxnet_bridge.c`, and the small bridge
  shims each Rust module reaches C through.
- `gtkhx.c` — `main()`, `GtkApplication` init, and the `GtkhxSession`
  signal→view adapters.
- `toolbar.c` plus the dock glue (`dock_bridge.c`, `dock_pages.c`,
  `panel_registry.c`, `dock_layout*.c`). The dock itself is mullion-gtk, a C
  library; Rust shells register through `dock_bridge.c` without ever naming a
  dock type; see [../docking.md](../docking.md). The toolbar ports with or
  after `main()`, because it makes the dock every shell registers into.

> One wrinkle worth remembering from the shell ports: windows that treat the
> panel as their window object should point their `window` field at the content
> box (a widget inside the panel's tree once embedded) rather than the dock
> panel the shell owns. The content `"destroy"` teardown disconnects any
> session handler on the embed-failure path — but must **not** free the backing
> struct there, because `destroy` fires at the *start* of teardown and child
> callbacks may still read it.

---

## How the rest of the port gets done

### Where it stands

Over July the port removed about a third of the C in `src/`. Then it stopped.
Since early August no C file has been deleted, and feature work has been adding
C back: video, whole-app theming, the Files browser redesign and the slimmer
window chrome. Most of it went into the files at the top of the port list:
`hx_panel.c`, `files_browser.c`, `gtkhx_theme.c`, `toolbar.c`, `dock_layout.c`.

That is not a lapse in discipline. It is what happens by default: a feature
lands in whichever language the code it touches is written in, and the code a
feature touches is exactly the code that is still C. Left alone, every feature
makes the remaining port bigger. The rules below exist to reverse that.

### The rules

1. **C does not grow.** `tools/check-c-growth.sh` runs on every pull request
   to `main`
   and fails if the branch adds net lines of C to `src/`. When a feature needs
   substantial changes to C content, port that content first, so the feature
   lands in Rust. When growth is genuinely the right call — a bridge shim that
   lets a larger port land, say — a `C-Growth: <reason>` trailer in the commit
   message records it where the reviewer reads, and the check passes.

2. **Port a feature end to end, not a file at a time.** Leaf-up drained the
   leaves; what is left is the trunk, and porting trunk code one file at a time
   leaves a bridge behind at every step. A feature port takes the feature's
   view, its controller glue, its receive handlers in `rcv.c` and its signal
   adapters in `gtkhx.c` together, and ends by deleting the bridges it made
   redundant. The [permanent seams](#permanent-seams-not-todos) still hold —
   each stays until the layer behind it ports. A feature port is how that
   layer ports.

3. **Measure the seam, not just the C.** The second number to watch is the
   FFI surface between the languages, in both directions — the functions
   Rust exports to C and the C functions Rust declares and calls:

   ```sh
   # exported to C
   grep -r '#\[no_mangle\]' rust/crates --include=*.rs | wc -l
   # imported from C: functions inside `extern "C" { … }` blocks
   awk '/extern "C" \{/{b=1;next} b&&/^[[:space:]]*\}/{b=0}
        b&&/^[[:space:]]*(pub )?(unsafe )?fn /{n++} END{print n}' \
       $(git ls-files 'rust/crates/*.rs')
   ```

   A feature port should bring it down. A port that moves code to Rust but
   adds exports for the C left behind has moved the problem, not solved it.

4. **Route by connection while porting.** Each reader that ports takes its
   connection explicitly instead of asking `hx_active_session()` or
   `gtkhx_active_htlc()` which one has focus, and each `thread_local`
   singleton holding per-connection state becomes an id-keyed map, the shape
   `useredit.rs` already has. That is the
   [multi-connection follow-through](#multi-connection--tabbed-ui), and doing
   it during the port means touching the code once.

5. **Break the circular waits.** Three items are each waiting on another: the
   Users controller waits for the C session structs; the toolbar waits for
   `main()`; `main()` is scheduled last. One way out, **not yet decided**:
   move `main()` into Rust early and have it call the existing C startup and
   toolbar construction as a library. The signal adapters in `gtkhx.c` then
   dissolve one at a time as their handlers port, instead of all at the end,
   and the toolbar stops blocking on the app crate. The cost is paying the
   build-system change (Cargo producing the binary) early instead of late.
   See the [`main()` section](#main-and-gtkapplication-in-rust).

---

## `main()` and `GtkApplication` in Rust

**Goal:** delete the last meaningful C and ship a Rust binary. `gtkhx.c`'s
`main()`, the `GtkApplication` activate handler and the signal-connect calls
move into a new application crate. (The GIOChannel socket watches this used to
list are already gone; the one left is the Unix-only `/exec` output pipe.)

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
   option at the time. Doing this step *first*, with the Rust `main()`
   calling the remaining C as a library, is the option rule 5 of
   [How the rest of the port gets done](#how-the-rest-of-the-port-gets-done)
   describes.
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

**Shipped in 1.3.0**, ahead of the port rather than after it (see locked-in
decision 8). Several connections run at once, one tab each, under the
tab-switched layout. The transport, the session signals and the
connection-scoped keys all carry the connection. The design and the open
question of coexisting per-connection panels are in
[`../multi-connection.md`](../multi-connection.md).

What it leaves for the port is the code that still asks "which connection has
focus?" when it means "which connection is this for?": readers that route
through `hx_active_session()` or `gtkhx_active_htlc()`, and the
`thread_local` singletons in `gtkhx-ui` that hold per-connection state. The
root [`ROADMAP.md`](../../ROADMAP.md) lists them. Rule 4 of
[How the rest of the port gets done](#how-the-rest-of-the-port-gets-done) is
the plan: fix each one as the code around it ports.

---

## Suggested next concrete step

In order:

1. **Port Files end to end**, in the order laid out under *Content still C
   inside a Rust window shell*. The largest content port by a distance, and
   the one feature work most recently added C to.
2. **Split the theme model out of `gtkhx_theme.c`** into a GTK-free crate while
   the whole-app theming work is fresh.
3. **The rest of inventory §B**, one feature at a time: Chat's render and
   output path and window construction, the Tasks list, `msg.c`, and the
   Users controller once the Chat and Users slices have taken the session
   structs it depends on with them.
4. **The standalone windows**: `tray.c` is unblocked; `preview.c` waits on the
   poppler / sourceview crate alignment.
5. **Decide on `main()`** — last, as planned, or early, as rule 5 describes.

There is also a standing cleanup item worth folding into whatever touches it:
the crate boundaries that still talk over `extern "C"` where a Cargo dependency
would do — see `crate-layout.md` §3 for which ones those are and which are
irreducible.

---

## The protocol core: `hxsession`

hx-libs' `hxsession` is the classic client's protocol logic with no I/O of its
own: bytes and the time in, bytes and typed events out. It was written for the
browser client (hx-ng reaches classic servers through it, compiled to wasm),
and it is GtkHx's behavior — the login without a nickname, the agreement and
its two-second fallback, the keep-alive, transaction ids — tested against the
same rig. GtkHx moving onto it means one implementation of the classic session
instead of two, and C retired as it goes: much of what it replaces is in
`rcv.c`, `tasks.c` and `network.c`.

What stays in `hxnet` is everything below the byte stream: the socket, TLS,
SOCKS, and the tokio runtime that drives them. `hxsession` sits on top of
that stream as the frame reader does today.

The order, each step its own branch and each checked against the rig:

1. **The frame reader.** `hxnet` cuts the stream with
   `hxsession::frame::FrameReader`, which brought the fragmented-transaction
   join GtkHx never had. *Done.*
2. **The handshake, login and agreement.** The session, in raw mode, drives
   the connection from the magic on (`hxnet`'s `session.rs`): the login,
   the agreement and the wait for it, a 1.2 server's user change, and when
   the post-login fetches may go out (`LoginReady`). C reads the login
   reply's fields, shows the agreement, and keeps everything after.
   *Done.*
3. **Transaction ids and the keep-alive.** The session numbers every
   transaction from one counter, and a GtkHx task keeps its view-side
   state, keyed by the trans the session gives it
   (`hxnet_connection_take_trans`). The keep-alive is the session's, in
   place of the ping timer in `network.c`. *Done.* Matching a reply to
   its task is still `hx_rcv_task` and `hxtask`'s table; it moves with
   the replies, in step 5.
4. **HOPE and compression.** `hxcrypto` moved to hx-libs, and HOPE with it
   as `hxhope`: the handshake as either side plays it, its keys, and the
   transport it agrees — Blowfish with its rekey marker, ChaCha20-Poly1305,
   and GZIP, LZ4 and ZSTD beneath either — as a codec between the session's
   `feed` / `take_outgoing` and the socket. `Session::with_hope` runs both
   steps on the session's own counter, so the login is step 2, on trans 2,
   and C keys its login task on the trans the session reports. `hxnet` only
   moves bytes: the HOPE lifecycle is connect plus session, and the
   transfer keys an HTXF subchannel derives from come from what the session
   agreed. Compression is negotiated when the user picks it, and the suite
   runs GZIP against mhxd and ZSTD and LZ4 against Janus; see *Shared code
   with hxd-ng* for what hxd-ng needs to accept HOPE. *Done.*
5. **The receive handlers, domain by domain.** Chat, users, messages, news,
   files: each moves from `rcv.c` and `hxhandlers` onto session events. Until
   a domain moves, its frames reach GtkHx whole, as `Event::Unhandled` and
   `Session::request` already allow. A domain's replies move with it, and
   the task table's correlation (`hx_rcv_task`, `hxtask`) goes once the
   last of them has.

   Chat, users, messages, news and files have moved. The session handles
   them (`Config::handled`, `Handled::CHAT`, `Handled::USERS`,
   `Handled::MSG`, `Handled::NEWS`, `Handled::FILES`), and `hxnet` hands
   what it makes of a chat line and the picture it carries, an invitation,
   a subject, a page of history, a user arriving, changing or leaving,
   what the server says about us, a private message, a broadcast, the
   server's parting words, a flat news post and a queued transfer moving
   up to `hx_recv_session_event` on the main thread, among the frames and
   in their order.
   `hxhandlers::recv::chat`, `::user`, `::msg`, `::news` and `::files` keep
   the model — the ignore list, each chat's subject, the history cursor,
   the rosters, our own uid and access bits, the user, news and file
   requests in flight — and emit the signals they always did, a broadcast
   and the parting words as `broadcast`; the chat and message events
   themselves are built in `gtkhx-core`. A history request, an invitation,
   the user list, a user's info, a kick, creating or joining a private
   chat, a private message, a broadcast, the user editor's account read,
   creation, save and delete, and every news request — flat news's file and
   posts, threaded news's listings, articles, posts, deletions and new
   bundles and categories — and every files request — a listing, Get Info,
   a folder made, something deleted, moved or renamed, a comment set, and a
   download or upload of a file or a folder — have their reply expected by
   the session (`Session::expect`), so none is a task any more, and a
   refusal comes back as `Failed`. A private chat is made when its join is
   answered; a user's info reaches the user it was asked of, an account
   the editor that asked, a news reply the browser node that asked, a
   listing the files pane and a transfer's reply the transfer, by its
   trans. The files browser names what a listing gave back, and the user
   editor an account, by the bytes the server sent. What arrives is traced
   from the session's tap. *In progress.*
   What remains:
   - the banner's download (`banner.rs`, `rcv_task_banner_get`), still a
     task;
   - of messages, the picture a private message carries, which the
     session reads and the view does not yet show;
   - inline media's upload and download;
   - the C history tests, which move to `hx-e2e`, retiring the
     `hx_history_entry_parse` they read history through: the proto and
     integration `test_chat_history.c` and the HOPE chat-history
     integration tests.
6. **Transfers.** The HTXF state machines — single files, folders, resume,
   upload — rewritten around bytes in and bytes out. The largest step, last.

Of the extensions GtkHx negotiates, text encoding, chat history, the
picture a chat line or a private message carries, the color a user's
nickname arrives with and Large Files' exact sizes have their
session-side handling; the rest (voice and video signaling, inline
media's upload and download, GIF icons) need theirs before the domains
that use them move. hxproto has the codecs.

---

## Shared code with hxd-ng

[hxd-ng](https://github.com/mishan/hxd-ng) is a Hotline server written in
Rust. It consumes `hxproto`, `hxfiles-xfer` and `hxhfs` from hx-libs at the
same revision GtkHx does, and GtkHx's Tier 3 rig runs it as one of the test
servers. A change to a shared crate needs both projects' suites green before
either pin moves.

The rule from [`crate-layout.md`](crate-layout.md) §5 still holds: code moves
to hx-libs when a real second consumer needs it, not before.

**Done: the tracker protocol.** `hxproto::tracker` covers every role —
the registration hxd-ng sends, the acknowledgment it reads, the listing
GtkHx fetches — with one typed `TrackerMeta` for the v3 fields both sides
speak. GtkHx's C tracker codec is gone; hxd-ng's hand-built registrations
go with its matching change, which also updates its roadmap to name hx-libs
as the home for shared crates.

The candidates that have a second consumer, most valuable first:

**Done: HOPE.** `hxcrypto` and `hxhope` are in hx-libs; GtkHx runs HOPE
through `hxsession`. hxd-ng still refuses HOPE logins; accepting them takes
no HOPE code of its own, only the I/O around `hxhope::server`:

- when a LOGIN is `server::is_step1`, answer it with `server::answer` and
  a 64-byte session key it makes random, and remember the `Server`;
- read the next LOGIN with `Server::step2`, find the account whose login it
  names (`Step2::names`, asked of each account, the guest's as the empty
  login), and `Server::accept` it with that account's password — which
  means keeping the password, or something it derives from, where
  `MAC(password, session key)` can be computed;
- from then on run both directions through the `Transport` it returns,
  the login reply included: `encode` before each write, `decode` after each
  read. With ChaCha20-Poly1305, `Negotiated::transfer_keys` derives each
  HTXF transfer's keys.

Its rig image would then let GtkHx's suites run HOPE, and compression,
against a server whose code is in hand.

The candidate that remains:

1. **Smaller pieces for `hxproto`**: the access-bit table, and the
   text-encoding helpers that GtkHx's `hxtext` and hxd-ng's `TextEncoding`
   each implement.

Beyond code:

- **Extension specs in one place.** The extension documents are spread across
  both projects' `docs/`, and only hxd-ng's video capability document is
  written as a spec. A `specs/` directory in hx-libs, holding normative
  versions of the extensions both projects implement, gives each a single
  reference to argue with.
- **A shared conformance corpus.** Wire fixtures from GtkHx's Tier 2 tests and
  hxd-ng's integration suites, kept in hx-libs and run by both CIs. hxd-ng's
  roadmap asks for the same thing.
- **Cheaper pin bumps.** Three pins move by hand: hx-libs in each project,
  hxd-ng in GtkHx's rig, and hx-ng in hxd-ng's end-to-end suite. Tagging
  hx-libs revisions and having hx-libs CI build both consumers would take
  most of the "run everything in both apps" cost out of a bump.
- **More of GtkHx's Tier 3 against hxd-ng.** Only the video tests use it
  today. It now implements chat history, inline media, GIF icons, TLS and
  tracker registration, and running voice against it as well as Janus would
  separate server-side SFU defects from client ones.

---

## Out of scope — things we're explicitly not doing

Keeping the "if it ever happens" pile separate from the actual plan.

- **A published, semver-stable Hotline crate API.** hx-libs is shared with
  hxd-ng, but as git revisions of `publish = false` crates; nothing commits to
  stability for outside consumers. See `crate-layout.md` §5 for what would
  have to change.
- **Plugin system reincarnation.** The dlopen ABI stays dead. If we reintroduce
  scripting hooks (Lua / Wasmtime), that's a fresh design conversation, not a
  port goal.
- **Mobile targets.** Windows and macOS are no longer on this list: both are
  built and packaged by the release and snapshot workflows
  (`build-packages.yml`), with the Linux-only
  pieces — glycin's sandbox and its `libseccomp` dependency, the `/exec`
  command — compiled out. iOS and Android remain out of scope. hx-ng, the
  browser client for hxd-ng's Hotline-ng wire, already covers phones.

  One portability follow-up stays open: `hxnet` still depends on `glib`,
  through `hxbridge`'s shared tokio runtime and the `g_critical!` calls on its
  FFI error paths. Decoupling that behind injected callbacks would let it
  build with no GTK stack at all, which is also what a server-side consumer of
  its HOPE or HTXF code would need.

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
