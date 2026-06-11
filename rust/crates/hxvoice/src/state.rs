//! The `SessionMachine` and its single entry point, `step`.
//!
//! Each transition is a pure function: `(state, event) ->
//! (state', actions)`. The state is held in the machine for
//! ergonomic reasons (the runtime would have to thread it
//! through otherwise), but every state mutation is a side
//! effect of one `step` call and no global state is touched.
//!
//! ## State diagram
//!
//! ```text
//!                  JoinRequested(cid)
//!     ┌──────────► JoinSent ──────────┐
//!     │             │                 │
//!     │             │ SdpOfferReceived│
//!     │             ▼                 │ Server task error
//!     │          OfferPending         │ (any opcode)
//!     │             │                 │
//!     │             │ AnswerCreated   │
//!     │             ▼                 │
//!             Leaving ◄── Connecting ◄┘
//!  (terminal)              │
//!                          │ ConnectionState::Connected
//!                          ▼
//!                      Connected
//!                          │
//!                          │ LeaveRequested
//!                          ▼
//!                       Leaving (terminal)
//! ```
//!
//! Connected ↔ OfferPending happens during renegotiation (a new
//! 602 arriving while we're already streaming) — the transition
//! is identical to the JoinSent → OfferPending step. Failures
//! (`WebrtcConnectionStateChanged::Failed`, server task error,
//! any `Timeout`) collapse to `Leaving`, which is a **terminal
//! state** — the runtime drops the `SessionMachine` once it
//! observes the post-step `state() == Leaving`; a fresh session
//! constructs a new machine in `Idle`. There is no `Leaving →
//! Idle` step in this enum.
//!
//! **Important — don't drop the machine on every `TearDown`.**
//! `Action::TearDown` describes runtime-resource teardown
//! (close webrtcbin, stop pipelines), not state-machine
//! destruction. The mid-session-room-switch path
//! (`JoinRequested { cid }` for a *different* `cid` while in
//! voice; see `event.rs`) also emits `TearDown`, but the
//! machine continues running and walks back to `JoinSent` with
//! fresh per-room state in the same `step()` call. The keying
//! rule is "drop if and only if `state() == Leaving` after the
//! step", never "drop on seeing `TearDown` in the action list".

use alloc::string::{String, ToString};
use alloc::vec;
use alloc::vec::Vec;
use core::mem;

use hashbrown::HashMap;
use crate::action::{
    Action, SignalKind, SignalPayload, TimerKind, WireFrameBody,
};
use crate::event::{
    ConnectionState, Event, Participant, ServerError, Timeout,
};

/// What the machine is currently doing.
///
/// `Copy` so it's cheap to hand to the UI signal as a payload
/// without cloning. The variants match the spec lifecycle 1:1;
/// renegotiation cycles through `OfferPending → Connecting →
/// Connected → OfferPending` without dropping back to `Idle`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum SessionState {
    /// No voice room joined. The **starting state only** — there
    /// is no `Leaving → Idle` transition in this enum, so a
    /// machine that has reached `Leaving` never re-enters `Idle`.
    /// A fresh voice session constructs a new `SessionMachine`
    /// (which starts in `Idle`); the runtime drops the previous
    /// one when it sees the post-step state hit `Leaving`. See
    /// the module-level doc for the full lifetime contract.
    Idle,
    /// We sent VOICE_JOIN (600); waiting for the server's reply
    /// or its accompanying SDP offer (602). Both arrive in
    /// close succession in practice; the JoinReply timer guards
    /// against a misbehaving server that never replies.
    JoinSent,
    /// We received an SDP offer (602) and asked webrtcbin to
    /// create an answer. Waiting for the
    /// `WebrtcAnswerCreated` event. Spec-driven serialisation:
    /// a second 602 arriving in this state replaces the
    /// in-flight offer (no queue), per the
    /// "client MUST discard the previous unanswered offer" rule.
    OfferPending,
    /// We sent VOICE_SDP_ANSWER (603) and are waiting for ICE +
    /// DTLS to complete. The state-change watcher transitions
    /// us out of this on the first `Connected` event.
    Connecting,
    /// Media is flowing. Renegotiation pulls us back to
    /// `OfferPending`; leave / failure pushes us to `Leaving`.
    Connected,
    /// We sent VOICE_LEAVE (601) or are tearing down due to a
    /// failure. **Terminal state** for this machine — the runtime
    /// drops the `SessionMachine` after dispatching the `TearDown`
    /// action that accompanied the transition here. A fresh
    /// session constructs a new machine in `Idle`; this enum has
    /// no `Leaving → Idle` step.
    Leaving,
}

/// Per-session state. One instance per active voice session.
///
/// Construction starts in `Idle` with no active `cid`. The
/// runtime calls `step` to drive every transition.
#[derive(Debug, Default, Clone)]
pub struct SessionMachine {
    state: SessionState,
    /// Currently active (or pending) chat-room id. Populated on
    /// `JoinRequested` (both the initial JOIN and the mid-session
    /// room-switch path), and cleared internally by `fail()` when
    /// the machine walks itself to terminal `Leaving`. There is
    /// no `Leaving → Idle` transition, so this is the only
    /// in-machine clear path; the explicit-leave handler doesn't
    /// touch this field because the runtime drops the whole
    /// machine afterwards.
    ///
    /// `0` is a valid cid (public chat), so we use `Option<u32>`
    /// rather than a sentinel.
    active_cid: Option<u32>,
    /// `a=mid:user-N` label → user_id map, populated from the
    /// last SDP we accepted. Looked up by `WebrtcPadAdded` so we
    /// know which user a receive leg corresponds to. Cleared on
    /// teardown.
    mid_to_user: HashMap<String, u16>,
    /// Last seen participant list, indexed by user_id. Updated
    /// on `ParticipantsUpdated`; consumed by the runtime layer
    /// via [`SessionMachine::participants`] when it builds the
    /// boxed payload for the `voice-room-status` signal.
    participants: HashMap<u16, Participant>,
    /// Local mute state. Reflects what we've told the server (and
    /// our send pipeline) — not what the server reports about
    /// us. Tracked separately so a redundant
    /// `MuteToggleRequested` is a no-op.
    muted: bool,
    /// Renegotiation queue (Phase 8.C step 6). When an
    /// `SdpOfferReceived` arrives while we're already in
    /// `OfferPending`, the new offer is parked here instead of
    /// being dispatched to webrtcbin immediately — issuing a
    /// second `SetRemoteDescription`+`CreateAnswer` pair before
    /// the first answer has flowed through corrupts webrtcbin's
    /// internal state machine. Drained on
    /// `OfferPending`+`WebrtcAnswerCreated`: after the current
    /// answer flushes (`SetLocalDescription` + 603 send), the
    /// queued offer becomes a new `SetRemoteDescription` +
    /// `CreateAnswer` pair in the same step, and the machine
    /// stays in `OfferPending` for the queued offer's answer
    /// flow.
    ///
    /// Replaces, not appends — a third offer arriving before the
    /// first answer would just supersede the queued one, per spec
    /// §"Renegotiation Flow" ("process only the newest one").
    /// `Option<(cid, sdp)>`; the cid is recorded so the wrong-cid
    /// guard can run at drain time too (defensive: the cid that
    /// was active at enqueue may have been the only legitimate
    /// match, but the active cid can have changed in the
    /// meantime via the mid-session room-switch path).
    queued_offer: Option<(u32, String)>,
}

impl Default for SessionState {
    fn default() -> Self {
        SessionState::Idle
    }
}

impl SessionMachine {
    /// Fresh machine. Equivalent to `Default::default()` but
    /// reads better at call sites.
    pub fn new() -> Self {
        Self::default()
    }

    /// The state the machine is currently in. Exposed for tests
    /// and for the runtime's diagnostic logging; production code
    /// should consume the `StateChanged` signal instead of
    /// polling.
    pub fn state(&self) -> SessionState {
        self.state
    }

    /// The cid the machine is currently bound to (active voice
    /// room) — `None` when `Idle`.
    pub fn active_cid(&self) -> Option<u32> {
        self.active_cid
    }

    /// Current local mute state.
    pub fn is_muted(&self) -> bool {
        self.muted
    }

