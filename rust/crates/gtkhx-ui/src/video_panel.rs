//! The Video panel: the room's cameras and screen shares, as tiles.
//!
//! One page per connection in a dockable panel, like Users. It shows the
//! voice room this connection is in as tiles laid out to fill the panel
//! (see `video_grid`), and this client's own publications as "You" tiles
//! from the capture's preview. A paused publication keeps its tile, marked
//! paused — the spec's present-but-paused.
//!
//! One tile can be in focus: it takes the stage at the top and the others
//! go smaller below. Someone's screen share is in focus until the user
//! picks another or none. Each tile's controls focus it, mute its user's
//! voice here, and stop watching it — which takes its tile away, stops
//! receiving it, and leaves a button at the bottom to watch it again.
//!
//! **What this client receives is decided here.** The server delivers no
//! video until asked (Video Subscribe, 610), and asks are the complete
//! set, so the panel is the one place that computes it: while its page is
//! mapped, the publications whose tiles are in view or about to be; while
//! hidden — another tab, a collapsed dock, the window withdrawn — nothing.
//! A collapsed panel or a tile scrolled away costs no bandwidth and no
//! decoding, which is the spec's reason 610 takes a whole set. Changes are
//! debounced so a burst of 611s costs one request, scrolling and resizes
//! send one set once the view comes to rest, and the state machine drops a
//! set that hasn't changed.
//!
//! The runtime is reached through its Rust API directly: `gtkhx-ui` links
//! it, and the panel is main-thread code talking to a main-thread object.
//! It is looked up through the session every time rather than held,
//! because a disconnect frees it.

use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet};
use std::ffi::{c_char, c_void};
use std::rc::{Rc, Weak};

use gtk4 as gtk;
use gtk4::gdk;
use gtk4::glib;
use gtk4::glib::translate::IntoGlibPtr;
use gtk4::prelude::*;
use libadwaita as adw;

use hxvoice_runtime::hxvoice::{SessionState, Stream, VideoKind};
use hxvoice_runtime::runtime::{VideoNotice, VoiceRuntime};
use hxvoice_runtime::video::{self_key, StreamKey, VideoFrame};

use crate::dock;
use crate::tr::tr;
use crate::video_grid::VideoGrid;

/// How long the subscription set may settle before it goes out.
const SUBSCRIBE_DEBOUNCE_MS: u64 = 150;

/// How far outside the view a tile starts being received, in view
/// heights. A stream takes a renegotiation and a keyframe to appear, so it
/// is asked for before it scrolls in.
const RECEIVE_AHEAD: f64 = 0.5;
/// How far outside the view a tile being received is let go, in view
/// heights. Wider than `RECEIVE_AHEAD`, so scrolling back and forth
/// across one edge doesn't renegotiate each time.
const RECEIVE_BEHIND: f64 = 1.5;

extern "C" {
    fn toolbar_present_panel(
        id: *const std::ffi::c_char,
        sess: *mut c_void,
        respect_saved_state: glib::ffi::gboolean,
    );
    fn hx_session_voice_runtime(sess: *mut c_void) -> *mut c_void;
    fn hx_session_voice_model(sess: *mut c_void) -> *mut c_void;
    fn hx_printf_prefix(
        htlc: *mut c_void,
        cid: u32,
        prefix: *const c_char,
        fmt: *const c_char,
        ...
    );
    static INFOPREFIX: *const c_char;
    fn hx_session_with_serial(serial: u16) -> *mut c_void;
    fn hx_session_htlc(sess: *mut c_void) -> *mut c_void;
    fn hx_htlc_video_cap(htlc: *mut c_void) -> glib::ffi::gboolean;
    fn hx_htlc_uid(htlc: *mut c_void) -> u16;
    fn chat_with_cid(sess: *mut c_void, cid: u32) -> *mut c_void;
    fn hx_chat_member_model(chat: *mut c_void) -> *mut c_void;
    fn hx_member_model_get_info(
        model: *mut c_void,
        uid: u16,
        out: *mut MemberInfo,
    ) -> glib::ffi::gboolean;
}

/// `#[repr(C)]` mirror of `struct hx_member_info` (chat_members.h).
#[repr(C)]
struct MemberInfo {
    uid: u16,
    icon: u16,
    status: u16,
    nick_color: u32,
    name: [c_char; 32],
}

/// The runtime for a session, if one exists.
///
/// # Safety
/// `sess` is NULL or a live session. The reference is valid until the
/// next disconnect, so it must not be kept past the current call.
pub(crate) unsafe fn runtime<'a>(sess: *mut c_void) -> Option<&'a VoiceRuntime> {
    if sess.is_null() {
        return None;
    }
    let rt = hx_session_voice_runtime(sess) as *const VoiceRuntime;
    rt.as_ref()
}

/// A user's nick in room `cid`, or a placeholder naming the uid.
unsafe fn nick(sess: *mut c_void, cid: u32, uid: u16) -> String {
    let chat = chat_with_cid(sess, cid);
    let model = if chat.is_null() {
        std::ptr::null_mut()
    } else {
        hx_chat_member_model(chat)
    };
    if !model.is_null() {
        let mut info = MemberInfo {
            uid: 0,
            icon: 0,
            status: 0,
            nick_color: 0,
            name: [0; 32],
        };
        if hx_member_model_get_info(model, uid, &mut info) != 0 {
            let bytes: Vec<u8> = info
                .name
                .iter()
                .take_while(|c| **c != 0)
                .map(|c| *c as u8)
                .collect();
            return String::from_utf8_lossy(&bytes).into_owned();
        }
    }
    crate::tr::tr1("User %s", &uid.to_string())
}

// ---------------------------------------------------------------------
// Tiles.
// ---------------------------------------------------------------------

struct Tile {
    root: gtk::Overlay,
    picture: gtk::Picture,
    name: gtk::Label,
    paused: gtk::Label,
    focus: gtk::Button,
    mute: gtk::Button,
    /// The newest frame's size, to notice a stream changing shape.
    size: Cell<(u32, u32)>,
}

/// Tiles show their controls while the pointer is over them or one has
/// keyboard focus; a muted user's mute button stays up as the mark of it.
const CSS: &str = "
.hx-video-tile .hx-video-controls > button {
  opacity: 0;
  transition: opacity 150ms ease-out;
}
.hx-video-tile:hover .hx-video-controls > button,
.hx-video-tile:focus-within .hx-video-controls > button,
.hx-video-tile .hx-video-controls > button.hx-video-muted {
  opacity: 1;
}
";

/// Once per process: the style is display-wide and the same for every panel.
fn install_css() {
    thread_local! {
        static INSTALLED: Cell<bool> = const { Cell::new(false) };
    }
    if INSTALLED.get() {
        return;
    }
    let Some(display) = gdk::Display::default() else {
        return;
    };
    INSTALLED.set(true);
    let css = gtk::CssProvider::new();
    // load_from_string is GTK 4.12, above the bindings' floor. Deprecated
    // only when a newer binding feature is on, as the chat view's
    // accessibility turns on.
    #[allow(deprecated)]
    css.load_from_data(CSS);
    gtk::style_context_add_provider_for_display(
        &display,
        &css,
        gtk::STYLE_PROVIDER_PRIORITY_APPLICATION,
    );
}

