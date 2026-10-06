//! Receive-side handlers — the `rcv_task_*` / notification bodies that used to
//! live in `rcv.c`, one module per protocol domain.
//!
//! Each was its own crate before the step 3 consolidation. They share the same
//! shape: parse the frame (natively via `hxproto`, or take an
//! already-parsed one from the C dispatcher), consult the model, emit the
//! matching `GtkhxSession` signal, and return a discriminant telling the C
//! caller which branch was taken.
//!
//! The login's reply, and the domains the session handles itself, arrive
//! as its events instead, through [`hx_recv_session_event`].

use std::os::raw::c_void;

use hxsession::{Closed, Event};

pub mod chat;
pub mod files;
pub mod icon;
pub mod login;
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
        Event::LoggedIn(info) => login::logged_in(htlc, info),
        Event::Closed(Closed::LoginRefused(Some(reason))) => login::refused(htlc, reason),
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
        Event::SelfInfo {
            uid,
            icon,
            access,
            color,
        } => user::selfinfo(htlc, *uid, *icon, *access, *color),
        Event::UserInfo { trans, name, info } => user::info(htlc, *trans, name, info),
        Event::Kicked { .. } => user::kicked(htlc),
        Event::Account { trans, account } => user::account(htlc, *trans, account),
        Event::AccountChanged { trans } => user::account_changed(htlc, *trans),
        Event::UserList { users, subject, .. } => user::listed(htlc, users, subject.as_deref()),
        Event::ChatCreated { cid, user, .. } => user::changed(htlc, *cid, user),
        Event::ChatJoined {
            trans,
            cid,
            users,
            subject,
        } => user::joined(htlc, *trans, *cid, users, subject.as_deref()),
        Event::Message {
            uid,
            from,
            text,
            media,
        } => msg::message(htlc, *uid, from, text, media.as_ref()),
        Event::Broadcast { uid, from, text } => msg::broadcast(htlc, *uid, from, text),
        Event::Disconnecting(text) => msg::parting(htlc, text),
        Event::NewsPosted(text) => news::posted(htlc, text),
        Event::NewsFile { text, .. } => news::file(htlc, text),
        Event::NewsListing { trans, items } => news::listing(htlc, *trans, items),
        Event::NewsCategory { trans, articles } => news::category(htlc, *trans, articles),
        Event::NewsArticle { trans, text } => news::article(htlc, *trans, text),
        Event::FileList { trans, files } => files::listed(htlc, *trans, files),
        Event::FileInfo { trans, info } => files::info(htlc, *trans, info),
        Event::FileChanged { trans } => files::changed(htlc, *trans),
        Event::Transfer { trans, transfer } => files::transfer(htlc, *trans, transfer),
        Event::TransferQueued { reference, queue } => xfer::queued(htlc, *reference, *queue),
        Event::IconList { trans, icons } => icon::listed(htlc, *trans, icons),
        Event::Icon { icon: i, .. } => icon::icon(htlc, i),
        Event::MediaUploading { trans, token } => {
            crate::media::uploading(htlc, *trans, token.as_deref())
        }
        Event::MediaUploaded { trans, media } => crate::media::uploaded(htlc, *trans, media),
        Event::MediaPart { trans, part } => crate::media::part(htlc, *trans, part),
        Event::MediaFailed {
            trans,
            code,
            reason,
        } => crate::media::failed(htlc, *trans, *code, reason.as_deref()),
        #[cfg(feature = "voice")]
        Event::VoiceJoined {
            trans,
            cid,
            sdp,
            codec,
            participants,
        } => crate::voice::joined(htlc, *trans, *cid, sdp, codec, participants),
        #[cfg(feature = "voice")]
        Event::VoiceDone { trans } => crate::voice::done(htlc, *trans),
        Event::Failed { trans, reason } => {
            let quiet = user::failed(htlc, *trans, reason.as_deref());
            #[cfg(feature = "voice")]
            let quiet = crate::voice::failed(htlc, *trans, reason.as_deref()) || quiet;
            let quiet = icon::failed(htlc, *trans, reason.as_deref()) || quiet;
            news::failed(htlc, *trans);
            files::failed(htlc, *trans);
            chat::failed(htlc, *trans, reason.as_deref().filter(|_| !quiet));
        }
        _ => {}
    }
}

/// Let go of every request `htlc` has in flight: a closed connection gets
/// no more replies, and a new login numbers its requests afresh. At the
/// login (`login`), the banner's request stays: a server may send its
/// banner, and be asked for it, before the login settles.
///
/// # Safety
/// Main thread.
pub(crate) unsafe fn forget(htlc: *mut c_void, login: bool) {
    user::forget(htlc);
    news::forget(htlc);
    files::forget(htlc, login);
    icon::forget(htlc);
    crate::media::forget(htlc);
    #[cfg(feature = "voice")]
    crate::voice::forget(htlc);
}

/// `void hx_recv_forget (struct htlc_conn *htlc)` — [`forget`], for the
/// connection closing.
///
/// # Safety
/// Main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_recv_forget(htlc: *mut c_void) {
    forget(htlc, false);
}

#[cfg(test)]
unsafe fn proto_trace_recv_hdr(_ty: u32, _trans: u32, _flag: u32, _len: u32) {}
#[cfg(test)]
unsafe fn proto_trace_recv_chunks(_frame: *const u8, _frame_len: usize) {}
