//! Per-chat voice toolbar (ported from `src/voice_panel.c`, Phase 8.D).
//!
//! A tight two-icon-button `GtkBox` (Join/Leave + Mute/Unmute) that sits
//! inline in the user-list action bar — one per surface that hosts a user
//! list (the standalone Users window's public room + each pchat sidebar).
//! Compiled only under the crate's `voice` feature (the whole module is
//! gated in lib.rs); the C callers that construct it (`chat.c`,
//! `users_bridge.c`, `gtkutil.c`) are themselves `#ifdef HAVE_VOICE`, so no
//! voice-off stub is needed.
//!
//! What stays C behind the FFI seam:
//!   - the wire senders `hx_send_voice_*` (voice.c),
//!   - the GStreamer runtime `gtkhx_voice_runtime_*` (hxvoice-runtime),
//!   - the voice presence model `hx_voice_model_*` (voice_model.c),
//!   - the session/htlc field access (`voice_bridge.c`),
//!   - the toast overlay + active-session lookup.
//!
//! The panel drives the runtime state machine and reflects its authoritative
//! state back through the state/mute/speaker/error signal callbacks; the
//! wire-frame bridge ships the runtime-originated 603/604/601 opcodes and
//! deliberately skips 600/606 (UI-driven — see `send_wire_frame_cb`).

use std::cell::{Cell, RefCell};
use std::ffi::{c_char, c_void};
use std::os::raw::c_int;
use std::rc::{Rc, Weak};
use std::time::Duration;

use gtk4 as gtk;
use gtk::glib;
use gtk::prelude::*;
use glib::translate::{from_glib_none, IntoGlibPtr};

use crate::tr::tr;

// ---------------------------------------------------------------------
// Wire opcodes / state enum / caps (mirror hotline.h + voice_runtime.h).
// ---------------------------------------------------------------------

const HTLC_HDR_VOICE_LEAVE: u32 = 0x0000_0259;
const HTLC_HDR_VOICE_SDP_ANSWER: u32 = 0x0000_025b;
const HTLC_HDR_VOICE_ICE: u32 = 0x0000_025c;

const STATE_JOIN_SENT: c_int = 1;
const STATE_OFFER_PENDING: c_int = 2;
const STATE_CONNECTING: c_int = 3;
const STATE_CONNECTED: c_int = 4;

/// Upper bound (ms) on GTKHX_VOICE_AUTOUNMUTE_MS so a negative / huge value
/// can't wrap the timeout.
const AUTOJOIN_UNMUTE_MAX_MS: i64 = 300_000;

/// gtk-rs qdata key for the panel's boxed `Rc<PanelInner>` state.
const STATE_KEY: &str = "voice-panel-state";

// ---------------------------------------------------------------------
// FFI seam.
// ---------------------------------------------------------------------

type SendWireFrameCb =
    unsafe extern "C" fn(*mut c_void, u32, *const u8, usize);

/// `#[repr(C)]` mirror of `gtkhx_voice_runtime_signal_callbacks`. Field order
/// pinned by voice_runtime.h; read once at construction.
#[repr(C)]
struct SignalCallbacks {
    state_changed: Option<unsafe extern "C" fn(*mut c_void, c_int)>,
    mute_changed: Option<unsafe extern "C" fn(*mut c_void, c_int)>,
    speaker_changed: Option<unsafe extern "C" fn(*mut c_void, u16, c_int)>,
    error: Option<unsafe extern "C" fn(*mut c_void, *const c_char)>,
}

