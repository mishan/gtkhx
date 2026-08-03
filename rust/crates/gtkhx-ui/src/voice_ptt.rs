//! Push-to-talk key controller (ported from `src/voice_ptt.c`, Phase 8.E).
//!
//! Attaches a CAPTURE-phase `GtkEventControllerKey` to a window: while the
//! session is in a voice room, holding the configured PTT key sends
//! `VOICE_MUTE(0)` (unmute) on press and `VOICE_MUTE(1)` (re-mute) on release,
//! edge-detected against GTK's key-repeat. Outside voice the controller stays
//! dormant (returns Proceed) so the bound key still works as a normal
//! shortcut.
//!
//! Wholly behind the crate's `voice` feature (gated in lib.rs) — the sole C
//! caller (`toolbar.c`) is `#ifdef HAVE_VOICE`, so no voice-off stub is
//! needed. The pure key-spec vocabulary stays C (`voice_ptt_keyspec.c`, keeps
//! its standalone unit test); this module externs only its `_parse`. Session /
//! htlc access + the runtime + the wire sender are the same C seams
//! `voice_panel` uses (`voice_bridge.c`, hxvoice-runtime, `voice.c`).

use std::cell::Cell;
use std::ffi::{c_char, c_void};
use std::os::raw::c_int;
use std::rc::Rc;

use glib::translate::{from_glib_borrow, IntoGlib};
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;

/// gtk-rs qdata key: the idempotence sentinel on the window.
const ATTACHED_KEY: &str = "voice-ptt-attached";

/// PTT modifier subset (mirrors `HX_VOICE_PTT_MODIFIER_MASK`). Lock-state bits
/// (CapsLock / NumLock / …) are excluded — they toggle rather than being held.
fn ptt_modifier_mask() -> gtk::gdk::ModifierType {
    gtk::gdk::ModifierType::CONTROL_MASK
        | gtk::gdk::ModifierType::SHIFT_MASK
        | gtk::gdk::ModifierType::ALT_MASK
        | gtk::gdk::ModifierType::SUPER_MASK
}

extern "C" {
    // voice_ptt_keyspec.c — parse the canonical bind string back into
    // (keyval, GdkModifierType). Returns TRUE on success (out params written).
    fn hx_voice_ptt_keyspec_parse(
        spec: *const c_char,
        out_keyval: *mut u32,
        out_state: *mut u32,
    ) -> glib::ffi::gboolean;

    // options.c — the typed by-name prefs bridge (same one options.rs uses).
    fn gtkhx_prefs_get_bool(name: *const c_char) -> c_int;
    fn gtkhx_prefs_get_string(name: *const c_char) -> *mut c_char;

    // voice_bridge.c — session field access.
    fn hx_session_htlc(sess: *mut c_void) -> *mut c_void;
    fn hx_session_voice_runtime(sess: *mut c_void) -> *mut c_void;

    // hxvoice-runtime.
    fn gtkhx_voice_runtime_active_cid(rt: *mut c_void, out_cid: *mut u32) -> c_int;
    fn gtkhx_voice_runtime_mute(rt: *mut c_void, muted: c_int);

    // voice.c — wire sender.
    fn hx_send_voice_mute(
        htlc: *mut c_void,
        cid: u32,
        muted: glib::ffi::gboolean,
    ) -> glib::ffi::gboolean;
}

/// Per-controller state. `pressed_keyval` latches the keyval that owns the
/// current press (0 = none) so the release re-mutes on the originally-pressed
/// key even if the user rebinds PTT mid-press.
///
/// Deliberately no session: the controller is attached once to the one
/// toolbar window, with whichever session existed at the time, and latching
/// that would have push-to-talk act on a connection that may not be the one
/// in voice. There is one microphone, so the question "whose mute does this
/// key toggle?" has exactly one answer — the connection holding it — and the
/// arbiter is where that is written down.
struct PttState {
    pressed_keyval: Cell<u32>,
}

/// The connection currently in voice, and the room it is in.
///
/// `None` when nobody is, in which case the key is not ours and propagates as
/// an ordinary shortcut.
fn voice_holder() -> Option<(*mut c_void, u32)> {
    let held = crate::voice_arbiter::holder()?;
    // Cross-check the runtime rather than trusting the token alone: the token
    // says who *claimed* the microphone, the runtime says which room it is
    // actually in, and a mute sent for the wrong room would be silently
    // ignored by the server.
    // NULL when the token outlived the connection holding it — a close that
    // got in ahead of the release. Nobody is in voice, so the key isn't ours.
    let sess = held.session();
    if sess.is_null() {
        return None;
    }
    session_in_voice(sess).map(|cid| (sess, cid))
}

/// The live PTT bind from prefs, or None when disabled / unset. Returns
/// `(keyval, masked_state_bits)`.
fn current_bind() -> Option<(u32, u32)> {
    let enabled = unsafe { gtkhx_prefs_get_bool(crate::cs("VOICEPTTENABLED").as_ptr()) != 0 };
    if !enabled {
        return None;
    }
    // This runs on every key event that reaches matches_bind(), so avoid the
    // Rust-side String + CString round-trip: hand the g_malloc'd prefs string
    // straight to the parser (NULL-safe — an unset key parses as FALSE), then
    // free it.
    let mut keyval: u32 = 0;
    let mut state: u32 = 0;
    unsafe {
        let p = gtkhx_prefs_get_string(crate::cs("VOICEPTTKEY").as_ptr());
        let ok = hx_voice_ptt_keyspec_parse(p, &mut keyval, &mut state) != glib::ffi::GFALSE;
        glib::ffi::g_free(p as *mut c_void);
        ok.then_some((keyval, state))
    }
}

