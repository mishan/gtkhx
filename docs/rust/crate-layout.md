# Rust crate layout — how the workspace is arranged, and why

Reference for `rust/Cargo.toml` and `rust/crates/`. It answers two questions
that come up whenever someone adds or moves Rust code:

1. Why is there exactly one static library, and what does that buy?
2. Why do the surviving crate boundaries exist — which ones are load-bearing
   and which are just history?

A third section records the provenance / licensing audit, because it decides
what could ever be published outside this tree.

---

## 1. One archive: `gtkhx-ffi`

**The crate count used to be a symptom of a link-architecture choice, not of
domain modelling.** Nearly every crate declared
`crate-type = ["staticlib", …]`, so each produced its own `.a` and
`src/meson.build` listed all of them on the C link line **in a hand-maintained
order**. Static archives resolve left to right, and a given archive is only
searched for symbols that are undefined at the point the linker reaches it, so
every crate had to sit ahead of the crates providing its externs. Each entry
carried a comment justifying its position.

Three consequences, and they were the real cost:

- **Rust crates talked to each other over `extern "C"` instead of Cargo** — no
  type checking, no inlining, no lifetime information. The compiler treated a
  sibling Rust crate exactly like a C object file.
- **Link order was load-bearing.** Adding a crate meant working out where it
  belonged and writing the paragraph explaining why.
- **Merging two crates was blocked outright.** A `staticlib` bundles its rlib
  dependencies, so two archives could each define the same `#[no_mangle]`
  symbol and collide at the final link. That is precisely why the GObject
  boxed-payload types had to be split into their own crate: folding them into
  the session crate failed at link. Every subsequent merge would have hit the
  same wall.

### The fix

`rust/crates/gtkhx-ffi` is the workspace's **only** `staticlib`. Every other
member is an `rlib`. `gtkhx-ffi` has no code of its own — its `Cargo.toml`
names every FFI-exporting crate as a dependency and its `lib.rs` is a list of
`use … as _;` bindings. rustc bundles the whole rlib graph, C ABI symbols
included, into one `libgtkhx_ffi.a`.

The `use … as _;` lines are load-bearing: a dependency declared in `Cargo.toml`
but never referenced can be dropped from the crate graph, taking its
`#[no_mangle]` symbols with it. The anonymous import is the standard way to say
"link this crate, I don't name anything from it". If one is ever dropped by
accident the symptom is an undefined symbol at the meson link — a symbol-set
assertion over the built archive would catch it earlier, and `lib.rs` describes
one, but there is no such test in the tree today.

What this bought: the C link line is **one** archive (`rust_gtkhx_ffi_dep` in
`rust/meson.build`, consumed as `rust_deps` in `src/meson.build`), with no
order to get wrong and no commentary to maintain; Rust↔Rust edges can be real
Cargo dependencies, so drift is a compile error instead of a link error; the
duplicate-symbol hazard is gone by construction; and crates are free to merge
or split on **design** grounds.

**Adding a crate:** add it to `gtkhx-ffi`'s `[dependencies]` and add a `use`
line in its `lib.rs`. Nothing in `src/meson.build` changes.

**The one exception.** A handful of crates additionally keep a standalone
`staticlib` alongside the rlib, because `tests/meson.build` links them
*directly* to keep the Tier 1/2 test binaries GTK-free — they cannot link the
façade without pulling in GTK, GStreamer, and the C symbols only the
application binary defines. No single link line ever sees both copies.

---

## 2. The three constraints that keep the surviving boundaries

Consolidation removed a large number of crates without a single behaviour
change. The boundaries that remain are not arbitrary; three distinct
constraints hold them, and none is visible from the crate list alone.

### 2a. Genuine Cargo dependency cycles

`gtkhx-ui` depends on `hxhandlers` (Cargo). The handler layer also needs to
reach *back* into the UI — reply handlers call window entry points to render
what they parsed. Cargo forbids the cycle, and `extern "C"` is exactly what
lets the two be mutually recursive today.

The fix for a cycle is to **move the misplaced code, not to merge the crates**.
When the chat-model rework left model types (`conversation`, `chat_members`) in
the UI crate, the handler crates ended up externing into `gtkhx-ui` for things
that were model, not view. Relocating them to `hxmodel` dissolved that part of
the cycle. Merging the crates instead would have replaced a three-node cycle
with a two-node one, which Cargo rejects just as firmly.

**Check the graph before moving any code.** It is cheap and it is what caught
the merge that would not have worked.

### 2b. Test-linked crates must stay free of external symbol references

This one is not solved by the façade, and it is the constraint that a purely
link-graph reading of the workspace misses.

