//! The friend family of fogWraith's messaging extension (capability bit 6):
//! the roster, friend requests, blocks, presence, instant messages with
//! their receipts and typing, finding an account, a friend's alias, and
//! the message requests draft's listing, answers and clearing.
//!
//! A Login is bytes as the wire carries it: what an entry gave, or what
//! [`login`] made of what the user typed. Text the user wrote (a note, a
//! status, a message, an alias) is UTF-8, encoded for the connection here.
//!
//! Each builder returns `None` for what the wire can't carry: an empty
//! Login, or a field longer than its u16 length allows.

use hxproto::build::HxChunk;
use hxproto::messages::ClientHdr;
use hxproto::messaging::{self as m, AckType, PresenceState, RequestAction, TypingState, GUID_LEN};

use crate::Request;

fn built(
    opcode: ClientHdr,
    fill: impl FnOnce(&mut [HxChunk], &mut [u8]) -> usize,
) -> Option<Request> {
    let mut chunks = [HxChunk::EMPTY; 4];
    let mut scratch = [0u8; m::SCRATCH_LEN];
    let hc = fill(&mut chunks, &mut scratch);
    Request::from_built(opcode as u32, &chunks, hc)
}

fn text(t: &[u8], utf8: bool) -> Vec<u8> {
    hxtext::for_wire(t, utf8, false)
}

/// A Login the user typed, as the wire carries it.
pub fn login(typed: &[u8], utf8: bool) -> Vec<u8> {
    text(typed, utf8)
}

/// GET_ROSTER (800): every entry, and what waited for us. No fields.
pub fn roster() -> Request {
    Request {
        opcode: ClientHdr::GetRoster as u32,
        chunks: Vec::new(),
    }
}

/// ADD_FRIEND (802): ask `login` to be a friend, with a `note`.
pub fn add_friend(login: &[u8], note: Option<&[u8]>, utf8: bool) -> Option<Request> {
    let note = note.map(|n| text(n, utf8));
    built(ClientHdr::AddFriend, |c, s| {
        m::build_add_friend_chunks(login, note.as_deref(), None, c, s)
    })
}

/// REMOVE_FRIEND (803): end a friendship, or withdraw a request.
pub fn remove_friend(login: &[u8]) -> Option<Request> {
    built(ClientHdr::RemoveFriend, |c, s| {
        m::build_remove_friend_chunks(m::Who::Login(login), None, None, c, s)
    })
}

/// FRIEND_RESPONSE (805): accept `login`'s request, or decline it.
pub fn respond(login: &[u8], accept: bool) -> Option<Request> {
    built(ClientHdr::FriendResponse, |c, s| {
        m::build_friend_response_chunks(login, accept, None, c, s)
    })
}

/// BLOCK_USER (806).
pub fn block(login: &[u8]) -> Option<Request> {
    built(ClientHdr::BlockUser, |c, s| {
        m::build_block_user_chunks(login, None, c, s)
    })
}

/// UNBLOCK_USER (807).
pub fn unblock(login: &[u8]) -> Option<Request> {
    built(ClientHdr::UnblockUser, |c, s| {
        m::build_login_only_chunks(login, c, s)
    })
}

/// SET_PRESENCE (808): our presence, and our status text where `status` is
/// given; an empty one clears it; and whether our account takes message
/// requests, where `takes_requests` says. Never offline, which a client
/// does not set for itself.
pub fn set_presence(
    state: PresenceState,
    status: Option<&[u8]>,
    takes_requests: Option<bool>,
    utf8: bool,
) -> Option<Request> {
    let status = status.map(|t| text(t, utf8));
    let p = m::Presence {
        state,
        status_text: status.as_deref(),
        discoverable: None,
        takes_requests,
    };
    built(ClientHdr::SetPresence, |c, s| {
        m::build_set_presence_chunks(&p, c, s)
    })
}

/// IM_SEND (810): `body` to `to`, under the `guid` its receipts name.
pub fn im_send(to: &[u8], guid: &[u8; GUID_LEN], body: &[u8], utf8: bool) -> Option<Request> {
    let body = hxtext::for_wire(body, utf8, true);
    let send = m::ImSend {
        to,
        guid,
        content: m::ImContent::Plain(&body),
    };
    built(ClientHdr::ImSend, |c, s| {
        m::build_im_send_chunks(&send, c, s)
    })
}

/// IM_ACK (812): `sender`'s message `guid` was delivered, or read.
pub fn im_ack(guid: &[u8; GUID_LEN], ack: AckType, sender: &[u8]) -> Option<Request> {
    built(ClientHdr::ImAck, |c, s| {
        m::build_im_ack_chunks(guid, ack, sender, c, s)
    })
}

/// IM_TYPING (813): we started, or stopped, typing to `login`.
pub fn im_typing(login: &[u8], typing: bool) -> Option<Request> {
    let state = if typing {
        TypingState::Started
    } else {
        TypingState::Stopped
    };
    built(ClientHdr::ImTyping, |c, s| {
        m::build_im_typing_chunks(login, state, c, s)
    })
}

/// FIND_USER (822): the account `login` names exactly, as an entry.
pub fn find_user(login: &[u8]) -> Option<Request> {
    built(ClientHdr::FindUser, |c, s| {
        m::build_find_user_chunks(m::Who::Login(login), c, s)
    })
}

/// SET_FRIEND_NICKNAME (824): our alias for `login`; an empty one clears it.
pub fn set_nickname(login: &[u8], nickname: &[u8], utf8: bool) -> Option<Request> {
    let nickname = text(nickname, utf8);
    built(ClientHdr::SetFriendNickname, |c, s| {
        m::build_set_friend_nickname_chunks(login, &nickname, c, s)
    })
}

/// GET_PROFILE (825), the spec's Get User Info: what `login` shows of
/// itself.
pub fn profile(login: &[u8]) -> Option<Request> {
    built(ClientHdr::GetProfile, |c, s| {
        m::build_login_only_chunks(login, c, s)
    })
}

/// GET_MESSAGE_REQUESTS (829): the first page of our requests and
/// approvals, with no fields, or the page at the `cursor` the one before
/// gave.
pub fn message_requests(cursor: Option<&[u8]>) -> Option<Request> {
    let Some(cursor) = cursor else {
        return Some(Request {
            opcode: ClientHdr::GetMessageRequests as u32,
            chunks: Vec::new(),
        });
    };
    built(ClientHdr::GetMessageRequests, |c, s| {
        m::build_get_message_requests_chunks(cursor, c, s)
    })
}

/// ANSWER_MESSAGE_REQUEST (830): approve or delete `login`'s request, or
/// end its approval.
pub fn answer_request(login: &[u8], action: RequestAction) -> Option<Request> {
    built(ClientHdr::AnswerMessageRequest, |c, s| {
        m::build_answer_message_request_chunks(login, action, c, s)
    })
}

/// CLEAR_MESSAGE_REQUESTS (831): every request and friend request received
/// from `since` to `before`, in Unix seconds, either end open; with
/// neither, everything, which the request says by carrying no fields.
pub fn clear_requests(since: Option<u64>, before: Option<u64>) -> Request {
    if since.is_none() && before.is_none() {
        return Request {
            opcode: ClientHdr::ClearMessageRequests as u32,
            chunks: Vec::new(),
        };
    }
    built(ClientHdr::ClearMessageRequests, |c, s| {
        m::build_clear_message_requests_chunks(since, before, c, s)
    })
    .expect("a span with an end always builds")
}

#[cfg(test)]
mod tests;
