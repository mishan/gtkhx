# Voice Chat Extension — Status Doc for GtkHx

Originally written 2026-06-09 as a scoping draft. **Mostly shipped as
of mid-June 2026** — end-to-end DTLS-SRTP voice against Janus
VoiceRoom works. This doc is now a status tracker plus design
reference; the scoping rationale is retained where it explains why a
choice was made.

Spec pinned at
<https://github.com/fogWraith/Hotline/blob/525e94e2208dc2ffb1ed65d69d681c7fd356e169/Docs/Protocol/Capabilities-Voice.md>
(`525e94e`, 2026-04-05).

## At-a-glance status

| Phase | Description | Status | Branch / commit |
|---|---|---|---|
| 8.0 | R3.0 glib-rs foothold (`hxbridge` crate) | **Shipped** | merged |
| 8.A | Capability bit + voice opcodes + Rust wire layer | **Shipped** | `1481da0` |
| 8.B | GStreamer dep + bare pipeline (loopback test) | **Shipped** | `7178b7f` |
| 8.C | State machine (`hxvoice`) + webrtcbin runtime (`hxvoice-runtime`) | **Shipped** | multiple |
| 8.D | Voice toolbar in chat tab + runtime wire-up + signal bridge | **Shipped** | multiple |
| 8.E | Settings → Voice device pickers | **Shipped** | merged from `claude/voice-phase-e-devices` |
| 8.F | Tier 3 integration matrix vs Janus | **Shipped** | merged from `claude/voice-phase-f-tests` |
| 8.G | Per-uid voice indicators in user list | **Shipped** (in-voice + muted) | `claude/voice-speaker-indicator` |
| follow-ups | PTT, "Start muted" toggle, "Auto-join", wedge-deadline hardening, real-VAD speaker detection | **Mixed** — wedge + soft-Media shipped; PTT / Start-muted / Auto-join / real-VAD open | `claude/voice-wedge-deadline` (wedge), `claude/voice-speaker-indicator` (soft-Media) |

What also shipped that wasn't in the original plan:

- **Wedge watchdog** (`claude/voice-wedge-deadline`): the original soft-
  IceConnectivity / soft-Dtls behaviour can leave a session in
  `Connecting` forever if webrtcbin never reports `Connected` AND no
  RTP arrives. A runtime-owned 60 s watchdog reads an always-on
  `Arc<AtomicU64>` RTP counter from the receive bin's depay sink and
  injects `Timeout::WedgeDeadline` if the counter doesn't advance.
  Full design lives inline in §5 below ("Unplanned: wedge watchdog").
- **Signal bridge** (`voice-phase-d-signal-bridge`): the runtime
  dispatches `state-changed` and `mute-changed` signals back into the
  C UI via the `SignalCallbacks` FFI struct, so `voice_panel.c`
  reflects state-machine state instead of optimistic UI.
- **`CallbackBackend`** (`voice-phase-d-bridge-backend`): replaces the
  earlier `NoopBackend` for outgoing wire frames. The state machine
  is now the single source of truth for 603 / 604 / 606 emission;
  the C side no longer double-sends.
- **Soft Media timeout + Error toast bridge**
  (`claude/voice-speaker-indicator`): the spec's `(Connected,
  Timeout::Media) → fail()` arm was softening sessions to LEAVING after
  30 s of quiet, which was the wrong call once servers started defaulting
  new joiners to MUTED — a fully-quiet room is the common case, not a
  failure. The arm now emits an Error toast and stays in Connected
  (same shape as the DTLS / IceConnectivity softenings). To make that
  toast visible, `SignalCallbacks` grew an `error` slot and
  `voice_panel.c` routes the message through `toolbar_show_toast`.
- **Per-uid voice indicator column** (`claude/voice-speaker-indicator`):
  the user list now shows a small icon per row — empty when the user is
  not in voice, dim speaker when they're in voice, mic-disabled when
  they're muted. Driven by a new GObject `HxVoiceModel` keyed on uid,
  ingested from the existing `VOICE_PARTICIPANTS` blob path (no new
  wire) and updated synchronously. The "actively speaking" arm is
  scaffolded end-to-end (per-pad RTP counter in `hxvoice-runtime`,
  `SignalKind::SpeakerChanged`, `HxVoiceModel::set_speaking`) but
  deliberately demoted to IN_VOICE in render until real VAD ships —
  see §12 step 7 below for the rationale and revert path.

---

## 1. What the spec actually requires of the client

Stripping it down to the client's contract:

- **Capability negotiation.** Set bit 2 (`0x0004`) in
  `HTLC_DATA_CAPABILITIES` (0x01F0) during LOGIN. If the server
  doesn't echo it, voice UI is hidden. Wired in `network.c` (legacy
  LOGIN) and `rcv.c` (HOPE Step-2 LOGIN). **Shipped.**
- **Seven new transaction opcodes** in the 600–606 range.
  **Shipped** in `hotline-proto::voice` + C `#define`s in
  `src/hotline.h`.