extern "C" {
    // voice.c — wire senders.
    fn hx_send_voice_join(htlc: *mut c_void, cid: u32) -> glib::ffi::gboolean;
    fn hx_send_voice_leave(htlc: *mut c_void, cid: u32) -> glib::ffi::gboolean;
    fn hx_send_voice_sdp_answer(
        htlc: *mut c_void,
        cid: u32,
        sdp: *const u8,
        sdp_len: usize,
    ) -> glib::ffi::gboolean;
    fn hx_send_voice_ice(
        htlc: *mut c_void,
        cid: u32,
        ice: *const u8,
        ice_len: usize,
    ) -> glib::ffi::gboolean;
    fn hx_send_voice_mute(
        htlc: *mut c_void,
        cid: u32,
        muted: glib::ffi::gboolean,
    ) -> glib::ffi::gboolean;

    // hxvoice-runtime — per-session runtime.
    fn gtkhx_voice_runtime_new_v2(
        user_data: *mut c_void,
        send_wire_frame_cb: SendWireFrameCb,
        signals: *const SignalCallbacks,
    ) -> *mut c_void;
    fn gtkhx_voice_runtime_join(rt: *mut c_void, cid: u32);
    fn gtkhx_voice_runtime_leave(rt: *mut c_void, cid: u32);
    fn gtkhx_voice_runtime_mute(rt: *mut c_void, muted: c_int);
    fn gtkhx_voice_runtime_set_self_uid(rt: *mut c_void, uid: u16);
    fn gtkhx_voice_runtime_active_cid(rt: *mut c_void, out_cid: *mut u32) -> c_int;

    // voice_model.c.
    fn hx_voice_model_set_self_uid(model: *mut c_void, uid: u16);
    fn hx_voice_model_set_speaking(
        model: *mut c_void,
        uid: u16,
        speaking: glib::ffi::gboolean,
    );
    fn hx_voice_model_clear(model: *mut c_void);

    // voice_bridge.c — session/htlc field access.
    fn hx_session_voice_model(sess: *mut c_void) -> *mut c_void;
    fn hx_session_voice_runtime(sess: *mut c_void) -> *mut c_void;
    fn hx_session_set_voice_runtime(sess: *mut c_void, rt: *mut c_void);
    fn hx_session_htlc(sess: *mut c_void) -> *mut c_void;
    fn hx_htlc_voice_cap(htlc: *mut c_void) -> glib::ffi::gboolean;
    fn hx_htlc_voice_access(htlc: *mut c_void) -> glib::ffi::gboolean;
    fn hx_htlc_uid(htlc: *mut c_void) -> u16;

    // existing app symbols.
    fn hx_active_session() -> *mut c_void;
    fn toolbar_show_toast(text: *const c_char);
}

// ---------------------------------------------------------------------
// Per-panel state + live-panel registry.
// ---------------------------------------------------------------------

struct PanelInner {
    /// Borrowed session pointer (stable for the session lifetime).
    sess: *mut c_void,
    cid: u32,
    join_btn: gtk::ToggleButton,
    mute_btn: gtk::ToggleButton,
    joined: Cell<bool>,
    muted: Cell<bool>,
    /// Re-entrancy guard so a programmatic set_active inside our handler
    /// doesn't re-fire the toggled handler.
    suppress: Cell<bool>,
    // AUTOJOIN test-hook source ids (only set when GTKHX_VOICE_AUTOJOIN is on).
    autojoin_done: Cell<bool>,
    autojoin_poll: RefCell<Option<glib::SourceId>>,
    autojoin_unmute: RefCell<Option<glib::SourceId>>,
}

thread_local! {
    /// Every live voice panel (weak widget refs). The runtime signal
    /// callbacks reach panels through this; entries are pruned lazily on
    /// iterate and on the panel's `destroy`.
    static PANELS: RefCell<Vec<glib::WeakRef<gtk::Widget>>> = RefCell::new(Vec::new());
}

/// Recover a panel's `Rc<PanelInner>` from its widget qdata.
fn panel_state(w: &gtk::Widget) -> Option<Rc<PanelInner>> {
    unsafe { w.data::<Rc<PanelInner>>(STATE_KEY).map(|p| p.as_ref().clone()) }
}

/// Snapshot the live panels (upgrading + pruning dead weaks), then run `f`
/// against each panel widget + its state. Snapshotting first keeps the
/// registry borrow off the stack while `f` runs.
fn for_each_panel(mut f: impl FnMut(&gtk::Widget, &Rc<PanelInner>)) {
    let live: Vec<gtk::Widget> = PANELS.with(|p| {
        let mut v = p.borrow_mut();
        v.retain(|w| w.upgrade().is_some());
        v.iter().filter_map(|w| w.upgrade()).collect()
    });
    for w in &live {
        if let Some(inner) = panel_state(w) {
            f(w, &inner);
        }
    }
}

