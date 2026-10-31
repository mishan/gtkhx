//! What each sender puts on the wire, and that none goes where the server
//! did not confirm the friend family. The connection is a real one; the
//! trans and the write primitive are stubbed, recording what was written.

use std::cell::{Cell, RefCell};

use gtkhx_core::conn::{hx_conn_free, hx_conn_new, hx_conn_set_caps};
use hxtask::HtlcConn;

use super::*;

/// A request as written: its opcode, trans, and each field's tag and data.
pub(crate) type Written = (u32, u32, Vec<(u16, Vec<u8>)>);

thread_local! {
    static TRANS: Cell<u32> = const { Cell::new(0) };
    static SENT: RefCell<Vec<Written>> = const { RefCell::new(Vec::new()) };
    static WOKEN: RefCell<Vec<Instant>> = const { RefCell::new(Vec::new()) };
}

pub(crate) unsafe fn wake(_htlc: *mut c_void, at: Instant) {
    WOKEN.with(|w| w.borrow_mut().push(at));
}

/// When the timers set since last asked are to fire.
pub(crate) fn woken() -> Vec<Instant> {
    WOKEN.with(|w| w.take())
}

pub(crate) unsafe fn next_trans(_h: *mut HtlcConn) -> u32 {
    TRANS.with(|t| t.get() + 1)
}

pub(crate) unsafe fn hlwrite_chunks(
    _h: *mut HtlcConn,
    ty: u32,
    _flag: u32,
    chunks: *const hxproto::build::HxChunk,
    hc: c_int,
) {
    let trans = TRANS.with(|t| {
        t.set(t.get() + 1);
        t.get()
    });
    let fields = std::slice::from_raw_parts(chunks, hc as usize)
        .iter()
        .map(|c| {
            let data = if c.len == 0 {
                Vec::new()
            } else {
                std::slice::from_raw_parts(c.data, c.len as usize).to_vec()
            };
            (c.tag, data)
        })
        .collect();
    SENT.with(|s| s.borrow_mut().push((ty, trans, fields)));
}

/// What was written since last asked.
pub(crate) fn sent() -> Vec<Written> {
    SENT.with(|s| s.take())
}

/// A connection agreed to `caps`, freed on drop.
pub(crate) struct Conn(pub *mut c_void);

impl Conn {
    pub(crate) fn with_caps(caps: u64) -> Conn {
        let h = hx_conn_new();
        unsafe { hx_conn_set_caps(h, caps) };
        Conn(h.cast())
    }

    /// One whose server serves message requests.
    pub(crate) fn serving_requests(caps: u64) -> Conn {
        let c = Conn::with_caps(caps | CAP_MESSAGING);
        with_roster_mut(c.0, |m| m.requests.served = true);
        c
    }
}

impl Drop for Conn {
    fn drop(&mut self) {
        unsafe {
            crate::recv::friends::forget(self.0);
            hx_conn_free(self.0.cast());
        }
    }
}

#[test]
fn nothing_goes_where_the_server_did_not_confirm_bit_6() {
    sent();
    let c = Conn::with_caps(cap::TEXT_ENCODING);
    unsafe {
        assert_eq!(roster(c.0), None);
        assert_eq!(im(c.0, b"bob", "hi"), None);
        assert_eq!(typing(c.0, b"bob", true), None);
    }
    assert_eq!(sent(), []);
}

/// Bit 6 alone, or a bound beside no bit 6, is no server that serves
/// message requests.
#[test]
fn no_request_of_the_draft_goes_where_it_is_not_served() {
    sent();
    let unbound = Conn::with_caps(CAP_MESSAGING);
    let no_bit = Conn::serving_requests(0);
    unsafe { hx_conn_set_caps(no_bit.0.cast(), 0) };
    for c in [&unbound, &no_bit] {
        unsafe {
            requests(c.0);
            assert_eq!(answer_request(c.0, b"bob", RequestAction::Approve), None);
            assert_eq!(clear_requests(c.0, None, None), None);
            assert_eq!(set_takes_requests(c.0, true, None), None);
        }
    }
    assert_eq!(sent(), []);
}

#[test]
fn our_setting_goes_with_the_presence_the_caller_gives_or_none() {
    let c = Conn::serving_requests(0);
    let cases = [
        (
            Some(PresenceState::Away),
            vec![(0x0602, vec![0, 2]), (0x0650, vec![0, 1])],
        ),
        (None, vec![(0x0650, vec![0, 1])]),
    ];
    for (state, want) in cases {
        sent();
        unsafe { set_takes_requests(c.0, true, state).unwrap() };
        let (opcode, _, fields) = sent().remove(0);
        assert_eq!((opcode, fields), (808, want), "{state:?}");
    }
}

#[test]
fn a_message_goes_with_a_fresh_id_its_receipts_will_name() {
    sent();
    let c = Conn::with_caps(CAP_MESSAGING | cap::TEXT_ENCODING);
    let (a, b) = unsafe {
        (
            im(c.0, b"bob", "one").unwrap(),
            im(c.0, b"bob", "two").unwrap(),
        )
    };
    assert_ne!(a, b);
    // A version 4 UUID, as other clients make them.
    assert_eq!((a[6] >> 4, a[8] >> 6), (4, 2));
    let written = sent();
    assert_eq!(written.len(), 2);
    let (opcode, _, fields) = &written[0];
    assert_eq!(*opcode, 810);
    assert!(fields.contains(&(0x0605, a.to_vec())));
    assert!(fields.contains(&(0x0606, b"one".to_vec())));
    let held = gtkhx_core::messaging::with_roster(c.0, |r| r.delivery(&a));
    assert_eq!(held, Some(gtkhx_core::messaging::Delivery::Sending));
}

#[test]
fn text_goes_in_the_connections_encoding() {
    sent();
    let classic = Conn::with_caps(CAP_MESSAGING);
    unsafe {
        set_presence(classic.0, PresenceState::Away, Some("caf\u{e9}"));
        assert_eq!(login(classic.0, "j\u{f6}rg"), b"j\x9arg");
    }
    let (opcode, _, fields) = sent().remove(0);
    assert_eq!(
        (opcode, fields),
        (
            808,
            vec![(0x0602, vec![0, 2]), (0x0603, b"caf\x8e".to_vec())]
        )
    );
}