- **Five new data field IDs** in the 0x01F5–0x01F9 range, fitting
  between Large-File and the chat-history block. **Shipped.**
- **A WebRTC peer connection** to the server on UDP base+4: SDP
  offer/answer over the existing TCP control channel, ICE candidates
  trickled both ways, DTLS handshake, SRTP-encrypted RTP carrying
  PCMU audio. The server is *always* the offerer. **Shipped via
  `gstreamer-rs` + `gstreamer-webrtc-rs`** in `hxvoice-runtime`.
- **Track-to-user mapping** via SDP `a=mid:user-{UID}` labels.
  **Shipped** — `hotline-proto::voice::sdp` extracts the labels;
  state machine maps mid → user_id; runtime asks state machine for
  the user_id on `pad-added`.
- **One room at a time.** Joining voice in B implicitly leaves A.
  **Shipped** — state machine's `JoinRequested` handling drives the
  implicit leave.
- **Mute** as a server-side enforced flag (606). Push-to-talk is just
  rapid mute/unmute from the client's side. **Mute shipped, PTT
  open.**
- **Disconnect = automatic leave.** Server cleans up if the TCP
  control connection drops. **Shipped** — runtime teardown on
  session free + `Action::TearDown` drives the pipeline back to
  Null.
- **PCMU only.** **Shipped** — `gstreamer-webrtc-rs` defaults +
  codec-preferences pinning in the `on-new-transceiver` handler.

---

## 2. Current state of GtkHx (post-Phase 8)

- `src/hotline.h:328` — `HTLC_CAP_VOICE 0x0004` is now asserted in
  both LOGIN masks. `htlc->caps & HTLC_CAP_VOICE` gates the voice
  panel visibility.
- `src/hl_access.h` — `HL_ACCESS_VOICE_CHAT` (bit 55) is defined and
  queried via `hl_access_has()`. The voice panel uses it for the
  greyed-vs-interactive distinction.
- **User list is `GtkColumnView`** (Phase 5 migration shipped — see
  `docs/users-columnview-scoping.md`). Adding a per-row speaker
  indicator is a column-builder change, not a tree-model change.
  The original scoping note flagged `gtk_hlist_compat` as a wart;
  the wart is gone.
- `src/tracker_v3_meta.c` decodes `supports_voice` (0x0305) and
  the tracker details popover surfaces it.
- **Audio I/O** is no longer "sound.c output only". `hxvoice-runtime`
  owns full capture + playback via `gstreamer-rs` `autoaudiosrc` /
  `autoaudiosink` with Settings → Voice device overrides.
- **Threading:** unchanged in shape (`gtkthreads.c` recursive mutex +
  custom poll wrapper). webrtcbin's worker threads emit signals onto
  arbitrary threads; the runtime marshals via
  `glib::MainContext::default().invoke` and the
  `MAIN_THREAD_RUNTIMES` thread-local registry. The pattern is
  documented in `docs/rust-glib-interop.md`.

---

## 3. The WebRTC stack — `gstreamer-rs` (shipped)

This was the single biggest decision in the plan: `gstreamer-rs` +
`gstreamer-webrtc-rs` ended up the right call. The C bindings + hybrid
state machine option in the original alternatives table was
deprecated before any code landed; the runtime is all-Rust from the
ground up.

In practice the gstreamer-rs typed API delivered what the scoping
doc claimed:

- Typed `WebRTCSDPType` / `WebRTCSignalingState` / `WebRTCPeerConnectionState`
  instead of stringly-typed `g_object_get`.
- `connect_pad_added` as a typed signal method, captured by closure
  with a clean lifetime instead of `void*` user-data.
- `gst::Promise::with_change_func` for `create-answer` — the
  scoping doc's pattern is what `hxvoice-runtime/src/runtime.rs`
  actually uses. (The original scoping draft cited
  `new_with_change_func`; the current gstreamer-rs API name is
  `with_change_func`, which is what shipped.)
- `gst_sdp::SDPMessage::parse_buffer` for SDP parsing.

The pipeline ended up matching the original sketch:

