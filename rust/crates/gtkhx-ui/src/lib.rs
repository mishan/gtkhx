//! `gtkhx-ui` — window UIs in gtk4-rs.
//!
//! One module per ported window (`tracker` is the first). Each module
//! exports the C ABI entry points its deleted `src/<window>.c` used to
//! provide (via `#[no_mangle] pub extern "C"`), so the C callers
//! (toolbar.c, gtkhx.c signal adapters, options.c, network.c) link
//! against Rust with no source change. Shared infrastructure lives at
//! the crate root: [`ffi`] (extern decls for the C helpers windows
//! call), [`tr`] (gettext).
//!
//! Everything here runs on the GTK main thread; interior state is held in
//! `thread_local!`s. There is no `Send`/`Sync` surface — the windows
//! never touch worker threads directly (network / transfers already
//! marshal back to the main loop in `hxnet`/C before these entry points
//! fire).

mod announce;
mod ffi;
mod hl_date;
mod tr;
// Shared wrapper over the C dock-embed bridge, used by every docked-window
// shell (Users, Tasks, …). See dock_bridge.c.
mod dock;

pub mod about;
pub mod agreement;
pub mod bookmark_store;
pub mod chat_find;
pub mod cipher_vocab;
pub mod connect;
mod identity;
mod shortcuts;
// the Settings form: the window and its page table (options_window), the
// shared row builders and the pages themselves (options), and the two pages
// custom enough to want their own module.
/// The Hotline icon catalogue and its picker dialog, shared by the Identity
/// and Connections settings pages.
mod icon_picker;
pub mod name_shadow;
pub mod options;
mod options_connections;
mod options_identity;
/// Link-time stubs for the C symbols the Settings pages call, so the page
/// table can be exercised in a `cargo test` binary. Test builds only.
#[cfg(test)]
mod options_test_stubs;
/// The Voice settings page. Feature-gated to match the page itself — see the
/// module docs for why the preferences are not.
#[cfg(feature = "voice")]
mod options_voice;
mod options_window;
pub mod rc4_dialog;
pub mod tls_trust_dialog;
pub mod tracker;
pub mod useredit;
// HxUserRow — the users/chat list row model GObject (was
// users_row.c). Exports the hx_user_row_* C ABI users_view.c links against.
pub mod user_row;
// HxUserListView — the GtkColumnView-backed user list. Exports the
// hx_user_list_view_* C ABI users.c / chat.c link against; the custom Name
// cell stays C behind FFI (users_cell.c).
pub mod users_view;
// the user-list voice-indicator column (was users_voice_col.c).
// Behind the `voice` feature; a NULL stub otherwise. Exports
// gtkhx_users_voice_column_new that users_view.rs appends.
pub mod users_voice_col;
// xdg-desktop-portal requests, shared by screen sharing and the camera,
// and the camera's access flow for the Flatpak sandbox.
#[cfg(feature = "voice")]
mod camera_portal;
#[cfg(all(feature = "voice", target_os = "linux"))]
mod portal;
// the per-chat voice toolbar (was voice_panel.c). Wholly behind the
// `voice` feature — its C callers (chat.c / users_bridge.c / gtkutil.c) are
// #ifdef HAVE_VOICE, so no voice-off stub is needed. Exports voice_panel_*.
#[cfg(feature = "voice")]
pub mod screen_share;
#[cfg(feature = "voice")]
mod video_grid;
#[cfg(feature = "voice")]
pub mod video_panel;
#[cfg(feature = "voice")]
pub mod voice_panel;
// Who owns the microphone. One voice chat at a time anywhere, so the token is
// process-global even though the runtimes it arbitrates are per-connection.
#[cfg(feature = "voice")]
pub mod voice_arbiter;
// the push-to-talk key controller (was voice_ptt.c). Wholly behind the
// `voice` feature — its C caller (toolbar.c) is #ifdef HAVE_VOICE. The pure
// key-spec vocabulary stays C (voice_ptt_keyspec.c). Exports hx_voice_ptt_attach.
#[cfg(feature = "voice")]
pub mod voice_ptt;
// The voice-chat wire-out senders (was voice.c) are hxhandlers' `voice`
// module. The sibling voice modules reach hx_send_voice_* through their
// existing externs.
// the Users window shell (raise + dock registration + lifecycle).
// The custom view widget + action-button handlers stay C behind
// users_bridge.c / dock_bridge.c; create_users_window is now this module's
// #[no_mangle] export.
pub mod users;
// the Tasks window shell (raise + dock registration + lifecycle).
// The task list content + transfer-model coupling stay C in tasks.c;
// create_tasks_window is now this module's #[no_mangle] export.
pub mod tasks;
// the News window shell (1.0/1.2 flat news). Dock registration via
// dock_bridge; the news content (viewers, search, fetch/post RPC) stays C
// in news.c. create_news_window is now this module's #[no_mangle] export.
pub mod news;
// the News browser (1.5 threaded news): a window per connection, its content
// and its fetches. open_news_browser is this module's export.
pub mod news_browser;
// the Chat window shell (public chat, CENTER; hosts the pchat/PM tabs).
// Dock registration via dock_bridge; the chat's leaf widgets stay C in
// chat.c. create_chat_window is this module's #[no_mangle] export.
pub mod chat;
// GtkHx's configuration of the Rotulus chat view, and the link detector
// gtkurl.c's callers share with it.
pub mod chat_view;
// What the chat views draw: their lines, live and from history, and GtkHx's
// own; and each view's registry of the rows of the lines it drew.
pub mod chat_output;
// Reacting to a lobby line: its chips, its menu with the emoji chooser, and who reacted.
mod reactions;
// The "Replying to …" bar over the public chat's input, and the reply it holds.
mod reply_bar;
// The Chat panel's internal AdwTabView tab strip (was chat_tabs.c). Exports the
// gtkhx_chat_tabs_* C ABI chat.c / gtkutil.c + chat.rs use.
pub mod chat_tabs;
// The connection tab strip — one tab per open connection, switching the focus
// and with it every per-connection panel's content page. Not chat_tabs, which
// switches conversations *within* whichever connection is showing.
pub mod conn_tabs;
// The main window's stacked notice banners, and the update notice among them.
// Exports gtkhx_main_banners.
pub mod main_banners;
mod updates;
// Every display-backed test in the crate, in one #[test]. There can only be
// one; the module note says why.
#[cfg(test)]
mod gtk_tests;
// The chat / pchat input GtkTextView key handler (was chat_input_key_pressed).
// Exports gtkhx_chat_input_attach.
pub mod chat_input;
// The incoming chat-invitation dialog (was output_chat_invitation +
// chat_invite_response). Join/Decline via the hxhandlers::send::chat Rust senders.
pub mod chat_invite;
// The C ABI over the session-owned HxMemberModel +
// the M1 nick completion (hx_member_model_* / hx_nick_complete).
// HxConversation — the Rust per-chat model that replaces the C struct chat
// (cid + subject + owned HxMemberModel + opaque view pointer).
// Private messages: each conversation's tab, its input and the render of
// what it says, over hxmodel::pm. Hears the msg signal itself, and exports
// the hx_msg_* hooks C still calls in with.
pub mod msg;
// The messaging extension's friend family: the users panel's friends, the
// user menu's items, presence, and what a friend's conversation is handed.
pub mod friends;
// The message requests' window, one a connection, opened from the Friends
// section.
mod requests;
// the Files browser: its window, both panels and every operation between
// them. The providers underneath stay C (files_*_provider.c, files_ops.c).
// open_files_browser is this module's #[no_mangle] export.
pub mod files;
// In-app frame-clock benchmarks, armed by GTKHX_BENCH. See bench/mod.rs.
mod bench;
// Private Chat content — create_pchat_window builds the pchat tab's
// content tree (subject bar, xtext output, input, user sidebar) in gtk4-rs
// around the C gtkhx_pchat_new leaf widgets + gtkhx_pchat_user_sidebar.
pub mod pchat;
// the inline-media click-to-view dialog. Builds the
// AdwDialog (Loading → image → error stack) + Save-As / Open-Externally
// handlers in gtk4-rs; the download (hxhandlers::media) and glycin decoder
// (inline_media_decode.c / hx-image-decode) sit behind the FFI seam.
// inline_media_show_dialog is this module's #[no_mangle] export.
pub mod inline_media_dialog;
// The inline picture a chat line or private message carries, as a chat-view
// row fetched and decoded in place. hx_inline_media_row_append is its export.
pub mod inline_media_row;
// The paperclip that sends a picture on a chat line or a private message.
pub mod media_attach;
// the Get-User-Info result window (was gtkhx.c::output_user_info). A
// small read-only text window; fired from the user-info GtkhxSession signal.
// output_user_info is this module's #[no_mangle] export.
pub mod user_info;
// the File "Get Info" dialog (was files.c's output_file_info + Save/date
// helpers); fired from the file-info GtkhxSession signal. Dates format natively
// (hl_date), the Save button sends FILE_SETINFO natively (hxproto).
// output_file_info is this module's #[no_mangle] export.
pub mod file_info;
// the Create-Post composer (was news.c's post window). A modal
// GtkTextView + Cancel/Post header; the wire sender hx_post_news stays C.
// create_post_window is this module's #[no_mangle] export.
pub mod create_post;
// the 1.5 news-browser create (new folder / category) + delete
// confirmation dialogs (were news_browser.c's open_create_dialog +
// on_delete_clicked bodies). AdwAlertDialogs over the hxhandlers::send::news
// senders; news_browser keeps the selection logic and the refresh.
pub mod news_dialogs;
// the 1.5 news-browser tree view: the GtkTreeListModel child-model
// function + the GtkListView row factory (setup/bind/unbind + lazy
// fetch-on-expand), was news_browser.c's factory callbacks.
pub mod news_tree;
// the 1.5 news-browser compose window (New Post / Reply), was
// news_browser.c's open_compose_window + build_reply_context_panel. Modal
// GtkWindow over hxhandlers::send::news's post_thread sender.
pub mod news_compose;
// the 1.5 news-browser right-pane post rendering + selection
// breadcrumb (was news_browser.c's render_selected_post + update_breadcrumb).
// news_browser calls both.
pub mod news_render;
// The pictures a news article lists after its text.
pub mod news_pictures;
// the emoji picker button + inline `:shortcode:` typeahead (was
// emoji.c). GTK wiring only — the shortcode match list comes from
// hxproto. Exports hx_emoji_button_new / _typeahead_attach / _detach.
pub mod emoji;
// the Broadcast composer + wire sender (was toolbar.c's broadcast
// dialog + msg.c's hx_send_broadcast). AdwAlertDialog + AdwEntryRow; the wire
// build is native hxproto, the text encoder / task / write stay C.
// gtkhx_broadcast_dialog_open is this module's #[no_mangle] export, and
// broadcastmsg shows a broadcast received.
pub mod broadcast;
// the server banner surface + URL / HTXF fetch state machines (was banner.c +
// banner_dispatch.c). Decode / connection-state / TLS-verify are native Rust;
// the hxnet fetch + tokio worker spawn + send/task stay on the C ABI.
pub mod banner;
// GIF user avatars and their animation (was gif_avatar.c). Exports the
// gtkhx_avatar_* C ABI in gif_avatar.h.
pub mod avatar;
// the header bar's lock: whether, and how, the focused connection is encrypted.
pub mod encryption_lock;
// the preview window's notice for a file it shows only part of, or none of.
pub mod preview_notice;
// Plays a remote audio or video file as it downloads; needs GStreamer, which
// comes with voice.
#[cfg(feature = "voice")]
pub mod media_preview;

