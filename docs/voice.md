# Voice chat

GtkHx implements the fogWraith voice-chat capability: a server-side SFU
that clients reach over WebRTC, with signalling carried by new
transactions on the existing Hotline TCP control channel. Media is
PCMU over DTLS-SRTP.

Spec: fogWraith `Docs/Protocol/Capabilities-Voice.md`, pinned at commit
`525e94e`. **The spec is not vendored in this repository** and many
source files cite it with no in-repo target, so the contract section
below is the closest thing the tree has to it.

## Build gate

Voice is an **optional build feature**, gated by the `-Dvoice` meson
option (`auto` / `enabled` / `disabled`, default `auto`):

- **`auto`** — compiles voice in when the GStreamer 1.20+ stack is
  found (`gstreamer-1.0`, `-app`, `-audio`, `-webrtc`), and silently
  drops it otherwise.
- **`enabled`** — a missing GStreamer stack is a hard configure error.
- **`disabled`** — voice is compiled out even where GStreamer is
  present.

With voice off, the voice Rust crates aren't built (so no GStreamer is
needed at build time at all), the GStreamer libraries aren't linked,
`HTLC_CAP_VOICE` isn't advertised at LOGIN, and every voice source,
call site, and test is compiled out behind `HAVE_VOICE`. The gate flows
from `voice_enabled` in the top-level `meson.build` into the
`HAVE_VOICE` define, the crate exclusion list in `rust/meson.build`,
and the `voice` Cargo feature on `gtkhx-ui`.

The 1.20 floor is where `webrtcbin`'s API stabilised. `webrtcbin` lives
in gst-plugins-bad on most distributions.

## The client's contract with the spec

- **Capability negotiation.** Set bit 2 (`0x0004`) —
  `HTLC_CAP_VOICE` — in `HTLC_DATA_CAPABILITIES` (`0x01F0`) during
  LOGIN. If the server doesn't echo it, the voice UI is hidden
  entirely.
- **Access bit.** `accessVoiceChat` is bit 55 of the access bitmap
  (`HL_ACCESS_VOICE_CHAT`). It gates `VOICE_JOIN` server-side. If the
  server echoes the capability but the bit is unset, the toolbar stays
  *visible but disabled*, with a "voice chat requires permission"
  tooltip — the spec calls for keeping it visible so a user knows to
  ask an admin.
- **The server is always the offerer.** The client never generates an
  SDP offer; it answers.
- **One room at a time.** Joining voice in room B implicitly leaves
  room A.
- **Mute is a server-enforced flag** (transaction 606). Push-to-talk is
  just rapid mute/unmute from the client's side.
- **Disconnect is an automatic leave.** The server cleans up when the
  TCP control connection drops.
- **PCMU only.** No other codec is mandated and none is offered.

### Transactions (600–606)

The range sits clear of the base protocol's 101–355 and the
chat-history extension at 700. Clients that don't negotiate the
capability never emit or receive these.

| Opcode | Name | Direction | Fields |
|---|---|---|---|
| 600 | `VOICE_JOIN` | C→S | `CHAT_ID` (u32) |
| 601 | `VOICE_LEAVE` | C→S | `CHAT_ID` |
| 602 | `VOICE_SDP_OFFER` | S→C | `CHAT_ID` + `VOICE_SDP` |
| 603 | `VOICE_SDP_ANSWER` | C→S | `CHAT_ID` + `VOICE_SDP` |
| 604 | `VOICE_ICE` | both | `CHAT_ID` + `VOICE_ICE` |
| 605 | `VOICE_ROOM_STATUS` | S→C | `CHAT_ID` + `VOICE_PARTICIPANTS` |
| 606 | `VOICE_MUTE` | C→S | `CHAT_ID` + `VOICE_MUTED` (u16) |

The reply to 600 additionally carries `VOICE_SDP`, `VOICE_CODEC`, and
`VOICE_PARTICIPANTS`. 604 is a bidirectional notification with no
reply, so unlike 600/601/603/606 it registers no reply task.

### Data fields (0x01F5–0x01F9)

