# Voice Chat Extension — Scoping Notes for GtkHx

Status: scoping draft, no code yet. Written 2026-06-09 against the
spec at
<https://github.com/fogWraith/Hotline/blob/525e94e2208dc2ffb1ed65d69d681c7fd356e169/Docs/Protocol/Capabilities-Voice.md>
(commit `525e94e`, 2026-04-05 — initial publication of the voice
spec). Re-pin to a newer SHA if the upstream doc moves before
implementation starts.

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

## 3. The WebRTC stack decision — GStreamer `webrtcbin`

This is the single biggest decision in the plan. The spec is written
assuming you can call an off-the-shelf WebRTC library and get the
SDP/ICE/DTLS/SRTP/RTP behaviour for free; the question is *which*.

| Option | Pros | Cons |
|---|---|---|
| **GStreamer `webrtcbin` (gst-plugins-bad)** *(picked)* | GLib-native API, plays well with GtkHx's existing main loop and thread model. Audio I/O comes for free via `autoaudiosrc` / `autoaudiosink` (PipeWire / PulseAudio / ALSA automatic). DTLS, ICE, RTP, RTCP, SRTP all handled. PCMU element built in (`mulawenc` / `mulawdec`). Already used by GNOME-shipped voice/video apps. Active upstream development. LGPL. | Big dep — `gstreamer-1.0`, `gst-plugins-base`, `gst-plugins-bad` (≥ 1.20 for stable `webrtcbin`), `gst-plugins-good`. Flatpak runtime already ships them; distro packaging adds a couple Recommends. The webrtcbin API is non-trivial to drive (pad-added signals, SDP munging callbacks). |
| **libdatachannel** (paullouisageneau/libdatachannel) | MIT. Single C++ library, C API binding. ~25k LOC, builds standalone. No GLib runtime to interact with. | Audio I/O is *not* included — we'd need PortAudio or miniaudio as a separate dep, plus a μ-law encoder, plus a thread that pumps RTP into libdatachannel's send queue. Three layers of code we'd write that GStreamer gives us for free. |
| **webrtc-rs** | Pure Rust. Could ride the existing `hotline-proto` crate path. MIT. | Heavy crate tree (~150 transitive deps). Still no audio I/O. Same "we'd have to wire up capture/playback ourselves" problem as libdatachannel. The R2 Rust integration is small and isolated today; this would blow that up. |
| **libwebrtc** (Google) | Reference impl. | Build is a nightmare (depot_tools, gn, ninja, ~hour first build). Vendored deps fight everything. Disqualified. |
| **Pion** | Spec author's reference. | Go. Disqualified for a C/GTK client. |

**Pick: GStreamer `webrtcbin`.** The decisive factor is that
GStreamer gives us audio capture + audio playback + μ-law encode/
decode + RTP packetization + WebRTC peer connection in one
dependency, with GLib idioms throughout. Every other option requires
us to write three layers of glue.

**C bindings now, gstreamer-rs later.** The Rust bindings
(`gstreamer-rs`, including `gstreamer-webrtc`) are mature and are
the obvious long-term home for the voice controller. We're not
adopting them in Phase 8 because doing so requires plumbing that
properly belongs to Phase R3: a `glib::MainContext` bridge for
Rust-side async work, a Rust→C `g_signal_emit_by_name` shim for
emitting on the C `GtkhxSession` GObject, and the first tokio
runtime in the tree. None of those are blockers individually, but
piling them into Phase 8 would expand its scope materially and
couple voice's schedule to a tangle of R3 prerequisites. The clean
boundary is: Rust owns the *state machine* (decisions, queues,
typed events / actions), C owns the *GStreamer instances*
(`GstElement*`, `GstPipeline*`, `GstWebRTCBin*`) using the
existing C bindings. When R3-R4 lands the tokio runtime + Rust
GtkhxSession, the voice controller is a strong candidate to swap
the C glue out for `gstreamer-rs` — the state machine slides
across unchanged because it never directly held a GstElement
pointer in the first place. See §4 for the split, §11 for the
migration path.

A representative pipeline for one voice session (one send + N-1
receive tracks bundled on one transport):

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

The per-receive-leg add/remove is driven by `pad-added` /
`pad-removed` signals on the webrtcbin element, which fire when the
SDP renegotiation completes. Standard GStreamer pattern.

---

## 4. New code layout — hybrid Rust/C