/// Tell gtk4-rs that GTK is already initialized.
///
/// The app initializes GTK from C (`gtk_init` / `adw_init`), so gtk4-rs's
/// own init flag was never set — and its widget/model constructors call
/// `assert_initialized_main_thread!()` on that flag, which panics (an
/// abort, since it can't unwind across the FFI) even though GTK is
/// running. Every gtk4-rs construction site reached from a C-ABI entry
/// point calls this first. Safe: `set_initialized()` verifies the real
/// `gtk_is_initialized()` and that we're on the main thread, and
/// early-returns once the binding is marked.
pub(crate) fn ensure_gtk_init() {
    unsafe { gtk4::set_initialized() };
}

/// Which way a wheel event moves the selection.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Wheel {
    Previous,
    Next,
}

/// Read a scroll delta as a tab step, or `None` for one that means neither.
///
/// Split out of the controller so it can be tested without one. Emitting
/// `scroll` on a live `GtkEventControllerScroll` turned out not to be
/// reliably reproducible — the handler runs on some invocations and not
/// others — and a flaky test is worse than none. This is the part with a
/// decision in it; that the controller is mounted, and *where*, is asserted
/// separately on the widget.
///
/// Both axes are read and the larger wins, so a diagonal touchpad flick moves
/// one tab rather than firing twice. Up is previous and down is next, which is
/// the direction every tabbed application agrees on.
pub(crate) fn wheel_direction(dx: f64, dy: f64) -> Option<Wheel> {
    let d = if dy.abs() >= dx.abs() { dy } else { dx };
    if d < 0.0 {
        Some(Wheel::Previous)
    } else if d > 0.0 {
        Some(Wheel::Next)
    } else {
        None
    }
}

