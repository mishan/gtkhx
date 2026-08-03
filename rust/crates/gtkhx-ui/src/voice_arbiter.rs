//! `voice_arbiter` — who owns the microphone.
//!
//! There is one microphone, so there is one voice chat at a time anywhere in
//! the application. Joining voice on server B, or a *different room* on server
//! A, has to leave whatever is currently held first.
//!
//! This is a token: a `(connection, room)` pair, or nothing. Acquiring it
//! reports what has to be given up, and the caller is expected to give it up
//! before proceeding — see [`acquire`].
//!
//! ## Why the join and not the runtime
//!
//! The obvious place to put this is where a session's `VoiceRuntime` is
//! constructed, since that is the one-per-session object that owns a pipeline.
//! That would be wrong. A runtime is built once per connection and then
//! reused: join room 3, leave, join room 7 — same runtime throughout. The
//! exclusive act is the *join*, so that is where the token is taken. Gating
//! construction would let a connection hop rooms without ever asking.
//!
//! ## What it does not do
//!
//! It does not perform the preemption. Leaving a room means driving a state
//! machine and putting a frame on that connection's wire, which is the voice
//! panel's job and needs its FFI; and under the confirm-first UX the leave
//! happens after an answer the user has yet to give. So this answers "what
//! would you have to give up?" and records the outcome, and `voice_panel`
//! carries it out.
//!
//! Main thread only, like everything else that touches a runtime.

use std::cell::RefCell;
use std::ffi::c_void;

use crate::dock::{self, ConnKey};

extern "C" {
    /// `session_registry.c` — the live session on this connection, or NULL if
    /// it has been closed.
    fn hx_session_with_serial(serial: u16) -> *mut c_void;
}

/// Who holds the microphone.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Holder {
    /// The connection whose runtime is joined, by serial rather than by
    /// session pointer. The token outlives any particular turn of the main
    /// loop — that is the whole point of it — and a session does not: closing
    /// a tab frees one. A serial names a connection without being able to
    /// dangle, and [`Holder::session`] resolves it when it is actually needed.
    pub conn: ConnKey,
    /// The room it is joined to. Only unique within the connection, which is
    /// why the pair is the token rather than the cid alone.
    pub cid: u32,
}

impl Holder {
    /// The session holding voice, or NULL if that connection has since been
    /// closed. Callers must check — this is the point at which a stale token
    /// becomes visible.
    pub fn session(&self) -> *mut c_void {
        unsafe { hx_session_with_serial(self.conn) }
    }
}

thread_local! {
    static HOLDER: RefCell<Option<Holder>> = const { RefCell::new(None) };
}

/// What acquiring the token requires of the caller.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Acquire {
    /// Nothing held, or the caller already holds exactly this room. Join.
    Free,
    /// This holder has to leave first. The caller confirms, leaves, and calls
    /// [`take`] when the room is actually theirs.
    Preempts(Holder),
}

/// Ask what joining `(sess, cid)` would cost.
///
/// Deliberately a question rather than a seizure: the answer drives a
/// confirmation the user can decline, and a token taken before the answer
/// would have to be handed back on "no".
///
/// Re-joining the room you already hold is `Free`, not a self-preemption —
/// the panel can re-drive a join on reconnect, and making that ask the user
/// whether to leave the room they are in would be nonsense.
///
/// Every path that tears a runtime down calls [`release`] or
/// [`release_session`], so a connection that stops holding voice says so. A
/// token that outlives its connection anyway — a close that got there first —
/// resolves to a NULL session rather than a stale one.
pub fn acquire(sess: *mut c_void, cid: u32) -> Acquire {
    let conn = dock::key_for_session(sess);
    match holder() {
        Some(h) if h.conn == conn && h.cid == cid => Acquire::Free,
        Some(h) => Acquire::Preempts(h),
        None => Acquire::Free,
    }
}

/// Record `(sess, cid)` as the holder. For after the join has actually gone
/// out, so a failed join doesn't leave the token pointing at a room nobody is
/// in.
pub fn take(sess: *mut c_void, cid: u32) {
    let conn = dock::key_for_session(sess);
    HOLDER.with(|h| *h.borrow_mut() = Some(Holder { conn, cid }));
    show_holder();
}

/// Give the token up, if `(sess, cid)` is holding it.
///
/// Keyed on the pair because the token is a pair: a panel leaving room A must
/// not drop a claim on room B of the same connection, which is a state one
/// connection can genuinely be in — the panels live in several toplevels and
/// nothing stops a user acting on two of them.
pub fn release(sess: *mut c_void, cid: u32) {
    release_conn(dock::key_for_session(sess), cid);
}

/// [`release`] for a caller that has the connection rather than the session.
///
/// The form to use when giving up a token that may have outlived its
/// connection: resolving a closed connection back to a session yields NULL,
/// and NULL resolves *forward* to serial 0, which matches no token — so
/// releasing through the session would silently fail to release anything.
pub fn release_conn(conn: ConnKey, cid: u32) {
    HOLDER.with(|h| {
        let mut slot = h.borrow_mut();
        if *slot == Some(Holder { conn, cid }) {
            *slot = None;
        }
    });
    show_holder();
}