```
webrtcbin name=webrtc bundle-policy=max-bundle

  # send leg
  autoaudiosrc ! audioconvert ! audioresample
              ! audio/x-raw,rate=8000,channels=1
              ! mulawenc ! rtppcmupay
              ! application/x-rtp,media=audio,encoding-name=PCMU,payload=0
              ! webrtc.

  # receive legs added dynamically on webrtc.pad-added
  webrtc. ! rtppcmudepay ! mulawdec
         ! audioconvert ! audioresample ! autoaudiosink
```

Bin construction lives in `hxvoice-runtime/src/audio.rs`; the
streaming-thread receive-leg linking lives in
`runtime.rs::connect_pad_added` and was deliberately moved off the
main loop to close a race where rtpbin dropped the first packet.

---

## 4. Code layout (actual)

Final shape, with shipped LOC noted (rounded):

### Rust workspace

Line counts below are approximate snapshots from main as of this
revision — `wc -l` rather than a careful impl/test split. They drift
under normal day-to-day churn; treat them as "this is a much bigger
crate than that other one" guidance, not as precise specs.

| Crate | Role | LOC (snapshot) |
|---|---|---|
| `hxbridge` | C-GObject wrapping pattern, scalar signal emit. The R3.0 foothold. | ~580 |
| `hotline-proto` (`voice.rs`) | Typed wire ops for 600–606 + 0x01F5–0x01F9, SDP summary + ICE JSON build/parse, FFI shims. | ~1,700 |
| `hxvoice` | Pure state machine, no GLib / GStreamer. `Event`, `Action`, `SessionMachine`. `no_std`-friendly. | ~2,800 total — `action.rs` ~230 + `event.rs` ~200 + `lib.rs` ~70 + `state.rs` ~2,280, where the bulk of `state.rs` is the test module. |
| `hxvoice-runtime` | gstreamer-rs runtime owning the pipeline + webrtcbin + state machine. `CallbackBackend` + `SignalCallbacks` bridges to the C side. | ~4,400 total — `runtime.rs` ~3,420 (much of which is the integration-test module) + `ffi.rs` ~520 + `audio.rs` ~270 + `debug.rs` ~145 + `lib.rs` ~70. |

The runtime crate is much larger than the original ~900 LOC estimate,
mostly because of (a) test coverage in `runtime.rs` (multi-thousand
LOC of integration tests for the dispatch arms + mid lookup + state
walks), (b) device-picker FFI + persistence (Phase 8.E), and (c) the
wedge watchdog (60s glib timeout + always-on RTP counter probe).

### C

| File | Role |
|---|---|
| `src/hotline.h` | `HTLC_HDR_VOICE_*` 600–606 + `HTLC_DATA_VOICE_*` 0x01F5–0x01F9 `#define`s (Rust holds the canonical typed defs). |
| `src/hl_access.h` | `HL_ACCESS_VOICE_CHAT 55`. |
| `src/voice.h` / `src/voice.c` | Thin wire-out helpers (`hx_send_voice_join` etc.) that `voice_panel.c` and `rcv.c` call directly. |
| `src/voice_runtime.h` | Opaque-pointer FFI surface for the Rust runtime (constructor, event injection, signal callbacks). |
| `src/voice_panel.{c,h}` | Per-chat-tab toolbar — Join / Leave / Mute buttons. Subscribes to `state-changed` and `mute-changed` runtime signals via `SignalCallbacks`. |
| `src/options.c` | `settings_page_voice()` — input/output device pickers via `pref_combo_row`. Lives in `options.c` between the Sound and Notifications pages, not in a separate `voice_settings.{c,h}` (the original scoping name). |
| `src/rcv.c` | `rcv_task_voice_*` for 600/601/603/606 replies; `rcv_voice_*` for the 602/604/605 server-initiated notifications. Each calls a Rust parser then feeds the typed result into `gtkhx_voice_runtime_*`. |
| `src/gtkhx_session.{c,h}` | Was scoped to gain six voice-related signals. **Re-scoped during Phase 8.D**: the runtime drives `voice_panel.c` directly via the `SignalCallbacks` FFI struct, no new `GtkhxSession` signals were added. Simpler and avoids an indirection that the panel was the only consumer of. |

---

## 5. Sub-phasing — shipped notes

### Phase 8.0 — R3.0 glib-rs foothold ✅

Shipped. `rust/crates/hxbridge/` lives in the workspace with
`session_from_ptr`, `session_from_ptr_full`, and
`emit_pointer_pair_signal`. The lifetime model is documented in
`docs/rust-glib-interop.md`. The gtk-rs-core 0.22 / gstreamer-rs 0.25
families are pinned in `rust/Cargo.toml`.