/// Exact-match an inbound event against the configured bind. Inbound modifiers
/// are masked to the PTT subset first (lock bits ignored); Ctrl+F12 bound does
/// NOT match Ctrl+Shift+F12.
fn matches_bind(inbound_keyval: u32, inbound_state: gtk::gdk::ModifierType) -> bool {
    let Some((bound_keyval, bound_state)) = current_bind() else {
        return false;
    };
    if inbound_keyval != bound_keyval {
        return false;
    }
    (inbound_state & ptt_modifier_mask()).bits() == bound_state
}

/// The active cid iff the session has a runtime reporting an active room.
fn session_in_voice(sess: *mut c_void) -> Option<u32> {
    if sess.is_null() {
        return None;
    }
    let rt = unsafe { hx_session_voice_runtime(sess) };
    if rt.is_null() {
        return None;
    }
    let mut cid = 0u32;
    if unsafe { gtkhx_voice_runtime_active_cid(rt, &mut cid) } == 0 {
        return None;
    }
    Some(cid)
}

/// Send VOICE_MUTE + drive the runtime state machine (keeps self.muted in
/// sync + updates the panel label via MuteChanged). Returns false if the wire
/// helper refused (e.g. CAP_VOICE cleared mid-session).
fn drive_mute(sess: *mut c_void, cid: u32, muted: bool) -> bool {
    let htlc = unsafe { hx_session_htlc(sess) };
    let mflag = if muted {
        glib::ffi::GTRUE
    } else {
        glib::ffi::GFALSE
    };
    if unsafe { hx_send_voice_mute(htlc, cid, mflag) } == 0 {
        return false;
    }
    let rt = unsafe { hx_session_voice_runtime(sess) };
    unsafe { gtkhx_voice_runtime_mute(rt, if muted { 1 } else { 0 }) };
    true
}

fn on_key_pressed(
    state: &PttState,
    keyval: gtk::gdk::Key,
    modifiers: gtk::gdk::ModifierType,
) -> glib::Propagation {
    let kv = keyval.into_glib();
    if !matches_bind(kv, modifiers) {
        return glib::Propagation::Proceed;
    }
    // Dormant outside voice: let the bound key work as a normal shortcut.
    let Some((sess, active_cid)) = voice_holder() else {
        return glib::Propagation::Proceed;
    };
    // Edge-detect: auto-repeat fires this repeatedly while held — one
    // unmute per physical press. Still consume the repeat (the bind is ours).
    if state.pressed_keyval.get() != 0 {
        return glib::Propagation::Stop;
    }
    if drive_mute(sess, active_cid, false) {
        // Latch the specific keyval so the release re-mutes on it even if the
        // user rebinds PTT mid-press.
        state.pressed_keyval.set(kv);
    }
    // In voice: consume regardless of send success so the key doesn't
    // double-act as a chat-input character.
    glib::Propagation::Stop
}

fn on_key_released(state: &PttState, keyval: gtk::gdk::Key) {
    if state.pressed_keyval.get() == 0 {
        return; // never latched (different bind / not joined / drive failed)
    }
    // Match the LATCHED keyval (modifier state on release may differ), so a
    // mid-press rebind can't leave the user unmuted forever.
    if keyval.into_glib() != state.pressed_keyval.get() {
        return;
    }
    // Re-derive the active cid — the user may have switched rooms between
    // press and release; re-mute the room they're CURRENTLY in (skip if
    // they've left voice entirely — runtime teardown already muted them).
    if let Some((sess, active_cid)) = voice_holder() {
        drive_mute(sess, active_cid, true);
    }
    state.pressed_keyval.set(0);
}

/// `void hx_voice_ptt_attach(GtkWidget *window)` — attach the PTT key
/// controller to `window`. Idempotent (a second call is a no-op).
///
/// Takes no session, and that is the change: there is one microphone and one
/// toolbar window, so a controller latched to whichever session existed at
/// attach time would have gone on toggling that connection's mute while the
/// microphone was somewhere else. The key acts on whoever holds voice, which
/// the arbiter answers.
///
/// # Safety
/// `window` is a valid `GtkWidget *`. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_voice_ptt_attach(window: *mut gtk::ffi::GtkWidget) {
    if window.is_null() {
        return;
    }
    crate::ensure_gtk_init();
    // Borrow, don't own: the controller we add is held BY the window, and the
    // closures capture `state` (not the window), so nothing here retains it.
    // `from_glib_none` would sink a floating reference and finalize the window
    // when this wrapper drops; `from_glib_borrow` leaves its lifetime with C.
    let window = from_glib_borrow::<_, gtk::Widget>(window);

    // Idempotence sentinel — don't attach twice (reconnect path).
    if window.data::<bool>(ATTACHED_KEY).is_some() {
        return;
    }
    window.set_data(ATTACHED_KEY, true);

    let ctrl = gtk::EventControllerKey::new();
    // CAPTURE phase: PTT keys must be consumed before the chat input's own
    // key controller sees them. Non-matching keys return Proceed, so other
    // widgets' default handling proceeds normally.
    ctrl.set_propagation_phase(gtk::PropagationPhase::Capture);

    let state = Rc::new(PttState {
        pressed_keyval: Cell::new(0),
    });

    {
        let state = state.clone();
        ctrl.connect_key_pressed(move |_c, keyval, _keycode, modifiers| {
            on_key_pressed(&state, keyval, modifiers)
        });
    }
    {
        let state = state.clone();
        ctrl.connect_key_released(move |_c, keyval, _keycode, modifiers| {
            let _ = modifiers;
            on_key_released(&state, keyval);
        });
    }

    window.add_controller(ctrl);
}
