//! Headless signal-behaviour tests for the chat receive handlers, driven
//! through the `test_env` recording doubles for the member-model / emit C ABIs.

use super::*;
use std::ffi::CString;

fn invite(cid: u32, uid: u16, name: &str) {
    let cname = CString::new(name).unwrap();
    // htlc / member_model are opaque and unused by the doubles.
    unsafe {
        hx_chat_invite_recv(
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            cid,
            uid,
            cname.as_ptr(),
        );
    }
}

fn push_chunk(v: &mut Vec<u8>, tag: u16, data: &[u8]) {
    v.extend_from_slice(&tag.to_be_bytes());
    v.extend_from_slice(&(data.len() as u16).to_be_bytes());
    v.extend_from_slice(data);
}

/// Build a real HTLS_HDR_CHAT_INVITE frame: 22-byte header + UID/CHAT_ID/NAME
/// chunks. The handler runs the production `hotline_proto::parse::parse_chat_invite`
/// over these bytes — no parse double.
fn invite_frame(uid: u16, cid: u32, name: &[u8]) -> Vec<u8> {
    use hotline_proto::messages::tag;
    let mut v = Vec::new();
    v.extend_from_slice(&0x0000_0071u32.to_be_bytes()); // type = CHAT_INVITE
    v.extend_from_slice(&[0u8; 18]); // trans(4) flag(4) len(4) len2(4) hc(2)
    push_chunk(&mut v, tag::UID, &uid.to_be_bytes());
    push_chunk(&mut v, tag::CHAT_ID, &cid.to_be_bytes());
    push_chunk(&mut v, tag::NAME, name);
    v
}

/// Drive the whole moved handler (native parse → chat lookup → recv) over `frame`.
fn rcv_invite(frame: &[u8]) {
    unsafe { hx_rcv_chat_invite(std::ptr::null_mut(), frame.as_ptr(), frame.len()) };
}

#[test]
fn rcv_handler_parses_and_emits() {
    test_env::reset();
    let f = invite_frame(5, 9, b"Alice");

    rcv_invite(&f);

    // native parse → lookups → hx_chat_invite_recv → chat-invitation emit.
    assert_eq!(test_env::EMITTED.with(|c| c.take()), Some((9, b"Alice".to_vec())));
}

#[test]
fn rcv_handler_honours_ignore() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(true));
    let f = invite_frame(2, 1, b"Blocked");

    rcv_invite(&f);

    assert_eq!(test_env::EMITTED.with(|c| c.take()), None);
}

#[test]
fn rcv_handler_header_only_emits_zeroed() {
    // A chunk-less frame parses to cid 0 + empty name and still emits — matching
    // the old C, whose extractor never failed on malformed data.
    test_env::reset();
    let mut f = Vec::new();
    f.extend_from_slice(&0x0000_0071u32.to_be_bytes());
    f.extend_from_slice(&[0u8; 18]);

    rcv_invite(&f);

    assert_eq!(test_env::EMITTED.with(|c| c.take()), Some((0, Vec::new())));
}

/// Build a real HTLS_HDR_CHAT_SUBJECT frame: 22-byte header + CHAT_ID/CHAT_SUBJECT.
fn subject_frame(cid: u32, subject: &[u8]) -> Vec<u8> {
    use hotline_proto::messages::tag;
    let mut v = Vec::new();
    v.extend_from_slice(&0x0000_0077u32.to_be_bytes()); // type = CHAT_SUBJECT
    v.extend_from_slice(&[0u8; 18]);
    push_chunk(&mut v, tag::CHAT_ID, &cid.to_be_bytes());
    push_chunk(&mut v, tag::CHAT_SUBJECT, subject);
    v
}

fn rcv_subject(frame: &[u8]) {
    unsafe { hx_rcv_chat_subject(std::ptr::null_mut(), frame.as_ptr(), frame.len()) };
}

#[test]
fn rcv_subject_handler_parses_and_emits() {
    // Native parse → lookup → change-gate (current subject is "" in the double,
    // so any non-empty subject is a change) → chat-subject emit.
    test_env::reset();
    let f = subject_frame(4, b"New Topic");

    rcv_subject(&f);

    assert_eq!(
        test_env::SUBJECT_EMITTED.with(|c| c.take()),
        Some((4, b"New Topic".to_vec()))
    );
}

