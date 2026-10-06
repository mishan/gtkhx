//! File-browser requests: listing, mkdir, delete, get info, move / rename, and
//! the folder-transfer kickoffs.
//!
//! Paths are remote `/`-separated paths, and paths and names are the bytes the
//! server knows them by: a listing's, as it sent them, which a decoded name
//! does not always encode back to, or a new name already encoded as the
//! connection sends text. A comment is text as the user typed it, encoded here
//! — passed through when the connection negotiated UTF-8 (`utf8`), Mac Roman
//! otherwise. A name travels as its own chunk wherever the wire allows, so one
//! holding a `/` survives; the flat-path requests (delete, move) take the last
//! component as the name.
//!
//! Each builder returns `None` for input the wire can't carry: a chunk longer
//! than its u16 length allows.

use hxproto::build::{self, FileMoveRequest, FilePutFolderRequest, FileSetInfoRequest, HxChunk};
use hxproto::messages::ClientHdr;

use crate::path::{below_root, encode_dir, split};
use crate::Request;

/// DOWNLOAD_BANNER: the server's banner, for a transfer connection to
/// fetch. No fields.
pub fn banner() -> Request {
    Request {
        opcode: ClientHdr::DownloadBanner as u32,
        chunks: Vec::new(),
    }
}

/// FILE_LIST for the folder at `dir`.
pub fn list(dir: &[u8]) -> Option<Request> {
    let enc = encode_dir(dir, false);
    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_file_list_chunks(&enc, &mut chunks);
    Request::from_built(ClientHdr::FileList as u32, &chunks, hc)
}

/// FILE_MKDIR for a new folder at `path`.
pub fn mkdir(path: &[u8]) -> Option<Request> {
    let enc = encode_dir(path, false);
    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_file_mkdir_chunks(&enc, &mut chunks);
    Request::from_built(ClientHdr::FileMkdir as u32, &chunks, hc)
}

/// FILE_DELETE for the file or folder at `path`.
pub fn delete(path: &[u8]) -> Option<Request> {
    let (dir, name) = split(path);
    let enc = (!dir.is_empty()).then(|| encode_dir(path, true));
    let mut chunks = [HxChunk::EMPTY; 2];
    let hc = build::build_file_delete_chunks(name, enc.as_deref(), &mut chunks);
    Request::from_built(ClientHdr::FileDelete as u32, &chunks, hc)
}

/// FILE_GETINFO for `name` in the folder `dir`.
pub fn get_info(dir: &[u8], name: &[u8]) -> Option<Request> {
    let enc = below_root(dir).then(|| encode_dir(dir, false));
    let mut chunks = [HxChunk::EMPTY; 2];
    let hc = build::build_file_getinfo_chunks(name, enc.as_deref(), &mut chunks);
    Request::from_built(ClientHdr::FileGetInfo as u32, &chunks, hc)
}

/// FILE_SETINFO renaming the file or folder at `path` to `rename`, and setting
/// its comment, each when given (the Get Info dialog's Save).
///
/// A `rename` that is the current name goes out as no rename at all: Janus
/// 2.0.13 and earlier answers a rename to the item's own name with an error,
/// even though it saves the comment.
pub fn set_info(
    path: &[u8],
    rename: Option<&[u8]>,
    comment: Option<&[u8]>,
    utf8: bool,
) -> Option<Request> {
    let (dir, name) = split(path);
    let rename = rename.filter(|r| *r != name);
    let comment = comment.map(|c| hxtext::for_wire(c, utf8, true));
    let enc = (!dir.is_empty()).then(|| encode_dir(path, true));
    let req = FileSetInfoRequest {
        name,
        rename,
        comment: comment.as_deref(),
        dir: enc.as_deref(),
    };
    let mut chunks = [HxChunk::EMPTY; 4];
    let hc = build::build_file_setinfo_chunks(&req, &mut chunks);
    Request::from_built(ClientHdr::FileSetInfo as u32, &chunks, hc)
}

/// Move and/or rename the file or folder at `src` to `dst`.
///
/// Hotline splits the two: FILE_MOVE changes the directory and keeps the name,
/// and FILE_SETINFO renames within a directory. A move that also renames is
/// both: the move, then the rename in the directory the item has moved to.
/// **Send the rename only once the move has succeeded** — after a failed move,
/// the destination may hold a different item under the old name, and the
/// rename would act on that one. Moving a path onto itself is no requests.
pub fn moves(src: &[u8], dst: &[u8]) -> Vec<Request> {
    let (_, src_name) = split(src);
    let (_, dst_name) = split(dst);
    // The directory prefixes, each through its trailing separator.
    let src_prefix = &src[..src.len() - src_name.len()];
    let dst_prefix = &dst[..dst.len() - dst_name.len()];
    let src_dir = encode_dir(src, true);

    let mut out = Vec::new();
    // Where the item is when the rename reaches the server.
    let mut rename_dir = src_dir.clone();
    if !dst_prefix.is_empty() && dst_prefix != src_prefix {
        let dst_dir = encode_dir(dst, true);
        let req = FileMoveRequest {
            name: src_name,
            dir: &src_dir,
            dir_rename: &dst_dir,
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let hc = build::build_file_move_chunks(&req, &mut chunks);
        out.extend(Request::from_built(ClientHdr::FileMove as u32, &chunks, hc));
        rename_dir = dst_dir;
    }
    if !dst_name.is_empty() && src_name != dst_name {
        let req = FileSetInfoRequest {
            name: src_name,
            rename: Some(dst_name),
            comment: None,
            dir: Some(&rename_dir),
        };
        let mut chunks = [HxChunk::EMPTY; 4];
        let hc = build::build_file_setinfo_chunks(&req, &mut chunks);
        out.extend(Request::from_built(
            ClientHdr::FileSetInfo as u32,
            &chunks,
            hc,
        ));
    }
    out
}

/// FILE_GETFOLDER for the folder `name` in `dir`.
pub fn get_folder(dir: &[u8], name: &[u8]) -> Option<Request> {
    let enc = below_root(dir).then(|| encode_dir(dir, false));
    let mut chunks = [HxChunk::EMPTY; 2];
    let hc = build::build_file_getfolder_chunks(name, enc.as_deref(), &mut chunks);
    Request::from_built(ClientHdr::FileGetFolder as u32, &chunks, hc)
}

/// FILE_PUTFOLDER for a folder `name` uploaded into `dir`, carrying the tree's
/// byte total (clamped to the wire's 32 bits) and file count, which the server
/// shows in its queue.
pub fn put_folder(dir: &[u8], name: &[u8], total_bytes: u64, nfiles: u32) -> Option<Request> {
    let enc = below_root(dir).then(|| encode_dir(dir, false));
    let req = FilePutFolderRequest {
        name,
        dir: enc.as_deref(),
        size: total_bytes.min(u64::from(u32::MAX)) as u32,
        nfiles,
    };
    let mut chunks = [HxChunk::EMPTY; 4];
    let mut scratch = [0u8; 8];
    let hc = build::build_file_putfolder_chunks(&req, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::FilePutFolder as u32, &chunks, hc)
}

#[cfg(test)]
mod tests;
