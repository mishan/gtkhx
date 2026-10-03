# Video chat

GtkHx implements the video extension to the voice capability: camera
video and screen sharing inside a voice room, forwarded by the server's
SFU. The wire contract is hxd-ng's `docs/capabilities-video.md`, drafted
in fogWraith's shape for upstream contribution. hxd-ng is the only
server that implements it, so it is the integration target.

Video is layered on voice: the same peer connection, the same UDP port,
the same 602 / 603 / 604 SDP and ICE transactions, the same room. What
it adds on the wire is the control transactions 607–611, the fields
`0x0220`–`0x0225`, capability bit 10, access bits 59 and 60, and the
`cam-` / `scr-` mid shapes. Everything in [voice.md](voice.md) still applies.

**Video is opt-in on both sides.** Nothing is published until the user
turns a camera on or shares a screen, and nothing is received until the
client asks for a stream by name with Video Subscribe (610).

## The client's contract with the spec

| Spec rule | What GtkHx does |
|---|---|
| Bit 10 only with bit 2 | Advertised only from a voice build, and only when VP8 decode is installed — a runtime question (`gtkhx_voice_video_receive_available`), so a host without gst-plugins-good's vpx still negotiates voice. A server echoing bit 10 without bit 2 is treated as offering neither. |
| Video Start's reply carries no SDP | 607 succeeds on its own; the capture is bound when the resulting 602 carries the `cam-send` / `scr-send` section. |
| Subscribe is the whole set | The Video panel computes "what is on screen now" and the state machine sends it as one 610 when it differs from the last one sent. |
| Key on `mid`, never on `sdpMLineIndex` | Everything is keyed by mid — and a receive pad's mid comes from its SSRC (see voice.md): `webrtcbin` will put a camera on the pad of an audio section. |
| Camera and screen are both VP8/96 | Each send section's `a=ssrc` is the payloader's own, set explicitly and fixed for the publication; the server tells the two apart by nothing else. |
| Pause keeps the slot; stop releases it | The camera button toggles with 609; ending a screen share is 608. |
| A failed publication doesn't end the call | A capture error stops that publication (608), toasts, and leaves voice alone. |
| Screen-share consent per share; persistent indicator | The platform picker every time, never remembered, and a banner in the main window while anything is shared. |
| Mids ≤ 16 bytes | The parsers reject a longer mid rather than truncating it. |

## Where it lives

