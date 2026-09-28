//! GIF user avatars: each user's decoded frames, and their animation.
//!
//! A GIF-icons server sends each user's icon as a GIF, which lands in
//! [`gtkhx_avatar_update`] and is decoded on the shared image decoder. The
//! result is an [`AvatarPaintable`] per user: a `GdkPaintable` holding every
//! frame, drawn by the user-list cells in place of the classic cicn icon.
//!
//! One shared timer animates them, and only the ones on screen. A paintable
//! records that it was drawn; the timer advances an avatar only once it has
//! been drawn since its last frame, and stops itself when a tick finds none
//! that was. The next draw of an animated avatar starts it again. An avatar
//! in a row scrolled out of view, a hidden panel or a minimized window costs
//! nothing — the same gating the chat view gives its images.
//!
//! Advancing a frame invalidates the paintable's contents, which redraws
//! the cells showing it and nothing else; the cells don't rebind.
//!
//! The chat view shows avatars too, but as a texture it fetches when it
//! draws a row ([`gtkhx_avatar_get`]), so there it shows whichever frame
//! the list last reached.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::c_void;

use gtk::gdk;
use gtk::glib;
use gtk::glib::translate::{from_glib_none, IntoGlib, ToGlibPtr};
use gtk::prelude::*;
use gtk::subclass::prelude::*;
use gtk4 as gtk;
use hx_image_decode::ffi::{
    inline_media_decode_async, inline_media_decode_cancel, inline_media_decoded_free,
    HxInlineMediaCaps, HxInlineMediaDecoded, HxInlineMediaFrame,
};

extern "C" {
    /// `users.c` — have every list showing `uid` re-read its avatar.
    fn users_refresh_avatar(uid: u16);
}

/// How often the timer looks for a frame that has outlived its delay:
/// about 16 frames a second at most, plenty for GIFs, whose delays are
/// typically 50–200 ms.
const ANIM_TICK_MS: u64 = 60;
/// What a frame with no delay is shown for, rather than spinning.
const ZERO_DELAY_MS: u32 = 100;

/// Avatar decode bounds. The wire length field already caps a GIF at
/// 64 KiB. Avatars are drawn where the classic icons are, from a normal
/// icon up to a wide banner whose visible width tops out around 200 px, so
/// nothing larger than 256 px a side is ever shown. The cap is checked on
/// the GIF's logical canvas, which every frame is composited onto, so it
/// bounds every frame: at most 256 KiB each, and 64 MiB for a hostile GIF
/// at the frame cap. An oversize canvas fails the decode, and the user
/// keeps their numeric icon.
const AVATAR_MAX_DIM: u32 = 256;
const AVATAR_CAPS: HxInlineMediaCaps = HxInlineMediaCaps {
    max_bytes: 64 * 1024,
    max_dimension: AVATAR_MAX_DIM,
    max_pixels: AVATAR_MAX_DIM * AVATAR_MAX_DIM,
    max_frames: 256,
    max_duration_ms: 30_000,
};

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct AvatarPaintable {
        /// Every frame and its delay in ms; never empty once built.
        pub frames: RefCell<Vec<(gdk::Texture, u32)>>,
        /// The frame showing while animation is on.
        pub cur: Cell<usize>,
        /// When `cur` started showing, in monotonic µs.
        pub cur_since: Cell<i64>,
        /// Frozen by the user (click the avatar, or the context menu).
        pub paused: Cell<bool>,
        /// Drawn since it last changed frame, so on screen.
        pub drawn: Cell<bool>,
        /// Frames moved on since it was built.
        pub steps: Cell<u64>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for AvatarPaintable {
        const NAME: &'static str = "GtkhxAvatarPaintable";
        type Type = super::AvatarPaintable;
        type Interfaces = (gdk::Paintable,);
    }

    impl ObjectImpl for AvatarPaintable {}

    impl PaintableImpl for AvatarPaintable {
        fn intrinsic_width(&self) -> i32 {
            self.frames.borrow().first().map_or(0, |(t, _)| t.width())
        }

        fn intrinsic_height(&self) -> i32 {
            self.frames.borrow().first().map_or(0, |(t, _)| t.height())
        }

        fn flags(&self) -> gdk::PaintableFlags {
            // Every frame is composited onto the same canvas.
            gdk::PaintableFlags::SIZE
        }

        fn snapshot(&self, snapshot: &gdk::Snapshot, width: f64, height: f64) {
            let obj = self.obj();
            if let Some(t) = obj.shown() {
                t.snapshot(snapshot, width, height);
            }
            if obj.running() {
                self.drawn.set(true);
                ensure_timer();
            }
        }
    }
}

