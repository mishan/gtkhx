//! The inline-media requests, pinned against hand-written wire bytes.

use super::*;

const PAYLOAD: u16 = 0x0203;
const DECLARED: u16 = 0x0204;
const ID: u16 = 0x0202;
const TOKEN: u16 = 0x0208;
const INDEX: u16 = 0x0209;
const COUNT: u16 = 0x020a;
const FINAL: u16 = 0x020b;

#[test]
fn each_request_goes_as_gtkhx_has_always_sent_it() {
    let req = |opcode: u32, chunks: &[(u16, &[u8])]| Request {
        opcode,
        chunks: chunks.iter().map(|(t, d)| (*t, d.to_vec())).collect(),
    };
    for (got, want) in [
        (
            upload(b"png", Some(b"image/png")).unwrap(),
            req(
                750,
                &[(PAYLOAD, b"png"), (DECLARED, b"image/png"), (FINAL, &[1])],
            ),
        ),
        // No hint, or an empty one, leaves the field out.
        (
            upload(b"png", Some(b"")).unwrap(),
            req(750, &[(PAYLOAD, b"png"), (FINAL, &[1])]),
        ),
        (
            upload_first(b"pn", Some(b"image/png"), 3).unwrap(),
            req(
                750,
                &[
                    (PAYLOAD, b"pn"),
                    (DECLARED, b"image/png"),
                    (INDEX, &[0, 0]),
                    (COUNT, &[0, 3]),
                    (FINAL, &[0]),
                ],
            ),
        ),
        (
            upload_next(b"tok", b"g", 1, false).unwrap(),
            req(
                750,
                &[
                    (TOKEN, b"tok"),
                    (INDEX, &[0, 1]),
                    (PAYLOAD, b"g"),
                    (FINAL, &[0]),
                ],
            ),
        ),
        (
            upload_next(b"tok", b"!", 2, true).unwrap(),
            req(
                750,
                &[
                    (TOKEN, b"tok"),
                    (INDEX, &[0, 2]),
                    (PAYLOAD, b"!"),
                    (FINAL, &[1]),
                ],
            ),
        ),
        (download(b"id", None).unwrap(), req(751, &[(ID, b"id")])),
        (
            download(b"id", Some(2)).unwrap(),
            req(751, &[(ID, b"id"), (INDEX, &[0, 2])]),
        ),
    ] {
        assert_eq!(got, want);
    }
}

#[test]
fn what_the_wire_cannot_carry_is_refused() {
    let big = vec![0; 65536];
    for refused in [
        upload(b"", None),
        upload(&big, None),
        upload_first(b"pn", None, 1),
        upload_next(b"", b"g", 1, true),
        upload_next(b"tok", b"g", 0, true),
        download(b"", None),
    ] {
        assert_eq!(refused, None);
    }
}
