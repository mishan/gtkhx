//! `HxVoiceModel` — per-uid voice presence + state for the chat / users list
//! speaker indicators. Rust `glib::subclass` port of `src/voice_model.c`
//! (Phase R5).
//!
//! A `GObject` holding a `HashMap<uid → Entry>` where each entry carries the
//! three input flags (`in_voice`, `muted`, `speaking`) and the last-emitted
//! indicator. Every ingest path recomputes the derived [`HxVoiceIndicator`]
//! via [`compute_indicator`] and emits `"indicator-changed"` only when a
//! uid's *computed* indicator actually flips — keeping last_indicator on the
//! entry (rather than recomputing the previous state) means one signal-emit
//! per real visible change, not a churn-burst.
//!
//! Three data sources flow in, exactly as the C original documented:
//!   1. `DATA_VOICE_PARTICIPANTS` blobs (605 ROOM_STATUS / JOIN reply) →
//!      [`HxVoiceModel::ingest_participants`].
//!   2. The runtime's per-pad voice-activity detector →
//!      [`HxVoiceModel::set_speaking`].
//!   3. Disconnect / teardown → [`HxVoiceModel::clear`].
//!
//! The C ABI (`hx_voice_model_*`) is preserved exactly so `rcv.c`, `gtkhx.c`,
//! `network.c`, `users.c` link unchanged, and the gtkhx-ui `voice` modules
//! (`users_voice_col`, `voice_panel`) keep reaching it through their existing
//! externs. Presence-transition chimes still call the C `play_sound`
//! (sound.c) over FFI; a test build stubs it (see the `tests` module).
//!
//! Main-thread only, same as the C side — no `Send`/`Sync` surface.

use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet};
use std::ffi::c_void;
use std::os::raw::c_int;

use glib::prelude::*;
use glib::subclass::prelude::*;
use glib::translate::IntoGlib;

// ---------------------------------------------------------------------
// Indicator enum (mirrors voice_model.h HxVoiceIndicator).
// ---------------------------------------------------------------------

/// uid is not in voice chat. No indicator.
pub const INDICATOR_NONE: u32 = 0;
/// In voice, not speaking, not muted. Dim speaker glyph.
pub const INDICATOR_IN_VOICE: u32 = 1;
/// In voice AND actively speaking (runtime VAD). Highlighted speaker glyph.
pub const INDICATOR_SPEAKING: u32 = 2;
/// In voice AND server-flagged muted. Overrides IN_VOICE / SPEAKING.
pub const INDICATOR_MUTED: u32 = 3;

/// Hard cap on participants tracked per ingest. `blob` is server-supplied;
/// without a cap a malicious/buggy server could force gigabyte allocations
/// and an O(n) sweep on the UI thread. 1024 is orders of magnitude above any
/// plausible room and well below any DoS-relevant size. (Mirrors the C
/// `HX_VOICE_MODEL_MAX_PARTICIPANTS`.)
const MAX_PARTICIPANTS: usize = 1024;

/// `sound.h`: `play_sound` ids for the presence chimes.
const VOICE_JOIN: c_int = 9;
const VOICE_LEAVE: c_int = 10;

// play_sound lives in sound.c in the real build (resolved at the final C
// link). A `cfg(test)` build has no sound.c, so the test module provides a
// recording stub under the same symbol name.
#[cfg(not(test))]
extern "C" {
    fn play_sound(sound: c_int);
}
#[cfg(test)]
use tests::play_sound;

#[derive(Default, Clone, Copy)]
pub(crate) struct Entry {
    in_voice: bool,
    muted: bool,
    speaking: bool,
    last_indicator: u32,
}

/// The indicator-policy projection of a single entry's flags. MUTED beats
/// SPEAKING (a server-flagged muted participant can't be producing audio from
/// the listener's perspective even if a residual VAD reading trips); SPEAKING
/// reflects real client-side voice-activity detection.
fn compute_indicator(e: &Entry) -> u32 {
    if !e.in_voice {
        return INDICATOR_NONE;
    }
    if e.muted {
        return INDICATOR_MUTED;
    }
    if e.speaking {
        return INDICATOR_SPEAKING;
    }
    INDICATOR_IN_VOICE
}

mod imp {
    use super::*;
    use glib::subclass::Signal;
    use std::sync::OnceLock;

    #[derive(Default)]
    pub struct HxVoiceModel {
        /// uid → entry. Interior mutability: never held across a signal emit
        /// (a handler may re-enter get_indicator).
        pub(crate) by_uid: RefCell<HashMap<u16, Entry>>,
        /// Our own uid, so presence chimes skip our own join/leave. 0 = off.
        pub(crate) self_uid: Cell<u16>,
        /// Whether the initial roster has been ingested. The first blob after
        /// a join/clear seeds silently (no chime burst for people already
        /// present); subsequent ingests chime. Re-armed by `clear`.
        pub(crate) seeded: Cell<bool>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for HxVoiceModel {
        const NAME: &'static str = "HxVoiceModel";
        type Type = super::HxVoiceModel;
        type ParentType = glib::Object;
    }

