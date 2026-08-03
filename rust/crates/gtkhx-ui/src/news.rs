//! News window — flat 1.0/1.2 content (ported from `news.c`) plus the R5.11
//! gtk4-rs shell.
//!
//! The 1.0/1.2 flat-news panel docks into the toolbar window's start (sidebar)
//! area. This module owns both the *window shell* (raise-if-open, dock
//! registration via `dock_bridge.c`, post-embed lifecycle) and the *content*:
//! the Post/Reload/Find button bar, the read-only `GtkTextView`, and the
//! in-buffer Find search bar (`GtkSearchBar` + match highlighting/navigation,
//! Ctrl+F).
//!
//! Widget ownership seam: the three widget handles (`news_text`, `postButton`,
//! `reloadButton`) still live on the C `session` so the two remaining C
//! consumers — `gtkutil.c`'s setbtns (connect-state sensitivity) and
//! `options.c`'s theme re-apply — reach them unchanged; the Rust build
//! populates them via `gtkhx_news_set_widgets`. The rest of the flat-news view
//! state (the search context) is held Rust-side in a thread-local (single
//! connection). The wire senders `hx_get_news` / `hx_post_news` stay C.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::{c_char, c_void};
use std::ptr;
use std::rc::Rc;

use glib::translate::{from_glib_none, IntoGlibPtr};
use gtk::gdk;
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;

use crate::dock;
use crate::dock::ConnKey;
use crate::ffi as cffi;
use crate::tr::tr;

/// Opaque C `session *`.
type Session = c_void;

use hxhandlers::send::news::hx_get_news;
use hxtext::gtkhx_text_to_utf8;

extern "C" {
    // Flat-News session seam (`gtkhx_ui_bridge.c`).
    fn gtkhx_news_set_widgets(
        sess: *mut Session,
        text: *mut gtk::ffi::GtkWidget,
        post: *mut gtk::ffi::GtkWidget,
        reload: *mut gtk::ffi::GtkWidget,
    );
    fn gtkhx_session_htlc(sess: *mut Session) -> *mut c_void;
    fn gtkhx_news_can_read(htlc: *mut c_void) -> glib::ffi::gboolean;
    /// `gtkhx-core` — has this connection passed the post-login boundary?
    /// The gate for anything that puts a post-login RPC on the wire; see
    /// hxconn.h for why the socket being up is not the same question.
    fn hx_conn_post_login_fetched(h: *const c_void) -> glib::ffi::gboolean;

    // URL tagging (`gtkurl.c`) + Mac-Roman→UTF-8 (`text_util.c`).
    fn gtkurl_textview_install(tv: *mut gtk::ffi::GtkTextView);
    fn gtkurl_textview_apply_tags(tv: *mut gtk::ffi::GtkTextView);
}

// Flat-news view state, one entry per connection. Main thread only.
//
// Was a single `Option<NewsView>`, which is the same thing while there is one
// connection and silently the wrong thing once there are two: a second build
// overwrote the first connection's entry, leaving its Find context holding
// widget references into a buffer nothing pointed at any more, and sending
// every subsequent news line to the wrong panel.
//
// Keyed on the connection rather than the session, so the entry points that
// arrive with an `htlc` and no session (the two `output_news_*` exports, which
// are called from the receive path) can find their own view.
thread_local! {
    static NEWS: RefCell<HashMap<ConnKey, NewsView>> = RefCell::new(HashMap::new());
}

/// Run `f` against `conn`'s news view, if it has one.
///
/// The borrow is held for the call. Everything reached through it is GTK on
/// the same thread and none of it calls back into this module, so there is no
/// re-entrancy to avoid — unlike the tab strips, whose teardown handlers do.
fn with_news<R>(conn: ConnKey, f: impl FnOnce(&NewsView) -> R) -> Option<R> {
    NEWS.with(|n| n.borrow().get(&conn).map(f))
}

struct NewsView {
    post_btn: gtk::Widget,
    reload_btn: gtk::Widget,
    search: Rc<SearchCtx>,
}