The five sit in the gap between the Large-File 64-bit extension (which
ends at `0x01F4`) and the chat-history block at `0x0F01`+. Standard
Hotline TLV framing (`u16` tag, `u16` length, payload); integers are
big-endian.

| ID | Name | Payload |
|---|---|---|
| `0x01F5` | `VOICE_SDP` | UTF-8 SDP blob (RFC 8866). Non-empty on both the offer and answer sides — an empty answer would tell the server we accept nothing, and both the builder and its C wrapper reject it. |
| `0x01F6` | `VOICE_ICE` | UTF-8 JSON-encoded `RTCIceCandidateInit`. **An empty string is the end-of-candidates marker.** |
| `0x01F7` | `VOICE_CODEC` | Active codec name, ASCII. |
| `0x01F8` | `VOICE_MUTED` | u16: 0 = unmuted, 1 = muted. |
| `0x01F9` | `VOICE_PARTICIPANTS` | Packed binary blob, 6 bytes per entry. |

The participants blob is a flat array of 6-byte entries:
`u16 uid` + `u16 flags` + `u16 codec_id`, all big-endian. **Flags bit 0
is muted.** A trailing partial entry is ignored.

### Track-to-user mapping

Remote audio is attributed by the SDP media-section label
**`a=mid:user-{UID}`**, where `{UID}` is the Hotline user id. Mid
parsing is strict — bounded, no leading zeros. The local send leg is
`a=mid:send`.

**A participant leaving is signalled as `a=inactive`, not `port=0`.**
The departed participant's media section keeps its mid and flips to
`a=inactive` with the port left at 9. `webrtcbin` fires **no**
`pad-removed` on that transition — the pad lingers — so the state
machine parses `a=inactive` out of each incoming offer and emits a
teardown for every receive bin on that mid. Without that, the departed
bin leaks, and a rejoin (fresh SSRC ⇒ a new pad on the reactivated mid)
piles a duplicate on top. The `m=audio 0` disabled-slot detector is
vestigial; current servers never use port 0.

Mids are stable: a mid is never reassigned, so `user-<uid>` always
means that uid. The server may also send a consolidated follow-up offer
immediately after our answer, so consecutive offer/answer cycles have
to work back to back.

## Where it lives

### Rust

| Crate | Role |
|---|---|
| `hotline-proto` (`voice.rs`) | Typed wire builders/parsers for 600–606 and `0x01F5`–`0x01F9`; SDP shape parsing (mid labels, BUNDLE group, disabled-slot detection); the `RTCIceCandidateInit` JSON build/parse, hand-rolled rather than pulling in a JSON dependency for a bounded four-field shape. |
| `hxvoice` | The pure state machine. `no_std`, zero non-Rust dependencies — no GLib, GStreamer, GTK, or OS surface — so its tests run in any container on any architecture regardless of audio devices. Every transition is `step(&mut self, Event) -> Vec<Action>`, which makes the spec's annotated lifecycle examples replayable verbatim as event traces. |
| `hxvoice-runtime` | The GStreamer runtime: owns the pipeline, `webrtcbin`, and the state machine. Pumps events in and walks the action list. Bridges back to the UI through a `SignalCallbacks` FFI struct. |
| `hxvoice-model` | `HxVoiceModel` — the per-uid voice presence GObject behind the user-list indicators. |
| `hxvoice-send` | The client-initiated wire senders (`hx_send_voice_*`). Deliberately lean — only glib plus the pure `hotline-proto`, no GTK — so it is `cargo test`-able and the send-path unit test links just that staticlib. |
| `gtkhx-ui` (`voice_panel.rs`, `voice_ptt.rs`, `users_voice_col.rs`) | The per-chat-tab toolbar, the push-to-talk key controller, and the user-list indicator column. Behind the crate's `voice` Cargo feature. |

### C