Two pieces are Rust, three pieces are C. The split is drawn along
the line that maximizes Rust's value (state-machine correctness,
exhaustive matching, wire-format parsing) while not requiring any
infrastructure that R3 hasn't built yet (no tokio, no glib-rs
main-context bridge, no Rust→C signal emit, no Rust ownership of
`GstElement*`). Existing chat-history / tracker-v3 / news pattern
extends naturally to the wire side; the state-machine module is
new but uses a pure-function shape that needs no glue.

### Rust — wire-protocol layer, in `rust/crates/hotline-proto/`

| Addition | Role |
|---|---|
| `src/voice.rs` (new module) | Typed representation of voice transactions: `JoinRequest`, `LeaveRequest`, `SdpAnswer`, `IceCandidate`, `MuteToggle`, `Participant`, `MidLabel`. Builders return `HxChunk` arrays like the rest of `build::`; parsers return `#[repr(C)]` out-structs with `_Static_assert`-mirrored layouts. |
| `src/voice.rs::sdp` submod | Minimal SDP shape parser — just enough to find `a=mid:user-{UID}` labels, the `a=group:BUNDLE` list, the disabled `m=audio 0` lines for left-participant slots, and confirm PCMU is offered. We don't reimplement SDP; the heavy lifting is GStreamer's job. We only extract the bits the UI needs. |
| `src/voice.rs::ice` submod | Tiny JSON build/parse for the `RTCIceCandidateInit` dict (`candidate`, `sdpMid`, `sdpMLineIndex`, `usernameFragment`). Use `serde_json` (already in the workspace via tracker-v3 work) or hand-roll given how small the schema is. |
| `messages.rs` additions | New opcode variants (`VoiceJoinRoom = 600`, etc.) and field-tag variants (`VoiceSdp = 0x01F5`, etc.) added to the `#[non_exhaustive]` enums. |
| `ffi.rs` additions | ~8–10 new `#[no_mangle] extern "C"` shims: `gtkhx_proto_build_voice_join`, `_build_voice_answer`, `_build_voice_ice`, `_build_voice_mute`, `_parse_voice_participants`, `_parse_voice_mid_label`, `_parse_voice_sdp_summary`, `_parse_voice_ice_json`. |

### Rust — voice session state machine, in a new crate

| Addition | Role |
|---|---|
| `rust/crates/hxvoice/` (new crate, `no_std`-friendly) | Pure state machine. One `SessionMachine` per active voice session. Holds: current state enum (`Idle` / `JoinSent` / `OfferPending` / `Connecting` / `Connected` / `Leaving`), pending-renegotiation queue, mid→user_id map, mute flag, current participant list. Single entry point `step(&mut self, Event) -> Vec<Action>`. **No I/O, no GStreamer, no GLib types** — just typed data in, typed data out. The C controller in `voice.{c,h}` pumps events in and dispatches the returned actions. |
| `src/event.rs` | Typed inbound events: `JoinRequested { cid }`, `LeaveRequested { cid }`, `MuteToggleRequested { muted }`, `SdpOfferReceived { sdp }`, `IceCandidateReceived { json }`, `EndOfRemoteCandidates`, `ParticipantsUpdated { entries }`, `ServerTaskError { code, text }`, `WebrtcPadAdded { mid }`, `WebrtcPadRemoved { mid }`, `WebrtcAnswerCreated { sdp }`, `WebrtcLocalIceGathered { json }`, `WebrtcConnectionStateChanged { state }`, `Timeout { kind }`. |
| `src/action.rs` | Typed outbound actions: `SendWireFrame { opcode, chunks }`, `SetRemoteDescription { sdp }`, `CreateAnswer`, `SetLocalDescription { sdp }`, `AddRemoteIce { json }`, `StartReceivePipeline { mid, user_id }`, `StopReceivePipeline { mid }`, `SetSendPipelineMute { muted }`, `EmitSignal { kind, payload }`, `ArmTimer { kind, ms }`, `CancelTimer { kind }`, `TearDown`. |
| `src/state.rs` | The `SessionMachine` itself. Pure `match (state, event)` impl. The hard cases the spec calls out — renegotiation serialization, dropping stale offers, implicit-leave when joining a second room, ICE/DTLS/media timeouts, mid-slot reuse after a participant leaves — are all enforced here, not in the C controller. |
| `tests/` | Property-style state-machine tests: every event in every state has a defined transition; renegotiation queue under all 4 combinations of (pending-offer, new-offer-arrives); spec's annotated lifecycle (join → renegotiate-for-late-arriver → leave) replayed as event sequences. This is where the spec's defensive notes (§Renegotiation Flow, §Session Timeout and Failure) become assertions. |