    /// Current participant list (last 605 ROOM_STATUS we accepted).
    ///
    /// The runtime layer reads this on
    /// `Action::EmitSignal { kind: SignalKind::RoomStatus, .. }`
    /// to populate the boxed payload it hands to GtkhxSession —
    /// keeping the participants here rather than in
    /// [`SignalPayload::RoomStatus`] lets the runtime skip a clone
    /// per emit and read the typed slice when (and only when) it
    /// needs to.
    ///
    /// Iteration order is unspecified — the underlying map is a
    /// `HashMap<u16, Participant>` keyed on `user_id`.
    pub fn participants(&self) -> impl Iterator<Item = &Participant> + '_ {
        self.participants.values()
    }

    /// Number of participants in the current room. Cheap u16-bounded
    /// accessor for the runtime's signal-emit fast path; avoids
    /// counting via the `participants()` iterator.
    pub fn participant_count(&self) -> usize {
        self.participants.len()
    }

    /// Drive one transition.
    ///
    /// Returns the list of side effects the runtime should
    /// dispatch, in order. An empty list means "event observed,
    /// no side effect" — which is a valid response for several
    /// idempotent / informational events.
    pub fn step(&mut self, event: Event) -> Vec<Action> {
        match (self.state, event) {
            // ---- Join / Leave lifecycle ----
            (SessionState::Idle, Event::JoinRequested { cid }) => {
                self.active_cid = Some(cid);
                self.mid_to_user.clear();
                self.participants.clear();
                self.queued_offer = None;
                self.set_state(SessionState::JoinSent, |actions| {
                    actions.push(Action::SendWireFrame {
                        opcode: HTLC_HDR_VOICE_JOIN,
                        body: WireFrameBody(encode_cid_only(cid)),
                    });
                    actions.push(Action::ArmTimer {
                        kind: TimerKind::JoinReply,
                        ms: JOIN_REPLY_TIMEOUT_MS,
                    });
                })
            }

            // Mid-session JoinRequested — spec §"Room Model": "A
            // user may only be in voice in one room at a time.
            // Joining voice in a second room implicitly leaves the
            // first." The server handles the teardown of the
            // current room on its side; the client tears down its
            // local pipeline and walks back to JoinSent for the
            // new room.
            //
            // Same-cid re-join: no-op. Avoids the awkward case
            // where a stuck UI re-fires JoinRequested for the
            // already-active room and we'd tear down a healthy
            // session for no reason.
            (s, Event::JoinRequested { cid })
                if matches!(
                    s,
                    SessionState::JoinSent
                        | SessionState::OfferPending
                        | SessionState::Connecting
                        | SessionState::Connected
                        | SessionState::Leaving
                ) =>
            {
                // Same-cid re-join: silent no-op unless we're in
                // Leaving (post-fail() collapse), in which case
                // re-joining the same room is exactly the
                // recovery path. fail() clears active_cid to
                // None, so the Some(cid) check below also passes
                // through cleanly for that case; we only short-
                // circuit on a stuck UI re-fire against a still-
                // live session.
                if self.state != SessionState::Leaving
                    && self.active_cid == Some(cid)
                {
                    return Vec::new();
                }
                // Cancel every armed timer kind (the runtime
                // tracks by kind; a Cancel for an unarmed kind is
                // a cheap no-op there). Clear per-session caches
                // since the new room has its own mid/participant
                // sets. Emit TearDown so the runtime can close the
                // old pipeline before the new offer arrives.
                self.active_cid = Some(cid);
                self.mid_to_user.clear();
                self.participants.clear();
                self.queued_offer = None;
                self.muted = false;
                self.set_state(SessionState::JoinSent, |actions| {
                    actions.push(Action::CancelTimer {
                        kind: TimerKind::JoinReply,
                    });
                    actions.push(Action::CancelTimer {
                        kind: TimerKind::Dtls,
                    });
                    actions.push(Action::CancelTimer {
                        kind: TimerKind::IceConnectivity,
                    });
                    actions.push(Action::CancelTimer {
                        kind: TimerKind::Media,
                    });
                    actions.push(Action::TearDown);
                    actions.push(Action::SendWireFrame {
                        opcode: HTLC_HDR_VOICE_JOIN,
                        body: WireFrameBody(encode_cid_only(cid)),
                    });
                    actions.push(Action::ArmTimer {
                        kind: TimerKind::JoinReply,
                        ms: JOIN_REPLY_TIMEOUT_MS,
                    });
                })
            }

            // Connected (or anywhere mid-session): user clicked
            // Leave. Cancel any armed timers, send 601, transition
            // to Leaving and emit TearDown so the runtime closes
            // the pipeline.
            //
            // `Leaving` is a TERMINAL state for this machine — the
            // runtime layer drops the `SessionMachine` after
            // dispatching `TearDown`, and a fresh session
            // constructs a new one in `Idle`. There is no
            // `Leaving → Idle` transition (no `step` arm consumes
            // `WebrtcConnectionStateChanged::Closed` here); the
            // runtime owns the lifecycle past TearDown.
            (s, Event::LeaveRequested { cid })
                if matches!(
                    s,
                    SessionState::JoinSent
                        | SessionState::OfferPending
                        | SessionState::Connecting
                        | SessionState::Connected
                ) =>
            {
                // Sanity: ignore a stray LeaveRequested for a
                // different cid than the one we joined. Don't
                // crash — UI bugs are recoverable.
                if self.active_cid != Some(cid) {
                    return Vec::new();
                }
                // Walk back to Idle directly rather than parking
                // at Leaving. The wire-side LEAVE is on its way out
                // and the runtime tears down webrtcbin on
                // Action::TearDown; by the time the user clicks Join
                // again the machine is fully reset and ready to
                // walk Idle → JoinSent through the normal arm
                // (otherwise the rejoin gets dropped because we
                // never had a Leaving → Idle / Leaving → JoinSent
                // transition wired).
                //
                // Leaving is still reachable via fail() for the
                // collapse path; the (Leaving, JoinRequested) arm
                // below handles recovery there separately.
                self.queued_offer = None;
                self.active_cid = None;
                self.mid_to_user.clear();
                self.participants.clear();
                self.muted = false;
                self.set_state(SessionState::Idle, |actions| {
                    // Cancel every kind the spec arms — the runtime
                    // tracks them by kind regardless of whether one
                    // is actually live, so emitting a Cancel for an
                    // unarmed kind is a cheap no-op on its side. The
                    // alternative (only cancelling JoinReply) lets a
                    // DTLS / ICE / Media watchdog fire after the
                    // pipeline has been torn down, which the runtime
                    // would then dispatch back into a torn-down
                    // SessionMachine.
                    actions.push(Action::CancelTimer { kind: TimerKind::JoinReply });
                    actions.push(Action::CancelTimer { kind: TimerKind::Dtls });
                    actions.push(Action::CancelTimer {
                        kind: TimerKind::IceConnectivity,
                    });
                    actions.push(Action::CancelTimer { kind: TimerKind::Media });
                    actions.push(Action::SendWireFrame {
                        opcode: HTLC_HDR_VOICE_LEAVE,
                        body: WireFrameBody(encode_cid_only(cid)),
                    });
                    actions.push(Action::TearDown);
                })
            }

            // ---- SDP offer / answer flow ----

            // First offer after JOIN: arm renegotiation by tearing
            // up webrtcbin with the offer, then asking it for an
            // answer.
            //
            // Drop stale 602s targeted at a previous room. The
            // mid-session-room-switch path (JoinRequested for a
            // different cid) updates `active_cid` before the new
            // server's 602 can race in; an offer with the old
            // cid would corrupt the mid cache and drive
            // renegotiation against the wrong server. Same guard
            // shape as the ICE / ROOM_STATUS handlers.
            (SessionState::JoinSent, Event::SdpOfferReceived { cid, sdp }) => {
                if self.active_cid != Some(cid) {
                    return Vec::new();
                }
                self.cache_offer_mids(&sdp);
                self.bind_cid_for_offer(cid);
                self.set_state(SessionState::OfferPending, |actions| {
                    actions.push(Action::CancelTimer { kind: TimerKind::JoinReply });
                    actions.push(Action::SetRemoteDescription { sdp });
                    actions.push(Action::CreateAnswer);
                })
            }

            // Renegotiation: a new offer arrived after we're
            // already Connected. Same shape as the initial offer
            // but no JoinReply timer to cancel. The wrong-cid
            // guard (see JoinSent arm above) applies here too —
            // even more important during Connected, where we
            // already have a live media path that a stale 602
            // could blow up.
            (SessionState::Connected, Event::SdpOfferReceived { cid, sdp }) => {
                if self.active_cid != Some(cid) {
                    return Vec::new();
                }
                self.cache_offer_mids(&sdp);
                self.bind_cid_for_offer(cid);
                self.set_state(SessionState::OfferPending, |actions| {
                    actions.push(Action::SetRemoteDescription { sdp });
                    actions.push(Action::CreateAnswer);
                })
            }

            // Spec §"Renegotiation Flow": "if a client receives a
            // new SDP offer while it has not yet answered a
            // previous offer, the client MUST discard the previous
            // unanswered offer and process only the newest one."
            //
            // Phase 8.C step 6 implements this as a serialised
            // queue rather than a replacement: stash the new
            // offer in `queued_offer` and return [] (no actions).
            // The `(OfferPending, WebrtcAnswerCreated)` arm below
            // drains the queue after the current answer has
            // flushed — that way webrtcbin only ever has one
            // pending SetRemoteDescription+CreateAnswer pair at a
            // time, and we don't depend on the runtime's
            // answer-generation guard to discriminate stale
            // promise resolutions (which is now belt-and-braces).
            //
            // The queue replaces, not appends: a third offer
            // arriving before the first answer just supersedes
            // the queued one ("process only the newest").
            //
            // Wrong-cid guard (same shape as the other SDP arms):
            // a late offer from a previously-joined room must NOT
            // enter the queue and surface later as a redirect to
            // a different webrtcbin session.
            (SessionState::OfferPending, Event::SdpOfferReceived { cid, sdp }) => {
                if self.active_cid != Some(cid) {
                    return Vec::new();
                }
                self.queued_offer = Some((cid, sdp));
                Vec::new()
            }

            // webrtcbin handed us the answer; finalise locally and
            // ship 603 to the server.
            //
            // Phase 8.C step 6 also lands the drain side of the
            // renegotiation queue: if a fresh offer arrived while
            // we were waiting, the answer-flush actions get
            // appended with a SetRemoteDescription+CreateAnswer
            // pair for the queued offer, and the machine stays in
            // OfferPending so the queued offer's answer flow can
            // proceed. Only when the queue is empty do we walk to
            // Connecting + arm the DTLS / ICE timers.
            (
                SessionState::OfferPending,
                Event::WebrtcAnswerCreated { sdp },
            ) => {
                let cid = self.active_cid.unwrap_or(0);
                // The wire-format build runs in the runtime layer
                // (we don't link hotline-proto here to keep the
                // crate free of crate-graph deps). The state
                // machine carries the SDP string in the body; the
                // runtime translates to chunks via
                // hotline_proto::voice::build_voice_answer_chunks
                // before calling hlwrite_chunks.
                let mut answer_actions = vec![
                    Action::SetLocalDescription { sdp: sdp.clone() },
                    Action::SendWireFrame {
                        opcode: HTLC_HDR_VOICE_SDP_ANSWER,
                        body: WireFrameBody(encode_cid_plus_sdp(cid, &sdp)),
                    },
                ];
                // Drain the renegotiation queue first. If a new
                // offer landed while this answer was being
                // computed, kick its SetRemote+CreateAnswer pair
                // off now and stay in OfferPending; the next
                // WebrtcAnswerCreated drains again (or transitions
                // to Connecting if no further offers piled up).
                //
                // The wrong-cid guard at enqueue time (above)
                // means queued_cid == active_cid at *enqueue*
                // time. By drain time the active cid may have
                // changed via the mid-session room-switch path
                // (the JoinRequested arm for a different cid
                // clears queued_offer inline alongside the other
                // per-session state — there is no separate
                // reset-for-new-room helper). We re-check
                // defensively here too — a stale queued offer
                // for a now-departed room is dropped, and we
                // fall through to the normal Connecting
                // transition so the current answer still
                // progresses the session.
                if let Some((queued_cid, queued_sdp)) = self.queued_offer.take()
                {
                    if self.active_cid == Some(queued_cid) {
                        self.cache_offer_mids(&queued_sdp);
                        self.bind_cid_for_offer(queued_cid);
                        answer_actions
                            .push(Action::SetRemoteDescription { sdp: queued_sdp });
                        answer_actions.push(Action::CreateAnswer);
                        return answer_actions;
                    }
                    // queued_cid != active_cid: drop the stale
                    // offer (already taken out of self.queued_offer
                    // above) and fall through so the answer for
                    // the current room still drives Connecting.
                }
                // No queued offer (or queued offer was stale) —
                // normal flow to Connecting.
                self.set_state(SessionState::Connecting, |actions| {
                    actions.extend(answer_actions);
                    // Spec timeouts §"Session Timeout and Failure":
                    // ICE checks must complete in 30s, DTLS in
                    // 10s. Arm both watchdogs.
                    actions.push(Action::ArmTimer {
                        kind: TimerKind::Dtls,
                        ms: DTLS_TIMEOUT_MS,
                    });
                    actions.push(Action::ArmTimer {
                        kind: TimerKind::IceConnectivity,
                        ms: ICE_TIMEOUT_MS,
                    });
                })
            }

            // ---- ICE trickle ----

            // Server-side candidate; hand to webrtcbin. Accept in
            // Connecting and Connected (during renegotiation the
            // server may send candidates while we're back in
            // OfferPending — accept those too for robustness).
            //
            // Spec-defensive: drop ICE for a different room than the
            // one we're currently bound to. A late 604 for a room we
            // already left would otherwise leak into the current
            // session's webrtcbin and corrupt the ICE table.
            (
                SessionState::Connecting
                | SessionState::Connected
                | SessionState::OfferPending,
                Event::IceCandidateReceived { cid, candidate_json },
            ) => {
                if self.active_cid != Some(cid) {
                    return Vec::new();
                }
                vec![Action::AddRemoteIce { candidate_json }]
            }

            // EndOfRemoteCandidates is informational only — the
            // WebRTC stack uses it for diagnostics but doesn't
            // strictly require it. Same cid filter as ICE.
            (
                SessionState::Connecting
                | SessionState::Connected
                | SessionState::OfferPending,
                Event::EndOfRemoteCandidates { cid },
            ) => {
                if self.active_cid != Some(cid) {
                    return Vec::new();
                }
                Vec::new()
            }

            // Local candidate; ship over 604.
            (
                SessionState::Connecting
                | SessionState::Connected
                | SessionState::OfferPending,
                Event::WebrtcLocalIceGathered { candidate_json },
            ) => {
                let cid = self.active_cid.unwrap_or(0);
                vec![Action::SendWireFrame {
                    opcode: HTLC_HDR_VOICE_ICE,
                    body: WireFrameBody(encode_cid_plus_ice(cid, &candidate_json)),
                }]
            }

            // ---- Track lifecycle ----

            // Receive pad appeared: map to user_id via the cached
            // mid map. mid `"send"` is the local send leg; we
            // don't open a receive bin for it.
            //
            // Unknown mid (not in the cached mid_to_user map): no-op.
            // Earlier revisions fabricated user_id=0 here, but spec
            // §"Track-to-User Mapping" reserves uid 0 and the runtime
            // would then start a receive pipeline tagged to an
            // invalid speaker — better to surface the missing
            // mid_to_user entry by NOT starting the receive bin at
            // all, which makes the issue (stale SDP cache vs a fresh
            // pad-added) visible at the runtime layer instead of
            // silently routing audio to nobody.
            // Accept pad-added in `OfferPending` too: during a
            // renegotiation drain we stay in `OfferPending` while
            // emitting `SetLocalDescription` / 603 for the
            // in-flight answer, and webrtcbin can produce pads
            // for the just-applied remote description before we
            // reach `Connecting`. The pad gets cached by
            // `connect_pad_added` and the matching mid is in
            // `mid_to_user` (the queue-drain block already
            // primed it), so we can start the receive bin right
            // here — dropping the pad would lose the audio leg
            // for the entire renegotiation cycle.
            (
                SessionState::OfferPending
                | SessionState::Connecting
                | SessionState::Connected,
                Event::WebrtcPadAdded { mid },
            ) => {
                if mid == "send" {
                    return Vec::new();
                }
                match self.mid_to_user.get(&mid).copied() {
                    Some(user_id) => vec![Action::StartReceivePipeline {
                        mid,
                        user_id,
                    }],
                    None => Vec::new(),
                }
            }

            // Receive pad gone: tear the matching bin down. mid
            // `"send"` here would be webrtcbin shutting our send
            // leg, which `LeaveRequested` / Failed already covers
            // — ignore. OfferPending included for symmetry with
            // the pad-added arm above: webrtcbin can drop a pad
            // mid-renegotiation when the new remote description
            // removes a media line.
            (
                SessionState::OfferPending
                | SessionState::Connecting
                | SessionState::Connected,
                Event::WebrtcPadRemoved { mid },
            ) => {
                if mid == "send" {
                    return Vec::new();
                }
                vec![Action::StopReceivePipeline { mid }]
            }

            // ---- Participants list ----

            // 605 ROOM_STATUS: stash + emit signal.
            //
            // Spec-defensive: drop updates for a different room than
            // the active one. A late 605 from a previous room
            // (server queued it before we finished leaving) would
            // otherwise overwrite our current room's participants
            // and push a misleading UI update.
            (
                SessionState::JoinSent
                | SessionState::OfferPending
                | SessionState::Connecting
                | SessionState::Connected,
                Event::ParticipantsUpdated { cid, entries },
            ) => {
                if self.active_cid != Some(cid) {
                    return Vec::new();
                }
                self.participants.clear();
                for p in &entries {
                    self.participants.insert(p.user_id, *p);
                }
                vec![Action::EmitSignal {
                    kind: SignalKind::RoomStatus,
                    payload: SignalPayload::RoomStatus {
                        cid,
                        connection_state: connection_state_for_session(
                            self.state,
                        ),
                    },
                }]
            }

            // ---- Mute toggle ----

            // Client-driven mute / unmute. No-op if already in
            // the requested state.
            (
                SessionState::Connecting | SessionState::Connected,
                Event::MuteToggleRequested { muted },
            ) => {
                if self.muted == muted {
                    return Vec::new();
                }
                self.muted = muted;
                let cid = self.active_cid.unwrap_or(0);
                vec![
                    Action::SetSendPipelineMute { muted },
                    Action::SendWireFrame {
                        opcode: HTLC_HDR_VOICE_MUTE,
                        body: WireFrameBody(encode_cid_plus_muted(cid, muted)),
                    },
                    Action::EmitSignal {
                        kind: SignalKind::MuteChanged,
                        payload: SignalPayload::MuteChanged { muted },
                    },
                ]
            }

            // ---- WebRTC peer connection state transitions ----

            // Connecting → Connected: the happy path. Cancel the
            // DTLS / ICE watchdogs, arm the media-timeout
            // watchdog, emit StateChanged.
            (
                SessionState::Connecting,
                Event::WebrtcConnectionStateChanged {
                    state: ConnectionState::Connected,
                },
            ) => self.set_state(SessionState::Connected, |actions| {
                actions.push(Action::CancelTimer { kind: TimerKind::Dtls });
                actions.push(Action::CancelTimer {
                    kind: TimerKind::IceConnectivity,
                });
                actions.push(Action::ArmTimer {
                    kind: TimerKind::Media,
                    ms: MEDIA_TIMEOUT_MS,
                });
            }),

            // Failure on the active connection — tear down.
            (
                SessionState::Connecting
                | SessionState::Connected
                | SessionState::OfferPending,
                Event::WebrtcConnectionStateChanged {
                    state: ConnectionState::Failed,
                },
            ) => self.fail("WebRTC connection failed".into()),

            // ---- Server task errors ----

            // Server rejected one of our voice opcodes. Surface as
            // a toast and (for JOIN / SDP_ANSWER errors) tear
            // down — without those there's no voice session.
            // MUTE / ICE / LEAVE errors are benign rollbacks; we
            // log via the toast but stay where we are.
            (_, Event::ServerTaskError(err)) => {
                self.handle_server_error(err)
            }

            // ---- Timeouts ----

            // Join reply didn't arrive — server's wedged. Tear
            // down.
            (SessionState::JoinSent, Event::Timeout { kind: Timeout::JoinReply }) => {
                self.fail("Server did not reply to voice join".into())
            }

            // ICE / DTLS / Media watchdogs — same shape.
            (
                SessionState::Connecting | SessionState::Connected,
                Event::Timeout { kind: Timeout::IceConnectivity },
            ) => self.fail("Voice ICE connectivity check failed".into()),
            (
                SessionState::Connecting,
                Event::Timeout { kind: Timeout::Dtls },
            ) => self.fail("Voice DTLS handshake failed".into()),
            (SessionState::Connected, Event::Timeout { kind: Timeout::Media }) => {
                self.fail("Voice media timeout (no RTP from peer)".into())
            }

            // ---- Catch-all ----

            // Unmatched (state, event) pair: documented as a no-op.
            // The categories that legitimately fall here are
            // late-arriving GStreamer signals after we've already
            // transitioned to Leaving / Idle, and events that
            // the spec doesn't require a response for (e.g.
            // EndOfRemoteCandidates while Idle).
            _ => Vec::new(),
        }
    }

    /// Build the action list with the state already moved to
    /// `new_state` and the `StateChanged` signal queued. Wraps
    /// the boilerplate around every multi-action transition.
    fn set_state<F>(&mut self, new_state: SessionState, f: F) -> Vec<Action>
    where
        F: FnOnce(&mut Vec<Action>),
    {
        self.state = new_state;
        let mut actions = Vec::new();
        f(&mut actions);
        actions.push(Action::EmitSignal {
            kind: SignalKind::StateChanged,
            payload: SignalPayload::StateChanged { new_state },
        });
        actions
    }

    fn fail(&mut self, text: String) -> Vec<Action> {
        // Drop transient state, transition to Leaving, emit
        // Error toast + StateChanged, and tell the runtime to
        // tear the pipeline down. The runtime decides when (or
        // whether) to walk back to Idle — typically immediately
        // after TearDown returns.
        self.active_cid = None;
        self.queued_offer = None;
        let prior_state = mem::replace(&mut self.state, SessionState::Leaving);
        let mut actions = vec![Action::CancelTimer {
            kind: TimerKind::JoinReply,
        }];
        if matches!(prior_state, SessionState::Connecting | SessionState::Connected) {
            actions.push(Action::CancelTimer { kind: TimerKind::Dtls });
            actions.push(Action::CancelTimer {
                kind: TimerKind::IceConnectivity,
            });
        }
        if matches!(prior_state, SessionState::Connected) {
            actions.push(Action::CancelTimer { kind: TimerKind::Media });
        }
        actions.push(Action::EmitSignal {
            kind: SignalKind::Error,
            payload: SignalPayload::Error { text },
        });
        actions.push(Action::EmitSignal {
            kind: SignalKind::StateChanged,
            payload: SignalPayload::StateChanged {
                new_state: SessionState::Leaving,
            },
        });
        actions.push(Action::TearDown);
        actions
    }

    fn handle_server_error(&mut self, err: ServerError) -> Vec<Action> {
        match err.origin_opcode {
            // JOIN / SDP_ANSWER errors = no voice session. Tear
            // down with the server-supplied text.
            HTLC_HDR_VOICE_JOIN | HTLC_HDR_VOICE_SDP_ANSWER => {
                self.fail(err.text)
            }
            // MUTE / ICE / LEAVE: surface as toast only, no
            // state change. We could roll mute state back here,
            // but the spec doesn't require it and "mute didn't
            // take" is rare enough that a toast is enough.
            _ => vec![Action::EmitSignal {
                kind: SignalKind::Error,
                payload: SignalPayload::Error { text: err.text },
            }],
        }
    }

    /// Walk the SDP for `a=mid:user-N` labels and populate the
    /// `mid_to_user` cache. Pure-shape parse — anything that
    /// isn't `user-N` (notably `send`) is skipped.
    fn cache_offer_mids(&mut self, sdp: &str) {
        self.mid_to_user.clear();
        for line in sdp.lines() {
            let line = line.trim_end_matches('\r');
            if let Some(rest) = line.strip_prefix("a=mid:") {
                if let Some(uid_str) = rest.strip_prefix("user-") {
                    // Reject leading zeros (spec violation) and
                    // out-of-range values.
                    if uid_str.starts_with('0') {
                        continue;
                    }
                    if let Ok(uid) = uid_str.parse::<u32>() {
                        if (1..=u16::MAX as u32).contains(&uid) {
                            self.mid_to_user
                                .insert(rest.to_string(), uid as u16);
                        }
                    }
                }
            }
        }
    }

    /// Update `active_cid` when an offer arrives. Defensive: the
    /// server should always echo the cid we joined with, but
    /// honour an explicit override just in case.
    fn bind_cid_for_offer(&mut self, cid: u32) {
        self.active_cid = Some(cid);
    }
}

