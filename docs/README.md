# GtkHx documentation

These are **subject references**: each one describes how a subsystem works and why it is
shaped that way. They are not project logs.

The convention, which is worth keeping: while work is in flight a doc may carry a plan.
When the work lands, the plan gets folded into the description of the thing that now
exists, or deleted — git history is the record of how we got here, and a doc that reads
like a changelog stops being read at all. Anything genuinely unfinished lives in a short
"Open" section at the end of the relevant subject doc, described as behaviour rather than
as phases.

For the codebase tour, start at [../CLAUDE.md](../CLAUDE.md). For what is left to build,
[../ROADMAP.md](../ROADMAP.md) (product) and [rust/ROADMAP.md](rust/ROADMAP.md) (the C→Rust
port).

## UI and presentation

| Doc | Subject |
|---|---|
| [chat-view.md](chat-view.md) | GtkHx's side of the chat view, the external Rotulus crate: how it is pinned, how the build finds its header and translations, what GtkHx configures, moving to a new release and changing the widget — and the record of the retired mIRC escape vocabulary. The widget's own design is in [its repository](https://github.com/mishan/rotulus/blob/main/docs/design.md), with the [xtext benchmark](https://github.com/mishan/rotulus/blob/main/docs/xtext-benchmark.md) that decided it. |
| [docking.md](docking.md) | The dock (mullion-gtk): the panels as panes, the corner and the tab strips, windows of their own, per-connection content, layout persistence and the import of layouts from the libpanel dock, and the `dock_bridge` contract. |
| [theming.md](theming.md) | Why the theming model looks the way it does — two unrelated icon systems, and the hidden-base-scale problem that produced the "source art is the honest 100%" rule. |
| [theming-file-format.md](theming-file-format.md) | The theme file schema. Reference. |
| [files-browser.md](files-browser.md) | The orthodox two-pane file manager, the function-key mapping, and the Hotline-specific protocol quirks it has to accommodate. |
| [updates.md](updates.md) | Update notices: which builds check and how, the `updates.json` feed, version ordering and channels, and what packagers should pass to `-Dupdate_check`. |

## Protocol and extensions

| Doc | Subject |
|---|---|
| [hotline-protocol.md](hotline-protocol.md) | The Hotline protocol itself: the 1.9 transaction and field set as the original source and servers implement it, the version forks from 1.2 on, file transfers, tunneling, trackers, every extension since with its spec and numbers, and the original servers as test targets. |
| [tls.md](tls.md) | Transport security: the dedicated-port model, the TOFU trust store, why TOFU is the expected path rather than a degraded one, and the bookmark format's compatibility trick. |
| [tracker-protocol.md](tracker-protocol.md) | HTRK v1 and v3, the full TLV catalogue, and the timed-probe-with-fallback version detection that a v1 tracker's silence forces. |
| [voice.md](voice.md) | Voice chat: the wire contract, the WebRTC pipeline and state machine, and the longest gotchas chapter in the tree. |
| [video.md](video.md) | Camera video and screen sharing on top of the voice room: the extension's contract, subscriptions as visibility, the capture and receive legs, the `webrtcbin` behaviors they had to work around, and screen-share consent. |
| [inline-media.md](inline-media.md) | Images in chat: the upload/handle/fetch pipeline, the field block, and the decoder's security posture. |
| [gif-icons.md](gif-icons.md) | Per-user GIF avatars, discovered by probe because the spec defines no capability bit. |
| [emoji-shortcodes.md](emoji-shortcodes.md) | Emoji that survive servers which don't speak UTF-8, and how the shortcode table is generated. |
| [image-decoding.md](image-decoding.md) | The glycin-backed decoder, the loader-generation compatibility problem, and the three backends. |
| [mhxd-bugs.md](mhxd-bugs.md) | Where mhxd misbehaves — rename and move replacing what's there, names with `/` acting on the parent folder, leaked transfer slots — so a failing test gets checked against the known list first. |
| [janus-bugs.md](janus-bugs.md) | The same for Janus, written as reports to send upstream: what the client sends, what comes back, what should. |
| [hlservd-bugs.md](hlservd-bugs.md) | The same for hlservd, the 1.9 server as a daemon, written to send to its author. |

## Process

| Doc | Subject |
|---|---|
| [coverage.md](coverage.md) | Coverage reporting, what it does and doesn't measure, and the static-analysis setup. |
| [performance.md](performance.md) | The performance-testing plan in four tiers, how to run the benchmarks, the current baseline, and what the measurements have found. |
| [screenshots.md](screenshots.md) | `tools/screenshot.py`: headless screenshots sealed off from the desktop's theme, accent and settings, with a scripted chat room and input steps. |

## Forward-looking

| Doc | Subject |
|---|---|
| [multi-connection.md](multi-connection.md) | The design survey for connecting to several servers at once. The two pivotal decisions are deliberately still open. |

## The Rust port — [rust/](rust/)

| Doc | Subject |
|---|---|
| [rust/ROADMAP.md](rust/ROADMAP.md) | The live inventory of what is still C, in what order it moves, the seams that are permanent rather than pending, the rules that keep C from growing, and what moves to hx-libs next. |
| [rust/networking.md](rust/networking.md) | The `hxnet` stack: connect lifecycle, the three silent-failure axes, proxy support, tracker fetch. |
| [rust/network-endgame.md](rust/network-endgame.md) | What Rust owns of the connection struct today, and the ordered plan for the receive handlers still in C. |
| [rust/glib-interop.md](rust/glib-interop.md) | The Rust↔GLib conventions: reference borrowing across signal emits, the tokio runtime, channels, and what is not allowed. |
| [rust/crate-layout.md](rust/crate-layout.md) | Why the crate graph is shaped the way it is, the single-façade link architecture, and the provenance/licensing audit. |
| [rust/preview-porting.md](rust/preview-porting.md) | A plan, not a description — the preview window has not been ported yet, and this is why and how. |
