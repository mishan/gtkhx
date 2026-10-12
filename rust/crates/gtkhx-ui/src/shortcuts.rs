//! The keyboard shortcuts window (`app.shortcuts`, Ctrl+?), and the panel
//! shortcuts it lists, which are bound from the same table so the two can't
//! drift apart.

use gtk4 as gtk;
use libadwaita as adw;
use libadwaita::prelude::*;

use crate::tr::tr;

/// `app.show-panel::<id>` and its key, in the main menu's order.
const PANELS: &[(&str, &str, &str)] = &[
    ("chat", "<Control>1", "Chat"),
    ("users", "<Control>2", "Users"),
    ("news", "<Control>3", "News"),
    ("news15", "<Control>4", "News (1.5+)"),
    ("tasks", "<Control>5", "Tasks"),
    #[cfg(feature = "voice")]
    ("video", "<Control>6", "Video"),
];

/// A shortcut: the keys (any of which does it) and what it does.
type Row = (&'static [&'static str], String);

/// Every other key, by where it works: a title, a note when the group needs
/// one, and the rows.
fn sections() -> Vec<(String, Option<String>, Vec<Row>)> {
    vec![
        (
            tr("General"),
            None,
            vec![
                (&["<Control>k"][..], tr("Connect")),
                (&["<Control>t"], tr("Tracker")),
                (&["<Control>comma"], tr("Settings")),
                (&["F10"], tr("Main menu")),
                (&["<Control>question"], tr("Keyboard Shortcuts")),
                (&["<Control>w"], tr("Close the window")),
                (&["Escape"], tr("Close a dialog")),
                (&["<Control>q"], tr("Quit")),
            ],
        ),
        (
            tr("Panes"),
            Some(tr(
                "The Alt keys don't work while the focus is in a text field.",
            )),
            vec![
                (
                    &["F6", "<Shift>F6"][..],
                    tr("Next and previous pane, from anywhere"),
                ),
                (
                    &["<Control><Shift>Page_Up", "<Control><Shift>Page_Down"],
                    tr("Move the tab along its strip"),
                ),
                (
                    &["<Alt>Left", "<Alt>Right", "<Alt>Up", "<Alt>Down"][..],
                    tr("Move the focus to the pane that way"),
                ),
                (
                    &[
                        "<Alt><Shift>Left",
                        "<Alt><Shift>Right",
                        "<Alt><Shift>Up",
                        "<Alt><Shift>Down",
                    ],
                    tr("Move the pane in front that way"),
                ),
                (
                    &["<Alt>backslash", "<Alt>minus"],
                    tr("Split the pane off to the right, or below"),
                ),
                (&["<Alt>Return"], tr("Fill the window with one pane")),
                (&["<Alt>w"], tr("Close the pane")),
                (&["<Alt>0"], tr("Reset the layout")),
            ],
        ),
        (
            tr("Chat"),
            None,
            vec![
                (&["Return"][..], tr("Send")),
                (&["<Shift>Return"], tr("New line")),
                (
                    &["Tab", "<Shift>Tab"],
                    tr("Complete a nickname, forward or back"),
                ),
                (&["Up", "Down"], tr("Earlier and later lines you sent")),
                (
                    &["<Control>b", "<Control>i", "<Control><Shift>c"],
                    tr("Bold, italic, code"),
                ),
                (&["<Control>u"], tr("Clear the input")),
                (&["Page_Up", "Page_Down"], tr("Scroll the chat")),
                (
                    &["<Control>plus", "<Control>minus", "<Control>0"],
                    tr("Larger, smaller and normal text"),
                ),
                (
                    &["<Control>Tab", "<Control><Shift>Tab"],
                    tr("Next and previous tab"),
                ),
                (&["<Control>f"], tr("Find")),
                (&["<Control>g", "F3"], tr("Next match")),
                (&["<Control><Shift>g", "<Shift>F3"], tr("Previous match")),
            ],
        ),
        (
            tr("News"),
            None,
            vec![
                (&["<Control>f"][..], tr("Find")),
                (&["<Control>g"], tr("Next match, while finding")),
                (&["<Control><Shift>g"], tr("Previous match, while finding")),
            ],
        ),
        (
            tr("Users"),
            None,
            vec![(&["Menu", "<Shift>F10"][..], tr("A user's menu"))],
        ),
        (
            tr("Text"),
            None,
            vec![
                (&["F7"][..], tr("Show the cursor in read-only text")),
                (
                    &["Menu", "<Shift>F10", "Return"],
                    tr("A link's menu, with the cursor on it"),
                ),
            ],
        ),
        (
            tr("Files"),
            None,
            vec![
                (&["Tab"][..], tr("The other list")),
                (&["F4"], tr("Open")),
                (&["BackSpace"], tr("Up a folder")),
                (&["F2"], tr("Rename")),
                (&["F3", "<Control>p"], tr("Preview")),
                (&["F5"], tr("Copy to the other list")),
                (&["F6"], tr("Move")),
                (&["F7", "<Control>n"], tr("New folder")),
                (&["F8", "Delete", "<Control>d"], tr("Delete")),
                (&["<Control>i"], tr("Get info")),
                (&["<Control>r"], tr("Reload")),
                (&["Menu", "<Shift>F10"], tr("A file's menu")),
            ],
        ),
    ]
}

