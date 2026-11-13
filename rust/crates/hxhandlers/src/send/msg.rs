//! A private message.

use std::ffi::{c_char, CStr};
use std::os::raw::{c_int, c_void};

use hxproto::target::Target;
use hxsession::Expect;

#[cfg(not(test))]
use hxtask::send::hlwrite_chunks;

/// `void hx_send_msg (struct htlc_conn *htlc, guint16 uid, const char *msg)`
/// — send `msg` to `uid` (SEND_MSG). The session expects its reply, which
/// says nothing unless the server refused, and reports that, or names the
/// message where the session has replies or reactions.
///
/// # Safety
/// `htlc` is NULL or a live connection; `msg` a C string or NULL; main
/// thread.
#[no_mangle]
pub unsafe extern "C" fn hx_send_msg(htlc: *mut c_void, uid: u16, msg: *const c_char) {
    send(htlc, uid, msg, None, None);
}

/// As [`hx_send_msg`], answering the message `reply` names where the
/// connection's login takes a reply to it: the trans it went on, whose
/// answer the `msg-answered` signal tells, where it went. A reply the
/// wire cannot carry refuses the message whole, as an instant message's.
///
/// # Safety
/// As [`hx_send_msg`].
pub unsafe fn message(
    htlc: *mut c_void,
    uid: u16,
    msg: &CStr,
    reply: Option<&Target>,
) -> Option<u32> {
    let reply = reply.filter(|t| crate::recv::replies::targets(htlc).is_some_and(|k| k.takes(t)));
    send(htlc, uid, msg.as_ptr(), None, reply)
}

/// As [`message`], carrying the picture an upload gave the handle `id`
/// and type `mime` of. The caller has checked the server agreed to inline
/// media.
///
/// # Safety
/// As [`hx_send_msg`].
pub unsafe fn send_msg_with_media(
    htlc: *mut c_void,
    uid: u16,
    msg: &CStr,
    id: &[u8],
    mime: &[u8],
) -> Option<u32> {
    send(htlc, uid, msg.as_ptr(), Some((id, mime)), None)
}

unsafe fn send(
    htlc: *mut c_void,
    uid: u16,
    msg: *const c_char,
    media: Option<(&[u8], &[u8])>,
    reply: Option<&Target>,
) -> Option<u32> {
    if htlc.is_null() {
        return None;
    }
    super::chat::with_wire(htlc, msg, glib::ffi::GTRUE, |body| {
        // A field the wire can't carry drops the picture, as on a chat line.
        let fits = |f: &[u8]| (1..=u16::MAX as usize).contains(&f.len());
        let media = media.filter(|(id, mime)| fits(id) && fits(mime));
        let req = hxrequest::msg::send(uid, body, media, reply)?;
        let trans = super::expect_next(htlc, Expect::Message);
        crate::recv::msg::sending(htlc, trans);
        req.with_hx_chunks(|c| {
            hlwrite_chunks(htlc.cast(), req.opcode, 0, c.as_ptr(), c.len() as c_int)
        });
        Some(trans)
    })
}

#[cfg(test)]
use tests::hlwrite_chunks;

#[cfg(test)]
mod tests {
    use super::*;
    use hxproto::build::HxChunk;
    use std::cell::RefCell;

    /// A request as written: its opcode, and each field's tag and data.
    type Written = (u32, Vec<(u16, Vec<u8>)>);

    thread_local! {
        static SENT: RefCell<Vec<Written>> = const { RefCell::new(Vec::new()) };
    }

    pub(super) unsafe fn hlwrite_chunks(
        _htlc: *mut c_void,
        ty: u32,
        _flag: u32,
        chunks: *const HxChunk,
        hc: c_int,
    ) {
        let chunks = std::slice::from_raw_parts(chunks, hc as usize)
            .iter()
            .map(|c| {
                let data = std::slice::from_raw_parts(c.data, c.len as usize);
                (c.tag, data.to_vec())
            })
            .collect();
        SENT.with(|s| s.borrow_mut().push((ty, chunks)));
    }

