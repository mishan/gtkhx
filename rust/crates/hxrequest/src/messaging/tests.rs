//! The friend family's requests, pinned against the extension's wire bytes.

use super::*;

#[test]
fn each_request_goes_as_the_extension_has_it() {
    let req = |opcode: u32, chunks: &[(u16, &[u8])]| Request {
        opcode,
        chunks: chunks.iter().map(|(t, d)| (*t, d.to_vec())).collect(),
    };
    let guid = [7u8; GUID_LEN];
    for (got, want) in [
        (roster(), req(800, &[])),
        (
            add_friend(b"bob", Some(b"hi"), true).unwrap(),
            req(802, &[(0x0600, b"bob"), (0x0610, b"hi")]),
        ),
        (
            add_friend(b"bob", None, true).unwrap(),
            req(802, &[(0x0600, b"bob")]),
        ),
        (
            remove_friend(b"bob").unwrap(),
            req(803, &[(0x0600, b"bob")]),
        ),
        (
            respond(b"bob", true).unwrap(),
            req(805, &[(0x0600, b"bob"), (0x0604, &[0, 3])]),
        ),
        // Declining sends no state.
        (
            respond(b"bob", false).unwrap(),
            req(805, &[(0x0600, b"bob")]),
        ),
        (block(b"bob").unwrap(), req(806, &[(0x0600, b"bob")])),
        (unblock(b"bob").unwrap(), req(807, &[(0x0600, b"bob")])),
        (
            set_presence(Some(PresenceState::Away), Some(b"out"), None, true).unwrap(),
            req(808, &[(0x0602, &[0, 2]), (0x0603, b"out")]),
        ),
        // No status leaves the stored one as it is.
        (
            set_presence(Some(PresenceState::Online), None, None, true).unwrap(),
            req(808, &[(0x0602, &[0, 1])]),
        ),
        (
            set_presence(Some(PresenceState::Online), None, Some(false), true).unwrap(),
            req(808, &[(0x0602, &[0, 1]), (0x0650, &[0, 0])]),
        ),
        // No state leaves our presence as it is.
        (
            set_presence(None, None, Some(true), true).unwrap(),
            req(808, &[(0x0650, &[0, 1])]),
        ),
        (
            im_send(b"bob", &guid, b"a\nb", true).unwrap(),
            req(810, &[(0x0600, b"bob"), (0x0605, &guid), (0x0606, b"a\nb")]),
        ),
        (
            im_ack(&guid, AckType::Read, b"bob").unwrap(),
            req(812, &[(0x0605, &guid), (0x0608, &[0, 2]), (0x0600, b"bob")]),
        ),
        (
            im_typing(b"bob", true).unwrap(),
            req(813, &[(0x0600, b"bob"), (0x0609, &[0, 1])]),
        ),
        (find_user(b"bob").unwrap(), req(822, &[(0x0600, b"bob")])),
        (
            set_nickname(b"bob", b"", true).unwrap(),
            req(824, &[(0x0600, b"bob"), (0x0601, b"")]),
        ),
        (profile(b"bob").unwrap(), req(825, &[(0x0600, b"bob")])),
        // The first page carries no cursor.
        (message_requests(None).unwrap(), req(829, &[])),
        (
            message_requests(Some(&[0, 0, 0, 3])).unwrap(),
            req(829, &[(0x0651, &[0, 0, 0, 3])]),
        ),
        (
            answer_request(b"bob", RequestAction::EndApproval).unwrap(),
            req(830, &[(0x0600, b"bob"), (0x0652, &[0, 3])]),
        ),
        // Neither end clears everything.
        (clear_requests(None, None), req(831, &[])),
        (
            clear_requests(Some(0x0102), None),
            req(831, &[(0x0655, &[0, 0, 0, 0, 0, 0, 1, 2])]),
        ),
        (
            clear_requests(Some(1), Some(2)),
            req(
                831,
                &[
                    (0x0655, &[0, 0, 0, 0, 0, 0, 0, 1]),
                    (0x0656, &[0, 0, 0, 0, 0, 0, 0, 2]),
                ],
            ),
        ),
    ] {
        assert_eq!(got, want);
    }
}

#[test]
fn text_goes_in_the_connections_encoding() {
    // Mac Roman, with a body's line ending; UTF-8 as typed.
    let sent = im_send(b"bob", &[0; GUID_LEN], "caf\u{e9}\n".as_bytes(), false).unwrap();
    assert_eq!(sent.chunk(0x0606), Some(&b"caf\x8e\r"[..]));
    let sent = im_send(b"bob", &[0; GUID_LEN], "caf\u{e9}\n".as_bytes(), true).unwrap();
    assert_eq!(sent.chunk(0x0606), Some("caf\u{e9}\n".as_bytes()));
    assert_eq!(login("j\u{f6}rg".as_bytes(), false), b"j\x9arg");
}

#[test]
fn what_the_wire_cannot_carry_is_refused() {
    assert_eq!(add_friend(b"", None, true), None);
    assert_eq!(
        im_send(b"bob", &[0; GUID_LEN], &vec![b'x'; 65536], true),
        None
    );
    assert_eq!(
        set_presence(Some(PresenceState::Offline), None, None, true),
        None
    );
    assert_eq!(message_requests(Some(b"")), None);
    assert_eq!(answer_request(b"", RequestAction::Approve), None);
}
