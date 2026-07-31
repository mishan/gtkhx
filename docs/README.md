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
| [chat-view.md](chat-view.md) | The chat rendering stack: the dependency-free layout engine, the GTK4 widget, the message model, scroll anchoring, markdown, selection and search — and the record of the retired mIRC escape vocabulary. |
| [chat-view-benchmark.md](chat-view-benchmark.md) | A dated, unrepeatable measurement of the old widget against its replacement, plus two documented ways the benchmark lied before it was trusted. |
| [docking.md](docking.md) | The libpanel dock: the recursive split tree, panel ownership, undocking and drag-and-drop, layout persistence, a long gotchas chapter, and why the dock stays C. |
| [theming.md](theming.md) | Why the theming model looks the way it does — two unrelated icon systems, and the hidden-base-scale problem that produced the "source art is the honest 100%" rule. |
| [theming-file-format.md](theming-file-format.md) | The theme file schema. Reference. |
| [files-browser.md](files-browser.md) | The orthodox two-pane file manager, the function-key mapping, and the Hotline-specific protocol quirks it has to accommodate. |

## Protocol and extensions

| Doc | Subject |
|---|---|
| [tls.md](tls.md) | Transport security: the dedicated-port model, the TOFU trust store, why TOFU is the expected path rather than a degraded one, and the bookmark format's compatibility trick. |
| [tracker-protocol.md](tracker-protocol.md) | HTRK v1 and v3, the full TLV catalogue, and the timed-probe-with-fallback version detection that a v1 tracker's silence forces. |
| [voice.md](voice.md) | Voice chat: the wire contract, the WebRTC pipeline and state machine, and the longest gotchas chapter in the tree. |
| [inline-media.md](inline-media.md) | Images in chat: the upload/handle/fetch pipeline, the field block, and the decoder's security posture. |
| [gif-icons.md](gif-icons.md) | Per-user GIF avatars, discovered by probe because the spec defines no capability bit. |
| [emoji-shortcodes.md](emoji-shortcodes.md) | Emoji that survive servers which don't speak UTF-8, and how the shortcode table is generated. |
| [image-decoding.md](image-decoding.md) | The glycin-backed decoder, the loader-generation compatibility problem, and the three backends. |

## Process

| Doc | Subject |
|---|---|
| [coverage.md](coverage.md) | Coverage reporting, what it does and doesn't measure, and the static-analysis setup. |

## Forward-looking

| Doc | Subject |
|---|---|
| [multi-connection.md](multi-connection.md) | The design survey for connecting to several servers at once. The two pivotal decisions are deliberately still open. |

## The Rust port — [rust/](rust/)

| Doc | Subject |
|---|---|
| [rust/ROADMAP.md](rust/ROADMAP.md) | The live inventory of what is still C, in what order it moves, and the seams that are permanent rather than pending. |
| [rust/networking.md](rust/networking.md) | The `hxnet` stack: connect lifecycle, the three silent-failure axes, proxy support, tracker fetch. |
| [rust/network-endgame.md](rust/network-endgame.md) | What Rust owns of the connection struct today, and the ordered plan for the receive handlers still in C. |
| [rust/glib-interop.md](rust/glib-interop.md) | The Rust↔GLib conventions: reference borrowing across signal emits, the tokio runtime, channels, and what is not allowed. |
| [rust/crate-layout.md](rust/crate-layout.md) | Why the crate graph is shaped the way it is, the single-façade link architecture, and the provenance/licensing audit. |
| [rust/preview-porting.md](rust/preview-porting.md) | A plan, not a description — the preview window has not been ported yet, and this is why and how. |