The Cargo.lock blast-radius worry from the original doc materialised
but was manageable — the CI cache restored fast enough that we didn't
need special handling.

### Phase 8.A — Capability + signaling scaffolding ✅

Shipped at `1481da0`. Capability bit set in both LOGIN masks,
opcodes / field tags in `hotline-proto::messages`, parser + builder
in `hotline-proto::voice`, FFI shims, `rcv.c` dispatch with debug
logging, `proto_trace.c` voice category.

### Phase 8.B — GStreamer dep + bare pipeline ✅

Shipped at `7178b7f`. `hxvoice-runtime` skeleton, `gst::init` from
Rust, audio.rs device enumeration via `gst::DeviceMonitor`, loopback
Tier 2 test that runs `audiotestsrc ! mulawenc ! mulawdec ! fakesink`.

### Phase 8.C — State machine + webrtcbin runtime ✅

Shipped across many sub-branches (`claude/voice-phase-c`,
`claude/voice-phase-c-runtime`, `claude/voice-phase-c-step3` through
`-step8`). The state machine lands as `hxvoice/src/state.rs` with
~50 unit tests; the runtime lands as `hxvoice-runtime/src/runtime.rs`
with the full SDP / ICE / pad-added / connection-state-changed
dispatch.

Notable deviations from the scoping plan:

- **SDP-answer chaining** had to happen INSIDE the
  set-remote-description promise rather than as a separate action,
  because issuing `create-answer` synchronously after kicking off
  set-remote-description made webrtcbin error the promise. The state
  machine's `Action::CreateAnswer` became a no-op handled by the
  runtime's promise chain instead.
- **Renegotiation queue** lives on `SessionMachine` (`queued_offer`
  field, cleared on `fail()` and `LeaveRequested`).
- **Stale create-answer guard** via `answer_generation` — the runtime
  bumps the generation on every `Action::CreateAnswer` dispatch and
  the promise callback drops stale resolutions on the floor.

### Phase 8.D — Chat tab UI + runtime wire-up ✅

Shipped across `claude/voice-phase-d`, `-runtime-wire`,
`-bridge-backend`, `-signal-bridge`. The toolbar lives at the top of
each chat tab content via `voice_panel_mount(chat_box)` from
`chat_tab_box_for_cid()`. Visibility / state logic matches the
original scoping:

- Cap not echoed → toolbar hidden.
- Cap echoed but `HL_ACCESS_VOICE_CHAT` unset → visible but greyed,
  with the spec-mandated "Voice chat requires permission" tooltip.
- Cap echoed + access set → fully interactive.

The signal-bridge approach replaced the planned `GtkhxSession` signal
additions: the runtime emits via `SignalCallbacks` FFI directly into
`voice_panel.c`'s handlers. The chat-tab toolbar is the only
consumer; the indirection through a session signal would have been
overkill.

Speaker-indicator decoration in the user list is **not yet
implemented** — it's a `GtkColumnView` column add that needs picking
up as a Phase 8.D follow-up.

### Phase 8.E — Settings device pickers ✅

Shipped on `claude/voice-phase-e-devices` (merged into main June 2026).
Two combo rows under a new "Voice" page between Sound and
Notifications. Picks persist as `VOICEINPUTDEVICE` /
`VOICEOUTPUTDEVICE` in `gtkhxrc`. The runtime reads them via the
`audio::DEVICE_PREFS` static and threads them through `make_send_bin`
/ `make_receive_bin`.

**Open Phase 8.E follow-ups** (mentioned in the original doc but not
yet implemented):

- "Start muted" toggle. Default ON per spec.
- "Push-to-talk key" capture row.
- "Auto-join voice when joining a chat room" toggle. Default OFF.

These are small additions to `settings_page_voice()` in `options.c`;
the runtime infrastructure for mute toggling is already in place,
and PTT just needs a `GtkEventControllerKey` on the chat input
widget that toggles mute on key down / up.

### Phase 8.F — Tier 3 integration matrix ✅

Shipped on `claude/voice-phase-f-tests` (merged June 2026). The Tier
3 matrix now exercises the voice extension end-to-end against the
Janus container:

- **`HX_TEST_CAP_VOICE`** + **`voice_port`** added to
  `tests/integration/server_matrix.{c,h}`. Janus is the only entry
  advertising the cap today (voice_port = 5514).