/// Bold the title of any tab flagged as needing attention.
///
/// `AdwTabPage:title` is plain text — no markup — so the weight has to come
/// from CSS. libadwaita puts a `needs-attention` class on the `AdwTab` widget
/// and holds the label in a `.tab-title`; both were read off a live widget
/// tree rather than assumed, because a rule naming a class that doesn't exist
/// fails silently and looks exactly like a flag that was never set.
///
/// Deliberately not scoped to the connection strip. The chat tabs use the same
/// flag for the same reason — something happened over there — and an unread
/// private message should read the same way whichever strip is carrying it.
///
/// Attached to the display, once, at
/// `GTK_STYLE_PROVIDER_PRIORITY_APPLICATION`: above the theme's defaults,
/// below the user's own `gtk.css`, which is the same priority the rest of the
/// client's CSS uses.
pub(crate) fn ensure_tab_attention_css() {
    use std::sync::atomic::{AtomicBool, Ordering};

    static LOADED: AtomicBool = AtomicBool::new(false);
    if LOADED.swap(true, Ordering::Relaxed) {
        return;
    }
    let Some(display) = gtk4::gdk::Display::default() else {
        LOADED.store(false, Ordering::Relaxed);
        return;
    };
    let css = gtk4::CssProvider::new();
    css.load_from_string("tab.needs-attention .tab-title { font-weight: bold; }");
    gtk4::style_context_add_provider_for_display(
        &display,
        &css,
        gtk4::STYLE_PROVIDER_PRIORITY_APPLICATION,
    );
}

