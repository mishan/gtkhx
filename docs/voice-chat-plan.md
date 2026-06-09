# Voice Chat Extension — Scoping Notes for GtkHx

Status: scoping draft, no code yet. Written 2026-06-09 against the
spec at
<https://github.com/fogWraith/Hotline/blob/525e94e2208dc2ffb1ed65d69d681c7fd356e169/Docs/Protocol/Capabilities-Voice.md>
(commit `525e94e`, 2026-04-05 — initial publication of the voice
spec). Re-pin to a newer SHA if the upstream doc moves before
implementation starts.

**Revised 2026-06-09**: §3, §4, §5 (new Phase 8.0 inserted), §7, §8,
§9, §11, §12 switched from the original "C bindings + hybrid Rust
state machine" pick to `gstreamer-rs` end-to-end, on the back of a
new R3.0 — glib-rs foothold prerequisite (~1 week). The trigger was
re-scoping the R3 prerequisite after R2 closeout: it turned out the
voice controller doesn't need tokio, doesn't need a full GtkhxSession
rewrite, and doesn't need the rest of R3 — it needs glib-rs in the
workspace and one wrapping pattern for the C GtkhxSession. See §3 for
the revised reasoning and what changed.

The fogWraith spec adds real-time voice to Hotline via a server-side
SFU (Selective Forwarding Unit). Clients negotiate the capability in
LOGIN, then run a WebRTC session against the server — DTLS + SRTP +
ICE, G.711 μ-law (PCMU) only, one UDP port (base + 4). The server
forwards RTP packets between participants in the same chat room; it
never decodes or mixes audio.

This doc supersedes the existing one-line "voice" hint in the
capability comment block in `src/hotline.h` (bit 2 is already reserved
as `HTLC_CAP_VOICE` but never advertised). Track this as **Phase 8**
in `ROADMAP.md`, after Phase 7 TLS — the natural slot for "another
fogWraith capability we picked up."

GtkHx is a client; the SFU lives on the server. Sections of the spec
that talk about server SFU lifecycle, room cleanup, RTP forwarding,
mute enforcement on the server side, etc. are out of scope here
except as the contract our client codes against. No new server is
shipped in this plan.

---

## 1. What the spec actually requires of the client

Stripping it down to the client's contract:

- **Capability negotiation.** Set bit 2 (`0x0004`) in
  `HTLC_DATA_CAPABILITIES` (0x01F0) during LOGIN. If the server doesn't
  echo it, voice UI is hidden. Already wired in `hotline.h`. Two
  emission sites need the new bit flipped on: the legacy/plaintext
  LOGIN in `send_login` (network.c:1231) and the HOPE Step-2 LOGIN
  in `rcv_task_login` (rcv.c:1396). Both hard-code the
  `HTLC_CAP_LARGE_FILES | HTLC_CAP_TEXT_ENCODING |
  HTLC_CAP_CHAT_HISTORY` mask today; both need `| HTLC_CAP_VOICE`.
  Missing the HOPE branch would silently exclude every HOPE server
  (Janus included) from voice negotiation.
- **Seven new transaction opcodes** in the 600–606 range (above the
  101–355 base + the 700 chat-history slot we already use).
- **Five new data field IDs** in the 0x01F5–0x01F9 range. Already
  collides with our `HTLS_DATA_FILESIZE64` (0x01F1) and friends — but
  not with each other: the voice IDs start at 0x01F5, so they fit
  between the Large-File extension (ends at 0x01F4) and the chat-
  history block at 0x0F01+. Fine.
- **A WebRTC peer connection** to the server on UDP base+4: SDP
  offer/answer over the existing TCP control channel, ICE candidates
  trickled both ways, DTLS handshake, SRTP-encrypted RTP carrying
  PCMU audio. The server is *always* the offerer.
- **Track-to-user mapping** via SDP `a=mid:user-{UID}` labels.
  Clients parse the mid prefix to associate an incoming audio track
  with a Hotline user ID for the UI.
- **One room at a time.** Joining voice in B implicitly leaves A.
  Text chat across multiple rooms is unaffected.
- **Mute** as a server-side enforced flag (606). Push-to-talk is just
  rapid mute/unmute from the client's side — no separate opcode.
- **Disconnect = automatic leave.** Server cleans up if the TCP
  control connection drops, so we don't have to send Leave (601) on
  shutdown — but it's polite to.
- **PCMU only.** Mandatory-to-implement in every WebRTC stack;
  no codec negotiation to worry about.

What we don't have to invent: encryption (DTLS/SRTP is handled by the
WebRTC stack), NAT traversal (host candidate to the known server
address is the only candidate that matters), codec selection (just
PCMU), or audio mixing (SFU forwards; we just decode N−1 streams in
parallel). The WebRTC stack we pick does all of this.

---

## 2. Current state of GtkHx

We have the capability scaffolding for "fogWraith extensions
negotiated in LOGIN" but nothing voice-shaped past the bit definition:

- `src/hotline.h:328` — `HTLC_CAP_VOICE 0x0004`, defined, never
  asserted by `send_login`.
- `src/network.c:1231` — `req.caps = HTLC_CAP_LARGE_FILES |
  HTLC_CAP_TEXT_ENCODING | HTLC_CAP_CHAT_HISTORY;` — voice excluded
  intentionally for now.
- `src/rcv.c` — `HTLS_DATA_CAPABILITIES` echo is parsed into
  `htlc->caps`; downstream code branches on `htlc->caps & HTLC_CAP_*`
  for chat-history (chat.c:1165, chat_history.c:144). Voice can
  follow the same convention.
- `src/tracker_v3_meta.c:205` already decodes the v3 tracker's
  `supports_voice` TLV (0x0305). The tracker details popover surfaces
  it as a "Voice" boolean row. **Useful for UI:** in the Connect
  dialog and the tracker browser we can already tell the user "this
  server supports voice" before they connect.