- **Seven new test binaries** under `tests/integration/`:
  `test_voice_join.c`, `_sdp_roundtrip.c`, `_ice_trickle.c`,
  `_mute.c`, `_implicit_leave.c`, `_participants.c`,
  `_disconnect_cleanup.c`. All seven follow the no-silent-skips
  policy (fail loudly when no voice-capable target is in the matrix)
  and are gated `is_parallel: false` per the per-IP connect-rate
  caveat documented on the other Janus tests.
- **Janus container shifted to `--network=host`** so libnice can
  negotiate ICE against 127.0.0.1 — Docker's default bridge strips
  the kernel route the server-reflexive candidate path needs. With
  host networking the container's listen ports ARE the host ports,
  so `tests/janus/conf/config.yaml` is now pinned to the matrix-
  published numbers directly: `Port: 5510`, `TLSPort: 5610`,
  `VoiceUDPPort: 5514`. mhxd at the conventional 5500/5501 still
  coexists via the usual bridge-net mapping.
- **Janus voice config** (`EnableVoice: true`, `VoiceUDPPort: 5514`,
  per-account `VoiceChat: true` on the bundled guest / admin accounts
  via `seed-hope-passwords.sh`) wires through the access bit gate
  on the toolbar.

Wire-fixture tests in `hotline-proto::voice` and state-machine
property tests in `hxvoice` remain the unit-test floor; the Tier 3
suite covers the live-wire shapes those tiers can't replicate.

### Unplanned: wedge watchdog ✅ (shipped on `claude/voice-wedge-deadline`)

The DTLS / ICE timer expiry got softened during Phase 8.C debugging
because the webrtcbin `_collate_peer_connection_states: Undefined
situation` FIXME can leave a session never reporting `Connected`
even with working audio. The wedge watchdog adds an
RTP-activity-driven last-ditch deadline: 60 s in `Connecting` with
no buffer arrivals on the receive bin's depay sink → inject
`Timeout::WedgeDeadline` → `fail()`. Design:

- `Inner::rtp_buffers_received: Arc<AtomicU64>` incremented by an
  always-on BUFFER probe attached in `start_receive_bin`. Probe runs
  in production (single relaxed atomic add per buffer), separate
  from the gated `voice-flow` diagnostic log probes.
- `arm_wedge_watchdog` runs on the state-machine transition into
  `Connecting` (detected by `handle_event` sampling state
  before-vs-after `step()`). Snapshots the counter, schedules a
  60 s one-shot `glib::timeout_add_local`.
- `wedge_watchdog_tick` on expiry: if counter advanced, rearm with
  fresh snapshot; if unchanged, inject the timeout.
- `state.rs` handles `(Connecting, WedgeDeadline)` → `fail()`
  (Leaving + TearDown). Other states fall through the catch-all.
- Cancelled on any transition out of Connecting (Connected, Leaving,
  Idle).
- Test hooks: `bump_rtp_buffers_received_for_test`,
  `fire_wedge_watchdog_for_test`, `wedge_watchdog_armed`. The 60 s
  glib timeout is exercised indirectly via the manual tick.

### Phase 8.G — Voice indicators in the user list ✅

Shipped on `claude/voice-speaker-indicator`. A small symbolic icon
appears next to each user in the chat / standalone users list,
reflecting their voice state. Three render values:

| Indicator | Shown when                              | Icon                              |
|-----------|-----------------------------------------|-----------------------------------|
| NONE      | uid is not in voice chat                | empty cell                        |
| IN_VOICE  | uid is in voice, not muted              | `audio-volume-low-symbolic`       |
| MUTED     | uid is in voice + server-flagged muted  | `microphone-disabled-symbolic`    |

A fourth value `SPEAKING` exists in the `HxVoiceIndicator` enum and
the runtime plumbing fires its underlying signal, but the C-side
`compute_indicator` deliberately demotes SPEAKING → IN_VOICE in the
current shipping configuration. The reason — and the revert path —
live in §12 step 7 below.

Architecture:

```
                 wire 605/JOIN reply
                          │
                          ▼
              rcv.c::hx_rcv_voice_room_status
                          │
              hx_voice_model_ingest_participants
                          │
                          ▼
              ┌─ HxVoiceModel (per-uid {in_voice, muted, speaking}) ─┐
              │                                                       │
              ▲                                                       ▼
runtime per-pad RTP probe                      "indicator-changed" (uid, ind)
              │                                                       │
              ▲                                                       ▼
        SignalKind::SpeakerChanged                        users_view voice column
              │                                                       │
              ▲                                                       ▼
voice_panel.c::voice_runtime_speaker_changed_cb            row repaint (icon swap)
              │
       hx_voice_model_set_speaking
```