glib::wrapper! {
    pub struct AvatarPaintable(ObjectSubclass<imp::AvatarPaintable>)
        @implements gdk::Paintable;
}

impl AvatarPaintable {
    /// An avatar of `frames`, each with its delay in ms. `None` if there
    /// are none.
    pub fn new(frames: Vec<(gdk::Texture, u32)>) -> Option<Self> {
        if frames.is_empty() {
            return None;
        }
        let a: Self = glib::Object::new();
        *a.imp().frames.borrow_mut() = frames;
        a.imp().cur_since.set(glib::monotonic_time());
        Some(a)
    }

    pub fn is_animated(&self) -> bool {
        self.imp().frames.borrow().len() > 1
    }

    pub fn is_paused(&self) -> bool {
        self.imp().paused.get()
    }

    /// Animated, unpaused, and animation is on.
    fn running(&self) -> bool {
        self.is_animated() && !self.is_paused() && anim_enabled()
    }

    /// The frame to draw: the current one, or the first while animation is
    /// off.
    pub fn shown(&self) -> Option<gdk::Texture> {
        let frames = self.imp().frames.borrow();
        let i = if anim_enabled() {
            self.imp().cur.get()
        } else {
            0
        };
        frames.get(i).or(frames.first()).map(|(t, _)| t.clone())
    }

    /// Move on a frame if this one has outlived its delay at `now`. Only an
    /// avatar drawn since its last frame moves; the answer is whether it
    /// was drawn, and so whether the timer still has work.
    fn step(&self, now: i64) -> bool {
        let imp = self.imp();
        if !self.running() || !imp.drawn.get() {
            return false;
        }
        let (n, delay) = {
            let frames = imp.frames.borrow();
            let d = frames[imp.cur.get()].1;
            (frames.len(), if d == 0 { ZERO_DELAY_MS } else { d })
        };
        if now - imp.cur_since.get() < i64::from(delay) * 1000 {
            return true;
        }
        imp.cur.set((imp.cur.get() + 1) % n);
        imp.steps.set(imp.steps.get() + 1);
        imp.cur_since.set(now);
        imp.drawn.set(false);
        self.invalidate_contents();
        true
    }

    fn set_paused(&self, paused: bool) {
        let imp = self.imp();
        if imp.paused.replace(paused) == paused {
            return;
        }
        if !paused {
            // Resume from now, not from a frame clock left stale by the
            // pause, and draw to start the timer if it's on screen. What
            // was drawn before the pause says nothing about now.
            imp.cur_since.set(glib::monotonic_time());
            imp.drawn.set(false);
            self.invalidate_contents();
        }
    }
}

// ---- The cache and the timer ------------------------------------------

#[derive(Default)]
struct State {
    /// Avatar key → the avatar.
    cache: HashMap<u32, AvatarPaintable>,
    /// Avatar key → the in-flight decode's cancel token. At most one per
    /// key; a new update for the key cancels the previous one.
    pending: HashMap<u32, Token>,
    /// The "animate avatars" preference; when off every avatar shows its
    /// first frame.
    disabled: bool,
    timer: Option<glib::SourceId>,
}

/// A decode's cancel token, which is also its free.
struct Token(*mut c_void);

impl Drop for Token {
    fn drop(&mut self) {
        // Cancel after completion is a no-op that still frees the token.
        unsafe { inline_media_decode_cancel(self.0) };
    }
}

thread_local! {
    static STATE: RefCell<State> = RefCell::new(State::default());
}

fn anim_enabled() -> bool {
    STATE.with(|s| !s.borrow().disabled)
}