    /// Its answer is the view's: a message is told as answered once its
    /// reply comes.
    #[test]
    fn a_message_goes_to_its_user_with_its_reply_expected() {
        use crate::recv::msg::test_env::{self, Emitted};
        crate::send::expected::take();
        test_env::reset();
        let htlc = std::ptr::dangling_mut();
        let trans = unsafe { message(htlc, 42, c"hi", None) };
        assert_eq!(crate::send::expected::take(), [(1, Expect::Message)]);
        assert_eq!(
            SENT.with(|s| s.take()),
            [(108, vec![(0x67, vec![0, 42]), (0x65, b"hi".to_vec())])]
        );
        assert_eq!(trans, Some(1));
        unsafe { crate::recv::msg::sent(htlc, 1, None) };
        assert_eq!(
            test_env::emitted(),
            [Emitted::Answered {
                htlc: htlc as usize,
                trans: 1,
                pm_id: None
            }]
        );
    }

    /// A message answers a stored one only where the login agreed to
    /// replies and takes a reply to it; anywhere else it goes plain.
    #[test]
    fn a_reply_names_its_message_only_where_the_login_takes_it() {
        use gtkhx_core::conn::{hx_conn_free, hx_conn_new, hx_conn_set_caps};
        use hxproto::replies::Targets;
        use hxsession::cap::REPLIES;
        let to = Target::Pm {
            stored: true,
            reference: [4; 16],
        };
        for (case, caps, targets, carried) in [
            ("a stored message", REPLIES, Some(1 << 5), true),
            ("a kind not taken", REPLIES, Some(1 << 2), false),
            ("no target kinds", REPLIES, None, false),
            ("bit 18 not agreed", 0, Some(1 << 5), false),
        ] {
            SENT.with(|s| s.take());
            let h = hx_conn_new();
            unsafe { hx_conn_set_caps(h, caps) };
            crate::recv::replies::agreed(h.cast(), targets.map(Targets));
            unsafe { message(h.cast(), 42, c"yes", Some(&to)) };
            crate::recv::replies::forget(h.cast());
            unsafe { hx_conn_free(h) };
            let mut want = vec![(0x67, vec![0, 42]), (0x65, b"yes".to_vec())];
            if carried {
                want.push((0x0f21, to.pack().unwrap()));
            }
            assert_eq!(SENT.with(|s| s.take()), [(108, want)], "{case}");
        }
    }

    /// A picture goes after the text; one with a field the wire can't
    /// carry is left behind and the text goes alone.
    #[test]
    fn a_message_carries_its_picture_only_where_the_wire_can() {
        let past = vec![b'x'; usize::from(u16::MAX) + 1];
        let picture = vec![(0x0202, b"h1".to_vec()), (0x0201, b"image/png".to_vec())];
        for (case, id, mime, carried) in [
            ("a picture", &b"h1"[..], &b"image/png"[..], true),
            ("an empty handle", b"", b"image/png", false),
            ("an empty type", b"h1", b"", false),
            ("a handle past a field", &past, b"image/png", false),
            ("a type past a field", b"h1", &past, false),
        ] {
            crate::send::expected::take();
            SENT.with(|s| s.take());
            let trans =
                unsafe { send_msg_with_media(std::ptr::dangling_mut(), 42, c"[image]", id, mime) };
            assert_eq!(trans, Some(1), "{case}");
            assert_eq!(
                crate::send::expected::take(),
                [(1, Expect::Message)],
                "{case}"
            );
            let mut want = vec![(0x67, vec![0, 42]), (0x65, b"[image]".to_vec())];
            if carried {
                want.extend(picture.clone());
            }
            assert_eq!(SENT.with(|s| s.take()), [(108, want)], "{case}");
        }
    }
}
