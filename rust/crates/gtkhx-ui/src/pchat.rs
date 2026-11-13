//! Private Chat window — Phase R5.15 gtk4-rs content port.
//!
//! A private chat is a tab inside the Chat panel's `AdwTabView`.
//! `create_pchat_window` builds the tab's content tree here in gtk4-rs — a
//! subject bar over a horizontal pane whose left side is the chat column
//! (xtext output + auto-grow input with emoji/inline-media buttons) and whose
//! right side is the user sidebar — then adds the tab via `chat_tabs.c`.
//!
//! The C-coupled leaves stay C behind a small seam (mirrors `msg.rs`):
//! `gtkhx_pchat_new` makes the gchat model + the leaf widgets (xtext output,
//! input with its key controller, subject entry, inline-media button), and
//! `gtkhx_pchat_user_sidebar` builds the whole user sidebar (the HxUserListView
//! plus the six action buttons wired to the C `view_*_btn` handlers plus the
//! voice panel). Rust reads the leaf widgets via `hx_gchat_*` accessors,
//! assembles the frame/box/pane skeleton, and writes back the window via a
//! setter.

use std::collections::{HashMap, HashSet};
use std::ffi::{c_char, c_void};

use glib::translate::from_glib_none;
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;

use crate::tr::tr;

/// Opaque C `struct gtkhx_chat *` / `struct chat *` / `struct htlc_conn *`.
type Gchat = c_void;

use gtkhx_core::conn::{hx_conn_sess, hx_conn_uid};
use gtkhx_core::session::gtkhx_session_get_default;
use hxmodel::conversation::{hx_chat_cid, hx_chat_member_model, hx_chat_subject};
use hxmodel::member::HxMember;