/// In-buffer Find state (ported from `news.c`'s `news_search_ctx` +
/// `news_search_*`). Matches are `(start_offset, end_offset)` char pairs so
/// Next/Prev are O(1); `current` is -1 when there's no active match.
struct SearchCtx {
    text_view: gtk::TextView,
    search_bar: gtk::SearchBar,
    entry: gtk::SearchEntry,
    count: gtk::Label,
    prev_btn: gtk::Button,
    next_btn: gtk::Button,
    matches: RefCell<Vec<(i32, i32)>>,
    current: Cell<i32>,
}

impl SearchCtx {
    /// Buffer-scoped highlight tags — Adwaita-friendly (warning-yellow for all
    /// hits, accent-orange for the active one), created on first use.
    fn ensure_tags(&self) {
        let buf = self.text_view.buffer();
        let tt = buf.tag_table();
        if tt.lookup("search-match").is_none() {
            buf.create_tag(
                Some("search-match"),
                &[("background", &"#f6d32d"), ("foreground", &"#000000")],
            );
        }
        if tt.lookup("search-current").is_none() {
            buf.create_tag(
                Some("search-current"),
                &[("background", &"#ff7800"), ("foreground", &"#ffffff")],
            );
        }
    }

    fn clear_highlights(&self) {
        let buf = self.text_view.buffer();
        let (start, end) = buf.bounds();
        buf.remove_tag_by_name("search-match", &start, &end);
        buf.remove_tag_by_name("search-current", &start, &end);
    }

    fn apply_current_tag(&self) {
        let buf = self.text_view.buffer();
        let (start, end) = buf.bounds();
        buf.remove_tag_by_name("search-current", &start, &end);

        let cur = self.current.get();
        let matches = self.matches.borrow();
        if cur < 0 || cur as usize >= matches.len() {
            return;
        }
        let (so, eo) = matches[cur as usize];
        let mut mstart = buf.iter_at_offset(so);
        let mend = buf.iter_at_offset(eo);
        buf.apply_tag_by_name("search-current", &mstart, &mend);
        // within_margin 0.1 keeps a small breathing area at top/bottom.
        self.text_view
            .scroll_to_iter(&mut mstart, 0.1, false, 0.0, 0.0);
    }

    fn update_count(&self) {
        if self.entry.text().is_empty() {
            self.count.set_text("");
            self.prev_btn.set_sensitive(false);
            self.next_btn.set_sensitive(false);
            return;
        }
        let n = self.matches.borrow().len();
        if n == 0 {
            self.count.set_text(&tr("No results"));
            self.prev_btn.set_sensitive(false);
            self.next_btn.set_sensitive(false);
        } else {
            self.count
                .set_text(&format!("{} / {}", self.current.get() + 1, n));
            self.prev_btn.set_sensitive(n > 1);
            self.next_btn.set_sensitive(n > 1);
        }
    }

    /// Walk the whole buffer collecting every case-insensitive match, tag them
    /// all, and select the first. `GTK_TEXT_SEARCH_CASE_INSENSITIVE` folds via
    /// Pango's normalize+casefold, so "café" matches "CAFÉ".
    fn run(&self) {
        let buf = self.text_view.buffer();
        let needle = self.entry.text();

        self.clear_highlights();
        self.matches.borrow_mut().clear();
        self.current.set(-1);

        if needle.is_empty() {
            self.update_count();
            return;
        }
        self.ensure_tags();

        let flags = gtk::TextSearchFlags::CASE_INSENSITIVE | gtk::TextSearchFlags::VISIBLE_ONLY;
        let end_buf = buf.end_iter();
        let mut cursor = buf.start_iter();
        while let Some((ms, me)) = cursor.forward_search(needle.as_str(), flags, Some(&end_buf)) {
            self.matches.borrow_mut().push((ms.offset(), me.offset()));
            buf.apply_tag_by_name("search-match", &ms, &me);
            cursor = me;
        }

        if !self.matches.borrow().is_empty() {
            self.current.set(0);
            self.apply_current_tag();
        }
        self.update_count();
    }

    fn navigate(&self, delta: i32) {
        let n = self.matches.borrow().len() as i32;
        if n == 0 {
            return;
        }
        // Euclidean wrap so Prev from the first hit lands on the last.
        self.current.set(((self.current.get() + delta) % n + n) % n);
        self.apply_current_tag();
        self.update_count();
    }
}

