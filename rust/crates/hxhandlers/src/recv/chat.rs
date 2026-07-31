//! Chat-domain receive handlers (ported from `rcv.c`).
//!
//! The server-notification handlers in `rcv.c` are thin glue: parse the frame
//! (still C — `proto_helpers` reads `htlc->in`), consult the per-chat model,
//! and emit a `GtkhxSession` signal. This crate owns the model-gate + emit half
//! so the decision (e.g. "drop an invite from an ignored user") is unit-testable
//! headlessly, instead of only against a live server. The C handler keeps the
//! parse + the model lookup and calls in here.

use std::os::raw::{c_char, c_int, c_void};

#[cfg(not(test))]
use gtkhx_core::boxed::chat::hx_chat_event_free;
#[cfg(not(test))]
use gtkhx_core::conn::{
    hx_conn_chat_history_last_msgid, hx_conn_has_cap, hx_conn_name, hx_conn_sess,
    hx_conn_set_chat_history_last_msgid,
};
#[cfg(not(test))]
use gtkhx_core::session::{
    gtkhx_session_emit_chat, gtkhx_session_emit_chat_history_batch,
    gtkhx_session_emit_chat_invitation, gtkhx_session_emit_chat_subject,
    gtkhx_session_emit_chat_subject_notice, gtkhx_session_get_default,
};
#[cfg(not(test))]
use hxmodel::chat_members::hx_member_model_get_ignore;
#[cfg(not(test))]
use hxmodel::conversation::{hx_chat_member_model, hx_chat_set_subject, hx_chat_subject};

// Chat-history batch build: native parse + free (gtkhx-core boxed value type),
// the glib GPtrArray the signal carries, and the native chunk walker. All are
// real in both builds — glib and gtkhx-core work under `cargo test`.
use glib::ffi::{g_ptr_array_add, g_ptr_array_new_with_free_func, g_ptr_array_unref, gpointer};
use gtkhx_core::boxed::history::{hx_history_entry_free, hx_history_entry_parse, HxHistoryEntry};
use hotline_proto::wire::ChunkIter;

/// Wire chunk types in a GET_CHAT_HISTORY (700) reply (hotline.h).
const HTLS_DATA_HISTORY_ENTRY: u16 = 0x0f05;
const HTLS_DATA_HISTORY_HAS_MORE: u16 = 0x0f06;
const HTLS_DATA_TASKERROR: u16 = 0x0064;

#[cfg(not(test))]
extern "C" {
    /// Look up a chat by id on a session (`struct chat *`; NULL if absent). cid 0
    /// is the always-present public chat (chat.c).
    fn chat_with_cid(sess: *mut c_void, cid: u32) -> *mut c_void;
    /// Build a boxed `HxChatEvent` from the raw (CR2LF + strip_ansi'd) chat body
    /// — copies the bytes, runs the UTF-8 validation + is-self classification
    /// (proto_helpers.c producer). Freed with [`hx_chat_event_free`].
    fn hx_chat_event_new(
        raw: *const c_char,
        raw_len: usize,
        cid: u32,
        uid: u16,
        self_nick: *const c_char,
    ) -> *mut c_void;
    /// Attach inline-media metadata to a chat event (copies id + mime;
    /// proto_helpers.c producer).
    #[allow(clippy::too_many_arguments)]
    fn hx_chat_event_attach_media(
        ev: *mut c_void,
        id: *const u8,
        id_len: usize,
        mime: *const c_char,
        mime_len: usize,
        width: u32,
        width_present: c_int,
        height: u32,
        height_present: c_int,
        bytes: u32,
        bytes_present: c_int,
    );
    /// Log a pre-formatted line under a debug category (debug.c).
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
}

/// The inline-media capability bit (HTLC_CAP_INLINE_MEDIA, hotline.h).
const HTLC_CAP_INLINE_MEDIA: u64 = 0x0008;

/// gboolean from a Rust bool (glib `gint`, i.e. `c_int`, TRUE=1 / FALSE=0).
#[inline]
fn gbool(b: bool) -> c_int {
    b as c_int
}