/// Give the token up if `sess` holds it, whichever room.
///
/// For teardown, where the room is not the question and often not knowable:
/// the connection is going away, so any claim it has goes with it. Still keyed
/// on the session, because this runs for connections that never had voice and
/// a disconnect on server B must not release server A's claim.
pub fn release_session(sess: *mut c_void) {
    let conn = dock::key_for_session(sess);
    if conn == 0 {
        return;
    }
    HOLDER.with(|h| {
        let mut slot = h.borrow_mut();
        if slot.map(|held| held.conn) == Some(conn) {
            *slot = None;
        }
    });
    show_holder();
}

/// Put the microphone mark on the holder's connection tab.
///
/// Driven from here rather than from the join and leave sites, so the mark and
/// the token cannot disagree: every path that changes who holds voice goes
/// through `take` or `release`.
///
/// `set_voice_indicator` is a no-op when the strip has no tabs, which is what
/// makes this safe to call unconditionally — including from the unit test
/// below, which runs without a display. Gating it out under `cfg(test)` would
/// have disabled it for the crate's *display-backed* test too, which is the
/// one place it could be covered.
fn show_holder() {
    crate::conn_tabs::set_voice_indicator(holder().map(|h| h.conn));
}

/// Who holds it, if anyone.
pub fn holder() -> Option<Holder> {
    HOLDER.with(|h| *h.borrow())
}

/// `void gtkhx_voice_arbiter_release (session *sess)` — give up the token from
/// C, for the disconnect path that frees a session's runtime.
///
/// # Safety
/// `sess` is a `session *` or NULL; main thread only.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_voice_arbiter_release(sess: *mut c_void) {
    release_session(sess);
}

/// `session *gtkhx_voice_arbiter_holder (void)` — the session holding voice, or
/// NULL.
///
/// No C caller yet; it exists because `voice_runtime.h` declares it and a
/// header that promises a symbol the library doesn't export is a link error
/// waiting for whoever first believes it. The Rust side asks `holder()`
/// directly — the tab-strip mark and push-to-talk both do — so this is here
/// for the C chrome that will want it.
#[no_mangle]
pub extern "C" fn gtkhx_voice_arbiter_holder() -> *mut c_void {
    holder().map_or(std::ptr::null_mut(), |h| h.session())
}

#[cfg(test)]
mod tests {
    use super::*;

    extern "C" {
        /// `gtkhx-core` — a fresh connection, with the next serial.
        fn hx_conn_new() -> *mut c_void;
    }

    /// A stand-in session: a real connection, not an invented pointer.
    ///
    /// `take` and `release` mark the connection tab, and that resolves a
    /// session to its serial through the real accessor — which dereferences.
    /// The stubbed `gtkhx_session_htlc` is an identity, so a connection is its
    /// own session here, the same arrangement the tab-strip test uses. Leaked
    /// on purpose; nothing frees a connection.
    fn new_session() -> *mut c_void {
        let sess = unsafe { hx_conn_new() };
        // The token holds a serial, so anything that resolves it back to a
        // session goes through the registry.
        crate::options_test_stubs::register_session(sess);
        sess
    }

    /// The token is free, taken, preempted and released — including the three
    /// cases that are easy to get backwards.
    ///
    /// The first is why the token is taken at the join rather than where a
    /// runtime is built: one runtime serves every room on its connection, so
    /// gating construction would let a connection hop rooms without ever
    /// asking. The other two are about giving the token up — releasing must
    /// be as precise as taking, or one panel's leave silently drops another
    /// room's claim.
    #[test]
    fn one_room_at_a_time_anywhere() {
        let (a, b) = (new_session(), new_session());
        assert_ne!(a, b, "two connections, one pointer");

        assert_eq!(acquire(a, 0), Acquire::Free, "nobody holds it");

        take(a, 0);
        assert_eq!(
            acquire(a, 0),
            Acquire::Free,
            "re-joining the room you hold asked you to leave it"
        );
        assert_eq!(
            acquire(a, 7),
            Acquire::Preempts(Holder {
                conn: dock::key_for_session(a),
                cid: 0
            }),
            "another room on the same connection went unnoticed"
        );
        assert_eq!(
            acquire(b, 0),
            Acquire::Preempts(Holder {
                conn: dock::key_for_session(a),
                cid: 0
            }),
            "the same room id on another connection went unnoticed"
        );

        // Releasing the wrong room on the right connection changes nothing: a
        // panel for room 7 leaving must not drop a claim on room 0.
        release(a, 7);
        assert_eq!(holder().map(|h| h.cid), Some(0), "released another room");

        // Nor a release from a connection that doesn't hold it — disconnecting
        // server B must not drop server A's claim.
        release_session(b);
        assert_eq!(holder().map(|h| h.session()), Some(a));

        release(a, 0);
        assert_eq!(holder(), None);
        assert_eq!(acquire(b, 0), Acquire::Free);

        // Teardown gives up whatever room the connection was in, without
        // needing to know which.
        take(b, 3);
        release_session(b);
        assert_eq!(holder(), None, "teardown left a claim behind");
    }
}
