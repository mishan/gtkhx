//! Phase R4 — `GtkhxSession` as a Rust `glib::subclass`-derived GObject.
//!
//! This crate replaces `src/gtkhx_session.c`. It is a *drop-in*: the C
//! header `src/gtkhx_session.h` stays exactly as it was, and every
//! symbol the old C TU exported is re-exported here with the identical
//! C ABI —
//!
//!   - `gtkhx_session_get_type` (the `G_DECLARE_FINAL_TYPE` accessor),
//!   - `gtkhx_session_get_default` (the process-lifetime singleton),
//!   - one `gtkhx_session_emit_*` wrapper per signal.
//!
//! C view-side code (`gtkhx.c::gtkhx_connect_signals` and the
//! `on_<name>_signal` adapters, plus every model-side `emit_*` caller)
//! compiles and links unchanged, because GObject signal dispatch is
//! transparent to the consumer — it never sees whether the emitting
//! type was defined in C or Rust.
//!
//! # Boxed-type payloads
//!
//! Three signals carry boxed payloads — `chat` (`HxChatEvent`), `msg`
//! (`HxMsgEvent`), and `tracker-server-create` (`HxTrackerServer`). As of
//! Phase R4.2 those boxed types live in Rust in the sibling `gtkhx-boxed`
//! crate; this crate references their `GType`s through the same
//! `hx_*_get_type()` C-ABI accessors as before (now resolving against
//! `gtkhx-boxed` instead of `proto_helpers.c` / `tracker_event.c`). We go
//! through the extern accessors rather than a direct Rust dependency on
//! `gtkhx-boxed` on purpose: a `staticlib` crate bundles its rlib deps,
//! so depending on `gtkhx-boxed` here would emit the boxed `_get_type` /
//! `_copy` / `_free` symbols into *both* archives and collide at final
//! link. The extern keeps a single definition. The signal-emit marshaling
//! uses `g_value_set_boxed`, which copies the payload via the boxed type's
//! copy func for the duration of the emission — byte-for-byte the same
//! lifetime the old `g_signal_emit(self, sig, 0, …, event)` varargs
//! collection produced (boxed params without `G_SIGNAL_TYPE_STATIC_SCOPE`
//! are copied at collect time).
//!
//! # Lifetime model
//!
//! Emit wraps the incoming `GtkhxSession*` with `from_glib_none` (one
//! ref taken, dropped when the Rust handle leaves scope). This is the
//! re-entrancy-safe "full form" from `docs/rust/glib-interop.md`: a
//! connected handler may `g_object_unref` the session synchronously
//! inside the emit, and the wrap-ref keeps it alive until the emit
//! returns. The singleton itself holds a permanent ref (the old C code
//! leaked one too — `gtkhx_session_get_default` never unrefs), so in
//! practice the session never reaches refcount 0, but the defensive
//! ref keeps the invariant local and matches the project convention.

// The per-session cid → conversation registry (was `session->chats`, a
// GHashTable). Type-agnostic + GTK-free so it can live in this crate — the one
// the headless wire-level tests link — rather than gtkhx-ui.
pub mod chat_registry;

use glib::prelude::*;
use glib::translate::{from_glib, FromGlibPtrNone, IntoGlib, ToGlibPtrMut};
use std::ffi::{c_char, c_int, c_void, CStr};
use std::sync::OnceLock;

// ----------------------------------------------------------------------
// Boxed-type GType accessors (defined in the gtkhx-boxed crate as of
// R4.2; resolved here via their C ABI — see the crate-level note on why
// we extern rather than take a Rust dependency on gtkhx-boxed).
//
// `signals()` must reference these GTypes when it registers the boxed
// payload signals, and calling the accessor *forces* the boxed type to
// register if it hasn't yet — `g_type_from_name` would return 0 for a
// not-yet-registered type, so we must go through the real accessor.
// ----------------------------------------------------------------------

#[cfg(not(test))]
use gtkhx_boxed::chat::hx_chat_event_get_type;
#[cfg(not(test))]
use gtkhx_boxed::msg::hx_msg_event_get_type;
#[cfg(not(test))]
use gtkhx_boxed::tracker::hx_tracker_server_get_type;

// Under `cargo test` there is no other archive to link against, so stub
// the three accessors with real Rust-registered boxed types. This lets
// the in-crate tests exercise the full registration + boxed-emit path
// (the standalone cargo test binary can't resolve the external symbols
// otherwise — it doesn't link gtkhx-boxed).
#[cfg(test)]
use test_boxed_stubs::{
    hx_chat_event_get_type, hx_msg_event_get_type, hx_tracker_server_get_type,
};

#[inline]
fn chat_event_type() -> glib::Type {
    unsafe { from_glib(hx_chat_event_get_type()) }
}

#[inline]
fn msg_event_type() -> glib::Type {
    unsafe { from_glib(hx_msg_event_get_type()) }
}

#[inline]
fn tracker_server_type() -> glib::Type {
    unsafe { from_glib(hx_tracker_server_get_type()) }
}

// ----------------------------------------------------------------------
// GObject subclass
// ----------------------------------------------------------------------

mod imp {
    use super::*;
    use glib::subclass::prelude::*;
    use glib::subclass::Signal;
    use glib::Type;