/// Emit a pre-formatted line under `cat` via debug_log_str.
///
/// The line often interpolates wire-derived bytes (e.g. a media MIME type),
/// which may contain interior NULs. CString::new rejects those, so strip any
/// NULs first rather than unwrap-panicking on hostile / damaged input — a
/// debug trace must never be able to crash the client.
///
/// # Safety
/// `debug_log_str` is an FFI call into debug.c.
unsafe fn debug_trace(cat: &std::ffi::CStr, line: String) {
    let sanitized: String = line.replace('\0', "");
    // Now infallible: sanitized has no interior NULs.
    if let Ok(c) = std::ffi::CString::new(sanitized) {
        debug_log_str(cat.as_ptr(), c.as_ptr());
    }
}

/// `void hx_chat_invite_recv (htlc, member_model, cid, uid, name)` — the
/// private-chat-invitation receive path: drop the invite if the inviter is on
/// the ignore list, otherwise emit the `chat-invitation` signal (the sound
/// subscriber chimes off it). `member_model` is the public chat's member model
/// (`hx_chat_member_model(chat_with_cid(sess, 0))`); `name` is the inviter's
/// wire name as the parser produced it — `strip_ansi`'d and length-capped, but
/// NOT encoding-converted, so it's the server's raw bytes (often Mac Roman) and
/// is not guaranteed valid UTF-8. It's only forwarded to the signal here; any
/// Mac Roman → UTF-8 conversion happens later on the view side.
///
/// # Safety
/// `member_model` is a valid `HxMemberModel *`; `name` is a valid C string;
/// `htlc` is the connection handle (opaque, only forwarded to the signal).
#[no_mangle]
pub unsafe extern "C" fn hx_chat_invite_recv(
    htlc: *mut c_void,
    member_model: *mut c_void,
    cid: u32,
    uid: u16,
    name: *const c_char,
) {
    if hx_member_model_get_ignore(member_model, uid) != 0 {
        return;
    }
    gtkhx_session_emit_chat_invitation(gtkhx_session_get_default(), htlc, cid, name);
}

/// `void hx_rcv_chat_invite (htlc, frame, frame_len)` — the HTLS_HDR_CHAT_INVITE
/// primary handler (was `rcv.c`). The first receive handler whose whole body
/// lives in Rust (docs/rust/network-endgame.md): the C dispatch switch in
/// `hx_dispatch_frame` calls this by name; the body parses the frame via
/// `hotline_proto::parse` (a native Rust call — no C-ABI round-trip), resolves
/// the public chat's member model through the chat.c lookups, and delegates the
/// ignore-gate + emit to [`hx_chat_invite_recv`].
///
/// The ignore check uses the **public** chat's member model (cid 0), where every
/// user lives — matching the old C, even though the invite targets `inv.cid`.
///
/// # Safety
/// C-ABI handler invoked from the receive dispatch on the main thread. `htlc` is
/// a valid connection handle; `frame` is valid for `frame_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn hx_rcv_chat_invite(htlc: *mut c_void, frame: *const u8, frame_len: usize) {
    if frame.is_null() {
        return;
    }
    let buf = std::slice::from_raw_parts(frame, frame_len);
    // 31: the C name cap (hx_chat_invite_msg.name[32] − NUL). Missing chunks
    // parse as uid/cid 0 + empty name — the same "always emit" behaviour the old
    // C had (its extractor never failed on malformed data).
    let inv = hotline_proto::parse::parse_chat_invite(buf, frame_len, 31);
    let sess = hx_conn_sess(htlc.cast());
    let chat = chat_with_cid(sess, 0);
    if chat.is_null() {
        return;
    }
    // NUL-terminate the (≤ 31-byte) name for the C chat-invitation signal.
    let mut name = [0u8; 32];
    let n = inv.name.len().min(31);
    name[..n].copy_from_slice(&inv.name[..n]);
    hx_chat_invite_recv(
        htlc,
        hx_chat_member_model(chat.cast()),
        inv.cid,
        inv.uid,
        name.as_ptr() as *const c_char,
    );
}

