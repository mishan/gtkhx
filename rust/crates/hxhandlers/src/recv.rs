//! Receive-side handlers — the `rcv_task_*` / notification bodies that used to
//! live in `rcv.c`, one module per protocol domain.
//!
//! Each was its own crate before the step 3 consolidation. They share the same
//! shape: parse the frame (natively via `hxproto`, or take an
//! already-parsed one from the C dispatcher), consult the model, emit the
//! matching `GtkhxSession` signal, and return a discriminant telling the C
//! caller which branch was taken.
//!
//! The domains the session handles itself arrive as its events instead,
//! through [`hx_recv_session_event`].

use std::os::raw::c_void;

use hxsession::Event;

pub mod chat;
pub mod files;
pub mod icon;
pub mod msg;
pub mod news;
pub mod user;
pub mod xfer;

#[cfg(not(test))]
extern "C" {
    /// proto_trace.c — the receive half of the `GTKHX_DEBUG=proto` trace.
    fn proto_trace_recv_hdr(ty: u32, trans: u32, flag: u32, len: u32);
    fn proto_trace_recv_chunks(frame: *const u8, frame_len: usize);
}

/// `void hx_recv_session_event (htlc, ev)` — what the session made of what
/// the server sent (`hxnet::Event::Session`), on the main thread, for a
/// connection still open.
///
/// # Safety
/// `htlc` is a live connection; `ev` points at an `hxsession::Event` for the
/// call.
#[no_mangle]
pub unsafe extern "C" fn hx_recv_session_event(htlc: *mut c_void, ev: *const c_void) {
    match &*(ev as *const Event) {
        Event::Received(frame) => {
            if let Some(h) = hxproto::parse::Header::parse(frame) {
                // The chunk trace reads from the field count on.
                let fields = &frame[20..];
                proto_trace_recv_hdr(h.type_, h.trans, h.flag, fields.len() as u32);
                proto_trace_recv_chunks(fields.as_ptr(), fields.len());
            }
        }
        Event::Chat {
            cid,
            uid,
            text,
            media,
        } => chat::line(htlc, *cid, *uid, text, media.as_ref()),
        Event::ChatInvite { cid, uid, name } => chat::invited(htlc, *cid, *uid, name),
        Event::ChatSubject { cid, subject } => chat::subject(htlc, *cid, subject),
        Event::ChatHistory {
            trans,
            cid,
            entries,
            has_more,
        } => chat::history(htlc, *trans, *cid, entries, *has_more),
        Event::UserChanged { cid, user } => user::changed(htlc, *cid, user),
        Event::UserLeft { cid, uid } => user::left(htlc, *cid, *uid),
        Event::UserList { users, subject, .. } => user::listed(htlc, users, subject.as_deref()),
        Event::ChatCreated { cid, user, .. } => user::changed(htlc, *cid, user),
        Event::ChatJoined {
            trans,
            cid,
            users,
            subject,
        } => user::joined(htlc, *trans, *cid, users, subject.as_deref()),
        Event::Failed { trans, reason } => {
            user::failed(htlc, *trans);
            chat::failed(htlc, *trans, reason.as_deref());
        }
        _ => {}
    }
}

#[cfg(test)]
unsafe fn proto_trace_recv_hdr(_ty: u32, _trans: u32, _flag: u32, _len: u32) {}
#[cfg(test)]
unsafe fn proto_trace_recv_chunks(_frame: *const u8, _frame_len: usize) {}