    #[derive(Default)]
    pub struct GtkhxSession;

    #[glib::object_subclass]
    impl ObjectSubclass for GtkhxSession {
        // Must match the GType name the rest of GLib expects for this
        // singleton. The C side never looked the type up by name (it
        // used the get_type() accessor directly), so this is just the
        // canonical registration name.
        const NAME: &'static str = "GtkhxSession";
        type Type = super::GtkhxSession;
        type ParentType = glib::Object;
    }

    impl ObjectImpl for GtkhxSession {
        fn signals() -> &'static [Signal] {
            static SIGNALS: OnceLock<Vec<Signal>> = OnceLock::new();
            // Registration order mirrors the SIGNAL_* enum in the old
            // gtkhx_session.c so that, should anyone ever rely on signal
            // IDs, they stay stable across the port. Param types are an
            // exact transcription of each g_signal_new() call there.
            //
            // Note: most "string" args ride as G_TYPE_POINTER (a raw
            // `const char *` the handler casts back), NOT G_TYPE_STRING.
            // The one true G_TYPE_STRING is tracker-batch-begin's URL.
            SIGNALS.get_or_init(|| {
                vec![
                    // chat: (htlc*, HxChatEvent* boxed)
                    Signal::builder("chat")
                        .param_types([Type::POINTER, chat_event_type()])
                        .build(),
                    // chat-subject: (htlc*, cid, subject*)
                    Signal::builder("chat-subject")
                        .param_types([Type::POINTER, Type::U32, Type::POINTER])
                        .build(),
                    // chat-subject-notice: (htlc*, cid, subject*) — the
                    // "Subject Changed to: X" log line for a real change (the
                    // change-gate already fired the chat-subject bar update).
                    Signal::builder("chat-subject-notice")
                        .param_types([Type::POINTER, Type::U32, Type::POINTER])
                        .build(),
                    // chat-invitation: (htlc*, cid, name*)
                    Signal::builder("chat-invitation")
                        .param_types([Type::POINTER, Type::U32, Type::POINTER])
                        .build(),
                    // chat-history-batch: (htlc*, cid, GPtrArray*, has_more)
                    Signal::builder("chat-history-batch")
                        .param_types([
                            Type::POINTER,
                            Type::U32,
                            Type::POINTER,
                            Type::BOOL,
                        ])
                        .build(),
                    // gif-icon-changed: (htlc*, uid) — GIF-icons extension
                    // (Phase 10). The ICON_CHANGE broadcast carries only the
                    // uid; a view re-fetches via hx_icon_get().
                    Signal::builder("gif-icon-changed")
                        .param_types([Type::POINTER, Type::U32])
                        .build(),
                    // gif-icon-data: (htlc*, uid, gif*, len) — raw GIF bytes
                    // from an ICON_GET reply / ICON_GETLIST entry. The gif
                    // pointer rides as G_TYPE_POINTER and is valid only for
                    // the emit; len == 0 means the avatar was cleared.
                    Signal::builder("gif-icon-data")
                        .param_types([
                            Type::POINTER,
                            Type::U32,
                            Type::POINTER,
                            Type::U32,
                        ])
                        .build(),
                    // msg: (HxMsgEvent* boxed)
                    Signal::builder("msg")
                        .param_types([msg_event_type()])
                        .build(),
                    // logged-in: (htlc*) — login task reply came back
                    // successful; the login chime and any future
                    // login-reaction consumer subscribe here.
                    Signal::builder("logged-in")
                        .param_types([Type::POINTER])
                        .build(),
                    // self-updated: (htlc*) — our own access bits / uid were
                    // (re)parsed from a SELFINFO reply. Toolbar-button
                    // sensitivity depends on the access bitmap, so the view
                    // refreshes it here.
                    Signal::builder("self-updated")
                        .param_types([Type::POINTER])
                        .build(),
                    // agreement: (session*, agreement*, len)
                    Signal::builder("agreement")
                        .param_types([Type::POINTER, Type::POINTER, Type::U32])
                        .build(),
                    // news-file: (htlc*, news*, len)
                    Signal::builder("news-file")
                        .param_types([Type::POINTER, Type::POINTER, Type::U32])
                        .build(),
                    // news-post: (htlc*, news*, len)
                    Signal::builder("news-post")
                        .param_types([Type::POINTER, Type::POINTER, Type::U32])
                        .build(),
                    // news-folder: (gnews_folder*)
                    Signal::builder("news-folder")
                        .param_types([Type::POINTER])
                        .build(),
                    // news-catalog: (gnews_catalog*)
                    Signal::builder("news-catalog")
                        .param_types([Type::POINTER])
                        .build(),
                    // news-thread: (news_post*)
                    Signal::builder("news-thread")
                        .param_types([Type::POINTER])
                        .build(),
                    // user-create: (htlc*, chat*, uid, nick_color, nam*, icon,
                    // color, incremental). `incremental` is TRUE for a genuine
                    // join broadcast and FALSE for a row synthesised during
                    // the bulk user-list load — sound/notification consumers
                    // gate on it so the join chime only fires on real joins,
                    // not once per user already in the room at login.
                    Signal::builder("user-create")
                        .param_types([
                            Type::POINTER,
                            Type::POINTER,
                            Type::U32,
                            Type::U32,
                            Type::POINTER,
                            Type::U32,
                            Type::U32,
                            Type::BOOL,
                        ])
                        .build(),
                    // user-delete: (htlc*, chat*, uid, incremental).
                    // `incremental` mirrors user-create — TRUE for a real
                    // part broadcast, FALSE for a teardown-driven removal.
                    Signal::builder("user-delete")
                        .param_types([
                            Type::POINTER,
                            Type::POINTER,
                            Type::U32,
                            Type::BOOL,
                        ])
                        .build(),
                    // user-change: (htlc*, chat*, uid, nick_color, nam*, icon, color)
                    Signal::builder("user-change")
                        .param_types([
                            Type::POINTER,
                            Type::POINTER,
                            Type::U32,
                            Type::U32,
                            Type::POINTER,
                            Type::U32,
                            Type::U32,
                        ])
                        .build(),
                    // users-clear: (htlc*, chat*)
                    Signal::builder("users-clear")
                        .param_types([Type::POINTER, Type::POINTER])
                        .build(),
                    // user-info: (uid, nam*, info*, len)
                    Signal::builder("user-info")
                        .param_types([
                            Type::U32,
                            Type::POINTER,
                            Type::POINTER,
                            Type::U32,
                        ])
                        .build(),
                    // file-info: (path*, name*, creator*, type*, comments*,
                    //             date_modify* (8 raw wire bytes), date_create*
                    //             (8 raw wire bytes), size:u64). The date stamps
                    //             ride raw so the view formats them for display
                    //             (model doesn't do locale date formatting).
                    Signal::builder("file-info")
                        .param_types([
                            Type::POINTER,
                            Type::POINTER,
                            Type::POINTER,
                            Type::POINTER,
                            Type::POINTER,
                            Type::POINTER,
                            Type::POINTER,
                            Type::U64,
                        ])
                        .build(),
                    // file-list: (cfl*, fh*, data*)
                    Signal::builder("file-list")
                        .param_types([Type::POINTER, Type::POINTER, Type::POINTER])
                        .build(),
                    // file-update: (session*, htxf*)
                    Signal::builder("file-update")
                        .param_types([Type::POINTER, Type::POINTER])
                        .build(),
                    // xfer-queue: (session*, htxf*)
                    Signal::builder("xfer-queue")
                        .param_types([Type::POINTER, Type::POINTER])
                        .build(),
                    // xfer-destroyed: (session*, htxf*)
                    Signal::builder("xfer-destroyed")
                        .param_types([Type::POINTER, Type::POINTER])
                        .build(),
                    // tracker-server-create: (HxTrackerServer* boxed)
                    Signal::builder("tracker-server-create")
                        .param_types([tracker_server_type()])
                        .build(),
                    // tracker-batch-begin: (url:string, version:u8, count:u32)
                    Signal::builder("tracker-batch-begin")
                        .param_types([Type::STRING, Type::U8, Type::U32])
                        .build(),
                    // task-update: (session*, task*)
                    Signal::builder("task-update")
                        .param_types([Type::POINTER, Type::POINTER])
                        .build(),
                    // chat-log-line: (htlc*, cid, body*)
                    Signal::builder("chat-log-line")
                        .param_types([Type::POINTER, Type::U32, Type::POINTER])
                        .build(),
                    // user-notice: (htlc*, cid, kind, name*, old_name*) — a
                    // roster notice line (join / parts / rename). The view
                    // handler applies the showjoin pref + gettext.
                    Signal::builder("user-notice")
                        .param_types([
                            Type::POINTER,
                            Type::U32,
                            Type::U32,
                            Type::POINTER,
                            Type::POINTER,
                        ])
                        .build(),
                    // connection-state-changed: (state:u32)
                    Signal::builder("connection-state-changed")
                        .param_types([Type::U32])
                        .build(),
                ]
            })
        }
    }
}