| Piece | Where |
|---|---|
| Wire: opcodes, fields, publishers / subscriptions / limits codecs, 607–610 builders, mid grammar | `hxproto` (hx-libs): `messages.rs`, `video.rs`, `voice::MidLabel`, `dispatch` (`VideoStatus` handler kind), `parse::LoginInfo` (one limits slot per kind) |
| Protocol decisions | `hxvoice`: `video.rs` (vocabulary, a `no_std` mid scanner held to hxproto's by a test), `state.rs` (publications, subscriptions, local publications) |
| Pipeline | `hxvoice-runtime`: `video.rs` (receive and capture bins, frame store), `runtime.rs` (binding senders, pad routing, observers) |
| Senders | `hxvoice-send`: `hx_send_video_start` / `_stop` / `_state` / `_subscribe` |
| Receive | `rcv.c`: `hx_rcv_video_status`, the 607 refusal path, login limits → `hx_conn_*_video_limits` |
| Presence | `hxvoice-model`: per-uid camera / screen / paused flags and the `video-changed` and `video-started` signals, shown by `users_voice_col.rs` and announced in chat by `video_panel.rs` |
| UI | `gtkhx-ui`: `video_panel.rs` (the dockable panel), `voice_panel.rs` (camera and screen buttons), `screen_share.rs` (portal, consent, banner), `options_voice.rs` (camera picker) |
| Settings | `hxconfig` `voice.camera_device` |

No new meson option: video is part of `-Dvoice`, and a voice-off build
compiles none of it and advertises neither bit.

## State machine

`SessionMachine` holds, all cleared on leave, failure and room switch:

- **publications** — the room's list from the last 611, replaced
  wholesale, never merged.
- **wanted / subscribed** — the receive set the UI asked for and the one
  the server was last given. A new `VideoSubscriptionsWanted` is sorted
  and deduplicated; a 610 goes out only when it differs and only while
  in the room (a set declared during `JoinSent` goes out with the first
  offer).
- **local publications**, per kind: not publishing, live, or paused.
  Start, pause and stop emit the wire frame plus `SetVideoPublishing` /
  `SetVideoPaused` for the runtime. A refused start (`VideoStartFailed`,
  from the task label `video-start-camera` / `-screen`) ends the
  publication without a wire frame; a refused pause or resume
  (`VideoPauseFailed`, from `video-state-camera` / `-screen`) puts the
  capture back as the server still has it; a capture failure ends the
  publication with a 608.
- **request generations**, per kind: every 607 and every 609 is numbered,
  and a start numbers the pauses too. The number rides in the action's
  body, the send keeps it in the task's ptr slot — it never goes on the
  wire — and rcv.c hands it back with a refusal. A refusal undoes its own
  request only if it is still the latest: start, stop, start inside one
  round trip with the first refused leaves the second publication alone,
  and likewise pause, resume, pause.

Receive mids of every kind feed `mid_to_user`, and every receive mid
that goes `a=inactive` tears its leg down — the voice rule, widened.

## Runtime

**Transceivers are pinned per kind.** `on-new-transceiver` fires with
the transceiver's `kind` already set, so video sections get VP8/96 with
the `nack`, `nack pli` and `ccm fir` feedback fields and everything else
keeps PCMU. Pinning PCMU on a video section would answer it with no
codec.

**Receive.** A receive pad whose mid is `cam-user-N` / `scr-user-N` gets
`queue ! rtpvp8depay ! vp8dec ! videoconvert ! videoscale ! appsink`
(RGBA). The depayloader waits for a keyframe and asks for one after loss;
the leaky queue keeps a slow decode from stalling the bundle. The appsink
keeps the newest frame per `(uid, kind)` in a shared store and posts one
main-loop notification per batch; the UI takes frames and wraps the
mapped buffer in a `GdkMemoryTexture` without copying. RTX (PT 97) is
not answered.

**Send.** When an offer carrying `cam-send` or `scr-send` is set and that
kind is wanted, the runtime sets the transceiver `sendonly` — before
requesting its pad, since `webrtcbin` refuses a sink pad for a `recvonly`
transceiver — requests `sink_<mline>`, and links the capture bin:

```
source ! videoconvert ! videoscale ! videorate ! caps(w×h) ! tee
  tee. ! queue ! vp8enc ! rtpvp8pay ! caps(VP8/96, ssrc) → webrtc
  tee. ! queue ! videoconvert ! appsink          (the "You" preview)
```

The encoder is realtime, error-resilient, CBR at a target inside the
server's ceiling (`DATA_VIDEO_LIMITS`, handed to the runtime at
construction and on every login), with a long keyframe interval —
receivers ask by PLI, and the server rate-limits those. A camera aims
for 640×480 at up to 30 fps; a screen share for the ceiling's full size
at up to 15.

**Pause stops capturing.** A pause tears the capture bin down (the camera
light goes off) and keeps the sink pad; a resume builds a new one on the
same pad. A publication stays one RTP stream across those bins: the SSRC
and RTP timestamp base are fixed per publication and each payloader
continues the last one's sequence number — without that, a receiver's
jitterbuffer sees the same SSRC jump backwards and drops it.

**The answer waits for the senders' caps.** See voice.md: an answer
written before a freshly linked sender's caps reach `webrtcbin` declares
no SSRC for it. Every live sender without caps is waited on, not only one
bound for this offer: a restart or a resume attaches a capture outside
the offer path, and an answer that once went out on the timeout would
otherwise leave the SSRC out of every answer after it. If the answer
still sends a live video section without its SSRC, the runtime ends that
publication (a capture failure, so a 608) before the answer goes out —
the server would drop it anyway, and a camera that never produces must
not stall every renegotiation.

**Frames end with their streams.** A receive pad that goes away takes
its stream's frame with it and raises `StreamEnded`, unless a rejoin has
already bound another pad to the same mid. A pipeline rebuild clears the
frame store only once the old pipeline has reached Null — an appsink
mid-frame can still put one until then, and a clear before it let the
old session's picture into the new one.