#[test]
fn rcv_subject_handler_empty_noops() {
    // An empty subject is dropped before any lookup/emit (matches the old C).
    test_env::reset();
    let f = subject_frame(4, b"");

    rcv_subject(&f);

    assert_eq!(test_env::SUBJECT_EMITTED.with(|c| c.take()), None);
}

/// Build a real HTLS_HDR_CHAT frame: 22-byte header + BODY/CHAT_ID/UID (+ any
/// extra chunks, e.g. media companions). The header type is ignored by
/// parse_chat — it walks chunks.
fn chat_frame(cid: u32, uid: u16, body: &[u8], extra: &[(u16, Vec<u8>)]) -> Vec<u8> {
    use hotline_proto::messages::tag;
    let mut v = Vec::new();
    v.extend_from_slice(&0x0000_0069u32.to_be_bytes()); // type (ignored)
    v.extend_from_slice(&[0u8; 18]);
    push_chunk(&mut v, tag::BODY, body); // HTLS_DATA_CHAT shares the BODY tag
    push_chunk(&mut v, tag::CHAT_ID, &cid.to_be_bytes());
    push_chunk(&mut v, tag::UID, &uid.to_be_bytes());
    for (t, d) in extra {
        push_chunk(&mut v, *t, d);
    }
    v
}

fn rcv_chat(frame: &[u8]) {
    unsafe { hx_rcv_chat(std::ptr::null_mut(), frame.as_ptr(), frame.len()) };
}

#[test]
fn rcv_chat_handler_parses_and_emits() {
    test_env::reset();
    let f = chat_frame(0, 42, b"hello world", &[]);

    rcv_chat(&f);

    // native parse → event build → gate → chat emit (the sentinel event ptr).
    assert_eq!(
        test_env::EVENT_NEW.with(|c| c.borrow().clone()),
        Some((0, b"hello world".to_vec()))
    );
    assert_eq!(test_env::CHAT_EMITTED.with(|c| c.take()), Some(FAKE_CHAT_EVENT));
    assert!(!test_env::MEDIA_ATTACHED.with(|c| c.get()));
}

#[test]
fn rcv_chat_handler_honours_ignore() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(true));
    let f = chat_frame(0, 42, b"blocked", &[]);

    rcv_chat(&f);

    // ignored sender → hx_chat_recv drops it (the event was still built + freed).
    assert_eq!(test_env::CHAT_EMITTED.with(|c| c.take()), None);
}

#[test]
fn rcv_chat_handler_attaches_media_when_cap_set() {
    use hotline_proto::messages::tag;
    test_env::reset();
    test_env::HAS_CAP.with(|c| c.set(true));
    let extra = vec![
        (tag::CHAT_MEDIA_ID, b"handle123".to_vec()),
        (tag::CHAT_MEDIA_TYPE, b"image/png".to_vec()),
    ];
    let f = chat_frame(0, 42, b"look", &extra);

    rcv_chat(&f);

    assert!(test_env::MEDIA_ATTACHED.with(|c| c.get()));
    assert_eq!(test_env::CHAT_EMITTED.with(|c| c.take()), Some(FAKE_CHAT_EVENT));
}

#[test]
fn rcv_chat_handler_drops_orphaned_media() {
    use hotline_proto::messages::tag;
    test_env::reset();
    test_env::HAS_CAP.with(|c| c.set(true));
    // Only the ID present (no TYPE) → orphan → drop the whole chat.
    let extra = vec![(tag::CHAT_MEDIA_ID, b"handle123".to_vec())];
    let f = chat_frame(0, 42, b"look", &extra);

    rcv_chat(&f);

    assert_eq!(test_env::CHAT_EMITTED.with(|c| c.take()), None);
    assert!(!test_env::MEDIA_ATTACHED.with(|c| c.get()));
}

#[test]
fn rcv_chat_handler_ignores_media_without_cap() {
    use hotline_proto::messages::tag;
    // Media chunks present but cap NOT negotiated → media ignored, line still emits.
    test_env::reset();
    let extra = vec![
        (tag::CHAT_MEDIA_ID, b"handle123".to_vec()),
        (tag::CHAT_MEDIA_TYPE, b"image/png".to_vec()),
    ];
    let f = chat_frame(0, 42, b"look", &extra);

    rcv_chat(&f);

    assert!(!test_env::MEDIA_ATTACHED.with(|c| c.get()));
    assert_eq!(test_env::CHAT_EMITTED.with(|c| c.take()), Some(FAKE_CHAT_EVENT));
}