// ---- Wire-format helpers ---------------------------------------------
//
// The state machine produces wire-frame bodies as opaque Vec<u8>
// rather than typed chunk arrays so it doesn't have to depend on
// hotline-proto's HxChunk surface. The runtime side translates
// these into the actual chunks by calling
// hotline_proto::voice::build_voice_*_chunks just before handing
// them to hlwrite_chunks. The encoding here is the simplest
// possible: a tagged payload the runtime side knows how to read.
//
// Phase 8.C lands this stub; Phase 8.D and beyond may switch to
// passing typed chunk descriptors if that turns out to be
// cleaner. For now this keeps the test-side action assertions
// readable (the test pattern-matches on a small Vec<u8> rather
// than a 4-tuple of borrowed slices).

fn encode_cid_only(cid: u32) -> Vec<u8> {
    cid.to_be_bytes().to_vec()
}

fn encode_cid_plus_sdp(cid: u32, sdp: &str) -> Vec<u8> {
    let mut v = Vec::with_capacity(4 + sdp.len());
    v.extend_from_slice(&cid.to_be_bytes());
    v.extend_from_slice(sdp.as_bytes());
    v
}

fn encode_cid_plus_ice(cid: u32, candidate_json: &str) -> Vec<u8> {
    let mut v = Vec::with_capacity(4 + candidate_json.len());
    v.extend_from_slice(&cid.to_be_bytes());
    v.extend_from_slice(candidate_json.as_bytes());
    v
}