/// Build the search bar + its child layout, wire its signals, and return the
/// bar (the caller packs it above the news scroller).
fn build_search_bar(ctx: &Rc<SearchCtx>) -> gtk::SearchBar {
    let hbox = gtk::Box::new(gtk::Orientation::Horizontal, 6);
    hbox.append(&ctx.entry);
    hbox.append(&ctx.count);
    hbox.append(&ctx.prev_btn);
    hbox.append(&ctx.next_btn);

    ctx.entry.set_hexpand(true);
    ctx.count.add_css_class("dim-label");
    ctx.count.set_margin_start(6);
    ctx.count.set_margin_end(6);
    ctx.prev_btn.set_tooltip_text(Some(&tr("Previous match")));
    ctx.prev_btn.add_css_class("flat");
    ctx.prev_btn.set_sensitive(false);
    ctx.next_btn.set_tooltip_text(Some(&tr("Next match")));
    ctx.next_btn.add_css_class("flat");
    ctx.next_btn.set_sensitive(false);

    ctx.search_bar.set_child(Some(&hbox));
    ctx.search_bar.connect_entry(&ctx.entry);

    ctx.entry.connect_search_changed({
        let c = ctx.clone();
        move |_| c.run()
    });
    ctx.entry.connect_next_match({
        let c = ctx.clone();
        move |_| c.navigate(1)
    });
    ctx.entry.connect_previous_match({
        let c = ctx.clone();
        move |_| c.navigate(-1)
    });
    ctx.entry.connect_activate({
        let c = ctx.clone();
        move |_| c.navigate(1)
    });
    ctx.prev_btn.connect_clicked({
        let c = ctx.clone();
        move |_| c.navigate(-1)
    });
    ctx.next_btn.connect_clicked({
        let c = ctx.clone();
        move |_| c.navigate(1)
    });
    // Esc closes the bar → clear highlights + reset the query.
    ctx.search_bar.connect_search_mode_enabled_notify({
        let c = ctx.clone();
        move |bar| {
            if !bar.is_search_mode() {
                c.clear_highlights();
                c.matches.borrow_mut().clear();
                c.current.set(-1);
                c.entry.set_text("");
            }
        }
    });

    ctx.search_bar.clone()
}