/// `void hx_rcv_chat_subject (htlc, frame, frame_len)` — the HTLS_HDR_CHAT_SUBJECT
/// primary handler (was `rcv.c`). Parses the frame via
/// `hotline_proto::parse` (native), and — for a non-empty subject on a known
/// chat — delegates the change-gate + emit to [`hx_chat_subject_recv`]. On a real
/// change it sets the chat model subject (C collaborator) and emits the
/// "Subject Changed to" notice signal (the view-side handler owns the gettext).
/// An empty subject or unknown chat is a no-op, exactly as the old C did.
///
/// # Safety
/// C-ABI handler invoked from the receive dispatch on the main thread. `htlc` is
/// a valid connection handle; `frame` is valid for `frame_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn hx_rcv_chat_subject(
    htlc: *mut c_void,
    frame: *const u8,
    frame_len: usize,
) {
    if frame.is_null() {
        return;
    }
    let buf = std::slice::from_raw_parts(frame, frame_len);
    // 255: the C subject cap. Subjects carry no line endings, so no CR2LF /
    // strip_ansi — raw wire bytes (may be Mac Roman; UTF-8 fix-up is view-side).
    let sub = hotline_proto::parse::parse_chat_subject(buf, frame_len, 255);
    if sub.subject.is_empty() {
        return;
    }
    let sess = hx_conn_sess(htlc.cast());
    let chat = chat_with_cid(sess, sub.cid);
    if chat.is_null() {
        return;
    }
    // NUL-terminate the (≤ 255-byte) subject for the C string calls.
    let mut s = [0u8; 256];
    let n = sub.subject.len().min(255);
    s[..n].copy_from_slice(&sub.subject[..n]);
    let subj_ptr = s.as_ptr() as *const c_char;
    if hx_chat_subject_recv(htlc, sub.cid, subj_ptr, n, hx_chat_subject(chat.cast())) != 0 {
        hx_chat_set_subject(chat.cast(), subj_ptr, n);
        gtkhx_session_emit_chat_subject_notice(
            gtkhx_session_get_default(),
            htlc,
            sub.cid,
            hx_chat_subject(chat.cast()),
        );
    }
}

/// `void hx_rcv_chat (htlc, frame, frame_len)` — the HTLS_HDR_CHAT public-chat
/// line handler (was `rcv.c`). Parses the body via native
/// `hotline_proto::parse::parse_chat`; when the inline-media cap is negotiated,
/// pulls the media companion via native `inline_media::extract_chat_media_meta`
/// (dropping the whole line on an orphaned companion, per spec). Builds the boxed
/// `HxChatEvent` (C producer, which copies + UTF-8-validates + self-classifies),
/// attaches any media, then delegates the ignore-gate + emit to the in-crate
/// [`hx_chat_recv`], and frees the event either way.
///
/// # Safety
/// C-ABI handler invoked from the receive dispatch on the main thread. `htlc` is
/// a valid connection handle; `frame` is valid for `frame_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn hx_rcv_chat(htlc: *mut c_void, frame: *const u8, frame_len: usize) {
    if frame.is_null() {
        return;
    }
    let buf = std::slice::from_raw_parts(frame, frame_len);
    // 8192: the C body cap. parse_chat CR2LF's + strip_ansi's the body and drops
    // a single leading LF; `text()` is the display slice.
    let cm = hotline_proto::parse::parse_chat(buf, frame_len, 8192);

    let sess = hx_conn_sess(htlc.cast());
    let chat = chat_with_cid(sess, 0);
    if chat.is_null() {
        return;
    }

    // Inline-media companion — only when the server confirmed the cap. `media`
    // borrows `buf` (the raw frame), valid for the whole handler.
    let mut media: Option<hotline_proto::inline_media::ChatMediaMeta> = None;
    if hx_conn_has_cap(htlc.cast(), HTLC_CAP_INLINE_MEDIA) != 0 {
        let walker = hotline_proto::wire::ChunkIter::over_message(buf, frame_len);
        match hotline_proto::inline_media::extract_chat_media_meta(walker) {
            Ok(None) => {}
            Ok(Some(m)) => media = Some(m),
            Err(_) => {
                // Spec: exactly one companion field present → reject the whole
                // chat (orphan implies server bug / wire damage).
                debug_trace(
                    c"media",
                    format!(
                        "drop chat with orphaned media companion (cid={}, uid={})",
                        cm.cid, cm.uid
                    ),
                );
                return;
            }
        }
    }

    // Boxed HxChatEvent (the C producer copies the bytes). self_nick = our own
    // display name, or NULL when unset.
    let own = hx_conn_name(htlc.cast());
    let self_nick = if !own.is_null() && *own != 0 {
        own
    } else {
        std::ptr::null()
    };
    let text = cm.text();
    // cm.uid comes straight off the wire's UID chunk (parse_chat), and
    // is 0 when the server sent none. Passing it through means the
    // render path gets the sender's identity from the protocol rather
    // than by looking the nick up — see chat.c::chat_speaker_for.
    let ev = hx_chat_event_new(
        text.as_ptr() as *const c_char,
        text.len(),
        cm.cid,
        cm.uid,
        self_nick,
    );
    if let Some(m) = media {
        hx_chat_event_attach_media(
            ev,
            m.id.as_ptr(),
            m.id.len(),
            m.type_.as_ptr() as *const c_char,
            m.type_.len(),
            m.width.unwrap_or(0),
            gbool(m.width.is_some()),
            m.height.unwrap_or(0),
            gbool(m.height.is_some()),
            m.bytes.unwrap_or(0),
            gbool(m.bytes.is_some()),
        );
        debug_trace(
            c"media",
            format!(
                "chat with media: cid={} uid={} mime={} dims={}x{} bytes={}",
                cm.cid,
                cm.uid,
                String::from_utf8_lossy(m.type_),
                m.width.unwrap_or(0),
                m.height.unwrap_or(0),
                m.bytes.unwrap_or(0)
            ),
        );
    }
    // Ignore-gate + emit; C owns `ev` and frees it either way.
    hx_chat_recv(htlc, hx_chat_member_model(chat.cast()), cm.uid, ev);
    hx_chat_event_free(ev.cast());
}