**Frame appsinks don't preroll** (`async=false`). A sink added to a
running pipeline holds its bin in PAUSED until it prerolls, and a live
capture source produces nothing until PLAYING — so without it a new
capture bin waited for some unrelated state change to start.

**Observers.** The UI registers `add_video_observer` closures for
publications, local changes, session state, new frames, ended streams
and per-user volume. They are keyed by the runtime's `id()`, never by a handle's
address: `VoiceRuntime` is a cheap clone and notices raised from
GStreamer callbacks come through a different one.

## The Video panel and subscriptions

A per-connection dock panel, built at startup beside Users. This
client's own publications show as "You" tiles from the preview; a paused
publication keeps its tile, marked paused.

**Tiles fill the panel** (`video_grid.rs`). They are laid out as a
gallery at whichever column count makes them largest, each the shape of
its stream so the picture fills it: a camera 4:3 and a screen 16:9 until
the first frame gives the real shape, and a gallery of mixed shapes
shares 4:3. A peer's frames can't make a tile narrower than 1:4 or wider
than 4:1. The block is centered and a short last row is centered under
the rest. The grid asks the scrolled window for only the height its tiles
need at their smallest (160 wide, and for a portrait stream as tall as
that takes), so the panel scrolls only when they can't all fit at that
size; otherwise they grow into the view. Tiles go
in user order, this client's own first and a user's camera before their
screen, so they don't swap places with the order publications arrive in.

**One tile can be in focus.** It takes the stage at the top, as tall as
the width lets it be at its shape, and the others share what is left
below — a quarter of the height at least — as a smaller gallery of their
own. By default someone else's screen share is in focus (the lowest uid's
if several); clicking a tile, or its focus button, puts it there instead,
and doing it again on the focused tile shows them all alike. A lone tile
has the panel to itself: no stage, no focus button, and a click on it
changes nothing. The choice lasts for the room: while the picked tile is
away the default applies, and it takes the stage again when it comes
back.

**Each tile has controls**, shown while the pointer is over it or one has
keyboard focus: focus, mute and stop watching.

- *Mute* is that user's voice, here only — their volume in the runtime
  set to zero, as the user list's slider does, and put back where it was
  on unmute. It is the same for their camera and their screen. A muted
  user's mute button stays up as the mark of it, and follows the slider:
  the runtime tells video observers of every volume change. "You" tiles
  have none.
- *Stop watching* takes the tile away, so the others get its room, and
  stops receiving it (below). A bar at the bottom of the panel lists what
  isn't being watched; a click there brings it back. The choice lasts for
  the room — across a publication stopping and starting, and a rejoin of
  the same room after a failure — and is dropped on a move to another
  room or a reconnect. On a "You" tile it just hides the preview.

**Visibility is the subscription policy.** While the panel's page is
mapped it subscribes to every publication whose tile is in view or about
to be, less those the user stopped watching; unmapped — another tab, a collapsed dock, a withdrawn window — it
sends the empty set. "About to be" is within half a view height of the
view, since a stream takes a renegotiation and a keyframe to appear (on
the local rig, about a quarter of a second after the 610; up to a second
more when another viewer has just asked the same publisher for a
keyframe, since servers allow one a second). A tile already
received is let go only once it is a view and a half away, so scrolling
back and forth across the edge doesn't renegotiate each time. A tile let
go goes blank, as a stopped publication's does. A burst of 611s costs one
610; scrolling and resizes send one once the view has been still for
150 ms, rather than one along the way. A collapsed
panel or a tile scrolled away costs no bandwidth and no decoding, which
is the spec's reason 610 takes a whole set.

**The user list shows who is publishing** regardless of the panel, from
the 611 (camera or screen glyph beside the voice indicator, dim when
paused). A connection that did not negotiate video — an installation
without the VP8 decoder — gets no 611, and reads the same glyphs from
bits 1 (camera) and 2 (screen) of each voice participant's flags, in
the join reply and every 605. Those carry no pause, so nothing is drawn
dim there.