fn control(icon: &str, tooltip: &str) -> gtk::Button {
    let b = gtk::Button::from_icon_name(icon);
    b.set_tooltip_text(Some(tooltip));
    b.add_css_class("osd");
    b.add_css_class("circular");
    b
}

impl Tile {
    /// `key`'s tile, its controls acting on `panel`. This client's own
    /// preview has no mute: there is nothing of its own to hear.
    fn new(key: StreamKey, panel: Weak<PanelInner>) -> Tile {
        let picture = gtk::Picture::new();
        picture.set_can_shrink(true);
        picture.set_content_fit(gtk::ContentFit::Contain);
        picture.add_css_class("hx-video-picture");

        let root = gtk::Overlay::new();
        root.set_child(Some(&picture));
        root.add_css_class("card");
        root.add_css_class("hx-video-tile");
        root.set_overflow(gtk::Overflow::Hidden);

        let name = gtk::Label::new(None);
        name.set_halign(gtk::Align::Start);
        name.set_valign(gtk::Align::End);
        name.set_margin_start(6);
        name.set_margin_end(6);
        name.set_margin_bottom(6);
        name.set_ellipsize(gtk::pango::EllipsizeMode::End);
        name.add_css_class("osd");
        name.add_css_class("caption");
        root.add_overlay(&name);

        let paused = gtk::Label::new(Some(&match key.kind {
            VideoKind::Camera => tr("Camera paused"),
            VideoKind::Screen => tr("Sharing paused"),
        }));
        paused.set_halign(gtk::Align::Center);
        paused.set_valign(gtk::Align::Center);
        paused.add_css_class("osd");
        paused.set_visible(false);
        root.add_overlay(&paused);

        let focus = control("view-fullscreen-symbolic", &tr("Focus"));
        let mute = control("audio-volume-high-symbolic", &tr("Mute"));
        mute.set_visible(key.user_id != 0);
        let unwatch = control(
            "window-close-symbolic",
            &if key.user_id == 0 {
                tr("Hide")
            } else {
                tr("Stop watching")
            },
        );
        let controls = gtk::Box::new(gtk::Orientation::Horizontal, 4);
        controls.add_css_class("hx-video-controls");
        controls.set_halign(gtk::Align::End);
        controls.set_valign(gtk::Align::Start);
        controls.set_margin_top(6);
        controls.set_margin_end(6);
        controls.append(&mute);
        controls.append(&focus);
        controls.append(&unwatch);
        root.add_overlay(&controls);

        let on = |f: fn(&Rc<PanelInner>, StreamKey)| {
            let panel = panel.clone();
            move || {
                if let Some(p) = panel.upgrade() {
                    f(&p, key);
                }
            }
        };
        let toggle_focus = on(PanelInner::toggle_focus);
        focus.connect_clicked(move |_| toggle_focus());
        let toggle_mute = on(PanelInner::toggle_mute);
        mute.connect_clicked(move |_| toggle_mute());
        let stop = on(PanelInner::stop_watching);
        unwatch.connect_clicked(move |_| stop());

        // A click anywhere else on the tile toggles its focus too. The
        // buttons claim their own clicks, so this never sees theirs.
        let click = gtk::GestureClick::new();
        let toggle_focus = on(PanelInner::toggle_focus);
        click.connect_released(move |g, n, _, _| {
            if n == 1 {
                g.set_state(gtk::EventSequenceState::Claimed);
                toggle_focus();
            }
        });
        root.add_controller(click);
        // The volume can change from the user list; look again whenever the
        // controls are about to show.
        let motion = gtk::EventControllerMotion::new();
        let sync = on(|p, _| p.sync_mute());
        motion.connect_enter(move |_, _, _| sync());
        root.add_controller(motion);

        Tile {
            root,
            picture,
            name,
            paused,
            focus,
            mute,
            size: Cell::new((0, 0)),
        }
    }

    /// Show `frame`. Returns its shape when it differs from the last one's.
    fn show(&self, frame: &VideoFrame) -> Option<f64> {
        let texture = gdk::MemoryTexture::new(
            frame.width as i32,
            frame.height as i32,
            gdk::MemoryFormat::R8g8b8a8,
            &frame.bytes,
            frame.stride as usize,
        );
        self.picture.set_paintable(Some(&texture));
        let size = (frame.width, frame.height);
        (self.size.replace(size) != size && frame.height > 0)
            .then(|| f64::from(frame.width) / f64::from(frame.height))
    }

    fn set_focused(&self, focused: bool) {
        if focused {
            self.focus.set_icon_name("view-restore-symbolic");
            self.focus.set_tooltip_text(Some(&tr("Show all")));
        } else {
            self.focus.set_icon_name("view-fullscreen-symbolic");
            self.focus.set_tooltip_text(Some(&tr("Focus")));
        }
    }

    fn set_muted(&self, muted: bool) {
        if muted {
            self.mute.set_icon_name("audio-volume-muted-symbolic");
            self.mute.set_tooltip_text(Some(&tr("Unmute")));
            self.mute.add_css_class("hx-video-muted");
        } else {
            self.mute.set_icon_name("audio-volume-high-symbolic");
            self.mute.set_tooltip_text(Some(&tr("Mute")));
            self.mute.remove_css_class("hx-video-muted");
        }
    }
}

/// Which tile is on the stage.
#[derive(Debug, Clone, Copy, PartialEq)]
enum Focus {
    /// Someone else's screen share, when there is one; none otherwise.
    Auto,
    /// The one the user picked.
    On(StreamKey),
    /// None: the user asked for them all alike.
    Off,
}

// ---------------------------------------------------------------------
// Per-panel state.
// ---------------------------------------------------------------------

struct PanelInner {
    conn: dock::ConnKey,
    /// Weak: the root owns this state (as widget data), so a strong ref
    /// back would keep both alive forever.
    root: glib::WeakRef<gtk::Box>,
    /// Itself, for the tiles' controls to reach it by.
    this: Weak<PanelInner>,
    stack: gtk::Stack,
    status: adw::StatusPage,
    grid: VideoGrid,
    scroll: gtk::ScrolledWindow,
    tiles: RefCell<HashMap<StreamKey, Tile>>,
    /// The room's streams as the last refresh found them: (stream, paused,
    /// label). Tiles are made from it, less the unwatched.
    want: RefCell<Vec<(StreamKey, bool, String)>>,
    /// Whether the server offers video, and whether this connection is
    /// in a voice room, as the last refresh found them: what the empty
    /// page says.
    video: Cell<bool>,
    in_room: Cell<bool>,
    /// The room the user's choices below were made in — the runtime's id
    /// and the chat's — so they are dropped on a move to another room or a
    /// reconnect, and kept across a rejoin of the same one.
    room: Cell<Option<(u64, u32)>>,
    focus: Cell<Focus>,
    /// Streams the user stopped watching: no tile, not received, and a
    /// button in the bar at the bottom to watch again.
    unwatched: RefCell<HashSet<StreamKey>>,
    unwatched_bar: gtk::Box,
    unwatched_list: gtk::FlowBox,
    /// Each muted user's volume before the mute, to go back to. Uids are
    /// the connection's, so a new runtime starts it afresh.
    premute: RefCell<HashMap<u16, f64>>,
    /// The runtime this panel has an observer on, by id (0 for none).
    observing: Cell<u64>,
    subscribe_timer: RefCell<Option<glib::SourceId>>,
}