    impl ObjectImpl for HxVoiceModel {
        fn signals() -> &'static [Signal] {
            // "indicator-changed" (uid: u32, indicator: u32). Both scalar so
            // no typed-boxed-payload dance for a tiny signal.
            static SIGNALS: OnceLock<Vec<Signal>> = OnceLock::new();
            SIGNALS.get_or_init(|| {
                vec![Signal::builder("indicator-changed")
                    .param_types([u32::static_type(), u32::static_type()])
                    .build()]
            })
        }
    }
}

glib::wrapper! {
    /// Per-uid voice presence model. See module docs.
    pub struct HxVoiceModel(ObjectSubclass<imp::HxVoiceModel>);
}

impl Default for HxVoiceModel {
    fn default() -> Self {
        Self::new()
    }
}

impl HxVoiceModel {
    /// Construct an empty model.
    pub fn new() -> Self {
        glib::Object::new()
    }

    /// Recompute `uid`'s indicator; emit `"indicator-changed"` iff it flipped.
    /// The `by_uid` borrow is released before the emit so a handler may safely
    /// call back into `get_indicator`.
    fn recompute_and_maybe_emit(&self, uid: u16) -> bool {
        let now = {
            let mut map = self.imp().by_uid.borrow_mut();
            let Some(e) = map.get_mut(&uid) else {
                return false;
            };
            let now = compute_indicator(e);
            if now == e.last_indicator {
                return false;
            }
            e.last_indicator = now;
            now
        };
        self.emit_by_name::<()>("indicator-changed", &[&(uid as u32), &now]);
        true
    }

    /// Refresh presence + mute from a freshly-arrived `DATA_VOICE_PARTICIPANTS`
    /// blob (6-byte packed records). uids absent from the blob transition to
    /// NONE; present uids to IN_VOICE / MUTED per flags bit 0. Speaking is
    /// preserved across the call (the VAD probe owns it). Empty blob = empty
    /// room (everyone leaves).
    pub fn ingest_participants(&self, blob: &[u8]) {
        // Each record is 6 bytes; len/6 bounds the count. Cap untrusted blobs.
        let total = blob.len() / 6;
        if total > MAX_PARTICIPANTS {
            glib::g_warning!(
                "hxvoice-model",
                "ingest_participants: blob carries {total} entries; capping at \
                 {MAX_PARTICIPANTS}. uids past the cap are treated as leavers."
            );
        }
        let parts: Vec<hotline_proto::voice::Participant> =
            hotline_proto::voice::parse_voice_participants(blob)
                .take(MAX_PARTICIPANTS)
                .collect();

        let keep: HashSet<u16> = parts.iter().map(|p| p.user_id).collect();
        let self_uid = self.imp().self_uid.get();
        let seeded = self.imp().seeded.get();

        // First pass: update / insert for every named uid.
        for p in &parts {
            let uid = p.user_id;
            let muted = p.is_muted();
            let was_in_voice = {
                let mut map = self.imp().by_uid.borrow_mut();
                let e = map.entry(uid).or_default();
                let was = e.in_voice;
                e.in_voice = true;
                e.muted = muted;
                // speaking preserved — the VAD probe owns it.
                was
            };
            self.recompute_and_maybe_emit(uid);
            // Join chime for a genuine presence transition, only after the
            // initial roster seeded and never for our own uid.
            if seeded && !was_in_voice && uid != self_uid {
                unsafe { play_sound(VOICE_JOIN) };
            }
        }

        // Second pass: uids that disappeared from the room drop to NONE.
        let leavers: Vec<u16> = self
            .imp()
            .by_uid
            .borrow()
            .keys()
            .copied()
            .filter(|u| !keep.contains(u))
            .collect();
        for uid in leavers {
            {
                let mut map = self.imp().by_uid.borrow_mut();
                match map.get_mut(&uid) {
                    Some(e) => {
                        e.in_voice = false;
                        e.muted = false;
                        e.speaking = false;
                    }
                    None => continue,
                }
            }
            self.recompute_and_maybe_emit(uid);
            if seeded && uid != self_uid {
                unsafe { play_sound(VOICE_LEAVE) };
            }
            // Drop the entry so a malicious server cycling random uids can't
            // grow the table unboundedly; re-join re-inserts cheaply.
            self.imp().by_uid.borrow_mut().remove(&uid);
        }

        self.imp().seeded.set(true);
    }

    /// Record our own uid so presence chimes skip our own join/leave. 0
    /// disables the self-exclusion.
    pub fn set_self_uid(&self, uid: u16) {
        self.imp().self_uid.set(uid);
    }

