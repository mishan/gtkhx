//! A private message.

use std::ffi::{c_char, CStr};
use std::os::raw::{c_int, c_void};

use hxproto::build::{build_msg_chunks, HxChunk, MsgRequest};
use hxproto::messages::{tag, ClientHdr};
use hxsession::Expect;

#[cfg(not(test))]
use hxtask::send::hlwrite_chunks;

/// `void hx_send_msg (struct htlc_conn *htlc, guint16 uid, const char *msg)`
/// — send `msg` to `uid` (SEND_MSG). The session expects its reply, which
/// says nothing unless the server refused, and reports that.
///
/// # Safety
/// `htlc` is NULL or a live connection; `msg` a C string or NULL; main
/// thread.
#[no_mangle]
pub unsafe extern "C" fn hx_send_msg(htlc: *mut c_void, uid: u16, msg: *const c_char) {
    send(htlc, uid, msg, None);
}

/// As [`hx_send_msg`], carrying the picture an upload gave the handle `id`
/// and type `mime` of. The caller has checked the server agreed to inline
/// media.
///
/// # Safety
/// As [`hx_send_msg`].
pub unsafe fn send_msg_with_media(htlc: *mut c_void, uid: u16, msg: &CStr, id: &[u8], mime: &[u8]) {
    send(htlc, uid, msg.as_ptr(), Some((id, mime)));
}

unsafe fn send(htlc: *mut c_void, uid: u16, msg: *const c_char, media: Option<(&[u8], &[u8])>) {
    if htlc.is_null() {
        return;
    }
    super::chat::with_wire(htlc, msg, glib::ffi::GTRUE, |body| {
        let mut chunks = [HxChunk::EMPTY; 4];
        let mut scratch = [0u8; 2];
        let mut hc = build_msg_chunks(&MsgRequest { uid, body }, &mut chunks, &mut scratch);
        if hc == 0 {
            return;
        }
        // A field the wire can't carry drops the picture, as on a chat line.
        let fits = |f: &[u8]| (1..=u16::MAX as usize).contains(&f.len());
        if let Some((id, mime)) = media.filter(|(id, mime)| fits(id) && fits(mime)) {
            for (tag, data) in [(tag::CHAT_MEDIA_ID, id), (tag::CHAT_MEDIA_TYPE, mime)] {
                chunks[hc] = HxChunk {
                    tag,
                    len: data.len() as u16,
                    data: data.as_ptr(),
                };
                hc += 1;
            }
        }
        super::expect_next(htlc, Expect::Message);
        hlwrite_chunks(
            htlc.cast(),
            ClientHdr::Msg as u32,
            0,
            chunks.as_ptr(),
            hc as c_int,
        );
    });
}

#[cfg(test)]
use tests::hlwrite_chunks;

#[cfg(test)]
mod tests {
    use super::*;
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

    #[test]
    fn a_message_goes_to_its_user_with_its_reply_expected() {
        crate::send::expected::take();
        unsafe { hx_send_msg(std::ptr::dangling_mut(), 42, c"hi".as_ptr()) };
        assert_eq!(crate::send::expected::take(), [(1, Expect::Message)]);
        assert_eq!(
            SENT.with(|s| s.take()),
            [(108, vec![(0x67, vec![0, 42]), (0x65, b"hi".to_vec())])]
        );
    }

    #[test]
    fn a_message_carries_its_picture_after_its_text() {
        crate::send::expected::take();
        unsafe {
            send_msg_with_media(
                std::ptr::dangling_mut(),
                42,
                c"[image]",
                b"h1",
                b"image/png",
            )
        };
        assert_eq!(crate::send::expected::take(), [(1, Expect::Message)]);
        assert_eq!(
            SENT.with(|s| s.take()),
            [(
                108,
                vec![
                    (0x67, vec![0, 42]),
                    (0x65, b"[image]".to_vec()),
                    (0x0202, b"h1".to_vec()),
                    (0x0201, b"image/png".to_vec()),
                ]
            )]
        );
    }
}