/// Make a tab bar switch tabs on a mouse wheel, the way a browser's does.
///
/// Attached to the *bar* rather than the view, because the view is what the
/// bar is a control for — and in the connection strip's case the view is never
/// drawn at all, so it would never see the event.
///
/// **In the capture phase**, which is what makes it work at all. `AdwTabBar`
/// wraps its tabs in a `GtkScrolledWindow` so a long strip can be scrolled
/// sideways, and that is a *descendant* of the bar — so in the ordinary bubble
/// phase it handles the wheel first and the event never reaches here. Capture
/// runs top-down, so the bar sees it before the scrolled window does. The cost
/// is deliberate: wheeling over the strip switches tabs instead of panning it,
/// which is the behaviour asked for and the one browsers have.
///
/// Wheel up selects the previous tab and down the next, which is the direction
/// every tabbed application agrees on. Horizontal wheels and touchpad kinetic
/// scrolling arrive through the same controller, so both axes are read and the
/// larger one wins; a diagonal flick otherwise fires twice.
///
/// The event is claimed only when a tab was actually switched. Refusing it at
/// the ends lets the scroll fall through to whatever is underneath instead of
/// swallowing it against a wall.
pub(crate) fn wheel_switches_tabs(
    bar: &impl gtk4::prelude::IsA<gtk4::Widget>,
    view: &libadwaita::TabView,
) {
    use gtk4::prelude::*;

    ensure_tab_attention_css();

    let scroll = gtk4::EventControllerScroll::new(
        gtk4::EventControllerScrollFlags::BOTH_AXES | gtk4::EventControllerScrollFlags::DISCRETE,
    );
    scroll.set_propagation_phase(gtk4::PropagationPhase::Capture);

    let view = view.clone();
    scroll.connect_scroll(move |_, dx, dy| {
        let moved = match wheel_direction(dx, dy) {
            Some(Wheel::Next) => view.select_next_page(),
            Some(Wheel::Previous) => view.select_previous_page(),
            None => false,
        };
        if moved {
            gtk4::glib::Propagation::Stop
        } else {
            gtk4::glib::Propagation::Proceed
        }
    });
    bar.as_ref().add_controller(scroll);
}

/// Keep a tab bar's tabs in it: a tab dragged off the bar stops there.
///
/// Past the bar's edge libadwaita turns a tab drag into a drag and drop, and
/// any tab bar or tab view in the process takes the tab — so a connection
/// could land among a Chat panel's conversations, and a conversation in the
/// connection strip or another connection's Chat. With animations off it also
/// crashed GTK: a tab dragged back over its bar gets a placeholder, which is
/// unparented the moment the pointer leaves again, while GTK is still walking
/// up from it to deliver that crossing. None of our bars can put a tab
/// anywhere else, so none of them needs the drop.
///
/// libadwaita starts the drop from the drag gesture on each of the bar's tab
/// boxes, once the pointer — on a motion or on the release — is more than
/// four drag thresholds outside that box. This controller sees each event
/// first, being in the capture phase, and just short of that distance cancels
/// the box's gesture, which ends the reorder with the tab where it is.
pub(crate) fn keep_tabs_in_bar(bar: &libadwaita::TabBar) {
    use gtk4::gdk;
    use gtk4::prelude::*;

    fn gestures(w: &gtk4::Widget, out: &mut Vec<(gtk4::Widget, gtk4::GestureDrag)>) {
        if w.type_().name() == "AdwTabBox" {
            let controllers = w.observe_controllers();
            for i in 0..controllers.n_items() {
                if let Some(g) = controllers.item(i).and_downcast::<gtk4::GestureDrag>() {
                    out.push((w.clone(), g));
                }
            }
        }
        let mut child = w.first_child();
        while let Some(c) = child {
            gestures(&c, out);
            child = c.next_sibling();
        }
    }
    let mut boxes = Vec::new();
    gestures(bar.upcast_ref(), &mut boxes);

    let legacy = gtk4::EventControllerLegacy::new();
    legacy.set_propagation_phase(gtk4::PropagationPhase::Capture);
    legacy.connect_event(move |_, event| {
        let kind = event.event_type();
        if kind != gdk::EventType::MotionNotify && kind != gdk::EventType::ButtonRelease {
            return gtk4::glib::Propagation::Proceed;
        }
        let Some((x, y)) = event.position() else {
            return gtk4::glib::Propagation::Proceed;
        };
        for (tab_box, gesture) in boxes.iter().filter(|(_, g)| g.is_active()) {
            let Some(native) = tab_box.native() else {
                continue;
            };
            let (sx, sy) = native.surface_transform();
            let Some(p) = native.compute_point(
                tab_box,
                &gtk4::graphene::Point::new((x - sx) as f32, (y - sy) as f32),
            ) else {
                continue;
            };
            let margin = (tab_box.settings().gtk_dnd_drag_threshold() * 4 - 1) as f32;
            if p.x() < -margin
                || p.y() < -margin
                || p.x() > tab_box.width() as f32 + margin
                || p.y() > tab_box.height() as f32 + margin
            {
                gesture.reset();
            }
        }
        gtk4::glib::Propagation::Proceed
    });
    bar.add_controller(legacy);
}

