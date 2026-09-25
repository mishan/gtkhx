//! The Video panel: the room's cameras and screen shares, as tiles.
//!
//! One page per connection in a dockable panel, like Users. It shows the
//! voice room this connection is in: a screen share takes the stage at
//! the top, cameras sit in a grid below it, and this client's own
//! publications appear as "You" tiles from the capture's preview. A
//! paused publication keeps its tile, marked paused — the spec's
//! present-but-paused.
//!
//! **What this client receives is decided here.** The server delivers no
//! video until asked (Video Subscribe, 610), and asks are the complete
//! set, so the panel is the one place that computes it: while its page is
//! mapped, every publication in the room; while hidden — another tab,
//! a collapsed dock, the window withdrawn — nothing. A collapsed panel
//! costs no bandwidth, which is the spec's reason 610 takes a whole set.
//! Changes are debounced so a burst of 611s and layout churn costs one
//! request, and the state machine drops a set that hasn't changed.
//!
//! The runtime is reached through its Rust API directly: `gtkhx-ui` links
//! it, and the panel is main-thread code talking to a main-thread object.
//! It is looked up through the session every time rather than held,
//! because a disconnect frees it.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
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

/// How long the subscription set may settle before it goes out.
const SUBSCRIBE_DEBOUNCE_MS: u64 = 150;

extern "C" {
    fn toolbar_present_panel(
        id: *const std::ffi::c_char,
        sess: *mut c_void,
        respect_saved_state: glib::ffi::gboolean,
    );
    fn hx_session_voice_runtime(sess: *mut c_void) -> *mut c_void;
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
}

impl Tile {
    fn new(kind: VideoKind) -> Tile {
        let picture = gtk::Picture::new();
        picture.set_can_shrink(true);
        picture.set_content_fit(gtk::ContentFit::Contain);
        picture.set_hexpand(true);
        picture.set_vexpand(true);
        let (w, h) = match kind {
            VideoKind::Camera => (240, 180),
            VideoKind::Screen => (480, 270),
        };
        picture.set_size_request(w, h);
        picture.add_css_class("hx-video-picture");

        let root = gtk::Overlay::new();
        root.set_child(Some(&picture));
        root.add_css_class("card");
        root.set_overflow(gtk::Overflow::Hidden);

        let name = gtk::Label::new(None);
        name.set_halign(gtk::Align::Start);
        name.set_valign(gtk::Align::End);
        name.set_margin_start(6);
        name.set_margin_bottom(6);
        name.set_ellipsize(gtk::pango::EllipsizeMode::End);
        name.add_css_class("osd");
        name.add_css_class("caption");
        root.add_overlay(&name);

        let paused = gtk::Label::new(Some(&match kind {
            VideoKind::Camera => tr("Camera paused"),
            VideoKind::Screen => tr("Sharing paused"),
        }));
        paused.set_halign(gtk::Align::Center);
        paused.set_valign(gtk::Align::Center);
        paused.add_css_class("osd");
        paused.set_visible(false);
        root.add_overlay(&paused);

        Tile {
            root,
            picture,
            name,
            paused,
        }
    }

    fn show(&self, frame: &VideoFrame) {
        let texture = gdk::MemoryTexture::new(
            frame.width as i32,
            frame.height as i32,
            gdk::MemoryFormat::R8g8b8a8,
            &frame.bytes,
            frame.stride as usize,
        );
        self.picture.set_paintable(Some(&texture));
    }
}

// ---------------------------------------------------------------------
// Per-panel state.
// ---------------------------------------------------------------------