Key pieces:

- **`src/voice_model.{c,h}`** — GObject keyed on uid, owns the
  canonical per-uid state. Two ingest paths (`_ingest_participants`
  for the wire blob, `_set_speaking` for the runtime). One signal
  (`indicator-changed`) fires per real visible flip.
- **`src/users_view.c`** — new 22 px column between UID and Name.
  Each cell subscribes to the model once and filters by row uid.
- **`hxvoice-runtime`** — `Inner::per_user_rtp_buffers:
  Arc<Mutex<HashMap<u16, Arc<AtomicU64>>>>` allocated lazily in
  `connect_pad_added` when `parse_voice_mid_label` resolves the pad's
  mid to `User(uid)`. A 200 ms `glib::timeout_add_local`
  (`speaker_tick`) diffs the counters against a previous snapshot
  and fires `SignalKind::SpeakerChanged` per uid whose state flips.
- **`SignalCallbacks`** grew two slots — `speaker_changed` (uid,
  is_speaking) and `error` (text) — wired through the FFI mirror,
  C header, and `voice_panel.c` handlers. The Error slot routes
  through `toolbar_show_toast` so the softened DTLS / ICE / Media
  timeouts now surface as visible AdwToasts instead of silent state
  churn.

Limitations and the cleanup path for "speaking":

- The runtime probe counts every PCMU buffer that lands on a
  participant's receive bin, but PCMU + WebRTC + `mulawenc` has no
  VAD — 50 pps arrive continuously while a peer is unmuted, with no
  per-packet volume information. Reporting "speaking" from the probe
  alone is therefore equivalent to "unmuted + pipeline alive", which
  the mute bit already conveys.
- Until real VAD ships, `voice_model.c::compute_indicator` falls
  through to IN_VOICE for the SPEAKING case behind a
  `#if HX_VOICE_INDICATOR_SHIPS_SPEAKING` gate. Flipping the gate
  to 1 (and updating the matching expectation in
  `tests/unit/test_voice_model.c`) re-enables the SPEAKING render.

---

## 6. Server availability — Janus is the Tier 3 target

Unchanged from the original scoping; Janus is still the only known
server-side implementation. Mobius issue tracker still doesn't have
a voice issue open (June 2026). Nothing in Phase 8.A–8.E ran into
the Janus implementation diverging from the spec on the happy path.

---

## 7. Gotchas (post-shipping, what mattered)

The original gotchas list — most are now resolved or are non-issues
in practice. What did matter:

- **The receive-bin link race.** The biggest debugging time sink was
  rtpbin dropping the first packet when the bin link happened on
  the main loop (after marshalling away from the streaming thread).
  The fix was synchronous-link-on-streaming-thread in
  `connect_pad_added`. Not in the original gotchas list because we
  didn't expect it.
- **webrtcbin `_collate_peer_connection_states: Undefined situation`.**
  The FIXME can keep a working session from reporting `Connected`.
  Drove the IceConnectivity / Dtls timer softening + the wedge
  watchdog. Not in the original plan.
- **Pipeline must be PLAYING before peer-connection work.** While
  the pipeline is NULL, webrtcbin's internal `is_closed` flag stays
  true and every SDP / ICE op silently aborts with "Peerconnection
  is closed, aborting execution" at DEBUG level. Fix is to move
  pipeline to Playing in `VoiceRuntime::new` before any
  set-remote-description / create-answer / add-ice-candidate.
- **`libnice-gstreamer1` (Debian) / `gstreamer1-plugins-bad-free-extras`
  (Fedora)** are required runtime deps for libnice — webrtcbin
  won't leave NULL without them. CI install instructions updated.
- **Audio device permissions on Flatpak.** Still open. The current
  `com.nasledov.gtkhx.yml` grants `--socket=pulseaudio` (good
  enough for playback against a host PulseAudio / pipewire-pulse
  shim), `--share=network`, `--share=ipc`, `--device=dri`. There is
  no mic-capture permission yet — neither `--device=all` nor the
  `org.freedesktop.portal.Device` portal. Voice capture works in
  practice on hosts where the PulseAudio socket exposes the mic
  through pipewire-pulse, but we haven't deliberately shipped a
  capture strategy. Decision and manifest change are a Phase 8.E /
  packaging follow-up.
- **GStreamer 1.20 floor.** Fine in practice. GNOME runtime 49 ships
  1.26.

Closed gotchas (no longer relevant):

- ~~`gtk_hlist_compat` user list~~ — Phase 5 GtkColumnView migration
  already done.
