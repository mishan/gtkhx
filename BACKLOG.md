# Backlog

Known gaps and follow-ups that are smaller than a roadmap item: things a review
turned up, or a feature left deliberately unfinished. Product direction lives in
[ROADMAP.md](ROADMAP.md). When an item is done, delete it; git history is the
record.

## Video

### Gaps

- **Screen sharing on macOS and Windows** is wired up but untested. So is a Linux
  desktop without a ScreenCast portal, which has no `ximagesrc` fallback.
- **RTX and NACK on receive** (`do-nack`), and REMB. The SSRC map already
  follows `ssrc-group:FID`, but the answer pins VP8 without RTX. If a server did
  negotiate RTX, its stream would expose a pad that can't link to
  `rtpvp8depay`.
- **Narrower subscriptions.** Tiles that are scrolled out of view, a
  backgrounded window, and a metered-connection preference should all shrink
  the receive set.
- **A chat notice when someone starts sharing**, gated like the voice chimes.
- **Renderer cost.** The panel builds a new `GdkMemoryTexture` for every frame,
  and GTK uploads each one to the GPU. `gtk4paintablesink` would keep frames on
  the GPU, but neither the GNOME runtime nor the bundle scripts ship it. A
  hidden Video page also still turns every local preview frame into a texture.
- **Camera hotplug.** The camera button's availability comes from the last
  device enumeration, which runs the first time it's needed and again when the
  camera picker opens or a capture starts. A camera plugged in after that only
  appears once one of those runs again. A `GstDeviceMonitor` bus watch would
  keep it current.
- **Settings don't show a missing camera.** When the saved camera is unplugged,
  the combo shows "First camera found" while the stored value still names the
  old device. A "(missing)" entry would be clearer.
- **Tile names** only update when the room refreshes, not when someone changes
  their nickname.
- **Accessible labels** for the icon-only camera and screen toggles and the
  user-list video glyph. They have tooltips; an explicit label would be better.

### Robustness

- **A refused 609 isn't rolled back.** The machine records the paused bit
  before the server confirms it, so after a refusal the local state and the
  server's disagree until the next toggle. The error carries only the opcode, so
  telling a camera refusal from a screen one needs the kind in the task label,
  the way 607 does it.
- **A late 607 refusal can clear a newer publication in the same room.** The
  refusal now carries its room, so one from a room the client has left is
  ignored. A start → stop → start inside one round trip can still have the first
  refusal end the second. A start generation (or the transaction id) in the
  event would settle it.
- **The caps wait has no recovery.** If an answer goes out on the 1500 ms
  timeout without a sender's `a=ssrc`, the next offer sees that pad as already
  bound and answers immediately, again without it. Either wait on every bound
  sender that has no caps, or fail the publication when its SSRC is missing from
  the answer.
- **Frames outlive their streams.** `pad-removed` doesn't remove a video mid's
  frames or send `StreamEnded`. `reset_legs` clears the frame store before the
  old pipeline reaches Null, so the old appsinks can put frames back, including
  the self preview.
- **SSRC routing before the offer is indexed.** RTP for a new SSRC that arrives
  before `set-remote-description` has indexed the offer falls back to the
  transceiver's mid, which is the misrouting the map exists to prevent. The map
  also isn't restored if that call fails.

## Voice

- **`voice_rejoin_media` against Janus is intermittent.** B sometimes stays in
  ICE Connecting after a rejoin or a concurrent join, and `vad_speaker`
  sometimes misses A's speaking flag. Both happen on `main` too, and the rate
  didn't change when a stale-answer race in the runtime was fixed. The rig's
  Janus is an old pinned build, and the snapshot it came from is no longer
  published. Moving the rig to a current Janus comes first; if the flake
  survives that, it's ours.
- **Every first answer now waits for the microphone's caps**, up to 1500 ms.
  `voice_rejoin_media` asserts that every answer declares the send SSRC, so a
  slow audio source would fail that assertion rather than hang.

## UI and theming

- **Hand-review the symbolic icon picks (Misha).** The mapping from each classic
  pixmap to its symbolic stand-in (`symbolic_icons[]` in `src/gtkhx_icon.c`) was
  chosen by searching Adwaita's symbolic set and the icon-development-kit, not by
  browsing them. Go through both — the Icon Library app is the easiest way — and
  swap in anything that fits a button better. The weakest current matches:
  kick (`system-log-out`), tasks (`view-list`), message (`mail-unread`), post
  news (`mail-message-new`), news posts (`text-x-generic`), the drop box and
  download (both `folder-download`), upload (`document-send`), disk images
  (`media-optical`), and HTML files (`text-x-generic`; Adwaita has no symbolic
  HTML icon). A kit icon has to be converted to filled paths before it's
  vendored — see `src/icons/README.md`.

## Tests and rig

- The pause check in `video_media` stops at "frames stop arriving". It never
  checks that B saw the 611 paused flag.
- `tests/hxd-ng/Dockerfile` builds from the floating `rust:1-trixie`, so each
  new upstream tag invalidates the layer cache. Pin a version next to
  `HXD_NG_REV`, and fetch just the revision instead of cloning the whole
  history.
- The hxd-ng entrypoint falls back to advertising `127.0.0.1` on an IPv6-only or
  loopback-only host, and the media tests then fail with "never reached
  CONNECTED". The README and the compose comment say "addresses" where only one
  is advertised.
- hxd-ng's `xfer_port` (5521) isn't set in the rig config. Nothing uses it yet.

## Legacy servers

- hlserver.com eventually closes the connection, possibly on an idle timeout.
  If that is what it is, say so rather than reporting a plain disconnect.
- hlserver.com broadcasts "0 command(s) at a time" on login.

## hx-libs (`hxproto`)

- `SdpSummary::has_vp8` is true if any line in the whole SDP matches, and the
  match is case-sensitive. That's the same limitation as `has_pcmu`, but the doc
  comment promises a per-section check.
- `VideoReply.cid` is 0 when `CHAT_ID` is missing, so a missing field can't be
  told apart from the public chat. `Option<u32>` would be clearer. The voice
  reply has the same shape.
- `build_video_stop_chunks` asks for six bytes of scratch even when it sends no
  kind.
- `parse_login` files every non-camera kind as screen. That's correct while
  `VideoKind` has two variants; matching `Screen` explicitly would stay correct
  when a third arrives.
- `MidLabel` isn't `#[non_exhaustive]`, so new variants break exhaustive matches
  downstream.
- The video builders don't reject uid 0 or duplicate streams. The spec doesn't
  forbid either, and hxd-ng tolerates both.