/// Build the flat-news panel content (button bar + search bar + read-only text
/// view), stash the widgets on the session + the thread-local view, and return
/// a still-floating container (the dock bridge sinks it). NULL if `sess` is
/// NULL.
///
/// # Safety
/// `sess` is NULL or a valid `session *`; GTK main thread only.
unsafe fn build_content(sess: *mut Session) -> *mut gtk::ffi::GtkWidget {
    if sess.is_null() {
        return ptr::null_mut();
    }

    // 2x-scaled themed Post / Reload buttons via the shared C helper (cb=NULL;
    // we wire "clicked" from Rust). Find is a stock symbolic GtkButton — the
    // pixmap-upscale path would render a symbolic icon blurry.
    let post_res = crate::cs("/com/nasledov/gtkhx/pixmaps/post_news.png");
    let post_tip = crate::cs(&tr("Post News"));
    let post_btn: gtk::Widget = from_glib_none(cffi::gtkhx_pixmap_button(
        post_res.as_ptr(),
        post_tip.as_ptr(),
        cffi::GTKHX_SCALE_WINDOW_BUTTONS,
        ptr::null(),
        ptr::null_mut(),
    ));
    let reload_res = crate::cs("/com/nasledov/gtkhx/pixmaps/refresh.png");
    let reload_tip = crate::cs(&tr("Reload News"));
    let reload_btn: gtk::Widget = from_glib_none(cffi::gtkhx_pixmap_button(
        reload_res.as_ptr(),
        reload_tip.as_ptr(),
        cffi::GTKHX_SCALE_WINDOW_BUTTONS,
        ptr::null(),
        ptr::null_mut(),
    ));
    let find_btn = gtk::Button::from_icon_name("system-search-symbolic");
    find_btn.set_tooltip_text(Some(&tr("Find in News (Ctrl+F)")));

    // Post → the Rust composer; Reload → reload_news (this module).
    // gtkhx_pixmap_button always returns a GtkButton, so downcast for a typed
    // connect_clicked rather than a stringly-typed connect_local("clicked").
    if let Some(btn) = post_btn.downcast_ref::<gtk::Button>() {
        btn.connect_clicked(move |_| {
            crate::create_post::create_post_window(ptr::null_mut(), sess);
        });
    }
    if let Some(btn) = reload_btn.downcast_ref::<gtk::Button>() {
        btn.connect_clicked(move |_| {
            news_reload(sess);
        });
    }

    let news_text = gtk::TextView::new();
    news_text.set_editable(false);
    news_text.set_cursor_visible(false);
    news_text.set_wrap_mode(gtk::WrapMode::Word);
    cffi::gtkhx_apply_text_style(news_text.as_ptr() as *mut gtk::ffi::GtkWidget);
    gtkurl_textview_install(news_text.as_ptr());

    let news_scroll = gtk::ScrolledWindow::new();
    news_scroll.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Automatic);
    news_scroll.set_vexpand(true);
    news_scroll.set_child(Some(&news_text));

    let ctx = Rc::new(SearchCtx {
        text_view: news_text.clone(),
        search_bar: gtk::SearchBar::new(),
        entry: gtk::SearchEntry::new(),
        count: gtk::Label::new(Some("")),
        prev_btn: gtk::Button::from_icon_name("go-up-symbolic"),
        next_btn: gtk::Button::from_icon_name("go-down-symbolic"),
        matches: RefCell::new(Vec::new()),
        current: Cell::new(-1),
    });
    // Create the tags up front so the first clear pass has them.
    ctx.ensure_tags();
    let search_bar = build_search_bar(&ctx);

    find_btn.connect_clicked({
        let bar = search_bar.clone();
        let entry = ctx.entry.clone();
        move |_| {
            let was_open = bar.is_search_mode();
            bar.set_search_mode(!was_open);
            if !was_open {
                entry.grab_focus();
            }
        }
    });

    // Post on start, Find + Reload on end, hexpand spacer between.
    let button_bar = gtk::Box::new(gtk::Orientation::Horizontal, 4);
    button_bar.set_margin_start(6);
    button_bar.set_margin_end(6);
    button_bar.set_margin_top(6);
    button_bar.set_margin_bottom(4);
    button_bar.append(&post_btn);
    let spacer = gtk::Label::new(None);
    spacer.set_hexpand(true);
    button_bar.append(&spacer);
    button_bar.append(&find_btn);
    button_bar.append(&reload_btn);

    let content_vbox = gtk::Box::new(gtk::Orientation::Vertical, 0);
    content_vbox.append(&button_bar);
    content_vbox.append(&search_bar);
    content_vbox.append(&news_scroll);

    post_btn.set_sensitive(false);
    reload_btn.set_sensitive(false);

    // Ctrl+F reveals the bar + focuses the entry; the bar also captures
    // printable keys typed on the content subtree.
    search_bar.set_key_capture_widget(Some(&content_vbox));
    {
        let sc = gtk::ShortcutController::new();
        sc.set_propagation_phase(gtk::PropagationPhase::Capture);
        let bar = search_bar.clone();
        let entry = ctx.entry.clone();
        let action = gtk::CallbackAction::new(move |_widget, _args| {
            bar.set_search_mode(true);
            entry.grab_focus();
            glib::Propagation::Stop
        });
        let trigger = gtk::KeyvalTrigger::new(gdk::Key::f, gdk::ModifierType::CONTROL_MASK);
        sc.add_shortcut(gtk::Shortcut::new(Some(trigger), Some(action)));
        content_vbox.add_controller(sc);
    }

    // Store widget handles: on the C session for the remaining C consumers
    // (gtkutil setbtns, options theme apply); in the thread-local for the Rust
    // output/reload paths.
    gtkhx_news_set_widgets(
        sess,
        news_text.as_ptr() as *mut gtk::ffi::GtkWidget,
        post_btn.as_ptr(),
        reload_btn.as_ptr(),
    );
    // Forget this connection's view when its page goes. Without it the
    // NewsView keeps its buttons and its Find context alive against a
    // destroyed buffer, and news_output happily keeps appending into it.
    {
        let conn = dock::key_for_session(sess);
        content_vbox.connect_destroy(move |_| {
            NEWS.with(|n| {
                n.borrow_mut().remove(&conn);
            });
        });
    }

    NEWS.with(|n| {
        n.borrow_mut().insert(
            dock::key_for_session(sess),
            NewsView {
                post_btn: post_btn.clone(),
                reload_btn: reload_btn.clone(),
                search: ctx.clone(),
            },
        );
    });

    into_floating_ptr(content_vbox)
}

