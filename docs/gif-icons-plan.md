# GIF Icons Plan (fogWraith Capability Extension)

Scoping doc for GtkHx's implementation of the **GIF Icons** extension to the
Hotline protocol. Spec lives at
`fogWraith/Hotline/Docs/Protocol/GIF-Icons.md`.

This is the fourth fogWraith extension we've scoped (after voice = Phase 8,
inline media = Phase 9, and chat-history). It follows the same Rust-wire /
C-UI split those established, with one structural difference worth flagging
up front: **the spec defines no `DATA_CAPABILITIES` bit.** Support is
discovered by probing, not negotiated in the LOGIN handshake. See
[Negotiation](#negotiation-probe-and-fallback).

## Status (June 2026)

Scoped, not started. **Janus support is confirmed end-to-end** (see
[Janus verification](#janus-verification-confirmed) below) — all four
transactions round-trip against the live container, so the Tier 3 target is
ready before any code lands.

## What the extension does

The standard Hotline protocol gives each user a 16-bit icon ID
(`DATA_ICON` / 0x0068) that indexes a built-in sprite. The GIF extension
supplements that with a per-user **custom GIF avatar** that other clients
download and display.

The design is **pull-based**: the server stores GIF data per session but
never pushes the bytes. When a user sets or clears their icon, the server
broadcasts a lightweight **Icon Change (1864)** notification carrying *only*
the user's ID. Clients that care re-fetch the image on demand via **Get Icon
(1863)**. This caps bandwidth — a busy room doesn't flood everyone with
avatar blobs on every join.

The GIF avatar is **independent of** the standard 16-bit icon. Both travel
on the wire; a capable client overlays or replaces the sprite with the GIF
at the presentation layer. Icons are per-session — not persisted across
reconnects (server-dependent). Servers impose a size cap (spec suggests
32 KiB) and validate the `GIF87a` / `GIF89a` signature.

## Wire protocol

### Transactions (1861–1864 / 0x0745–0x0748)

| Dec | Hex | Name | Direction | Proposed C constant |
|---|---|---|---|---|
| 1861 | 0x0745 | Get Icon List | C→S | `HTLC_HDR_GIF_ICON_GET_LIST` |
| 1862 | 0x0746 | Set Icon | C→S | `HTLC_HDR_GIF_ICON_SET` |
| 1863 | 0x0747 | Get Icon | C→S | `HTLC_HDR_GIF_ICON_GET` |
| 1864 | 0x0748 | Icon Change | S→C | `HTLS_HDR_GIF_ICON_CHANGE` |

> **Naming collision — read this.** `HTLC_HDR_ICON_GET` already exists at
> `0x00000e90` (`src/hotline.h:604`) — that's the **legacy Mac cicn**
> icon-get, unrelated to this extension, and `HTLS_DATA_ICON_CICN` (0x0e90)
> is its data field. The new 0x0747 opcode must **not** reuse the bare
> `ICON_GET` name. Prefix every new constant with `GIF_ICON` to keep the two
> systems unambiguous in `proto_trace.c` and everywhere else.

Mirror the voice-opcode style in `hotline.h` — `((guint32)0x00000745)` with
a `/* 1861 client->server */` comment.

### Fields

| Dec | Hex | Name | Proposed C constant |
|---|---|---|---|
| 768 | 0x0300 | GIF Icon Data | `HTLC/HTLS_DATA_GIF_ICON_DATA` |
| 769 | 0x0301 | Icon List Entry | `HTLS_DATA_GIF_ICON_LIST_ENTRY` |
| 103 | 0x0067 | User ID | `HTLC/HTLS_DATA_UID` (exists) |

> **Numeric coincidence (not a real collision).** `0x0300` / `0x0301`
> already appear in `hotline.h` as `HTRK_V3_TLV_PROTOCOL_VERSION` /
> `HTRK_V3_TLV_SUPPORTS_HOPE` — but those are **tracker-v3 TLV** field IDs,
> a separate namespace from Hotline transaction `DATA_` fields. No macro
> clashes as long as the new constants keep the `HTLC_DATA_` / `HTLS_DATA_`
> prefix. Worth a comment so nobody "deduplicates" them later.

### Packed Icon List Entry (0x0301)

The Get Icon List reply carries one 0x0301 field per user that has an icon:

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 2 | u16 BE | User ID |
| 2 | 2 | u16 BE | GIF data length |
| 4 | N | bytes | Raw GIF |

The 2-byte length field caps a single entry's GIF at 64 KiB on the wire,
comfortably above the 32 KiB upload recommendation.

### Per-transaction field usage

- **Get Icon List (1861)** — request: no fields. Reply: 0..N × 0x0301.
- **Set Icon (1862)** — request: 0x0300 (raw GIF; **empty clears**). Reply:
  standard task completion (no fields). Server then broadcasts 1864.
- **Get Icon (1863)** — request: 103 (target UID). Reply: 103 + 0x0300.
- **Icon Change (1864)** — broadcast: 103 (changed UID) only. No GIF data.

## Negotiation (probe-and-fallback)

Per Misha's decision, **no capability bit.** The spec defines none, and
inventing a speculative `HTLC_CAP_GIF_ICONS = 0x0040` would be guessing at
fogWraith's future allocation. We discover support by probing.

**Critical finding from Janus testing:** an unsupported server does **not**
return a task error for an unknown opcode — Janus *silently drops* it with
**no reply at all** (verified: a bogus opcode 9999 got zero response). So
the probe **cannot** rely on `err != 0`; it must use a **timeout watchdog**,
exactly like the tracker-v3 probe already does (`hx_tracker_v3_probe_ms()` /
the v3 watchdog in `network.c`).

Proposed flow:

1. After login completes, fire **Get Icon List (1861)** once.
2. Start a short watchdog (reuse the tracker-v3 timeout pref pattern,
   ~2 s default).
3. If a reply arrives (err 0, any number of 0x0301 entries) → mark the
   session GIF-icon-capable; ingest the entries; enable the Set-Icon UI.
4. If the watchdog fires first → mark unsupported; stay silent; leave the
   Set-Icon UI disabled. A legacy server is none the wiser.

This is safe against every legacy server: the worst case is one ignored
transaction and a 2 s timer.

## Rust / C split

Matches the voice / inline-media / chat-history precedent.

### Wire → Rust (`rust/crates/hotline-proto/src/gif_icons.rs`, new module)

Typed builders/parsers for all four transactions plus the packed 0x0301
entry. Same shape as `voice.rs` / `inline_media.rs`:

- `build_get_icon_list()`, `build_set_icon(&[u8])`, `build_clear_icon()`,
  `build_get_icon(uid)` → byte buffers.
- `parse_icon_list_reply(&[u8]) -> Vec<IconListEntry { uid, gif }>` with a
  **bounds-checked** walk of repeated 0x0301 fields (length-prefixed; reject
  truncated entries — `assert!`, not `debug_assert!`, per house rule).
- `parse_get_icon_reply(&[u8]) -> (uid, Vec<u8>)`.
- `parse_icon_change(&[u8]) -> uid`.
- `validate_gif_signature(&[u8]) -> bool` — `GIF87a` / `GIF89a` only.

FFI shims in `ffi.rs`, registered the way the ~95 R2 shims are. Tier 2
wire-fixture tests in the crate pin the entry-packing layout under `-O0`
and ASan.

### Dispatch + cache → C

- **`src/rcv.c`** — handle the 1864 broadcast (→ invalidate that uid's
  cached avatar, fire a re-fetch) and the 1863/1861 replies (→ decode +
  store). New `GtkhxSession` signal, e.g. `gif-icon-changed (uid)`, to keep
  the model/view boundary clean (no `gtk_*` calls in `rcv.c`).
- **`src/commands.c`** — senders: `hx_gif_icon_get_list`,
  `hx_gif_icon_set(bytes,len)`, `hx_gif_icon_clear`, `hx_gif_icon_get(uid)`.
- **Per-user GIF cache** — keyed by uid, holding the decoded avatar (a
  `GdkPaintable`) plus the raw bytes. Natural home is the per-session user
  table; invalidate on 1864, on user-delete, and on disconnect (icons are
  session-scoped server-side, so a fresh connection re-probes from scratch).

### Decode → reuse the inline-media loader

The inline-media work already built a bounded, worker-thread image loader
around `src/preview.{c,h}` (magic-byte sniff → `GdkPixbufLoader` → size
caps). Reuse it, narrowed to GIF. **Never decode on the main thread** — a
hostile 64 KiB GIF shouldn't be able to stall the UI. Enforce the 32 KiB
recommended cap before decode and a sane pixel ceiling after.

### Animated avatars → C (with a pause control)

Per Misha's decision, render **animated** GIF avatars, with a user control
to pause them when they're disruptive.

- Decode via `GdkPixbufAnimation` / `GdkPixbufAnimationIter` (no current
  use in-tree — this is net-new). Drive frame advance off the user-list
  widget's frame clock; convert each frame to a `GdkTexture` for snapshot.
  Only animate **visible** rows — the user list is a `GtkColumnView`, so
  bind/unbind on the cell is the gate.
- **Pause control:** a pref (`Settings → Appearance` or `Chat`), e.g.
  "Animate avatar icons", default on. When off, render frame 0 as a still.
  Cheap to honour — the animation iter just isn't advanced. Consider also a
  global "reduce motion" respect via `Gtk`/portal if it's free.
- **Render integration point:** `src/users_view.c`
  `hx_user_cell_name_refresh_icon()` already resolves `row->icon` → a
  `GdkPaintable` via `load_icon` and caches it on the cell, painting it in
  `snapshot`. The GIF avatar slots in **ahead of** that resolution: if the
  user has a cached GIF avatar, use it (animated or still); else fall back
  to the existing cicn/sprite path. `HxUserRow` (`src/users_row.c`) gains an
  optional avatar handle alongside its `guint16 icon`.

### Send UX → C

- **Identity settings** (`src/options.c`, `settings_page_identity` — already
  hosts the cicn icon-picker `GtkFlowBox`): add a "Custom GIF avatar" row —
  choose a `.gif` (validate signature + size, offer to downscale if over the
  server-advertised/spec cap), preview it, and a "Clear" button. Setting or
  clearing calls `hx_gif_icon_set` / `hx_gif_icon_clear`.
- Gate the control on the session being GIF-icon-capable (greyed out with a
  tooltip otherwise), same pattern as the version/access-bit gating already
  used for News/Post buttons.

## Janus verification (confirmed)

Probed the live Janus container (`192.168.2.4:5510`, guest / empty
password) on 2026-06-28. All four transactions behave per spec:

- **Get Icon List (1861)** → `err=0`; empty list when no one has an icon,
  and after a set it returns the expected `0x0301` entry.
- **Set Icon (1862)** with a real `GIF89a` → `err=0` (accepted).
- **Get Icon (1863)** for my uid → `err=0`, reply carries field `103` + field
  `0x0300`; the GIF round-tripped byte-for-byte (signature `GIF89a`).
- **Icon Change (1864)** → with two clients connected, client A's Set Icon
  triggered a `type=1864` broadcast to client B carrying only field `103`
  with A's uid. Exactly the pull-based design.
- **Control:** a bogus opcode (9999) got **no reply at all** — confirming
  the probe must be timeout-based, not error-code-based.

So Janus is a ready Tier 3 target; no Go mock server is needed (unlike the
chat-history early phases, which predated a public implementation).

## Testing

- **Tier 2 (`hotline-proto`)** — wire fixtures for the four builders, the
  0x0301 entry walker (including truncated/oversized rejection), and the GIF
  signature validator.
- **Tier 3 (Janus)** — add `HX_TEST_CAP_GIF_ICONS`-style coverage to the
  Janus matrix row (`tests/integration/server_matrix.c`). End-to-end
  binaries: set→get round-trip, set→list, the 1864 broadcast to a second
  client (the two-client pattern already exists —
  `test_two_client_chat.c`), clear-icon, and the probe-fallback timeout
  against a non-supporting server (mhxd) to prove the watchdog path.
- **Unit (Tier 1)** — the per-uid cache invalidation logic and the
  animated/still paintable selection given the pause pref.

## Sub-phases

- **10.A — Wire foundation.** `hotline-proto::gif_icons` module + FFI shims,
  the four opcodes + two fields in `hotline.h` (carefully named), C
  dispatch in `rcv.c`/`commands.c`, the `GtkhxSession` signal. Tier 2
  fixtures. No UI. Probe-and-fallback (timeout watchdog) wired but headless.
- **10.B — Receive + cache + still render.** Per-uid GIF cache, bounded
  decode via the preview loader, 1864→re-fetch, and **static** avatar
  render in the user list (frame 0). End of 10.B the client displays other
  users' avatars. Tier 3 set/get/list/broadcast against Janus.
- **10.C — Send UX.** Identity-settings GIF picker (choose / preview /
  clear / downscale), capability-gated. Round-trips your own avatar.
- **10.D — Animation + pause.** Animated rendering off the frame clock for
  visible rows, plus the "Animate avatar icons" pref (still fallback when
  off). Honour reduce-motion if free.
- **10.E — Docs.** Update this doc + ROADMAP; README/man-page mention in the
  next release-notes pass.

## Open questions / risks

- **Animation cost.** A roomful of animated avatars is N frame-clocked
  textures. Animating only bound (visible) `GtkColumnView` cells should keep
  it bounded, but worth profiling on a busy server; the pause pref is the
  escape hatch.
- **Where does the avatar show besides the user list?** Chat message
  prefixes? Private-message windows? v1 scope is the user list only;
  broader surfaces are a follow-up.
- **Idle timer.** Spec notes Get Icon (1863) should not reset the server's
  idle timer (clients may poll). Server-side concern, but our re-fetch
  cadence should stay event-driven (on 1864) rather than polling, which
  sidesteps it entirely.
- **Re-fetch storms.** 1864 carries no content hash, so a strict client
  re-fetches on every change. Fine at avatar sizes; if it ever bites, a
  short per-uid debounce is the mitigation.
