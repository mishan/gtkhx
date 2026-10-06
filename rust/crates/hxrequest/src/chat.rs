//! Chat requests.

use hxproto::build::{self, GetChatHistoryRequest, HxChunk};
use hxproto::messages::ClientHdr;

use crate::Request;

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
