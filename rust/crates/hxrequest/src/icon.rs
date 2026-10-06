//! GIF-icon requests: everyone's icon, one user's, and setting ours.

use hxproto::build::HxChunk;
use hxproto::gif_icons;
use hxproto::messages::ClientHdr;

use crate::Request;

/// ICON_GETLIST: every user's icon. No fields.
pub fn list() -> Request {
    Request {
        opcode: ClientHdr::IconGetList as u32,
        chunks: Vec::new(),
    }
}

/// ICON_GET: `uid`'s icon.
pub fn get(uid: u16) -> Option<Request> {
    let mut chunks = [HxChunk::EMPTY; 1];
    let mut scratch = [0u8; 2];
    let hc = gif_icons::build_icon_get_chunks(uid, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::IconGet as u32, &chunks, hc)
}

/// ICON_SET: our icon becomes `gif`, or none when it is empty.
pub fn set(gif: &[u8]) -> Option<Request> {
    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = gif_icons::build_icon_set_chunks(gif, &mut chunks);
    Request::from_built(ClientHdr::IconSet as u32, &chunks, hc)
}

#[cfg(test)]
mod tests;
