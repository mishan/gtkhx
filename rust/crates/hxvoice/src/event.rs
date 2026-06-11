//! Inbound events the state machine reacts to.
//!
//! Every entry point into the runtime — UI click, server wire
//! frame, GStreamer signal, GLib timer fire — gets normalised into
//! exactly one `Event` variant before reaching the state machine.
//! That uniform shape is what makes `cargo test -p hxvoice` worth
//! anything: replay an event trace, assert on the action list,
//! done.
//!
//! Variants stay typed-data only. No `Box<dyn …>`, no
//! `glib::Object` refs, no GStreamer types. Variable-length
//! payloads (`sdp`, `candidate_json`, the participant `Vec`)
//! are OWNED (`String` / `Vec<Participant>`) so the state
//! machine can outlive any single event-source buffer the
//! runtime handed in. The clone cost is bounded — SDP blobs
//! stay under a few KB per spec, ICE candidate JSON is ~200
//! bytes, and the participant list caps at 16 entries × 6
//! bytes per the spec's room-size default.

use alloc::string::String;
use alloc::vec::Vec;

/// All inbound events the state machine handles.
///
/// Categorised by source:
/// - **UI** — `JoinRequested`, `LeaveRequested`, `MuteToggleRequested`.
/// - **Wire** (incoming from the Hotline control channel via
///   `hotline-proto`'s parsers): `SdpOfferReceived`,
///   `IceCandidateReceived`, `EndOfRemoteCandidates`,
///   `ParticipantsUpdated`, `ServerTaskError`.
/// - **WebRTC** (from `webrtcbin` signals): `WebrtcPadAdded`,
///   `WebrtcPadRemoved`, `WebrtcAnswerCreated`,
///   `WebrtcLocalIceGathered`, `WebrtcConnectionStateChanged`.
/// - **Time** — `Timeout`. The runtime arms `glib::timeout_*`
///   timers in response to `Action::ArmTimer` and fires this
///   event when they expire.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub enum Event {
    // ---- UI ----
    /// User clicked "Join voice" on a chat tab. `cid` is the
    /// chat-room id (`0` = public chat). Per spec, joining voice
    /// in a second room implicitly leaves the first — the state
    /// machine handles the teardown serialisation.
    JoinRequested { cid: u32 },

    /// User clicked "Leave voice" on the active voice room.
    /// `cid` is included for sanity checking; the machine
    /// already tracks the active cid on `JoinSent`+ states.
    LeaveRequested { cid: u32 },

    /// User toggled mute (button, PTT key, etc.). `muted == true`
    /// means "stop sending"; `false` means "resume".
    MuteToggleRequested { muted: bool },

    // ---- Wire ----
    /// Server sent HTLS_HDR_VOICE_SDP_OFFER (602). Carries the
    /// raw SDP blob; the runtime hands it to webrtcbin via
    /// `Action::SetRemoteDescription`. Renegotiation arrives via
    /// this event too — the state machine handles the offer-in-
    /// flight collision (drop the older offer, answer only the
    /// newest, per spec §"Renegotiation Flow").
    SdpOfferReceived { cid: u32, sdp: String },

    /// Server sent HTLS_HDR_VOICE_ICE (604) carrying a single
    /// trickle ICE candidate. (604 is the bidirectional opcode
    /// number — both `HTLC_HDR_VOICE_ICE` and
    /// `HTLS_HDR_VOICE_ICE` are defined in `hotline.h`; the
    /// server-sent direction is HTLS.) Empty `candidate` string
    /// inside the JSON would normally be the end-of-candidates
    /// marker; the runtime intercepts that case and emits
    /// `EndOfRemoteCandidates` instead so the state machine sees
    /// the distinction clearly.
    IceCandidateReceived { cid: u32, candidate_json: String },

    /// Server's ICE trickle is complete. Either because the JSON
    /// candidate was an explicit `{"candidate":""}` shorthand or
    /// because the VOICE_ICE chunk arrived empty. Currently
    /// informational; the WebRTC stack doesn't require the
    /// signal but the spec calls it out.
    EndOfRemoteCandidates { cid: u32 },

    /// Server sent HTLS_HDR_VOICE_ROOM_STATUS (605). Carries the
    /// updated participant list — used to drive the user-list
    /// speaker indicator and to validate mid → user_id mappings
    /// against renegotiation.
    ParticipantsUpdated { cid: u32, entries: Vec<Participant> },

    /// Server sent a TASK reply with the error bit set for one of
    /// the voice-family opcodes we issued. Includes the parsed
    /// error text where available so the runtime can surface it
    /// as a toast.
    ServerTaskError(ServerError),

    // ---- WebRTC ----
    /// `webrtcbin::pad-added` fired with a sink pad bound to the
    /// given mid label (`"send"` or `"user-N"`). The runtime
    /// looks up the mid in the cached SDP and emits
    /// `Action::StartReceivePipeline` so audio playback starts.
    WebrtcPadAdded { mid: String },

    /// `webrtcbin::pad-removed` fired for an existing receive
    /// leg. Triggers `Action::StopReceivePipeline`.
    WebrtcPadRemoved { mid: String },

    /// `webrtcbin::create-answer` promise resolved with the SDP
    /// answer. The runtime then sets the local description and
    /// sends a 603 over the wire.
    WebrtcAnswerCreated { sdp: String },

    /// `webrtcbin::on-ice-candidate` fired with a local candidate
    /// ready to be sent to the server via 604.
    WebrtcLocalIceGathered { candidate_json: String },

    /// `webrtcbin::peer-connection-state` property changed.
    /// Drives the `JoinSent → Connecting → Connected` walk plus
    /// the `Failed` / `Disconnected` paths.
    WebrtcConnectionStateChanged { state: ConnectionState },

    // ---- Time ----
    /// A `Action::ArmTimer` timer expired. The runtime passes back
    /// the same `TimerKind` (alias re-exported here via
    /// `Timeout`) so the state machine can route the
    /// expiry to the right handler.
    Timeout { kind: Timeout },
}