`hxvoice` is a separate crate, not a submodule of `hotline-proto`,
because it depends on `hotline-proto`'s typed messages (one-way:
hxvoice → hotline-proto) and because keeping it isolated lets it
stay `no_std`-friendly with zero GLib / GStreamer dependencies.
That isolation is what makes the future gstreamer-rs migration
cheap — `hxvoice` doesn't change, only the glue that pumps events
into it.

### C — GStreamer glue, dispatch, UI

| File | Role | Rough LOC |
|---|---|---|
| `src/hotline.h` additions | 7 opcodes (`HTLC_HDR_VOICE_JOIN` 600 … `HTLC_HDR_VOICE_MUTE` 606), 5 data IDs (0x01F5–0x01F9). `#define`s only; the canonical typed definitions live in Rust. Same dual-define convention chat-history already uses. | ~50 |
| `src/hotline_proto.h` additions | `extern` declarations for the new FFI shims, paired with `_Static_assert(sizeof(gtkhx_proto_voice_participant) == N, ...)` for each out-struct. | ~50 |
| `src/voice.{c,h}` | Thin GStreamer / GLib glue. Holds the per-session `GstPipeline*`, `GstWebRTCBin*`, and an opaque `HxVoiceSession*` handle into the Rust state machine. Three responsibilities: (1) translate C-side events (UI click, rcv_task result, webrtcbin signal, g_timeout fire) into Rust `Event`s and call `hxvoice_step`; (2) interpret the returned `Action` array — `SetRemoteDescription` calls `gst_webrtc_bin_set_remote_description`, `SendWireFrame` calls `hlwrite_chunks`, `EmitSignal` calls `g_signal_emit_by_name` on `GtkhxSession`, etc.; (3) own the GStreamer signal callbacks (`pad-added`, `on-ice-candidate`, `on-negotiation-needed`) and marshal them through `g_idle_add` back into Rust events. **No protocol decisions here. No "should I queue this offer" logic. Pure transport.** | ~350 |
| `src/voice_audio.{c,h}` | Audio device enumeration + GStreamer source/sink element factories. Handles "no mic" graceful degradation (listen-only). | ~200 |
| `src/voice_panel.{c,h}` | UI: per-chat-tab voice toolbar (Join/Leave/Mute/PTT) + participant speaker indicators in the user list. Attaches to the AdwTabPage for each cid. | ~400 |
| `src/voice_settings.{c,h}` | AdwPreferencesPage for input/output device, default-muted, PTT keybind, "auto-join voice when joining a chat room." | ~200 |
| `src/rcv.c` additions | `rcv_task_voice_*` for 600/601/603/606 replies. Switch arms for 602/604/605 server-initiated notifications. Each calls a Rust parser, then pumps the typed result into the state machine via `voice.c`. | ~200 |
| `src/gtkhx_session.{c,h}` additions | New signals: `voice-room-status`, `voice-track-added`, `voice-track-removed`, `voice-mute-changed`, `voice-state-changed`, `voice-error`. Model→view bridge mirrors the existing taxonomy. | ~100 |

The C controller is meaningfully smaller (~350 vs the ~500 in the
all-C draft) because the state-machine logic — which is where the
bug surface area concentrates — moved to Rust. The C is now nearly
pure transport: take an event, call `hxvoice_step`, walk the
action list with a switch.

Total roughly 1.55 k C LOC + ~900 Rust LOC + tests. FFI surface
grows by ~10 shims in `hotline-proto::ffi` plus a small handful in
`hxvoice::ffi` (opaque session pointer, `_new`, `_free`, `_step`,
event/action marshalling — about 6 functions and a couple of
`#[repr(C)]` structs).

The Hotline 1.x wire-protocol additions are pure: 7 new TRAN
opcodes (600–606) and 5 new data fields (0x01F5–0x01F9). They don't
touch any existing parse paths, don't shift any existing IDs, and
are gated behind the capability echo — legacy servers and legacy
clients are completely unaffected. Same property as every other
fogWraith extension we've already shipped.

### Why split the SDP / ICE work between Rust and GStreamer

GStreamer parses SDP into `GstSDPMessage` and ICE candidates into
`GstWebRTCICE` internally. We don't need a second full SDP parser
in Rust. What we *do* need from Rust:

- **mid label parsing** — `user-{UID}` → `u16 user_id` with strict
  validation (no leading zeros, in 1..=65535, etc.). This is wire-
  format-shaped, belongs in `hotline-proto`.