glib::wrapper! {
    /// The model→view signal hub. A process singleton; see
    /// [`gtkhx_session_get_default`].
    pub struct GtkhxSession(ObjectSubclass<imp::GtkhxSession>);
}

impl Default for GtkhxSession {
    fn default() -> Self {
        glib::Object::new::<Self>()
    }
}

// ----------------------------------------------------------------------
// Value-construction helpers
// ----------------------------------------------------------------------

/// A `G_TYPE_POINTER`-typed `Value` holding a raw pointer. Borrow
/// semantics: the Value box is freed on drop, the pointee is not
/// touched. Matches how the old C code passed `const char *` /
/// `struct foo *` args through `g_signal_emit`.
fn ptr_value(p: *const c_void) -> glib::Value {
    let mut v = unsafe { glib::Value::from_type_unchecked(glib::Type::POINTER) };
    unsafe {
        glib::gobject_ffi::g_value_set_pointer(v.to_glib_none_mut().0, p as *mut c_void);
    }
    v
}

/// A boxed-typed `Value` holding `p`. `g_value_set_boxed` *copies* the
/// payload via the boxed type's copy func; the copy is freed when the
/// Value drops (after the emit). This is the exact lifetime the old
/// `g_signal_emit` varargs collection produced for a non-static-scope
/// boxed param.
///
/// # Safety
/// `gtype` must be a registered boxed `GType` and `p` a valid instance
/// of it (or NULL — `g_value_set_boxed(NULL)` is well-defined and
/// stores NULL).
unsafe fn boxed_value(gtype: glib::ffi::GType, p: *mut c_void) -> glib::Value {
    let ty: glib::Type = from_glib(gtype);
    let mut v = glib::Value::from_type_unchecked(ty);
    glib::gobject_ffi::g_value_set_boxed(v.to_glib_none_mut().0, p);
    v
}