**The chat says when someone starts sharing**: "*name* turned their
camera on", "*name* started sharing their screen", in the voice room's
chat. The voice model raises it (`video-started`) under the presence
chime's gate — never for this client's own share, and not for the shares
already running when it joined. Those come from the publication bits of
the first participant list since the model was cleared, which the server
sets on every connection. The model is cleared on a leave and on a switch
to another room, and takes participant lists only for the room this
client is in, so that list is the join reply. Every later 611 (or 605,
without video) is compared with it, and a share started after the join
is announced even if the 611 the spec asks for right after the join
never comes. A resume is not a start. The line
follows "Show join / leave in chat", the preference join and leave lines
answer to.

## Publishing

The **camera button** in the voice panel is shown when the server
confirmed video and enabled while in voice in that panel's room, with
access bit 59 and a camera. First press sends 607 and brings the Video
panel forward; after that it pauses and resumes (609). Leaving voice
ends everything, and the server would anyway. While the user is in
voice in that panel's room, outside the sandbox, a `GstDeviceMonitor`
for `Video/Source` runs with a bus watch, so a camera plugged in or out
grays the button or lights it; the monitor stops when the user leaves
and the Settings window is closed. A live camera keeps its button
pressable, so it can be turned off after its camera is unplugged. Until
the monitor has listed a camera, a camera is assumed wherever
`autovideosrc` is installed and the attempt says if there is none: the
PipeWire and libcamera providers hide V4L2's, so a camera only V4L2
reaches, or a virtual one, can be missing from the list. Once one has
been listed, an empty list grays the button.

The **screen button** needs bit 60. On Linux it goes through the
xdg-desktop-portal ScreenCast interface: CreateSession, SelectSources
(monitors or windows, one, `persist_mode` 0), Start, OpenPipeWireRemote,
then `pipewiresrc fd=… path=<node>`. The picker is the consent step, the
same on Wayland, X11 and in the Flatpak sandbox. Elsewhere, where the
platform source (`avfvideosrc`, `d3d11screencapturesrc`) needs no
picker, a confirmation dialog stands in for it. Pressing it again stops
the share (608); the portal session closes when the publication ends,
however it ends. While anything is shared the main window shows an
`AdwBanner` — "You are sharing your screen", with a Stop button.

Inside the **Flatpak sandbox** there is no `/dev/video*`, so the camera
goes through the xdg-desktop-portal Camera interface instead. The first
press of the camera button reads `IsCameraPresent`, calls `AccessCamera`
(the portal asks the user once and remembers the answer), then
`OpenPipeWireRemote`. The runtime keeps that remote and captures through
it until a camera capture fails; then it lets go, since the remote may
have died with a PipeWire restart, and the next press asks the portal for
a fresh one (silently: the grant is remembered). Captures go through it
like this: PipeWire's device provider, given the
remote's fd, lists the cameras, and its elements are `pipewiresrc`s that
connect through a duplicate of the same fd. A device provider too old
to take an fd falls back to `pipewiresrc fd=…` and the remote's default
camera. Access is never asked for earlier than that first press — not at
startup, and not from the settings page, which until then offers only
"First camera found". `IsCameraPresent` is also read, silently, when a
camera button first appears, and watched from then on, so the button
greys out on a machine without one and lights when one is plugged in. `GTKHX_CAMERA_PORTAL=1` takes the portal path on a host
session, for testing it outside the sandbox.

The **camera picker** is a Video group on the Voice settings page
(`voice.camera_device`, a stable device path, empty for the first
camera). Cameras are keyed by path rather than `gst::Device::name()`,
which libcamera leaves NULL. A saved camera the scan doesn't find is
listed as "*path* (missing)" and stays selected, rather than the row
showing "First camera found" over a setting that still names it; inside
the sandbox, where nothing is listed before access is granted, it isn't.
The list is built when the group is first shown, not when Settings
opens, so the camera scan — which wakes every GStreamer device provider
— happens there only when someone actually looks at the picker. The
device monitor started then keeps the list current until the Settings
window closes: a saved camera unplugged meanwhile turns into its
"(missing)" entry, still selected, and plugging it back in replaces the
entry with the camera. A rebuild never rewrites the setting.

