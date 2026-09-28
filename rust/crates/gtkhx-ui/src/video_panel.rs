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
    /// The scrolled content, stage and grid, and its window.
    tiles_box: gtk::Box,
    scroll: gtk::ScrolledWindow,
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
            VideoNotice::Frames => self.pull_frames(|key| rt.take_video_frame(key)),
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

    /// Show the newest frame of every tile that has one waiting.
    fn pull_frames(&self, take: impl Fn(StreamKey) -> Option<VideoFrame>) {
        for (key, tile) in self.tiles.borrow().iter() {
            if let Some(frame) = take(*key) {
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

        let has_tiles = self.set_tiles(&want);
        if !has_tiles {
            self.status.set_description(Some(&if !video {
                tr("This server doesn't support video.")
            } else if !in_room {
                tr("Join voice to see who has a camera or screen on.")
            } else {
                tr("Nobody in this voice chat has a camera or screen on.")
            }));
        }
        self.schedule_subscribe();
    }

    /// Make the tiles exactly `want` — (stream, paused, label) — and show
    /// them, or the status page when there are none. Returns whether there
    /// are any.
    fn set_tiles(&self, want: &[(StreamKey, bool, String)]) -> bool {
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
            for (key, paused, label) in want {
                let tile = tiles.entry(*key).or_insert_with(|| {
                    let t = Tile::new(key.kind);
                    // The grid sorts on this; see tile_uid.
                    t.root.set_widget_name(&format!("hx-video-{}", key.user_id));
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
                    .compute_bounds(&self.tiles_box)
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
            rt.video_publications()
                .into_iter()
                .filter(|p| p.user_id != me)
                .filter(|p| {
                    let key = StreamKey {
                        user_id: p.user_id,
                        kind: p.kind,
                    };
                    // A publication with no tile yet is one the next
                    // refresh adds: receive it rather than wait.
                    !tiled(&key) || reach.contains(&key)
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

fn sort_tiles(a: &gtk::FlowBoxChild, b: &gtk::FlowBoxChild) -> gtk::Ordering {
    tile_uid(a).cmp(&tile_uid(b)).into()
}

/// The user a grid child's tile shows, from the name `set_tiles` gave it.
fn tile_uid(child: &gtk::FlowBoxChild) -> u16 {
    child
        .child()
        .and_then(|w| w.widget_name().strip_prefix("hx-video-")?.parse().ok())
        .unwrap_or(u16::MAX)
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
    // By user, not by arrival: the order publications first reach this
    // client varies, and tiles shouldn't swap places between sessions.
    // This client's own preview (uid 0) comes first.
    grid.set_sort_func(sort_tiles);

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
        conn,
        root: root.downgrade(),
        stack,
        status,
        stage,
        grid,
        tiles_box,
        scroll,
        tiles: RefCell::new(HashMap::new()),
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
            let t = Tile::new(kind);
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
        assert!(panel.inner.stage.is_visible());

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
        assert!(!panel.inner.stage.is_visible());
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

    /// Camera tiles sit in user order whatever order they arrive in, this
    /// client's own preview first. Driven by `crate::gtk_tests`.
    pub(crate) fn check_tiles_sort_by_user() {
        let grid = gtk::FlowBox::new();
        grid.set_sort_func(sort_tiles);
        for uid in [5u16, 2, 0, 9] {
            let t = Tile::new(VideoKind::Camera);
            t.root.set_widget_name(&format!("hx-video-{uid}"));
            grid.append(&t.root);
        }
        let order: Vec<u16> = (0..4)
            .map(|i| tile_uid(&grid.child_at_index(i).expect("a child")))
            .collect();
        assert_eq!(order, [0, 2, 5, 9]);
    }
}