/// Emit `name` on the C-owned session pointer with `values`. Wraps the
/// pointer in the re-entrancy-safe full form (one ref for the duration
/// of the synchronous emit).
///
/// # Safety
/// `self_ptr` must be a valid non-NULL `GtkhxSession*`.
unsafe fn emit(self_ptr: *mut c_void, name: &str, values: &[glib::Value]) {
    if self_ptr.is_null() {
        glib::g_critical!("gtkhx-session", "{name}: NULL session pointer; emit skipped");
        return;
    }
    let obj: glib::Object =
        glib::Object::from_glib_none(self_ptr as *mut glib::gobject_ffi::GObject);
    let _ = obj.emit_by_name_with_values(name, values);
}

// ----------------------------------------------------------------------
// FFI: type accessor + singleton
// ----------------------------------------------------------------------

/// The `G_DECLARE_FINAL_TYPE` accessor. Registers the type (and thus
/// its signals) on first call.
///
/// # Safety
/// Trivially safe; `extern "C"` for ABI compatibility with the
/// `gtkhx_session.h` declaration.
#[no_mangle]
pub extern "C" fn gtkhx_session_get_type() -> glib::ffi::GType {
    <GtkhxSession as StaticType>::static_type().into_glib()
}

/// Returns the process-lifetime singleton emitter, lazily constructed.
/// The singleton holds a permanent ref (never unref'd), matching the
/// old C behaviour.
///
/// # Safety
/// Returns a borrowed `GtkhxSession*` the caller must not unref below
/// the permanent ref. Main-thread use only (GObject construction is
/// thread-safe to *register* but the singleton is a UI object).
#[no_mangle]
pub extern "C" fn gtkhx_session_get_default() -> *mut c_void {
    // Cache the singleton in a `SendPtr` newtype rather than a `usize`:
    // a pointer→integer→pointer round-trip drops provenance under
    // Rust's strict-provenance model, and GLib *will* dereference this
    // pointer. Storing the real `*mut c_void` keeps provenance intact
    // end-to-end. The GObject ref the constructor returns is leaked
    // (`mem::forget`) so the singleton holds a permanent reference, as
    // the old C `gtkhx_session_get_default` did.
    static SINGLETON: OnceLock<SendPtr> = OnceLock::new();
    SINGLETON
        .get_or_init(|| {
            let obj = glib::Object::new::<GtkhxSession>();
            let raw = obj.as_ptr() as *mut c_void;
            std::mem::forget(obj);
            SendPtr(raw)
        })
        .0
}

/// Send+Sync wrapper so a raw `*mut c_void` can live in a `static
/// OnceLock`. SAFETY: the pointer is written exactly once (the
/// singleton is immutable after init) and is only ever dereferenced on
/// the GLib main thread; sharing the pointer *value* across threads is
/// sound, and we never form a `&mut` to the pointee from here.
#[derive(Copy, Clone)]
struct SendPtr(*mut c_void);
unsafe impl Send for SendPtr {}
unsafe impl Sync for SendPtr {}

// ----------------------------------------------------------------------
// FFI: emit wrappers (one per signal; ABI matches gtkhx_session.h)
// ----------------------------------------------------------------------

/// # Safety
/// `self_`/`htlc` valid pointers; `event` a valid `HxChatEvent*` (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_chat(
    self_: *mut c_void,
    htlc: *mut c_void,
    event: *mut c_void,
) {
    let v = [ptr_value(htlc), boxed_value(hx_chat_event_get_type(), event)];
    emit(self_, "chat", &v);
}

/// # Safety
/// `self_`/`htlc` valid; `subj` a valid NUL-terminated C string (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_chat_subject(
    self_: *mut c_void,
    htlc: *mut c_void,
    cid: u32,
    subj: *const c_char,
) {
    let v = [ptr_value(htlc), glib::Value::from(cid), ptr_value(subj as *const c_void)];
    emit(self_, "chat-subject", &v);
}

/// Emit the "Subject Changed to: X" notice line for chat `cid` (distinct from
/// the chat-subject bar update, which also fires for a room's initial subject).
/// The view-side handler owns the gettext + INFOPREFIX.
///
/// # Safety
/// `self_`/`htlc` valid; `subj` a valid NUL-terminated C string (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_chat_subject_notice(
    self_: *mut c_void,
    htlc: *mut c_void,
    cid: u32,
    subj: *const c_char,
) {
    let v = [ptr_value(htlc), glib::Value::from(cid), ptr_value(subj as *const c_void)];
    emit(self_, "chat-subject-notice", &v);
}

/// # Safety
/// `self_`/`htlc` valid; `name` a valid C string (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_chat_invitation(
    self_: *mut c_void,
    htlc: *mut c_void,
    cid: u32,
    name: *const c_char,
) {
    let v = [ptr_value(htlc), glib::Value::from(cid), ptr_value(name as *const c_void)];
    emit(self_, "chat-invitation", &v);
}

