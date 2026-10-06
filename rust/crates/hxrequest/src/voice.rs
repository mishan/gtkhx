//! Voice and video requests: joining and leaving a room's voice, the SDP
//! answer and ICE candidates, muting, and video's start, stop, state and
//! subscriptions. Each carries the room as its chat id, 0 the public chat.

use hxproto::build::HxChunk;
use hxproto::messages::ClientHdr;
use hxproto::video::{self, Stream, VideoKind};
use hxproto::voice;

use crate::Request;

/// VOICE_JOIN (600).
pub fn join(cid: u32) -> Option<Request> {
    let (mut chunks, mut scratch) = ([HxChunk::EMPTY; 1], [0u8; 4]);
    let hc = voice::build_voice_join_chunks(cid, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::VoiceJoin as u32, &chunks, hc)
}

/// VOICE_LEAVE (601).
pub fn leave(cid: u32) -> Option<Request> {
    let (mut chunks, mut scratch) = ([HxChunk::EMPTY; 1], [0u8; 4]);
    let hc = voice::build_voice_leave_chunks(cid, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::VoiceLeave as u32, &chunks, hc)
}

/// VOICE_SDP_ANSWER (603): our answer to the server's offer. An empty one
/// is refused.
pub fn sdp_answer(cid: u32, sdp: &[u8]) -> Option<Request> {
    let (mut chunks, mut scratch) = ([HxChunk::EMPTY; 2], [0u8; 4]);
    let hc = voice::build_voice_answer_chunks(cid, sdp, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::VoiceSdpAnswer as u32, &chunks, hc)
}

/// VOICE_ICE (604): one of our candidates, as the extension's JSON; empty
/// when there are no more.
pub fn ice(cid: u32, ice: &[u8]) -> Option<Request> {
    let (mut chunks, mut scratch) = ([HxChunk::EMPTY; 2], [0u8; 4]);
    let hc = voice::build_voice_ice_chunks(cid, ice, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::VoiceIce as u32, &chunks, hc)
}

/// VOICE_MUTE (606).
pub fn mute(cid: u32, muted: bool) -> Option<Request> {
    let (mut chunks, mut scratch) = ([HxChunk::EMPTY; 2], [0u8; 6]);
    let hc = voice::build_voice_mute_chunks(cid, muted.into(), &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::VoiceMute as u32, &chunks, hc)
}

/// VIDEO_START (607).
pub fn video_start(cid: u32, kind: VideoKind) -> Option<Request> {
    let (mut chunks, mut scratch) = ([HxChunk::EMPTY; 2], [0u8; 6]);
    let hc = video::build_video_start_chunks(cid, kind, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::VideoStart as u32, &chunks, hc)
}

/// VIDEO_STOP (608): `kind`, or with `None` everything we publish in the
/// room.
pub fn video_stop(cid: u32, kind: Option<VideoKind>) -> Option<Request> {
    let (mut chunks, mut scratch) = ([HxChunk::EMPTY; 2], [0u8; 6]);
    let hc = video::build_video_stop_chunks(cid, kind, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::VideoStop as u32, &chunks, hc)
}

/// VIDEO_STATE (609): pause or resume `kind`.
pub fn video_state(cid: u32, kind: VideoKind, paused: bool) -> Option<Request> {
    let (mut chunks, mut scratch) = ([HxChunk::EMPTY; 3], [0u8; 8]);
    let hc = video::build_video_state_chunks(cid, kind, paused, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::VideoState as u32, &chunks, hc)
}

/// VIDEO_SUBSCRIBE (610): every stream we want to see, as the packed
/// four-byte `uid | kind` entries the field carries. An entry of a kind
/// the extension doesn't define is dropped; an empty set says no video.
pub fn video_subscribe(cid: u32, streams: &[u8]) -> Option<Request> {
    let set: Vec<Stream> = video::parse_video_subscriptions(streams).collect();
    let mut chunks = [HxChunk::EMPTY; 2];
    let mut scratch = vec![0u8; video::video_subscribe_scratch_len(set.len())];
    let hc = video::build_video_subscribe_chunks(cid, &set, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::VideoSubscribe as u32, &chunks, hc)
}

#[cfg(test)]
mod tests;
