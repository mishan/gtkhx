//! Receive-dispatch routing: map an incoming server frame's `type` opcode to
//! the handler category `rcv.c`'s `hx_rcv_hdr` selects. This is the pure,
//! exhaustively-tested core of the receive dispatch (network-untangling N3) —
//! the opcode→handler decision, including the composite-`TASK` mask some servers
//! use — lifted out of the C `switch` so a mis-mapped opcode is caught by a unit
//! test rather than only in production against a live server.
//!
//! The C side (`hx_rcv_hdr`) keeps the per-kind side effects it always had (the
//! `POLITEQUIT` notice, the "unknown header type" log for [`HandlerKind::Unknown`])
//! and the `htlc->rcv` assignment; this crate just makes the decision.
//!
//! Opcode values mirror `src/hotline.h` (the wire authority). The unit tests
//! pin every one.

/// The handler category for a server→client opcode. `#[repr(i32)]` with
/// explicit discriminants; the C mirror `hx_recv_handler_kind`
/// (`hotline_proto.h`) restates the same integers and must be kept in step by
/// hand. What's tested is the routing *behaviour* — see
/// [`tests::every_wire_opcode_routes_to_its_handler`].
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HandlerKind {
    Chat = 0,
    Msg = 1,
    UserChange = 2,
    UserPart = 3,
    NewsPost = 4,
    Task = 5,
    ChatSubject = 6,
    ChatInvite = 7,
    UserSelfInfo = 8,
    Agreement = 9,
    Banner = 10,
    /// `POLITEQUIT` — handled by the msg handler, but the C side logs a notice
    /// first, so it gets its own kind.
    PoliteQuit = 11,
    XferQueue = 12,
    VoiceSdpOffer = 13,
    VoiceIce = 14,
    VoiceRoomStatus = 15,
    IconChange = 16,
    /// No recognised handler — the C `default` (log + `hx_rcv_dump`). Also the
    /// landing spot for voice opcodes in a `-Dvoice=disabled` build (the C
    /// kind→handler switch has no case for the voice kinds there).
    Unknown = 17,
}

// Opcode constants — mirror src/hotline.h. (ServerHdr in messages.rs covers a
// subset; the chat-room USER_CHANGE / USER_PART / SUBJECT / INVITE and a few
// others aren't in that enum, so the routing table names them all here.)
const HTLS_HDR_NEWS_POST: u32 = 0x0000_0066;
const HTLS_HDR_MSG: u32 = 0x0000_0068;
const HTLS_HDR_CHAT: u32 = 0x0000_006a;
const HTLS_HDR_AGREEMENT: u32 = 0x0000_006d;
const HTLS_HDR_POLITEQUIT: u32 = 0x0000_006f;
const HTLS_HDR_CHAT_INVITE: u32 = 0x0000_0071;
const HTLS_HDR_CHAT_USER_CHANGE: u32 = 0x0000_0075;
const HTLS_HDR_CHAT_USER_PART: u32 = 0x0000_0076;
const HTLS_HDR_CHAT_SUBJECT: u32 = 0x0000_0077;
const HTLS_HDR_BANNER: u32 = 0x0000_007a;
const HTLS_HDR_QUEUE: u32 = 0x0000_00d3;
const HTLS_HDR_USER_CHANGE: u32 = 0x0000_012d;
const HTLS_HDR_USER_PART: u32 = 0x0000_012e;
const HTLS_HDR_USER_SELFINFO: u32 = 0x0000_0162;
const HTLS_HDR_MSG_BROADCAST: u32 = 0x0000_0163;
const HTLS_HDR_VOICE_SDP_OFFER: u32 = 0x0000_025a;
const HTLS_HDR_VOICE_ICE: u32 = 0x0000_025c;
const HTLS_HDR_VOICE_ROOM_STATUS: u32 = 0x0000_025d;
const HTLS_HDR_ICON_CHANGE: u32 = 0x0000_0748;
const HTLS_HDR_TASK: u32 = 0x0001_0000;