/// One voice participant as carried in a 605 ROOM_STATUS update.
/// Stripped down to the fields the state machine cares about; the
/// raw 6-byte wire layout lives in `hotline_proto::voice`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Participant {
    /// Hotline user id.
    pub user_id: u16,
    /// Codec id (0 = PCMU; everything else reserved per spec).
    pub codec_id: u16,
    /// Server-side mute flag (bit 0 of the wire `flags` field).
    pub muted: bool,
}

/// Server-side error reported as a TASK reply with the in_error
/// bit set. Carries the opcode that errored so the state machine
/// can decide which transition to apply (a JOIN error tears the
/// session down; a MUTE error is a benign rollback).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ServerError {
    /// Which client → server opcode produced the error: 600
    /// (Join), 601 (Leave), 603 (SDP Answer), 604 (ICE), 606
    /// (Mute).
    pub origin_opcode: u32,
    /// `DATA_ERROR_TEXT` body as UTF-8. Spec says this is
    /// human-readable; the runtime surfaces it verbatim as a
    /// toast.
    pub text: String,
}

/// Mirrors the subset of `gst_webrtc::WebRTCPeerConnectionState`
/// the state machine actually branches on. Keeping the enum here
/// rather than re-using the GStreamer one is the load-bearing
/// "no GStreamer types in the state machine's public surface"
/// rule — the runtime crate translates between the two.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnectionState {
    /// Peer connection is being assembled (ICE checks pending,
    /// DTLS in flight).
    Connecting,
    /// ICE + DTLS complete; media should be flowing.
    Connected,
    /// `WebRTCPeerConnectionState::Disconnected` —
    /// connectivity loss the stack thinks is temporary.
    Disconnected,
    /// `WebRTCPeerConnectionState::Failed` — unrecoverable.
    /// State machine treats this as a hard teardown.
    Failed,
    /// `WebRTCPeerConnectionState::Closed`.
    Closed,
}

/// Which timer expired. The runtime arms timers via
/// `Action::ArmTimer { kind, ms }` and fires `Event::Timeout {
/// kind }` when they expire.
///
/// Values mirror the spec's §"Session Timeout and Failure" table:
/// - `JoinReply` — no SDP answer 10s after JOIN reply (tear down).
/// - `IceConnectivity` — no successful ICE pair in 30s (tear down).
/// - `Dtls` — DTLS handshake didn't complete in 10s (tear down).
/// - `Media` — no RTP/RTCP from peer for 30s (tear down + leave).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Timeout {
    JoinReply,
    IceConnectivity,
    Dtls,
    Media,
}