thread_local! {
    static PANELS: RefCell<Vec<Weak<PanelInner>>> = const { RefCell::new(Vec::new()) };
}

fn for_each_panel(conn: Option<dock::ConnKey>, mut f: impl FnMut(&Rc<PanelInner>)) {
    let live: Vec<Rc<PanelInner>> = PANELS.with(|p| {
        let mut v = p.borrow_mut();
        v.retain(|w| w.upgrade().is_some());
        v.iter().filter_map(Weak::upgrade).collect()
    });
    for p in live.iter().filter(|p| conn.is_none_or(|c| c == p.conn)) {
        f(p);
    }
}

impl PanelInner {
    fn sess(&self) -> *mut c_void {
        unsafe { hx_session_with_serial(self.conn) }
    }

    /// Hook this panel to its connection's runtime, once per runtime. The
    /// observer holds the panel weakly and unregisters itself when the
    /// panel is gone.
    fn attach(self: &Rc<Self>) {
        let sess = self.sess();
        let Some(rt) = (unsafe { runtime(sess) }) else {
            self.observing.set(0);
            return;
        };
        let id = rt.id();
        if self.observing.get() == id {
            return;
        }
        self.observing.set(id);
        self.premute.borrow_mut().clear();
        let weak = Rc::downgrade(self);
        rt.add_video_observer(Box::new(move |rt, notice| {
            let Some(panel) = weak.upgrade() else {
                return false;
            };
            if panel.observing.get() != rt.id() {
                return false;
            }
            panel.on_notice(rt, notice);
            true
        }));
    }

    fn on_notice(self: &Rc<Self>, rt: &VoiceRuntime, notice: &VideoNotice) {
        match notice {
            VideoNotice::Frames => self.pull_frames(|key| rt.take_video_frame(key)),
            VideoNotice::StreamEnded(key) => {
                if let Some(t) = self.tiles.borrow().get(key) {
                    t.picture.set_paintable(None::<&gdk::Paintable>);
                }
            }
            VideoNotice::Volume(_) => self.sync_mute(),
            VideoNotice::Publications | VideoNotice::Local(_) | VideoNotice::Session(_) => {
                self.refresh();
            }
        }
    }

    /// Show the newest frame of every tile that has one waiting.
    fn pull_frames(&self, take: impl Fn(StreamKey) -> Option<VideoFrame>) {
        for (key, tile) in self.tiles.borrow().iter() {
            if let Some(aspect) = take(*key).and_then(|frame| tile.show(&frame)) {
                self.grid.set_aspect(*key, aspect);
            }
        }
    }

    /// Rebuild the tile set from the runtime's view of the room.
    fn refresh(self: &Rc<Self>) {
        self.attach();
        let sess = self.sess();
        let rt = unsafe { runtime(sess) };
        let htlc = unsafe { hx_session_htlc(sess) };
        let video = !htlc.is_null() && unsafe { hx_htlc_video_cap(htlc) } != 0;
        let cid = rt.and_then(|r| r.active_cid());
        let in_room = rt.is_some_and(|r| {
            matches!(
                r.state(),
                SessionState::OfferPending | SessionState::Connecting | SessionState::Connected
            )
        });

        if let (Some(rt), Some(cid)) = (rt, cid) {
            let room = (rt.id(), cid);
            if self.room.replace(Some(room)) != Some(room) {
                self.new_room();
            }
        }
        self.video.set(video);
        self.in_room.set(in_room);

        // (key, paused, label)
        let mut want: Vec<(StreamKey, bool, String)> = Vec::new();
        if let (Some(rt), true, Some(cid)) = (rt, in_room, cid) {
            let me = unsafe { hx_htlc_uid(htlc) };
            for p in rt.video_publications() {
                if p.user_id == me {
                    continue; // our own is shown from the local preview
                }
                let label = unsafe { nick(sess, cid, p.user_id) };
                want.push((
                    StreamKey {
                        user_id: p.user_id,
                        kind: p.kind,
                    },
                    p.paused,
                    label,
                ));
            }
            for kind in VideoKind::ALL {
                if let Some(paused) = rt.video_local(kind) {
                    let label = match kind {
                        VideoKind::Camera => tr("You"),
                        VideoKind::Screen => tr("Your screen"),
                    };
                    want.push((self_key(kind), paused, label));
                }
            }
        }

        self.set_tiles(&want);
        self.schedule_subscribe();
    }

    /// Forget the choices made in the last room: what to focus, what not
    /// to watch.
    fn new_room(&self) {
        self.focus.set(Focus::Auto);
        self.unwatched.borrow_mut().clear();
    }

    /// Lay the tiles out again from the last refresh's streams, after a
    /// change of the user's own: what to watch, what to focus.
    fn relayout(self: &Rc<Self>) {
        let want = self.want.borrow().clone();
        self.set_tiles(&want);
        self.schedule_subscribe();
    }

    /// The tile on the stage: the user's pick while it has a tile, or
    /// someone else's screen share unless the user asked for none. A lone
    /// tile has the panel to itself, and no stage.
    fn focused_key(&self) -> Option<StreamKey> {
        let tiles = self.tiles.borrow();
        if tiles.len() < 2 {
            return None;
        }
        match self.focus.get() {
            Focus::On(key) if tiles.contains_key(&key) => Some(key),
            Focus::Off => None,
            _ => tiles
                .keys()
                .filter(|k| k.kind == VideoKind::Screen && k.user_id != 0)
                .min_by_key(|k| k.user_id)
                .copied(),
        }
    }

    fn apply_focus(&self) {
        let focused = self.focused_key();
        self.grid.set_focused(focused);
        let tiles = self.tiles.borrow();
        for (key, tile) in tiles.iter() {
            tile.set_focused(Some(*key) == focused);
            tile.focus.set_visible(tiles.len() > 1);
        }
    }

    /// Put `key` on the stage, or take it off if it is there. A lone tile
    /// has no stage to go on, and a click on it changes nothing.
    fn toggle_focus(self: &Rc<Self>, key: StreamKey) {
        if self.tiles.borrow().len() < 2 {
            return;
        }
        self.focus.set(if self.focused_key() == Some(key) {
            Focus::Off
        } else {
            Focus::On(key)
        });
        self.apply_focus();
        // The layout moved under the view without necessarily changing its
        // extent: look again at what is in reach.
        self.schedule_subscribe();
    }