fn encode_cid_plus_muted(cid: u32, muted: bool) -> Vec<u8> {
    let mut v = Vec::with_capacity(6);
    v.extend_from_slice(&cid.to_be_bytes());
    v.extend_from_slice(&(muted as u16).to_be_bytes());
    v
}

/// Map the session state to the matching ConnectionState that the
/// UI's RoomStatus signal cares about. Mostly just a one-way enum
/// translation; the runtime side could compute this itself but
/// having it here keeps tests' assertions tight.
fn connection_state_for_session(s: SessionState) -> ConnectionState {
    match s {
        SessionState::Idle => ConnectionState::Closed,
        SessionState::JoinSent
        | SessionState::OfferPending
        | SessionState::Connecting => ConnectionState::Connecting,
        SessionState::Connected => ConnectionState::Connected,
        // Leaving is the terminal state of this machine — the
        // runtime drops the SessionMachine after reaching it.
        // That's a session that has *ended*, not one with
        // temporary connectivity loss, so map to Closed.
        // Disconnected per `ConnectionState`'s own doc is
        // "connectivity loss the stack thinks is temporary"; a
        // RoomStatus signal carrying Disconnected here would
        // mislead the UI into showing "reconnecting" for a
        // session that's actually done.
        SessionState::Leaving => ConnectionState::Closed,
    }
}

// ---- Wire-protocol opcodes ----
//
// Re-declared here rather than imported from `hotline-proto::messages`
// so this crate stays dep-free. The numeric values are pinned by
// the spec-matched test fixture at the bottom of this file's
// tests module — see `crate::state::tests::wire_opcode_constants_match_spec`
// (and the corresponding fixture in hotline-proto, which pins the
// same opcode numbers on its own).
const HTLC_HDR_VOICE_JOIN: u32 = 600;
const HTLC_HDR_VOICE_LEAVE: u32 = 601;
const HTLC_HDR_VOICE_SDP_ANSWER: u32 = 603;
const HTLC_HDR_VOICE_ICE: u32 = 604;
const HTLC_HDR_VOICE_MUTE: u32 = 606;

// ---- Timeout values ----
//
// Per the spec's §"Session Timeout and Failure" table. The state
// machine emits these as `Action::ArmTimer { ms }` — the runtime
// translates ms to a `glib::timeout_add` interval.
const JOIN_REPLY_TIMEOUT_MS: u32 = 10_000;
const DTLS_TIMEOUT_MS: u32 = 10_000;
const ICE_TIMEOUT_MS: u32 = 30_000;
const MEDIA_TIMEOUT_MS: u32 = 30_000;

#[cfg(test)]
mod tests {
    use super::*;

    fn machine() -> SessionMachine {
        SessionMachine::new()
    }

    fn drain_state_changed(actions: &[Action]) -> Option<SessionState> {
        for a in actions {
            if let Action::EmitSignal {
                payload: SignalPayload::StateChanged { new_state },
                ..
            } = a
            {
                return Some(*new_state);
            }
        }
        None
    }

    // ---- Sanity: starting state ----
    #[test]
    fn starts_idle() {
        let m = machine();
        assert_eq!(m.state(), SessionState::Idle);
        assert!(m.active_cid().is_none());
        assert!(!m.is_muted());
    }

    // ---- Join + leave lifecycle ----
    #[test]
    fn join_request_transitions_idle_to_join_sent() {
        let mut m = machine();
        let acts = m.step(Event::JoinRequested { cid: 42 });
        assert_eq!(m.state(), SessionState::JoinSent);
        assert_eq!(m.active_cid(), Some(42));
        // Expect SendWireFrame(JOIN), ArmTimer, StateChanged.
        assert_eq!(acts.len(), 3);
        match &acts[0] {
            Action::SendWireFrame { opcode, body } => {
                assert_eq!(*opcode, 600);
                assert_eq!(body.0, 42u32.to_be_bytes());
            }
            a => panic!("expected SendWireFrame, got {a:?}"),
        }
        assert!(matches!(
            acts[1],
            Action::ArmTimer { kind: Timeout::JoinReply, .. }
        ));
        assert_eq!(drain_state_changed(&acts), Some(SessionState::JoinSent));
    }