The GObject boxed payloads had **two** independent reasons to live in their own
crate. The façade solved the first (duplicate symbols at the binary link). The
second survives: the crate is deliberately "glib only, zero undefined externs",
so a Tier 2 proto test that pulls one boxed `_copy` links that archive *alone*
— and those tests link the standalone staticlib, not the façade.

Merging an extern-ful crate into a test-linked one therefore breaks the tests.
The per-session task owner (`hxtask`) carries a set of undefined C externs, and
folding it into the extern-free GObject core meant proto tests that wanted only
a boxed `_copy` dragged in `task_new`'s unresolved references and failed to
link. That is why `gtkhx-core` absorbed the session, boxed, and connection
crates but **not** `hxtask`.

**The rule.** A crate in the test-linked set can only be merged with another
crate whose undefined-extern set the linking tests already satisfy. In
practice: *merge extern-free crates with extern-free crates.*

Check before proposing any further merge:

```sh
# undefined C externs a crate would contribute (approximate — the -A window
# can spill past the end of the extern block; use it to spot "zero vs. some",
# not for a precise number)
grep -A99 'extern "C" {' rust/crates/<c>/src/*.rs | grep -cP '^\s*(pub )?fn '

# is it linked directly by the Tier 1/2 tests?
grep -c "rust_<c>_dep" tests/meson.build
```

This is also why the crypto/compression merge worked: all its members are leaf
primitives with no C externs, so the merged archive is still self-contained.

### 2c. Deliberate dependency floors

`hxvoice` — the pure voice-chat state machine — was a mechanically possible,
cycle-free merge candidate and was **deliberately not merged**. It is
`no_std`-friendly with *zero non-Rust dependencies* (one pure-Rust hashmap
crate), and its own module docs list that as the first of three load-bearing
reasons: `cargo test -p hxvoice` runs in any container on any architecture,
regardless of GStreamer install state or audio-device availability, so CI
catches every state-machine regression before the runtime layer even compiles.

Folding in `hxvoice-runtime` adds GStreamer and `hxvoice-model` adds glib —
both unconditionally, both C libraries. Feature-gating them back out leaves a
conditionally-`no_std` crate whose default build is a strict subset of itself,
which is worse than the boundary it replaces. **Leave it.**

---

## 3. The remaining cross-crate `extern "C"` edges

Most `extern` blocks in the workspace declare genuine C symbols, which is
correct and permanent. What follows is the taxonomy of the edges where *both*
sides are Rust — each category is a real constraint, not leftover work.

| what | why it stays |
|---|---|
| `gtkhx-ui` → the voice crates (`hxvoice-model` / `-runtime` / `-send`) | Already *optional* Cargo dependencies, but the calls still go through `extern` blocks. Converting them means code that only typechecks under `--features voice`, and their callback parameters need `Option<fn>` wrapping — a careful pass, not a mechanical one. **Genuine remaining work**, not a constraint. |
| `hxhandlers` → `gtkhx-ui` | The dependency cycle of §2a. `gtkhx-ui` depends on `hxhandlers` through Cargo, so the reverse edge cannot be one. |
| `task_new` (`hxtask`) | Deliberate type erasure. The `rcv_task_*` reply handlers have heterogeneous argument lists cast to a canonical shape; each caller declares its own local `RcvTaskFn` alias. Importing the real signature would force a `transmute` at every call site. |
| `hx_tracker_v3_meta_{copy,free}` | Two intentional views of the same memory. `gtkhx-core` models the tracker-v3 metadata as an opaque, correctly-sized buffer (its copy/free only need the owned-string byte offsets); `gtkhx-ui` carries a full typed `#[repr(C)]` mirror because the tracker window reads the fields. Both are pinned by const asserts against the C `_Static_assert`s. |

### What converting the extern edges to Cargo dependencies surfaced

Turning linker-resolved externs into real dependencies paid for itself
immediately. rustc caught **signature drifts the linker could not** — all of
them ABI-identical on x86-64 and therefore invisible until then: `*const u8`
standing in for `*const c_char`, and several cases of a bare `*mut c_void`
standing in for a concrete pointer type (`*const HtlcConn`, `*mut GObject`,
`*mut HxConversation`, `*mut HxChatEvent`). It also eliminated several
hand-synced `#[repr(C)]` struct mirrors, **two of which had already drifted** —
a date struct that no longer matched its counterpart, and a decoded-image
struct whose `sniffed_format` field was typed `c_int` where the real field is
`u32`.

That is the general argument for preferring a Cargo edge whenever the graph
allows one: the linker checks names, the compiler checks types.

---

## 4. Provenance and licensing

Every crate is currently `GPL-2.0-or-later`, `publish = false`, and pinned at
`0.1.0`. Whether that can change is a provenance question, not a preference.