fn gbool(b: bool) -> glib::ffi::gboolean {
    if b {
        glib::ffi::GTRUE
    } else {
        glib::ffi::GFALSE
    }
}

// ---------------------------------------------------------------------
// Runtime lifecycle + wire-frame / signal bridges.
// ---------------------------------------------------------------------

/// Lazy-create the per-session runtime on first use, registering the wire-out
/// + signal bridges. Returns the runtime (NULL on construction failure —
/// GStreamer not initialised / webrtcbin missing). Idempotent.
unsafe fn ensure_voice_runtime(sess: *mut c_void) -> *mut c_void {
    if sess.is_null() {
        return std::ptr::null_mut();
    }
    let mut rt = hx_session_voice_runtime(sess);
    if rt.is_null() {
        let signals = SignalCallbacks {
            state_changed: Some(state_changed_cb),
            mute_changed: Some(mute_changed_cb),
            speaker_changed: Some(speaker_changed_cb),
            error: Some(error_cb),
        };
        // user_data = &sess->htlc; the signal handlers reach sess back via
        // hx_active_session().
        rt = gtkhx_voice_runtime_new_v2(
            hx_session_htlc(sess),
            send_wire_frame_cb,
            &signals,
        );
        hx_session_set_voice_runtime(sess, rt);
    }
    rt
}

/// Bridge `Action::SendWireFrame` → the C `hx_send_voice_*` helpers.
/// `user_data` is the `htlc_conn *`. Ships the runtime-originated opcodes
/// (603 SDP_ANSWER, 604 ICE, 601 LEAVE); 600 JOIN + 606 MUTE are UI-driven
/// and skipped here to avoid double-send (see the module doc + voice_panel.c
/// history).
unsafe extern "C" fn send_wire_frame_cb(
    user_data: *mut c_void,
    opcode: u32,
    body: *const u8,
    body_len: usize,
) {
    let htlc = user_data;
    if htlc.is_null() || body.is_null() || body_len < 4 {
        return;
    }
    let bytes = std::slice::from_raw_parts(body, body_len);
    // All four encodings start with a 4-byte BE cid.
    let cid = u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
    let payload = &bytes[4..];

    match opcode {
        HTLC_HDR_VOICE_SDP_ANSWER => {
            if payload.is_empty() {
                return;
            }
            if hx_send_voice_sdp_answer(htlc, cid, payload.as_ptr(), payload.len()) == 0 {
                glib::g_debug!(
                    "gtkhx",
                    "voice bridge: sdp_answer FAILED cid={cid} len={}",
                    payload.len()
                );
            }
        }
        HTLC_HDR_VOICE_ICE => {
            // Empty ICE body = end-of-candidates marker (NULL/0 to the helper).
            let sent = if payload.is_empty() {
                hx_send_voice_ice(htlc, cid, std::ptr::null(), 0)
            } else {
                hx_send_voice_ice(htlc, cid, payload.as_ptr(), payload.len())
            };
            if sent == 0 {
                glib::g_debug!("gtkhx", "voice bridge: ice FAILED cid={cid}");
            }
        }
        HTLC_HDR_VOICE_LEAVE => {
            if hx_send_voice_leave(htlc, cid) == 0 {
                glib::g_debug!("gtkhx", "voice bridge: leave FAILED cid={cid}");
            }
        }
        // 600 JOIN / 606 MUTE handled by the UI click handlers; other opcodes
        // never reach this bridge.
        _ => {}
    }
}

/// Anything past Idle (except the terminal Leaving) reads as "in voice".
fn state_is_joined(state: c_int) -> bool {
    matches!(
        state,
        STATE_JOIN_SENT | STATE_OFFER_PENDING | STATE_CONNECTING | STATE_CONNECTED
    )
}