/// # Safety
/// `self_`/`htlc`/`entries` valid pointers (`entries` a GPtrArray*).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_chat_history_batch(
    self_: *mut c_void,
    htlc: *mut c_void,
    cid: u32,
    entries: *mut c_void,
    has_more: c_int,
) {
    let v = [
        ptr_value(htlc),
        glib::Value::from(cid),
        ptr_value(entries),
        glib::Value::from(has_more != 0),
    ];
    emit(self_, "chat-history-batch", &v);
}

/// GIF-icons extension (Phase 10). Fired on an ICON_CHANGE (1864)
/// broadcast, which carries only the uid; a view re-fetches the avatar
/// via `hx_icon_get()`.
///
/// # Safety
/// `self_`/`htlc` valid pointers.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_gif_icon_changed(
    self_: *mut c_void,
    htlc: *mut c_void,
    uid: u16,
) {
    let v = [ptr_value(htlc), glib::Value::from(uid as u32)];
    emit(self_, "gif-icon-changed", &v);
}

/// GIF-icons extension (Phase 10). Raw GIF bytes for a user from an
/// ICON_GET reply or ICON_GETLIST entry. `gif` rides as `G_TYPE_POINTER`
/// and is valid only for the synchronous emit — subscribers decode/copy
/// before returning; `len == 0` means the avatar was cleared.
///
/// # Safety
/// `self_`/`htlc` valid; `gif` valid for `len` bytes during the emit (or
/// NULL when `len == 0`).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_gif_icon_data(
    self_: *mut c_void,
    htlc: *mut c_void,
    uid: u16,
    gif: *const c_void,
    len: u32,
) {
    let v = [
        ptr_value(htlc),
        glib::Value::from(uid as u32),
        ptr_value(gif),
        glib::Value::from(len),
    ];
    emit(self_, "gif-icon-data", &v);
}

/// # Safety
/// `self_` valid; `event` a valid `HxMsgEvent*` (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_msg(self_: *mut c_void, event: *mut c_void) {
    let v = [boxed_value(hx_msg_event_get_type(), event)];
    emit(self_, "msg", &v);
}

/// # Safety
/// `self_`/`htlc` valid pointers.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_logged_in(
    self_: *mut c_void,
    htlc: *mut c_void,
) {
    let v = [ptr_value(htlc)];
    emit(self_, "logged-in", &v);
}

/// # Safety
/// `self_`/`htlc` valid pointers.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_self_updated(
    self_: *mut c_void,
    htlc: *mut c_void,
) {
    let v = [ptr_value(htlc)];
    emit(self_, "self-updated", &v);
}

/// # Safety
/// `self_`/`sess` valid; `agreement` a valid buffer pointer (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_agreement(
    self_: *mut c_void,
    sess: *mut c_void,
    agreement: *const c_char,
    len: u16,
) {
    let v = [ptr_value(sess), ptr_value(agreement as *const c_void), glib::Value::from(len as u32)];
    emit(self_, "agreement", &v);
}

/// # Safety
/// `self_`/`htlc` valid; `news` a valid buffer pointer (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_news_file(
    self_: *mut c_void,
    htlc: *mut c_void,
    news: *const c_char,
    len: u16,
) {
    let v = [ptr_value(htlc), ptr_value(news as *const c_void), glib::Value::from(len as u32)];
    emit(self_, "news-file", &v);
}

/// # Safety
/// `self_`/`htlc` valid; `news` a valid buffer pointer (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_news_post(
    self_: *mut c_void,
    htlc: *mut c_void,
    news: *const c_char,
    len: u16,
) {
    let v = [ptr_value(htlc), ptr_value(news as *const c_void), glib::Value::from(len as u32)];
    emit(self_, "news-post", &v);
}

/// # Safety
/// `self_`/`gfnews` valid pointers.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_news_folder(self_: *mut c_void, gfnews: *mut c_void) {
    let v = [ptr_value(gfnews)];
    emit(self_, "news-folder", &v);
}

/// # Safety
/// `self_`/`gcnews` valid pointers.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_news_catalog(self_: *mut c_void, gcnews: *mut c_void) {
    let v = [ptr_value(gcnews)];
    emit(self_, "news-catalog", &v);
}

/// # Safety
/// `self_`/`post` valid pointers.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_news_thread(self_: *mut c_void, post: *mut c_void) {
    let v = [ptr_value(post)];
    emit(self_, "news-thread", &v);
}

/// # Safety
/// `self_`/`htlc`/`chat` valid; `nam` a valid C string (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_user_create(
    self_: *mut c_void,
    htlc: *mut c_void,
    chat: *mut c_void,
    uid: u16,
    nick_color: u32,
    nam: *const c_char,
    icon: u16,
    color: u16,
    incremental: c_int,
) {
    let v = [
        ptr_value(htlc),
        ptr_value(chat),
        glib::Value::from(uid as u32),
        glib::Value::from(nick_color),
        ptr_value(nam as *const c_void),
        glib::Value::from(icon as u32),
        glib::Value::from(color as u32),
        glib::Value::from(incremental != 0),
    ];
    emit(self_, "user-create", &v);
}

