//! Outbound effects the runtime should perform.
//!
//! Every effect the state machine wants the world to undergo
//! gets emitted as exactly one `Action` variant. The runtime
//! crate walks the action list returned from
//! `SessionMachine::step` and dispatches each to its concrete
//! handler:
//!
//! - `SendWireFrame` → `hlwrite_chunks` via the C-side FFI shim.
//! - `SetRemoteDescription` / `CreateAnswer` / etc. →
//!   `webrtcbin.emit(…)` via `gstreamer-rs`.
//! - `EmitSignal` → `GtkhxSession::emit_by_name(…)` via the
//!   Phase R3.0 `hxbridge` wrapping shim.
//! - `ArmTimer` / `CancelTimer` → `glib::timeout_add_local` (the
//!   millisecond-granularity main-context entry point; the
//!   `_seconds_local` variant rounds to whole seconds and would
//!   lose the spec's 10 000 ms / 30 000 ms watchdog precision).
//!
//! The point of expressing actions as typed data — not as
//! `Box<dyn FnMut>` callbacks — is that the test suite can
//! pattern-match on the list. `assert_eq!(actions, vec![…])` is
//! how the spec's annotated lifecycle replays land.

use alloc::string::String;
use alloc::vec::Vec;

use crate::event::{ConnectionState, Timeout};

/// What the state machine wants done in response to an event.
///
/// Order in the returned `Vec<Action>` matters — the runtime
/// dispatches them in order, and several pairs (`SetRemoteDescription`
/// then `CreateAnswer`, `SetLocalDescription` then `SendWireFrame`)
/// rely on that for the webrtcbin handshake to land correctly.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub enum Action {
    // ---- Wire ----
    /// Emit a Hotline transaction over the existing TCP control
    /// channel. `opcode` is one of the `HTLC_HDR_VOICE_*` family
    /// (600, 601, 603, 604, 606).
    ///
    /// `body` is an OPAQUE payload the state machine produces — a
    /// small `Vec<u8>` shaped per opcode:
    ///
    /// - JOIN / LEAVE: 4-byte big-endian `cid`.
    /// - SDP_ANSWER: 4-byte `cid` + UTF-8 SDP bytes.
    /// - ICE: 4-byte `cid` + UTF-8 JSON candidate (or empty for
    ///   end-of-candidates).
    /// - MUTE: 4-byte `cid` + 2-byte BE `muted` flag.
    ///
    /// The state machine deliberately doesn't depend on
    /// `hotline-proto`'s `HxChunk` ABI. The runtime side parses
    /// this payload, calls the matching
    /// `hotline_proto::voice::build_voice_*_chunks` builder to
    /// produce a real chunk array, and hands those chunks to
    /// `hlwrite_chunks` via the C FFI. Keeping the wire-format
    /// encoding out of the state machine lets `cargo test -p
    /// hxvoice` run without a `hotline-proto` dependency.
    SendWireFrame {
        /// The HTLC opcode (e.g. `0x258` for VOICE_JOIN).
        opcode: u32,
        /// Opaque payload bytes — see [`SendWireFrame`] for the
        /// per-opcode shape the runtime must re-pack into
        /// `hotline-proto`'s `HxChunk` array before calling
        /// `hlwrite_chunks`.
        body: WireFrameBody,
    },

    // ---- webrtcbin ----
    /// Hand the SDP to `webrtcbin.set-remote-description`. Always
    /// followed by `CreateAnswer` in the offer-arrived branch.
    SetRemoteDescription { sdp: String },
    /// Tell webrtcbin to generate the answer SDP. The runtime
    /// arms a `gst::Promise::new_with_change_func` callback that
    /// turns into `Event::WebrtcAnswerCreated` when the answer
    /// is ready.
    CreateAnswer,
    /// Hand the answer SDP back to webrtcbin via
    /// `set-local-description`. Always followed by
    /// `SendWireFrame(603)` so the server gets the answer.
    SetLocalDescription { sdp: String },
    /// Hand a remote ICE candidate to webrtcbin via
    /// `add-ice-candidate`.
    AddRemoteIce { candidate_json: String },

    // ---- Audio pipeline (managed by hxvoice-runtime) ----
    /// A receive pad appeared for the given mid; map it to the
    /// associated user_id and start playback. The runtime
    /// constructs the `rtppcmudepay ! mulawdec ! audioconvert !
    /// audioresample ! autoaudiosink` bin and links it to the
    /// new pad.
    StartReceivePipeline { mid: String, user_id: u16 },
    /// A receive pad disappeared (mid slot recycled or
    /// participant left). Tear down the matching playback bin.
    StopReceivePipeline { mid: String },
    /// Apply the local mute state to the send leg of the
    /// pipeline. `muted = true` drops outgoing buffers; `false`
    /// resumes them.
    SetSendPipelineMute { muted: bool },

    // ---- GtkhxSession signals (model → view bridge) ----
    /// Emit a GtkhxSession signal so the UI updates without the
    /// state machine knowing anything about GLib. The runtime
    /// maps `SignalKind` to the concrete signal name + payload.
    EmitSignal { kind: SignalKind, payload: SignalPayload },

    // ---- Timers ----
    /// Arm a one-shot timer of the given kind to fire `ms`
    /// milliseconds from now. The runtime tracks it (so it can
    /// implement `CancelTimer`) and dispatches `Event::Timeout`
    /// back to the state machine on expiry.
    ArmTimer { kind: TimerKind, ms: u32 },
    /// Cancel a previously armed timer. No-op if the timer
    /// already fired or wasn't armed.
    CancelTimer { kind: TimerKind },

    // ---- Lifecycle ----
    /// Tear down the runtime-side resources tied to the current
    /// session — close `webrtcbin`, stop the audio pipelines,
    /// release any RTP-bin caps. **This action describes runtime
    /// teardown, not state-machine destruction.** Whether the
    /// runtime drops the `SessionMachine` afterwards is keyed off
    /// the resulting state, not off seeing this action:
    ///
    /// - When the machine has transitioned to
    ///   [`crate::state::SessionState::Leaving`] (the terminal
    ///   state), the runtime drops the machine; a fresh voice
    ///   session must construct a new one in `Idle`.
    /// - When `TearDown` accompanies a mid-session room switch
    ///   (the implicit-leave path triggered by
    ///   [`crate::event::Event::JoinRequested`] for a different
    ///   `cid` while in voice — see the `state.rs` module doc),
    ///   the machine keeps running. It walks back to `JoinSent`
    ///   with fresh per-room state in the same `step()` call;
    ///   the runtime must rebuild `webrtcbin` and the pipelines
    ///   to match the new room but keep the same machine
    ///   instance.
    ///
    /// Runtime implementations should check
    /// `SessionMachine::state()` after the action list finishes
    /// dispatching, not infer lifetime from the presence of this
    /// action alone.
    TearDown,
}