    /// Stop receiving `key` and give its tile's room to the others.
    fn stop_watching(self: &Rc<Self>, key: StreamKey) {
        self.unwatched.borrow_mut().insert(key);
        self.relayout();
    }

    fn watch(self: &Rc<Self>, key: StreamKey) {
        self.unwatched.borrow_mut().remove(&key);
        self.relayout();
    }

    /// Mute `key`'s user here, or give them back the volume they had. It is
    /// their voice, so it is the same for their camera and their screen,
    /// and the same as the user list's volume slider at zero.
    fn toggle_mute(self: &Rc<Self>, key: StreamKey) {
        let Some(rt) = (unsafe { runtime(self.sess()) }) else {
            return;
        };
        let uid = key.user_id;
        let volume = rt.user_volume(uid);
        if volume > 0.0 {
            self.premute.borrow_mut().insert(uid, volume);
            rt.set_user_volume(uid, 0.0);
        } else {
            let back = self.premute.borrow_mut().remove(&uid).unwrap_or(1.0);
            rt.set_user_volume(uid, back);
        }
    }

    /// Show each tile's user as muted or not, as the runtime has them.
    fn sync_mute(&self) {
        let rt = unsafe { runtime(self.sess()) };
        for (key, tile) in self.tiles.borrow().iter() {
            let muted = key.user_id != 0 && rt.is_some_and(|r| r.user_volume(key.user_id) == 0.0);
            tile.set_muted(muted);
        }
    }

    /// The bar's buttons, one per unwatched stream in `want`.
    fn fill_unwatched(&self, want: &[(StreamKey, bool, String)]) {
        while let Some(child) = self.unwatched_list.first_child() {
            self.unwatched_list.remove(&child);
        }
        let unwatched = self.unwatched.borrow();
        let mut streams: Vec<_> = want
            .iter()
            .filter(|(k, _, _)| unwatched.contains(k))
            .collect();
        streams.sort_by_key(|(k, _, _)| (k.user_id, k.kind));
        for (key, _, label) in streams {
            let content = adw::ButtonContent::new();
            content.set_icon_name(match key.kind {
                VideoKind::Camera => "camera-video-symbolic",
                VideoKind::Screen => "screen-shared-symbolic",
            });
            content.set_label(label);
            // A long nick ellipsizes rather than widening the panel.
            content.set_can_shrink(true);
            let button = gtk::Button::new();
            button.set_child(Some(&content));
            button.add_css_class("flat");
            button.set_tooltip_text(Some(&tr("Watch")));
            let (panel, key) = (self.this.clone(), *key);
            button.connect_clicked(move |_| {
                if let Some(p) = panel.upgrade() {
                    p.watch(key);
                }
            });
            self.unwatched_list.append(&button);
        }
        self.unwatched_bar
            .set_visible(self.unwatched_list.first_child().is_some());
    }

    /// Make the tiles exactly `want` — (stream, paused, label) — less the
    /// streams the user stopped watching, and show them, or the status page
    /// when there are none. Returns whether there are any.
    fn set_tiles(&self, want: &[(StreamKey, bool, String)]) -> bool {
        *self.want.borrow_mut() = want.to_vec();
        {
            let unwatched = self.unwatched.borrow();
            let shown =
                |key: &StreamKey| !unwatched.contains(key) && want.iter().any(|(k, _, _)| k == key);
            let mut tiles = self.tiles.borrow_mut();
            tiles.retain(|key, _| {
                let keep = shown(key);
                if !keep {
                    self.grid.remove(*key);
                }
                keep
            });
            for (key, paused, label) in want.iter().filter(|(k, _, _)| shown(k)) {
                let tile = tiles.entry(*key).or_insert_with(|| {
                    let t = Tile::new(*key, self.this.clone());
                    self.grid.insert(*key, &t.root);
                    t
                });
                tile.name.set_text(label);
                tile.paused.set_visible(*paused);
                if *paused {
                    tile.picture.set_paintable(None::<&gdk::Paintable>);
                }
            }
        }
        self.apply_focus();
        self.fill_unwatched(want);
        self.sync_mute();

        let has_tiles = !self.tiles.borrow().is_empty();
        if !has_tiles {
            self.status.set_description(Some(&if !self.video.get() {
                tr("This server doesn't support video.")
            } else if !self.in_room.get() {
                tr("Join voice to see who has a camera or screen on.")
            } else if !want.is_empty() {
                tr("You aren't watching anyone. Pick someone below to watch.")
            } else {
                tr("Nobody in this voice chat has a camera or screen on.")
            }));
        }
        self.stack
            .set_visible_child_name(if has_tiles { "tiles" } else { "empty" });
        has_tiles
    }

    /// `uid` in room `cid` changed nick: rename their tiles in place. A
    /// full refresh would do it too, but it also clears paused pictures
    /// and resends the receive set, which a rename doesn't call for.
    fn user_changed(&self, cid: u32, uid: u16) {
        let sess = self.sess();
        let Some(rt) = (unsafe { runtime(sess) }) else {
            return;
        };
        if rt.active_cid() != Some(cid) {
            return;
        }
        let label = unsafe { nick(sess, cid, uid) };
        relabel(&self.tiles.borrow(), uid, &label);
        let mut renamed = false;
        for (key, _, l) in self.want.borrow_mut().iter_mut() {
            if key.user_id == uid && uid != 0 {
                l.clone_from(&label);
                renamed = true;
            }
        }
        if renamed {
            let want = self.want.borrow().clone();
            self.fill_unwatched(&want);
        }
    }

    /// Queue the receive set to be recomputed and sent.
    fn schedule_subscribe(self: &Rc<Self>) {
        if self.subscribe_timer.borrow().is_some() {
            return;
        }
        self.arm_subscribe();
    }

    /// Queue the receive set once the view has stopped moving: every
    /// call puts it off again, so a scroll or a fling sends one set where
    /// it comes to rest rather than one per debounce along the way. Each
    /// set that differs costs the server a renegotiation.
    fn schedule_subscribe_settled(self: &Rc<Self>) {
        if let Some(id) = self.subscribe_timer.borrow_mut().take() {
            id.remove();
        }
        self.arm_subscribe();
    }

    fn arm_subscribe(self: &Rc<Self>) {
        let weak = Rc::downgrade(self);
        let id = glib::timeout_add_local_once(
            std::time::Duration::from_millis(SUBSCRIBE_DEBOUNCE_MS),
            move || {
                if let Some(p) = weak.upgrade() {
                    p.subscribe_timer.borrow_mut().take();
                    p.send_subscriptions();
                }
            },
        );
        *self.subscribe_timer.borrow_mut() = Some(id);
    }