/// # Safety
/// `self_`/`htlc`/`chat` valid pointers.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_user_delete(
    self_: *mut c_void,
    htlc: *mut c_void,
    chat: *mut c_void,
    uid: u16,
    incremental: c_int,
) {
    let v = [
        ptr_value(htlc),
        ptr_value(chat),
        glib::Value::from(uid as u32),
        glib::Value::from(incremental != 0),
    ];
    emit(self_, "user-delete", &v);
}

/// # Safety
/// `self_`/`htlc`/`chat` valid; `nam` a valid C string (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_user_change(
    self_: *mut c_void,
    htlc: *mut c_void,
    chat: *mut c_void,
    uid: u16,
    nick_color: u32,
    nam: *const c_char,
    icon: u16,
    color: u16,
) {
    let v = [
        ptr_value(htlc),
        ptr_value(chat),
        glib::Value::from(uid as u32),
        glib::Value::from(nick_color),
        ptr_value(nam as *const c_void),
        glib::Value::from(icon as u32),
        glib::Value::from(color as u32),
    ];
    emit(self_, "user-change", &v);
}

/// # Safety
/// `self_`/`htlc`/`chat` valid pointers.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_users_clear(
    self_: *mut c_void,
    htlc: *mut c_void,
    chat: *mut c_void,
) {
    let v = [ptr_value(htlc), ptr_value(chat)];
    emit(self_, "users-clear", &v);
}

/// # Safety
/// `self_` valid; `nam`/`info` valid C strings/buffers (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_user_info(
    self_: *mut c_void,
    uid: u16,
    nam: *const c_char,
    info: *const c_char,
    len: u16,
) {
    let v = [
        glib::Value::from(uid as u32),
        ptr_value(nam as *const c_void),
        ptr_value(info as *const c_void),
        glib::Value::from(len as u32),
    ];
    emit(self_, "user-info", &v);
}

/// # Safety
/// `self_` valid; the five string args valid C strings (or NULL); `date_modify`
/// / `date_create` each point to 8 raw wire bytes (the Hotline date stamp) and
/// stay valid for the synchronous emit. The view side decodes + formats them.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn gtkhx_session_emit_file_info(
    self_: *mut c_void,
    path: *const c_char,
    name: *const c_char,
    creator: *const c_char,
    type_: *const c_char,
    comments: *const c_char,
    date_modify: *const u8,
    date_create: *const u8,
    size: u64,
) {
    let v = [
        ptr_value(path as *const c_void),
        ptr_value(name as *const c_void),
        ptr_value(creator as *const c_void),
        ptr_value(type_ as *const c_void),
        ptr_value(comments as *const c_void),
        ptr_value(date_modify as *const c_void),
        ptr_value(date_create as *const c_void),
        glib::Value::from(size),
    ];
    emit(self_, "file-info", &v);
}

/// # Safety
/// `self_`/`cfl`/`fh`/`data` valid pointers.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_file_list(
    self_: *mut c_void,
    cfl: *mut c_void,
    fh: *mut c_void,
    data: *mut c_void,
) {
    let v = [ptr_value(cfl), ptr_value(fh), ptr_value(data)];
    emit(self_, "file-list", &v);
}

/// # Safety
/// `self_`/`sess`/`htxf` valid pointers.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_file_update(
    self_: *mut c_void,
    sess: *mut c_void,
    htxf: *mut c_void,
) {
    let v = [ptr_value(sess), ptr_value(htxf)];
    emit(self_, "file-update", &v);
}

/// # Safety
/// `self_`/`sess`/`htxf` valid pointers.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_xfer_queue(
    self_: *mut c_void,
    sess: *mut c_void,
    htxf: *mut c_void,
) {
    let v = [ptr_value(sess), ptr_value(htxf)];
    emit(self_, "xfer-queue", &v);
}

/// # Safety
/// `self_`/`sess`/`htxf` valid pointers.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_xfer_destroyed(
    self_: *mut c_void,
    sess: *mut c_void,
    htxf: *mut c_void,
) {
    let v = [ptr_value(sess), ptr_value(htxf)];
    emit(self_, "xfer-destroyed", &v);
}

/// # Safety
/// `self_` valid; `event` a valid `HxTrackerServer*` (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_tracker_server_create(
    self_: *mut c_void,
    event: *mut c_void,
) {
    let v = [boxed_value(hx_tracker_server_get_type(), event)];
    emit(self_, "tracker-server-create", &v);
}

/// # Safety
/// `self_` valid; `tracker_url` a valid C string (or NULL → empty).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_tracker_batch_begin(
    self_: *mut c_void,
    tracker_url: *const c_char,
    version: u8,
    expected_count: u16,
) {
    // Registered as G_TYPE_STRING — copy the bytes into the Value (the
    // old C code did the same via the varargs string-collect path).
    // NULL maps to "" exactly as the C wrapper did.
    let url = if tracker_url.is_null() {
        String::new()
    } else {
        CStr::from_ptr(tracker_url).to_string_lossy().into_owned()
    };
    let v = [
        glib::Value::from(url.as_str()),
        glib::Value::from(version),
        glib::Value::from(expected_count as u32),
    ];
    emit(self_, "tracker-batch-begin", &v);
}