- `hl_access.h` already decodes the full 8-byte (bits 0–63) access
  bitmap via `hl_access_has(access, bit)` — the legacy bits 0–31
  plus the higher-range additions like `HL_ACCESS_READ_CHAT_HISTORY`
  (bit 56) used by the chat-history extension. Janus's voice-chat
  bit 55 is in the same range and is queryable today via
  `hl_access_has(access, 55)`; what's missing is a named
  `HL_ACCESS_VOICE_CHAT` constant (Phase 8.A adds one) and any
  schema beyond 64 bits. The 128-bit extended bitmap (gated on
  `HTLC_CAP_EXTENDED_PRIV`, which we don't advertise yet) is the
  follow-up scope. For Phase 8 we have everything we need to
  grey out the Voice button when bit 55 is unset.

**Audio I/O:** `src/sound.c` is *output only*, via GSound (a thin
GLib wrapper over libcanberra) — fire-and-forget event sound effects
for chat alerts. **There is no audio capture path anywhere in the
codebase.** This is the biggest single new piece of infrastructure
voice needs.

**Threading:** `gtkthreads.c` (recursive mutex + custom main-context
poll wrapper). Workers marshal to main via `g_idle_add`. WebRTC
glue threads from whatever stack we pick will need the same
discipline.

---

## 3. The WebRTC stack decision — `gstreamer-rs`

This is the single biggest decision in the plan. The spec is written
assuming you can call an off-the-shelf WebRTC library and get the
SDP/ICE/DTLS/SRTP/RTP behaviour for free; the question is *which*.

GStreamer is the right base regardless of bindings — audio capture
+ playback + μ-law encode/decode + RTP packetization + the WebRTC
peer connection in one dep, with GLib idioms throughout, GNOME
runtime already ships it, active upstream, LGPL. Every alternative
(`libdatachannel`, `webrtc-rs`, `libwebrtc`, `Pion`) requires us to
write three layers of glue for audio I/O, μ-law, and RTP that
GStreamer hands us. The decision below is purely *which language
drives `webrtcbin`*.

| Option | Pros | Cons |
|---|---|---|
| **`gstreamer-rs` + `gstreamer-webrtc-rs`** *(picked, revised 2026-06-09)* | Voice controller is one language end-to-end (state machine *and* dispatch in Rust). Typed `WebRTCSessionDescription` / `WebRTCSDPType` / `WebRTCPeerConnectionState` / `WebRTCSignalingState` / `WebRTCICEGatheringState` enums instead of stringly-typed `g_object_get`. `connect_pad_added` is a typed signal method instead of `g_signal_connect` with a `void*` payload + manual lifetime tracking. `gst::Promise::new_with_change_func(|reply| …)` for `create-offer` / `create-answer` is the established pattern (Centricular's `gstwebrtc-demos` is the reference, since superseded by `gst-examples/webrtc` in the main GStreamer tree). MIT/Apache-2.0. Bindings are at 0.25.2 as of 2026-05, paired with `glib` 0.22 / `gtk-rs-core` 0.22 / `gtk4-rs` 0.11+ — consistent with RUST-ROADMAP's "gtk4-rs is at 0.11+ as of May 2026" note (gtk4-rs 0.11 ships in the gtk-rs-core 0.22 family; RUST-ROADMAP doesn't pin a strict family version, but the 0.22 line is the natural reading). Prerequisite is the new **Phase 8.0 — R3.0 glib-rs foothold** (~1 week, §5), not full R3. | Workspace gains the `glib` / `gio` / `gtk4` / `libadwaita` / `gstreamer*` Rust crate trees before R3 proper. We commit to gtk-rs idioms inside the voice controller before R3/R4 has shaken them out elsewhere — though "we will adopt them eventually" was already a locked-in roadmap decision. |
| GStreamer `webrtcbin` via C bindings + hybrid Rust state machine *(previous pick, deprecated)* | Voice work starts immediately without R3.0 plumbing. Smaller blast radius if gtk-rs idioms turn out to need iteration. | ~550 LOC of C glue (`voice.{c,h}` + `voice_audio.{c,h}`) we'd delete during the §11 migration anyway. `g_signal_connect` lifetime tracking around `GstWebRTCBin` callbacks is exactly the bug surface 8.C debugging is most likely to hit. |
| libdatachannel (paullouisageneau) | MIT. Single C++ library, C API binding. ~25k LOC standalone. | Audio I/O isn't included — PortAudio / miniaudio dep, μ-law encoder, RTP pump all become our problem. |
| webrtc-rs | Pure Rust. MIT. | Heavy crate tree (~150 transitive deps). Same audio-I/O gap as libdatachannel — none of the WebRTC media path is wired. |
| libwebrtc (Google) | Reference impl. | Build is a nightmare (depot_tools, gn, ninja, vendored deps). Disqualified. |
| Pion | Spec author's reference. | Go. Disqualified for a Linux/GTK client. |

**Why the revision.** An earlier draft of this plan picked the C
bindings with a hybrid Rust state machine, on the assumption that
`gstreamer-rs` required a stack of R3/R4 prerequisites we hadn't paid
for yet. On re-examination after R2 closed, the actual prerequisite
is much smaller — see §5 Phase 8.0 — and Phase R2 already shipped the
Cargo workspace plumbing we'd build on. Switching now saves us from
writing ~550 LOC of C glue we'd delete during the migration §11
described anyway. Tokio is *not* a prerequisite: `webrtcbin` runs on
GLib threads and dispatches through `GMainContext`, which
`gstreamer-rs` integrates with via `gst::bus::BusStream` and
`glib::MainContext::default().spawn_local`. The voice controller
doesn't need an async runtime.

**`gstreamer-webrtc-rs` API coverage** (verified against
`gstreamer-webrtc` 0.25.2, dropped 2026-05-11):

- `WebRTCSessionDescription`, `WebRTCSDPType` — SDP offer / answer.
- `WebRTCPeerConnectionState`, `WebRTCSignalingState`,
  `WebRTCICEGatheringState`, `WebRTCICEConnectionState` — the
  state-machine reads these out of `webrtcbin` properties instead of
  parsing strings.
- `WebRTCRTPTransceiver` / `Sender` / `Receiver` — per-track mid /
  direction handling.
- `WebRTCICECandidate` (v1.28), `WebRTCICETransport`,
  `WebRTCDTLSTransport` — typed ICE / DTLS state for toasts and
  proto-trace output.
- `connect_pad_added` typed on `WebRTCBin`; `connect("on-negotiation-needed", …)`,
  `connect("on-ice-candidate", …)`; `Promise::new_with_change_func`
  + `emit("create-offer" / "create-answer", …)`;
  `emit("set-{local,remote}-description", …)`;
  `emit("add-ice-candidate", …)`.
- `gst_sdp::SDPMessage::parse_buffer` for SDP parsing into typed
  structures.
- Renegotiation supported since 2019; same `webrtcbin`-core caveat
  applies whether you drive it from C or Rust (removed streams
  marked inactive rather than transceiver/m-line reuse).

A representative pipeline for one voice session (one send + N−1
receive tracks bundled on one transport) — unchanged from the C
draft, `webrtcbin` is `webrtcbin` regardless of the language driving
it:

```
webrtcbin name=webrtc

  # send leg — captured mic → mulaw → RTP → webrtcbin
  autoaudiosrc ! audioconvert ! audioresample
              ! audio/x-raw,rate=8000,channels=1
              ! mulawenc ! rtppcmupay
              ! application/x-rtp,media=audio,encoding-name=PCMU,payload=0
              ! webrtc.

  # receive legs are added dynamically on webrtc::pad-added,
  # one per remote track:
  webrtc. ! rtppcmudepay ! mulawdec
         ! audioconvert ! audioresample ! autoaudiosink
```

Per-receive-leg add/remove is driven by `connect_pad_added` /
`connect_pad_removed` on the Rust-side `WebRTCBin`, which fire when
SDP renegotiation completes. Standard GStreamer pattern, typed in
gstreamer-rs.

---

## 4. New code layout — all-Rust controller

Three Rust crates, the existing `hotline-proto` crate extended, plus
three small C touch-points (UI panel, settings page, `rcv.c`
dispatch) and the GtkhxSession signal additions. The all-Rust pivot
in §3 collapsed the previous "hybrid Rust/C" split: the C voice
controller (`voice.{c,h}`) and audio-factory module
(`voice_audio.{c,h}`) move into the new `hxvoice-runtime` Rust crate
that drives `gstreamer-rs` directly.

### Rust — wire-protocol layer, in `rust/crates/hotline-proto/`

| Addition | Role |
|---|---|
| `src/voice.rs` (new module) | Typed representation of voice transactions: `JoinRequest`, `LeaveRequest`, `SdpAnswer`, `IceCandidate`, `MuteToggle`, `Participant`, `MidLabel`. Builders return `HxChunk` arrays like the rest of `build::`; parsers return `#[repr(C)]` out-structs with `_Static_assert`-mirrored layouts. |
| `src/voice.rs::sdp` submod | Minimal SDP shape parser — just enough to find `a=mid:user-{UID}` labels, the `a=group:BUNDLE` list, the disabled `m=audio 0` lines for left-participant slots, and confirm PCMU is offered. We don't reimplement SDP; the heavy lifting is `gst_sdp::SDPMessage`. We only extract the bits the state machine needs. |
| `src/voice.rs::ice` submod | Tiny JSON build/parse for the `RTCIceCandidateInit` dict (`candidate`, `sdpMid`, `sdpMLineIndex`, `usernameFragment`). Use `serde_json` (already in the workspace via tracker-v3 work). |
| `messages.rs` additions | New opcode variants (`VoiceJoinRoom = 600`, etc.) and field-tag variants (`VoiceSdp = 0x01F5`, etc.) added to the `#[non_exhaustive]` enums. |
| `ffi.rs` additions | ~4 new `#[no_mangle] extern "C"` shims for the C wire-dispatch in `rcv.c`: `gtkhx_proto_parse_voice_participants`, `_parse_voice_mid_label`, `_parse_voice_sdp_summary`, `_parse_voice_ice_json`. (Builders no longer need FFI shims — `hxvoice-runtime` calls the Rust builders directly.) |

### Rust — voice session state machine, in a new crate

| Addition | Role |
|---|---|
| `rust/crates/hxvoice/` (new crate, `no_std`-friendly) | Pure state machine. One `SessionMachine` per active voice session. Holds: current state enum (`Idle` / `JoinSent` / `OfferPending` / `Connecting` / `Connected` / `Leaving`), pending-renegotiation queue, mid→user_id map, mute flag, current participant list. Single entry point `step(&mut self, Event) -> Vec<Action>`. **No I/O, no GStreamer, no GLib types** — just typed data in, typed data out. The `hxvoice-runtime` crate pumps events in and dispatches the returned actions. |
| `src/event.rs` | Typed inbound events: `JoinRequested { cid }`, `LeaveRequested { cid }`, `MuteToggleRequested { muted }`, `SdpOfferReceived { sdp }`, `IceCandidateReceived { json }`, `EndOfRemoteCandidates`, `ParticipantsUpdated { entries }`, `ServerTaskError { code, text }`, `WebrtcPadAdded { mid }`, `WebrtcPadRemoved { mid }`, `WebrtcAnswerCreated { sdp }`, `WebrtcLocalIceGathered { json }`, `WebrtcConnectionStateChanged { state }`, `Timeout { kind }`. |
| `src/action.rs` | Typed outbound actions: `SendWireFrame { opcode, chunks }`, `SetRemoteDescription { sdp }`, `CreateAnswer`, `SetLocalDescription { sdp }`, `AddRemoteIce { json }`, `StartReceivePipeline { mid, user_id }`, `StopReceivePipeline { mid }`, `SetSendPipelineMute { muted }`, `EmitSignal { kind, payload }`, `ArmTimer { kind, ms }`, `CancelTimer { kind }`, `TearDown`. |
| `src/state.rs` | The `SessionMachine` itself. Pure `match (state, event)` impl. The hard cases the spec calls out — renegotiation serialization, dropping stale offers, implicit-leave when joining a second room, ICE/DTLS/media timeouts, mid-slot reuse after a participant leaves — are all enforced here, not in the runtime layer. |
| `tests/` | Property-style state-machine tests: every event in every state has a defined transition; renegotiation queue under all 4 combinations of (pending-offer, new-offer-arrives); spec's annotated lifecycle (join → renegotiate-for-late-arriver → leave) replayed as event sequences. This is where the spec's defensive notes (§Renegotiation Flow, §Session Timeout and Failure) become assertions. |

`hxvoice` is a separate crate, not a submodule of `hotline-proto`,
because it depends on `hotline-proto`'s typed messages (one-way:
hxvoice → hotline-proto) and because keeping it isolated lets it
stay `no_std`-friendly with zero GLib / GStreamer dependencies.
CI can run its property tests without GStreamer installed.

### Rust — GStreamer / webrtcbin runtime, in a new crate

| Addition | Role |
|---|---|
| `rust/crates/hxvoice-runtime/` (new crate) | Side-effectful layer that holds the `gstreamer::Pipeline` and `gstreamer_webrtc::WebRTCBin` for one voice session, pumps GStreamer signals into `hxvoice::SessionMachine`, and dispatches the returned actions. Subscribes to `connect_pad_added`, `connect("on-negotiation-needed", …)`, `connect("on-ice-candidate", …)` using `gstreamer-rs`'s typed methods. Dispatches `Action`s via `webrtcbin.emit("set-remote-description", &[&sdp, &None::<gst::Promise>])`, `gst::Promise::new_with_change_func(…)` for `create-answer`, etc. |
| `src/runtime.rs` | `VoiceRuntime` struct — the per-session owner of the pipeline + webrtcbin + `SessionMachine`. Implements `step_with_event(Event)` (which calls into `SessionMachine::step` and walks the action list). |
| `src/wire_io.rs` | Thin layer that turns `Action::SendWireFrame { opcode, chunks }` into a call back into the C `hlwrite_chunks` (via one C-side FFI helper) until R3 lands the Rust-side connection. Documented as the one remaining piece that ports to native Rust when R3's `hxnet` crate exists. |
| `src/signals.rs` | `EmitSignal` action handler — wraps the C-side `GtkhxSession` pointer with `unsafe { glib::Object::from_glib_borrow(ptr) }` and calls `obj.emit_by_name::<()>("voice-*", &[…])`. The wrapping pattern is the one R3.0 (Phase 8.0) lands and documents. |
| `src/audio.rs` | Audio device enumeration (`gst::DeviceMonitor`) + factory functions for the configured `autoaudiosrc` / `autoaudiosink` elements. Handles "no mic" graceful degradation (listen-only). Replaces the planned C `voice_audio.{c,h}`. |
| `src/ffi.rs` | ~6 `#[no_mangle] extern "C"` shims: `gtkhx_voice_runtime_new(session_ptr) -> *mut VoiceRuntime`, `_free`, `_handle_join`, `_handle_leave`, `_handle_mute_toggle`, `_handle_rcv_task(opcode, …)`. Opaque pointer ABI; the C side only sees `HxVoiceRuntime *`. |
| `tests/` | Tier 2 integration tests that build a runtime against a `webrtcbin` loopback (two `WebRTCBin` elements connected to each other in one process) and run the full SDP/ICE round-trip without a network. Catches binding-level regressions independently of Janus. |

The runtime crate is the file that materially benefits from
`gstreamer-rs`: typed signal connections instead of
`g_signal_connect` with `void*`; `gst::Promise::new_with_change_func`
instead of manual closure lifetime tracking around promise replies;
exhaustive matching on `WebRTCSignalingState` instead of stringly
compared property reads.

### C — wire dispatch, UI, GtkhxSession bridge

| File | Role | Rough LOC |
|---|---|---|
| `src/hotline.h` additions | 7 opcodes (`HTLC_HDR_VOICE_JOIN` 600 … `HTLC_HDR_VOICE_MUTE` 606), 5 data IDs (0x01F5–0x01F9). `#define`s only; the canonical typed definitions live in Rust. Same dual-define convention chat-history already uses. | ~50 |
| `src/hotline_proto.h` additions | `extern` declarations for the new `gtkhx_proto_parse_voice_*` FFI shims, paired with `_Static_assert(sizeof(gtkhx_proto_voice_participant) == N, ...)` for each out-struct. | ~40 |
| `src/voice_panel.{c,h}` | UI: per-chat-tab voice toolbar (Join/Leave/Mute/PTT) + participant speaker indicators in the user list. Attaches to the AdwTabPage for each cid. Stays C until R5 picks up `chat.c` / `users.c`. Calls `gtkhx_voice_runtime_handle_join` etc. on the opaque Rust runtime pointer. | ~400 |
| `src/voice_settings.{c,h}` | AdwPreferencesPage for input/output device, default-muted, PTT keybind, "auto-join voice when joining a chat room." Calls into the Rust runtime's `gst::DeviceMonitor` wrapper for device enumeration. | ~200 |
| `src/rcv.c` additions | `rcv_task_voice_*` for 600/601/603/606 replies. Switch arms for 602/604/605 server-initiated notifications. Each calls a Rust parser, then calls `gtkhx_voice_runtime_handle_rcv_task` to feed the typed result into the state machine. | ~150 |
| `src/gtkhx_session.{c,h}` additions | New signals: `voice-room-status`, `voice-track-added`, `voice-track-removed`, `voice-mute-changed`, `voice-state-changed`, `voice-error`. Model→view bridge mirrors the existing taxonomy. The Rust runtime emits them via `glib::Object::from_glib_borrow` (Phase 8.0 pattern). | ~100 |
| `src/voice.h` (header only) | Tiny C-visible API: `HxVoiceRuntime` opaque struct, `gtkhx_voice_runtime_new` / `_free` / `_handle_*` declarations. The implementation is the Rust `hxvoice-runtime::ffi` shims. | ~30 |

Total roughly **940 C LOC + ~1,500 Rust LOC** + tests, vs the
previous draft's 1.55 k C + ~900 Rust. The C shrinkage comes from
`voice.c` (~350) and `voice_audio.c` (~200) moving into the Rust
runtime; the Rust growth covers `hxvoice-runtime`'s gstreamer-rs
dispatch (~600) and audio-monitor wrapper (~150).

The Hotline 1.x wire-protocol additions are pure: 7 new TRAN
opcodes (600–606) and 5 new data fields (0x01F5–0x01F9). They don't
touch any existing parse paths, don't shift any existing IDs, and
are gated behind the capability echo — legacy servers and legacy
clients are completely unaffected. Same property as every other
fogWraith extension we've already shipped.

### Why split the SDP / ICE work between `hotline-proto` and `gstreamer-rs`

`gstreamer-rs` parses SDP into `gst_sdp::SDPMessage` and ICE
candidates into `WebRTCICECandidate` (v1.28). We don't need a second
full SDP parser in Rust. What we *do* still want from
`hotline-proto`:

- **mid label parsing** — `user-{UID}` → `u16 user_id` with strict
  validation (no leading zeros, in 1..=65535, etc.). Wire-format
  shaped, belongs in `hotline-proto`. The runtime crate then maps
  the parsed mid to the `WebRTCRTPTransceiver` it associates with.
- **Participant blob walking** — the packed 6-byte-per-entry
  `DATA_VOICE_PARTICIPANTS` binary is exactly the shape `hotline-
  proto::parse` already handles for the file-list and history walkers.
- **ICE JSON build / parse** — the inner JSON payload is wire data
  carried inside a Hotline data field. Rust owns that boundary, and
  the runtime hands the parsed `{ candidate, sdp_mline_index }` to
  `webrtcbin.emit("add-ice-candidate", …)`.
- **SDP summary** — extract just the `a=mid` list and the BUNDLE
  group from the SDP blob so the state machine can sanity-check what
  it's about to set as the remote description. Cheap defensive parse,
  not a full SDP implementation.

Full SDP and full ICE handling stays in `gstreamer-rs` /
`gst-sdp-rs` where they belong.

---

## 5. Sub-phasing

Each sub-phase ends on a clean build + passing tests. Same discipline
as the TLS phasing — don't pile up "and these other 800 lines also
need to land before anything works."

### Phase 8.0 — R3.0 glib-rs foothold (prerequisite)

Goal: the Rust workspace gains the `glib` / `gio` / `gtk4` /
`libadwaita` crate trees, and we land + document the pattern for
Rust code to wrap a C-owned GObject and emit signals on it. This is
the precondition that makes `gstreamer-rs` (Phase 8.B+) a reasonable
choice. Independent of the rest of R3 (no tokio, no `hxnet`, no
`xfers.c` port — just the GLib/gtk-rs deps and the wrapping pattern).

Crate name: **`hxbridge`** — the name RUST-ROADMAP §R3 work item 2
already uses for the Rust↔GLib bridge crate. Phase 8.0 lands the
precursor; subsequent R3 work expands it with the tokio↔GLib
forwarding pipeline.

Work:
1. Add `glib`, `gio`, `gtk4`, `libadwaita`, `gstreamer`,
   `gstreamer-app`, `gstreamer-audio`, `gstreamer-sdp`,
   `gstreamer-webrtc`, `gstreamer-rtp` to `rust/Cargo.toml`. Pin to
   the gtk-rs-core 0.22 family (which is what RUST-ROADMAP's
   "gtk4-rs is at 0.11+" note refers to — gtk4 0.11 ships in
   gtk-rs-core 0.22) and the gstreamer-rs 0.25 line. Plumb through
   the existing `rust/meson.build` `custom_target` — no special
   build work, the cargo workspace picks them up.
2. New `rust/crates/hxbridge/`. Contains:
   - `pub unsafe fn session_from_ptr(ptr: *mut GObject) -> glib::Object` —
     wraps `glib::Object::from_glib_borrow` with the lifetime model
     spelled out in a doc-comment (Rust holds a borrowed ref, C owns
     the GObject, do not `g_object_unref` from Rust). **Decision to
     lock in via the lifetime-model doc page:** use `from_glib_borrow`
     for read-only / non-emit access; use `from_glib_none` (which adds
     a ref, dropped on Rust-side drop) for the emit path, because
     C signal handlers can re-enter and `g_object_unref` the session
     out from under a borrowed Rust handle. Picking `from_glib_none`
     for emit is one extra ref per call — negligible cost vs the
     use-after-free window `from_glib_borrow` would open.
   - One reference implementation of "emit a `GtkhxSession` signal
     from Rust" — **pick a scalar-only signal**. `task-update`
     (carries `(session, task)` where `task` is an opaque pointer in
     `G_TYPE_POINTER`, no boxed-type plumbing involved) is the
     obvious candidate, or `user-delete` / `users-clear` if a less
     hot-path signal is preferable. **Do NOT** pick a boxed-type
     signal (`chat`, `msg`, `user-create`, etc.); the
     `HxChatEvent`/`HxMsgEvent` boxed-type port is R4 work per the
     post-R2 roadmap, and dragging it into R3.0 inflates scope.
3. CI: add `cargo build -p hxbridge` to the existing matrix.
   Confirm Tier 1, 2, 3 still green; the existing C-only build path
   is unaffected because nothing C-side uses the new crate yet.
4. Document the wrapping pattern in `docs/rust-glib-interop.md` —
   future contributors should not have to re-derive the lifetime
   model. Two pages, max: the wrapping shape, the `from_glib_borrow`
   vs `from_glib_none` rule (with the re-entrant-emit example that
   motivates it), and a one-paragraph pointer to
   `glib::MainContext::default().spawn_local` for futures (no
   wrapper — the canonical glib name is the documented entry point;
   wrapping it would just hide the canonical spelling without adding
   type safety).

Tests (~150 LOC, not 50):
- Ref-count doesn't leak across N emits (loop 1000 times,
  `g_object_unref` self afterwards, check refcount is back to 1).
- C-side handler observes the Rust-driven emit (set a flag in a
  `g_signal_connect`'d C callback, assert it fires).
- Re-entrant emit: Rust emits → C handler emits the same signal →
  Rust handler. The `from_glib_none` decision above keeps this
  re-entrant case safe; the test pins it.
- `glib::MainContext::default().spawn_local` actually polls the
  future (set a flag from inside the future, drive the main loop,
  assert the flag flipped).

**Costs to flag.** Phase 8.0 isn't free even at "~1 week + 150 LOC":

- **Cargo.lock blast radius.** The gtk-rs-core + gstreamer-webrtc-rs
  dep tree is large — today's `Cargo.lock` is around 150 crates;
  after 8.0 it's 700+. This affects (a) CI cache size / restore
  time, (b) the vendored `cargo-sources.json` Flatpak builds depend
  on (offline-sources tarball balloons proportionally), (c) cold-
  build time for new contributors.
- **GStreamer runtime floor.** gstreamer-rs 0.25 tracks GStreamer
  1.26. The Flatpak runtime (`com.nasledov.gtkhx.yml`) needs to
  ship 1.26-or-newer when 8.B starts consuming the WebRTC bindings —
  GNOME 47 runtime ships GStreamer 1.24, GNOME 48 brings 1.26.
  Verify before 8.B; not blocking for 8.0 itself since 8.0 only
  pulls the bindings in, doesn't exercise them yet.
- **Pre-emptive commit to gtk-rs 0.22 idioms.** R3.0 ships the
  lifetime model and signal-emit shape ahead of R3 proper, R4, and
  R5. A bad pattern at this layer propagates. The
  `docs/rust-glib-interop.md` page is the single source of truth
  for the convention; treat its review as Phase 8.0 work, not 8.0
  follow-up.

Exit criteria: workspace compiles cleanly with the new deps. The
chosen scalar signal (e.g. `task-update`) is emitted from Rust via
`hxbridge`, with the C-side call site reduced to a thin pass-
through. `docs/rust-glib-interop.md` checked in and reviewed.
Tier 1/2/3 green. The `gstreamer-webrtc` crate graph is in the
build but unused — voice work in 8.B starts using it. Flatpak
runtime version + GStreamer 1.26 floor verified (note in the doc
even if action defers to 8.B).

Branch: `claude/voice-phase-0-glib-foothold` (a sub-branch of the
RUST-ROADMAP R3 work, even though it lands ahead of formal R3
kickoff).

**Order note** — §5 lands 8.0 before 8.A. The case for: 8.0 is the
gtk-rs version + dep-tree commit, and 8.B (gstreamer-rs pipeline)
shouldn't have to scramble for the wrapping pattern under time
pressure. The case against: 8.A is pure hotline-proto wire-format
extension and could land first with no gtk-rs deps at all,
deferring the 700-crate Cargo.lock churn. We keep 8.0 first
because the lifetime-model doc page lands once and is a permanent
asset; landing 8.A first would mean reviewing the 8.0 PR with
nothing to look at except "one signal moved to Rust + Cargo.lock
ballooned." If 8.0 starts and the dep churn looks worse than
expected (Flatpak vendoring blows up, CI cache eviction starts
churning), swap the order — 8.A doesn't depend on 8.0 work at
all.

### Phase 8.A — Capability advertisement + signaling scaffolding

Goal: server sees us as voice-capable, we can send and receive every
600–606 opcode, but no media flows yet. **All new wire code lands in
Rust.** Depends on Phase 8.0 only for the `hotline-proto` workspace
machinery already shipped in R2 — 8.A could land before 8.0 if we
wanted to interleave, but the order in this doc lands 8.0 first so
8.B doesn't have to scramble for the glib-rs pattern under time
pressure.

Work:
1. Add `VoiceJoinRoom` (600) through `VoiceMute` (606) variants to
   `messages::Opcode` in `hotline-proto`. Add `VoiceSdp` (0x01F5)
   through `VoiceParticipants` (0x01F9) variants to
   `messages::FieldTag`. Mirror with `HTLC_HDR_*` / `HTLC_DATA_*`
   `#define`s in `src/hotline.h` for the C-side switch-case
   readability — Rust holds the canonical typed definitions, C holds
   integer aliases for the dispatch code.
2. New `rust/crates/hotline-proto/src/voice.rs` module with
   `build::voice_{join,leave,answer,ice,mute}_chunks` returning
   `HxChunk` arrays plus `parse::voice_{participants,mid_label,
   sdp_summary,ice_json}`. Layout-pinned out-structs (`#[repr(C)]`
   + `const _: () = assert!(size_of::<…>() == N, …)`). Unit-tested
   against the spec's annotated SDP / ICE examples.
3. FFI shims in `hotline-proto::ffi` (`gtkhx_proto_build_voice_*`,
   `gtkhx_proto_parse_voice_*`) plus matching `extern` declarations
   and `_Static_assert` mirrors in `src/hotline_proto.h`.
4. Add `HTLC_CAP_VOICE` to **both** `req.caps` masks: the legacy
   LOGIN path in `send_login` (network.c:1231) and the HOPE Step-2
   LOGIN path in `rcv_task_login` (rcv.c:1396). Both currently
   carry the same three-bit mask hard-coded inline; both need
   `| HTLC_CAP_VOICE`. Missing the HOPE branch would silently
   exclude Janus (and any other HOPE server) from voice
   negotiation, which is exactly the case the Phase 8.A exit
   criterion smoke-tests.
5. Add `#define HL_ACCESS_VOICE_CHAT 55` to `src/hl_access.h`, in
   the same block as `HL_ACCESS_READ_CHAT_HISTORY` (bit 56) — the
   bitmap accessor `hl_access_has()` already handles bits 0–63,
   no new decoding work needed. Phase 8.D queries this constant to
   grey out the Voice button when the user lacks permission.
6. New `src/voice.h` opaque-handle header + thin `rcv.c`-side
   wire-out path that calls the Rust builders in `hotline-proto::voice`
   and emits the chunks through `hlwrite_chunks`. No `hxvoice-runtime`
   crate yet — that lands in 8.C. Builder FFI is needed for these
   in-flight 8.A sends; mark the shims for retirement once 8.C wires
   the runtime to call the Rust builders directly.
7. New `rcv_task_voice_*` in `src/rcv.c` — call Rust parsers, log
   the typed result, emit GtkhxSession signals. View side not wired
   yet.
8. `proto_trace.c` additions so `GTKHX_DEBUG=voice` traces every
   voice transaction inbound and outbound — same as we did for
   `chat-history`. (Trace label tables stay C per the R2 "defer
   `proto_trace.c` migration" note.)

Exit criteria: launch against Janus (already in our Tier 3 matrix
as the TLS test target — voice just adds the UDP base+4 port),
confirm capability is echoed, fire Join (600) manually from a
debug toggle, see the 602 SDP offer come back in the trace, see
participant list updates from 605 parsed by Rust into structured
events. Nothing audible. Rust test count grows by ~15–20.

Branch: `claude/voice-phase-a`.

### Phase 8.B — GStreamer dependency + bare pipeline (Rust)

Goal: a `hxvoice-runtime` skeleton that owns a `gst::Pipeline`,
captures from the mic, encodes to μ-law, and renders silence on
playback — proves the gstreamer-rs crates are wired, the audio
devices are reachable, the build works on Flatpak, none of the
protocol changes have moved.

Work:
1. C-side `meson.build` additions: `dependency('gstreamer-1.0',
   version: '>=1.20')`, `gstreamer-app-1.0`, `gstreamer-audio-1.0`,
   `gstreamer-webrtc-1.0`. The system libs are still required (the
   Rust crates wrap them via `gstreamer-sys`). Flatpak
   `com.nasledov.gtkhx.yml` confirms the GNOME runtime already ships
   them; if not, add `org.freedesktop.Sdk.Extension.gst-plugins-bad`
   lines.
2. `gst::init()` from Rust, called once from the C `main` via a
   small `gtkhx_voice_init()` shim. Order: `gst::init` after
   `gtk_init` since GStreamer doesn't care about display init.
3. New `rust/crates/hxvoice-runtime/src/audio.rs` — enumerate input
   devices via `gst::DeviceMonitor`, factory functions that return
   configured `gst::Element` for source / sink. Replaces the planned
   C `voice_audio.{c,h}` outright.
4. Smoke test (`hxvoice-runtime/tests/loopback.rs`, Tier 2) that
   builds an `audiotestsrc ! mulawenc ! mulawdec ! fakesink`
   pipeline, runs it for 100 ms, checks state reached `Playing`.
   `audiotestsrc` (not `autoaudiosrc`) because CI containers have no
   audio device. Catches "gstreamer-rs is here and the wrapped libs
   work" before any voice-specific code.

Exit criteria: clean build with new deps. Loopback Rust test
passes locally and in the Tier 2 suite. No protocol changes in this
phase — voice transactions still scaffolded only from Phase A.

Branch: `claude/voice-phase-b`.

### Phase 8.C — state machine + webrtcbin runtime (both Rust)

Goal: send-only voice works against Janus. We can hear a remote
participant but they can't hear us yet. (Or vice versa — pick
one direction to chase down completely first.)

This is the phase where the all-Rust pivot earns its keep. The state
machine lands first, fully tested in pure Rust against scripted
event sequences from the spec's annotated lifecycle examples; the
`gstreamer-rs` runtime lands second and is reduced to "translate
typed GStreamer signal → typed Rust event, walk action list → call
typed GStreamer methods."

Work:
1. New `rust/crates/hxvoice/` — pure `SessionMachine` per §4. Lands
   with property tests covering every event-in-every-state
   transition, plus replays of the spec's "B joins room with A
   already in voice" and "B leaves" sequences as event traces. No
   FFI needed — `hxvoice-runtime` consumes it as a normal Rust crate
   dep.
2. New `rust/crates/hxvoice-runtime/src/runtime.rs` — `VoiceRuntime`
   owns the `gst::Pipeline`, `gstreamer_webrtc::WebRTCBin`, the
   `hxvoice::SessionMachine`, and a `glib::Object` borrow of the
   C-side `GtkhxSession`. One method per inbound event source
   (`handle_join`, `handle_leave`, `handle_mute_toggle`,
   `handle_rcv_task`), each calling `SessionMachine::step` and
   dispatching the returned actions.
3. Wire the SDP offer flow with typed `gstreamer-rs` calls: 602
   arrives → C `rcv_task_voice_sdp_offer` calls
   `gtkhx_voice_runtime_handle_rcv_task` → `SessionMachine::step`
   returns `[SetRemoteDescription, CreateAnswer]` →
   `webrtcbin.emit("set-remote-description", &[&sdp, &None::<gst::Promise>])`,
   then `webrtcbin.emit("create-answer", &[&None::<gst::Structure>, &promise])`
   where `promise = gst::Promise::new_with_change_func(move |reply| { … step(WebrtcAnswerCreated …) })`
   → state machine returns `[SetLocalDescription, SendWireFrame(603, …)]`.
4. Wire the ICE flow with the same shape: `webrtcbin.connect("on-ice-candidate", false, …)`
   → `WebrtcLocalIceGathered` event → `SendWireFrame(604)` action;
   incoming 604 → `IceCandidateReceived` event → `webrtcbin.emit("add-ice-candidate", &[&mline_index, &candidate])`.
   The empty-candidate end-of-candidates marker becomes a distinct
   `EndOfRemoteCandidates` event so the state machine knows it has a
   complete remote candidate set.
5. Track-to-user mapping in Rust: `webrtcbin.connect_pad_added(…)`
   (typed!) → `WebrtcPadAdded { mid }` event → the state machine
   cross-references its mid→UID map (populated from the SDP summary
   parse) and returns `StartReceivePipeline { mid, user_id }`. The
   runtime adds `rtppcmudepay ! mulawdec ! audioconvert !
   audioresample ! autoaudiosink` as a `gst::Bin` and links it to
   the new pad.
6. Renegotiation serialization in the state machine — `OfferPending`
   state queues a second `SdpOfferReceived`, transitions emit `[]`
   until the first answer flushes, then drains. The runtime doesn't
   know about the queue at all.
7. Timeouts (spec §Session Timeout and Failure) as `ArmTimer` /
   `CancelTimer` actions plus a `Timeout { kind }` event the runtime
   fires from `glib::timeout_add_seconds_local` callbacks. ICE
   failure, DTLS failure, media timeout all funnel through the same
   shape.
8. Bus message handling — `pipeline.bus().unwrap().add_watch_local(…)`
   forwards `gst::message::MessageView::Error` / `::Warning` /
   `::StateChanged` into typed events the state machine can react to
   (e.g. on `WebRTCPeerConnectionState::Failed`, transition to
   `Leaving` and emit `EmitSignal { voice-error }`).

Exit criteria: against Janus, join an empty room → invite a second
client (or use the Janus admin tooling to inject a participant) →
SDP offer received → answer + ICE complete → audio flows. Rust
state-machine test count >50; `hxvoice-runtime` loopback test count
>5.

Branch: `claude/voice-phase-c`.

### Phase 8.D — UI in the chat tab + user list

Goal: a Voice button appears on the chat tab when the server echoed
CAP_VOICE; clicking it joins voice in that room. Mute toggle. Per-
user speaker indicator in the user list.

Work:
1. New `src/voice_panel.{c,h}`. The voice toolbar lives at the top
   of each `chat_tabs.c`-mounted chat page. Buttons: Join Voice /
   Leave Voice (toggle), Mute / Unmute. Visibility / state logic:
   - `htlc->caps & HTLC_CAP_VOICE` is unset → toolbar is *hidden*
     entirely (server didn't echo the cap; most servers won't,
     so don't clutter the chat tab with an unusable button).
   - Cap echoed but `hl_access_has(htlc->access,
     HL_ACCESS_VOICE_CHAT)` is FALSE → toolbar is *visible but
     greyed* with tooltip "Voice chat requires permission" (the
     server supports voice, this account just can't use it; spec
     recommends not hiding in this case so users know to ask an
     admin for the access bit). The `HL_ACCESS_VOICE_CHAT`
     constant is added in Phase 8.A.
   - Cap echoed and bit 55 set → fully interactive.
2. Hook the `voice-room-status` GtkhxSession signal to update the
   participant list in `users.c` — small mic / muted-mic / talking
   icon next to the user, driven by the mute bit in
   `DATA_VOICE_PARTICIPANTS` and (where we have RTP-level activity
   info from the audio level extension) by speaker activity.
3. AdwToastOverlay messages on join / leave / error.
4. Spec compliance: clients SHOULD join muted by default. Make
   that the default; offer a "Auto-unmute on join" pref in 8.E.
5. Single-room-at-a-time enforcement client-side: clicking Join
   Voice in room B while voice is active in A pops an AdwAlertDialog
   confirming the implicit leave (spec mandates the server does the
   teardown anyway, but a confirm dialog avoids accidents during
   the chat-tab era of multi-room UI).

Exit criteria: round-trip voice between two GtkHx instances
connected to the same Janus room. Speaker indicators light up
while the other side talks. Mute is honoured on both ends.

Branch: `claude/voice-phase-d`.

### Phase 8.E — Settings + push-to-talk

Goal: device selection, PTT, polish.

Work:
1. New AdwPreferencesPage under Settings → Voice: input device
   combobox (populated from `GstDeviceMonitor`), output device
   combobox, "Start muted" toggle (default ON per spec),
   "Push-to-talk key" capture row, "Auto-join voice when joining a
   chat room" toggle (default OFF).
2. PTT implementation: `GtkEventControllerKey` on the chat input
   widget. Key down → send 606 muted=0; key up → send 606 muted=1.
   Spec recommends server-side ~100 ms debounce; client-side debounce
   isn't required but is polite — a 50 ms hold-down prevents
   accidental taps from spamming the wire.
3. Persist all of this to `$CONFIG/gtkhxrc` via the existing
   GKeyFile path in `prefs.c`.

Exit criteria: usable voice chat for one full evening of testing.
Pref persistence verified across restarts.

Branch: `claude/voice-phase-e`.

### Phase 8.F — Tests against Janus

Since Janus already implements the voice extension and is already
in the Tier 3 matrix as the TLS test target, this phase is mostly
"write the tests against the server that already speaks the
protocol." No mock SFU required for v1 (see §6).

Work:
1. **Tier 2 state-machine property tests** in `hxvoice/tests/` —
   most of these already land in Phase 8.C alongside the state
   machine. This phase fills in the gaps: malformed SDP rejection,
   oversized participant list, dropped 603 answer, ICE timeout,
   mid-slot reuse after a participant leaves.
2. **Tier 2 wire-fixture tests** in `hotline-proto/tests/voice_*.rs`
   for the build/parse round-trips: SDP-chunk pack/unpack, ICE-JSON
   build, participant-blob walker against the spec's annotated
   examples, mid-label parsing across the valid range.
3. **Janus voice matrix row**. Extend `tests/janus/Dockerfile` (and
   `tests/janus/conf/` if a server-side flag is needed to enable
   voice) to expose UDP base+4, and add the matching port to the CI
   service-container definition in `.github/workflows/tests.yml`.
   In `tests/integration/server_matrix.c`, add an `HX_TEST_CAP_VOICE`
   cap bit and a `voice_port` field on the row analogous to the
   existing `tls_port` / `HX_TEST_CAP_TLS` pair.
4. **Tier 3 integration tests** under `tests/integration/`:
   `test_voice_join.c` (join empty room, leave),
   `test_voice_sdp_roundtrip.c` (full WebRTC handshake completes,
   peer-connection reaches CONNECTED),
   `test_voice_ice_trickle.c` (candidate exchange both ways,
   end-of-candidates signal),
   `test_voice_mute.c` (606 round-trips, Janus's debounce window
   is respected on the wire),
   `test_voice_participants.c` (Voice Room Status 605 deltas
   match expected sequence as the test harness adds/removes
   participants),
   `test_voice_implicit_leave.c` (join voice in room B while in
   voice in room A → 605 to room A removes us, room B join
   succeeds),
   `test_voice_disconnect_cleanup.c` (drop control channel, watch
   Janus emit 605 with us removed from any rooms we were in).
   Two-client tests use the existing harness pattern from
   `test_two_client_chat.c`.
5. **Tier 3 negative tests**: voice disabled in Janus config → 600
   returns task error; user without `accessVoiceChat` → 600 returns
   permission error with `DATA_ERROR_TEXT` surfaced as toast.
6. **Audio I/O in headless CI** — `autoaudiosrc` / `autoaudiosink`
   don't work in a container with no audio device. Tests that care
   about actual RTP flow use `audiotestsrc` (1 kHz sine wave) as
   the source and `fakesink` as the sink, then assert on RTP
   packet counts from Janus's stats / from a `tee ! appsink` tap.
   Tests that only care about the signaling round-trip use a
   listen-only join (no send track, no audio device).

Exit criteria: full Tier 3 voice matrix green in CI against Janus.
Rust test count for `hxvoice` >75 + `hotline-proto::voice` >25.

Branch: `claude/voice-phase-f`.

**If we ever need a mock SFU later** for adversarial inputs Janus
won't generate (malformed SDP, deliberately-out-of-spec
participant lists, deliberately-dropped 603 answers), the natural
template is a small Pion-based Go binary Dockerized alongside the
existing servers under `tests/`. Same Dockerfile + `meson.build`
shape as `tests/janus/`, `tests/mhxd/`, `tests/hxtrackd/`,
`tests/argus/`. Punt until something motivates it; the
state-machine property tests in `hxvoice` already cover the
adversarial event sequences a mock would inject.

---

## 6. Server availability — Janus is the Tier 3 target

**Janus already implements the voice extension.** That changes the
shape of this phase materially compared to chat-history (where we
had to spike a mock first because no server spoke the protocol).

Janus is already in our Tier 3 matrix: it's the TLS test target
today (see `docs/tls-scoping.md`; shipped on
`claude/tls-phase1-control-channel`), already containerized at
`tests/janus/` (`Dockerfile`, `conf/`, `README.md`,
`seed-hope-passwords.sh`), and already wired into
`tests/integration/server_matrix.c` as its own matrix row. The
Janus container that runs the TLS suite is the same one that
will run the voice suite — adding the voice port (UDP base+4) is
a `tests/janus/Dockerfile` + CI-workflow port-mapping change, not
new infrastructure.

What this means for the plan:

- **Phase 8.A can start immediately**, no mock-SFU spike required.
  The capability echo + first SDP offer/answer round-trip can be
  verified against Janus from day one.
- **Phase 8.F's primary deliverable shrinks** to "wire Janus into
  the Tier 3 voice matrix and write the integration tests" — the
  Janus container, the docker-compose definition, the matrix-row
  for voice all live in the existing test infrastructure.
- **The mock SFU goes from primary test target to optional
  edge-case fixture.** See decision in §8: I lean *skip the mock
  entirely for v1*, lean on Janus for happy-path and on Tier 2
  fixture tests in `hxvoice` / `hotline-proto::voice` for the
  edge cases the mock would have covered (malformed SDP, oversized
  participant list, dropped 603, etc.). The state-machine
  property tests already cover most of what a mock would
  exercise.
- **Mobius issue tracker doesn't have a voice issue open** at the
  time of writing. So Janus is the only known server-side
  implementation today, but it's the *known good* one — and
  having one matters much more than having two.

Confirm with the VesperNet maintainers whether Janus's voice
implementation is feature-complete or still in flux (the wire
protocol is stable per the fogWraith spec, but server-side mute
enforcement, room-full handling, the access bit check, and the
recommended ~100 ms mute-debounce window are all places early
implementations can diverge). Worth pinging before we start
Phase 8.C and finding incompatibilities the hard way.

---

## 7. Gotchas worth flagging up front

- **Audio device permissions on Flatpak.** Microphone access goes
  through the `device=all` finish-arg or the (newer, sandboxed)
  `org.freedesktop.portal.Device` portal. The current
  `com.nasledov.gtkhx.yml` doesn't request either. Phase 8.B adds
  `--device=all` as the cheap path; portal-based capture is a Phase
  8.x follow-up if we care about stricter sandboxing.
- **`gtkthreads.c` recursive mutex + gstreamer-rs.** GStreamer's bus
  callbacks fire on the main loop and should be safe under the
  existing thread model, but webrtcbin's worker threads emit signals
  (`on-ice-candidate`, `on-negotiation-needed`) on arbitrary threads.
  In gstreamer-rs, `connect_local` / `connect_closure_local` is the
  main-thread-only variant; for `connect` (which can fire from any
  thread), wrap the closure body in `glib::MainContext::default().spawn(async move { … })`
  to marshal back to main before touching the state machine or the
  C-side GtkhxSession. Same discipline as the HTXF workers in
  `network.c` / `xfers.c`; documenting in `rust-glib-interop.md`
  (Phase 8.0) is part of the foothold work.
- **SDP size and Hotline framing.** `DATA_VOICE_SDP` is a Hotline
  data field with the standard `uint16` length prefix → 65535 bytes
  max. Spec says a 16-participant voice SDP is well under 10 KB.
  Add a guard rail and a `g_warning` if we ever see one larger
  than 32 KB — that's diagnostic gold for "did the server decide to
  generate a weird SDP."
- **mid label parsing.** `user-{UID}` where `{UID}` is a decimal
  uint16. Spec says no leading zeros. Be conservative — sscanf with
  bounds, reject the track on parse failure rather than asserting.
- **Renegotiation queueing.** Spec mandates server-side serialization
  (one offer in flight at a time) but tells the client to handle
  the broken-server case anyway. Don't pile pending answers — drop
  the older offer, answer only the newest.
- **PCMU at 8 kHz is bandwidth-only, not codec-CPU friendly.** μ-law
  encode/decode are cheap, but 64 kbps × N receive streams isn't
  trivial. A 16-person voice room is 960 kbps down per client.
  Surface in the UI? Probably not in v1 — the spec's room-size
  default is 16 and operators set it.
- **`gtk_hlist_compat` user list is a GtkTreeView under the hood.**
  Adding a per-row "speaking" icon means a new pixbuf column in
  `users.c`'s tree model. Doable, but it's the kind of place where
  Phase 5's `GtkColumnView` migration would have made this nicer.
  We don't block on that — just add the column where it goes
  today.
- **Access bit 55 (`accessVoiceChat`) — name it and use it.**
  `hl_access_has()` already decodes the 64-bit bitmap (bit 56
  `HL_ACCESS_READ_CHAT_HISTORY` is the in-tree precedent for a
  spec-extension bit beyond mhxd's original 0–40 range). Phase 8.A
  adds `#define HL_ACCESS_VOICE_CHAT 55` next to
  `HL_ACCESS_READ_CHAT_HISTORY` in `hl_access.h`; the chat-tab
  Voice button in Phase 8.D queries `hl_access_has(htlc->access,
  HL_ACCESS_VOICE_CHAT)` to decide greyed-out vs. interactive
  (the cap-not-echoed case hides the toolbar entirely; only the
  cap-echoed-but-access-denied case shows the greyed state). The
  128-bit extended bitmap
  (gated on `HTLC_CAP_EXTENDED_PRIV`, which we don't advertise yet)
  is a separate follow-up; voice doesn't need it.
- **Rust workspace grows by two crates + ~10 FFI shims.** `hxvoice`
  is one new crate (~600 LOC, pure state machine, no GLib /
  GStreamer / GTK deps — CI can test it without GStreamer
  installed); `hxvoice-runtime` is the other (~900 LOC, gstreamer-rs
  + glib-rs dispatch); `hotline-proto` gains a `voice` module
  (~300 LOC, wire-protocol parsers/builders) and ~4 new FFI shims
  for `rcv.c`'s wire-dispatch path. `hxvoice-runtime` exposes ~6
  opaque-handle FFI entry points (`gtkhx_voice_runtime_new` / `_free`
  / `_handle_*`). The existing `rust/meson.build` `custom_target`
  picks both crates up by listing them in `rust/Cargo.toml`'s
  workspace members. Phase 8.0 lands the new gtk-rs / gstreamer-rs
  workspace deps; 8.B onward consumes them.
- **State machine stays a no_std-friendly pure crate.** No GLib, no
  GStreamer, no GTK in `hxvoice`. The deliberate isolation lets
  `cargo test -p hxvoice` run in any CI container regardless of
  audio device or GStreamer install state; it also means
  `hxvoice-runtime` is the only crate that takes the gstreamer-rs
  dep surface. Concrete consequence: do not let GLib/GStreamer types
  creep into `hxvoice`'s public types. Events and actions stay plain
  Rust structs of primitives + byte slices + small typed enums.
- **R3.0 makes glib-rs idioms first-class in the workspace.** Phase
  8.0 is the first time we write Rust that wraps a C-owned GObject
  (`unsafe { glib::Object::from_glib_borrow(ptr) }`) and emit
  signals on it. Document the lifetime model in
  `docs/rust-glib-interop.md` (Rust holds a borrowed ref, C owns the
  GObject — do not `g_object_unref` from Rust under any
  circumstance, including `Drop`). Future contributors should not
  have to re-derive this.
- **GStreamer 1.20 floor.** That's the version webrtcbin stabilized
  in. GNOME runtime 49 (our Flatpak target) ships 1.24+, so we're
  fine. Distro packagers on Debian stable / older RHEL might lag —
  document as a build requirement.
- **Server disconnect cleanup.** Spec says the server cleans up
  automatically if the TCP control connection drops. The client side
  needs to tear down the GStreamer pipeline + free the webrtcbin
  state when `hx_connection_close` fires — Phase 8.C wires this into
  the existing close path, don't forget.

---

## 8. Open decisions to lock in before Phase 8.A

- **WebRTC stack**: GStreamer `webrtcbin` via `gstreamer-rs` (revised
  2026-06-09 — see §3). Voice controller is all-Rust: state machine
  in `hxvoice`, runtime in `hxvoice-runtime`. Phase 8.0 (R3.0
  glib-rs foothold) is the ~1-week prerequisite that lands the
  gtk-rs / gstreamer-rs crate deps and the C-GObject wrapping
  pattern. Confirm.
- **Mock SFU**: skip for v1 — Janus (already in our Tier 3 matrix
  as the TLS target) implements the spec, so the happy-path tests
  run against a real server and the adversarial cases land as
  state-machine property tests in `hxvoice`. Revisit if we hit a
  class of bug Janus can't reproduce. If we ever do build one,
  Go + Pion is the obvious pick. Confirm.
- **Janus voice-impl status check**: ping VesperNet before
  Phase 8.C to confirm server-side mute enforcement, room-full
  handling, and the access-bit check are all functional. The wire
  protocol is stable; server-side behaviour is where early impls
  diverge.
- **Toolbar location for the Voice button**: per-chat-tab toolbar
  (so each chat room has its own join button) vs. headerbar action.
  Recommend per-tab; voice is room-scoped, the headerbar is global.
  Confirm.
- **Default behaviour when joining a chat room**: auto-join voice
  vs. explicit click. Spec doesn't say; both are common patterns
  (Discord = explicit, some IRC voice extensions = auto). Recommend
  explicit, with a pref to auto-join. Confirm.
- **PTT keybind capture UI**: GTK 4 doesn't make global hotkeys
  easy. Local-to-the-window PTT is fine for v1 (it only works when
  GtkHx has focus). Recommend punting global PTT to a follow-up.
  Confirm.
- **CAP_EXTENDED_PRIV scope**: do we ship the 128-bit access decode
  alongside voice, or rely on server error toasts and decode later?
  Recommend toast-only for Phase 8; decoding is its own scoping pass.
  Confirm.

---

## 9. Effort estimate

Rough numbers, in the same units as the TLS scoping doc (calendar
weeks at evening pace; multiply by ~3 for life).

| Phase | C LOC | Rust LOC | Test LOC | Calendar |
|---|---|---|---|---|
| 8.0 — R3.0 glib-rs foothold (prereq) | ~20 | ~150 | ~150 | 1 week |
| 8.A — capability + signaling | ~120 | ~300 | ~400 | 1–2 weeks |
| 8.B — gstreamer-rs deps + bare pipeline | ~30 | ~200 | ~150 | 1 week |
| 8.C — state machine + webrtcbin runtime (all Rust) | ~50 | ~1,500 | ~800 | 3–4 weeks (the hard one) |
| 8.D — UI in chat tab | ~600 | — | ~200 | 1–2 weeks |
| 8.E — settings + PTT | ~300 | ~50 | ~100 | 1 week |
| 8.F — tests against Janus | ~150 | — | ~800 | 1 week (no mock SFU to build) |

Totals (revised): ~1,270 C LOC + ~2,200 Rust LOC + ~2,600 test LOC,
across 9–11 calendar weeks at evening pace (~27–33 weeks lifeful).
The C → Rust shift vs the old plan: ~280 C LOC moved into Rust
(`voice.c` 350 + `voice_audio.c` 200, minus ~270 C LOC retained for
the opaque-handle header + thin wire-out path + UI panels +
settings + rcv.c dispatch), Rust gains ~1,300 LOC (the
`hxvoice-runtime` crate replacing the C glue). Phase 8.0 is the
new ~1-week prerequisite.

Biggest risk is still 8.C: `webrtcbin`'s API has sharp edges around
`pad-added` race conditions and the renegotiation flow. The all-
Rust pivot mitigates this differently than the hybrid plan did —
instead of one language for the state machine + another for
dispatch, we get typed signal connections, typed
`gst::Promise::new_with_change_func` callbacks, and exhaustive
`match` on `WebRTCSignalingState`. The state machine still ships
first (testable in `cargo test -p hxvoice` with zero GStreamer dep),
and the runtime crate's loopback tests catch binding-level issues
against an in-process two-webrtcbin loopback before we even reach
Janus. Budget debugging time generously anyway; the GStreamer
surprises will land in the runtime crate, not the state machine.

---

## 10. What this is not

- **Not a P2P voice extension.** The spec explicitly rules out a
  mesh; we route everything through the server. (See spec
  §Architecture > Why Not Peer-to-Peer.)
- **Not video.** PCMU only, audio only. Video is a separate
  capability bit nobody has speced yet.
- **Not Hotline-NG.** Voice rides on the existing 1.x wire protocol
  using new TRAN opcodes — no new framing, no STARTTLS-style
  upgrade, no protocol version bump. Same model as chat-history.
- **Not coupled to TLS Phase 7.** Voice signaling rides the existing
  control channel (plaintext or TLS, whichever the user negotiated).
  Voice media is its own DTLS-encrypted SRTP session on UDP base+4
  regardless of whether the control channel is TLS — DTLS is part
  of WebRTC, the control channel's transport mode doesn't change it.

---

## 11. What this means for R3 / R4 / R5

Phase 8 (per the 2026-06-09 revision) is the leading edge of the
gtk-rs adoption that `docs/RUST-ROADMAP.md` plans across R3–R6. The
voice work doesn't *do* R3 — there's still no tokio runtime, no
`hxnet` Connection actor, no `xfers.c` / `banner.c` port — but it
establishes the glib-rs / gstreamer-rs idioms the rest of R3+ will
build on.

What R3 inherits from Phase 8:

- **The C-GObject wrapping pattern.** `unsafe { glib::Object::from_glib_borrow(ptr) }`
  with the lifetime model documented in `docs/rust-glib-interop.md`.
  R3's tokio↔GLib bridge crate (`hxbridge` per RUST-ROADMAP) uses
  the same pattern when forwarding events into the C `GtkhxSession`.
- **The `glib::MainContext::default().spawn_local` discipline.**
  Phase 8's `hxvoice-runtime` uses it for webrtcbin signal handlers;
  R3 uses it for the tokio→GLib forwarding pipeline. The Phase 8.0
  doc page is the canonical reference.
- **The Rust workspace's gtk-rs / gstreamer-rs / libadwaita
  dependency graph.** Phase 8.0 lands the deps; subsequent R3+ work
  consumes them without further `Cargo.toml` plumbing.

What changes for R4 (GtkhxSession in Rust):

- The C `GtkhxSession` adds 6 new voice-related signals in Phase 8.D
  (`voice-room-status`, `voice-track-added`, etc.). When R4 ports
  GtkhxSession to a `glib::subclass`-derived Rust GObject, the voice
  signals come along — they're nothing special, just more `Signal::builder()`
  entries in `class_init`. The `hxvoice-runtime` emit calls go from
  `glib::Object::from_glib_borrow(c_ptr).emit_by_name(…)` to direct
  method calls on the typed `GtkhxSession`, but that's a one-call-
  site mechanical change.

What changes for R5 (UI in Rust, window by window):

- `voice_panel.{c,h}` ports across when `chat.c` does (chat-tab UI).
  Calls into `hxvoice-runtime` switch from FFI shims to direct
  method calls.
- `voice_settings.{c,h}` ports across when `options.c` does. Same
  story.
- The `voice.h` opaque-handle header and the FFI shim layer in
  `hxvoice-runtime::ffi` both delete when no C UI is left.

What doesn't change ever: `hxvoice` stays `no_std`-friendly with
pure typed data in/out. That's load-bearing for keeping CI's
state-machine test runs cheap.

The original §11 framed gstreamer-rs adoption as a future migration
that would replace C glue. The 2026-06-09 revision collapses that
distinction — we ship Phase 8 already using gstreamer-rs, the
"migration" never happens because the C glue never gets written.

---

## 12. Suggested next concrete step

The 2026-06-09 revision changed the first step. **Phase 8.0 — R3.0
glib-rs foothold** is now the gating work: ~1 week to land the
gtk-rs / gstreamer-rs / libadwaita workspace deps and document the
C-GObject wrapping pattern. Without it, 8.B+ doesn't have the
machinery to drive `gstreamer-rs`.

Pre-flight items in parallel with 8.0:

1. **Confirm Janus voice impl status** with the VesperNet
   maintainers — wire protocol vs. server-side behaviour
   (mute enforcement, access bit, room-full). One short ping,
   not a blocker for starting 8.0 or 8.A.
2. **Extend the Janus test container** to expose UDP base+4
   alongside the existing TCP control + TLS ports. Edit
   `tests/janus/Dockerfile` (and `tests/janus/conf/` if voice
   needs a server-side enable flag), then mirror the port in the
   CI service-container block in `.github/workflows/tests.yml`.

Phase 8.A can interleave with 8.0 — the wire-protocol Rust code
(`hotline-proto::voice` module + the `gtkhx_proto_parse_voice_*`
FFI shims) lives entirely in `hotline-proto` and doesn't depend on
glib-rs. Land 8.A's wire scaffolding while 8.0's glib-rs foothold
shakes out, then 8.B builds on both. First user-visible milestone is
still 8.A: a debug toggle that fires Join (600) and we see the 602
SDP offer come back in the proto trace, parsed into a structured
event by Rust. No GStreamer, no UI.

Branch sequencing:

1. `claude/voice-phase-0-glib-foothold` — adds the gtk-rs /
   gstreamer-rs / libadwaita workspace deps, lands `hxbridge`
   wrapping shim, docs the pattern. Merges to main first.
2. `claude/voice-phase-a` — `hotline-proto::voice` module,
   capability bit, rcv.c dispatch with debug-only logging. Can
   open in parallel with 8.0 once the rebase target is clear.
3. `claude/voice-phase-b` — `hxvoice-runtime` skeleton with a
   loopback pipeline. Depends on both above being on main.
4. `claude/voice-phase-c` — the hard one. State machine in
   `hxvoice` + full webrtcbin dispatch in `hxvoice-runtime`.