/// Both tables are app-global, but a uid is unique only within a
/// connection: two servers can each have a user 5 with a different face.
/// The key pairs the uid with the connection's serial, a 16-bit value
/// each. No connection gives serial 0, so a connectionless caller's key
/// can never collide with a real connection's.
fn avatar_key(htlc: *mut c_void, uid: u16) -> u32 {
    let serial = unsafe { gtkhx_core::conn::hx_conn_serial(htlc.cast()) };
    (u32::from(serial) << 16) | u32::from(uid)
}

fn key_uid(key: u32) -> u16 {
    (key & 0xffff) as u16
}

fn lookup(htlc: *mut c_void, uid: u16) -> Option<AvatarPaintable> {
    if uid == 0 {
        return None;
    }
    let key = avatar_key(htlc, uid);
    STATE.with(|s| s.borrow().cache.get(&key).cloned())
}

fn all() -> Vec<AvatarPaintable> {
    STATE.with(|s| s.borrow().cache.values().cloned().collect())
}

/// Start the timer if it isn't running. A drawn animated avatar calls it.
fn ensure_timer() {
    STATE.with(|s| {
        let mut s = s.borrow_mut();
        if s.timer.is_none() {
            s.timer = Some(glib::timeout_add_local(
                std::time::Duration::from_millis(ANIM_TICK_MS),
                tick,
            ));
        }
    });
}

fn stop_timer() {
    if let Some(id) = STATE.with(|s| s.borrow_mut().timer.take()) {
        id.remove();
    }
}

fn tick() -> glib::ControlFlow {
    let now = glib::monotonic_time();
    // Every avatar steps, so none can short-circuit the rest.
    let busy = all().iter().fold(false, |busy, a| a.step(now) | busy);
    if busy {
        return glib::ControlFlow::Continue;
    }
    STATE.with(|s| s.borrow_mut().timer = None);
    glib::ControlFlow::Break
}

// ---- Decoding -----------------------------------------------------------

/// The frames of a decode, each with its delay: the animation's when there
/// is one, else the single still.
unsafe fn frames_of(d: &HxInlineMediaDecoded) -> Vec<(gdk::Texture, u32)> {
    let arr = d.frames as *const glib::ffi::GArray;
    if !arr.is_null() && (*arr).len > 0 {
        let items = std::slice::from_raw_parts(
            (*arr).data as *const HxInlineMediaFrame,
            (*arr).len as usize,
        );
        return items
            .iter()
            .filter(|f| !f.texture.is_null())
            .map(|f| (from_glib_none(f.texture), f.delay_ms))
            .collect();
    }
    if d.texture.is_null() {
        return Vec::new();
    }
    vec![(from_glib_none(d.texture), 0)]
}

unsafe extern "C" fn on_decoded(result: *mut HxInlineMediaDecoded, user_data: *mut c_void) {
    // The key rides in user_data rather than a heap context, which would
    // leak when a cancel suppresses this callback. It is the key and not
    // the connection because the connection could be gone by now.
    let key = user_data as usize as u32;
    let token = STATE.with(|s| s.borrow_mut().pending.remove(&key));
    drop(token);

    let avatar = result
        .as_ref()
        .and_then(|d| AvatarPaintable::new(frames_of(d)));
    match &avatar {
        Some(a) => debug(&format!(
            "avatar decoded for uid={} ({} frame{})",
            key_uid(key),
            a.imp().frames.borrow().len(),
            if a.is_animated() { "s" } else { "" }
        )),
        None => debug(&format!(
            "avatar decode failed for uid={} (code={})",
            key_uid(key),
            result.as_ref().map_or(0, |d| d.error_code)
        )),
    }
    // A failed decode (not a GIF, oversize, corrupt) drops any stale
    // avatar, so the cell falls back to the numeric icon.
    STATE.with(|s| {
        let mut s = s.borrow_mut();
        match avatar {
            Some(a) => s.cache.insert(key, a),
            None => s.cache.remove(&key),
        }
    });
    inline_media_decoded_free(result);
    users_refresh_avatar(key_uid(key));
}

fn debug(msg: &str) {
    extern "C" {
        fn debug_log_str(cat: *const std::ffi::c_char, msg: *const std::ffi::c_char);
    }
    if let Ok(m) = std::ffi::CString::new(msg) {
        unsafe { debug_log_str(c"icon".as_ptr(), m.as_ptr()) };
    }
}