/// Post-embed lifecycle: mark the panel open + sensitize the action buttons if
/// we're already connected.
///
/// # Safety
/// `sess` is NULL or a valid `session *`; GTK main thread only.
unsafe fn news_after_embed(sess: *mut Session) {
    if sess.is_null() {
        return;
    }
    // This session's connection, not the focused one: building connection
    // two's News page while connection one happens to be up should not come
    // up with live Post and Reload buttons.
    //
    // And post-login, not socket-up. `fd` is set as soon as the TCP connect
    // lands — well before the "fully joined" boundary the server gates
    // post-login RPCs on — so a live Reload button between those two points
    // would put a NEWS_GETFILE on the wire mid-handshake, which stricter 1.5+
    // servers answer with a disconnect. It is also -1 during teardown, which
    // is non-zero. See hxconn.h; the files browser's remote provider gates the
    // same way for the same reason.
    if hx_conn_post_login_fetched(gtkhx_session_htlc(sess)) != 0 {
        with_news(dock::key_for_session(sess), |v| {
            v.post_btn.set_sensitive(true);
            v.reload_btn.set_sensitive(true);
        });
    }
}

/// Reload the flat news file: gate on the open pref + the READ_NEWS access
/// bit, clear the buffer, and re-fetch. Shared by the Reload button and the
/// C `reload_news` export (called from `rcv.c` after a fresh login).
///
/// # Safety
/// `sess` is NULL or a valid `session *`; GTK main thread only.
unsafe fn news_reload(sess: *mut Session) {
    if sess.is_null() {
        return;
    }
    let htlc = gtkhx_session_htlc(sess);
    if gtkhx_news_can_read(htlc) == glib::ffi::GFALSE {
        return;
    }
    // Clearing the buffer *is* the has-a-view test: no view for this
    // connection means there is nothing to reload into, and no reason to put a
    // NEWS_GETFILE on its wire. Replaces a process-wide "has a News panel ever
    // been built?" latch, which answered yes on behalf of every connection as
    // soon as one of them had one.
    if with_news(dock::conn_key(htlc), |v| {
        v.search.text_view.buffer().set_text("");
    })
    .is_none()
    {
        return;
    }
    hx_get_news(htlc);
}

/// Return the news bytes as a UTF-8 `String` via the C Mac-Roman→UTF-8
/// converter (the same sanitiser the C output path used). Empty on NULL.
unsafe fn news_bytes_to_utf8(news: *const c_char, len: u16) -> String {
    if news.is_null() || len == 0 {
        return String::new();
    }
    let mut out_len: usize = 0;
    let utf8 = gtkhx_text_to_utf8(news, len as usize, &mut out_len);
    if utf8.is_null() {
        return String::new();
    }
    let bytes = std::slice::from_raw_parts(utf8 as *const u8, out_len);
    let s = String::from_utf8_lossy(bytes).into_owned();
    glib::ffi::g_free(utf8 as *mut c_void);
    s
}

/// Append `text` to the news buffer (`at_start` = prepend, else append), re-tag
/// URLs, and re-run any active Find query. Shared body of `output_news_post`
/// (prepend) and `output_news_file` (append).
unsafe fn news_output(htlc: *mut c_void, news: *const c_char, len: u16, at_start: bool) {
    let text = news_bytes_to_utf8(news, len);
    // No open-panel gate any more: having a view for this connection *is* the
    // gate, and it is the per-connection one. The old `gtkhx_news_is_open`
    // asked whether a News panel had ever been built by anyone, which would
    // have let one connection's news arrive while another's panel was the only
    // one that existed.
    with_news(dock::conn_key(htlc), |v| {
        let tv = &v.search.text_view;
        let buf = tv.buffer();
        let mut iter = if at_start {
            buf.start_iter()
        } else {
            buf.end_iter()
        };
        buf.insert(&mut iter, &text);
        // Re-tag URLs across the buffer so the new chunk's links pick up the
        // "url" GtkTextTag (single regex pass — cheap).
        gtkurl_textview_apply_tags(tv.as_ptr());
        // If Find is open, refresh so the new text picks up highlights.
        v.search.run();
    });
}