- ~~mid label parsing strictness~~ — strict parsing shipped in
  `hotline-proto::voice::sdp::parse_mid_label` with bounds + no
  leading zeros, hardened with property tests.
- ~~Renegotiation queueing~~ — `queued_offer` ships on
  `SessionMachine`, tests pin "drop older offer, answer only newest".

---

## 8. Decisions, locked in

- **WebRTC stack**: `gstreamer-rs` + `gstreamer-webrtc-rs`. ✅
- **Mock SFU**: skipped for v1. ✅ — Janus is the Tier 3 target;
  state-machine property tests cover the adversarial cases. If we
  ever hit a class of bug Janus can't reproduce, Go + Pion is the
  natural pick.
- **Toolbar location**: per-chat-tab. ✅
- **Default behaviour when joining a chat room**: explicit click. ✅
  (The "Auto-join" pref toggle is still open per Phase 8.E
  follow-ups.)
- **PTT keybind capture UI**: local-to-the-window for v1. Punt global
  PTT. **Open** — PTT not yet implemented at all.
- **CAP_EXTENDED_PRIV scope**: toast-only for Phase 8, decode later.
  Not blocking voice.
- **GtkhxSession voice signals**: re-scoped to NONE. The runtime
  drives `voice_panel.c` directly via `SignalCallbacks`. ✅

---

## 9. Effort estimate — actuals

| Phase | Estimated calendar | Actual calendar | Estimated LOC | Actual LOC |
|---|---|---|---|---|
| 8.0 | 1 week | ~1 week | ~320 | ~580 (`hxbridge`) |
| 8.A | 1–2 weeks | ~1 week | ~820 | ~700 (Rust wire + C dispatch + tests) |
| 8.B | 1 week | ~3 days | ~380 | ~280 (skeleton + loopback) |
| 8.C | 3–4 weeks | ~5 weeks (the debugging weeks) | ~2,350 | ~3,500 (state machine + runtime + the eight sub-steps) |
| 8.D | 1–2 weeks | ~2 weeks | ~800 | ~600 (panel + runtime wire + bridge backend + signal bridge) |
| 8.E | 1 week | ~3 days | ~450 | ~500 (device pickers + FFI + prefs) |
| 8.F | 1 week | ~2 days | ~950 | ~1,800 (matrix + 7 Tier 3 binaries + Janus host-net repinning) |
| 8.G | — | ~1 day | — | ~1,900 (voice indicator column + model + runtime per-pad probe + signal bridge + soft Media + Error toast) |
| wedge | — | ~3 days | — | ~500 |

Biggest underestimate: Phase 8.C. The webrtcbin pad-added race, the
codec-preferences pinning, the bundle-policy choice, the receive-leg
synchronous-link decision, the `_collate_peer_connection_states`
FIXME workarounds — none of these were in the scoping doc and all of
them ate calendar time. Budget generously for any future webrtcbin
work.

---

## 10. What this is not (unchanged)

- **Not a P2P voice extension.** Spec rules out a mesh; server
  routes everything.
- **Not video.** PCMU only.
- **Not Hotline-NG.** Voice rides on the existing 1.x wire protocol
  via new TRAN opcodes.
- **Not coupled to TLS Phase 7.** Voice signaling rides the existing
  control channel; voice media has its own DTLS-encrypted SRTP
  regardless.

---

## 11. What this means for R3 / R4 / R5

Phase 8 became the leading edge of gtk-rs adoption as planned.
Concrete shipping outcomes:

- **`hxbridge`** is in the workspace as the canonical home for
  C-GObject wrapping. Lifetime model documented in
  `docs/rust-glib-interop.md`. Used by `hxvoice-runtime` today; R3+
  work extends it.
- **The `MAIN_THREAD_RUNTIMES` thread-local registry pattern** —
  `hxvoice-runtime` uses it to look up `&VoiceRuntime` from
  `Send`-required closures (Promise change-funcs, ICE callbacks,
  pad-added) by an integer id. R3's eventual tokio↔GLib pipeline
  can adopt the same shape if it needs Send-friendly handles to
  main-thread state.
- **`glib::MainContext::default().invoke` discipline** for
  cross-thread marshalling is established and tested.
- **gtk-rs-core 0.22 + gstreamer-rs 0.25 + libadwaita-1 deps** are
  in the workspace.

What R4 (GtkhxSession in Rust) inherits:

- The Phase 8 work didn't add any new `GtkhxSession` signals (the
  re-scoping to direct `SignalCallbacks` instead). So R4's
  GtkhxSession port carries less voice-specific work than the
  scoping doc anticipated.