- **Participant blob walking** — the packed 6-byte-per-entry
  `DATA_VOICE_PARTICIPANTS` binary is exactly the shape `hotline-
  proto::parse` already handles for the file-list and history walkers.
- **ICE JSON build / parse** — the inner JSON payload is wire data
  carried inside a Hotline data field. Rust owns that boundary.
- **SDP summary** — extract just the `a=mid` list and the BUNDLE
  group from the SDP blob so the C controller can sanity-check what
  GStreamer is about to set as the remote description. Cheap defensive
  parse, not a full SDP implementation.

The full SDP and full ICE handling stays in GStreamer where they
belong. The C controller drives `GstSDPMessage` / `GstWebRTCICE`
directly — those aren't wire types, they're media-stack types.

---

## 5. Sub-phasing

Each sub-phase ends on a clean build + passing tests. Same discipline
as the TLS phasing — don't pile up "and these other 800 lines also
need to land before anything works."

### Phase 8.A — Capability advertisement + signaling scaffolding

Goal: server sees us as voice-capable, we can send and receive every
600–606 opcode, but no media flows yet. **All new wire code lands in
Rust.**

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
6. New thin `src/voice.{c,h}` controller that just calls into the
   Rust builders + emits the chunks through `hlwrite_chunks`. No
   GStreamer yet.
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

### Phase 8.B — GStreamer dependency + bare pipeline

Goal: a GStreamer pipeline that captures from the mic, encodes to
μ-law, and renders silence on playback — proves the dep is wired,
the audio devices are reachable, the build works on Flatpak, none
of the protocol changes have moved.

Work:
1. `meson.build` additions: `dependency('gstreamer-1.0', version:
   '>=1.20')`, `gstreamer-app-1.0`, `gstreamer-audio-1.0`,
   `gstreamer-webrtc-1.0`. Flatpak `com.nasledov.gtkhx.yml`
   confirms the GNOME runtime already ships them; if not, add
   `org.freedesktop.Sdk.Extension.gst-plugins-bad` lines.
2. `gst_init` in `gtkhx.c::main` before `gtk_init`.
3. New `src/voice_audio.{c,h}` — enumerate input devices via
   `GstDeviceMonitor`, factory functions that return configured
   source/sink elements.
4. Smoke test (`tests/integration/test_voice_loopback.c`,
   Tier 1) that builds a `autoaudiosrc ! mulawenc ! mulawdec !
   fakesink` pipeline, runs it for 100 ms, checks state went to
   PLAYING. Catches "GStreamer is here, audio is accessible" on CI.

Exit criteria: clean build with new deps. Loopback test passes
locally and in the Tier 1 suite. No protocol changes in this
phase — voice transactions still scaffolded only from Phase A.

Branch: `claude/voice-phase-b`.

### Phase 8.C — state machine (Rust) + webrtcbin glue (C)

Goal: send-only voice works against Janus. We can hear a remote
participant but they can't hear us yet. (Or vice versa — pick
one direction to chase down completely first.)

This is the phase where the hybrid split earns its keep. The state
machine lands first, fully tested in pure Rust against scripted
event sequences from the spec's annotated lifecycle examples; the
GStreamer glue lands second and is reduced to "translate
GStreamer signal → Rust event, walk action list → call GStreamer."

Work:
1. New `rust/crates/hxvoice/` — pure `SessionMachine` per §4. Lands
   with property tests covering every event-in-every-state
   transition, plus replays of the spec's "B joins room with A
   already in voice" and "B leaves" sequences as event traces.
   Builder, FFI shims (`hxvoice_session_new`, `_free`, `_step`),
   `#[repr(C)]` event/action marshalling structs. Layout pinned
   with the same `_Static_assert` + `const _: ()` pattern R2
   already uses.
2. New `src/voice.{c,h}` — transport glue. Holds the per-cid
   `GstPipeline*`, `GstWebRTCBin*`, the opaque `HxVoiceSession*`,
   and a small action-dispatch switch. Roughly one C function per
   `Action` variant (`do_set_remote_description`,
   `do_send_wire_frame`, `do_emit_signal`, …); roughly one
   pump-event helper per `Event` source (`pump_from_rcv_task`,
   `pump_from_webrtcbin_signal`, `pump_from_ui`, …).
