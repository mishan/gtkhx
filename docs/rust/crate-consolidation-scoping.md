# Rust crate consolidation — scoping

Review of `rust/crates/` as of July 2026: **41 crates**, ~104k LOC of Rust.
Two questions drive this doc:

1. Is the crate count excessive, and what should merge?
2. What has to change so that other projects can depend on some of these crates?

Short answers: **yes, ~41 → ~18**, and the crate count is a *symptom* of a link
architecture choice, not of over-eager domain modelling. And the reuse blockers
are almost entirely licensing and packaging, not structure.

---

## 1. The root cause: one staticlib per FFI seam

**39 of 41 crates declare `crate-type = ["staticlib", ...]`.** Each one becomes
its own `.a`, and `src/meson.build` puts 37 of them on the link line in a
hand-maintained order. The comments there tell the story:

```
# gtkhx-ui (Phase R5 windows) must precede gtkhx-boxed on the link line:
# it has undefined refs to hx_tracker_v3_meta_copy/_free which the later
# gtkhx-boxed archive resolves.
...
# Same receive-handler group + consumer-before-provider rule: undefined refs
# to gtkhx_session_get_default / _emit_msg and hx_member_model_get_ignore.
```

Three consequences follow, and they are the actual cost of the current shape:

- **Rust crates talk to each other over `extern "C"` instead of Cargo.**
  `hxmsg-recv` declares `fn hx_member_model_get_ignore(...)` in an extern block
  rather than `use hxmember_model::...`. No type checking, no inlining, no
  lifetime information — the compiler treats a sibling Rust crate exactly like
  a C object file.
- **Link order is load-bearing and undocumented-by-default.** Every new crate
  needs a paragraph of comment explaining where it sits and why. That's ~40
  lines of link-order commentary in `src/meson.build` today.