    /// Update the speaking flag for `uid`. No-op for uids not currently in the
    /// room (avoids a transient flicker when the runtime's pad-added fires a
    /// beat before the 605 confirming presence).
    pub fn set_speaking(&self, uid: u16, speaking: bool) {
        {
            let mut map = self.imp().by_uid.borrow_mut();
            let Some(e) = map.get_mut(&uid) else {
                return;
            };
            if !e.in_voice || e.speaking == speaking {
                return;
            }
            e.speaking = speaking;
        }
        self.recompute_and_maybe_emit(uid);
    }

    /// Clear all per-uid state, transitioning every active uid back to NONE
    /// (with per-uid signals) and re-arming the seed gate.
    pub fn clear(&self) {
        let uids: Vec<u16> = self.imp().by_uid.borrow().keys().copied().collect();
        for uid in uids {
            {
                let mut map = self.imp().by_uid.borrow_mut();
                match map.get_mut(&uid) {
                    Some(e) => {
                        e.in_voice = false;
                        e.muted = false;
                        e.speaking = false;
                    }
                    None => continue,
                }
            }
            self.recompute_and_maybe_emit(uid);
        }
        self.imp().by_uid.borrow_mut().clear();
        self.imp().seeded.set(false);
    }

    /// Computed indicator for `uid` (NONE for unknown uids). O(1).
    pub fn get_indicator(&self, uid: u16) -> u32 {
        self.imp()
            .by_uid
            .borrow()
            .get(&uid)
            .map(|e| e.last_indicator)
            .unwrap_or(INDICATOR_NONE)
    }
}

// ---------------------------------------------------------------------
// C ABI (mirrors voice_model.h).
// ---------------------------------------------------------------------

/// Borrow a C-passed `HxVoiceModel *` without touching its refcount.
///
/// # Safety
/// `p` is a valid `HxVoiceModel *`.
unsafe fn borrow(p: *mut c_void) -> glib::translate::Borrowed<HxVoiceModel> {
    glib::translate::from_glib_borrow::<_, HxVoiceModel>(
        p as *mut <HxVoiceModel as glib::object::ObjectType>::GlibType,
    )
}

/// The `G_DECLARE_FINAL_TYPE` accessor.
#[no_mangle]
pub extern "C" fn hx_voice_model_get_type() -> glib::ffi::GType {
    <HxVoiceModel as StaticType>::static_type().into_glib()
}

/// Construct an empty model. Transfer-full (one owned ref), matching
/// `g_object_new`.
#[no_mangle]
pub extern "C" fn hx_voice_model_new() -> *mut c_void {
    let obj = HxVoiceModel::new();
    let raw = obj.as_ptr() as *mut c_void;
    std::mem::forget(obj);
    raw
}

/// # Safety
/// `self_` is a valid `HxVoiceModel *`; `blob` is NULL or valid for `len`.
#[no_mangle]
pub unsafe extern "C" fn hx_voice_model_ingest_participants(
    self_: *mut c_void,
    blob: *const u8,
    len: usize,
) {
    if self_.is_null() {
        return;
    }
    let slice: &[u8] = if blob.is_null() || len == 0 {
        &[]
    } else {
        std::slice::from_raw_parts(blob, len)
    };
    borrow(self_).ingest_participants(slice);
}

/// # Safety
/// `self_` is a valid `HxVoiceModel *`.
#[no_mangle]
pub unsafe extern "C" fn hx_voice_model_set_self_uid(self_: *mut c_void, uid: u16) {
    if self_.is_null() {
        return;
    }
    borrow(self_).set_self_uid(uid);
}

/// # Safety
/// `self_` is a valid `HxVoiceModel *`.
#[no_mangle]
pub unsafe extern "C" fn hx_voice_model_set_speaking(
    self_: *mut c_void,
    uid: u16,
    is_speaking: glib::ffi::gboolean,
) {
    if self_.is_null() {
        return;
    }
    borrow(self_).set_speaking(uid, is_speaking != glib::ffi::GFALSE);
}

/// # Safety
/// `self_` is a valid `HxVoiceModel *`.
#[no_mangle]
pub unsafe extern "C" fn hx_voice_model_clear(self_: *mut c_void) {
    if self_.is_null() {
        return;
    }
    borrow(self_).clear();
}

/// Returns the computed `HxVoiceIndicator` (NONE for unknown uids / NULL).
///
/// # Safety
/// `self_` is NULL or a valid `HxVoiceModel *`.
#[no_mangle]
pub unsafe extern "C" fn hx_voice_model_get_indicator(self_: *mut c_void, uid: u16) -> u32 {
    if self_.is_null() {
        return INDICATOR_NONE;
    }
    borrow(self_).get_indicator(uid)
}

#[cfg(test)]
mod tests;