What R5 (UI in Rust, window by window) inherits:

- `voice_panel.{c,h}` ports when `chat.c` does.
- `settings_page_voice()` ports when `options.c` does.
- The `voice.h` opaque-handle helpers and the FFI shim layer in
  `hxvoice-runtime::ffi` can delete when no C UI is left.

What doesn't change: `hxvoice` stays `no_std`-friendly with pure
typed data in/out.

---

## 12. Next concrete steps

Voice works end-to-end. The remaining roadmap:

1. **Phase 8.E follow-ups** — small additions to
   `settings_page_voice()`:
   - "Start muted" toggle (default ON) — partially shipped; the
     `on_join_toggled` path in `voice_panel.c` already forces
     `gtkhx_voice_runtime_mute(rt, 1)` after every JOIN. A
     user-facing toggle to disable that "always-mute-on-join"
     default still hasn't landed; punt unless someone asks.
   - ~~PTT key capture + `GtkEventControllerKey`~~ — **Shipped on
     `claude/voice-ptt`** (June 2026). Window-scoped key controller
     (not chat-input-scoped — the captured key works while focus is
     on any widget in the GtkHx window). Two prefs:
     `CFG_VOICE_PTT_ENABLED` (BOOLEAN) and `CFG_VOICE_PTT_KEY`
     (STRING — canonical `gtk_accelerator_name` output). Capture
     dialog rejects plain typing keys (letters, digits, Space, Tab,
     Return) so PTT can never eat chat-input keystrokes; accepts
     F1–F24, Pause, Scroll Lock, Insert, Print, Menu, and any
     Ctrl/Alt/Super-modified combination. `src/voice_ptt_keyspec.{c,h}`
     hosts the pure vocabulary + canonicalisation; `src/voice_ptt.{c,h}`
     attaches the controller in `toolbar.c::create_toolbar_window`
     and drives `hx_send_voice_mute` + `gtkhx_voice_runtime_mute`
     on press/release edges. Unit-tested via
     `tests/unit/test_voice_ptt_keyspec.c`, which pins the
     vocabulary contract (acceptance + rejection lists), the
     canonicalisation round-trip, and the deterministic modifier
     ordering. Start-muted is the existing default — the toolbar
     already forces `mute=1` after every JOIN — so PTT layers on
     top with no additional code on the runtime side.
   - "Auto-join voice when joining a chat room" toggle (default OFF).
   Persist via the existing `cfgvars` table.
2. **Ping VesperNet** for confirmation that Janus's voice impl is
   feature-complete (server-side mute enforcement, room-full
   handling, access-bit check, ~100 ms mute debounce).
3. **Flatpak mic-capture permission**: pick between the cheap
   `--device=all` finish-arg vs the sandboxed
   `org.freedesktop.portal.Device` portal and update
   `com.nasledov.gtkhx.yml`. Neither is shipped today — the manifest
   only grants `--socket=pulseaudio` for playback. Decision and the
   one-line manifest change belong with Phase 8.E or a packaging
   follow-up.
4. **Real voice-activity detection** for the speaker indicator. The
   Phase 8.G plumbing already wires runtime → C end-to-end; what's
   missing is a volume-graded signal to feed it. Two paths:

   a. **Client-side VAD via GStreamer `level`.** Insert the `level`
      element into each receive bin's decoded PCM path
      (`mulawdec ! audioconvert ! level interval=100ms ! audioresample
      ! autoaudiosink`); hook the bus for `"level"` messages; threshold
      the RMS at around -45 to -50 dB; feed the result into
      `Inner::per_user_speaking` instead of (or in addition to) the
      per-pad RTP-arrival probe. ~100–150 LOC of Rust. No server
      changes. CPU cost is negligible: per participant ~8 kHz × 1 ch ×
      one RMS sum per 100 ms interval ≈ 8 000 sample-ops/sec; even
      a 16-participant room runs at <1 % of one core.

   b. **RFC 6464 `audio-level` header extension.** Best signal quality
      eventually but requires fogWraith `Capabilities-Voice.md` to
      ratify the extension and Janus to advertise it. Calendar-coupled
      to upstream; don't block on this.

   When either lands, flip `HX_VOICE_INDICATOR_SHIPS_SPEAKING` to 1 in
   `src/voice_model.c` and update the matching expectations in
   `tests/unit/test_voice_model.c::test_speaking_overlay` and
   `_signal_emitted` (both annotate the flip point inline).

5. **Phase ∞**: video, group video, etc. Out of scope; would need
   its own capability bit and a new scoping doc.