/// `int hx_chat_subject_recv (htlc, cid, subject, subject_len, current_subject)`
/// — the chat-subject-change receive path. Returns 1 and emits `chat-subject`
/// when the subject is non-empty AND differs from the current one; the C side
/// then updates the model + logs the "Subject Changed to" line. Returns 0 (no
/// emit) for an empty or unchanged subject.
///
/// The emit forwards `subject` verbatim, which is byte-identical to the value
/// the old C read back from the model after setting it; the one subscriber
/// (`output_chat_subject`) uses the signal argument, not the model, so emitting
/// before the C-side set is safe.
///
/// # Safety
/// `subject` / `current_subject` are NUL-terminated C strings (the wire parse
/// and the model getter); `htlc` is opaque and only forwarded to the signal.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_subject_recv(
    htlc: *mut c_void,
    cid: u32,
    subject: *const c_char,
    subject_len: usize,
    current_subject: *const c_char,
) -> c_int {
    if subject_len == 0 {
        return 0;
    }
    // Unchanged subject → no announcement. (An empty subject was already
    // rejected above; current_subject is the model's "" when unset.)
    if !subject.is_null()
        && !current_subject.is_null()
        && std::ffi::CStr::from_ptr(subject) == std::ffi::CStr::from_ptr(current_subject)
    {
        return 0;
    }
    gtkhx_session_emit_chat_subject(gtkhx_session_get_default(), htlc, cid, subject);
    1
}

/// `void hx_chat_subject_emit (htlc, cid, subject)` — the initial-subject-
/// discovery emit (the `rcv_task_user_list` room-load path). Unlike
/// [`hx_chat_subject_recv`], this has no change-gate: the room just came into
/// view and the caller has already set the model, so the subject is always
/// published to refresh the widget (with no "Subject Changed to" log line).
///
/// # Safety
/// `subject` is a NUL-terminated C string; `htlc` is opaque and only forwarded.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_subject_emit(htlc: *mut c_void, cid: u32, subject: *const c_char) {
    gtkhx_session_emit_chat_subject(gtkhx_session_get_default(), htlc, cid, subject);
}

/// `void hx_chat_history_recv (htlc, cid, entries, has_more)` — publish a
/// `chat-history-batch` reply. The C handler keeps the chunk walk that builds
/// the `GPtrArray<HxHistoryEntry*>` and advances the newest-msgid cursor; this
/// is the view-notify hop. The array is borrowed for the emit only — the C side
/// still owns and frees it after.
///
/// # Safety
/// `entries` is a valid `GPtrArray *` live for the duration of the call; `htlc`
/// is opaque and only forwarded to the signal.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_history_recv(
    htlc: *mut c_void,
    cid: u32,
    entries: *mut c_void,
    has_more: c_int,
) {
    gtkhx_session_emit_chat_history_batch(
        gtkhx_session_get_default(),
        htlc,
        cid,
        entries,
        has_more,
    );
}