#[test]
fn emits_chat_invitation_when_not_ignored() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(false));

    invite(7, 42, "Inviter");

    let got = test_env::EMITTED.with(|c| c.take());
    assert_eq!(got, Some((7, b"Inviter".to_vec())));
}

#[test]
fn drops_invitation_from_ignored_user() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(true));

    invite(7, 42, "Blocked");

    // Ignored inviter → no chat-invitation signal.
    assert_eq!(test_env::EMITTED.with(|c| c.take()), None);
}

/// Returns the applied flag; records the emit (if any) in SUBJECT_EMITTED.
fn subject(cid: u32, new: &str, current: &str) -> bool {
    let cnew = CString::new(new).unwrap();
    let ccur = CString::new(current).unwrap();
    let applied = unsafe {
        hx_chat_subject_recv(
            std::ptr::null_mut(),
            cid,
            cnew.as_ptr(),
            new.len(),
            ccur.as_ptr(),
        )
    };
    applied != 0
}

#[test]
fn emits_chat_subject_when_changed() {
    test_env::reset();
    assert!(subject(3, "New Topic", "Old Topic"));
    assert_eq!(
        test_env::SUBJECT_EMITTED.with(|c| c.take()),
        Some((3, b"New Topic".to_vec()))
    );
}

#[test]
fn suppresses_unchanged_subject() {
    test_env::reset();
    assert!(!subject(3, "Same", "Same"));
    assert_eq!(test_env::SUBJECT_EMITTED.with(|c| c.take()), None);
}

#[test]
fn suppresses_empty_subject() {
    test_env::reset();
    // subject_len 0 → no announcement, even against a non-empty current.
    let ccur = CString::new("Existing").unwrap();
    let applied =
        unsafe { hx_chat_subject_recv(std::ptr::null_mut(), 3, c"".as_ptr(), 0, ccur.as_ptr()) };
    assert_eq!(applied, 0);
    assert_eq!(test_env::SUBJECT_EMITTED.with(|c| c.take()), None);
}

/// A sentinel boxed-event pointer (never dereferenced by the crate).
fn fake_event() -> *mut std::os::raw::c_void {
    0xC0FE_usize as *mut std::os::raw::c_void
}

fn chat(uid: u16) -> c_int {
    unsafe { hx_chat_recv(std::ptr::null_mut(), std::ptr::null_mut(), uid, fake_event()) }
}

#[test]
fn emits_chat_when_not_ignored() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(false));
    assert_eq!(chat(42), 1);
    assert_eq!(test_env::CHAT_EMITTED.with(|c| c.take()), Some(fake_event()));
}

#[test]
fn drops_chat_from_ignored_user() {
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(true));
    assert_eq!(chat(42), 0);
    assert_eq!(test_env::CHAT_EMITTED.with(|c| c.take()), None);
}

#[test]
fn system_line_uid_zero_always_emits() {
    // uid 0 is a server/system line — the ignore list is never consulted.
    test_env::reset();
    test_env::IGNORE.with(|c| c.set(true));
    assert_eq!(chat(0), 1);
    assert_eq!(test_env::CHAT_EMITTED.with(|c| c.take()), Some(fake_event()));
}

#[test]
fn subject_discovery_emits_unconditionally() {
    // The room-load discovery path has no change-gate: always publish.
    test_env::reset();
    let subj = CString::new("Welcome").unwrap();
    unsafe { hx_chat_subject_emit(std::ptr::null_mut(), 5, subj.as_ptr()) };
    assert_eq!(
        test_env::SUBJECT_EMITTED.with(|c| c.take()),
        Some((5, b"Welcome".to_vec()))
    );
}

#[test]
fn chat_history_batch_forwards_array_and_flag() {
    test_env::reset();
    let entries = 0xE117_usize as *mut std::os::raw::c_void;
    unsafe { hx_chat_history_recv(std::ptr::null_mut(), 0, entries, 1) };
    assert_eq!(
        test_env::HISTORY_EMITTED.with(|c| c.take()),
        Some((0, entries, true))
    );
}