/// Name each of a tab bar's tabs for its page's title, and its lists of tabs
/// `list` and `pinned_list`, and keep them so.
///
/// libadwaita labels a tab by the title it shows, and a pinned tab hides its
/// title to show its icon alone, which leaves a screen reader nothing to say
/// for it. A page has no accessible name of its own to set, so the name goes
/// on the tab widget the bar made for it, found by its `page`, in place of
/// that label; the bar has made it by the time `page-attached` reaches a
/// handler connected after the bar's view.
///
/// libadwaita shows which tab is selected, and which needs attention, to the
/// eye alone, so each tab is also marked selected or not, and described as
/// unread while its page needs attention. The bar keeps its pinned tabs in a
/// list of their own, hidden from the tree while it is empty, so a strip
/// with none pinned does not read as two lists, one of them empty.
///
/// Each tab is read from its own page, whose notifications cover all of this;
/// a pin reaches here after the bar has moved the tab between its lists. The
/// last pinned page leaving notifies nothing of its own, so a page detached
/// is a change too.
pub(crate) fn name_tabs(bar: &libadwaita::TabBar, list: &str, pinned_list: &str) {
    use crate::tr::tr;
    use gtk4::accessible::{Property, State};
    use gtk4::prelude::*;

    /// The page's handler for its notifications, as `page-detached` takes it.
    const FOLLOWED: &str = "gtkhx-name-tabs-notify";

    let Some(view) = bar.view() else {
        return;
    };
    // TRANSLATORS: what a screen reader adds to a tab whose conversation or
    // connection has something not yet read.
    let unread = tr("Unread");
    let (list, pinned_list) = (list.to_owned(), pinned_list.to_owned());
    let (weak, weak_view) = (bar.downgrade(), view.downgrade());
    let sync = move || {
        let (Some(bar), Some(view)) = (weak.upgrade(), weak_view.upgrade()) else {
            return;
        };
        for tab in widgets_of_type(bar.upcast_ref(), "AdwTab") {
            let page = tab
                .find_property("page")
                .and_then(|_| tab.property::<Option<libadwaita::TabPage>>("page"));
            // A closed tab animates out with no page, under the name and
            // state it had, in a container of its own; hidden, they never
            // read as a second tab of that name, selected, or an empty item.
            // A container given a page again, a drop's placeholder, is shown.
            let shown = page.is_some();
            for w in std::iter::once(tab.clone()).chain(tab.parent()) {
                w.update_state(&[State::Hidden(!shown)]);
            }
            let Some(page) = page else {
                continue;
            };
            tab.reset_relation(gtk4::AccessibleRelation::LabelledBy);
            tab.update_property(&[Property::Label(&page.title())]);
            tab.update_state(&[State::Selected(Some(page.is_selected()))]);
            if page.needs_attention() {
                tab.update_property(&[Property::Description(&unread)]);
            } else {
                tab.reset_property(gtk4::AccessibleProperty::Description);
            }
        }
        for b in widgets_of_type(bar.upcast_ref(), "AdwTabBox") {
            let pinned = b.find_property("pinned").is_some() && b.property::<bool>("pinned");
            b.update_property(&[Property::Label(if pinned { &pinned_list } else { &list })]);
            b.update_state(&[State::Hidden(pinned && view.n_pinned_pages() == 0)]);
        }
    };
    fn unfollow(page: &libadwaita::TabPage) {
        // Safety: the key is this function's alone, and only `follow` sets
        // it, to a handler id.
        if let Some(id) = unsafe { page.steal_data::<gtk4::glib::SignalHandlerId>(FOLLOWED) } {
            page.disconnect(id);
        }
    }
    let follow = {
        let sync = sync.clone();
        move |page: &libadwaita::TabPage| {
            // A view named twice follows its pages once.
            unfollow(page);
            let changed = sync.clone();
            let id = page.connect_notify_local(None, move |_, _| changed());
            // Safety: see `unfollow`.
            unsafe { page.set_data(FOLLOWED, id) };
        }
    };
    for i in 0..view.n_pages() {
        follow(&view.nth_page(i));
    }
    sync();
    let synced = sync.clone();
    view.connect_page_attached(move |_, page, _| {
        follow(page);
        synced();
    });
    view.connect_page_detached(move |_, page, _| {
        unfollow(page);
        sync();
    });
}