/// `GDestroyNotify` shim: the `GPtrArray` frees each entry with the gtkhx-core
/// `hx_history_entry_free` (which is typed `*mut HxHistoryEntry`).
///
/// # Safety
/// `p` is NULL or a valid `HxHistoryEntry*` (the array only ever holds those).
unsafe extern "C" fn destroy_history_entry(p: gpointer) {
    hx_history_entry_free(p as *mut HxHistoryEntry);
}

/// `void rcv_task_chat_history (htlc, frame, frame_len, channel_ptr, data)` — the
/// TRAN_GET_CHAT_HISTORY (700) reply walker (was `rcv.c`). Walks the reply's
/// chunks natively: each `HTLS_DATA_HISTORY_ENTRY` parses into a heap
/// `HxHistoryEntry` (gtkhx-core `hx_history_entry_parse`) accumulated into a
/// `GPtrArray` whose destroy-func frees them; `HTLS_DATA_HISTORY_HAS_MORE` sets
/// the more-pages flag; a `HTLS_DATA_TASKERROR` means the server refused the
/// request (logged — the subscriber then sees zero entries + has_more=FALSE, the
/// same shape as "no history to return"). The channel id isn't echoed in the
/// reply, so it rides the task ptr (`GUINT_TO_POINTER`) and is recovered here.
///
/// Before emitting, advance the session-wide newest-msgid cursor used for the
/// `AFTER=` reconnect catch-up (it grows monotonically over the htlc's lifetime,
/// independent of the per-chat oldest-msgid the Load-older flow shrinks). The
/// `chat-history-batch` emit borrows the array for the call; it's unref'd (and
/// every entry freed via the destroy-func) right after.
///
/// # Safety
/// C-ABI reply callback (`hx_rcv_task`, main thread). `frame` is valid for
/// `frame_len` bytes; `channel_ptr` is `GUINT_TO_POINTER(cid)`.
#[no_mangle]
pub unsafe extern "C" fn rcv_task_chat_history(
    htlc: *mut c_void,
    frame: *const u8,
    frame_len: usize,
    channel_ptr: *mut c_void,
    _data: *mut c_void,
) {
    let cid = channel_ptr as usize as u32; // GPOINTER_TO_UINT
    let entries = g_ptr_array_new_with_free_func(Some(destroy_history_entry));
    let mut has_more = false;
    let mut max_msgid: u64 = 0;

    if !frame.is_null() {
        let buf = std::slice::from_raw_parts(frame, frame_len);
        for chunk in ChunkIter::over_message(buf, frame_len) {
            match chunk.tag {
                HTLS_DATA_HISTORY_ENTRY => {
                    let e = hx_history_entry_parse(chunk.data.as_ptr(), chunk.data.len());
                    if e.is_null() {
                        debug_trace(
                            c"chat-history",
                            format!("skipping malformed entry, len={}", chunk.data.len()),
                        );
                        continue;
                    }
                    if (*e).message_id > max_msgid {
                        max_msgid = (*e).message_id;
                    }
                    g_ptr_array_add(entries, e as gpointer);
                }
                HTLS_DATA_HISTORY_HAS_MORE => {
                    if let Some(&b0) = chunk.data.first() {
                        has_more = b0 != 0;
                    }
                }
                HTLS_DATA_TASKERROR => {
                    debug_trace(
                        c"chat-history",
                        format!("server returned task error for GET_CHAT_HISTORY (cid={cid})"),
                    );
                }
                _ => {}
            }
        }
    }

    if max_msgid > hx_conn_chat_history_last_msgid(htlc.cast()) {
        hx_conn_set_chat_history_last_msgid(htlc.cast(), max_msgid);
    }

    debug_trace(
        c"chat-history",
        format!(
            "received batch: cid={cid} entries={} has_more={}",
            (*entries).len,
            has_more as i32
        ),
    );

    hx_chat_history_recv(htlc, cid, entries as *mut c_void, has_more as c_int);
    g_ptr_array_unref(entries);
}

