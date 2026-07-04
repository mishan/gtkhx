# Voice Chat Extension — Status Doc for GtkHx

Originally written 2026-06-09 as a scoping draft. **Mostly shipped as
of mid-June 2026** — end-to-end DTLS-SRTP voice against Janus
VoiceRoom works. This doc is now a status tracker plus design
reference; the scoping rationale is retained where it explains why a
choice was made.

Spec pinned at
<https://github.com/fogWraith/Hotline/blob/525e94e2208dc2ffb1ed65d69d681c7fd356e169/Docs/Protocol/Capabilities-Voice.md>
(`525e94e`, 2026-04-05).

**Optional at build time.** Voice is gated behind the `-Dvoice` meson
option (`auto` / `enabled` / `disabled`, default `auto`): it compiles in
when the GStreamer 1.20+ stack is present and drops out cleanly when it
isn't. With voice off, the `hxvoice` / `hxvoice-runtime` crates aren't
built, the GStreamer libs aren't linked, `HTLC_CAP_VOICE` isn't
advertised, and every voice C source / call site / test is compiled out
behind the `HAVE_VOICE` define. The gate plumbing is documented in
`CLAUDE.md` ("Voice chat (Phase 8) is an optional build feature").

## At-a-glance status

| Phase | Description | Status | Branch / commit |
|---|---|---|---|
| 8.0 | R3.0 glib-rs foothold (`hxbridge` crate) | **Shipped** | merged |
| 8.A | Capability bit + voice opcodes + Rust wire layer | **Shipped** | `1481da0` |
| 8.B | GStreamer dep + bare pipeline (loopback test) | **Shipped** | `7178b7f` |
| 8.C | State machine (`hxvoice`) + webrtcbin runtime (`hxvoice-runtime`) | **Shipped** | multiple |
| 8.D | Voice toolbar in chat tab + runtime wire-up + signal bridge | **Shipped** | multiple |
| 8.E | Settings → Voice device pickers (+ startup-apply + hot-swap) | **Shipped** | `claude/voice-phase-e-devices`; device persistence + live hot-swap on `claude/voice-device-startup-apply` |
| 8.F | Tier 3 integration matrix vs Janus | **Shipped** | merged from `claude/voice-phase-f-tests` |
| 8.G | Per-uid voice indicators in user list | **Shipped** (in-voice + muted) | `claude/voice-speaker-indicator` |
| follow-ups | PTT, "Start muted" toggle, "Auto-join", wedge-deadline hardening, real-VAD speaker detection | **Mixed** — wedge, soft-Media, PTT, real-VAD shipped; Start-muted / Auto-join open | `claude/voice-wedge-deadline` (wedge), `claude/voice-speaker-indicator` (soft-Media), `claude/voice-vad-level` (real-VAD) |

What also shipped that wasn't in the original plan:

- **Wedge watchdog** (`claude/voice-wedge-deadline`): the original soft-
  IceConnectivity / soft-Dtls behaviour can leave a session in
  `Connecting` forever if webrtcbin never reports `Connected` AND no
  RTP arrives. A runtime-owned 60 s watchdog reads an always-on
  `Arc<AtomicU64>` RTP counter from the receive bin's depay sink and
  injects `Timeout::WedgeDeadline` if the counter doesn't advance.
  Full design lives inline in §5 below ("Unplanned: wedge watchdog").