/// Route a wire `type` opcode to its [`HandlerKind`].
///
/// The composite-`TASK` mask is applied first: some servers (Heidrun's
/// Inn-family) echo the request opcode in the low u16 of a TASK reply
/// (`0x0001_006b` = `TASK | LOGIN`), while standard servers send plain
/// `0x0001_0000`. Correlation is by `trans` inside `hx_rcv_task` either way, so
/// any `type` with the TASK bits in the high u16 routes to [`HandlerKind::Task`].
pub fn route(opcode: u32) -> HandlerKind {
    use HandlerKind::*;

    if opcode & 0xffff_0000 == HTLS_HDR_TASK {
        return Task;
    }

    match opcode {
        HTLS_HDR_CHAT => Chat,
        HTLS_HDR_MSG | HTLS_HDR_MSG_BROADCAST => Msg,
        HTLS_HDR_USER_CHANGE | HTLS_HDR_CHAT_USER_CHANGE => UserChange,
        HTLS_HDR_USER_PART | HTLS_HDR_CHAT_USER_PART => UserPart,
        HTLS_HDR_NEWS_POST => NewsPost,
        HTLS_HDR_CHAT_SUBJECT => ChatSubject,
        HTLS_HDR_CHAT_INVITE => ChatInvite,
        HTLS_HDR_USER_SELFINFO => UserSelfInfo,
        HTLS_HDR_AGREEMENT => Agreement,
        HTLS_HDR_BANNER => Banner,
        HTLS_HDR_POLITEQUIT => PoliteQuit,
        HTLS_HDR_QUEUE => XferQueue,
        HTLS_HDR_VOICE_SDP_OFFER => VoiceSdpOffer,
        HTLS_HDR_VOICE_ICE => VoiceIce,
        HTLS_HDR_VOICE_ROOM_STATUS => VoiceRoomStatus,
        HTLS_HDR_ICON_CHANGE => IconChange,
        _ => Unknown,
    }
}

/// `hx_recv_handler_kind hx_recv_route (guint32 opcode)` — route an opcode to
/// its [`HandlerKind`] discriminant, the integer the C `hx_recv_handler_kind`
/// enum (hotline_proto.h) mirrors. C `hx_dispatch_frame` switches on it instead
/// of the in-line opcode `switch`.
#[no_mangle]
pub extern "C" fn hx_recv_route(opcode: u32) -> i32 {
    route(opcode) as i32
}

#[cfg(test)]
mod tests {
    use super::HandlerKind::*;
    use super::*;

    #[test]
    fn every_wire_opcode_routes_to_its_handler() {
        // Each server→client opcode (from hotline.h, the wire authority) paired
        // with the handler category it must reach. This is the behaviour of the
        // extracted router: a mis-mapped opcode would misroute silently against a
        // live server. Aliases fold here too — MSG_BROADCAST rides the msg
        // handler, and the chat-room USER_CHANGE / USER_PART variants share the
        // roster handlers with their globals.
        let table: &[(u32, HandlerKind)] = &[
            (0x0000_006a, Chat),        // CHAT — 0x6a; guards the old 0x68/0x6a swap
            (0x0000_0068, Msg),         // MSG
            (0x0000_0163, Msg),         // MSG_BROADCAST folds onto msg
            (0x0000_012d, UserChange),  // USER_CHANGE
            (0x0000_0075, UserChange),  // CHAT_USER_CHANGE (room variant)
            (0x0000_012e, UserPart),    // USER_PART
            (0x0000_0076, UserPart),    // CHAT_USER_PART (room variant)
            (0x0000_0066, NewsPost),
            (0x0001_0000, Task),
            (0x0000_0077, ChatSubject),
            (0x0000_0071, ChatInvite),
            (0x0000_0162, UserSelfInfo),
            (0x0000_006d, Agreement),
            (0x0000_007a, Banner),
            (0x0000_006f, PoliteQuit),
            (0x0000_00d3, XferQueue),
            (0x0000_025a, VoiceSdpOffer),
            (0x0000_025c, VoiceIce),
            (0x0000_025d, VoiceRoomStatus),
            (0x0000_0748, IconChange),
        ];
        for &(opcode, kind) in table {
            assert_eq!(route(opcode), kind, "route(0x{opcode:08x})");
        }
    }

    #[test]
    fn composite_task_replies_fold_to_task() {
        // The non-trivial bit: standard servers send plain 0x0001_0000, but
        // Heidrun echoes the request opcode in the low u16 — every variant with
        // the TASK bits set must still route to the task handler.
        assert_eq!(route(0x0001_0000), Task);
        assert_eq!(route(0x0001_006b), Task); // TASK | LOGIN
        assert_eq!(route(0x0001_012c), Task); // TASK | USER_GETLIST
        assert_eq!(route(0x0001_ffff), Task);
    }

    #[test]
    fn unrecognised_opcodes_are_unknown() {
        assert_eq!(route(0x0000_0000), Unknown);
        assert_eq!(route(0xdead_beef), Unknown);
        assert_eq!(route(0x0000_01f4), Unknown); // Ping has no receive handler
    }
}