/// `int hx_chat_recv (htlc, member_model, uid, event)` — the public-chat line
/// receive path: drop the line when its sender (`uid`) is on the ignore list,
/// otherwise emit the `chat` signal carrying the boxed `HxChatEvent`. Returns 1
/// when it emitted, 0 when it dropped. A `uid` of 0 is a server/system line
/// (no sender to ignore), so it always emits.
///
/// The C handler owns `event`: it builds the `HxChatEvent` (including any inline
/// -media companion) before calling and frees it after, whether or not this
/// emitted. The emit only borrows it for the duration of the signal.
///
/// # Safety
/// `member_model` is a valid `HxMemberModel *`; `event` is a valid boxed
/// `HxChatEvent *`; `htlc` is opaque and only forwarded to the signal.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_recv(
    htlc: *mut c_void,
    member_model: *mut c_void,
    uid: u16,
    event: *mut c_void,
) -> c_int {
    if uid != 0 && hx_member_model_get_ignore(member_model, uid) != 0 {
        return 0;
    }
    gtkhx_session_emit_chat(gtkhx_session_get_default(), htlc, event);
    1
}

// ---- test doubles for the C environment ------------------------------------

#[cfg(test)]
pub(crate) mod test_env {
    use std::cell::Cell;

    thread_local! {
        /// Configurable return for the stubbed ignore lookup.
        pub static IGNORE: Cell<bool> = const { Cell::new(false) };
        /// Records the last emitted invitation as (cid, name-bytes), or None.
        pub static EMITTED: Cell<Option<(u32, Vec<u8>)>> = const { Cell::new(None) };
        /// Records the last emitted chat-subject as (cid, subj-bytes), or None.
        pub static SUBJECT_EMITTED: Cell<Option<(u32, Vec<u8>)>> = const { Cell::new(None) };
        /// Records the boxed-event pointer of the last emitted `chat`, or None.
        pub static CHAT_EMITTED: Cell<Option<*mut std::os::raw::c_void>> = const { Cell::new(None) };
        /// Records the last emitted chat-history-batch as (cid, entries-ptr,
        /// has_more), or None.
        pub static HISTORY_EMITTED: Cell<Option<(u32, *mut std::os::raw::c_void, bool)>> =
            const { Cell::new(None) };
        /// Configurable return for the stubbed inline-media cap check.
        pub static HAS_CAP: Cell<bool> = const { Cell::new(false) };
        /// Records the (cid, body-bytes) the stubbed hx_chat_event_new saw.
        pub static EVENT_NEW: std::cell::RefCell<Option<(u32, Vec<u8>)>> =
            const { std::cell::RefCell::new(None) };
        /// Whether the stubbed hx_chat_event_attach_media was called.
        pub static MEDIA_ATTACHED: Cell<bool> = const { Cell::new(false) };
        /// The session-wide chat-history newest-msgid cursor.
        pub static CURSOR: Cell<u64> = const { Cell::new(0) };
    }

    pub fn reset() {
        IGNORE.with(|c| c.set(false));
        EMITTED.with(|c| c.set(None));
        SUBJECT_EMITTED.with(|c| c.set(None));
        CHAT_EMITTED.with(|c| c.set(None));
        HISTORY_EMITTED.with(|c| c.set(None));
        HAS_CAP.with(|c| c.set(false));
        EVENT_NEW.with(|c| *c.borrow_mut() = None);
        MEDIA_ATTACHED.with(|c| c.set(false));
        CURSOR.with(|c| c.set(0));
    }
}