3. Wire the SDP offer flow: 602 arrives → rcv_task parses with
   Rust → `pump_from_rcv_task(SdpOfferReceived)` → state machine
   returns `[SetRemoteDescription, CreateAnswer]` → C calls
   `gst_webrtc_bin_set_remote_description`, then
   `g_signal_emit_by_name(webrtcbin, "create-answer", ...)` →
   answer-created callback fires → `pump_from_webrtcbin_signal(
   WebrtcAnswerCreated)` → state machine returns
   `[SetLocalDescription, SendWireFrame(603, …)]`.
4. Wire the ICE flow with the same shape: `on-ice-candidate` →
   `WebrtcLocalIceGathered` event → `SendWireFrame(604)` action;
   incoming 604 → `IceCandidateReceived` event → `AddRemoteIce`
   action; empty-candidate end-of-candidates handled with a
   distinct `EndOfRemoteCandidates` event so the state machine
   knows it has a complete remote candidate set.
5. Track-to-user mapping in Rust: `WebrtcPadAdded { mid }` event →
   the machine cross-references its mid→UID map (populated from
   the SDP summary parse) and returns
   `StartReceivePipeline { mid, user_id }`. C builds the
   `rtppcmudepay ! mulawdec ! … ! autoaudiosink` sub-pipeline
   rooted at the new pad.
6. Renegotiation serialization in the state machine — `OfferPending`
   state queues a second `SdpOfferReceived`, transitions emit
   `[]` until the first answer flushes, then drains. The C side
   doesn't know about the queue at all.
7. Timeouts (spec §Session Timeout and Failure) as `ArmTimer` /
   `CancelTimer` actions plus a `Timeout { kind }` event the C
   side fires from `g_timeout_add_seconds` callbacks. ICE failure,
   DTLS failure, media timeout all funnel through the same shape.

Exit criteria: against Janus, join an empty room → invite a second
client (or use the Janus admin tooling to inject a participant) →
SDP offer received → answer + ICE complete → audio flows. Rust
state-machine test count >50.

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
- **`gtkthreads.c` recursive mutex + GStreamer.** GStreamer's bus
  callbacks fire on the main loop and should be safe under the
  existing thread model, but webrtcbin's worker threads emit signals
  (`on-ice-candidate`, `on-negotiation-needed`) that we'll be
  marshalling back to main via `g_idle_add` — same pattern as the
  HTXF workers in `network.c` / `xfers.c`. Don't be tempted to send
  604/606 directly from the webrtcbin callback thread; route through
  the idle.
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
- **Rust workspace grows by one crate + ~16 FFI shims.** `hxvoice`
  is the new crate (~600 LOC, the state machine); `hotline-proto`
  gains a `voice` module (~300 LOC, the wire-protocol parsers/
  builders) and ~10 new FFI shims. The state machine adds ~6 more
  FFI entry points (`hxvoice_session_new` / `_free` / `_step` plus
  event/action marshalling). The existing `rust/meson.build`
  `custom_target` picks up the new crate by listing it in
  `rust/Cargo.toml`'s workspace members and recompiles whenever
  any `.rs` under `rust/` changes — no special build-system work.
- **State machine is a no_std-friendly pure crate.** No GLib, no
  GStreamer, no GTK in `hxvoice`. This is what makes the future
  gstreamer-rs migration cheap: when the C glue eventually swaps
  to a Rust controller (post-R3, when there's a tokio runtime and
  a glib-rs main-context bridge), `hxvoice` keeps its shape
  exactly — only the *event sources* and *action dispatchers*
  change from `unsafe extern "C"` to safe glib-rs / gstreamer-rs
  calls. Concrete consequence for Phase 8: do not let
  GLib/GStreamer types creep into `hxvoice`'s public types, even
  via FFI. Events and actions are plain `#[repr(C)]` structs of
  primitives + byte slices.
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

- **WebRTC stack**: GStreamer webrtcbin via the C bindings, with
  the hybrid Rust state machine (§4) in `hxvoice`. Migration to
  `gstreamer-rs` deferred to after R3-R4 per §11. Confirm.
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
| 8.A — capability + signaling | ~150 | ~300 | ~400 | 1–2 weeks |
| 8.B — GStreamer pipeline | ~250 | — | ~100 | 1 week |
| 8.C — state machine + webrtcbin glue | ~350 | ~600 | ~700 | 3–4 weeks (the hard one) |
| 8.D — UI in chat tab | ~600 | — | ~200 | 1–2 weeks |
| 8.E — settings + PTT | ~300 | — | ~100 | 1 week |
| 8.F — tests against Janus | ~150 | — | ~800 | 1 week (no mock SFU to build) |

