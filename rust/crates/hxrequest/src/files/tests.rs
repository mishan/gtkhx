//! The file requests, pinned against hand-written wire bytes.

use super::*;

const TAG_HTXF_SIZE: u16 = 0x006c;
const TAG_FILE_NAME: u16 = 0x00c9;
const TAG_DIR: u16 = 0x00ca;
const TAG_FILE_COMMENT: u16 = 0x00d2;
const TAG_FILE_RENAME: u16 = 0x00d3;
const TAG_DIR_RENAME: u16 = 0x00d4;
const TAG_FILE_NFILES: u16 = 0x00dc;

fn req(opcode: ClientHdr, chunks: &[(u16, &[u8])]) -> Request {
    Request {
        opcode: opcode as u32,
        chunks: chunks.iter().map(|(t, d)| (*t, d.to_vec())).collect(),
    }
}

fn dir(path: &str) -> Vec<u8> {
    encode_dir(path.as_bytes(), false)
}

#[test]
fn list_always_names_a_directory_even_the_root() {
    assert_eq!(
        list(b"/").unwrap(),
        req(ClientHdr::FileList, &[(TAG_DIR, &[0, 0])])
    );
    assert_eq!(
        list(b"/Uploads").unwrap(),
        req(ClientHdr::FileList, &[(TAG_DIR, b"\0\x01\0\0\x07Uploads")])
    );
}

#[test]
fn mkdir_names_the_whole_path() {
    assert_eq!(
        mkdir(b"/a/new").unwrap(),
        req(ClientHdr::FileMkdir, &[(TAG_DIR, &dir("/a/new"))])
    );
}

#[test]
fn delete_sends_the_name_and_its_directory() {
    assert_eq!(
        delete(b"/a/b/file", true).unwrap(),
        req(
            ClientHdr::FileDelete,
            &[(TAG_FILE_NAME, b"file"), (TAG_DIR, &dir("/a/b"))]
        )
    );
    // At the root there is still a (componentless) directory.
    assert_eq!(
        delete(b"/file", true).unwrap(),
        req(
            ClientHdr::FileDelete,
            &[(TAG_FILE_NAME, b"file"), (TAG_DIR, &[0, 0])]
        )
    );
    // A bare name has no directory at all.
    assert_eq!(
        delete(b"file", true).unwrap(),
        req(ClientHdr::FileDelete, &[(TAG_FILE_NAME, b"file")])
    );
}

#[test]
fn names_are_mac_roman_unless_utf8_was_negotiated() {
    let name = "caf\u{e9}".as_bytes();
    let r = get_info(b"/", name, false).unwrap();
    assert_eq!(r.chunk(TAG_FILE_NAME).unwrap(), b"caf\x8e");
    let r = get_info(b"/", name, true).unwrap();
    assert_eq!(r.chunk(TAG_FILE_NAME).unwrap(), name);
    // Outside Mac Roman: a question mark.
    let r = get_info(b"/", "\u{4e2d}".as_bytes(), false).unwrap();
    assert_eq!(r.chunk(TAG_FILE_NAME).unwrap(), b"?");
}

#[test]
fn get_info_keeps_a_slash_inside_the_name() {
    let r = get_info(b"/pub", b"AC/DC", true).unwrap();
    assert_eq!(
        r,
        req(
            ClientHdr::FileGetInfo,
            &[(TAG_FILE_NAME, b"AC/DC"), (TAG_DIR, &dir("/pub"))]
        )
    );
    assert_eq!(
        get_info(b"/", b"x", true).unwrap(),
        req(ClientHdr::FileGetInfo, &[(TAG_FILE_NAME, b"x")])
    );
}

#[test]
fn set_info_carries_a_body_encoded_comment() {
    let r = set_info(b"/pub/old", b"new", Some(b"line1\nline2"), false).unwrap();
    assert_eq!(
        r,
        req(
            ClientHdr::FileSetInfo,
            &[
                (TAG_FILE_NAME, b"old"),
                (TAG_FILE_RENAME, b"new"),
                (TAG_FILE_COMMENT, b"line1\rline2"),
                (TAG_DIR, &dir("/pub")),
            ]
        )
    );
}

