//! Inline-media requests: a picture going up, whole or in parts, and one
//! coming down, part by part.

use hxproto::build::HxChunk;
use hxproto::inline_media::{self, DownloadMedia, UploadMediaFirst, UploadMediaFollowup};
use hxproto::messages::ClientHdr;

use crate::Request;

/// UPLOAD_MEDIA: a picture that goes up whole. `mime` is a hint the server
/// may ignore.
pub fn upload(payload: &[u8], mime: Option<&[u8]>) -> Option<Request> {
    let req = inline_media::UploadMediaSingle {
        payload,
        declared_type: mime,
    };
    let (mut chunks, mut scratch) = ([HxChunk::EMPTY; 3], [0u8; 1]);
    let hc = inline_media::build_upload_media_single_chunks(&req, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::UploadMedia as u32, &chunks, hc)
}

/// UPLOAD_MEDIA: the first of the `parts` a picture goes up in.
pub fn upload_first(payload: &[u8], mime: Option<&[u8]>, parts: u16) -> Option<Request> {
    let req = UploadMediaFirst {
        payload,
        declared_type: mime,
        part_count: parts,
    };
    let (mut chunks, mut scratch) = ([HxChunk::EMPTY; 5], [0u8; 5]);
    let hc = inline_media::build_upload_media_first_chunks(&req, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::UploadMedia as u32, &chunks, hc)
}

/// UPLOAD_MEDIA: part `index` after the first, on the `token` the first's
/// reply gave.
pub fn upload_next(token: &[u8], payload: &[u8], index: u16, last: bool) -> Option<Request> {
    let req = UploadMediaFollowup {
        upload_token: token,
        payload,
        part_index: index,
        final_chunk: last,
    };
    let (mut chunks, mut scratch) = ([HxChunk::EMPTY; 4], [0u8; 3]);
    let hc = inline_media::build_upload_media_followup_chunks(&req, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::UploadMedia as u32, &chunks, hc)
}

/// DOWNLOAD_MEDIA: the picture `id` names, or part `part` of it after the
/// first.
pub fn download(id: &[u8], part: Option<u16>) -> Option<Request> {
    let req = DownloadMedia {
        media_id: id,
        part_index: part,
    };
    let (mut chunks, mut scratch) = ([HxChunk::EMPTY; 2], [0u8; 2]);
    let hc = inline_media::build_download_media_chunks(&req, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::DownloadMedia as u32, &chunks, hc)
}

#[cfg(test)]
mod tests;