/// Return a freshly-built widget with a floating reference (matching a GTK C
/// constructor; the dock bridge's embed sinks it).
unsafe fn into_floating_ptr<W: IsA<gtk::Widget>>(w: W) -> *mut gtk::ffi::GtkWidget {
    let ptr = w.upcast::<gtk::Widget>().into_glib_ptr();
    glib::gobject_ffi::g_object_force_floating(ptr as *mut glib::gobject_ffi::GObject);
    ptr
}

// ---------------------------------------------------------------------
// C-ABI exports.
// ---------------------------------------------------------------------

/// Open (or raise) the News panel. C ABI replacement for the old
/// `news.c::create_news_window`. `toolbar_window` is vestigial (the panel is a
/// resident of the toolbar dock, not reparented); `sess` is the `session *`.
///
/// # Safety
/// `sess` is a valid `session *` (or NULL); called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn create_news_window(_toolbar_window: *mut c_void, sess: *mut c_void) {
    crate::ensure_gtk_init();

    let dock::Open::Build(page) = dock::open(dock::ID_NEWS, sess) else {
        return;
    };

    let sess = sess as *mut Session;
    let content = build_content(sess);
    if content.is_null() {
        return;
    }

    // On failure the bridge has already destroyed `content`; skip the
    // post-embed lifecycle so we don't mark a non-existent panel open.
    if dock::place(
        dock::ID_NEWS,
        &page,
        dock::KIND_SIDEBAR,
        dock::AREA_START,
        "News",
        "text-x-generic-symbolic",
        content,
    ) {
        news_after_embed(sess);
    }
}

/// `void open_news(GtkWidget *widget, gpointer data)` — the toolbar News
/// button. Open/raise the panel, then fetch if connected (the first connect
/// leaves the panel up but the buffer empty).
///
/// # Safety
/// `data` is a valid `session *`; called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn open_news(widget: *mut gtk::ffi::GtkWidget, data: *mut c_void) {
    create_news_window(widget as *mut c_void, data);
    // `data`'s connection, not the focused one — the fetch goes down this
    // session's wire, so this session's state is what decides whether to send
    // it. Post-login rather than socket-up: see news_after_embed.
    let htlc = gtkhx_session_htlc(data as *mut Session);
    if hx_conn_post_login_fetched(htlc) != 0 {
        hx_get_news(htlc);
    }
}

/// `void reload_news(GtkWidget *widget, gpointer data)` — Reload button + the
/// post-login refresh from `rcv.c`.
///
/// # Safety
/// `data` is NULL or a valid `session *`; called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn reload_news(_widget: *mut gtk::ffi::GtkWidget, data: *mut c_void) {
    news_reload(data as *mut Session);
}

/// `void output_news_post(struct htlc_conn *htlc, char *news, guint16 len)` —
/// a freshly posted article. Prepends to the buffer (newest on top).
///
/// # Safety
/// `news` is NULL or valid for `len` bytes; GTK main thread only.
#[no_mangle]
pub unsafe extern "C" fn output_news_post(htlc: *mut c_void, news: *mut c_char, len: u16) {
    news_output(htlc, news, len, /*at_start=*/ true);
}

/// `void output_news_file(struct htlc_conn *htlc, char *news, guint16 len)` —
/// the bulk news-file load. Appends to the buffer.
///
/// # Safety
/// `news` is NULL or valid for `len` bytes; GTK main thread only.
#[no_mangle]
pub unsafe extern "C" fn output_news_file(htlc: *mut c_void, news: *mut c_char, len: u16) {
    news_output(htlc, news, len, /*at_start=*/ false);
}