/// The keys as they read on the keyboard, e.g. "Ctrl+G, F3".
fn keys_label(accels: &[&str]) -> String {
    accels
        .iter()
        .filter_map(|a| gtk::accelerator_parse(*a))
        .map(|(key, mods)| gtk::accelerator_get_label(key, mods).to_string())
        .collect::<Vec<_>>()
        .join(", ")
}

fn group(title: &str, note: Option<&str>, rows: &[Row]) -> adw::PreferencesGroup {
    let group = adw::PreferencesGroup::new();
    group.set_title(title);
    group.set_description(note);
    for (accels, what) in rows {
        let row = adw::ActionRow::new();
        row.set_title(what);
        let keys = gtk::Label::new(Some(&keys_label(accels)));
        keys.add_css_class("dim-label");
        keys.add_css_class("numeric");
        row.add_suffix(&keys);
        group.add(&row);
    }
    group
}

fn show(parent: Option<gtk::Window>) {
    let page = adw::PreferencesPage::new();
    let panels: Vec<Row> = PANELS
        .iter()
        .map(|(_, accel, label)| (std::slice::from_ref(accel), tr(label)))
        .collect();
    page.add(&group(&tr("Panels"), None, &panels));
    for (title, note, rows) in sections() {
        page.add(&group(&title, note.as_deref(), &rows));
    }

    let view = adw::ToolbarView::new();
    view.add_top_bar(&adw::HeaderBar::new());
    view.set_content(Some(&page));

    let dialog = adw::Dialog::new();
    dialog.set_title(&tr("Keyboard Shortcuts"));
    dialog.set_content_width(520);
    dialog.set_content_height(640);
    dialog.set_child(Some(&view));
    dialog.present(parent.as_ref());
}

/// `void gtkhx_shortcuts_install (GtkApplication *app)` — the
/// `app.shortcuts` action, and the keys for it and for each panel.
///
/// # Safety
/// `app` is a valid `GtkApplication *`. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_shortcuts_install(app: *mut gtk::ffi::GtkApplication) {
    use glib::translate::from_glib_none;
    let app: gtk::Application = from_glib_none(app);
    let action = gtk::gio::SimpleAction::new("shortcuts", None);
    let weak = app.downgrade();
    action.connect_activate(move |_, _| {
        show(weak.upgrade().and_then(|a| a.active_window()));
    });
    app.add_action(&action);
    app.set_accels_for_action("app.shortcuts", &["<Control>question"]);
    for (id, accel, _) in PANELS {
        app.set_accels_for_action(&format!("app.show-panel::{id}"), &[accel]);
    }
}