/// How many frames `uid`'s avatar has moved on, for the benchmark's checks.
pub(crate) fn steps(htlc: *mut c_void, uid: u16) -> Option<u64> {
    lookup(htlc, uid).map(|a| a.imp().steps.get())
}

// ---- The C ABI ------------------------------------------------------------

/// The avatar's frame to show now, or NULL when the user has none.
/// Transfer none.
///
/// # Safety
/// `htlc` is a live connection or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_avatar_get(
    htlc: *mut c_void,
    uid: u16,
) -> *mut gdk::ffi::GdkTexture {
    // The texture stays alive in the avatar's frame list after `shown`'s
    // clone drops.
    lookup(htlc, uid)
        .and_then(|a| a.shown())
        .map_or(std::ptr::null_mut(), |t| t.to_glib_none().0)
}

/// The user's avatar as a paintable that animates itself, or NULL when the
/// user has none. Transfer none; the same object for as long as the avatar
/// is unchanged.
///
/// # Safety
/// `htlc` is a live connection or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_avatar_get_paintable(
    htlc: *mut c_void,
    uid: u16,
) -> *mut gdk::ffi::GdkPaintable {
    lookup(htlc, uid).map_or(std::ptr::null_mut(), |a| {
        a.upcast_ref::<gdk::Paintable>().to_glib_none().0
    })
}

/// A user's GIF icon arrived: decode it and replace their avatar. An empty
/// one clears it.
///
/// # Safety
/// `htlc` is a live connection or NULL; `gif` is valid for `len` bytes or
/// NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_avatar_update(
    htlc: *mut c_void,
    uid: u16,
    gif: *const u8,
    len: usize,
) {
    if uid == 0 {
        return;
    }
    let key = avatar_key(htlc, uid);
    let superseded = STATE.with(|s| s.borrow_mut().pending.remove(&key));
    drop(superseded);

    if gif.is_null() || len == 0 {
        // The user dropped their avatar.
        STATE.with(|s| s.borrow_mut().cache.remove(&key));
        users_refresh_avatar(uid);
        return;
    }
    // A NULL token means the decode was refused on the spot, and the
    // callback has already run.
    let token = inline_media_decode_async(
        gif,
        len,
        &AVATAR_CAPS,
        on_decoded,
        key as usize as *mut c_void,
    );
    if !token.is_null() {
        STATE.with(|s| s.borrow_mut().pending.insert(key, Token(token)));
    }
}

/// Drop one connection's avatars and cancel its decodes, when its user list
/// is torn down. Other connections keep theirs.
///
/// # Safety
/// `htlc` is a live connection or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_avatar_clear_conn(htlc: *mut c_void) {
    let serial = avatar_key(htlc, 0);
    let (pending, cache) = STATE.with(|s| {
        let mut s = s.borrow_mut();
        let pending: Vec<Token> = {
            let keys: Vec<u32> = s
                .pending
                .keys()
                .copied()
                .filter(|k| k & 0xffff_0000 == serial)
                .collect();
            keys.iter().filter_map(|k| s.pending.remove(k)).collect()
        };
        let cache: Vec<AvatarPaintable> = {
            let keys: Vec<u32> = s
                .cache
                .keys()
                .copied()
                .filter(|k| k & 0xffff_0000 == serial)
                .collect();
            keys.iter().filter_map(|k| s.cache.remove(k)).collect()
        };
        (pending, cache)
    });
    // Cancel before anything else can run, so no late decode lands in a
    // cleared cache; drop the avatars outside the borrow.
    drop(pending);
    drop(cache);
}

/// The "animate avatars" preference.
#[no_mangle]
pub extern "C" fn gtkhx_avatar_set_animation_enabled(enabled: glib::ffi::gboolean) {
    let enabled = enabled != glib::ffi::GFALSE;
    let was = STATE.with(|s| !std::mem::replace(&mut s.borrow_mut().disabled, !enabled));
    if was == enabled {
        return;
    }
    if !enabled {
        stop_timer();
    }
    // Redraw every animated avatar: off snaps to the first frame, on
    // resumes each from its current one, timed from now rather than from
    // before the preference changed.
    let now = glib::monotonic_time();
    for a in all().iter().filter(|a| a.is_animated()) {
        if enabled && !a.is_paused() {
            a.imp().cur_since.set(now);
        }
        a.imp().drawn.set(false);
        a.invalidate_contents();
    }
}