- **Renegotiation Connected-resync + cleanup-on-fail**
  (`claude/voice-renegotiate-fix`): four-part fix for an idle-mute
  scenario where a renegotiation cycle left the state machine
  stranded in `Connecting`, the wedge watchdog eventually tore the
  session down, the server never learned, and a manual rejoin
  inherited the broken webrtcbin's stale ICE / DTLS credentials.
  Fixes: (1) when the state machine re-enters `Connecting` after a
  renegotiation, the runtime synthesises a
  `WebrtcConnectionStateChanged(Connected)` event so the machine
  advances back to `Connected` without waiting on a notify that
  webrtcbin won't emit (its `connection-state` property never
  changed). (2) Per-session `has_been_connected_since_join`
  bookkeeping suppresses wedge-watchdog arming on re-entries to
  `Connecting`, so a future renegotiation cycle that slipped past
  the synthesis can't trigger the wedge alone. (3) `fail()` in the
  state machine now emits `VOICE_LEAVE` (601) before `TearDown` so
  the server cleans up the participant slot instead of keeping a
  ghost user in the room. (4) `Action::TearDown` walks the
  pipeline back to `Null` synchronously and rebuilds fresh
  pipeline-bits via `build_pipeline_bits`, so the next
  `JoinRequested` builds a clean webrtcbin instead of renegotiating
  against the previous session's keys.
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
  wire) and updated synchronously. The "actively speaking" arm
  (`SignalKind::SpeakerChanged`, `HxVoiceModel::set_speaking`) shipped
  its real signal on `claude/voice-vad-level`: a GStreamer `level`
  RMS detector replaced the per-pad RTP-arrival proxy, and
  `HX_VOICE_INDICATOR_SHIPS_SPEAKING` is now on. See §12 step 4 for
  the design and the kept-as-revert-switch gate.

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
  rapid mute/unmute from the client's side. **Mute and PTT shipped.**
  Mute is now also enforced **locally**: the
  `Action::SetSendPipelineMute` arm — previously a no-op, so a "muted"
  client kept streaming its mic to the server and relied entirely on
  the server to drop it — toggles a `volume` element's `mute` in the
  send bin, replacing the captured mic with digital silence before it
  is encoded. Silence (not a `valve` drop) keeps RTP flowing at the
  normal rate so the NAT/ICE path stays warm.
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
  documented in `docs/rust/glib-interop.md`.

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
`docs/rust/glib-interop.md`. The gtk-rs-core 0.21 / gstreamer-rs 0.24
families are pinned in `rust/Cargo.toml` (downgraded from 0.22 / 0.25
to fit Debian trixie's stock rustc 1.85 — see `rust-toolchain.toml`).

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

**Device persistence + hot-swap ✅** (`claude/voice-device-startup-apply`).
Two follow-on fixes closed the loop on the device pickers:

- **Applied at startup.** `prefs_read` deliberately doesn't run cfgvar
  changefuncs, so a saved device sat in `gtkhx_prefs` (and displayed
  correctly in Settings) but never reached the runtime's
  `DEVICE_PREFS` — the send/receive bins fell back to the system
  default on the first Join after launch. `apply_loaded_xtext_prefs`
  now pushes both loaded device names via `gtkhx_voice_set_input_device`
  / `_set_output_device`, alongside the other load-vs-changefunc fixups.
- **Hot-swappable during a call.** Changing the device in Settings now
  takes effect immediately, no Leave + Join. `changed_voice_{input,
  output}_device` updates the global `DEVICE_PREFS` (for future bins)
  then calls `gtkhx_voice_runtime_reload_{input,output}_device` on the
  active session's runtime. `reload_input_device` rebuilds the send bin
  in place — unlink the ghost src from the webrtcbin sink pad, drop the
  old bin, build a fresh one against the new device, re-link the SAME
  sink pad, re-apply the current mute state, sync — so there's no SDP
  renegotiation. `reload_output_device` rebuilds every live receive bin
  via the existing `stop_receive_bin` + `start_receive_bin` pair
  (replaying stored per-user volumes). Whole-bin granularity (the same
  the `TearDown` rebuild uses, all on the main thread) sidesteps the
  streaming-thread deadlocks per-element surgery on a live source
  invites; there's a sub-second gap on the affected leg while the new
  element prerolls. Covered by runtime unit tests (send-bin peer-pad
  reuse + mute re-apply, the unlinked-bin bail, and the live
  `reload_input_device` path against a real pipeline + webrtcbin).

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

A fourth value `SPEAKING` (`audio-volume-high-symbolic`) renders when
the runtime's voice-activity detector reports the uid above the
speaking threshold. Originally demoted to IN_VOICE pending real VAD;
since `claude/voice-vad-level` it renders for real
(`HX_VOICE_INDICATOR_SHIPS_SPEAKING == 1`). The gate is kept as a
one-line revert switch. Design in §12 step 4.

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
runtime `level` RMS VAD                         "indicator-changed" (uid, ind)
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
- **`hxvoice-runtime`** — `Inner::per_user_voice_activity:
  Arc<Mutex<HashMap<u16, Arc<AtomicU64>>>>` (renamed from
  `per_user_rtp_buffers` when VAD landed). `handle_level_message`
  bumps a uid's counter from the `level` element's RMS bus messages
  when the reading clears `SPEAKING_RMS_THRESHOLD_DB`. A 200 ms
  `glib::timeout_add_local` (`speaker_tick`) diffs the counters
  against a previous snapshot and fires `SignalKind::SpeakerChanged`
  per uid whose state flips.
- **`SignalCallbacks`** grew two slots — `speaker_changed` (uid,
  is_speaking) and `error` (text) — wired through the FFI mirror,
  C header, and `voice_panel.c` handlers. The Error slot routes
  through `toolbar_show_toast` so the softened DTLS / ICE / Media
  timeouts now surface as visible AdwToasts instead of silent state
  churn.

Speaking detection (now real VAD, `claude/voice-vad-level`):

- The original per-pad probe counted every PCMU buffer landing on a
  receive bin. PCMU + WebRTC + `mulawenc` has no silence suppression,
  so 50 pps arrive continuously while a peer is unmuted regardless of
  speech — "speaking" off arrival alone was just "unmuted + pipeline
  alive", barely more than the mute bit.
- Replaced by a GStreamer `level` element on each receive bin's
  decoded PCM. `handle_level_message` thresholds the per-window RMS
  at `SPEAKING_RMS_THRESHOLD_DB` (-50 dB) and drives
  `per_user_voice_activity`; `speaker_tick`'s existing diff turns
  that into SPEAKING with a ~one-tick (200 ms) hangover. The global
  `rtp_buffers_received` RTP counter stays for the wedge watchdog.
- `HX_VOICE_INDICATOR_SHIPS_SPEAKING` is now `1` and the
  `test_voice_model.c` expectations assert the SPEAKING render; the
  gate is retained as the one-line revert switch if the threshold
  needs to be silenced in the field.

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
- **Audio device permissions on Flatpak.** Partially shipped on
  `claude/flatpak-pipewire-mic`. The manifest keeps
  `--socket=pulseaudio` (which already gives capture in practice
  via pipewire-pulse, the pipewire-pulse shim doesn't gate mic
  separately from playback) and additionally exposes the native
  PipeWire socket via `--filesystem=xdg-run/pipewire-0` so
  GStreamer's pipewiresrc / pipewiresink can use it directly.
  Same belt-and-suspenders shape every other voice-capable
  Flatpak (Discord, Element, Signal, OBS) ships. Narrower than
  `--device=all` (no `/dev/snd`) but not per-app-prompted — a
  dedicated Audio portal that would let us drop the PipeWire
  socket exposure in favour of a system permission prompt is
  under discussion upstream (flatpak/xdg-desktop-portal #1129)
  but hasn't shipped. Revisit when it does.
- **GStreamer 1.20 floor.** Fine in practice. GNOME runtime 49 ships
  1.26.
- **Receive bins must be keyed by webrtcbin pad, not by mid.** When
  the local user is the lone first joiner, Janus negotiates its single
  transceiver as `a=mid:send` and bundles every remote's audio onto
  it — so all remote audio arrives under `mid=send`. The original code
  keyed receive bins (and named them) by mid, so a peer leaving and
  rejoining (new SSRC ⇒ a fresh webrtcbin src pad with the SAME mid)
  collided with the stale bin, and the first joiner stopped hearing
  the rejoiner. Fix: key (and name) receive bins by the webrtcbin pad
  name (`hxvoice-recv-<mid>__<pad>`, mid kept for VAD), and wire
  `webrtcbin.pad-removed` → tear down that pad's bin (the runtime had
  never connected `pad-removed`, though the state machine already had
  the `WebrtcPadRemoved → StopReceivePipeline` arm). Reproduced and
  guarded end-to-end against live Janus by
  `tests/integration/test_voice_rejoin_media.c` — a two-client media
  Tier 3 test (RED before the fix: A's RTP counter `before=100
  after=100`; GREEN after: `before=100 after=200`).
- **VAD speaker attribution on the bundled `mid=send` leg is a
  best-effort cname read, and is inherently unreliable.** Same
  lone-first-joiner topology: A's remote audio arrives under `mid=send`,
  whose bin name carries no uid, so the speaker indicator can't
  attribute it from the mid. `handle_level_message` falls back to the
  per-SSRC cname (`ssrc-<N>-cname=voice-<uid>`) read off the demuxed
  receive pad's `current_caps`. That cname originates in the server's
  SDP `a=ssrc … cname:voice-<uid>` lines (NOT RTCP SDES) — a non-spec
  extra; the spec's track-to-user mapping is mid-based. Whether it
  surfaces in the pad caps is subject to the same webrtcbin
  shared-transceiver race as the receive pad itself, so on the bundled
  leg attribution succeeds in some sessions and not others, while the
  per-user `mid:user-<uid>` leg attributes every time. The
  `/integration/voice/vad_speaker` Tier 3 test exercises the fallback
  and passes (single-process harness always wins the race); it is a
  positive guard, not a reproduction of the GUI's intermittent failure.
  Root cause and the deterministic fix are server-side — see the Janus
  renegotiation bug note (server must send `mid:user-<newUID>` recvonly
  sections instead of bundling onto `mid:send`).
- **RESOLVED server-side (spec + Janus update, July 2026).** fogWraith
  fixed both the spec (Capabilities-Voice.md commit "Clarify direction
  attributes…") and Janus. Every participant — including the first
  joiner — now gets a dedicated per-user `a=mid:user-<uid>` section, so
  there is no bundled `mid=send` receive leg and the race is gone: audio
  and VAD both resolve from the mid, reliably, in both directions. The
  bundled-leg cname fallback above is now dead code against a current
  server; it's kept only for older/non-compliant ones. Two client-facing
  changes the update requires (both landed):
  - **Leave = `a=inactive`, not `port=0`.** A departed participant's
    section keeps its `mid` but flips to `a=inactive` (port stays 9).
    webrtcbin fires NO `pad-removed` on that transition, so the state
    machine parses `a=inactive` out of the offer and emits
    `StopReceivePipeline{mid}`; the runtime tears down every receive bin
    for that mid (`mid_from_recv_bin_name`). Without this the departed
    bin leaks and a rejoin (fresh SSRC ⇒ new pad on the reactivated mid)
    piles up a duplicate. The `m=audio 0` (`has_disabled_slot`) detector
    is now vestigial — the current server never uses port 0.
  - **Stable mids + back-to-back renegotiation.** A `mid` is never
    reassigned (`user-<uid>` always = that uid), and the server may send
    a consolidated follow-up offer right after our answer; the state
    machine's existing `queued_offer` serialisation + the Connecting/
    Connected offer arms handle consecutive offer/answer cycles.
  Guarded by hxvoice unit tests
  (`inactive_user_section_emits_stop_receive_pipeline`,
  `active_only_offer_emits_no_stop_receive_pipeline`) and the live-Janus
  media Tier 3 suite.

- **STILL OPEN — residual server-side (Pion) race, July 2026.** A
  separate, flaky failure remains: the *first* joiner intermittently
  never hears a *second* joiner. Root-caused from the server's Pion log
  to `SetHandleUndeclaredSSRCWithoutAnswer` — the joiner answers then
  publishes, and its first mic RTP occasionally reaches the SFU before
  the SFU has applied the answer, so Pion drops the "undeclared" SSRC,
  `OnTrack` never fires, and the publisher is never forwarded. The fix is
  server-side (the one-line Pion `SettingEngine` flag). The client is
  correct and symmetric; a client publish-delay does NOT close it
  (webrtcbin emits its send SSRC before any delay we can impose — tried
  and reverted), and the only client action that works is a full
  leave+rejoin of the joiner (too disruptive to automate). Because it
  only reproduces with two *real* GtkHx GUI processes (the VoiceRuntime
  test harness can't trigger the timing), the repro is scripted:
  `tools/voice-gui-repro.sh` runs two headless gtkhx under `gtk4-broadwayd`
  + a virtual mic and asserts the asymmetry; `tools/voice-gui-repro-loop.sh`
  loops to catch the flake. The `GTKHX_VOICE_AUTOJOIN` /
  `GTKHX_VOICE_AUTOUNMUTE_MS` env hooks in `voice_panel.c` drive the real
  Join/unmute path for those scripts (no-op unless set).

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
  PTT. ✅
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
  `docs/rust/glib-interop.md`. Used by `hxvoice-runtime` today; R3+
  work extends it.
- **The `MAIN_THREAD_RUNTIMES` thread-local registry pattern** —
  `hxvoice-runtime` uses it to look up `&VoiceRuntime` from
  `Send`-required closures (Promise change-funcs, ICE callbacks,
  pad-added) by an integer id. R3's eventual tokio↔GLib pipeline
  can adopt the same shape if it needs Send-friendly handles to
  main-thread state.
- **`glib::MainContext::default().invoke` discipline** for
  cross-thread marshalling is established and tested.
- **gtk-rs-core 0.21 + gstreamer-rs 0.24 + libadwaita-1 deps** are
  in the workspace. (Initially landed at the 0.22 / 0.25 family;
  downgraded to fit Debian trixie's stock rustc 1.85.)

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
   - ~~Device persistence + hot-swap~~ — **Shipped on
     `claude/voice-device-startup-apply`.** Saved capture/playback
     devices are now applied at startup (`apply_loaded_xtext_prefs`
     pushes them into the runtime, since `prefs_read` doesn't run
     changefuncs), and a device change in Settings takes effect live
     during a call — `changed_voice_{input,output}_device` reload the
     active runtime, which rebuilds the send bin / receive bins in
     place reusing the existing webrtcbin pads (no renegotiation). See
     the "Device persistence + hot-swap" note under Phase 8.E above.
2. **Ping VesperNet** for confirmation that Janus's voice impl is
   feature-complete (server-side mute enforcement, room-full
   handling, access-bit check, ~100 ms mute debounce).
3. **Flatpak mic-capture permission** — partially shipped on
   `claude/flatpak-pipewire-mic`. The "true" portal route I
   originally floated turned out not to be purchasable: Flatpak
   has no `--socket=pipewire` (closed allowlist), and an Audio
   portal sibling to the Camera portal is still in upstream
   discussion (flatpak/xdg-desktop-portal #1129). What shipped
   instead is the conventional PipeWire-native pathway every
   current voice-capable Flatpak uses: keep `--socket=pulseaudio`
   (it already grants capture in practice via the pipewire-pulse
   shim) and additionally expose the native PipeWire socket via
   `--filesystem=xdg-run/pipewire-0` so GStreamer's pipewiresrc /
   pipewiresink can use it directly. Narrower than `--device=all`
   (no `/dev/snd` access) but not per-app-prompted — that final
   step waits for the upstream Audio portal to land. Zero code
   changes; `gst::DeviceMonitor` in the Phase 8.E device picker
   enumerates through PipeWire the same way it did before.
4. **Real voice-activity detection** for the speaker indicator —
   ✅ **shipped** via path (a), client-side VAD with GStreamer
   `level`. As built (`claude/voice-vad-level`):

   - A `level` element sits on each receive bin's decoded PCM tap
     (`mulawdec ! audioconvert ! level ! audioresample !
     autoaudiosink`) in `audio.rs::make_receive_bin`, posting a
     per-channel RMS message every 100 ms
     (`LEVEL_MESSAGE_INTERVAL_NS`).
   - The pipeline bus watch routes `"level"` messages to
     `runtime.rs::handle_level_message`, which maps the posting
     element back to its receive bin's uid, thresholds the RMS at
     `SPEAKING_RMS_THRESHOLD_DB` (-50 dB), and bumps that uid's
     voice-activity counter.
   - The per-uid counter (renamed `per_user_rtp_buffers` →
     `per_user_voice_activity`) is now driven by these RMS bumps,
     **not** the old per-pad RTP-arrival probe — PCMU has no silence
     suppression, so RTP arrival meant "unmuted + alive", not
     "speaking". The existing 200 ms `speaker_tick` diff/emit machine
     is unchanged: a counter that advanced in the window → speaking,
     with a natural ~one-tick hangover. The global
     `rtp_buffers_received` RTP probe stays as the wedge-watchdog
     liveness signal.
   - `HX_VOICE_INDICATOR_SHIPS_SPEAKING` is now `1` in
     `src/voice_model.c`; `test_speaking_overlay` and
     `_signal_emitted` assert the SPEAKING render. New Rust unit
     tests cover the dB threshold, the bin-name → uid parse, and a
     real-pipeline `level`-message extraction.

   Path (b) — **RFC 6464 `audio-level` header extension** — remains
   the better-quality eventual option but requires fogWraith
   `Capabilities-Voice.md` to ratify the extension and Janus to
   advertise it. Calendar-coupled to upstream; not pursued. The
   `HX_VOICE_INDICATOR_SHIPS_SPEAKING` gate is kept as the one-line
   revert switch should the `level` threshold prove too noisy in the
   field.

5. **Phase ∞**: video, group video, etc. Out of scope; would need
   its own capability bit and a new scoping doc.