/// # Safety
/// `self_`/`sess`/`tsk` valid pointers.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_task_update(
    self_: *mut c_void,
    sess: *mut c_void,
    tsk: *mut c_void,
) {
    let v = [ptr_value(sess), ptr_value(tsk)];
    emit(self_, "task-update", &v);
}

/// # Safety
/// `self_`/`htlc` valid; `body` a valid C string (or NULL).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_chat_log_line(
    self_: *mut c_void,
    htlc: *mut c_void,
    cid: u32,
    body: *const c_char,
) {
    let v = [ptr_value(htlc), glib::Value::from(cid), ptr_value(body as *const c_void)];
    emit(self_, "chat-log-line", &v);
}

/// Emit a roster notice line (join / parts / rename) for chat `cid`. `kind` is
/// one of the `HX_USER_NOTICE_*` values (gtkhx_session.h); `name`/`old_name` are
/// raw C-string pointers (`old_name` is NULL except for a rename).
///
/// # Safety
/// `self_`/`htlc` valid; `name`/`old_name` are valid C strings or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_user_notice(
    self_: *mut c_void,
    htlc: *mut c_void,
    cid: u32,
    kind: u32,
    name: *const c_char,
    old_name: *const c_char,
) {
    let v = [
        ptr_value(htlc),
        glib::Value::from(cid),
        glib::Value::from(kind),
        ptr_value(name as *const c_void),
        ptr_value(old_name as *const c_void),
    ];
    emit(self_, "user-notice", &v);
}

/// # Safety
/// `self_` valid. `state` is a `GtkhxConnectionState` (C enum = int).
#[no_mangle]
pub unsafe extern "C" fn gtkhx_session_emit_connection_state(self_: *mut c_void, state: c_int) {
    let v = [glib::Value::from(state as u32)];
    emit(self_, "connection-state-changed", &v);
}

// ----------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------

#[cfg(test)]
mod test_boxed_stubs {
    //! Stand-ins for the C boxed types so `cargo test` can link and
    //! exercise the full registration + boxed-emit path. Each is a
    //! real Rust-registered boxed type, so the GType is a genuine
    //! `G_TYPE_BOXED` derivative — `g_value_set_boxed` works against it.
    use glib::prelude::StaticType;

    #[derive(Clone, Default, glib::Boxed)]
    #[boxed_type(name = "HxChatEventTestStub")]
    pub struct ChatStub(pub u32);

    #[derive(Clone, Default, glib::Boxed)]
    #[boxed_type(name = "HxMsgEventTestStub")]
    pub struct MsgStub(pub u32);

    #[derive(Clone, Default, glib::Boxed)]
    #[boxed_type(name = "HxTrackerServerTestStub")]
    pub struct TrackerStub(pub u32);