| File | Role |
|---|---|
| `src/hotline.h` | `HTLC_HDR_VOICE_*` 600–606 and `HTLC_DATA_VOICE_*` `0x01F5`–`0x01F9` integer aliases for switch-case readability in `rcv.c`. The canonical typed definitions are in `hotline-proto`. |
| `src/hl_access.h` | `HL_ACCESS_VOICE_CHAT` (bit 55). |
| `src/voice.h` | The `hx_send_voice_*` C ABI the receive path calls; implemented by `hxvoice-send`. |
| `src/voice_runtime.h` | The opaque-handle FFI surface for the Rust runtime — construction, event injection, device enumeration, signal callbacks. |
| `src/voice_bridge.{c,h}` | Session / htlc field accessors for the Rust voice UI and senders. Rather than mirror the `session` / `htlc_conn` layouts in Rust — fragile, they change often and carry packed protocol fields — the Rust modules reach the handful of fields they need through these. |
| `src/voice_ptt_keyspec.{c,h}` | The pure push-to-talk key vocabulary and canonicalisation, split out so its rules are unit-testable without GTK. |
| `src/voice_panel.h`, `src/voice_model.h`, `src/voice_ptt.h` | Surviving headers for the ported modules; the matching `.c` files are gone. |
| `src/rcv.c` | `rcv_task_voice_*` for the 600/601/603/606 replies; `hx_rcv_voice_*` for the 602/604/605 server-initiated notifications. Each calls a Rust parser and feeds the typed result to the runtime. |
| `src/options.c` | The Settings → Voice page. |
| `src/sound_events.c` | Subscribes to the model's presence-chime signal and plays the chime, keeping the model itself sound-agnostic. |

The runtime drives the toolbar directly through `SignalCallbacks`; no
`GtkhxSession` signals were added for voice. The panel was the only
prospective consumer, so the indirection would have bought nothing.

`webrtcbin`'s worker threads emit signals on arbitrary threads; the
runtime marshals to the main loop via `glib::MainContext::default()
.invoke` plus a thread-local registry that maps an integer id back to
the runtime, so `Send`-required closures (promise change-funcs, ICE
callbacks, pad-added) can reach main-thread state.

## Pipeline

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
         ! audioconvert ! level ! audioresample ! autoaudiosink