#[cfg(test)]
unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_chat_invitation(
    _self_: *mut c_void,
    _htlc: *mut c_void,
    cid: u32,
    name: *const c_char,
) {
    let bytes = if name.is_null() {
        Vec::new()
    } else {
        std::ffi::CStr::from_ptr(name).to_bytes().to_vec()
    };
    test_env::EMITTED.with(|c| c.set(Some((cid, bytes))));
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_chat_subject(
    _self_: *mut c_void,
    _htlc: *mut c_void,
    cid: u32,
    subj: *const c_char,
) {
    let bytes = if subj.is_null() {
        Vec::new()
    } else {
        std::ffi::CStr::from_ptr(subj).to_bytes().to_vec()
    };
    test_env::SUBJECT_EMITTED.with(|c| c.set(Some((cid, bytes))));
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_chat(_self_: *mut c_void, _htlc: *mut c_void, event: *mut c_void) {
    test_env::CHAT_EMITTED.with(|c| c.set(Some(event)));
}

#[cfg(test)]
unsafe fn gtkhx_session_emit_chat_history_batch(
    _self_: *mut c_void,
    _htlc: *mut c_void,
    cid: u32,
    entries: *mut c_void,
    has_more: c_int,
) {
    test_env::HISTORY_EMITTED.with(|c| c.set(Some((cid, entries, has_more != 0))));
}

#[cfg(test)]
unsafe fn hx_member_model_get_ignore(_model: *mut c_void, _uid: u16) -> c_int {
    c_int::from(test_env::IGNORE.with(|c| c.get()))
}

// Non-null sentinels so the handler proceeds past the lookups to the recv logic;
// the doubles above ignore the actual pointer values.
#[cfg(test)]
unsafe fn hx_conn_sess(_htlc: *const c_void) -> *mut c_void {
    std::ptr::dangling_mut::<c_void>()
}
#[cfg(test)]
unsafe fn chat_with_cid(_sess: *mut c_void, _cid: u32) -> *mut c_void {
    2 as *mut c_void
}
#[cfg(test)]
unsafe fn hx_chat_member_model(_chat: *mut c_void) -> *mut c_void {
    3 as *mut c_void
}
// Empty current subject → any non-empty new subject is a change (the change-gate
// itself is exercised directly against hx_chat_subject_recv elsewhere).
#[cfg(test)]
unsafe fn hx_chat_subject(_chat: *mut c_void) -> *const c_char {
    c"".as_ptr()
}
#[cfg(test)]
unsafe fn hx_chat_set_subject(_chat: *mut c_void, _s: *const c_char, _len: usize) {}
#[cfg(test)]
unsafe fn gtkhx_session_emit_chat_subject_notice(
    _self_: *mut c_void,
    _htlc: *mut c_void,
    _cid: u32,
    _subject: *const c_char,
) {
}

/// A fixed non-null sentinel the chat-event doubles hand back / expect.
#[cfg(test)]
pub(crate) const FAKE_CHAT_EVENT: *mut c_void = 0xC0FE_usize as *mut c_void;

#[cfg(test)]
unsafe fn hx_conn_has_cap(_htlc: *const c_void, _cap: u64) -> c_int {
    c_int::from(test_env::HAS_CAP.with(|c| c.get()))
}
#[cfg(test)]
unsafe fn hx_conn_name(_htlc: *const c_void) -> *const c_char {
    c"".as_ptr()
}
#[cfg(test)]
unsafe fn hx_chat_event_new(
    raw: *const c_char,
    raw_len: usize,
    cid: u32,
    _uid: u16,
    _self_nick: *const c_char,
) -> *mut c_void {
    let body = std::slice::from_raw_parts(raw as *const u8, raw_len).to_vec();
    test_env::EVENT_NEW.with(|c| *c.borrow_mut() = Some((cid, body)));
    FAKE_CHAT_EVENT
}
#[cfg(test)]
#[allow(clippy::too_many_arguments)]
unsafe fn hx_chat_event_attach_media(
    _ev: *mut c_void,
    _id: *const u8,
    _id_len: usize,
    _mime: *const c_char,
    _mime_len: usize,
    _width: u32,
    _width_present: c_int,
    _height: u32,
    _height_present: c_int,
    _bytes: u32,
    _bytes_present: c_int,
) {
    test_env::MEDIA_ATTACHED.with(|c| c.set(true));
}
#[cfg(test)]
unsafe fn hx_chat_event_free(_ev: *mut c_void) {}
#[cfg(test)]
unsafe fn debug_log_str(_cat: *const c_char, _msg: *const c_char) {}

#[cfg(test)]
unsafe fn hx_conn_chat_history_last_msgid(_h: *const c_void) -> u64 {
    test_env::CURSOR.with(|c| c.get())
}

#[cfg(test)]
unsafe fn hx_conn_set_chat_history_last_msgid(_h: *mut c_void, v: u64) {
    test_env::CURSOR.with(|c| c.set(v));
}

#[cfg(test)]
mod tests;