- **Merging crates is *blocked* by the current design.** R4.2 documents this
  directly (`docs/rust/ROADMAP.md` §R4.2, "Why a separate `gtkhx-boxed`
  crate"): putting the boxed types in `gtkhx-session` failed at link time,
  because a staticlib bundles its rlib dependencies and release-mode
  codegen-unit merging drags dangling externs into the same object. So the
  workaround was *another crate*. Every future merge hits the same wall.

### The fix, and it is the prerequisite for everything else

Introduce **one** façade crate — `gtkhx-ffi` — with
`crate-type = ["staticlib"]`. Every other crate becomes `rlib`-only.
`gtkhx-ffi` depends on all of them and its `lib.rs` is a set of
`pub use` re-exports (plus `#[used]` anchors if any `#[no_mangle]` symbol gets
dead-stripped; `-C link-dead-code` or an explicit re-export handles that).

What this buys:

- The C link line goes from 37 archives with a hand-tuned order to **one**.
- Rust↔Rust edges become real Cargo dependencies. `hxmsg-recv` can `use
  hxmember_model::MemberModel` and get a compile error instead of a link error.
- The duplicate-symbol hazard disappears: one staticlib means one definition
  of every `#[no_mangle]` symbol, by construction.
- Crates become free to merge or split on *design* grounds rather than
  link-graph grounds.

This is a mechanical change — no logic moves — and it should land before any of
the merges below.

---

## 2. What to merge

Sizes are `wc -l` over `src/**/*.rs`.

### 2a. The `*-recv` / `*-send` family → `hxhandlers` (10 → 1, 6,330 LOC)

| crate | LOC | deps |
|---|---|---|
| `hxagreement-recv` | 133 | — |
| `hxmsg-recv` | 159 | — |
| `hxicon-recv` | 215 | — |
| `hxnews-recv` | 453 | hotline-proto |
| `hxfiles-recv` | 488 | hotline-proto |
| `hxchat-recv` | 946 | hotline-proto |
| `hxxfer-recv` | 984 | hotline-proto |
| `hxuser-recv` | 1,318 | hotline-proto |
| `hxchat-send` | 683 | hotline-proto, glib |
| `hxnews-send` | 951 | hotline-proto, glib |

These are the same crate wearing ten hats. Every one is thin glue with an
identical shape: parse via `hotline-proto`, apply a gate, emit a
`GtkhxSession` signal, return a discriminant to the C caller. They share
dependencies, they share the extern block (`gtkhx_session_get_default` appears
in eight of them), and they all sit in the same spot in the link order for the
same reason. Three of them are under 250 lines.

One crate, `src/{agreement,msg,icon,news,files,chat,xfer,user}.rs` plus
`send/{chat,news}.rs`. The module boundaries survive; the packaging overhead
doesn't. If you want the recv/send split preserved, `hxrecv` (8→1) and
`hxsend` (2→1) is a defensible stopping point — but the two halves have the
same deps and the same consumers, so I'd merge.

### 2b. Crypto + compression → `hxcrypto` (4 → 1, 2,228 LOC)

`hxcrypto-hash` (251), `hxcrypto-stream` (456), `hxcrypto-aead` (798),
`hxcompress` (723).

Two findings here:

- **They have no C callers left.** `src/cipher.c`, `src/cipher_aead.c`,
  `src/compress.c`, and `src/hmac.c` are all deleted. The only surviving
  reference is `protocol.h`'s inline wrapper around `gtkhx_hmac_xxx`. Yet all
  four are still `staticlib` and still on the link line — that is dead link
  surface.
- **`hxnet` doesn't use `hxcompress`.** It re-declares `flate2`, `lz4_flex`,
  and `zstd` as its own direct dependencies and does the compression inline.
  So the compression code exists twice in the workspace's dependency graph,
  with `hxcompress` compiled and linked for nobody.

Merge the three crypto crates into `hxcrypto` with `hash` / `stream` / `aead` /
`compress` modules, make it `rlib`-only, and have `hxnet` depend on it —
dropping `hxnet`'s duplicate `flate2`/`lz4_flex`/`zstd` lines. Keeping it as
one crate rather than folding it into `hxnet` outright is worth it: crypto that
can be audited and Tier-1 tested in isolation is a real property, and the wire
format pinning tests live there.

### 2c. Model crates → `hxmodel` (5 → 1, 4,087 LOC)

`hxchat-model` (760), `hxmember-model` (535), `hxnews-model` (1,342),
`hxfiles-model` (862), `hxfiles-entry` (588).

All consumed by `gtkhx-ui` (and by the recv handlers, over FFI, which the
façade change fixes). `hxchat-model` + `hxmember-model` are already a pure
model and its `gio::ListModel` wrapper — that's one concept in two crates.
Same for `hxfiles-model` + `hxfiles-entry`. Merge to `hxmodel` with
`{chat,member,news,files}` modules; keep the pure/glib split as a `glib`
Cargo feature if the headless-test property matters (it does for `chat`).

### 2d. GObject infrastructure → `gtkhx-core` (4 → 1, 3,684 LOC)

`gtkhx-session` (1,417), `gtkhx-boxed` (1,198), `hxconn` (617), `hxtask` (452).

`gtkhx-boxed` exists *solely* because of the staticlib duplicate-symbol
problem (§R4.2 spells this out). Once `gtkhx-ffi` is the only staticlib, the
reason evaporates and the `#[cfg(test)]` boxed-stub scaffolding in
`gtkhx-session` can go with it — replaced by a plain Rust dependency. `hxconn`
and `hxtask` are per-session state owners in the same layer.

### 2e. Voice → one `hxvoice` with features (4 → 1, 12,719 LOC)

`hxvoice` (3,437), `hxvoice-runtime` (8,162), `hxvoice-model` (629),
`hxvoice-send` (491).

Voice is already gated as a unit by `-Dvoice` — `rust/meson.build` excludes
`hxvoice` and `hxvoice-runtime` together, and `src/meson.build` conditionally
appends the model and send deps. That's a build-gated *subsystem*, which is
exactly what Cargo features are for:

```toml
[features]
default = []           # pure state machine + wire model, no GStreamer
runtime = ["dep:gstreamer", ...]
```

`cargo test -p hxvoice` still runs in a GStreamer-free container (the property
the current split protects) because the runtime module is behind the feature.
`--exclude hxvoice hxvoice-runtime` becomes `--no-default-features`.

### 2f. Mac formats → `macresource` (2 → 1, 916 LOC)

`hxmacres` (423) + `hxcicn` (493). A resource-fork parser and a decoder for one
resource type found in that fork. `cicn` is a `macresource` concern. Both use
glib only inside their `ffi.rs` (a single `g_malloc` call each) — see §3.

### 2g. Small fry

- `hxfiles-xfer` (586) — only consumer is `hxnet`. Fold in as a module, or
  leave it; low stakes either way.
- `feature-unify` (6 lines) — keep. It's a CI compile-time hack, well
  documented, and costs nothing.

### Net

| | before | after |
|---|---|---|
| crates | 41 | ~18 |
| staticlib archives on the link line | 37 | 1 |
| lines of link-order commentary in `src/meson.build` | ~40 | 0 |

Unchanged: `hotline-proto`, `hxnet`, `gtkhx-ui`, `hxbridge`, `hxhfs`,
`hxbookmarks`, `hxsound`, `hxtext`, `hxtls-trust`, `hx-image-decode`,
`feature-unify`.

---

## 3. Making crates reusable by other projects

This reverses a locked-in decision. `docs/rust/ROADMAP.md` lines 38–42
currently read:

> Notably **not** a motivation: shipping a reusable `libhotline` crate for
> other clients. […] we will not optimize for external consumers and will not
> freeze APIs for them.

That paragraph needs rewriting if external reuse is now a goal, otherwise the
next person to read the roadmap will make choices that undo this work.

### Which crates are actually reusable

Only crates with no `extern "C"` edge back into GtkHx's C are candidates.
That rules out every `*-recv` / `*-send` crate, `gtkhx-*`, `hxconn`, `hxtask`,
`hxnet` (it externs into `hxbridge`'s C-owned runtime). What's left:

| crate | LOC | license ceiling | who else would want it |
|---|---|---|---|
| **`hotline-proto`** | 29,466 | GPL-2.0-or-later (fixed) | Any Hotline client or server in Rust. A typed 1.0/1.2/1.5/1.9 wire parser and builder is not something anyone else has. |
| **`hxhfs`** → `appledouble` | 1,767 | GPL-2.0-or-later (fixed) | Genuinely generic: CAP / AppleDouble / Netatalk sidecar metadata. Would have been the widest-appeal crate here — but see §3 provenance. |
| **`macresource`** (2f) | 916 | GPL-2.0-or-later (fixed) | Mac resource fork parsing + `cicn` decode. Same audience, same constraint. |
| **`hx-image-decode`** → `glycin-compat` | 2,155 | **relicensable** | The dual-backend glycin shim (crate 3.x and 2.x loaders, auto-detected). Every GNOME-Rust app straddling Debian stable and a modern Flatpak runtime has this exact problem. Written fresh in June 2026 — no hxd ancestor. |
| **`hxtls-trust`** | 1,079 | **relicensable** | A TOFU known-hosts store with SHA-256 cert pinning. Modest but generic. From `tls_trust.c`, written May 2026 — no hxd ancestor. |

`hxbookmarks`, `hxsound`, `hxtls-trust`, `hxvoice` are technically standalone
but too GtkHx-shaped (or too thin a wrapper over `rodio` / `rustls`) to be
worth the maintenance of a public API.

### The blockers, in order of how much they matter

1. **License.** Every crate is `GPL-2.0-or-later`. The Rust ecosystem norm is
   `MIT OR Apache-2.0`, and a GPL-2 *library* crate sees little adoption — it
   forecloses every non-GPL consumer.

   **Provenance audit (July 2026) — the constraint is tighter than it looks.**
   GtkHx was forked from hx/hxd, the tree mhxd later forked from, and the
   inheritance is pervasive: **48 source basenames** in the original CVS import
   have hxd counterparts, including the whole protocol core (`rcv.c`,
   `commands.c`, `cipher.c`, `compress.c`, `hotline.h`, `hx.h`) and — checked
   line by line — `hfs.c`, `macres.c`, and `cicn.c`:

   | GtkHx file | vs. hxd | evidence |
   |---|---|---|
   | `src/cicn.c` | `mhxd/src/ghx/cicn.c` | 380 identical non-trivial lines |
   | `src/macres.c` | `mhxd/src/hx/macres.c` | entire `macres_*` API identical (16 shared symbols) |
   | `src/hfs.c` | `mhxd/src/common/hfs.c` | 151 identical lines; `hfsinfo_read`, `funkdat`, `resource_open`, … |

   These three carry a sole `Copyright (C) 2000-2002 Misha Nasledov` header in
   the GtkHx tree, but that header is **inaccurate** — they are derivative
   works of hxd. So `hotline-proto`, `hxhfs`, and `macresource` all stay
   `GPL-2.0-or-later`. They can still be *published* to crates.io under the
   GPL; the realistic audience for a Hotline protocol crate descends from the
   same tree and is already GPL, so the cost is lower than it would be for a
   general-purpose crate. (`appledouble` is the real loss here — it was the one
   with genuine appeal outside this world.)

   **What is relicensable: the revival-era work with no C ancestor.** Verified
   by first-appearance commit, all well after the 2026-04-27 CVS import:

   - `hx-image-decode` ← `inline_media_decode.c`, added 2026-06-15
   - `hxtls-trust` ← `tls_trust.c`, added 2026-05-28
   - `hxvoice` / `hxvoice-runtime` ← `voice.c`, added 2026-06-10

   None have any counterpart in the mhxd tree (which predates TLS, voice, and
   glycin entirely). These are Misha's to dual-license `MIT OR Apache-2.0`.

   **Caveat on method.** Shared basenames and line-overlap counts are strong
   evidence, not a legal audit; and absence of a same-named file doesn't prove
   nothing was copied under a different name. Before publishing anything under
   a non-GPL license, the specific crate wants a deliberate file-by-file
   read-through and a sign-off from Misha. The safe default for anything
   touching the CVS import is: **stays GPL**.

   GtkHx itself is unaffected either way: GPL-2 can consume MIT/Apache
   dependencies, so relicensing the leaves doesn't touch the binary's license.

2. **`publish = false` on all 41 crates**, all pinned at `version = "0.1.0"`.
   Nothing is publishable today, and the versions carry no information.

3. **FFI leaking into the public API.** `hotline-proto` has 131 `#[no_mangle]`
   functions, all in `ffi.rs`. An external Rust consumer wants none of them —
   and worse, they'd collide if two crates in a dependency tree both exported
   `gtkhx_proto_*`. Gate it:

   ```toml
   [features]
   default = []
   capi = []   # GtkHx's own build turns this on
   ```

   The internal module structure is already right for this — `ffi.rs` is
   cleanly separated from `wire.rs` / `parse.rs` / `messages.rs`. Same move
   for `macresource` and `hxhfs`, which would then have **zero** dependencies
   (their only glib use is one `g_malloc` inside `ffi.rs`).

4. **Naming.** `hx*` means nothing on crates.io and reads as squatting.
   Reusable crates should be named for what they do (`appledouble`,
   `macresource`, `glycin-compat`) or namespaced honestly
   (`hotline-proto` — already correct). Internal crates can keep `hx*`; the
   prefix then usefully signals "not for you."

5. **API stability.** Publishing means semver, a changelog, and not breaking
   `pub` signatures casually. That's the real ongoing cost, and it's the thing
   the roadmap paragraph was protecting against. A reasonable compromise:
   publish at `0.x`, which signals "breaking changes in minor releases" while
   still being `cargo add`-able.

6. **Docs.** `#![warn(missing_docs)]` and a README per published crate. Most of
   these already have good module-level docs; it's the `pub` items that need
   the pass.

---

## Suggested sequencing

Each step is independently shippable and green.

1. **`gtkhx-ffi` façade.** Every crate → `rlib`; one staticlib; delete the
   link-order commentary. No logic changes. *Prerequisite for 2–5.*
2. **Convert `extern "C"` sibling edges to Cargo dependencies** where both
   sides are Rust. ✅ **Mostly done** — 133 → 29 declarations (261 genuine C
   declarations correctly untouched), 101 path-dep edges in the workspace.

   It paid for itself immediately. rustc caught **five signature drifts** the
   linker could not, all of them ABI-identical on x86-64 and therefore
   invisible until now — `*const u8` vs `*const c_char`, and four cases of a
   bare `*mut c_void` standing in for `*const HtlcConn` / `*mut GObject` /
   `*mut HxConversation` / `*mut HxChatEvent`. It also eliminated **three
   hand-synced `#[repr(C)]` struct mirrors**, two of which had already drifted
   (`DateTime` vs `HxNewsDate`; `Decoded`'s `sniffed_format` typed `c_int`
   where the real field is `u32`).

   **The 29 that remain, and why** — this is the interesting part, because
   each category is a real constraint rather than leftover work:

   | count | what | why it stays |
   |---|---|---|
   | 15 | `gtkhx-ui` → `hxvoice-{model,runtime,send}` | Only compile under `--features voice`, so the default workspace build doesn't typecheck them. Their callbacks need `Option<fn>` wrapping — a careful pass, not a mechanical one. Genuine remaining work. |
   | 8 | `gtkhx-ui` ↔ `hxchat-send` ↔ `hxuser-recv` | **A dependency cycle.** Cargo forbids it; `extern "C"` is precisely what lets these three be mutually recursive today. |
   | 4 | `task_new` (`hxtask`) | Deliberate type erasure: `rcv_task_*` handlers have heterogeneous arg lists cast to a canonical 3-arg shape. Importing it would force a `transmute` at every caller. |
   | 2 | `hx_tracker_v3_meta_{copy,free}` | `gtkhx-boxed` models the type as an opaque 216-byte buffer; `gtkhx-ui` keeps a typed mirror (R5.1). Two intentional views of the same memory. |

   **The cycle matters for step 3 — but not the way this doc first claimed.**
   The original note said folding the send/recv crates into `hxhandlers` (§2a)
   would dissolve it. **That is wrong**, and checking the graph before moving
   any code is what caught it: `hxchat-recv` and `hxmsg-recv` depend on
   `gtkhx-ui`, while `gtkhx-ui` depends on `hxnews-recv` and `hxnews-send`.
   Merging all ten therefore replaces a 3-node cycle with a 2-node one,
   `gtkhx-ui ↔ hxhandlers`, which Cargo rejects just as firmly.

   The real blocker is that **model code is living in the UI crate**. What the
   recv crates actually want from `gtkhx-ui` is
   `gtkhx-ui::{conversation, chat_members}` — `HxConversation`, the member
   model, `hx_chat_set_subject`. That is model, not view; it ended up in
   `gtkhx-ui` because the chat-model re-think (ROADMAP M4b.5) landed there.

   So `hxhandlers` has a prerequisite: **move `conversation.rs` and
   `chat_members.rs` out of `gtkhx-ui` into `hxmodel`** (which now exists and
   is exactly where they belong — next to `chat` and `member`, which they
   already use). After that both the 3-node cycle and the merge conflict
   disappear, because nothing in the handler layer needs the UI crate any
   more. That relocation is worth doing on its own merits regardless of the
   merge.
3. **Merge the internal families** — §2a–2g, one PR each. Verify each merge is
   cycle-free *before* moving code; the check is cheap and it is what caught
   the `hxhandlers` problem above. Status:

   - ✅ **`hxcicn` → `hxmacres`** (§2f). Kept the `hxmacres` name rather than
     renaming to `macresource` — the provenance audit made it internal, and
     §3's own naming rule says internal crates keep the `hx*` prefix.
   - ✅ **`hxmodel`** (§2c). Five crates → one; 106 tests carried over.
   - ⛔ **`hxhandlers`** (§2a) — blocked on moving `conversation.rs` +
     `chat_members.rs` from `gtkhx-ui` into `hxmodel` (see step 2 above).
   - ⏳ **`hxcrypto`** (§2b), **`gtkhx-core`** (§2d), **`hxvoice`** (§2e).
     All three verified cycle-free. Each contains crates from the
     test-linked 16, so each also needs its `-l<name>` archive references in
     `tests/meson.build` updated — the only reason they weren't done first.

   Crate count so far: **41 → 35**.
4. **Relicense** `hx-image-decode` and `hxtls-trust` to `MIT OR Apache-2.0`
   after a file-by-file read-through (§3). `hotline-proto`, `hxhfs`, and
   `macresource` stay GPL-2.0-or-later — hxd-derived. Blocks 5.
5. **Publish prep** — `capi` feature gating, rename, `missing_docs`, README,
   drop `publish = false`.
6. **Update `docs/rust/ROADMAP.md`** lines 38–42 to reflect that a reusable
   protocol crate is now an explicit goal, and record the crate-layout policy
   (one staticlib façade; crates split on design, not link graph) alongside the
   other locked-in decisions.
