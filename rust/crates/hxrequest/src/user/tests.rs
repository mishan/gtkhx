//! The user requests, pinned against hand-written wire bytes.

use super::*;

const TAG_BODY: u16 = 0x0065;
const TAG_LOGIN: u16 = 0x0069;
const TAG_PASSWORD: u16 = 0x006a;
const TAG_UID: u16 = 0x0067;
const TAG_BAN: u16 = 0x0071;
const TAG_NAME: u16 = 0x0066;
const TAG_ACCESS: u16 = 0x006e;

fn req(opcode: u32, chunks: &[(u16, &[u8])]) -> Request {
    Request {
        opcode,
        chunks: chunks.iter().map(|(t, d)| (*t, d.to_vec())).collect(),
    }
}

#[test]
fn each_request_goes_as_gtkhx_has_always_sent_it() {
    let access = [0x60, 0x60, 0x0c, 0, 0, 0, 0, 1];
    // "bob", every byte inverted.
    let bob = [0x9d, 0x90, 0x9d];
    let cases = [
        (info(0x1234).unwrap(), req(303, &[(TAG_UID, &[0x12, 0x34])])),
        (kick(5, false).unwrap(), req(110, &[(TAG_UID, &[0, 5])])),
        (
            kick(5, true).unwrap(),
            req(110, &[(TAG_BAN, &[0, 1]), (TAG_UID, &[0, 5])]),
        ),
        (
            broadcast("one\ncafé".as_bytes(), false).unwrap(),
            req(355, &[(TAG_BODY, b"one\rcaf\x8e")]),
        ),
        (
            broadcast("one\ncafé".as_bytes(), true).unwrap(),
            req(355, &[(TAG_BODY, "one\ncafé".as_bytes())]),
        ),
        (
            account_read(b"bob").unwrap(),
            req(352, &[(TAG_LOGIN, b"bob")]),
        ),
        (
            account_save(b"bob", b"", "Bé".as_bytes(), access).unwrap(),
            req(
                353,
                &[
                    (TAG_LOGIN, &bob),
                    (TAG_PASSWORD, &[0]),
                    (TAG_NAME, "Bé".as_bytes()),
                    (TAG_ACCESS, &access),
                ],
            ),
        ),
        (
            account_create(b"bob", b"pw", b"Bob", [0; 8]).unwrap(),
            req(
                350,
                &[
                    (TAG_LOGIN, &bob),
                    (TAG_PASSWORD, &[0x8f, 0x88]),
                    (TAG_NAME, b"Bob"),
                    (TAG_ACCESS, &[0; 8]),
                ],
            ),
        ),
        (
            account_delete(b"bob").unwrap(),
            req(351, &[(TAG_LOGIN, &bob)]),
        ),
    ];
    for (got, want) in cases {
        assert_eq!(got, want);
    }
}