Biggest risk is 8.C: webrtcbin's API has sharp edges around
`pad-added` race conditions and the renegotiation flow. The hybrid
split mitigates this — the state machine is fully testable in
isolated Rust before any GStreamer glue exists, so the C side
ships against a known-good control flow. Budget debugging time
generously anyway; the glue layer is where the GStreamer
surprises will land.

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

## 11. Future migration to `gstreamer-rs`

Long-term, the voice controller should be all Rust — `gstreamer-rs`
+ `gstreamer-webrtc-rs` owning the `GstWebRTCBin` outright, with
the C glue gone. This was the path not taken in Phase 8 because
the prerequisites (tokio runtime, `glib::MainContext` interop,
Rust-side `g_signal_emit` on the C GtkhxSession GObject) belong to
Phase R3-R4 and bundling them into voice would inflate the phase.
The hybrid split in §4 is the intermediate step that gets us most
of the value now while preserving cheap migration later.

The migration plan, once R3-R4 has set the table:

1. **`hxvoice` doesn't change.** This is the whole point of keeping
   it free of GLib / GStreamer types. The state machine, the
   property tests, the action/event marshalling all stay byte-for-
   byte identical. The handful of FFI shims in `hxvoice::ffi`
   become safe-Rust functions in `hxvoice::lib` directly callable
   from the Rust controller.
2. **A new `rust/crates/hxvoice-runtime/`** wraps `hxvoice` with
   the side-effectful layer: holds the `gstreamer::Pipeline`, the
   `gstreamer_webrtc::WebRTCBin`, the per-cid receive pipelines.
   Subscribes to webrtcbin signals using gstreamer-rs's typed
   `connect_*` methods (no more `g_signal_connect` with a void*
   callback and manual data lifetime tracking). Pumps events into
   `SessionMachine::step` and dispatches the returned actions.
3. **C `src/voice.{c,h}` deletes.** Its rcv_task entry points
   become FFI calls into `hxvoice-runtime`. Its GStreamer
   pipeline ownership moves to Rust. The UI side (`voice_panel.c`)
   keeps talking to it through the same `GtkhxSession` signals,
   which by then will themselves be Rust-side per R4.
4. **`voice_audio.c` either deletes or becomes a 50-LOC wrapper**
   for `GstDeviceMonitor` enumeration that the settings page
   consumes. The device-monitor API is also available through
   gstreamer-rs, so it likely just disappears.

Order matters: this migration sits *behind* R3 (tokio + main-
context bridge) and *behind* R4 (Rust GtkhxSession, so action
dispatch can emit signals natively). It does not require waiting
for R5's UI migration — the C UI side can keep talking to a Rust
controller across the existing GObject signal boundary.

Estimated cost when the time comes: the state machine is already
there, so this is roughly 600–800 LOC of `hxvoice-runtime` Rust
replacing ~550 LOC of `voice.c` + `voice_audio.c` glue. Net code
change is small; the win is removing manual GObject lifetime
management for `GstWebRTCBin` callbacks and getting
gstreamer-rs's typed pad / message / promise APIs in exchange.

The decision to go hybrid for Phase 8 is not a vote against
gstreamer-rs; it is a vote against doing R3's prerequisite work
inside a feature phase. The state-machine-in-Rust split is
explicitly designed to make this migration a swap, not a rewrite.

---

## 12. Suggested next concrete step

Janus already implements the voice extension, so unlike the chat-
history rollout there's no gating spike — we can start Phase 8.A
immediately against the existing Tier 3 Janus container.

Two pre-flight items:

1. **Confirm Janus voice impl status** with the VesperNet
   maintainers — wire protocol vs. server-side behaviour
   (mute enforcement, access bit, room-full). One short ping,
   not a blocker for starting the Rust scaffolding.
2. **Extend the Janus test container** to expose UDP base+4
   alongside the existing TCP control + TLS ports. Edit
   `tests/janus/Dockerfile` (and `tests/janus/conf/` if voice
   needs a server-side enable flag), then mirror the port in the
   CI service-container block in `.github/workflows/tests.yml`.

Then Phase 8.A — Rust opcode/field enums in `hotline-proto`,
builders/parsers, capability bit advertised, `voice.{c,h}` stub.
First milestone: a debug toggle that fires Join (600) and we see
the 602 SDP offer come back in the proto trace, parsed into a
structured event by Rust. No GStreamer, no UI.