    /// Where each tile sits in the scrolled content, top and bottom, and
    /// the span in view. `None` for what isn't laid out yet.
    fn layout(&self) -> (Vec<(StreamKey, Option<Span>)>, Option<Span>) {
        let adj = self.scroll.vadjustment();
        let view = (adj.page_size() > 0.0).then(|| (adj.value(), adj.value() + adj.page_size()));
        let tiles = self
            .tiles
            .borrow()
            .iter()
            .map(|(key, tile)| {
                let span = tile
                    .root
                    .compute_bounds(&self.grid)
                    .filter(|b| b.height() > 0.0)
                    .map(|b| (f64::from(b.y()), f64::from(b.y() + b.height())));
                (*key, span)
            })
            .collect();
        (tiles, view)
    }

    /// The publications in the room while this page is on screen, less
    /// those whose tiles are scrolled well away (see `in_reach`); nothing
    /// while it isn't. Paused publications are included: the subscription
    /// survives the pause, and resuming then costs no renegotiation.
    fn send_subscriptions(&self) {
        let sess = self.sess();
        let Some(rt) = (unsafe { runtime(sess) }) else {
            return;
        };
        let htlc = unsafe { hx_session_htlc(sess) };
        let me = if htlc.is_null() {
            0
        } else {
            unsafe { hx_htlc_uid(htlc) }
        };
        let visible = self.root.upgrade().is_some_and(|r| r.is_mapped());
        let streams: Vec<Stream> = if visible {
            let receiving: Vec<StreamKey> = rt
                .video_subscriptions()
                .iter()
                .map(|s| StreamKey {
                    user_id: s.user_id,
                    kind: s.kind,
                })
                .collect();
            let (tiles, view) = self.layout();
            let reach = in_reach(&tiles, view, &receiving);
            let tiled = |key: &StreamKey| tiles.iter().any(|(k, _)| k == key);
            let unwatched = self.unwatched.borrow();
            rt.video_publications()
                .into_iter()
                .filter(|p| p.user_id != me)
                .filter(|p| {
                    let key = StreamKey {
                        user_id: p.user_id,
                        kind: p.kind,
                    };
                    // A publication with no tile yet is one the next
                    // refresh adds, unless the user stopped watching it:
                    // receive it rather than wait.
                    !unwatched.contains(&key) && (!tiled(&key) || reach.contains(&key))
                })
                .map(|p| Stream {
                    user_id: p.user_id,
                    kind: p.kind,
                })
                .collect()
        } else {
            Vec::new()
        };
        rt.video_subscribe(streams);
    }
}

/// A vertical extent in the scrolled content: top, bottom.
type Span = (f64, f64);

/// Which of `tiles` to receive: those within `RECEIVE_AHEAD` view heights
/// of `view`, and those already in `receiving` within `RECEIVE_BEHIND`.
/// Each tile comes with its top and bottom in the scrolled content; one
/// not laid out yet, or a view not laid out yet, means receive it — the
/// first layout then narrows the set.
fn in_reach(
    tiles: &[(StreamKey, Option<Span>)],
    view: Option<Span>,
    receiving: &[StreamKey],
) -> Vec<StreamKey> {
    tiles
        .iter()
        .filter(|(key, span)| {
            let (Some((top, bottom)), Some((v_top, v_bottom))) = (*span, view) else {
                return true;
            };
            let margin = if receiving.contains(key) {
                RECEIVE_BEHIND
            } else {
                RECEIVE_AHEAD
            } * (v_bottom - v_top);
            bottom > v_top - margin && top < v_bottom + margin
        })
        .map(|(key, _)| *key)
        .collect()
}

fn build_content(sess: *mut c_void) -> (gtk::Box, Rc<PanelInner>) {
    let (root, inner) = build_panel(dock::key_for_session(sess));
    inner.refresh();
    (root, inner)
}

/// The panel's widgets and state for connection `conn`, before it has
/// read the room.
fn build_panel(conn: dock::ConnKey) -> (gtk::Box, Rc<PanelInner>) {
    let root = gtk::Box::new(gtk::Orientation::Vertical, 0);
    root.set_hexpand(true);
    root.set_vexpand(true);

    let status = adw::StatusPage::new();
    status.set_icon_name(Some("camera-video-symbolic"));
    status.set_title(&tr("No Video"));
    status.add_css_class("compact");

    let grid = VideoGrid::new();
    grid.set_margin_start(6);
    grid.set_margin_end(6);
    grid.set_margin_top(6);
    grid.set_margin_bottom(6);
    let scroll = gtk::ScrolledWindow::new();
    scroll.set_hscrollbar_policy(gtk::PolicyType::Never);
    scroll.set_vexpand(true);
    scroll.set_child(Some(&grid));

    // Streams the user stopped watching, to watch again.
    let unwatched_list = gtk::FlowBox::new();
    unwatched_list.set_selection_mode(gtk::SelectionMode::None);
    unwatched_list.set_hexpand(true);
    unwatched_list.set_max_children_per_line(8);
    unwatched_list.set_column_spacing(2);
    unwatched_list.set_row_spacing(2);
    let unwatched_label = gtk::Label::new(Some(&tr("Not watching")));
    unwatched_label.add_css_class("dim-label");
    unwatched_label.add_css_class("caption");
    unwatched_label.set_valign(gtk::Align::Center);
    let unwatched_row = gtk::Box::new(gtk::Orientation::Horizontal, 6);
    unwatched_row.set_margin_start(6);
    unwatched_row.set_margin_end(6);
    unwatched_row.set_margin_top(2);
    unwatched_row.set_margin_bottom(2);
    unwatched_row.append(&unwatched_label);
    unwatched_row.append(&unwatched_list);
    let unwatched_bar = gtk::Box::new(gtk::Orientation::Vertical, 0);
    unwatched_bar.append(&gtk::Separator::new(gtk::Orientation::Horizontal));
    unwatched_bar.append(&unwatched_row);
    unwatched_bar.set_visible(false);

    let stack = gtk::Stack::new();
    stack.add_named(&status, Some("empty"));
    stack.add_named(&scroll, Some("tiles"));
    stack.set_vexpand(true);
    root.append(&stack);
    root.append(&unwatched_bar);
    install_css();

    let inner = Rc::new_cyclic(|this| PanelInner {
        conn,
        root: root.downgrade(),
        this: this.clone(),
        stack,
        status,
        grid,
        scroll,
        tiles: RefCell::new(HashMap::new()),
        want: RefCell::new(Vec::new()),
        video: Cell::new(false),
        in_room: Cell::new(false),
        room: Cell::new(None),
        focus: Cell::new(Focus::Auto),
        unwatched: RefCell::new(HashSet::new()),
        unwatched_bar,
        unwatched_list,
        premute: RefCell::new(HashMap::new()),
        observing: Cell::new(0),
        subscribe_timer: RefCell::new(None),
    });

    // Visibility is the subscription policy: mapping and unmapping the
    // page (tab switches, dock collapse, a withdrawn window) recompute it,
    // and so do scrolling and anything that changes the scrolled extent —
    // a resize, tiles coming and going.
    {
        let adj = inner.scroll.vadjustment();
        let weak = Rc::downgrade(&inner);
        adj.connect_value_changed(move |_| {
            if let Some(p) = weak.upgrade() {
                p.schedule_subscribe_settled();
            }
        });
        // The vertical extent changes with the rows; the horizontal one
        // catches a width change that reflows tiles between rows without
        // changing how many there are.
        for adj in [adj, inner.scroll.hadjustment()] {
            let weak = Rc::downgrade(&inner);
            adj.connect_changed(move |_| {
                if let Some(p) = weak.upgrade() {
                    p.schedule_subscribe_settled();
                }
            });
        }
        let weak = Rc::downgrade(&inner);
        root.connect_map(move |_| {
            if let Some(p) = weak.upgrade() {
                p.schedule_subscribe();
            }
        });
        let weak = Rc::downgrade(&inner);
        root.connect_unmap(move |_| {
            if let Some(p) = weak.upgrade() {
                p.schedule_subscribe();
            }
        });
        let weak = Rc::downgrade(&inner);
        root.connect_destroy(move |_| {
            if let Some(p) = weak.upgrade() {
                if let Some(id) = p.subscribe_timer.borrow_mut().take() {
                    id.remove();
                }
                // Closing the panel unmaps and destroys it in the same turn,
                // so the unmap's debounced send never runs. Say "nothing"
                // now, or the server keeps streaming to a panel that is gone.
                if let Some(rt) = unsafe { runtime(p.sess()) } {
                    rt.video_subscribe(Vec::new());
                }
                p.observing.set(0);
            }
        });
    }

    PANELS.with(|p| p.borrow_mut().push(Rc::downgrade(&inner)));
    // The panel owns itself through the widget: dropping the last strong
    // ref with the widget is what unregisters its observer.
    unsafe { root.set_data("video-panel-state", inner.clone()) };
    (root, inner)
}