/// The widgets under `w`, `w` too, whose type is called `type_name`, not
/// looking inside one.
fn widgets_of_type(w: &gtk4::Widget, type_name: &str) -> Vec<gtk4::Widget> {
    use gtk4::prelude::*;
    if w.type_().name() == type_name {
        return vec![w.clone()];
    }
    std::iter::successors(w.first_child(), |c| c.next_sibling())
        .flat_map(|c| widgets_of_type(&c, type_name))
        .collect()
}

/// C `char*` → owned `String` (empty on NULL). UTF-8 lossy.
///
/// # Safety
/// `p` is NULL or a valid NUL-terminated C string.
pub(crate) unsafe fn cstr(p: *const std::ffi::c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

/// An icon-only button, named for assistive technology. An icon names
/// nothing, so the name is an argument: no icon button goes without one, and
/// it is stated rather than left to GTK's fallback to the tooltip.
/// `tools/check-icon-buttons.sh` keeps `from_icon_name` buttons coming through
/// here.
pub(crate) fn icon_button(icon: &str, label: &str) -> gtk4::Button {
    let b = gtk4::Button::from_icon_name(icon);
    set_name_tip(&b, label);
    b
}

/// Set `text` as both the tooltip and the accessible name of a widget whose
/// only content is an icon (see [`icon_button`]).
pub(crate) fn set_name_tip(w: &impl gtk4::prelude::IsA<gtk4::Widget>, text: &str) {
    use gtk4::prelude::*;
    w.set_tooltip_text(Some(text));
    w.upcast_ref::<gtk4::Widget>()
        .update_property(&[gtk4::accessible::Property::Label(text)]);
}

/// Name a column view and the list inside it, which is what takes the focus:
/// empty, it has no rows to say.
pub(crate) fn name_column_view(cv: &gtk4::ColumnView, label: &str) {
    use gtk4::prelude::*;
    cv.update_property(&[gtk4::accessible::Property::Label(label)]);
    let mut child = cv.first_child();
    while let Some(w) = child {
        if w.is::<gtk4::ListView>() {
            w.update_property(&[gtk4::accessible::Property::Label(label)]);
        }
        child = w.next_sibling();
    }
}

/// `&str` → `CString`, dropping any interior NUL (never fails).
pub(crate) fn cs(s: &str) -> std::ffi::CString {
    std::ffi::CString::new(s).unwrap_or_else(|e| {
        let mut v = e.into_vec();
        v.retain(|&b| b != 0);
        std::ffi::CString::new(v).unwrap()
    })
}

#[cfg(test)]
mod wheel_tests {
    use super::{wheel_direction, Wheel};

    /// Up is previous, down is next, and a diagonal follows its larger axis
    /// rather than firing twice. Pure, so unlike the rest of this crate's
    /// checks it needs no display and no `gtk_tests` slot.
    #[test]
    fn wheel_direction_reads_the_larger_axis() {
        assert_eq!(wheel_direction(0.0, -1.0), Some(Wheel::Previous));
        assert_eq!(wheel_direction(0.0, 1.0), Some(Wheel::Next));

        // Horizontal alone still counts — a touchpad's sideways flick.
        assert_eq!(wheel_direction(-1.0, 0.0), Some(Wheel::Previous));
        assert_eq!(wheel_direction(1.0, 0.0), Some(Wheel::Next));

        // Diagonals resolve to one step, taking the axis the user leaned on.
        assert_eq!(wheel_direction(-0.2, 1.0), Some(Wheel::Next));
        assert_eq!(wheel_direction(1.0, -0.2), Some(Wheel::Next));

        // A tie goes to the vertical, which is the axis a mouse wheel has.
        assert_eq!(wheel_direction(1.0, -1.0), Some(Wheel::Previous));

        // Nothing at all is not a step, and must not claim the event.
        assert_eq!(wheel_direction(0.0, 0.0), None);
    }
}

#[cfg(test)]
pub(crate) mod tab_tests {
    use gtk4 as gtk;
    use gtk4::prelude::*;
    use libadwaita as adw;

    /// A pinned tab, which shows no title, is named for its page's title
    /// rather than labeled by the one it hides, and follows it when it
    /// changes.
    pub(crate) fn check_a_pinned_tab_is_named_for_its_title() {
        let view = adw::TabView::new();
        let bar = adw::TabBar::new();
        bar.set_view(Some(&view));
        super::name_tabs(&bar, "Conversations", "Pinned conversations");
        let page = view.append_pinned(&gtk::Box::new(gtk::Orientation::Vertical, 0));
        page.set_title("Chat");

        fn find_tab(w: &gtk::Widget) -> Option<gtk::Widget> {
            if w.type_().name() == "AdwTab" {
                return Some(w.clone());
            }
            std::iter::successors(w.first_child(), |c| c.next_sibling()).find_map(|c| find_tab(&c))
        }
        let tab = find_tab(bar.upcast_ref()).expect("the bar made a tab for the page");
        let named = |label: &str| {
            let label = crate::cs(label);
            // Safety: a live widget, and the label as the C string its
            // varargs take.
            let err = unsafe {
                gtk::ffi::gtk_test_accessible_check_property(
                    tab.as_ptr().cast(),
                    gtk::ffi::GTK_ACCESSIBLE_PROPERTY_LABEL,
                    label.as_ptr(),
                )
            };
            assert!(err.is_null(), "{}", unsafe { crate::cstr(err) });
            // Safety: a live widget.
            let labeled = unsafe {
                gtk::ffi::gtk_test_accessible_has_relation(
                    tab.as_ptr().cast(),
                    gtk::ffi::GTK_ACCESSIBLE_RELATION_LABELLED_BY,
                )
            };
            assert_eq!(labeled, 0, "labeled by a title it hides");
        };
        named("Chat");
        page.set_title("Lobby");
        named("Lobby");
    }

    /// `Ok` for a null answer from a `gtk_test_accessible_check_*`, else
    /// the mismatch it describes.
    fn checked(err: *mut std::ffi::c_char) -> Result<(), String> {
        if err.is_null() {
            return Ok(());
        }
        // Safety: a string the check allocated for its caller to free.
        let msg = unsafe { crate::cstr(err) };
        unsafe { gtk4::glib::ffi::g_free(err.cast()) };
        Err(msg)
    }

    /// Whether `w` holds `state` as `value`.
    fn state_is(w: &gtk::Widget, state: gtk::ffi::GtkAccessibleState, value: bool) -> bool {
        // Safety: a live widget, and a boolean state's value as its varargs
        // take it.
        checked(unsafe {
            gtk::ffi::gtk_test_accessible_check_state(
                w.as_ptr().cast(),
                state,
                std::ffi::c_int::from(value),
            )
        })
        .is_ok()
    }

    /// `w`'s `property`, as `want` or, for `None`, unset.
    pub(crate) fn assert_property(
        w: &gtk::Widget,
        property: gtk::ffi::GtkAccessibleProperty,
        want: Option<&str>,
    ) {
        // Safety: a live widget.
        let has =
            unsafe { gtk::ffi::gtk_test_accessible_has_property(w.as_ptr().cast(), property) };
        let Some(want) = want else {
            assert_eq!(has, 0, "{} has property {property}", w.type_().name());
            return;
        };
        let want = crate::cs(want);
        // Safety: a live widget, and the value as the C string its varargs
        // take.
        let err = unsafe {
            gtk::ffi::gtk_test_accessible_check_property(w.as_ptr().cast(), property, want.as_ptr())
        };
        if let Err(e) = checked(err) {
            panic!("{}: {e}", w.type_().name());
        }
    }

    /// The tab `bar` made for `page`.
    pub(crate) fn tab_of(bar: &adw::TabBar, page: &adw::TabPage) -> gtk::Widget {
        super::widgets_of_type(bar.upcast_ref(), "AdwTab")
            .into_iter()
            .find(|t| t.property::<Option<adw::TabPage>>("page").as_ref() == Some(page))
            .expect("the bar made a tab for the page")
    }

    /// Every tab of `view`'s pages is marked selected or not as its page is.
    pub(crate) fn assert_selection(bar: &adw::TabBar, view: &adw::TabView) {
        let selected = view.selected_page();
        for page in (0..view.n_pages()).map(|i| view.nth_page(i)) {
            let want = selected.as_ref() == Some(&page);
            assert!(
                state_is(
                    &tab_of(bar, &page),
                    gtk::ffi::GTK_ACCESSIBLE_STATE_SELECTED,
                    want
                ),
                "{:?}'s tab not marked selected {want}",
                page.title()
            );
        }
    }

    /// `bar`'s tab lists are named `list` and `pinned_list`, the pinned one
    /// hidden as `pinned_hidden` says and the other shown.
    pub(crate) fn assert_lists(
        bar: &adw::TabBar,
        list: &str,
        pinned_list: &str,
        pinned_hidden: bool,
    ) {
        let boxes = super::widgets_of_type(bar.upcast_ref(), "AdwTabBox");
        assert_eq!(boxes.len(), 2, "a bar holds a pinned list and the rest");
        for b in boxes {
            let pinned = b.property::<bool>("pinned");
            let name = if pinned { pinned_list } else { list };
            assert_property(&b, gtk::ffi::GTK_ACCESSIBLE_PROPERTY_LABEL, Some(name));
            let hidden = pinned_hidden && pinned;
            assert!(
                state_is(&b, gtk::ffi::GTK_ACCESSIBLE_STATE_HIDDEN, hidden),
                "{name} not hidden {hidden}"
            );
        }
    }

    /// How many handlers `page` has for `notify`, of any property.
    fn notify_handlers(page: &adw::TabPage) -> u32 {
        use gtk4::glib::{gobject_ffi, translate::ToGlibPtr};
        let obj: *mut gobject_ffi::GObject =
            page.upcast_ref::<gtk4::glib::Object>().to_glib_none().0;
        // Safety: a live object, and a signal every object has; each handler
        // blocked is unblocked at once.
        unsafe {
            let id = gobject_ffi::g_signal_lookup(c"notify".as_ptr(), gobject_ffi::G_TYPE_OBJECT);
            let matched = gobject_ffi::G_SIGNAL_MATCH_ID;
            let n = gobject_ffi::g_signal_handlers_block_matched(
                obj.cast(),
                matched,
                id,
                0,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            );
            gobject_ffi::g_signal_handlers_unblock_matched(
                obj.cast(),
                matched,
                id,
                0,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            );
            n
        }
    }

    /// Each strip says which tab is selected, through a tab added and the
    /// selected one closed, hides a closed tab as it animates out, names its
    /// lists of tabs, hiding the pinned list while it holds none, the last
    /// pinned page leaving too, and describes a tab as unread while its page
    /// needs attention. A page there before the strip was named is followed,
    /// and one moved within the view is followed once. The chat strip pins
    /// its public tab first and names its view; the connection strip pins
    /// none, and its view is never shown.
    pub(crate) fn check_a_strip_says_what_it_shows() {
        type Case = (&'static str, &'static str, bool, fn() -> adw::TabView);
        let cases: [Case; 2] = [
            (
                "Conversations",
                "Pinned conversations",
                true,
                crate::chat_tabs::new_view,
            ),
            ("Connections", "Connections", false, adw::TabView::new),
        ];
        for (list, pinned_list, pin, new_view) in cases {
            let view = new_view();
            let bar = adw::TabBar::new();
            bar.set_view(Some(&view));
            // On screen and never hidden, so a closed tab animates out
            // rather than going at once.
            let window = gtk::Window::new();
            window.set_child(Some(&bar));
            bar.set_autohide(false);
            window.present();
            let add = |title: &str| {
                let child = gtk::Box::new(gtk::Orientation::Vertical, 0);
                let page = if pin && view.n_pages() == 0 {
                    view.append_pinned(&child)
                } else {
                    view.append(&child)
                };
                page.set_title(title);
                page
            };
            let first = add("first");
            // Twice, as a rebuilt panel might name its strip again.
            super::name_tabs(&bar, list, pinned_list);
            super::name_tabs(&bar, list, pinned_list);
            let second = add("second");
            assert_eq!(view.selected_page().as_ref(), Some(&first), "{list}");
            assert_selection(&bar, &view);
            view.set_selected_page(&second);
            assert_selection(&bar, &view);
            let third = add("third");
            assert_selection(&bar, &view);
            view.set_selected_page(&third);
            view.close_page(&third);
            assert_eq!(view.selected_page().as_ref(), Some(&second), "{list}");
            assert_selection(&bar, &view);
            let closing: Vec<gtk::Widget> = super::widgets_of_type(bar.upcast_ref(), "AdwTab")
                .into_iter()
                .filter(|t| t.property::<Option<adw::TabPage>>("page").is_none())
                .collect();
            assert!(!closing.is_empty(), "{list}: no closed tab animating out");
            for w in closing.iter().flat_map(|t| [Some(t.clone()), t.parent()]) {
                let w = w.expect("a tab in its container");
                assert!(
                    state_is(&w, gtk::ffi::GTK_ACCESSIBLE_STATE_HIDDEN, true),
                    "{list}: a closed tab's {} still in the tree",
                    w.type_().name()
                );
            }

            if pin {
                assert_property(
                    view.upcast_ref(),
                    gtk::ffi::GTK_ACCESSIBLE_PROPERTY_LABEL,
                    Some(list),
                );
            }
            assert_lists(&bar, list, pinned_list, !pin);
            view.set_page_pinned(&first, !pin);
            assert_lists(&bar, list, pinned_list, pin);
            assert_selection(&bar, &view);
            view.set_page_pinned(&first, pin);
            assert_lists(&bar, list, pinned_list, !pin);

            let unread = |page: &adw::TabPage, want| {
                assert_property(
                    &tab_of(&bar, page),
                    gtk::ffi::GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                    want,
                )
            };
            let handlers = notify_handlers(&second);
            view.transfer_page(&second, &view, view.page_position(&second));
            assert_eq!(notify_handlers(&second), handlers, "{list}: followed twice");
            for page in [&first, &second] {
                unread(page, None);
                page.set_needs_attention(true);
                unread(page, Some("Unread"));
                page.set_needs_attention(false);
                unread(page, None);
            }

            view.set_page_pinned(&first, true);
            assert_lists(&bar, list, pinned_list, false);
            view.transfer_page(&first, &adw::TabView::new(), 0);
            assert_lists(&bar, list, pinned_list, true);
            window.destroy();
        }
    }
}