/// Whether the user's avatar animates. False while animation is off, so
/// neither the click-to-pause gesture nor the Pause item offers itself
/// with nothing moving.
///
/// # Safety
/// `htlc` is a live connection or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_avatar_is_animated(
    htlc: *mut c_void,
    uid: u16,
) -> glib::ffi::gboolean {
    (anim_enabled() && lookup(htlc, uid).is_some_and(|a| a.is_animated())).into_glib()
}

/// # Safety
/// `htlc` is a live connection or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_avatar_is_paused(
    htlc: *mut c_void,
    uid: u16,
) -> glib::ffi::gboolean {
    lookup(htlc, uid).is_some_and(|a| a.is_paused()).into_glib()
}

/// Freeze or resume one user's avatar.
///
/// # Safety
/// `htlc` is a live connection or NULL.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_avatar_set_paused(
    htlc: *mut c_void,
    uid: u16,
    paused: glib::ffi::gboolean,
) {
    if let Some(a) = lookup(htlc, uid) {
        a.set_paused(paused != glib::ffi::GFALSE);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn texture() -> gdk::Texture {
        let bytes = glib::Bytes::from_owned(vec![0u8; 4 * 4 * 4]);
        gdk::MemoryTexture::new(4, 4, gdk::MemoryFormat::R8g8b8a8, &bytes, 16).upcast()
    }

    fn animated(delays: &[u32]) -> AvatarPaintable {
        AvatarPaintable::new(delays.iter().map(|&d| (texture(), d)).collect()).unwrap()
    }

    #[test]
    fn an_avatar_not_drawn_since_its_last_frame_stays_put() {
        let a = animated(&[100, 100]);
        let t0 = a.imp().cur_since.get();
        assert!(!a.step(t0 + 1_000_000), "undrawn, so no work");
        assert_eq!(a.imp().cur.get(), 0);
    }

    #[test]
    fn a_drawn_avatar_moves_on_once_its_delay_has_passed() {
        let a = animated(&[100, 50]);
        let t0 = a.imp().cur_since.get();
        a.imp().drawn.set(true);
        assert!(a.step(t0 + 99_000), "drawn and waiting");
        assert_eq!(a.imp().cur.get(), 0);
        assert!(a.step(t0 + 100_000));
        assert_eq!(a.imp().cur.get(), 1);
        // It must be drawn again before it moves again.
        assert!(!a.imp().drawn.get());
        assert!(!a.step(t0 + 1_000_000));
        a.imp().drawn.set(true);
        assert!(a.step(t0 + 1_000_000));
        assert_eq!(a.imp().cur.get(), 0, "wraps to the first frame");
    }

    #[test]
    fn a_zero_delay_frame_is_held_rather_than_spun() {
        let a = animated(&[0, 0]);
        let t0 = a.imp().cur_since.get();
        a.imp().drawn.set(true);
        a.step(t0 + i64::from(ZERO_DELAY_MS) * 1000 - 1);
        assert_eq!(a.imp().cur.get(), 0);
    }

    #[test]
    fn stills_and_paused_avatars_never_need_the_timer() {
        let still = AvatarPaintable::new(vec![(texture(), 0)]).unwrap();
        still.imp().drawn.set(true);
        assert!(!still.is_animated());
        assert!(!still.step(i64::MAX));

        let a = animated(&[100, 100]);
        a.imp().paused.set(true);
        a.imp().drawn.set(true);
        assert!(!a.step(i64::MAX));
        assert_eq!(a.imp().cur.get(), 0);
    }

    #[test]
    fn no_frames_is_no_avatar() {
        assert!(AvatarPaintable::new(Vec::new()).is_none());
    }

    #[test]
    fn the_key_keeps_the_uid_whole() {
        let key = avatar_key(std::ptr::null_mut(), 0xbeef);
        assert_eq!(key, 0xbeef, "no connection is serial 0");
        assert_eq!(key_uid(key), 0xbeef);
    }
}