/// A Video panel tied to no connection, for the UI benchmark: it shows
/// the tiles it is given and the frames it is fed, through the code a
/// room's streams go through.
pub(crate) struct Standalone {
    pub(crate) root: gtk::Box,
    inner: Rc<PanelInner>,
}

impl Standalone {
    pub(crate) fn new() -> Standalone {
        // Serial 0 is no connection: no runtime is ever found for it, and
        // no session's refresh reaches it.
        let (root, inner) = build_panel(0);
        inner.video.set(true);
        inner.in_room.set(true);
        Standalone { root, inner }
    }

    /// Show a tile, unpaused, for each (stream, label).
    pub(crate) fn set_tiles(&self, tiles: &[(StreamKey, String)]) {
        let want: Vec<_> = tiles.iter().map(|(k, l)| (*k, false, l.clone())).collect();
        self.inner.set_tiles(&want);
    }

    /// What a frames notice does: show each tile's newest frame.
    pub(crate) fn pull_frames(&self, take: impl Fn(StreamKey) -> Option<VideoFrame>) {
        self.inner.pull_frames(take);
    }

    /// Whether there is a tile for `key`.
    pub(crate) fn has_tile(&self, key: StreamKey) -> bool {
        self.inner.tiles.borrow().contains_key(&key)
    }

    /// The picture `key`'s tile shows, if any.
    pub(crate) fn paintable(&self, key: StreamKey) -> Option<gdk::Paintable> {
        self.inner.tiles.borrow().get(&key)?.picture.paintable()
    }
}

/// Open (or raise) the Video panel for `sess`.
///
/// # Safety
/// `sess` is a valid `session *` or NULL; GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn create_video_window(_parent: *mut c_void, sess: *mut c_void) {
    crate::ensure_gtk_init();
    let dock::Open::Build(page) = dock::open(dock::ID_VIDEO, sess) else {
        return;
    };
    let (root, _inner) = build_content(sess);
    // The dock sinks what it is given: hand it this reference, floating.
    let ptr: *mut gtk::ffi::GtkWidget = root.upcast::<gtk::Widget>().into_glib_ptr();
    glib::gobject_ffi::g_object_force_floating(ptr as *mut glib::gobject_ffi::GObject);
    dock::place(
        dock::ID_VIDEO,
        &page,
        dock::KIND_SIDEBAR,
        dock::AREA_END,
        "Video",
        "camera-video-symbolic",
        ptr,
    );
}

/// Re-read the room for every Video panel on `sess`'s connection: the
/// runtime was just built, the login reply changed what the server
/// offers, or the connection went away.
///
/// # Safety
/// `sess` is a valid `session *` or NULL.
#[no_mangle]
pub unsafe extern "C" fn video_panel_refresh_all(sess: *mut c_void) {
    if sess.is_null() {
        return;
    }
    let key = dock::key_for_session(sess);
    crate::screen_share::prune(key, runtime(sess));
    for_each_panel(Some(key), |p| p.refresh());
}

/// Rename `uid`'s tiles to `label`. uid 0 is this client's own preview,
/// which is labeled "You" whatever the nick.
fn relabel(tiles: &HashMap<StreamKey, Tile>, uid: u16, label: &str) {
    if uid == 0 {
        return;
    }
    for (key, tile) in tiles {
        if key.user_id == uid {
            tile.name.set_text(label);
        }
    }
}

/// `uid` changed nick or icon in room `cid` on `sess`'s connection:
/// rename their tiles.
///
/// # Safety
/// `sess` is a valid `session *` or NULL.
#[no_mangle]
pub unsafe extern "C" fn video_panel_user_changed(sess: *mut c_void, cid: u32, uid: u16) {
    if sess.is_null() {
        return;
    }
    let key = dock::key_for_session(sess);
    for_each_panel(Some(key), |p| p.user_changed(cid, uid));
}

/// Bring the Video panel for `sess` to the front, building it if needed.
/// Starting a camera does this, so the user sees what they're sending.
pub(crate) fn present(sess: *mut c_void) {
    // Through the toolbar, like the Panels menu: it reattaches a panel
    // the user closed, and a panel built afresh gets a page on every
    // connection, not only this one.
    let id = crate::cs(dock::ID_VIDEO);
    unsafe { toolbar_present_panel(id.as_ptr(), sess, glib::ffi::GFALSE) };
}

/// Say in the voice room's chat when someone starts sharing, wherever the
/// Video panel is or isn't. Hooked to the session's voice model once; the
/// model lives as long as the session does.
///
/// # Safety
/// `sess` is a valid `session *`; called on the GTK main thread.
pub(crate) unsafe fn announce_shares(sess: *mut c_void) {
    let model = hx_session_voice_model(sess);
    if model.is_null() {
        return;
    }
    let model: glib::Object =
        glib::translate::from_glib_none(model as *mut glib::gobject_ffi::GObject);
    if model.data::<bool>("gtkhx-announce-shares").is_some() {
        return;
    }
    model.set_data("gtkhx-announce-shares", true);
    let conn = dock::key_for_session(sess);
    model.connect_local("video-started", false, move |args| {
        let uid = args[1].get::<u32>().ok()? as u16;
        let kind = args[2].get::<u32>().ok()?;
        announce_share(conn, uid, kind);
        None
    });
}

