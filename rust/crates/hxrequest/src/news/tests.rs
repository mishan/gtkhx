//! The news requests, pinned against hand-written wire bytes.

use super::*;

const TAG_BODY: u16 = 0x0065;
const TAG_FILE_NAME: u16 = 0x00c9;
const TAG_CATEGORY: u16 = 0x0142;
const TAG_NEWSPATH: u16 = 0x0145;
const TAG_THREADID: u16 = 0x0146;
const TAG_NEWSTYPE: u16 = 0x0147;
const TAG_NEWSSUBJECT: u16 = 0x0148;
const TAG_NEWSDATA: u16 = 0x014d;
const TAG_NEWSFLAGS: u16 = 0x014e;

fn req(opcode: ClientHdr, chunks: &[(u16, &[u8])]) -> Request {
    Request {
        opcode: opcode as u32,
        chunks: chunks.iter().map(|(t, d)| (*t, d.to_vec())).collect(),
    }
}

/// "/Café", as a Mac Roman listing names it.
const CAFE: &[u8] = b"/Caf\x8e";
const CAFE_DIR: &[u8] = b"\0\x01\0\0\x04Caf\x8e";

#[test]
fn each_request_goes_as_gtkhx_has_always_sent_it() {
    let cases = [
        (file(), req(ClientHdr::NewsGetFile, &[])),
        (
            post(b"one\ntwo", false).unwrap(),
            req(ClientHdr::NewsPost, &[(TAG_BODY, b"one\rtwo")]),
        ),
        (
            listing(b"/").unwrap(),
            req(ClientHdr::NewsListDir, &[(TAG_NEWSPATH, &[0, 0])]),
        ),
        (
            category(CAFE).unwrap(),
            req(ClientHdr::NewsListCategory, &[(TAG_NEWSPATH, CAFE_DIR)]),
        ),
        (
            article(CAFE, 0x0102_0304, b"").unwrap(),
            req(
                ClientHdr::GetThread,
                &[
                    (TAG_NEWSPATH, CAFE_DIR),
                    (TAG_THREADID, &[1, 2, 3, 4]),
                    (TAG_NEWSTYPE, b"text/plain"),
                ],
            ),
        ),
        (
            post_article(CAFE, 0x2a, "Café".as_bytes(), b"a\nb", false).unwrap(),
            req(
                ClientHdr::PostThread,
                &[
                    (TAG_NEWSPATH, CAFE_DIR),
                    (TAG_NEWSFLAGS, &[0, 0, 0, 0]),
                    (TAG_NEWSTYPE, b"text/plain"),
                    (TAG_NEWSSUBJECT, b"Caf\x8e"),
                    (TAG_NEWSDATA, b"a\rb"),
                    (TAG_THREADID, &[0, 0, 0, 0x2a]),
                ],
            ),
        ),
        (
            delete_article(CAFE, 7).unwrap(),
            req(
                ClientHdr::DeleteThread,
                &[(TAG_NEWSPATH, CAFE_DIR), (TAG_THREADID, &[0, 0, 0, 7])],
            ),
        ),
        (
            delete(CAFE).unwrap(),
            req(ClientHdr::NewsDelete, &[(TAG_NEWSPATH, CAFE_DIR)]),
        ),
        (
            create_category(CAFE, "Thé".as_bytes(), true).unwrap(),
            req(
                ClientHdr::NewsMkCategory,
                &[(TAG_NEWSPATH, CAFE_DIR), (TAG_CATEGORY, "Thé".as_bytes())],
            ),
        ),
        // The new bundle is a name of its own, never the path's last part.
        (
            create_bundle(CAFE, b"new", false).unwrap(),
            req(
                ClientHdr::NewsMkdir,
                &[(TAG_NEWSPATH, CAFE_DIR), (TAG_FILE_NAME, b"new")],
            ),
        ),
    ];
    for (got, want) in cases {
        assert_eq!(got, want);
    }
}

#[test]
fn an_article_is_asked_for_as_its_listing_typed_it() {
    let got = article(b"/c", 1, b"text/html").unwrap();
    assert_eq!(got.chunk(TAG_NEWSTYPE), Some(&b"text/html"[..]));
}

#[test]
fn what_a_field_cannot_hold_is_refused() {
    let long = vec![b'x'; u16::MAX as usize + 1];
    assert_eq!(post(&long, true), None);
    assert_eq!(post_article(b"/c", 0, b"s", &long, true), None);
}