    #[test]
    fn leave_from_connected_emits_tear_down() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 7 });
        m.step(Event::SdpOfferReceived {
            cid: 7,
            sdp: "v=0\na=mid:send\n".into(),
        });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        m.step(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        assert_eq!(m.state(), SessionState::Connected);

        let acts = m.step(Event::LeaveRequested { cid: 7 });
        // User-driven Leave walks directly back to Idle so a
        // subsequent JoinRequested can use the canonical
        // Idle → JoinSent arm. Leaving is reserved for the
        // fail() collapse path.
        assert_eq!(m.state(), SessionState::Idle);
        assert_eq!(m.active_cid(), None);
        // Should cancel JoinReply timer, send 601, tear down, emit
        // StateChanged.
        let kinds: Vec<&'static str> = acts
            .iter()
            .map(|a| match a {
                Action::CancelTimer { .. } => "cancel",
                Action::SendWireFrame { opcode: 601, .. } => "send601",
                Action::TearDown => "tear",
                Action::EmitSignal { .. } => "signal",
                _ => "other",
            })
            .collect();
        assert!(kinds.contains(&"send601"));
        assert!(kinds.contains(&"tear"));
        assert!(kinds.contains(&"cancel"));
        assert!(kinds.contains(&"signal"));
    }

    /// Regression: after a user-driven LeaveRequested, the
    /// machine must be re-joinable. Earlier behaviour parked at
    /// Leaving with active_cid still Some(prev_cid), and a
    /// subsequent JoinRequested fell to the catch-all (no arm
    /// matched Leaving) — UI button stayed stuck on "Join Voice"
    /// even though the wire VOICE_JOIN went out.
    #[test]
    fn rejoin_after_leave_walks_idle_to_join_sent() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 0 });
        m.step(Event::SdpOfferReceived {
            cid: 0,
            sdp: "v=0\na=mid:send\n".into(),
        });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        m.step(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        assert_eq!(m.state(), SessionState::Connected);

        m.step(Event::LeaveRequested { cid: 0 });
        // The fix: Leave walks to Idle, not Leaving.
        assert_eq!(m.state(), SessionState::Idle);
        assert_eq!(m.active_cid(), None);

        // Rejoin should now flow through the canonical Idle arm.
        let acts = m.step(Event::JoinRequested { cid: 0 });
        assert_eq!(m.state(), SessionState::JoinSent);
        assert_eq!(m.active_cid(), Some(0));
        assert!(acts.iter().any(|a| matches!(
            a,
            Action::SendWireFrame { opcode: 600, .. }
        )));
        assert!(acts.iter().any(|a| matches!(
            a,
            Action::EmitSignal {
                kind: SignalKind::StateChanged,
                ..
            }
        )));
    }

    /// Regression: after fail() collapse the machine sits in
    /// Leaving with active_cid cleared. A JoinRequested must
    /// recover — otherwise a single server-side error (e.g. a
    /// 603 SDP_ANSWER rejected) wedges the UI permanently.
    #[test]
    fn rejoin_after_fail_recovers_from_leaving() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 5 });
        // ServerTaskError on the JOIN opcode drives fail() →
        // Leaving + active_cid=None.
        m.step(Event::ServerTaskError(ServerError {
            origin_opcode: HTLC_HDR_VOICE_JOIN,
            text: "no room".into(),
        }));
        assert_eq!(m.state(), SessionState::Leaving);
        assert_eq!(m.active_cid(), None);

        // User retries — should walk back through the room-switch
        // arm (now Leaving-aware) to JoinSent.
        let acts = m.step(Event::JoinRequested { cid: 5 });
        assert_eq!(m.state(), SessionState::JoinSent);
        assert_eq!(m.active_cid(), Some(5));
        assert!(acts.iter().any(|a| matches!(
            a,
            Action::SendWireFrame { opcode: 600, .. }
        )));
    }

    /// Regression (Copilot review): Event::JoinRequested docs
    /// claim the state machine handles the spec's implicit-leave
    /// (joining voice in room B while in voice in A). Earlier
    /// drafts only handled `JoinRequested` in `Idle`; mid-session
    /// re-joins fell through the catch-all and became silent
    /// no-ops. Now the transition cancels every armed timer,
    /// emits TearDown, sends 600 for the new cid, and walks back
    /// to JoinSent with fresh state.
    #[test]
    fn join_for_different_room_mid_session_implicitly_leaves() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived {
            cid: 1,
            sdp: "a=mid:user-5\na=mid:send\n".into(),
        });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        m.step(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        assert_eq!(m.state(), SessionState::Connected);
        // Cached mids from room 1.
        assert_eq!(m.mid_to_user.get("user-5").copied(), Some(5));

        let acts = m.step(Event::JoinRequested { cid: 99 });
        // Walked back to JoinSent for the new cid.
        assert_eq!(m.state(), SessionState::JoinSent);
        assert_eq!(m.active_cid(), Some(99));
        // Room 1's mids and participants were cleared.
        assert!(m.mid_to_user.is_empty());
        assert_eq!(m.participant_count(), 0);
        // TearDown + 600 for new cid + JoinReply timer armed.
        assert!(acts.iter().any(|a| matches!(a, Action::TearDown)));
        assert!(acts.iter().any(|a| matches!(
            a,
            Action::SendWireFrame { opcode: 600, .. }
        )));
        assert!(acts.iter().any(|a| matches!(
            a,
            Action::ArmTimer { kind: Timeout::JoinReply, .. }
        )));
    }

    /// Same-cid re-join (a stuck UI re-fires JoinRequested for
    /// the already-active room): no-op. Avoids tearing down a
    /// healthy session for no reason.
    #[test]
    fn join_for_active_room_is_noop() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 7 });
        m.step(Event::SdpOfferReceived {
            cid: 7,
            sdp: "v=0\n".into(),
        });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        m.step(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        let acts = m.step(Event::JoinRequested { cid: 7 });
        assert_eq!(m.state(), SessionState::Connected);
        assert!(acts.is_empty());
    }

    #[test]
    fn leave_for_wrong_cid_is_ignored() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 7 });
        let acts = m.step(Event::LeaveRequested { cid: 99 });
        // No transition, no actions.
        assert_eq!(m.state(), SessionState::JoinSent);
        assert!(acts.is_empty());
    }

    /// Regression (Copilot review): LeaveRequested used to cancel
    /// only the JoinReply timer. If the session was already in
    /// Connecting / Connected, the DTLS / ICE / Media watchdogs
    /// stayed armed and could fire after teardown — the runtime
    /// would then dispatch a `Timeout` event into a half-freed
    /// SessionMachine. The transition now emits CancelTimer for
    /// every kind the spec arms.
    #[test]
    fn leave_cancels_every_armed_timer() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        m.step(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        assert_eq!(m.state(), SessionState::Connected);

        let acts = m.step(Event::LeaveRequested { cid: 1 });
        // Collect every cancel kind the leave path emitted.
        let cancels: Vec<Timeout> = acts
            .iter()
            .filter_map(|a| match a {
                Action::CancelTimer { kind } => Some(*kind),
                _ => None,
            })
            .collect();
        assert!(cancels.contains(&Timeout::JoinReply));
        assert!(cancels.contains(&Timeout::Dtls));
        assert!(cancels.contains(&Timeout::IceConnectivity));
        assert!(cancels.contains(&Timeout::Media));
    }

    // ---- SDP offer / answer ----
    #[test]
    fn sdp_offer_after_join_caches_mids_and_walks_to_offer_pending() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        let sdp = "v=0\r\n\
                   a=group:BUNDLE user-12 send\r\n\
                   m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n\
                   a=mid:user-12\r\n\
                   m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n\
                   a=mid:send\r\n";
        let acts = m.step(Event::SdpOfferReceived {
            cid: 1,
            sdp: sdp.into(),
        });
        assert_eq!(m.state(), SessionState::OfferPending);
        assert_eq!(m.mid_to_user.get("user-12").copied(), Some(12));
        // Should: cancel JoinReply, SetRemoteDescription, CreateAnswer,
        // StateChanged.
        assert!(acts
            .iter()
            .any(|a| matches!(a, Action::CancelTimer { kind: Timeout::JoinReply })));
        assert!(acts.iter().any(|a| matches!(a, Action::SetRemoteDescription { .. })));
        assert!(acts.iter().any(|a| matches!(a, Action::CreateAnswer)));
    }

    #[test]
    fn renegotiation_offer_while_pending_is_queued_not_replaced() {
        // Phase 8.C step 6: a second offer arriving while we're
        // already in OfferPending must be queued (no actions
        // emitted now), then drained after the current answer
        // flushes. The previous behavior (replace immediately
        // with a fresh SetRemoteDescription+CreateAnswer pair)
        // would have webrtcbin holding two pending
        // set-remote-descriptions, which corrupts its internal
        // state machine.
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 5 });
        m.step(Event::SdpOfferReceived { cid: 5, sdp: "v=0\n".into() });
        assert_eq!(m.state(), SessionState::OfferPending);

        // Second offer arrives mid-pending: enqueue, no actions.
        let acts = m.step(Event::SdpOfferReceived {
            cid: 5,
            sdp: "v=0\na=mid:user-99\n".into(),
        });
        assert!(
            acts.is_empty(),
            "second offer must enqueue silently while OfferPending; got {acts:?}"
        );
        // Mids from the QUEUED offer must not surface yet —
        // they get cached at drain time.
        assert_eq!(m.mid_to_user.get("user-99").copied(), None);
        // State stays OfferPending.
        assert_eq!(m.state(), SessionState::OfferPending);

        // Now the first answer flows in; the queue drains so
        // the queued offer's SetRemoteDescription+CreateAnswer
        // pair lands AFTER the SetLocalDescription/Send603
        // pair for the current answer. Machine stays in
        // OfferPending (queued offer's answer still in flight).
        let acts = m.step(Event::WebrtcAnswerCreated {
            sdp: "v=0\n".into(),
        });
        assert_eq!(
            m.state(),
            SessionState::OfferPending,
            "queue drain must keep us in OfferPending, not advance to Connecting"
        );
        let kinds: Vec<&str> = acts
            .iter()
            .map(|a| match a {
                Action::SetLocalDescription { .. } => "set_local",
                Action::SendWireFrame { opcode: 603, .. } => "send_603",
                Action::SetRemoteDescription { .. } => "set_remote",
                Action::CreateAnswer => "create_answer",
                _ => "other",
            })
            .collect();
        assert!(kinds.contains(&"set_local"));
        assert!(kinds.contains(&"send_603"));
        assert!(kinds.contains(&"set_remote"));
        assert!(kinds.contains(&"create_answer"));
        // Ordering: the answer flush (SetLocalDescription + 603
        // send) MUST land before the queued offer's
        // SetRemoteDescription + CreateAnswer pair. That's the
        // whole point of the queue: webrtcbin only ever has one
        // pending SDP exchange. Swapping the order would feed
        // the new offer to webrtcbin before the current answer
        // has been applied, which is the wedge the queue exists
        // to prevent.
        let set_local_pos = kinds.iter().position(|k| *k == "set_local");
        let send_603_pos = kinds.iter().position(|k| *k == "send_603");
        let set_remote_pos = kinds.iter().position(|k| *k == "set_remote");
        let create_answer_pos =
            kinds.iter().position(|k| *k == "create_answer");
        assert!(
            set_local_pos < set_remote_pos,
            "SetLocalDescription must precede the queued \
             SetRemoteDescription; got order={kinds:?}"
        );
        assert!(
            send_603_pos < set_remote_pos,
            "SendWireFrame(603) must precede the queued \
             SetRemoteDescription; got order={kinds:?}"
        );
        assert!(
            set_remote_pos < create_answer_pos,
            "SetRemoteDescription must precede CreateAnswer in \
             the queued-offer kick-off; got order={kinds:?}"
        );
        // Now the queued offer's mids are cached.
        assert_eq!(m.mid_to_user.get("user-99").copied(), Some(99));
    }

    #[test]
    fn answer_created_emits_set_local_send_603_and_arms_watchdogs() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 3 });
        m.step(Event::SdpOfferReceived { cid: 3, sdp: "v=0\n".into() });
        let acts = m.step(Event::WebrtcAnswerCreated {
            sdp: "v=0\no=- 1 1 IN IP4 0.0.0.0\n".into(),
        });
        assert_eq!(m.state(), SessionState::Connecting);
        let mut saw_set_local = false;
        let mut saw_send_603 = false;
        let mut saw_dtls = false;
        let mut saw_ice = false;
        for a in &acts {
            match a {
                Action::SetLocalDescription { .. } => saw_set_local = true,
                Action::SendWireFrame { opcode: 603, .. } => saw_send_603 = true,
                Action::ArmTimer { kind: Timeout::Dtls, .. } => saw_dtls = true,
                Action::ArmTimer { kind: Timeout::IceConnectivity, .. } => {
                    saw_ice = true
                }
                _ => {}
            }
        }
        assert!(saw_set_local);
        assert!(saw_send_603);
        assert!(saw_dtls);
        assert!(saw_ice);
    }

    // ---- ICE trickle ----
    #[test]
    fn local_ice_gathered_sends_604() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        let acts = m.step(Event::WebrtcLocalIceGathered {
            candidate_json: "{\"candidate\":\"c\",\"sdpMid\":\"send\"}".into(),
        });
        assert!(acts.iter().any(|a| matches!(
            a,
            Action::SendWireFrame { opcode: 604, .. }
        )));
    }

    #[test]
    fn remote_ice_received_adds_to_webrtcbin() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        let acts = m.step(Event::IceCandidateReceived {
            cid: 1,
            candidate_json: "{\"candidate\":\"c\",\"sdpMid\":\"send\"}".into(),
        });
        assert!(acts.iter().any(|a| matches!(a, Action::AddRemoteIce { .. })));
    }

    #[test]
    fn end_of_remote_candidates_is_informational_only() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        let acts = m.step(Event::EndOfRemoteCandidates { cid: 1 });
        assert!(acts.is_empty());
    }

    /// Regression (Copilot review): ICE events for a non-active
    /// room used to be passed through to AddRemoteIce, which would
    /// corrupt the active session's webrtcbin ICE table. Now the
    /// state machine drops them.
    #[test]
    fn remote_ice_for_wrong_cid_is_dropped() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        // ICE for a different room: dropped, no action.
        let acts = m.step(Event::IceCandidateReceived {
            cid: 99,
            candidate_json: "{\"candidate\":\"c\",\"sdpMid\":\"send\"}".into(),
        });
        assert!(acts.is_empty());
        // EndOfRemoteCandidates for the wrong room is also dropped
        // (the spec uses an empty payload as EOC; we filter the
        // same way as the regular candidate).
        let acts = m.step(Event::EndOfRemoteCandidates { cid: 99 });
        assert!(acts.is_empty());
    }

    // ---- Track lifecycle ----
    #[test]
    fn pad_added_for_user_mid_starts_receive_pipeline_with_uid() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived {
            cid: 1,
            sdp: "a=mid:user-23\na=mid:send\n".into(),
        });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        let acts = m.step(Event::WebrtcPadAdded {
            mid: "user-23".into(),
        });
        match &acts[..] {
            [Action::StartReceivePipeline { mid, user_id }] => {
                assert_eq!(mid, "user-23");
                assert_eq!(*user_id, 23);
            }
            _ => panic!("expected single StartReceivePipeline, got {acts:?}"),
        }
    }

    #[test]
    fn pad_added_for_send_mid_does_nothing() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived {
            cid: 1,
            sdp: "a=mid:send\n".into(),
        });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        let acts = m.step(Event::WebrtcPadAdded { mid: "send".into() });
        assert!(acts.is_empty());
    }

    /// Regression (Copilot review): WebrtcPadAdded for a mid not in
    /// the cached mid_to_user map used to fabricate `user_id = 0`
    /// and start a receive pipeline anyway. uid 0 is the spec's
    /// reserved sentinel — the runtime would then route audio to
    /// a non-existent speaker. Now the unknown-mid path no-ops so
    /// the missing cache entry surfaces as a silent leg rather
    /// than corrupted UI.
    #[test]
    fn pad_added_for_unknown_user_mid_is_a_noop() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        // SDP carries user-5 only; user-99 isn't in the cache.
        m.step(Event::SdpOfferReceived {
            cid: 1,
            sdp: "a=mid:user-5\na=mid:send\n".into(),
        });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        // Pad arrives for an uncached mid.
        let acts = m.step(Event::WebrtcPadAdded {
            mid: "user-99".into(),
        });
        // No StartReceivePipeline — pad is silently dropped.
        assert!(acts.is_empty());
    }

    /// Regression: during a renegotiation drain the machine
    /// stays in `OfferPending` while the in-flight answer's
    /// `SetLocalDescription` + 603 ship. If webrtcbin produces
    /// pad-added for the just-applied remote description
    /// before we reach `Connecting`, the pad must not be
    /// dropped — that would lose the audio leg for the entire
    /// renegotiation cycle.
    #[test]
    fn pad_added_in_offer_pending_starts_receive_pipeline() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        // First offer primes mid_to_user with user-5.
        m.step(Event::SdpOfferReceived {
            cid: 1,
            sdp: "a=mid:user-5\na=mid:send\n".into(),
        });
        assert_eq!(m.state(), SessionState::OfferPending);
        // Pad-added arrives while we're still computing the
        // answer (state stays OfferPending).
        let acts = m.step(Event::WebrtcPadAdded {
            mid: "user-5".into(),
        });
        match acts.as_slice() {
            [Action::StartReceivePipeline { mid, user_id }] => {
                assert_eq!(mid, "user-5");
                assert_eq!(*user_id, 5);
            }
            _ => panic!(
                "expected StartReceivePipeline in OfferPending, got {acts:?}"
            ),
        }
        // Sanity: state is still OfferPending (pad-added didn't
        // transition us).
        assert_eq!(m.state(), SessionState::OfferPending);
    }

    #[test]
    fn pad_removed_for_user_mid_stops_pipeline() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived {
            cid: 1,
            sdp: "a=mid:user-9\n".into(),
        });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        let acts = m.step(Event::WebrtcPadRemoved {
            mid: "user-9".into(),
        });
        assert!(matches!(acts[..], [Action::StopReceivePipeline { .. }]));
    }

    // ---- Mute toggle ----
    #[test]
    fn mute_request_emits_pipeline_wire_and_signal() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 4 });
        m.step(Event::SdpOfferReceived { cid: 4, sdp: "v=0\n".into() });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        let acts = m.step(Event::MuteToggleRequested { muted: true });
        assert!(m.is_muted());
        let kinds: Vec<&'static str> = acts
            .iter()
            .map(|a| match a {
                Action::SetSendPipelineMute { .. } => "pipeline",
                Action::SendWireFrame { opcode: 606, .. } => "wire606",
                Action::EmitSignal { kind: SignalKind::MuteChanged, .. } => "signal",
                _ => "other",
            })
            .collect();
        assert!(kinds.contains(&"pipeline"));
        assert!(kinds.contains(&"wire606"));
        assert!(kinds.contains(&"signal"));
    }

    #[test]
    fn redundant_mute_is_noop() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 4 });
        m.step(Event::SdpOfferReceived { cid: 4, sdp: "v=0\n".into() });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        m.step(Event::MuteToggleRequested { muted: true });
        let acts = m.step(Event::MuteToggleRequested { muted: true });
        assert!(acts.is_empty());
        assert!(m.is_muted());
    }

    // ---- Participants ----
    #[test]
    fn participants_update_emits_room_status_signal() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        let acts = m.step(Event::ParticipantsUpdated {
            cid: 1,
            entries: vec![
                Participant { user_id: 5, codec_id: 0, muted: false },
                Participant { user_id: 12, codec_id: 0, muted: true },
            ],
        });
        let saw_signal = acts.iter().any(|a| {
            matches!(
                a,
                Action::EmitSignal {
                    kind: SignalKind::RoomStatus,
                    ..
                }
            )
        });
        assert!(saw_signal);
        assert_eq!(m.participants.len(), 2);
        assert!(m.participants[&12].muted);
    }

    /// Regression (Copilot review): ParticipantsUpdated for a
    /// non-active cid used to overwrite the local participants
    /// map and emit a RoomStatus signal for the wrong room. Now
    /// the cid mismatch path is a no-op.
    #[test]
    fn participants_update_for_wrong_cid_is_dropped() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::ParticipantsUpdated {
            cid: 1,
            entries: vec![Participant {
                user_id: 7,
                codec_id: 0,
                muted: false,
            }],
        });
        // Sanity: the legit update landed.
        assert_eq!(m.participant_count(), 1);

        // Stale update for a different room arrives — must NOT
        // overwrite the current room's state, and must not emit
        // a signal.
        let acts = m.step(Event::ParticipantsUpdated {
            cid: 99,
            entries: vec![
                Participant { user_id: 1, codec_id: 0, muted: false },
                Participant { user_id: 2, codec_id: 0, muted: true },
            ],
        });
        assert!(acts.is_empty());
        // Original participant still there, stale ones not added.
        assert_eq!(m.participant_count(), 1);
        let still_there: Vec<u16> = m.participants().map(|p| p.user_id).collect();
        assert_eq!(still_there, vec![7]);
    }

    /// Regression (Copilot review): the runtime layer couldn't
    /// access the cached participants without reaching into
    /// private state. The public participants() / participant_count()
    /// accessors expose what it needs.
    #[test]
    fn participants_accessor_returns_cached_list() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::ParticipantsUpdated {
            cid: 1,
            entries: vec![
                Participant { user_id: 3, codec_id: 0, muted: false },
                Participant { user_id: 8, codec_id: 0, muted: true },
            ],
        });
        assert_eq!(m.participant_count(), 2);
        let mut uids: Vec<u16> = m.participants().map(|p| p.user_id).collect();
        uids.sort_unstable();
        assert_eq!(uids, vec![3, 8]);
        let p8 = m.participants().find(|p| p.user_id == 8).unwrap();
        assert!(p8.muted);
    }

    // ---- Connection state walk ----
    #[test]
    fn connecting_to_connected_cancels_watchdogs_and_arms_media_timeout() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        let acts = m.step(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        assert_eq!(m.state(), SessionState::Connected);
        let mut cancel_dtls = false;
        let mut cancel_ice = false;
        let mut arm_media = false;
        for a in &acts {
            match a {
                Action::CancelTimer { kind: Timeout::Dtls } => cancel_dtls = true,
                Action::CancelTimer { kind: Timeout::IceConnectivity } => cancel_ice = true,
                Action::ArmTimer { kind: Timeout::Media, .. } => arm_media = true,
                _ => {}
            }
        }
        assert!(cancel_dtls);
        assert!(cancel_ice);
        assert!(arm_media);
    }

    #[test]
    fn connection_state_failed_tears_down() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        let acts = m.step(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Failed,
        });
        assert_eq!(m.state(), SessionState::Leaving);
        assert!(acts.iter().any(|a| matches!(a, Action::TearDown)));
        assert!(acts.iter().any(|a| matches!(
            a,
            Action::EmitSignal { kind: SignalKind::Error, .. }
        )));
    }

    // ---- Server task errors ----
    #[test]
    fn task_error_on_join_tears_down() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        let acts = m.step(Event::ServerTaskError(ServerError {
            origin_opcode: HTLC_HDR_VOICE_JOIN,
            text: "Room is full".into(),
        }));
        assert_eq!(m.state(), SessionState::Leaving);
        assert!(acts.iter().any(|a| matches!(a, Action::TearDown)));
    }

    #[test]
    fn task_error_on_mute_is_toast_only() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        let prior_state = m.state();
        let acts = m.step(Event::ServerTaskError(ServerError {
            origin_opcode: HTLC_HDR_VOICE_MUTE,
            text: "No mic".into(),
        }));
        assert_eq!(m.state(), prior_state);
        assert!(acts.iter().any(|a| matches!(
            a,
            Action::EmitSignal { kind: SignalKind::Error, .. }
        )));
        assert!(!acts.iter().any(|a| matches!(a, Action::TearDown)));
    }

    // ---- Timeouts ----
    #[test]
    fn join_reply_timeout_tears_down() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        let acts = m.step(Event::Timeout {
            kind: Timeout::JoinReply,
        });
        assert_eq!(m.state(), SessionState::Leaving);
        assert!(acts.iter().any(|a| matches!(a, Action::TearDown)));
    }

    #[test]
    fn media_timeout_in_connected_tears_down() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        m.step(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        let acts = m.step(Event::Timeout { kind: Timeout::Media });
        assert_eq!(m.state(), SessionState::Leaving);
        assert!(acts.iter().any(|a| matches!(a, Action::TearDown)));
    }

    // ---- Spec replay: User B joins room with A already in voice ----
    //
    // Mirrors the annotated sequence at
    // Capabilities-Voice.md §"Renegotiation Flow", from B's
    // (joining-client) perspective. Each `step()` line corresponds
    // to one wire arrival or one webrtcbin signal in the diagram.
    #[test]
    fn spec_replay_b_joins_room_with_a_in_voice() {
        let mut m = machine();

        // B clicks Join Voice in chat room 42 where A is already
        // in voice.
        let acts = m.step(Event::JoinRequested { cid: 42 });
        assert_eq!(m.state(), SessionState::JoinSent);
        assert!(acts.iter().any(|a| matches!(
            a,
            Action::SendWireFrame { opcode: 600, .. }
        )));

        // Server replies with SDP offer (the JOIN reply carries
        // it). Offer contains B's own send leg + receive leg for A.
        let offer = "v=0\r\n\
                     a=group:BUNDLE user-1 send\r\n\
                     m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n\
                     a=mid:user-1\r\n\
                     a=recvonly\r\n\
                     a=rtpmap:0 PCMU/8000\r\n\
                     m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n\
                     a=mid:send\r\n\
                     a=sendonly\r\n\
                     a=rtpmap:0 PCMU/8000\r\n";
        let acts = m.step(Event::SdpOfferReceived {
            cid: 42,
            sdp: offer.into(),
        });
        assert_eq!(m.state(), SessionState::OfferPending);
        assert!(acts.iter().any(|a| matches!(a, Action::SetRemoteDescription { .. })));
        assert!(acts.iter().any(|a| matches!(a, Action::CreateAnswer)));
        assert_eq!(m.mid_to_user.get("user-1").copied(), Some(1));

        // webrtcbin returns the answer SDP.
        let acts = m.step(Event::WebrtcAnswerCreated {
            sdp: "v=0\r\na=mid:user-1\r\na=mid:send\r\n".into(),
        });
        assert_eq!(m.state(), SessionState::Connecting);
        assert!(acts.iter().any(|a| matches!(
            a,
            Action::SendWireFrame { opcode: 603, .. }
        )));

        // ICE trickle exchange.
        m.step(Event::WebrtcLocalIceGathered {
            candidate_json: "{\"candidate\":\"c\",\"sdpMid\":\"send\"}".into(),
        });
        m.step(Event::IceCandidateReceived {
            cid: 42,
            candidate_json: "{\"candidate\":\"c\",\"sdpMid\":\"send\"}".into(),
        });
        m.step(Event::EndOfRemoteCandidates { cid: 42 });

        // webrtcbin reaches Connected; arms media watchdog.
        let acts = m.step(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        assert_eq!(m.state(), SessionState::Connected);
        assert!(acts.iter().any(|a| matches!(
            a,
            Action::ArmTimer { kind: Timeout::Media, .. }
        )));

        // A's receive pad appears.
        let acts = m.step(Event::WebrtcPadAdded { mid: "user-1".into() });
        match &acts[..] {
            [Action::StartReceivePipeline { mid, user_id }] => {
                assert_eq!(mid, "user-1");
                assert_eq!(*user_id, 1);
            }
            _ => panic!("expected single StartReceivePipeline"),
        }

        // Server announces the new participant list (A and B).
        let acts = m.step(Event::ParticipantsUpdated {
            cid: 42,
            entries: vec![
                Participant { user_id: 1, codec_id: 0, muted: false },
                Participant { user_id: 2, codec_id: 0, muted: false },
            ],
        });
        assert!(acts.iter().any(|a| matches!(
            a,
            Action::EmitSignal { kind: SignalKind::RoomStatus, .. }
        )));
    }

    // ---- Spec replay: User B leaves ----
    #[test]
    fn spec_replay_b_leaves_voice() {
        let mut m = machine();
        // Drive into Connected first (replicate the join above
        // but shorter — we already pinned the join sequence in
        // the previous test).
        m.step(Event::JoinRequested { cid: 42 });
        m.step(Event::SdpOfferReceived {
            cid: 42,
            sdp: "a=mid:user-1\na=mid:send\n".into(),
        });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        m.step(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        assert_eq!(m.state(), SessionState::Connected);

        // B clicks Leave Voice. Walks straight back to Idle
        // (see leave_from_connected_emits_tear_down for why).
        let acts = m.step(Event::LeaveRequested { cid: 42 });
        assert_eq!(m.state(), SessionState::Idle);
        assert!(acts.iter().any(|a| matches!(
            a,
            Action::SendWireFrame { opcode: 601, .. }
        )));
        assert!(acts.iter().any(|a| matches!(a, Action::TearDown)));
    }

    // ---- Sanity: opcode constants match hotline-proto ----
    //
    // The state machine deliberately doesn't depend on the
    // hotline-proto crate (keeps it no_std-friendly), so these
    // constants are re-declared from the spec. Pin the numeric
    // values here so a future tweak in either crate surfaces
    // the drift at test time.
    #[test]
    fn wire_opcode_constants_match_spec() {
        assert_eq!(HTLC_HDR_VOICE_JOIN, 600);
        assert_eq!(HTLC_HDR_VOICE_LEAVE, 601);
        assert_eq!(HTLC_HDR_VOICE_SDP_ANSWER, 603);
        assert_eq!(HTLC_HDR_VOICE_ICE, 604);
        assert_eq!(HTLC_HDR_VOICE_MUTE, 606);
    }

    // ---- Wrong-cid SDP offer guards (Copilot review #3) ----

    /// Regression: an SDP offer for a different cid arriving in
    /// JoinSent must be dropped on the floor, mirroring the
    /// ICE / ROOM_STATUS handler shape. Before the guard, a
    /// stale 602 raced in by the previous server during a
    /// mid-session room switch would corrupt the mid cache and
    /// drive renegotiation against the wrong webrtcbin session.
    #[test]
    fn sdp_offer_with_wrong_cid_in_join_sent_is_dropped() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 7 });
        assert_eq!(m.state(), SessionState::JoinSent);
        let actions = m.step(Event::SdpOfferReceived {
            cid: 99, // not the active cid
            sdp: "a=mid:user-1\n".into(),
        });
        assert!(
            actions.is_empty(),
            "wrong-cid SDP offer must produce no actions; got {actions:?}"
        );
        assert_eq!(
            m.state(),
            SessionState::JoinSent,
            "state must not advance on a dropped offer"
        );
        // The mid_to_user cache must NOT have absorbed the
        // wrong-cid offer's mids. The offer's body contains
        // `a=mid:user-1`; if cache_offer_mids() had run
        // (regression shape) the cache would now hold the
        // "user-1" → 1 entry. Checking the private field
        // directly here (rather than the participants() iter)
        // is the precise invariant — a corrupted mid cache that
        // happens to have zero pad-added events would slip past
        // a count-based check.
        assert!(
            m.mid_to_user.is_empty(),
            "mid_to_user must stay empty on a dropped offer; got {:?}",
            m.mid_to_user
        );
    }

    /// Regression: same guard for the Connected (renegotiation)
    /// arm. A stale 602 arriving here would re-enter
    /// OfferPending and replace the live media path with bad
    /// SDP — the worst-case shape, since we have a working
    /// connection to break.
    #[test]
    fn sdp_offer_with_wrong_cid_in_connected_is_dropped() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 7 });
        m.step(Event::SdpOfferReceived {
            cid: 7,
            sdp: "a=mid:user-1\n".into(),
        });
        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        m.step(Event::WebrtcConnectionStateChanged {
            state: ConnectionState::Connected,
        });
        assert_eq!(m.state(), SessionState::Connected);

        let actions = m.step(Event::SdpOfferReceived {
            cid: 42, // wrong cid
            sdp: "a=mid:user-2\n".into(),
        });
        assert!(
            actions.is_empty(),
            "wrong-cid SDP renegotiation must produce no actions; got {actions:?}"
        );
        assert_eq!(
            m.state(),
            SessionState::Connected,
            "state must not move to OfferPending on a dropped renegotiation"
        );
    }

    /// Regression: same guard for the OfferPending replacement
    /// arm. A late offer from a previously-joined room must NOT
    /// supersede the in-flight offer for the active room.
    #[test]
    fn sdp_offer_with_wrong_cid_in_offer_pending_is_dropped() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 7 });
        let _ = m.step(Event::SdpOfferReceived {
            cid: 7,
            sdp: "a=mid:user-1\n".into(),
        });
        assert_eq!(m.state(), SessionState::OfferPending);

        let actions = m.step(Event::SdpOfferReceived {
            cid: 99, // wrong cid
            sdp: "a=mid:user-2\n".into(),
        });
        assert!(
            actions.is_empty(),
            "wrong-cid OfferPending replacement must produce no actions; got {actions:?}"
        );
        assert_eq!(m.state(), SessionState::OfferPending);
    }

    /// Counter-test: an offer with the correct cid in
    /// OfferPending is queued (Phase 8.C step 6 — used to
    /// be "replace immediately", now is "park in
    /// queued_offer"). Guards the wrong-cid-drop test
    /// above from silently passing because we broke the
    /// legitimate enqueue path.
    #[test]
    fn sdp_offer_with_correct_cid_in_offer_pending_queues() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 7 });
        let _ = m.step(Event::SdpOfferReceived {
            cid: 7,
            sdp: "a=mid:user-1\n".into(),
        });

        let actions = m.step(Event::SdpOfferReceived {
            cid: 7, // active cid
            sdp: "a=mid:user-2\n".into(),
        });
        // No actions on enqueue — that's the whole point of
        // step 6's serialisation.
        assert!(
            actions.is_empty(),
            "queued offer must not emit actions; got {actions:?}"
        );
        // The offer is parked, ready to drain when the
        // current answer comes in.
        assert!(m.queued_offer.is_some());
    }

    // ---- Phase 8.C step 6: renegotiation queue/drain ----

    /// Multiple offers arriving back-to-back while OfferPending
    /// — only the latest survives in the queue (spec
    /// §Renegotiation Flow: "process only the newest one").
    #[test]
    fn multiple_queued_offers_keep_only_the_latest() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        // First queued offer.
        m.step(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\na=mid:user-2\n".into(),
        });
        // Second queued offer — replaces the first.
        m.step(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\na=mid:user-7\n".into(),
        });
        let queued = m.queued_offer.as_ref().expect("an offer is queued");
        assert!(
            queued.1.contains("user-7"),
            "the latest queued offer must win, got: {:?}",
            queued.1
        );
        assert!(
            !queued.1.contains("user-2"),
            "the older queued offer must be discarded"
        );
    }

    /// Wrong-cid queued offer is dropped at enqueue time
    /// (not parked). Prevents a stale 602 from a previously-
    /// joined room from materialising as a renegotiation
    /// against the current room when the queue drains.
    #[test]
    fn wrong_cid_offer_is_not_queued_while_offer_pending() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        m.step(Event::SdpOfferReceived {
            cid: 99, // wrong cid
            sdp: "v=0\na=mid:user-7\n".into(),
        });
        assert!(
            m.queued_offer.is_none(),
            "wrong-cid offer must not enter the queue"
        );
    }

    /// Drain-time staleness: if `active_cid` no longer matches
    /// the queued offer's cid (because the session walked
    /// through a mid-session room-switch between enqueue and
    /// drain), the queued offer is dropped and the in-flight
    /// answer drives Connecting on its own.
    ///
    /// Regression for a wedge where the drain block returned
    /// `answer_actions` unconditionally — on a cid mismatch the
    /// machine would have stayed in OfferPending with no follow-
    /// up action queued.
    #[test]
    fn stale_queued_offer_does_not_wedge_the_drain() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        // Queue a renegotiation offer for cid=1.
        m.step(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\na=mid:user-2\n".into(),
        });
        assert!(m.queued_offer.is_some());

        // Simulate the mid-session room-switch path by hand: a
        // `JoinRequested { cid: 2 }` here would reset state to
        // JoinSent and clear the queue inline — we want to hit
        // the in-arm defensive re-check at drain time, not the
        // enqueue-arm guard. Force the mismatch by mutating
        // active_cid directly so we exercise the drain block's
        // fall-through path.
        m.active_cid = Some(2);

        let acts = m.step(Event::WebrtcAnswerCreated {
            sdp: "v=0\n".into(),
        });
        // The current answer still drives Connecting; the stale
        // queued offer is dropped.
        assert_eq!(
            m.state(),
            SessionState::Connecting,
            "stale queue must not wedge — machine must reach Connecting"
        );
        assert!(m.queued_offer.is_none());
        // The current answer's SetLocalDescription + 603 send
        // are emitted; the dropped offer contributes no
        // SetRemoteDescription / CreateAnswer.
        let has_set_remote = acts.iter().any(|a| {
            matches!(a, Action::SetRemoteDescription { .. })
        });
        let has_create_answer =
            acts.iter().any(|a| matches!(a, Action::CreateAnswer));
        assert!(
            !has_set_remote,
            "stale queued offer must not produce SetRemoteDescription"
        );
        assert!(
            !has_create_answer,
            "stale queued offer must not produce CreateAnswer"
        );
    }

    /// LeaveRequested clears the queue. Otherwise a leave +
    /// rejoin would surface the queued offer against the new
    /// session.
    #[test]
    fn leave_request_clears_queued_offer() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        m.step(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\na=mid:user-2\n".into(),
        });
        assert!(m.queued_offer.is_some());

        m.step(Event::LeaveRequested { cid: 1 });
        assert!(m.queued_offer.is_none());
    }

    /// A failure path (server-side rejection, terminal WebRTC
    /// state, etc.) also clears the queue — `fail()` resets
    /// `active_cid` and walks to `Leaving`, the queued offer
    /// would be stale against any subsequent fresh session.
    ///
    /// Driven through a `ServerTaskError` against the
    /// `HTLC_HDR_VOICE_SDP_ANSWER` opcode because that's the
    /// natural fail-causing event reachable while we're in
    /// `OfferPending` (the DTLS / ICE / Media timeouts are
    /// only matched in `Connecting` / `Connected`; an
    /// `OfferPending` Dtls Timeout would silently fall to the
    /// catch-all).
    #[test]
    fn fail_path_clears_queued_offer() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        m.step(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\na=mid:user-2\n".into(),
        });
        assert!(m.queued_offer.is_some());

        m.step(Event::ServerTaskError(ServerError {
            origin_opcode: HTLC_HDR_VOICE_SDP_ANSWER,
            text: "server rejected answer".into(),
        }));
        assert!(m.queued_offer.is_none());
        assert_eq!(m.state(), SessionState::Leaving);
    }

    /// Mid-session room switch (JoinRequested for a different
    /// cid while in voice) clears the queue. The queued offer
    /// belongs to the old room's webrtcbin session; the new
    /// room will issue its own.
    #[test]
    fn mid_session_room_switch_clears_queued_offer() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 1 });
        m.step(Event::SdpOfferReceived { cid: 1, sdp: "v=0\n".into() });
        m.step(Event::SdpOfferReceived {
            cid: 1,
            sdp: "v=0\na=mid:user-2\n".into(),
        });
        assert!(m.queued_offer.is_some());

        m.step(Event::JoinRequested { cid: 99 });
        assert!(m.queued_offer.is_none());
    }

    /// Happy path: drain to Connecting. When OfferPending sees
    /// WebrtcAnswerCreated AND queued_offer is None, the
    /// machine walks to Connecting (no queue drain to keep us
    /// stuck in OfferPending).
    #[test]
    fn empty_queue_lets_answer_walk_to_connecting() {
        let mut m = machine();
        m.step(Event::JoinRequested { cid: 4 });
        m.step(Event::SdpOfferReceived { cid: 4, sdp: "v=0\n".into() });
        assert!(m.queued_offer.is_none());

        m.step(Event::WebrtcAnswerCreated { sdp: "v=0\n".into() });
        assert_eq!(m.state(), SessionState::Connecting);
    }
}