unsafe extern "C" fn state_changed_cb(_user_data: *mut c_void, state: c_int) {
    let sess = hx_active_session();
    let joined_now = state_is_joined(state);

    let mut active_cid = 0u32;
    let mut has_active = 0;
    let rt = hx_session_voice_runtime(sess);
    if !rt.is_null() {
        has_active = gtkhx_voice_runtime_active_cid(rt, &mut active_cid);
    }

    for_each_panel(|_w, inner| {
        let is_active = has_active != 0 && inner.cid == active_cid;
        inner.joined.set(joined_now && is_active);
        // Clear stale muted on a now-inactive / left panel so a later Join
        // doesn't inherit a stale muted icon.
        if !joined_now || !is_active {
            inner.muted.set(false);
        }
        update_button_labels(inner);
    });

    // Left the room → the speaker indicators we've been showing are now
    // lying (no more 605 updates). Blank the model synchronously.
    if !joined_now {
        let model = hx_session_voice_model(sess);
        if !model.is_null() {
            hx_voice_model_clear(model);
        }
    }
}

unsafe extern "C" fn mute_changed_cb(_user_data: *mut c_void, muted: c_int) {
    let sess = hx_active_session();
    let rt = hx_session_voice_runtime(sess);
    if rt.is_null() {
        return;
    }
    let mut active_cid = 0u32;
    if gtkhx_voice_runtime_active_cid(rt, &mut active_cid) == 0 {
        return;
    }
    for_each_panel(|_w, inner| {
        if inner.cid == active_cid {
            inner.muted.set(muted != 0);
            update_button_labels(inner);
        }
    });
}

unsafe extern "C" fn speaker_changed_cb(
    _user_data: *mut c_void,
    uid: u16,
    is_speaking: c_int,
) {
    let sess = hx_active_session();
    let model = hx_session_voice_model(sess);
    if model.is_null() {
        return;
    }
    hx_voice_model_set_speaking(model, uid, gbool(is_speaking != 0));
}

unsafe extern "C" fn error_cb(_user_data: *mut c_void, text: *const c_char) {
    if text.is_null() {
        return;
    }
    toolbar_show_toast(text);
}

// ---------------------------------------------------------------------
// Button label / state application.
// ---------------------------------------------------------------------

fn update_button_labels(inner: &PanelInner) {
    let joined = inner.joined.get();
    let muted = inner.muted.get();
    let access_ok = !inner.sess.is_null()
        && unsafe { hx_htlc_voice_access(hx_session_htlc(inner.sess)) != 0 };

    // Join ↔ Leave (icon-only; the tooltip carries the words).
    inner.join_btn.set_icon_name(if joined {
        "call-stop-symbolic"
    } else {
        "call-start-symbolic"
    });
    inner.join_btn.set_tooltip_text(Some(&if !access_ok {
        tr("Voice chat requires permission")
    } else if joined {
        tr("Leave the voice room")
    } else {
        tr("Join the voice room for this chat")
    }));
    inner.suppress.set(true);
    inner.join_btn.set_active(joined);
    inner.suppress.set(false);

    // Mute is only relevant when joined AND access is granted.
    inner.mute_btn.set_sensitive(joined && access_ok);
    inner.mute_btn.set_icon_name(if muted {
        "microphone-sensitivity-muted-symbolic"
    } else {
        "audio-input-microphone-symbolic"
    });
    inner.mute_btn.set_tooltip_text(Some(&if muted {
        tr("Restore your microphone")
    } else {
        tr("Stop sending audio")
    }));
    inner.suppress.set(true);
    inner.mute_btn.set_active(muted);
    inner.suppress.set(false);
}

fn do_refresh(widget: &gtk::Widget, inner: &PanelInner, sess: *mut c_void) {
    if sess.is_null() {
        return;
    }
    let htlc = unsafe { hx_session_htlc(sess) };
    let show = unsafe { hx_htlc_voice_cap(htlc) != 0 };
    if !show {
        // Server didn't echo CAP_VOICE (or the session dropped). Reset
        // joined/muted so a reconnect to a different server doesn't carry
        // stale UI across the persistent chat tab.
        inner.joined.set(false);
        inner.muted.set(false);
        update_button_labels(inner);
        widget.set_visible(false);
        return;
    }
    widget.set_visible(true);
    // Only the Join button's sensitivity lives here; the Mute button's is
    // computed in update_button_labels (joined && access) to avoid a race.
    let enabled = unsafe { hx_htlc_voice_access(htlc) != 0 };
    inner.join_btn.set_sensitive(enabled);
    update_button_labels(inner);
}

