//! Pure-Rust voice-chat state machine.
//!
//! `hxvoice` is the no_std-friendly heart of the Phase 8.C voice
//! runtime. It models the complete lifecycle of a single
//! fogWraith-spec voice session — join, SDP offer/answer, ICE
//! trickle, participant tracking, mute toggle, renegotiation,
//! timeout-driven failure, and leave — as a typed
//! [`state::SessionMachine`] driven by [`event::Event`]s and
//! emitting [`action::Action`]s.
//!
//! ## Why a separate crate
//!
//! Three load-bearing properties of this layering:
//!
//! 1. **Zero non-Rust deps.** No GLib, no GStreamer, no GTK, no
//!    OS surface. `cargo test -p hxvoice` runs in any container,
//!    on any architecture, regardless of audio device or
//!    GStreamer install state. CI catches every state-machine
//!    regression before the runtime layer (which does have those
//!    dependencies) even compiles.
//!
//! 2. **One place for the protocol logic.** All "when do we send
//!    603 after a 602?" and "what does the queue look like during
//!    renegotiation?" decisions live here. The runtime crate is
//!    deliberately mechanical: pump events in, walk the action
//!    list, repeat.
//!
//! 3. **Replayable.** Because every transition is `step(&mut
//!    self, Event) -> Vec<Action>`, tests are just event traces.
//!    The spec's annotated lifecycle examples ("B joins room
//!    with A already in voice", "B leaves") replay verbatim;
//!    regressions become a one-line `assert_eq!` on the action
//!    list.
//!
//! ## What this crate is NOT
//!
//! - Not an FFI surface. The C side never talks to `SessionMachine`
//!   directly — `hxvoice-runtime` wraps it and exposes opaque
//!   handles in `gtkhx_voice_runtime_*` shims.
//! - Not a GStreamer driver. Actions like `CreateAnswer` describe
//!   _what_ should happen; the runtime crate decides _how_
//!   (`webrtcbin.emit("create-answer", …)` in practice).
//! - Not a wire-format library. The action list carries
//!   `SendWireFrame { opcode, body }` payloads where `body` is a
//!   small opaque `Vec<u8>` (cid + payload bytes per opcode); the
//!   runtime crate parses this and calls `hotline-proto`'s
//!   `build_voice_*_chunks` to produce a real `HxChunk` array
//!   before handing the chunks to `hlwrite_chunks` via the FFI.
//!   Building the chunks is `hotline-proto`'s job, not this
//!   crate's.
//!
//! See `docs/voice-chat-plan.md` §4 and §5.C for the full
//! architectural rationale and the spec lifecycle replay
//! examples this crate's tests pin.

#![no_std]
extern crate alloc;

pub mod action;
pub mod event;
pub mod state;

pub use action::{Action, SignalKind, TimerKind};
pub use event::{ConnectionState, Event, Participant, ServerError, Timeout};
pub use state::{SessionMachine, SessionState};