extern "C" {
    fn gtkhx_pchat_new(htlc: *mut c_void, chat: *mut c_void) -> *mut Gchat;
    fn gtkhx_pchat_user_sidebar(htlc: *mut c_void, chat: *mut c_void) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_output(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_vscroll(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_input(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_subject(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_media_btn(g: *mut Gchat) -> *mut gtk::ffi::GtkWidget;
    fn hx_gchat_set_window(g: *mut Gchat, w: *mut gtk::ffi::GtkWidget);
    fn hx_gchat_userlist(g: *mut Gchat) -> *mut c_void;

    fn hx_emoji_button_new(target: *mut gtk::ffi::GtkWidget) -> *mut gtk::ffi::GtkWidget;
    fn hx_emoji_typeahead_attach(target: *mut gtk::ffi::GtkWidget);
    fn gtkhx_chat_tabs_add_pchat(
        htlc: *mut c_void,
        content: *mut gtk::ffi::GtkWidget,
        cid: u32,
        title: *const std::ffi::c_char,
    ) -> *mut c_void;
    fn gtkhx_chat_tabs_raise_pchat(htlc: *mut c_void, cid: u32);
    fn init_keyaccel(widget: *mut gtk::ffi::GtkWidget);
    fn chat_with_cid(sess: *mut c_void, cid: u32) -> *mut c_void;
}

/// `*mut GtkWidget` for a gtk-rs widget.
fn wptr<W: IsA<gtk::Widget>>(w: &W) -> *mut gtk::ffi::GtkWidget {
    w.as_ref().as_ptr()
}

/// The most nicks a name lists before it counts the rest.
const NAMED: usize = 3;

/// What a private chat is called: the wire gives it only a number, so its
/// subject, or failing that who else is in it.
fn label<S: AsRef<str>>(subject: &str, members: &[(u16, S)], me: u16) -> String {
    if !subject.is_empty() {
        return subject.to_owned();
    }
    let mut nicks: Vec<&str> = members
        .iter()
        .filter(|(uid, _)| *uid != me)
        .map(|(_, nick)| nick.as_ref())
        .filter(|nick| !nick.is_empty())
        .collect();
    if nicks.is_empty() {
        return tr("Private Chat");
    }
    nicks.sort_by_key(|n| n.to_lowercase());
    let named = nicks[..nicks.len().min(NAMED)].join(", ");
    match nicks.len().saturating_sub(NAMED) {
        0 => named,
        more => format!("{named} +{more}"),
    }
}

/// [`label`] for `chat` of `htlc`, with `subject` or else the one stored.
///
/// # Safety
/// `htlc` and `chat` are live, `chat` one of `htlc`'s.
unsafe fn chat_label(htlc: *mut c_void, chat: *mut c_void, subject: Option<String>) -> String {
    let subject = subject.unwrap_or_else(|| crate::cstr(hx_chat_subject(chat.cast())));
    let model: gio::ListModel =
        from_glib_none(hx_chat_member_model(chat.cast()) as *mut gio::ffi::GListModel);
    let members: Vec<(u16, String)> = model
        .iter::<HxMember>()
        .filter_map(Result::ok)
        .map(|m| (m.uid(), m.name()))
        .collect();
    label(&subject, &members, hx_conn_uid(htlc.cast()))
}

/// `labels`, numbered from the second of any that are alike, so no two
/// chats look the same: a number that would make another's name, or one
/// already given, is passed over.
fn numbered(labels: Vec<String>) -> Vec<String> {
    let names: HashSet<String> = labels.iter().cloned().collect();
    let mut given = HashSet::new();
    let mut last = HashMap::<String, usize>::new();
    labels
        .into_iter()
        .map(|l| {
            if given.insert(l.clone()) {
                return l;
            }
            let n = last.entry(l.clone()).or_insert(1);
            loop {
                *n += 1;
                let name = format!("{l} ({n})");
                if !names.contains(&name) && given.insert(name.clone()) {
                    return name;
                }
            }
        })
        .collect()
}

/// What each of `htlc`'s private chat tabs is called, in tab order, with
/// `subject` standing in for one chat's stored subject.
///
/// # Safety
/// `htlc` is live.
unsafe fn tab_labels(htlc: *mut c_void, subject: Option<(u32, String)>) -> Vec<(u32, String)> {
    let cids = crate::chat_tabs::pchat_cids(htlc);
    let labels = cids
        .iter()
        .map(|&cid| {
            let chat = chat_of(htlc, cid);
            if chat.is_null() {
                return tr("Private Chat");
            }
            let s = subject
                .as_ref()
                .filter(|(c, _)| *c == cid)
                .map(|(_, s)| s.clone());
            chat_label(htlc, chat, s)
        })
        .collect();
    cids.into_iter().zip(numbered(labels)).collect()
}

/// `char *gtkhx_pchat_label(struct htlc_conn *, struct chat *)` — the name the
/// chat's tab shows, for C to list it by; the caller `g_free`s it.
///
/// # Safety
/// `htlc` and `chat` are live, `chat` one of `htlc`'s.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_pchat_label(htlc: *mut c_void, chat: *mut c_void) -> *mut c_char {
    let cid = hx_chat_cid(chat.cast());
    let label = tab_labels(htlc, None)
        .into_iter()
        .find(|(c, _)| *c == cid)
        .map_or_else(|| chat_label(htlc, chat, None), |(_, l)| l);
    let s = crate::cs(&label);
    glib::ffi::g_strdup(s.as_ptr())
}

/// Rename every private chat tab of `htlc`: a chat's name can change the
/// numbering of another's.
///
/// # Safety
/// `htlc` is NULL or live.
pub(crate) unsafe fn retitle(htlc: *mut c_void, subject: Option<(u32, String)>) {
    if htlc.is_null() {
        return;
    }
    for (cid, title) in tab_labels(htlc, subject) {
        crate::chat_tabs::set_title_pchat(htlc, cid, &title);
    }
}

/// `void gtkhx_pchat_retitle(struct htlc_conn *)` — [`retitle`], for a
/// change to the members no signal tells of.
///
/// # Safety
/// `htlc` is NULL or live.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_pchat_retitle(htlc: *mut c_void) {
    retitle(htlc, None);
}

/// Add `chat`'s tab with `content`, and name it among the others.
///
/// # Safety
/// `htlc` and `chat` are live, `chat` one of `htlc`'s.
unsafe fn add_tab(htlc: *mut c_void, chat: *mut c_void, content: &impl IsA<gtk::Widget>) {
    let title = crate::cs(&chat_label(htlc, chat, None));
    gtkhx_chat_tabs_add_pchat(
        htlc,
        wptr(content),
        hx_chat_cid(chat.cast()),
        title.as_ptr(),
    );
    retitle(htlc, None);
}

/// `htlc`'s chat `cid`, or NULL.
///
/// # Safety
/// `htlc` is live.
unsafe fn chat_of(htlc: *mut c_void, cid: u32) -> *mut c_void {
    let sess = hx_conn_sess(htlc.cast());
    if sess.is_null() {
        std::ptr::null_mut()
    } else {
        chat_with_cid(sess.cast(), cid)
    }
}

/// Retitle a private chat's tab whenever what [`label`] reads changes.
pub(crate) fn install() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(install_once);
}

fn install_once() {
    let session: glib::Object =
        unsafe { from_glib_none(gtkhx_session_get_default() as *mut glib::gobject_ffi::GObject) };
    let ptr = |args: &[glib::Value], i: usize| {
        args[i].get::<*mut c_void>().unwrap_or(std::ptr::null_mut())
    };
    // After the view's own handlers, which are what update the members. A
    // rename comes with the public chat alone, the private chats' members
    // renamed without a signal of their own, so every tab is retitled.
    for signal in ["user-create", "user-delete", "user-change"] {
        session.connect_local(signal, true, move |args| {
            unsafe { retitle(ptr(args, 1), None) };
            None
        });
    }
    // A changed subject is announced before it is stored, so it is read
    // from the signal.
    session.connect_local("chat-subject", true, move |args| {
        let (htlc, subject) = (ptr(args, 1), ptr(args, 3));
        if !htlc.is_null() && !subject.is_null() {
            let cid = args[2].get::<u32>().unwrap_or(0);
            unsafe { retitle(htlc, Some((cid, crate::cstr(subject.cast())))) };
        }
        None
    });
}

/// Build a Private Chat tab. C ABI replacement for the old
/// `chat.c::create_pchat_window`; returns the `struct gtkhx_chat *`.
///
/// # Safety
/// `htlc` / `chat` are valid C pointers; called on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn create_pchat_window(htlc: *mut c_void, chat: *mut c_void) -> *mut c_void {
    crate::ensure_gtk_init();

    let gchat = gtkhx_pchat_new(htlc, chat);
    if gchat.is_null() {
        return std::ptr::null_mut();
    }
    let cid = hx_chat_cid(chat.cast());

    // Leaf widgets from the C helper (still floating; append/set_child sink).
    let output: gtk::Widget = from_glib_none(hx_gchat_output(gchat));
    let vscroll: gtk::Widget = from_glib_none(hx_gchat_vscroll(gchat));
    let input: gtk::Widget = from_glib_none(hx_gchat_input(gchat));
    let subject: gtk::Widget = from_glib_none(hx_gchat_subject(gchat));
    let media: gtk::Widget = from_glib_none(hx_gchat_media_btn(gchat));

    // Subject bar.
    let subj_hbox = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    subject.set_hexpand(true);
    subject.update_property(&[gtk::accessible::Property::Label(&tr("Chat subject"))]);
    subj_hbox.append(&subject);
    let subj_frame = gtk::Frame::new(None);
    subj_frame.set_child(Some(&subj_hbox));

    // Output frame: xtext + its scrollbar.
    let pchat_hbox = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    output.set_hexpand(true);
    output.set_vexpand(true);
    pchat_hbox.append(&output);
    pchat_hbox.append(&vscroll);
    let outputframe = gtk::Frame::new(None);
    outputframe.set_child(Some(&pchat_hbox));
    outputframe.set_vexpand(true);

    // Input frame: auto-grow input + emoji button + inline-media button.
    let input_scroll = gtk::ScrolledWindow::new();
    input_scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    input_scroll.set_propagate_natural_height(true);
    input_scroll.set_min_content_height(28);
    input_scroll.set_max_content_height(120);
    input_scroll.set_child(Some(&input));

    let input_hbox = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    input_scroll.set_hexpand(true);
    input_hbox.append(&input_scroll);
    let emoji: gtk::Widget = from_glib_none(hx_emoji_button_new(wptr(&input)));
    emoji.set_valign(gtk::Align::End);
    input_hbox.append(&emoji);
    hx_emoji_typeahead_attach(wptr(&input));
    media.set_valign(gtk::Align::End);
    input_hbox.append(&media);

    let inputframe = gtk::Frame::new(None);
    inputframe.set_child(Some(&input_hbox));
    inputframe.set_vexpand(false);

    // Chat column: output over input, under the subject bar.
    let vstack = gtk::Box::new(gtk::Orientation::Vertical, 4);
    vstack.append(&outputframe);
    if let Some(view) = output.downcast_ref::<rotulus::RotulusView>() {
        vstack.append(&crate::reply_bar::build(view, &input));
    }
    vstack.append(&inputframe);

    let vbox = gtk::Box::new(gtk::Orientation::Vertical, 4);
    vbox.set_margin_start(5);
    vbox.set_margin_end(5);
    vbox.set_margin_top(5);
    vbox.set_margin_bottom(5);
    vbox.append(&subj_frame);
    vstack.set_vexpand(true);
    vbox.append(&vstack);

    // User sidebar (C helper) on the right of the pane. It returns NULL if the
    // gchat lookup fails; from_glib_none on NULL is UB, so fall back to an
    // empty box (the pane still needs an end child) rather than wrap NULL.
    let sidebar_ptr = gtkhx_pchat_user_sidebar(htlc, chat);
    let user_sidebar: gtk::Widget = if sidebar_ptr.is_null() {
        gtk::Box::new(gtk::Orientation::Vertical, 0).upcast()
    } else {
        from_glib_none(sidebar_ptr)
    };
    crate::users_view::name_pchat(hx_gchat_userlist(gchat), &output);

    let hpane = gtk::Paned::new(gtk::Orientation::Horizontal);
    hpane.set_start_child(Some(&vbox));
    hpane.set_end_child(Some(&user_sidebar));
    hpane.set_position(435);

    // gchat->window = the tab content (the pane). notify.c only needs a widget
    // in the panel's tree; the input carries the "sess"/"gchat" qdata the key
    // handler reads (set in gtkhx_pchat_new), so the pane needs no qdata.
    hx_gchat_set_window(gchat, wptr(&hpane));

    add_tab(htlc, chat, &hpane);
    gtkhx_chat_tabs_raise_pchat(htlc, cid);
    init_keyaccel(wptr(&hpane));
    input.grab_focus();

    gchat
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use gtkhx_core::conn::{hx_conn_new, hx_conn_set_sess, hx_conn_set_uid};
    use hxmodel::chat_members::hx_member_model_upsert;
    use hxmodel::conversation::{hx_conversation_new, HxConversation};

    /// A rename is heard with the public chat alone, and still reaches the
    /// tabs of private chats named for the renamed member, those alike told
    /// apart by number there and in the invite picker, in the order they
    /// were opened whatever the tabs' order, and renumbered once one closes.
    ///
    /// Driven by `crate::gtk_tests` rather than being a `#[test]` — see there.
    pub(crate) fn check_a_rename_retitles_the_tabs() {
        unsafe {
            // A connection stands in for its own session, as in conn_tabs'.
            let htlc = hx_conn_new();
            let sess: *mut c_void = htlc.cast();
            hx_conn_set_sess(htlc, sess);
            hx_conn_set_uid(htlc, 7);
            crate::options_test_stubs::register_session(sess);
            let htlc: *mut c_void = htlc.cast();
            let chats = [0, 5, 6, 7].map(|cid| {
                let c = hx_conversation_new(cid);
                crate::options_test_stubs::chats().lock().unwrap().push((
                    sess as usize,
                    cid,
                    c as usize,
                ));
                c
            });
            let rename = |nick: &std::ffi::CStr| {
                for c in chats {
                    hx_member_model_upsert(hx_chat_member_model(c), 2, nick.as_ptr(), 0, 0, 0);
                }
                gtkhx_core::session::gtkhx_session_emit_user_change(
                    gtkhx_session_get_default(),
                    htlc,
                    chats[0].cast(),
                    2,
                    u32::MAX,
                    nick.as_ptr(),
                    0,
                    0,
                );
            };
            let titles = |cids: &[u32]| -> Vec<String> {
                cids.iter()
                    .map(|&cid| {
                        let p: libadwaita::TabPage =
                            from_glib_none(crate::chat_tabs::gtkhx_chat_tabs_find_pchat(htlc, cid));
                        p.title().to_string()
                    })
                    .collect()
            };
            let picked = |chat: *mut HxConversation| {
                let p = gtkhx_pchat_label(htlc, chat.cast());
                let s = crate::cstr(p);
                glib::ffi::g_free(p.cast());
                s
            };

            crate::chat_tabs::tests::bare_view(htlc);
            // Before install: the members change without a signal heard.
            rename(c"alice");
            for c in &chats[1..] {
                add_tab(
                    htlc,
                    c.cast(),
                    &gtk::Box::new(gtk::Orientation::Vertical, 0),
                );
            }
            assert_eq!(titles(&[5, 6, 7]), ["alice", "alice (2)", "alice (3)"]);

            install();
            // What users.c's user_change does to each private chat the
            // member is in, before the signal for the public chat.
            rename(c"carol");
            assert_eq!(titles(&[5, 6, 7]), ["carol", "carol (2)", "carol (3)"]);
            assert_eq!(picked(chats[2]), "carol (2)");

            crate::chat_tabs::tests::move_first_to_end(htlc);
            rename(c"carol");
            assert_eq!(titles(&[5, 6, 7]), ["carol", "carol (2)", "carol (3)"]);
            assert_eq!(picked(chats[1]), "carol");

            crate::chat_tabs::tests::close_pchat(htlc, 5);
            while glib::MainContext::default().iteration(false) {}
            assert_eq!(titles(&[6, 7]), ["carol", "carol (2)"]);
        }
    }

    #[test]
    fn alike_names_are_numbered_from_the_second() {
        let cases: [(&[&str], &[&str]); 5] = [
            (&["a", "b"], &["a", "b"]),
            (&["a", "b", "a", "a"], &["a", "b", "a (2)", "a (3)"]),
            (&["a (2)", "a", "a"], &["a (2)", "a", "a (3)"]),
            (&["a", "a", "a (2)"], &["a", "a (3)", "a (2)"]),
            (&[], &[]),
        ];
        for (labels, want) in cases {
            let got = numbered(labels.iter().map(|l| l.to_string()).collect());
            assert_eq!(got, want, "{labels:?}");
        }
    }

    #[test]
    fn a_private_chat_is_named_by_its_subject_or_who_else_is_in_it() {
        let me = 7;
        type Members<'a> = &'a [(u16, &'a str)];
        let cases: [(&str, Members, &str); 7] = [
            ("Plans", &[(7, "me"), (2, "bob")], "Plans"),
            ("", &[(7, "me"), (2, "bob"), (3, "Alice")], "Alice, bob"),
            ("", &[(7, "me")], "Private Chat"),
            ("", &[(7, "me"), (2, "")], "Private Chat"),
            ("", &[], "Private Chat"),
            (
                "",
                &[(2, "dan"), (3, "carol"), (4, "bob")],
                "bob, carol, dan",
            ),
            (
                "",
                &[
                    (7, "me"),
                    (2, "eve"),
                    (3, "dan"),
                    (4, "carol"),
                    (5, "bob"),
                    (6, "alice"),
                ],
                "alice, bob, carol +2",
            ),
        ];
        for (subject, members, want) in cases {
            assert_eq!(label(subject, members, me), want, "{subject:?} {members:?}");
        }
    }
}