// ---------------------------------------------------------------------
// Toggle handlers.
// ---------------------------------------------------------------------

fn on_join_toggled(inner: &PanelInner) {
    if inner.suppress.get() {
        return;
    }
    let sess = inner.sess;
    if sess.is_null() {
        return;
    }
    let cid = inner.cid;
    let want_joined = inner.join_btn.is_active();
    let htlc = unsafe { hx_session_htlc(sess) };

    let mut sent;
    if want_joined {
        sent = unsafe { hx_send_voice_join(htlc, cid) != 0 };
        if sent {
            let rt = unsafe { ensure_voice_runtime(sess) };
            if !rt.is_null() {
                unsafe {
                    // Our own uid: send-leg VAD lights our own indicator, and
                    // the model excludes our own presence sounds.
                    let uid = hx_htlc_uid(htlc);
                    gtkhx_voice_runtime_set_self_uid(rt, uid);
                    let model = hx_session_voice_model(sess);
                    if !model.is_null() {
                        hx_voice_model_set_self_uid(model, uid);
                    }
                    gtkhx_voice_runtime_join(rt, cid);
                    // Join muted by default. The explicit 606 MUST be the UI's
                    // job — the wire-frame bridge skips runtime-emitted MUTE —
                    // then drive the state machine to keep it in sync.
                    hx_send_voice_mute(htlc, cid, glib::ffi::GTRUE);
                    gtkhx_voice_runtime_mute(rt, 1);
                }
            } else {
                // Runtime construction failed — roll back the wire join.
                unsafe { hx_send_voice_leave(htlc, cid) };
                sent = false;
            }
        }
    } else {
        // Runtime-driven LEAVE: the state machine's LeaveRequested arm emits
        // the 601 wire frame through send_wire_frame_cb. Defensive NULL guard.
        let rt = unsafe { hx_session_voice_runtime(sess) };
        if !rt.is_null() {
            unsafe { gtkhx_voice_runtime_leave(rt, cid) };
        }
        sent = true;
    }

    if !sent {
        // Wire-out skipped — revert the toggle to match the underlying state.
        inner.suppress.set(true);
        inner.join_btn.set_active(!want_joined);
        inner.suppress.set(false);
        update_button_labels(inner);
    }
}

fn on_mute_toggled(inner: &PanelInner) {
    if inner.suppress.get() {
        return;
    }
    let sess = inner.sess;
    if sess.is_null() {
        return;
    }
    let cid = inner.cid;
    let want_muted = inner.mute_btn.is_active();
    let htlc = unsafe { hx_session_htlc(sess) };

    let sent = unsafe { hx_send_voice_mute(htlc, cid, gbool(want_muted)) != 0 };
    if sent {
        // Drive the runtime; MuteChanged updates KEY_MUTED synchronously.
        let rt = unsafe { hx_session_voice_runtime(sess) };
        unsafe { gtkhx_voice_runtime_mute(rt, if want_muted { 1 } else { 0 }) };
    } else {
        inner.suppress.set(true);
        inner.mute_btn.set_active(!want_muted);
        inner.suppress.set(false);
        update_button_labels(inner);
    }
}

// ---------------------------------------------------------------------
// AUTOJOIN headless test hook (no-op unless GTKHX_VOICE_AUTOJOIN is set).
// ---------------------------------------------------------------------

fn autojoin_unmute(w: &Weak<PanelInner>) -> glib::ControlFlow {
    if let Some(inner) = w.upgrade() {
        // Self-removing: drop the stored id first (dropping a SourceId does
        // NOT remove the source) so destroy won't later .remove() a
        // GLib-recycled id.
        *inner.autojoin_unmute.borrow_mut() = None;
        if !inner.sess.is_null() {
            let rt = unsafe { hx_session_voice_runtime(inner.sess) };
            if !rt.is_null() {
                let htlc = unsafe { hx_session_htlc(inner.sess) };
                unsafe {
                    hx_send_voice_mute(htlc, inner.cid, glib::ffi::GFALSE);
                    gtkhx_voice_runtime_mute(rt, 0);
                }
                glib::g_debug!("gtkhx", "AUTOJOIN: unmuted (cid={})", inner.cid);
            }
        }
    }
    glib::ControlFlow::Break
}

