//! Chat requests.

use hxproto::build::{self, ChatRequest, GetChatHistoryRequest, HxChunk};
use hxproto::messages::{tag, ClientHdr};
use hxproto::target::Target;

use crate::Request;

/// CHAT (105): `body`, already encoded for the wire, in chat `cid` (0 for
/// the public chat) as `style`, with the picture `media` names (its handle
/// and type) and the line `reply` answers. `None` for a body or a media
/// field past a field's length, or a reply that has no packed form.
pub fn say(
    body: &[u8],
    cid: u32,
    style: u16,
    media: Option<(&[u8], &[u8])>,
    reply: Option<&Target>,
) -> Option<Request> {
    let mut chunks = [HxChunk::EMPTY; 3];
    let mut scratch = [0u8; 6];
    let req = ChatRequest { cid, style, body };
    let hc = build::build_chat_chunks(&req, &mut chunks, &mut scratch);
    let mut r = Request::from_built(ClientHdr::Chat as u32, &chunks, hc)?;
    if let Some((id, mime)) = media {
        if id.len().max(mime.len()) > usize::from(u16::MAX) {
            return None;
        }
        r.chunks.push((tag::CHAT_MEDIA_ID, id.to_vec()));
        r.chunks.push((tag::CHAT_MEDIA_TYPE, mime.to_vec()));
    }
    if let Some(t) = reply {
        r.chunks.push((tag::REPLY_TO, t.pack()?));
    }
    Some(r)
}

/// GET_CHAT_HISTORY (700): up to `limit` lines of chat `cid`, older than line
/// `before` or newer than line `after`; a cursor or limit of 0 is left out.
pub fn history(cid: u32, before: u64, after: u64, limit: u16) -> Request {
    let mut chunks = [HxChunk::EMPTY; 4];
    let mut scratch = [0u8; 22];
    let req = GetChatHistoryRequest {
        channel_id: cid,
        before,
        after,
        limit,
    };
    let hc = build::build_get_chat_history_chunks(&req, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::GetChatHistory as u32, &chunks, hc)
        .expect("a history request always has its channel")
}

/// GET_CHAT_HISTORY (700) under the Replies draft: chat `cid`'s lines
/// `ids`, by id; `None` for none or more than
/// [`hxproto::replies::MAX_IDS`].
pub fn history_ids(cid: u32, ids: &[u64]) -> Option<Request> {
    const MOST: usize = hxproto::replies::MAX_IDS;
    let mut chunks = [HxChunk::EMPTY; 1 + MOST];
    let mut scratch = [0u8; 4 + 8 * MOST];
    let hc = build::build_get_chat_history_ids_chunks(cid, ids, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::GetChatHistory as u32, &chunks, hc)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_picture_field_past_a_fields_length_is_refused() {
        let most = vec![0u8; usize::from(u16::MAX)];
        let past = vec![0u8; usize::from(u16::MAX) + 1];
        for (case, id, mime, sent) in [
            ("both at the most", &most, &most, true),
            ("a handle past it", &past, &most, false),
            ("a type past it", &most, &past, false),
        ] {
            let got = say(b"hi", 0, 0, Some((id, mime)), None);
            assert_eq!(got.is_some(), sent, "{case}");
        }
    }
}