**GtkHx was forked from hx/hxd — the tree mhxd later forked from — and the
inheritance is pervasive.** Dozens of source basenames in the original CVS
import have hxd counterparts, including the whole protocol core (`rcv.c`,
`commands.c`, `cipher.c`, `compress.c`, `hotline.h`, `hx.h`) and — checked line
by line — `hfs.c`, `macres.c`, and `cicn.c`:

| GtkHx file | vs. hxd | evidence |
|---|---|---|
| `src/cicn.c` | `mhxd/src/ghx/cicn.c` | large blocks of identical non-trivial lines |
| `src/macres.c` | `mhxd/src/hx/macres.c` | entire `macres_*` API identical, symbol for symbol |
| `src/hfs.c` | `mhxd/src/common/hfs.c` | substantial identical body; `hfsinfo_read`, `funkdat`, `resource_open`, … |

Those three carry a sole `Copyright (C) 2000-2002 Misha Nasledov` header in the
GtkHx tree, but that header is **inaccurate** — they are derivative works of
hxd.

**Stays GPL-2.0-or-later (hxd-derived):** `hotline-proto` (the typed
1.0/1.2/1.5/1.9 wire parser and builder — the crate with the widest genuine
appeal to other Hotline clients and servers), `hxhfs` (CAP / AppleDouble /
Netatalk sidecar metadata; genuinely generic, and the real loss here — it is
the one with appeal well outside this world), and `hxmacres` (Mac resource-fork
parsing plus `cicn` decode).

They can still be *published* under the GPL. The realistic audience for a
Hotline protocol crate descends from the same tree and is already GPL, so the
cost is lower than it would be for a general-purpose crate.

**Relicensable — revival-era work with no C ancestor**, verified by
first-appearance commit, all well after the CVS import, and with no counterpart
in the mhxd tree (which predates TLS, voice, and glycin entirely):

- `hx-image-decode` (from `inline_media_decode.c`) — the dual-backend glycin
  shim that auto-detects the host's loader generation. Every GNOME-Rust app
  straddling Debian stable and a modern Flatpak runtime has this exact problem.
- `hxtls-trust` (from `tls_trust.c`) — a TOFU known-hosts store with SHA-256
  certificate pinning. Modest but generic.
- `hxvoice` / `hxvoice-runtime` (from `voice.c`).

**Caveat on method.** Shared basenames and line-overlap counts are strong
evidence, **not a legal opinion**; and the absence of a same-named file does
not prove nothing was copied under a different name. Before publishing anything
under a non-GPL license, that specific crate wants a deliberate file-by-file
read-through and a sign-off from Misha. The safe default for anything touching
the CVS import is: **stays GPL**.

GtkHx itself is unaffected either way — GPL-2 can consume MIT/Apache
dependencies, so relicensing the leaves does not touch the binary's license.

Relicensable is not the same as worth publishing. `hxvoice` is clean of hxd
ancestry but implements one server extension's state machine, and
`hxbookmarks` / `hxsound` are either too GtkHx-shaped or too thin a wrapper
over an existing crate to justify a public API and its maintenance.

---

## 5. Open work

- **Relicense** `hx-image-decode` and `hxtls-trust` to `MIT OR Apache-2.0`
  after the file-by-file read-through above. Blocks the packaging work.
- **Package for external reuse.** Gate the C ABI behind a Cargo feature so a
  pure-Rust consumer does not pay for it:

  ```toml
  [features]
  default = []
  capi = []   # GtkHx's own build turns this on
  ```

  An external Rust consumer wants none of the `#[no_mangle]` surface, and
  worse, two crates in one dependency tree both exporting `gtkhx_proto_*` would
  collide. The module structure already supports the gate — `ffi.rs` is cleanly
  separated in every candidate crate, and `hxmacres` / `hxhfs` would then have
  essentially no dependencies at all (their only glib use is inside `ffi.rs`).

  The rest of the publish prep: rename the reusable crates for what they do
  rather than the internal `hx*` prefix (`hotline-proto` is already right;
  something like `appledouble` / `macresource` / `glycin-compat` for the
  others), turn on `#![warn(missing_docs)]`, write a README per published
  crate, and drop `publish = false`. Publish at `0.x` — it signals "breaking
  changes in minor releases" while still being `cargo add`-able, which is the
  compromise that makes the semver commitment bearable.

- **Reconcile with `docs/rust/ROADMAP.md`.** The roadmap's motivations section
  states that a reusable protocol crate is a *side effect* rather than a goal,
  and that we will not freeze APIs for outside consumers. That is still the
  accurate description of today. If the publishing work above is taken up, that
  paragraph is the thing to change first — otherwise the next person to read
  the roadmap will make choices that undo it.