Picking another camera while the camera is live moves the publication
onto it at once, the way the audio pickers do (`docs/voice.md`): only
the capture bin's source is replaced. The scaler and the fixed-size caps
behind it absorb a camera with a different native size, so the encoder,
the payloader — its SSRC and sequence numbers — and the self-preview
carry on, and receivers see the same stream with new pictures in it. A
camera that is paused or not published has no capture to move; it reads
the preference when it next starts. A replacement that opens and then
fails reports through the bus like any capture failure, ending the
publication and not the call.

libcamera, one of those providers, logs its enumeration (each camera it
adds, each pixel format it can't use) at INFO and WARN on stderr. `main`
sets `LIBCAMERA_LOG_LEVELS=*:ERROR` before GStreamer loads unless
`GTKHX_DEBUG` includes `voice` (or `all`), so users see only its errors;
an explicit `LIBCAMERA_LOG_LEVELS` in the environment is left alone.

## Tests

- **Unit**: every hxproto builder and parser (mid grammar and ceiling,
  publishers, subscriptions, limits, login limits); the state machine's
  transitions; the frame store and encode targets; that both capture
  kinds produce RTP; the mid scanners agreeing; the senders; the
  presence model; a camera plugged in and out of a test device provider
  reaching a camera watch, the camera list and the button's answer.
- **Proto** (`tests/proto/test_voice.c`): the C ABI for video mids, the
  611 parse, and the login limits.
- **Integration, control** (`test_video_control.c`): bit 10 with bit 2
  and not without; limits per kind; start / pause / resume / stop and
  the 611 each produces; 611 reaching another participant and losing a
  disconnected publisher; the one screen slot; an account without the
  bits refused.
- **Integration, media** (`test_video_media.c`): two real runtimes. A
  publishes a `videotestsrc` camera, B subscribes late and decodes it
  (so a keyframe request has to reach A), A pauses — B must see the 611
  paused flag and its runtime must list the publication as paused — and
  resumes, B unsubscribes and its leg goes, B subscribes again and
  decodes again (a tile scrolled away and back), A stops
  and B's leg goes; every answer declares the microphone's SSRC and
  `cam-send`'s. A second case publishes camera and screen from one user
  and B must decode both as two streams.
- **GUI, by hand** (neither the suite nor CI runs it): two instances
  under Xvfb, driven by `GTKHX_VOICE_AUTOJOIN`,
  `GTKHX_VIDEO_AUTOSTART` (camera on after the join) and
  `GTKHX_VIDEO_AUTOPRESENT` (bring the Video panel forward).
  `GTKHX_VOICE_TEST_VIDEO_SRC` makes every capture a live
  `videotestsrc` (its value names the pattern) and skips the screen
  picker.

The rig's hxd-ng (`tests/hxd-ng`, port 5520, media on 5524/udp) is its
published GHCR image at a pinned digest, and advertises the host's primary IPv4 address:
it is ICE-lite, and libnice never gathers a loopback candidate, so
`127.0.0.1` is an advertisement no check reaches.

## Packaging

Runtime elements beyond voice's: `vpx` (vp8enc/vp8dec), `rtp`
(rtpvp8pay/depay), `videoconvertscale`, `videorate`, `app`, and for
capture `videotestsrc` in tests, `v4l2`/`pipewire` on Linux,
`applemedia` on macOS, `mediafoundation` and `d3d11` on Windows. Debian
needs `gstreamer1.0-plugins-good` and `gstreamer1.0-pipewire`. The GNOME
runtime ships all of them. In the Flatpak, screen sharing and the camera
go through the ScreenCast and Camera portals the manifest already talks
to; neither needs a device permission.

## Known gaps

Follow-ups — untested platforms, RTX, narrower
subscriptions and the rest — are tracked in [BACKLOG.md](../BACKLOG.md).

## What this is not

- **Not simulcast, not H.264, not screen audio, not recording.** All
  reserved in the spec, none defined.
- **Not a second peer connection.** One `webrtcbin`, one ICE session,
  one DTLS handshake.
- **Not a room-wide mode.** A room with video in it costs a
  non-subscriber nothing.
