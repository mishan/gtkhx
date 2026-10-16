//! The file requests, pinned against hand-written wire bytes.

use super::*;

const TAG_HTXF_SIZE: u16 = 0x006c;
const TAG_FILE_NAME: u16 = 0x00c9;
const TAG_DIR: u16 = 0x00ca;
const TAG_FILE_PREVIEW: u16 = 0x00cc;
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
fn the_banner_is_asked_for_with_no_fields() {
    let r = banner();
    assert_eq!((r.opcode, r.chunks.len()), (212, 0));
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
        delete(b"/a/b/file").unwrap(),
        req(
            ClientHdr::FileDelete,
            &[(TAG_FILE_NAME, b"file"), (TAG_DIR, &dir("/a/b"))]
        )
    );
    // At the root there is still a (componentless) directory.
    assert_eq!(
        delete(b"/file").unwrap(),
        req(
            ClientHdr::FileDelete,
            &[(TAG_FILE_NAME, b"file"), (TAG_DIR, &[0, 0])]
        )
    );
    // A bare name has no directory at all.
    assert_eq!(
        delete(b"file").unwrap(),
        req(ClientHdr::FileDelete, &[(TAG_FILE_NAME, b"file")])
    );
}

/// A name goes as the bytes it is given: a listing's, which a decode and an
/// encode would not give back.
#[test]
fn names_go_as_their_bytes() {
    for name in [&b"caf\x8e"[..], "caf\u{e9}".as_bytes()] {
        let r = get_info(b"/", name).unwrap();
        assert_eq!(r.chunk(TAG_FILE_NAME).unwrap(), name);
        let r = delete(&[b"/a/", name].concat()).unwrap();
        assert_eq!(r.chunk(TAG_FILE_NAME).unwrap(), name);
    }
}

#[test]
fn get_info_keeps_a_slash_inside_the_name() {
    let r = get_info(b"/pub", b"AC/DC").unwrap();
    assert_eq!(
        r,
        req(
            ClientHdr::FileGetInfo,
            &[(TAG_FILE_NAME, b"AC/DC"), (TAG_DIR, &dir("/pub"))]
        )
    );
    assert_eq!(
        get_info(b"/", b"x").unwrap(),
        req(ClientHdr::FileGetInfo, &[(TAG_FILE_NAME, b"x")])
    );
}

#[test]
fn set_info_carries_a_body_encoded_comment() {
    let r = set_info(b"/pub/old", Some(b"new"), Some(b"line1\nline2"), false).unwrap();
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
    let r = set_info(b"/pub/same", Some(b"same"), Some(b"note"), false).unwrap();
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
        moves(b"/a/f", b"/b/f"),
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
        moves(b"/a/old", b"/a/new"),
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
    let r = moves(b"/a/old", b"/bb/new");
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
    assert!(moves(b"/a/f", b"/a/f").is_empty());
    // A destination with no directory can only rename.
    let r = moves(b"/a/f", b"g");
    assert_eq!(r.len(), 1);
    assert_eq!(r[0].opcode, ClientHdr::FileSetInfo as u32);
    // A destination with no name can only move.
    let r = moves(b"/a/f", b"/b/");
    assert_eq!(r.len(), 1);
    assert_eq!(r[0].opcode, ClientHdr::FileMove as u32);
}

#[test]
fn folder_get_names_the_parent() {
    assert_eq!(
        get_folder(b"/pub", b"Album").unwrap(),
        req(
            ClientHdr::FileGetFolder,
            &[(TAG_FILE_NAME, b"Album"), (TAG_DIR, &dir("/pub"))]
        )
    );
    assert_eq!(
        get_folder(b"/", b"Album").unwrap(),
        req(ClientHdr::FileGetFolder, &[(TAG_FILE_NAME, b"Album")])
    );
}

#[test]
fn folder_put_carries_the_totals_and_clamps_the_size() {
    assert_eq!(
        put_folder(b"/up", b"Tree", 15, 2, false).unwrap(),
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
    assert_eq!(
        put_folder(b"/up", b"Tree", 15, 2, true).unwrap(),
        req(
            ClientHdr::FilePutFolder,
            &[
                (TAG_FILE_NAME, b"Tree"),
                (TAG_DIR, &dir("/up")),
                (TAG_FILE_PREVIEW, &[0, 1]),
                (TAG_HTXF_SIZE, &15u32.to_be_bytes()),
                (TAG_FILE_NFILES, &2u32.to_be_bytes()),
            ]
        )
    );
    let r = put_folder(b"/", b"Big", 5 << 32, 1, false).unwrap();
    assert_eq!(r.chunk(TAG_HTXF_SIZE).unwrap(), u32::MAX.to_be_bytes());
}

#[test]
fn a_name_too_long_for_its_chunk_is_refused() {
    let huge = vec![b'n'; 70_000];
    assert!(get_info(b"/", &huge).is_none());
    assert!(delete(&huge).is_none());
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

#[test]
fn resume_rflt_is_well_formed() {
    let (data_pos, rsrc_pos) = (0x0102_0304u32, 0x0a0b_0c0du32);
    let r = resume_rflt(data_pos, rsrc_pos);

    // Fork-list header: RFLT magic at byte 0, version 1, fork count 2, then
    // the DATA + MACR fork tags at [42] / [58]. The historical bug had the
    // record's C string literal leak 26 leading spaces in, shoving RFLT to
    // [26] and the fork tags past byte 74 — this pins the correct layout.
    assert_eq!(&r[0..4], b"RFLT");
    assert_eq!(&r[4..6], &[0, 1]); // version 1 (u16 BE)
    assert_eq!(&r[40..42], &[0, 2]); // fork count 2 (u16 BE)
    assert_eq!(&r[42..46], b"DATA");
    assert_eq!(&r[58..62], b"MACR");

    // The two resume offsets, big-endian at the fixed positions mhxd reads.
    assert_eq!(&r[46..50], &data_pos.to_be_bytes());
    assert_eq!(&r[62..66], &rsrc_pos.to_be_bytes());

    // Every other byte is zero (no stray content).
    assert!(r[6..40].iter().all(|&b| b == 0));
    assert!(r[50..58].iter().all(|&b| b == 0));
    assert!(r[66..74].iter().all(|&b| b == 0));
}