#[test]
fn set_info_keeping_the_name_sends_no_rename() {
    let r = set_info(b"/pub/same", b"same", Some(b"note"), false).unwrap();
    assert_eq!(
        r,
        req(
            ClientHdr::FileSetInfo,
            &[
                (TAG_FILE_NAME, b"same"),
                (TAG_FILE_COMMENT, b"note"),
                (TAG_DIR, &dir("/pub")),
            ]
        )
    );
}

#[test]
fn a_move_across_directories_is_one_move() {
    assert_eq!(
        moves(b"/a/f", b"/b/f", true),
        vec![req(
            ClientHdr::FileMove,
            &[
                (TAG_FILE_NAME, b"f"),
                (TAG_DIR, &dir("/a")),
                (TAG_DIR_RENAME, &dir("/b")),
            ]
        )]
    );
}

#[test]
fn a_rename_in_place_is_one_setinfo() {
    assert_eq!(
        moves(b"/a/old", b"/a/new", true),
        vec![req(
            ClientHdr::FileSetInfo,
            &[
                (TAG_FILE_NAME, b"old"),
                (TAG_FILE_RENAME, b"new"),
                (TAG_DIR, &dir("/a")),
            ]
        )]
    );
}

#[test]
fn a_move_and_rename_is_a_move_then_a_rename_where_it_landed() {
    let r = moves(b"/a/old", b"/bb/new", true);
    assert_eq!(r.len(), 2);
    assert_eq!(r[0].opcode, ClientHdr::FileMove as u32);
    assert_eq!(r[0].chunk(TAG_FILE_NAME).unwrap(), b"old");
    assert_eq!(
        r[1],
        req(
            ClientHdr::FileSetInfo,
            &[
                (TAG_FILE_NAME, b"old"),
                (TAG_FILE_RENAME, b"new"),
                (TAG_DIR, &dir("/bb")),
            ]
        )
    );
}

#[test]
fn moving_onto_itself_or_to_a_bare_name_sends_what_it_can() {
    assert!(moves(b"/a/f", b"/a/f", true).is_empty());
    // A destination with no directory can only rename.
    let r = moves(b"/a/f", b"g", true);
    assert_eq!(r.len(), 1);
    assert_eq!(r[0].opcode, ClientHdr::FileSetInfo as u32);
    // A destination with no name can only move.
    let r = moves(b"/a/f", b"/b/", true);
    assert_eq!(r.len(), 1);
    assert_eq!(r[0].opcode, ClientHdr::FileMove as u32);
}

#[test]
fn folder_get_names_the_parent() {
    assert_eq!(
        get_folder(b"/pub", b"Album", true).unwrap(),
        req(
            ClientHdr::FileGetFolder,
            &[(TAG_FILE_NAME, b"Album"), (TAG_DIR, &dir("/pub"))]
        )
    );
    assert_eq!(
        get_folder(b"/", b"Album", true).unwrap(),
        req(ClientHdr::FileGetFolder, &[(TAG_FILE_NAME, b"Album")])
    );
}

#[test]
fn folder_put_carries_the_totals_and_clamps_the_size() {
    assert_eq!(
        put_folder(b"/up", b"Tree", 15, 2, true).unwrap(),
        req(
            ClientHdr::FilePutFolder,
            &[
                (TAG_FILE_NAME, b"Tree"),
                (TAG_DIR, &dir("/up")),
                (TAG_HTXF_SIZE, &15u32.to_be_bytes()),
                (TAG_FILE_NFILES, &2u32.to_be_bytes()),
            ]
        )
    );
    let r = put_folder(b"/", b"Big", 5 << 32, 1, true).unwrap();
    assert_eq!(r.chunk(TAG_HTXF_SIZE).unwrap(), u32::MAX.to_be_bytes());
}

#[test]
fn a_name_too_long_for_its_chunk_is_refused() {
    let huge = vec![b'n'; 70_000];
    assert!(get_info(b"/", &huge, true).is_none());
    assert!(delete(&huge, true).is_none());
}

#[test]
fn pack_frames_the_request() {
    let r = mkdir(b"/x").unwrap();
    let f = r.pack(7);
    let h = hxproto::parse::Header::parse(&f).unwrap();
    assert_eq!(h.type_, ClientHdr::FileMkdir as u32);
    assert_eq!(h.trans, 7);
    assert_eq!(f.len(), 22 + 4 + dir("/x").len());
}