/// The model has already applied the presence chime's gate — never our own
/// share, nor one running when we joined — so what is left is the chat's,
/// the preference join and leave lines answer to.
fn announce_share(conn: dock::ConnKey, uid: u16, kind: u32) {
    if !hxconfig::ffi::with_settings(|s| s.chat.show_joins).unwrap_or(false) {
        return;
    }
    let sess = unsafe { hx_session_with_serial(conn) };
    let Some(cid) = unsafe { runtime(sess) }.and_then(|rt| rt.active_cid()) else {
        return;
    };
    let name = unsafe { nick(sess, cid, uid) };
    let line = if kind == hxvoice_model::VIDEO_SCREEN {
        crate::tr::tr1("%s started sharing their screen", &name)
    } else {
        crate::tr::tr1("%s turned their camera on", &name)
    };
    let line = crate::cs(&(line + "\n"));
    unsafe {
        hx_printf_prefix(
            hx_session_htlc(sess),
            cid,
            INFOPREFIX,
            c"%s".as_ptr(),
            line.as_ptr(),
        )
    };
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// A tile builds and takes a frame. Driven by `crate::gtk_tests`.
    pub(crate) fn check_tile_shows_a_frame() {
        let tile = Tile::new(cam(1), Weak::new());
        let frame = VideoFrame {
            width: 4,
            height: 2,
            stride: 16,
            bytes: glib::Bytes::from_owned(vec![255u8; 32]),
        };
        assert_eq!(tile.show(&frame), Some(2.0), "the first frame's shape");
        assert_eq!(tile.show(&frame), None, "the same shape again");
        let p = tile
            .picture
            .paintable()
            .expect("the frame became a paintable");
        assert_eq!(p.intrinsic_width(), 4);
        assert_eq!(p.intrinsic_height(), 2);
    }

    /// A rename reaches both of a user's tiles and nobody else's, and
    /// never the local preview. Driven by `crate::gtk_tests`.
    pub(crate) fn check_relabel_follows_a_nick_change() {
        let mut tiles = HashMap::new();
        for (uid, kind, label) in [
            (5, VideoKind::Camera, "Old"),
            (5, VideoKind::Screen, "Old"),
            (6, VideoKind::Camera, "Other"),
            (0, VideoKind::Camera, "You"),
        ] {
            let t = Tile::new(StreamKey { user_id: uid, kind }, Weak::new());
            t.name.set_text(label);
            tiles.insert(StreamKey { user_id: uid, kind }, t);
        }
        let name = |uid, kind| {
            tiles[&StreamKey { user_id: uid, kind }]
                .name
                .text()
                .to_string()
        };

        relabel(&tiles, 5, "NewNick");
        assert_eq!(name(5, VideoKind::Camera), "NewNick");
        assert_eq!(name(5, VideoKind::Screen), "NewNick");
        assert_eq!(name(6, VideoKind::Camera), "Other");
        assert_eq!(name(0, VideoKind::Camera), "You");

        relabel(&tiles, 0, "Me");
        assert_eq!(name(0, VideoKind::Camera), "You");
    }

    /// A panel shows the tiles it is given, each takes the frame waiting
    /// for it and no other, and an empty set brings the status page back.
    /// Driven by `crate::gtk_tests`.
    pub(crate) fn check_panel_shows_tiles_and_frames() {
        let panel = Standalone::new();
        let key = |uid| StreamKey {
            user_id: uid,
            kind: VideoKind::Camera,
        };
        let screen = StreamKey {
            user_id: 2,
            kind: VideoKind::Screen,
        };
        panel.set_tiles(&[
            (key(1), "one".into()),
            (key(2), "two".into()),
            (screen, "two".into()),
        ]);
        assert_eq!(
            panel.inner.stack.visible_child_name().as_deref(),
            Some("tiles")
        );
        assert_eq!(
            panel.inner.grid.focused(),
            Some(screen),
            "a share takes the stage"
        );

        let frame = VideoFrame {
            width: 8,
            height: 6,
            stride: 32,
            bytes: glib::Bytes::from_owned(vec![255u8; 8 * 6 * 4]),
        };
        panel.pull_frames(|k| (k == key(2)).then(|| frame.clone()));
        let shown = panel.paintable(key(2)).expect("tile 2 took its frame");
        assert_eq!((shown.intrinsic_width(), shown.intrinsic_height()), (8, 6));
        assert!(panel.paintable(key(1)).is_none());
        assert!(panel.paintable(screen).is_none());

        panel.set_tiles(&[(key(1), "one".into())]);
        assert!(panel.paintable(key(2)).is_none(), "tile 2 went");
        assert_eq!(panel.inner.grid.focused(), None);
        panel.set_tiles(&[]);
        assert_eq!(
            panel.inner.stack.visible_child_name().as_deref(),
            Some("empty")
        );
    }

    fn cam(uid: u16) -> StreamKey {
        StreamKey {
            user_id: uid,
            kind: VideoKind::Camera,
        }
    }

    /// Tiles in view and just ahead of it are received, those well away
    /// are not, and one already received is kept a little further out.
    #[test]
    fn in_reach_takes_the_view_and_a_margin() {
        // A 100-high view at 1000; tiles 100 high.
        let view = Some((1000.0, 1100.0));
        let at = |uid, top: f64| (cam(uid), Some((top, top + 100.0)));
        let tiles = [
            at(1, 1000.0), // in view
            at(2, 1120.0), // below, inside the look-ahead
            at(3, 1200.0), // below, past it
            at(4, 770.0),  // above, past the look-ahead, inside the hold
            at(5, 500.0),  // above, past both
            (cam(6), None),
        ];
        let got = in_reach(&tiles, view, &[]);
        assert_eq!(got, [cam(1), cam(2), cam(6)]);
        let got = in_reach(&tiles, view, &[cam(3), cam(4), cam(5)]);
        assert_eq!(got, [cam(1), cam(2), cam(3), cam(4), cam(6)]);
        // No view yet: everything.
        assert_eq!(in_reach(&tiles, None, &[]).len(), tiles.len());
    }

    /// A laid-out panel reports its tiles where they are: with the view at
    /// the top, the bottom tiles are out of reach, and scrolled to the
    /// bottom, the top ones. Driven by `crate::gtk_tests`.
    pub(crate) fn check_reach_follows_the_scroll() {
        let panel = Standalone::new();
        let tiles: Vec<_> = (1..=24).map(|uid| (cam(uid), uid.to_string())).collect();
        panel.set_tiles(&tiles);
        let win = gtk::Window::new();
        // One tile a row, a little over one row in view.
        win.set_default_size(260, 220);
        win.set_child(Some(&panel.root));
        win.present();
        let adj = panel.inner.scroll.vadjustment();
        let ctx = glib::MainContext::default();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        while (adj.page_size() <= 0.0 || adj.upper() <= adj.page_size())
            && std::time::Instant::now() < deadline
        {
            ctx.iteration(false);
        }
        assert!(
            adj.upper() > 3.0 * adj.page_size(),
            "the tiles overflow the view"
        );

        let reach = |receiving: &[StreamKey]| {
            let (tiles, view) = panel.inner.layout();
            in_reach(&tiles, view, receiving)
        };
        let top = reach(&[]);
        assert!(top.contains(&cam(1)));
        assert!(!top.contains(&cam(24)));

        adj.set_value(adj.upper() - adj.page_size());
        let bottom = reach(&[]);
        assert!(bottom.contains(&cam(24)));
        assert!(!bottom.contains(&cam(1)));

        // A width change is a reason to look again even when the row
        // count, and so the vertical extent, stays put: in a wider grid
        // tiles can move between rows without adding one. Here the grid
        // stays one tile wide.
        let timer = || panel.inner.subscribe_timer.borrow().is_some();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        while timer() && std::time::Instant::now() < deadline {
            ctx.iteration(true);
        }
        assert!(!timer(), "the pending set went out");
        win.set_default_size(300, 220);
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        while !timer() && std::time::Instant::now() < deadline {
            ctx.iteration(false);
        }
        assert!(timer(), "a resize schedules the set");
        win.destroy();
    }

    /// Tiles sit in user order whatever order they arrive in, this
    /// client's own preview first and a user's camera before their screen.
    /// Driven by `crate::gtk_tests`.
    pub(crate) fn check_tiles_sort_by_user() {
        let grid = VideoGrid::new();
        let screen = StreamKey {
            user_id: 2,
            kind: VideoKind::Screen,
        };
        for key in [cam(5), screen, cam(2), cam(0), cam(9)] {
            grid.insert(key, &gtk::Label::new(None));
        }
        assert_eq!(grid.keys(), [cam(0), cam(2), screen, cam(5), cam(9)]);
        // The widgets are in the same order, for keyboard focus.
        let mut n = 0;
        let mut child = grid.first_child();
        while let Some(c) = child {
            n += 1;
            child = c.next_sibling();
        }
        assert_eq!(n, 5);
        grid.remove(screen);
        assert_eq!(grid.keys(), [cam(0), cam(2), cam(5), cam(9)]);
    }

    /// A tile goes on the stage and off it again; a share is there by
    /// default until the user asks for none; a focused tile that goes
    /// takes the user's pick with it. Driven by `crate::gtk_tests`.
    pub(crate) fn check_focus_follows_the_user() {
        let panel = Standalone::new();
        let p = &panel.inner;
        let screen = StreamKey {
            user_id: 3,
            kind: VideoKind::Screen,
        };
        let own_screen = StreamKey {
            user_id: 0,
            kind: VideoKind::Screen,
        };
        // A lone tile has no stage, and a click on it leaves nothing behind.
        panel.set_tiles(&[(cam(1), "one".into())]);
        p.toggle_focus(cam(1));
        assert_eq!(p.focus.get(), Focus::Auto);
        assert!(!p.tiles.borrow()[&cam(1)].focus.is_visible());

        panel.set_tiles(&[(cam(1), "one".into()), (cam(2), "two".into())]);
        assert_eq!(p.grid.focused(), None);
        assert!(p.tiles.borrow()[&cam(1)].focus.is_visible());
        panel.set_tiles(&[(cam(1), "one".into()), (own_screen, "mine".into())]);
        assert_eq!(
            p.grid.focused(),
            None,
            "one's own share isn't worth the stage"
        );

        let all = [
            (cam(1), "one".into()),
            (cam(2), "two".into()),
            (screen, "three".into()),
        ];
        panel.set_tiles(&all);
        assert_eq!(p.grid.focused(), Some(screen));
        p.toggle_focus(cam(1));
        assert_eq!(p.grid.focused(), Some(cam(1)));
        p.toggle_focus(cam(1));
        assert_eq!(p.grid.focused(), None, "and back to all alike");
        panel.set_tiles(&all);
        assert_eq!(p.grid.focused(), None, "a refresh keeps the choice");

        p.toggle_focus(cam(2));
        panel.set_tiles(&[(cam(1), "one".into()), (screen, "three".into())]);
        assert_eq!(
            p.grid.focused(),
            Some(screen),
            "cam 2 went; the default is back"
        );
        panel.set_tiles(&all);
        assert_eq!(
            p.grid.focused(),
            Some(cam(2)),
            "cam 2 back, the pick with it"
        );

        p.new_room();
        panel.set_tiles(&all);
        assert_eq!(
            p.grid.focused(),
            Some(screen),
            "a new room starts from the default"
        );
    }

    /// Stopping watching takes the tile away and leaves a button to watch
    /// again, across refreshes, until the publication itself goes. Driven
    /// by `crate::gtk_tests`.
    pub(crate) fn check_stop_watching_and_watch_again() {
        let panel = Standalone::new();
        let p = &panel.inner;
        let buttons = || {
            let mut n = 0;
            let mut child = p.unwatched_list.first_child();
            while let Some(c) = child {
                n += 1;
                child = c.next_sibling();
            }
            n
        };
        let both = [(cam(1), "one".into()), (cam(2), "two".into())];
        panel.set_tiles(&both);
        assert!(!p.unwatched_bar.is_visible());

        p.toggle_focus(cam(2));
        p.stop_watching(cam(2));
        assert!(!panel.has_tile(cam(2)));
        assert_eq!(p.grid.keys(), [cam(1)]);
        assert_eq!(p.grid.focused(), None, "focus went with the tile");
        assert!(p.unwatched_bar.is_visible());
        assert_eq!(buttons(), 1);

        panel.set_tiles(&both);
        assert!(!panel.has_tile(cam(2)), "a refresh doesn't bring it back");

        p.stop_watching(cam(1));
        assert_eq!(
            p.stack.visible_child_name().as_deref(),
            Some("empty"),
            "nothing left to show"
        );
        assert_eq!(
            p.status.description().as_deref(),
            Some(tr("You aren't watching anyone. Pick someone below to watch.").as_str())
        );
        assert_eq!(buttons(), 2);

        p.watch(cam(1));
        assert!(panel.has_tile(cam(1)));
        assert_eq!(buttons(), 1);

        // Its publication ends and comes back — or the room is rejoined,
        // which empties the list for a moment: still not watched.
        panel.set_tiles(&[(cam(1), "one".into())]);
        assert!(!p.unwatched_bar.is_visible(), "nothing to watch again");
        panel.set_tiles(&[]);
        panel.set_tiles(&both);
        assert!(!panel.has_tile(cam(2)));
        assert_eq!(buttons(), 1);

        // Another room: everything is watched.
        p.new_room();
        panel.set_tiles(&both);
        assert!(panel.has_tile(cam(2)));
        assert!(!p.unwatched_bar.is_visible());
    }
}