fn autojoin_poll(w: &Weak<PanelInner>) -> glib::ControlFlow {
    let Some(inner) = w.upgrade() else {
        return glib::ControlFlow::Break;
    };
    if inner.autojoin_done.get() {
        *inner.autojoin_poll.borrow_mut() = None;
        return glib::ControlFlow::Break;
    }
    if inner.sess.is_null() {
        return glib::ControlFlow::Continue;
    }
    // Hold until the server echoed voice support + the Join button is live.
    let htlc = unsafe { hx_session_htlc(inner.sess) };
    if unsafe { hx_htlc_voice_access(htlc) == 0 } || !inner.join_btn.is_sensitive() {
        return glib::ControlFlow::Continue;
    }
    inner.autojoin_done.set(true);
    glib::g_debug!("gtkhx", "AUTOJOIN: activating Join button");
    inner.join_btn.set_active(true);

    let mut unmute_ms: i64 = 4000;
    if let Ok(s) = std::env::var("GTKHX_VOICE_AUTOUNMUTE_MS") {
        if let Ok(v) = s.trim().parse::<i64>() {
            unmute_ms = v;
        }
    }
    let unmute_ms = unmute_ms.clamp(0, AUTOJOIN_UNMUTE_MAX_MS);
    let w2 = w.clone();
    let id = glib::timeout_add_local(Duration::from_millis(unmute_ms as u64), move || {
        autojoin_unmute(&w2)
    });
    *inner.autojoin_unmute.borrow_mut() = Some(id);
    // Poll is self-removing; drop its stored id so destroy won't touch it.
    *inner.autojoin_poll.borrow_mut() = None;
    glib::ControlFlow::Break
}

// ---------------------------------------------------------------------
// Public C ABI.
// ---------------------------------------------------------------------

/// `GtkWidget *voice_panel_new(session *sess, guint32 cid)`.
///
/// # Safety
/// `sess` is a valid session pointer; called on the GTK main thread. The
/// returned widget carries a floating reference (the caller's
/// `gtk_box_append` / `gtk_widget_set_parent` sinks it), matching the C
/// constructor convention.
#[no_mangle]
pub unsafe extern "C" fn voice_panel_new(
    sess: *mut c_void,
    cid: u32,
) -> *mut gtk::ffi::GtkWidget {
    crate::ensure_gtk_init();

    let panel = gtk::Box::new(gtk::Orientation::Horizontal, 2);
    let join_btn = gtk::ToggleButton::new();
    let mute_btn = gtk::ToggleButton::new();
    panel.append(&join_btn);
    panel.append(&mute_btn);

    let inner = Rc::new(PanelInner {
        sess,
        cid,
        join_btn: join_btn.clone(),
        mute_btn: mute_btn.clone(),
        joined: Cell::new(false),
        muted: Cell::new(false),
        suppress: Cell::new(false),
        autojoin_done: Cell::new(false),
        autojoin_poll: RefCell::new(None),
        autojoin_unmute: RefCell::new(None),
    });

    // Toggle handlers capture a Weak to avoid a button→closure→inner→button
    // reference cycle.
    {
        let w = Rc::downgrade(&inner);
        join_btn.connect_toggled(move |_| {
            if let Some(i) = w.upgrade() {
                on_join_toggled(&i);
            }
        });
    }
    {
        let w = Rc::downgrade(&inner);
        mute_btn.connect_toggled(move |_| {
            if let Some(i) = w.upgrade() {
                on_mute_toggled(&i);
            }
        });
    }

    let widget: gtk::Widget = panel.clone().upcast();
    // Store state on the widget (dropped at finalize).
    panel.set_data(STATE_KEY, inner.clone());

    // Initial state, then visibility/enabled for the current htlc.
    update_button_labels(&inner);
    do_refresh(&widget, &inner, sess);

    // Register for signal-driven updates; deregister on destroy.
    PANELS.with(|p| p.borrow_mut().push(widget.downgrade()));
    {
        let w = Rc::downgrade(&inner);
        panel.connect_destroy(move |dying| {
            if let Some(i) = w.upgrade() {
                if let Some(id) = i.autojoin_poll.borrow_mut().take() {
                    id.remove();
                }
                if let Some(id) = i.autojoin_unmute.borrow_mut().take() {
                    id.remove();
                }
            }
            // Explicitly drop THIS panel from the registry (and prune dead
            // weaks). "destroy" fires during dispose — before finalize — so a
            // weak upgrade still succeeds here; pruning only dead refs would
            // leave the just-destroyed widget in the registry, and a runtime
            // signal callback could still iterate it and call
            // update_button_labels on a torn-down subtree (GTK criticals).
            // Match by pointer identity to remove exactly this one.
            // Same GObject address; cast GtkBox* → GtkWidget* to compare
            // against the registry's WeakRef<Widget> upgrades.
            let dying_ptr = dying.as_ptr() as *mut gtk::ffi::GtkWidget;
            PANELS.with(|p| {
                p.borrow_mut()
                    .retain(|x| x.upgrade().is_some_and(|up| up.as_ptr() != dying_ptr));
            });
        });
    }

    // Headless test hook.
    if std::env::var_os("GTKHX_VOICE_AUTOJOIN").is_some() {
        let w = Rc::downgrade(&inner);
        let id = glib::timeout_add_local(Duration::from_millis(500), move || autojoin_poll(&w));
        *inner.autojoin_poll.borrow_mut() = Some(id);
    }

    // Hand C a floating ref: gtk-rs sank the box's floating ref at
    // construction (the wrapper owns a hard ref), so transfer that ref via
    // into_glib_ptr and re-float it — the caller's ref_sink then balances to
    // a single parent-owned ref instead of leaking one.
    let ptr = panel.upcast::<gtk::Widget>().into_glib_ptr();
    glib::gobject_ffi::g_object_force_floating(ptr as *mut glib::gobject_ffi::GObject);
    ptr
}