/// Body bytes for a wire frame. Owned `Vec<u8>` so the state
/// machine can hand the runtime a payload it doesn't have to
/// copy.
///
/// Wrapped in a struct rather than aliased to `Vec<u8>` because
/// future work may want to carry the chunk array as a typed
/// `Vec<HxChunkRef>` instead — keeping the wrapping point lets
/// us evolve without rippling through the public surface.
#[derive(Clone, PartialEq, Eq)]
pub struct WireFrameBody(pub Vec<u8>);

impl core::fmt::Debug for WireFrameBody {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "WireFrameBody({} bytes)", self.0.len())
    }
}

/// The GtkhxSession signals the state machine asks the runtime
/// to emit. Concrete signal names + boxed payload types live on
/// the runtime side; this enum is the runtime's lookup key.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum SignalKind {
    /// `voice-room-status` — emitted in response to a 605
    /// ROOM_STATUS notification updating the participant list.
    /// UI uses it to drive the speaker indicator in the user
    /// list. Phase 8.C step 1 emits this on
    /// `Event::ParticipantsUpdated` only — earlier draft docs
    /// mentioned join / leave transitions but the state machine
    /// doesn't fire it on those (the `StateChanged` signal
    /// covers the lifecycle UI updates).
    RoomStatus,
    /// `voice-state-changed` — high-level session state for the
    /// UI's headerbar indicator. Fires on every state transition
    /// the machine performs: Idle → JoinSent → OfferPending →
    /// Connecting → Connected, plus the Leaving paths from any
    /// of those.
    StateChanged,
    /// `voice-error` — surfaces a user-facing error toast.
    /// Fired on `ServerTaskError` (any opcode) and on the
    /// `ConnectionState::Failed` path that collapses the session
    /// to Leaving.
    Error,
    /// `voice-mute-changed` — UI mute toggle reflection. Fires
    /// when our local mute state changes via
    /// `MuteToggleRequested`. The state machine does NOT
    /// currently derive the local mute flag from 605 ROOM_STATUS
    /// (server-reported flips don't change our local state); a
    /// future revision may wire that path through.
    MuteChanged,
    /// `voice-speaker-changed` — per-participant talking indicator.
    /// Fired by the runtime's periodic per-pad RTP activity
    /// evaluator (NOT by the state machine), so the payload is
    /// always [`SignalPayload::SpeakerChanged`].
    ///
    /// State-machine code never produces this — it's emitted from
    /// the runtime side directly via
    /// `backend.borrow_mut().emit_signal`, bypassing the state-
    /// machine `Action::EmitSignal` channel. Including it in this
    /// enum keeps the signal taxonomy consistent so the runtime's
    /// `CallbackBackend::emit_signal` dispatch arm + the C-side
    /// `SignalCallbacks` struct have the same vocabulary, even
    /// though the producer is different from the rest.
    SpeakerChanged,
}

/// Typed payload carried with an `EmitSignal` action. Variants
/// match `SignalKind` 1:1 — pairing them keeps the runtime's
/// dispatch loop a single `match` rather than a property-bag
/// downcast.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub enum SignalPayload {
    RoomStatus {
        cid: u32,
        connection_state: ConnectionState,
    },
    StateChanged {
        /// `crate::state::SessionState` discriminant the
        /// machine just entered. Carried as a copy (the enum is
        /// `Copy`) so the runtime can hand it to the signal's
        /// boxed-type wrapper.
        new_state: crate::state::SessionState,
    },
    Error {
        /// Human-readable text suitable for a toast.
        text: String,
    },
    MuteChanged {
        muted: bool,
    },
    /// Per-participant speaker activity transition. The runtime
    /// fires this whenever a uid's "speaking" state flips between
    /// the runtime's per-pad RTP-activity probe ticks (default
    /// cadence: 200 ms). `uid` is the Hotline user id from the
    /// SDP `a=mid:user-{uid}` label resolved at pad-added; the C
    /// side cross-references it with the chat user list to repaint
    /// the indicator column.
    SpeakerChanged {
        uid: u16,
        is_speaking: bool,
    },
}

/// Which timer is being armed / cancelled / fired.
///
/// Identifies the spec's §"Session Timeout and Failure" table
/// entries. `Event::Timeout` carries the same enum
/// (re-exported as `event::Timeout`); pairing them rather than
/// reusing a single `Timeout` enum keeps the action-side and
/// event-side concerns in their respective module's docs.
pub type TimerKind = Timeout;