```

Receive bins additionally carry a `volume` element, so per-remote
playback gain is settable per uid and replayed onto a rebuilt bin
(a mid-call rejoin tears down and rebuilds, and shouldn't forget the
listener's choice).

## State machine

The state machine owns every protocol decision — when 603 follows 602,
what the queue looks like during renegotiation — and the runtime is
deliberately mechanical. Actions describe *what* should happen; the
runtime decides *how*.

### Deviations from the spec's shape

- **SDP-answer chaining happens inside the set-remote-description
  promise**, not as a separate action. Issuing `create-answer`
  synchronously after kicking off set-remote-description makes
  `webrtcbin` error the promise. `Action::CreateAnswer` is therefore a
  no-op that the runtime's promise chain handles.
- **A renegotiation queue** lives on the session machine. An offer that
  arrives while one is in flight is queued; a newer offer replaces the
  older one, and the queue is cleared on failure and on leave.
- **A stale-answer guard.** The runtime bumps an answer generation
  counter on every `CreateAnswer` dispatch and drops stale promise
  resolutions on the floor.

### Timeouts

Three of the spec's timers are **soft** — they emit an error toast and
leave the session in place rather than failing it:

- **DTLS** and **ICE connectivity** were softened because of the
  `webrtcbin` collation bug (below): a session with working audio can
  simply never report `Connected`, and failing on that timer killed
  healthy sessions.
- **Media** ("no RTP for 30s while Connected") was softened later, and
  for a different reason: once servers began defaulting new joiners to
  **muted**, a fully-quiet room became the *common* case rather than a
  failure. Pre-softening, a local session would silently walk to
  Leaving after 30 seconds and the toolbar would flip back to "Join
  Voice" with no warning. What the timer still usefully flags is
  "audio has been quiet for a while" — a UX signal, not a session
  killer.

The error toasts exist because of that softening: `SignalCallbacks`
carries an error slot so a softened timeout surfaces as a visible toast
instead of silent state churn.

### The wedge watchdog

Softening the DTLS/ICE timers left a hole: a session could sit in
`Connecting` forever if `webrtcbin` never reported `Connected` *and* no
RTP ever arrived. The wedge watchdog closes it with an
RTP-activity-driven last-ditch deadline.

- An always-on buffer probe on the receive bin's depay sink increments
  a shared atomic counter — one relaxed atomic add per buffer, running
  in production, separate from the gated diagnostic probes.
- Arming happens on the state-machine transition *into* `Connecting`
  (detected by sampling state before and after `step`). It snapshots
  the counter and schedules a one-shot timeout.
- On expiry: if the counter advanced, rearm with a fresh snapshot; if
  unchanged, inject the wedge timeout, which the state machine handles
  as a real failure and tears the session down.
- Cancelled on any transition out of `Connecting`.
- The snapshot is a *delta* baseline, not a zeroing — pre-existing RTP
  from an earlier session must not make the current window look busy.
- Test hooks let a test bump the counter and fire the tick manually, so
  the wall-clock timeout doesn't have to elapse.

### Renegotiation Connected-resync

`webrtcbin`'s `connection-state` property **does not change** during a
renegotiation — it was already `connected` and stays there — so no
notify fires. Without help, a state machine that re-enters `Connecting`
for the renegotiation would wait forever for an event that will never
come, the wedge watchdog would eventually tear the session down, the
server would never learn, and a manual rejoin would inherit the broken
`webrtcbin`'s stale ICE/DTLS credentials.

Four parts fix it:

1. When the state machine re-enters `Connecting` after a
   renegotiation, the runtime **synthesises** a connection-state-changed
   (`Connected`) event so the machine advances without waiting.
2. Per-session "has been connected since join" bookkeeping suppresses
   wedge-watchdog arming on re-entries to `Connecting`, so a future
   renegotiation cycle that slips past the synthesis can't trip the
   wedge on its own.
3. Failure emits `VOICE_LEAVE` (601) before teardown, so the server
   frees the participant slot instead of keeping a ghost user in the
   room.
4. Teardown walks the pipeline back to `Null` synchronously and
   rebuilds fresh pipeline bits, so the next join builds a clean
   `webrtcbin` instead of renegotiating against the previous session's
   keys.

## Local mute is digital silence, not a valve drop

Mute is enforced server-side, but the client enforces it locally too —
the mute action toggles a `volume` element's `mute` property in the
send bin, replacing the captured microphone with digital silence
*before* encoding. (Before that, a "muted" client kept streaming its
microphone to the server and relied entirely on the server to drop it.)

**Silence rather than a `valve` drop is deliberate and easy to
"optimize" wrongly.** Digital silence keeps RTP flowing at the normal
rate, which keeps the NAT/ICE path warm. Dropping the buffers outright
would save a trivial amount of bandwidth and risk the path going cold.

## Voice-activity detection

**RTP arrival was never usable as a speaking signal.** PCMU over WebRTC
has no silence suppression, so packets arrive continuously at the
normal rate the whole time a peer is unmuted, regardless of whether
anyone is talking. "Speaking" derived from arrival alone was really
"unmuted and the pipeline is alive" — barely more information than the
mute bit already carries.

The real detector is a GStreamer `level` element on each receive bin's
decoded PCM, posting per-channel RMS on the bus. The bus watch routes
those messages to a handler that maps the posting element back to its
receive bin's uid, thresholds the RMS (`-50 dB`), and bumps that uid's
voice-activity counter. A periodic tick diffs the counters against the
previous snapshot and emits a speaker-changed signal per uid whose
state flipped, giving a natural one-tick hangover. The separate global
RTP counter stays, but only as the wedge watchdog's liveness signal.

`f64::NEG_INFINITY` is the `level` element's silence sentinel and
compares below every finite threshold, so digital silence correctly
reads as not speaking.

RFC 6464's `audio-level` header extension would be the better-quality
detector, but it requires the spec to ratify the extension and servers
to advertise it, so it's calendar-coupled to upstream and not pursued.

### Speaker attribution on a bundled leg

When remote audio arrives on the bundled `mid=send` leg rather than a
per-user `mid:user-<uid>` leg, the bin name carries no uid and the
level handler can't attribute it from the mid. It falls back to reading
the per-SSRC cname (`ssrc-<N>-cname` = `voice-<uid>`) off the demuxed
receive pad's negotiated caps. That cname originates in the server's
SDP `a=ssrc … cname:voice-<uid>` lines, **not** RTCP SDES — it is a
non-spec extra; the spec's track-to-user mapping is mid-based. Whether
it surfaces in the pad caps is subject to the same shared-transceiver
race as the receive pad itself, so on a bundled leg attribution
succeeds in some sessions and not others, while a per-user leg
attributes every time. Resolutions are cached per bin (bin names embed
the unique-per-lifetime pad name, so an entry can never alias a
different remote) because level messages arrive roughly ten times a
second per remote.

## Voice indicators in the user list

A small symbolic icon per row in the chat and standalone user lists:

| Indicator | Shown when | Icon |
|---|---|---|
| None | uid is not in voice chat | empty cell |
| In voice | in voice, not muted, not speaking | dim speaker |
| Speaking | in voice and above the VAD threshold | highlighted speaker |
| Muted | in voice and server-flagged muted | microphone-disabled |

Muted beats speaking: a server-flagged muted participant can't be
producing audio from a listener's perspective even if a residual VAD
reading trips.

`HxVoiceModel` is a GObject keyed on uid holding the three input flags
plus the last emitted indicator. Three sources flow in: the
`VOICE_PARTICIPANTS` blob (from 605 and the join reply), the runtime's
VAD, and disconnect/teardown. Every ingest recomputes the derived
indicator and emits `indicator-changed` **only when the computed
indicator actually flips**, so there's one signal per real visible
change rather than a churn burst. Ingest is capped at a bounded
participant count — the blob is server-supplied, and without a cap a
malicious or buggy server could force large allocations and an O(n)
sweep on the UI thread. The model also emits a presence-chime signal
(uid, joined) that the C sound layer subscribes to.

## Push-to-talk and settings

Settings → Voice has audio device pickers (input and output, with a
"System default" entry following the desktop's configuration) and the
push-to-talk group. Device choices persist in `gtkhxrc` and are read
when the runtime builds its send and receive bins, so they take effect
on the next join.

The PTT key controller is **window-scoped**, not chat-input-scoped, so
the captured key works with focus on any widget in the window. Its
accepted vocabulary is deliberately restricted so PTT can never eat a
chat keystroke:

- **Accepted:** F1–F24; Pause/Break, Scroll Lock, Insert, Print Screen,
  Menu; and any keyval with Control, Alt, or Super held. Shift alone
  does not promote a key — Shift+letter is still typing on every
  layout, and Shift+Home is the standard extend-selection shortcut.
- **Rejected without a strong modifier:** letters, digits, punctuation,
  Space, Tab, Return, Escape, Backspace, Delete, arrows,
  Home/End/PageUp/PageDown.
- **Always rejected:** modifier keys in isolation (the keyval that
  fires when you press Control by itself is `Ctrl_L`, and binding to
  that would trigger on every modifier press).

Matching masks off modifier bits outside the binding, but *extra*
strong modifiers do not match — `<Control>F12` does not fire on
`<Control><Shift>F12`. The press edge fires unmute and is consumed so
the key can't leak downstream; auto-repeat from a held key is
suppressed. Release fires mute. The hook is dormant when PTT is
disabled, when no key is captured, or when the session isn't joined to
a voice room.

## Gotchas

These are the findings that cost real debugging time. They are the most
load-bearing content in this document.

- **The pipeline must be PLAYING before any peer-connection work.**
  While the pipeline is in `Null`, `webrtcbin`'s internal `is_closed`
  flag is true, and **every** peer-connection task —
  set-remote-description, create-answer, add-ice-candidate — logs
  "Peerconnection is closed, aborting execution" **at DEBUG level** and
  returns silently. The promise never resolves. User-side this
  manifests as the state machine stuck in offer-pending forever, with
  nothing in the default log output to explain it. The fix is to move
  the pipeline to Playing at runtime construction, before any SDP or
  ICE operation.
- **The receive-bin link race.** The single biggest time sink.
  `pad-added` fires from `webrtcbin`'s worker thread the moment
  `gst_element_add_pad` runs, and rtpbin starts pushing the first
  buffer downstream immediately after, on its streaming thread. If the
  bin is built and linked after marshalling to the main loop, there's a
  window of milliseconds-to-tens-of-milliseconds in which buffers hit a
  pad with no peer, return `GST_FLOW_NOT_LINKED`, and rtpjitterbuffer /
  rtpbin can conclude the consumer is gone — producing the "one buffer
  at `src_0`, then silence" pattern. The fix is to **build and link the
  receive bin synchronously on the streaming thread**, before returning
  from the signal handler. `Pipeline::add`, `Element::link`, and
  `sync_state_with_parent` are all thread-safe; only the bookkeeping
  (recording the bin so teardown can find it) hops to the main thread.
- **`webrtcbin` `_collate_peer_connection_states: Undefined
  situation`.** This upstream FIXME can leave a session with working
  audio never reporting `Connected`. It is why the DTLS and ICE timers
  are soft and why the wedge watchdog exists. The cleaner eventual fix
  is to also watch `ice-connection-state` and use ICE's `connected` /
  `completed` as the "we're actually connected" gate, since ICE state
  is what `webrtcbin` manages itself — a bigger refactor than the
  softening.
- **Receive bins must be keyed by `webrtcbin` pad, not by mid.** When
  the local user is the lone first joiner, the server can negotiate its
  single transceiver as `a=mid:send` and bundle every remote's audio
  onto it, so all remote audio arrives under one mid. Keying receive
  bins by mid meant a peer leaving and rejoining — new SSRC, therefore
  a fresh `webrtcbin` src pad with the **same** mid — collided with the
  stale bin, and the first joiner stopped hearing the rejoiner. Bins
  are now keyed and named `hxvoice-recv-<mid>__<pad-name>` (the mid is
  retained for VAD), and `webrtcbin.pad-removed` is wired to tear down
  that pad's bin — the runtime had never connected `pad-removed` at
  all, even though the state machine already had the corresponding
  transition. Guarded end-to-end by a two-client media integration test
  against a live server: before the fix the first joiner's RTP counter
  was identical before and after the rejoin; after, it advances.
- **libnice is a required runtime dependency.** `webrtcbin` refuses to
  leave `NULL` without the nice plugin registered, and then silently
  aborts every peer-connection task afterwards (the same DEBUG-level
  "Peerconnection is closed" message). On Debian/Ubuntu the plugin
  ships in its own package (`gstreamer1.0-nice`) because of libnice's
  split licensing; gst-plugins-bad alone is not enough. On Fedora it's
  `gstreamer1-plugins-bad-free-extras`.
- **Flatpak audio has no clean answer yet.** Flatpak has **no
  `--socket=pipewire`** (the socket list is a closed allowlist), and an
  Audio portal sibling to the Camera portal is still under upstream
  discussion, so there is no per-app-prompted permission to request.
  What ships is the belt-and-suspenders arrangement every current
  voice-capable Flatpak uses: keep `--socket=pulseaudio` (which already
  grants capture in practice, since the pipewire-pulse shim doesn't
  gate microphone separately from playback) **and** additionally expose
  the native PipeWire socket via `--filesystem=xdg-run/pipewire-0` so
  GStreamer's `pipewiresrc` / `pipewiresink` can use it directly. This
  is narrower than `--device=all` (no raw `/dev/snd`) but it is not
  prompted. Revisit when the Audio portal lands.

## What this is not

- **Not peer-to-peer.** The spec rules out a mesh; the server routes
  everything.
- **Not video.** PCMU audio only.
- **Not a new protocol.** Voice rides the existing Hotline 1.x wire
  protocol via new transaction opcodes.
- **Not coupled to TLS.** Voice signalling rides whatever the control
  channel is; voice media has its own DTLS-encrypted SRTP regardless.
- **No mock SFU.** A live server is the integration target, and the
  state machine's property tests cover the adversarial cases. If a
  class of bug turns up that the live server can't reproduce, Go + Pion
  is the natural pick.

## Test targets

Janus (VesperNet) is the only known server-side implementation and is
the integration target. The container runs with host networking so
libnice can negotiate ICE against localhost — Docker's default bridge
strips the kernel route the server-reflexive candidate path needs — and
because host networking makes the container's listen ports the host
ports, its config pins the published numbers directly.

Wire-fixture tests in `hotline-proto` and state-machine tests in
`hxvoice` are the unit floor; the integration suite covers the
live-wire shapes those tiers can't replicate (join, SDP round-trip, ICE
trickle, mute, implicit leave, participants, disconnect cleanup, and
the two-client rejoin-media case). The voice tests are gated on the
same build switch as the code, and CI additionally builds the whole
binary with voice disabled on a GStreamer-free image to keep that path
green.

## Open

### Server publishes before the SFU applies the answer

The *first* joiner intermittently never hears a *second* joiner.

Root-caused from the server's Pion log to
`SetHandleUndeclaredSSRCWithoutAnswer`: the joiner answers and then
publishes, and its first microphone RTP occasionally reaches the SFU
before the SFU has applied that answer. Pion drops the "undeclared"
SSRC, `OnTrack` never fires, and the publisher is never forwarded to
anyone.

**The fix is server-side** (a `SettingEngine` flag). The client is
correct and symmetric here. A client-side publish delay does **not**
close it — `webrtcbin` emits its send SSRC before any delay we can
impose; this was tried and reverted. The only client action that
recovers is a full leave-and-rejoin by the affected joiner, which is
too disruptive to automate.

It only reproduces with **two real GtkHx GUI processes** — the runtime
test harness can't produce the timing — so the repro is scripted:
`tools/voice-gui-repro.sh` runs two headless instances under
`gtk4-broadwayd` with a virtual microphone and asserts the asymmetry,
and `tools/voice-gui-repro-loop.sh` loops to catch the flake. The
`GTKHX_VOICE_AUTOJOIN` and `GTKHX_VOICE_AUTOUNMUTE_MS` environment
hooks in the voice panel drive the real join/unmute path for those
scripts; both are no-ops unless set.

### Server omits the per-user mid on renegotiation to existing participants

When a new participant joins a room, the renegotiation offer sent to
the *existing* participants can omit the spec-required
`a=mid:user-{UID}` section for the newcomer and instead bundle their
audio onto the offerer's own `a=mid:send` transceiver.

Consequences for a spec-correct client:

- The receive bin for that audio is named from the `send` mid, which
  carries no uid, so **voice-activity attribution has no mid to work
  from** and falls back to the non-spec per-SSRC cname read described
  above — which is itself subject to a `webrtcbin` shared-transceiver
  race and therefore resolves in some sessions and not others.
- The first joiner in a room is the participant most likely to be hit,
  and the symptom is that their speaker indicator stays dark for other
  participants (and, historically, that audio itself degraded) until
  they leave and rejoin, at which point they get a per-user mid section
  and everything resolves reliably.

**The client is spec-correct and the fix is server-side**: send
`recvonly` `mid:user-<newUID>` sections rather than bundling onto
`mid:send`. The bundled-leg cname fallback in the runtime exists only
to keep older or non-compliant servers usable, and it is explicitly
best-effort. The integration test that exercises the fallback is a
positive guard — the single-process harness always wins the race — not
a reproduction of the intermittent GUI failure.

### Smaller items

- **"Start muted" and "Auto-join a room's voice" preferences.** The
  toolbar currently forces mute after every join, and joining voice is
  always an explicit click. Neither behaviour is user-configurable.
- **Global (system-wide) push-to-talk.** The key controller is scoped
  to the GtkHx window; PTT does not work while another application has
  focus.