struct PanelInner {
    conn: dock::ConnKey,
    /// Weak: the root owns this state (as widget data), so a strong ref
    /// back would keep both alive forever.
    root: glib::WeakRef<gtk::Box>,
    stack: gtk::Stack,
    status: adw::StatusPage,
    stage: gtk::Box,
    grid: gtk::FlowBox,
    tiles: RefCell<HashMap<StreamKey, Tile>>,
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
            VideoNotice::Frames => self.pull_frames(rt),
            VideoNotice::StreamEnded(key) => {
                if let Some(t) = self.tiles.borrow().get(key) {
                    t.picture.set_paintable(None::<&gdk::Paintable>);
                }
            }
            VideoNotice::Publications | VideoNotice::Local(_) | VideoNotice::Session(_) => {
                self.refresh();
            }
        }
    }

    fn pull_frames(&self, rt: &VoiceRuntime) {
        for (key, tile) in self.tiles.borrow().iter() {
            if let Some(frame) = rt.take_video_frame(*key) {
                tile.show(&frame);
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

        {
            let mut tiles = self.tiles.borrow_mut();
            tiles.retain(|key, tile| {
                let keep = want.iter().any(|(k, _, _)| k == key);
                if !keep {
                    if let Some(parent) = tile.root.parent() {
                        if let Some(child) = parent.downcast_ref::<gtk::FlowBoxChild>() {
                            self.grid.remove(child);
                        } else {
                            self.stage.remove(&tile.root);
                        }
                    }
                }
                keep
            });
            for (key, paused, label) in &want {
                let tile = tiles.entry(*key).or_insert_with(|| {
                    let t = Tile::new(key.kind);
                    match key.kind {
                        VideoKind::Screen => self.stage.append(&t.root),
                        VideoKind::Camera => self.grid.append(&t.root),
                    }
                    t
                });
                tile.name.set_text(label);
                tile.paused.set_visible(*paused);
                if *paused {
                    tile.picture.set_paintable(None::<&gdk::Paintable>);
                }
            }
        }

        let has_tiles = !self.tiles.borrow().is_empty();
        self.stage.set_visible(
            self.tiles
                .borrow()
                .keys()
                .any(|k| k.kind == VideoKind::Screen),
        );
        if has_tiles {
            self.stack.set_visible_child_name("tiles");
        } else {
            self.status.set_description(Some(&if !video {
                tr("This server doesn't support video.")
            } else if !in_room {
                tr("Join voice to see who has a camera or screen on.")
            } else {
                tr("Nobody in this voice chat has a camera or screen on.")
            }));
            self.stack.set_visible_child_name("empty");
        }
        self.schedule_subscribe();
    }

    /// Queue the receive set to be recomputed and sent.
    fn schedule_subscribe(self: &Rc<Self>) {
        if self.subscribe_timer.borrow().is_some() {
            return;
        }
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

    /// Everything the room publishes while this page is on screen;
    /// nothing while it isn't. Paused publications are included: the
    /// subscription survives the pause, and resuming then costs no
    /// renegotiation.
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
            rt.video_publications()
                .into_iter()
                .filter(|p| p.user_id != me)
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

fn build_content(sess: *mut c_void) -> (gtk::Box, Rc<PanelInner>) {
    let root = gtk::Box::new(gtk::Orientation::Vertical, 0);
    root.set_hexpand(true);
    root.set_vexpand(true);

    let status = adw::StatusPage::new();
    status.set_icon_name(Some("camera-video-symbolic"));
    status.set_title(&tr("No Video"));
    status.add_css_class("compact");

    let stage = gtk::Box::new(gtk::Orientation::Vertical, 6);
    stage.set_margin_start(6);
    stage.set_margin_end(6);
    stage.set_margin_top(6);
    stage.set_visible(false);

    let grid = gtk::FlowBox::new();
    grid.set_selection_mode(gtk::SelectionMode::None);
    grid.set_homogeneous(true);
    grid.set_min_children_per_line(1);
    grid.set_max_children_per_line(4);
    grid.set_row_spacing(6);
    grid.set_column_spacing(6);
    grid.set_margin_start(6);
    grid.set_margin_end(6);
    grid.set_margin_top(6);
    grid.set_margin_bottom(6);
    grid.set_valign(gtk::Align::Start);

    let tiles_box = gtk::Box::new(gtk::Orientation::Vertical, 0);
    tiles_box.append(&stage);
    tiles_box.append(&grid);
    let scroll = gtk::ScrolledWindow::new();
    scroll.set_hscrollbar_policy(gtk::PolicyType::Never);
    scroll.set_vexpand(true);
    scroll.set_child(Some(&tiles_box));

    let stack = gtk::Stack::new();
    stack.add_named(&status, Some("empty"));
    stack.add_named(&scroll, Some("tiles"));
    stack.set_vexpand(true);
    root.append(&stack);

    let inner = Rc::new(PanelInner {
        conn: dock::key_for_session(sess),
        root: root.downgrade(),
        stack,
        status,
        stage,
        grid,
        tiles: RefCell::new(HashMap::new()),
        observing: Cell::new(0),
        subscribe_timer: RefCell::new(None),
    });

    // Visibility is the subscription policy: mapping and unmapping the
    // page (tab switches, dock collapse, a withdrawn window) recompute it.
    {
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
                p.observing.set(0);
            }
        });
    }

    PANELS.with(|p| p.borrow_mut().push(Rc::downgrade(&inner)));
    // The panel owns itself through the widget: dropping the last strong
    // ref with the widget is what unregisters its observer.
    unsafe { root.set_data("video-panel-state", inner.clone()) };
    inner.refresh();
    (root, inner)
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

/// Bring the Video panel for `sess` to the front, building it if needed.
/// Starting a camera does this, so the user sees what they're sending.
pub(crate) fn present(sess: *mut c_void) {
    // Through the toolbar, like the Panels menu: it reattaches a panel
    // the user closed, and a panel built afresh gets a page on every
    // connection, not only this one.
    let id = crate::cs(dock::ID_VIDEO);
    unsafe { toolbar_present_panel(id.as_ptr(), sess, glib::ffi::GFALSE) };
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// A tile builds and takes a frame. Driven by `crate::gtk_tests`.
    pub(crate) fn check_tile_shows_a_frame() {
        let tile = Tile::new(VideoKind::Camera);
        let frame = VideoFrame {
            width: 4,
            height: 2,
            stride: 16,
            bytes: glib::Bytes::from_owned(vec![255u8; 32]),
        };
        tile.show(&frame);
        let p = tile
            .picture
            .paintable()
            .expect("the frame became a paintable");
        assert_eq!(p.intrinsic_width(), 4);
        assert_eq!(p.intrinsic_height(), 2);
    }
}