    pub fn hx_chat_event_get_type() -> glib::ffi::GType {
        use glib::translate::IntoGlib;
        ChatStub::static_type().into_glib()
    }
    pub fn hx_msg_event_get_type() -> glib::ffi::GType {
        use glib::translate::IntoGlib;
        MsgStub::static_type().into_glib()
    }
    pub fn hx_tracker_server_get_type() -> glib::ffi::GType {
        use glib::translate::IntoGlib;
        TrackerStub::static_type().into_glib()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use glib::subclass::prelude::ObjectImpl;
    use glib::translate::ToGlibPtr;
    use std::cell::Cell;
    use std::rc::Rc;

    fn new_session() -> GtkhxSession {
        glib::Object::new::<GtkhxSession>()
    }

    /// Read a `G_TYPE_POINTER` Value back as a usize for assertions.
    fn pval(v: &glib::Value) -> usize {
        unsafe { glib::gobject_ffi::g_value_get_pointer(v.to_glib_none().0) as usize }
    }

    #[test]
    fn user_notice_signal_round_trips() {
        // (htlc ptr, cid:u32, kind:u32, name ptr, old_name ptr) — the 5-arg
        // roster-notice shape. Proves the emit wrapper marshals all five and
        // that the signal is registered (an unregistered signal would
        // GLib-critical on emit rather than reach the handler).
        let s = new_session();
        let got: Rc<Cell<(usize, u32, u32, usize, usize)>> = Rc::new(Cell::new((0, 0, 0, 0, 0)));
        let got2 = got.clone();
        s.connect_local("user-notice", false, move |args| {
            got2.set((
                pval(&args[1]),
                args[2].get::<u32>().unwrap(),
                args[3].get::<u32>().unwrap(),
                pval(&args[4]),
                pval(&args[5]),
            ));
            None
        });
        let raw = s.as_ptr() as *mut c_void;
        let name = std::ffi::CString::new("Alice").unwrap();
        unsafe {
            gtkhx_session_emit_user_notice(
                raw,
                0xABCD as *mut c_void,
                7,
                2,
                name.as_ptr(),
                std::ptr::null(),
            );
        }
        assert_eq!(
            got.get(),
            (0xABCD, 7, 2, name.as_ptr() as usize, 0)
        );
    }

    #[test]
    fn chat_subject_notice_signal_round_trips() {
        // (htlc ptr, cid:u32, subject ptr) — distinct from chat-subject so the
        // "Subject Changed to" line only fires on a real change.
        let s = new_session();
        let got: Rc<Cell<(usize, u32, usize)>> = Rc::new(Cell::new((0, 0, 0)));
        let got2 = got.clone();
        s.connect_local("chat-subject-notice", false, move |args| {
            got2.set((pval(&args[1]), args[2].get::<u32>().unwrap(), pval(&args[3])));
            None
        });
        let raw = s.as_ptr() as *mut c_void;
        let subj = std::ffi::CString::new("New topic").unwrap();
        unsafe {
            gtkhx_session_emit_chat_subject_notice(raw, 0x1234 as *mut c_void, 5, subj.as_ptr());
        }
        assert_eq!(got.get(), (0x1234, 5, subj.as_ptr() as usize));
    }

    #[test]
    fn pointer_pair_signal_round_trips() {
        let s = new_session();
        let got: Rc<Cell<(usize, usize)>> = Rc::new(Cell::new((0, 0)));
        let got2 = got.clone();
        // connect_local hands the raw &[Value] (args[0] is the instance).
        s.connect_local("task-update", false, move |args| {
            got2.set((pval(&args[1]), pval(&args[2])));
            None
        });
        let raw = s.as_ptr() as *mut c_void;
        unsafe {
            gtkhx_session_emit_task_update(raw, 0x1111 as *mut c_void, 0x2222 as *mut c_void);
        }
        assert_eq!(got.get(), (0x1111, 0x2222));
    }

    #[test]
    fn scalar_and_string_signal_round_trips() {
        let s = new_session();
        let url: Rc<std::cell::RefCell<String>> = Rc::new(std::cell::RefCell::new(String::new()));
        let ver: Rc<Cell<u8>> = Rc::new(Cell::new(0));
        let count: Rc<Cell<u32>> = Rc::new(Cell::new(0));
        let (u2, v2, c2) = (url.clone(), ver.clone(), count.clone());
        s.connect_local("tracker-batch-begin", false, move |args| {
            *u2.borrow_mut() = args[1].get::<String>().unwrap();
            v2.set(args[2].get::<u8>().unwrap());
            c2.set(args[3].get::<u32>().unwrap());
            None
        });
        let raw = s.as_ptr() as *mut c_void;
        let curl = std::ffi::CString::new("hxtrackd://example").unwrap();
        unsafe {
            gtkhx_session_emit_tracker_batch_begin(raw, curl.as_ptr(), 3, 42);
        }
        assert_eq!(&*url.borrow(), "hxtrackd://example");
        assert_eq!(ver.get(), 3);
        assert_eq!(count.get(), 42);
    }

    #[test]
    fn boxed_signal_round_trips_and_copies() {
        // The boxed payload must reach the handler intact (proves the
        // g_value_set_boxed marshaling + the boxed copy func ran).
        let s = new_session();
        let val: Rc<Cell<u32>> = Rc::new(Cell::new(0));
        let v2 = val.clone();
        s.connect_local("chat", false, move |args| {
            let ev = args[2].get::<test_boxed_stubs::ChatStub>().unwrap();
            v2.set(ev.0);
            None
        });
        let raw = s.as_ptr() as *mut c_void;
        let mut payload = test_boxed_stubs::ChatStub(0xABCD);
        unsafe {
            gtkhx_session_emit_chat(
                raw,
                std::ptr::null_mut(),
                &mut payload as *mut _ as *mut c_void,
            );
        }
        assert_eq!(val.get(), 0xABCD);
    }

    #[test]
    fn gif_icon_data_round_trips() {
        // GIF-icons emit: (htlc ptr, uid:u32, gif ptr, len:u32).
        let s = new_session();
        let got: Rc<Cell<(usize, u32, usize, u32)>> = Rc::new(Cell::new((0, 0, 0, 0)));
        let got2 = got.clone();
        s.connect_local("gif-icon-data", false, move |args| {
            got2.set((
                pval(&args[1]),
                args[2].get::<u32>().unwrap(),
                pval(&args[3]),
                args[4].get::<u32>().unwrap(),
            ));
            None
        });
        let raw = s.as_ptr() as *mut c_void;
        unsafe {
            gtkhx_session_emit_gif_icon_data(
                raw,
                0x1234 as *mut c_void,
                42,
                0xBEEF as *const c_void,
                7,
            );
        }
        assert_eq!(got.get(), (0x1234, 42, 0xBEEF, 7));
    }

    #[test]
    fn connection_state_round_trips() {
        let s = new_session();
        let got: Rc<Cell<u32>> = Rc::new(Cell::new(99));
        let got2 = got.clone();
        s.connect_local("connection-state-changed", false, move |args| {
            got2.set(args[1].get::<u32>().unwrap());
            None
        });
        let raw = s.as_ptr() as *mut c_void;
        unsafe {
            gtkhx_session_emit_connection_state(raw, 3);
        }
        assert_eq!(got.get(), 3);
    }

    #[test]
    fn get_default_is_a_stable_singleton() {
        let a = gtkhx_session_get_default();
        let b = gtkhx_session_get_default();
        assert!(!a.is_null());
        assert_eq!(a, b, "get_default must return the same pointer");
    }

    #[test]
    fn null_session_is_handled_gracefully() {
        // Must g_critical + return, not crash.
        unsafe {
            gtkhx_session_emit_connection_state(std::ptr::null_mut(), 1);
        }
    }
}