/// `void voice_panel_refresh(GtkWidget *panel, session *sess)`.
///
/// # Safety
/// `panel` is NULL or a valid voice-panel widget; `sess` NULL or valid.
#[no_mangle]
pub unsafe extern "C" fn voice_panel_refresh(
    panel: *mut gtk::ffi::GtkWidget,
    sess: *mut c_void,
) {
    if panel.is_null() || sess.is_null() {
        return;
    }
    let widget: gtk::Widget = from_glib_none(panel);
    if let Some(inner) = panel_state(&widget) {
        do_refresh(&widget, &inner, sess);
    }
}

/// `void voice_panel_refresh_all_chats(session *sess)` — refresh every live
/// panel bound to `sess` (public-room controls in the Users window + every
/// pchat sidebar).
///
/// # Safety
/// `sess` is NULL or a valid session pointer.
#[no_mangle]
pub unsafe extern "C" fn voice_panel_refresh_all_chats(sess: *mut c_void) {
    if sess.is_null() {
        return;
    }
    for_each_panel(|w, inner| {
        if inner.sess == sess {
            do_refresh(w, inner, sess);
        }
    });
}

/// `void voice_panel_set_joined(GtkWidget *panel, gboolean joined)`.
///
/// # Safety
/// `panel` is NULL or a valid voice-panel widget.
#[no_mangle]
pub unsafe extern "C" fn voice_panel_set_joined(
    panel: *mut gtk::ffi::GtkWidget,
    joined: glib::ffi::gboolean,
) {
    if panel.is_null() {
        return;
    }
    let widget: gtk::Widget = from_glib_none(panel);
    if let Some(inner) = panel_state(&widget) {
        inner.joined.set(joined != 0);
        update_button_labels(&inner);
    }
}

/// `void voice_panel_set_muted(GtkWidget *panel, gboolean muted)`.
///
/// # Safety
/// `panel` is NULL or a valid voice-panel widget.
#[no_mangle]
pub unsafe extern "C" fn voice_panel_set_muted(
    panel: *mut gtk::ffi::GtkWidget,
    muted: glib::ffi::gboolean,
) {
    if panel.is_null() {
        return;
    }
    let widget: gtk::Widget = from_glib_none(panel);
    if let Some(inner) = panel_state(&widget) {
        inner.muted.set(muted != 0);
        update_button_labels(&inner);
    }
}
