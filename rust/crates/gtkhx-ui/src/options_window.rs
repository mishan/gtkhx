//! The Settings window: the dialog, the sidebar, and the page table.
//!
//! This is the last piece of Settings that was C. What it retires is not the
//! window so much as the *seam*: every page used to be reached through a
//! `void (*draw)(AdwPreferencesPage *)` in a C table, which meant each page
//! needed a `#[no_mangle] extern "C"` export, a `from_glib_borrow` of a
//! C-owned page, and a `ensure_gtk_init()` because any page could be the
//! first Rust code GTK ever reached. With the table on this side, `draw` is
//! an ordinary `fn(&adw::PreferencesPage)`, the exports are gone, and the
//! init guard happens once here — at the single point C still calls in.
//!
//! The layout is unchanged from the C version: an `AdwDialog` wrapping an
//! `AdwNavigationSplitView`, sidebar of categories on the left grouped under
//! section headers, an `AdwPreferencesPage` per entry in a `GtkStack` on the
//! right. (It is deliberately not an `AdwPreferencesDialog`, whose built-in
//! navigation is a header-bar view switcher — the trade was losing its search
//! entry in exchange for a sidebar that holds this many categories.)

use crate::tr::tr;
use gtk4 as gtk;
use gtk4::glib;
use gtk4::prelude::*;
use libadwaita as adw;
use libadwaita::prelude::*;
use std::cell::RefCell;

/// One sidebar row and the page behind it.
struct Entry {
    /// Section header to draw above this row, or `None` to continue the
    /// previous section.
    section: Option<&'static str>,
    /// Stable `GtkStack` child name.
    name: &'static str,
    /// Sidebar and content-header title.
    title: &'static str,
    icon: &'static str,
    /// The page builder. A plain fn pointer — this is the whole point of the
    /// change; it used to be a C function pointer through an FFI export.
    draw: fn(&adw::PreferencesPage),
}

/// The sidebar, in order. A non-`None` `section` starts a new header group.
///
/// Titles and sections are untranslated here and passed through `tr()` at
/// build time, matching the C table's `N_()` markers — the strings have to be
/// literals for extraction, but must be translated after the locale is known.
fn entries() -> Vec<Entry> {
    let mut v = vec![
        Entry {
            section: Some("General"),
            name: "general",
            title: "General",
            icon: "preferences-system-symbolic",
            draw: crate::options::page_general,
        },
        Entry {
            section: None,
            name: "identity",
            title: "Identity",
            icon: "user-info-symbolic",
            draw: crate::options::page_identity,
        },
        Entry {
            section: None,
            name: "filexfer",
            title: "File Transfers",
            icon: "folder-download-symbolic",
            draw: crate::options::page_file_transfers,
        },
        Entry {
            section: Some("Chat"),
            name: "chat_appearance",
            title: "Appearance",
            icon: "user-available-symbolic",
            draw: crate::options::page_chat_appearance,
        },
        Entry {
            section: None,
            name: "chat_behavior",
            title: "Behavior",
            icon: "preferences-other-symbolic",
            draw: crate::options::page_chat_behavior,
        },
        Entry {
            section: None,
            name: "chat_history",
            title: "History",
            icon: "document-open-recent-symbolic",
            draw: crate::options::page_chat_history,
        },
        Entry {
            section: None,
            name: "chat_emoji",
            title: "Emoji",
            icon: "face-smile-symbolic",
            draw: crate::options::page_chat_emoji,
        },
        Entry {
            section: Some("Notifications"),
            name: "notify_events",
            title: "Events",
            icon: "preferences-system-notifications-symbolic",
            draw: crate::options::page_notify_events,
        },
        Entry {
            section: None,
            name: "notify_behavior",
            title: "Behavior",
            icon: "preferences-other-symbolic",
            draw: crate::options::page_notify_behavior,
        },
        Entry {
            section: Some("Audio"),
            name: "sound",
            title: "Sound",
            icon: "audio-speakers-symbolic",
            draw: crate::options::page_sound,
        },
    ];

    // Voice sits under Audio beside Sound, and only exists in a build that
    // has it — same `#ifdef HAVE_VOICE` shape the C table had, one layer
    // down. Pushed here rather than declared with a `#[cfg]` attribute on a
    // struct literal so the Network section below doesn't have to know.
    #[cfg(feature = "voice")]
    v.push(Entry {
        section: None,
        name: "voice",
        title: "Voice",
        icon: "audio-input-microphone-symbolic",
        draw: crate::options::page_voice,
    });

    v.push(Entry {
        section: Some("Network"),
        name: "connections",
        title: "Connections",
        icon: "user-bookmarks-symbolic",
        draw: crate::options::page_connections,
    });
    v.push(Entry {
        section: None,
        name: "trackers",
        title: "Trackers",
        icon: "network-server-symbolic",
        draw: crate::options::page_tracker,
    });
    v
}

thread_local! {
    /// The live dialog, so a second Settings request re-presents rather than
    /// building a duplicate.
    ///
    /// Weak, *and* cleared on close. Weak because a strong reference here
    /// would keep the whole page tree alive after the dialog goes away; and
    /// cleared explicitly because "the weak reference died" is not the same
    /// claim as "the dialog is gone". A closed AdwDialog normally finalizes
    /// synchronously inside `close()`, but anything still holding a reference
    /// to it — a strong widget clone captured into one of its own rows, say —
    /// leaves a closed, still-parented dialog reachable here, and
    /// re-presenting that one is a `gtk_widget_set_parent` critical rather
    /// than a reopened window. The C global was nulled from the `closed`
    /// handler regardless of refcount; this keeps that guarantee.
    static WINDOW: RefCell<Option<glib::WeakRef<adw::Dialog>>> = const { RefCell::new(None) };
    /// The live sidebar, so a page request against an already-open window can
    /// move the selection. Weak for the same reason as `WINDOW`, and dropped
    /// with it.
    static SIDEBAR: RefCell<Option<glib::WeakRef<gtk::ListBox>>> = const { RefCell::new(None) };
}

fn existing() -> Option<adw::Dialog> {
    WINDOW.with(|w| w.borrow().as_ref().and_then(|weak| weak.upgrade()))
}

/// The section label above the first row of a group.
///
/// **Index-coupled**: this and the row-selected handler both address
/// `entries` by `row.index()`, which is the row's *sorted and filtered*
/// position. The C stashed the strings on the row itself and so was
/// reorder-proof. Do not put a sort or filter func on this list box — the
/// obvious reason to want one is a search entry, which is what dropping
/// `AdwPreferencesDialog` cost us — without moving this back onto the rows.
fn section_header(row: &gtk::ListBoxRow, before: Option<&gtk::ListBoxRow>, entries: &[Entry]) {
    let idx = row.index();
    if idx < 0 {
        return;
    }
    let Some(section) = entries.get(idx as usize).and_then(|e| e.section) else {
        row.set_header(gtk::Widget::NONE);
        return;
    };
    // A row that starts a section always gets a header; `before` only decides
    // how much space goes above it.
    let first = before.is_none();

    let label = gtk::Label::new(Some(&tr(section)));
    label.add_css_class("heading");
    label.add_css_class("dim-label");
    label.set_xalign(0.0);
    label.set_margin_start(12);
    label.set_margin_end(12);
    label.set_margin_top(if first { 6 } else { 12 });
    label.set_margin_bottom(3);
    row.set_header(Some(&label));
}

/// Select the sidebar row whose entry has this stack name.
///
/// The lookup is by name rather than by index because the caller is C passing
/// a string constant, and because the table's order is not something a call
/// site should have to know.
fn select_page(listbox: &gtk::ListBox, entries: &[Entry], name: &str) {
    // An unknown name falls back to the first page rather than selecting
    // nothing: GtkStack shows its first child regardless, so leaving the
    // sidebar unselected would put the window in a state where the highlight
    // and the content disagree.
    let idx = entries
        .iter()
        .position(|e| e.name == name)
        .unwrap_or_else(|| {
            glib::g_warning!("gtkhx", "no settings page named {name:?}");
            0
        });
    if let Some(row) = listbox.row_at_index(idx as i32) {
        listbox.select_row(Some(&row));
    }
}

/// Open Settings, or re-present it if it is already up.
///
/// `page` names a stack child to select instead of the first one — how a
/// menu item can point straight at one page.
fn present(page: Option<&str>) {
    // GTK is initialised from C, so gtk4-rs's own init flag isn't set;
    // building adw widgets without this trips libadwaita-rs's
    // assert_initialized_main_thread!. One call here covers every page,
    // because this is the only way in.
    crate::ensure_gtk_init();

    let parent = unsafe { crate::ffi::gtkhx_active_window() };
    let parent: Option<gtk::Window> =
        (!parent.is_null()).then(|| unsafe { glib::translate::from_glib_none(parent) });

    if let Some(dialog) = existing() {
        // Already up: still honour the requested page, so a second menu item
        // aimed at a different one doesn't just raise whatever was showing.
        if let Some(name) = page {
            if let Some(listbox) = SIDEBAR.with_borrow(|s| s.as_ref().and_then(|w| w.upgrade())) {
                let entries = entries();
                select_page(&listbox, &entries, name);
            }
        }
        dialog.present(parent.as_ref());
        return;
    }

    let entries = std::rc::Rc::new(entries());

    // A plain AdwDialog: it handles transient-for, modality and adaptive
    // sizing itself. content_width is the *preferred* size and has to fit
    // sidebar plus page side by side or Adw warns that the split view exceeds
    // the dialog width; the size request is the collapsed *minimum*, without
    // which Adw warns the dialog has none. Below the breakpoint only one pane
    // is visible, so the minimum only has to fit one.
    let dialog = adw::Dialog::new();
    dialog.set_title(&tr("GtkHx Preferences"));
    dialog.set_content_width(920);
    dialog.set_content_height(680);
    dialog.set_size_request(360, 480);

    // Esc closes via AdwDialog's built-in close response; this adds Ctrl+W
    // and Ctrl+Q for parity with the rest of the app's windows.
    unsafe {
        crate::ffi::gtkhx_dialog_add_close_shortcuts(dialog.as_ptr() as *mut _);
    }

    // Claimed before the pages are built, as the C did: a page builder that
    // somehow reached back in here should re-present, not recurse.
    WINDOW.with(|w| *w.borrow_mut() = Some(dialog.downgrade()));

    let stack = gtk::Stack::new();
    stack.set_hexpand(true);
    stack.set_vexpand(true);

    let listbox = gtk::ListBox::new();
    listbox.set_selection_mode(gtk::SelectionMode::Single);
    listbox.add_css_class("navigation-sidebar");

    for entry in entries.iter() {
        let title = tr(entry.title);

        let page = adw::PreferencesPage::new();
        page.set_title(&title);
        (entry.draw)(&page);
        stack.add_named(&page, Some(entry.name));

        let rbox = gtk::Box::new(gtk::Orientation::Horizontal, 12);
        rbox.set_margin_start(6);
        rbox.set_margin_end(6);
        rbox.set_margin_top(8);
        rbox.set_margin_bottom(8);
        let img = gtk::Image::from_icon_name(entry.icon);
        let lbl = gtk::Label::new(Some(&title));
        lbl.set_xalign(0.0);
        rbox.append(&img);
        rbox.append(&lbl);

        let row = gtk::ListBoxRow::new();
        row.set_child(Some(&rbox));
        listbox.append(&row);
    }

    // The header func and the selection handler both key off the row's index
    // into `entries`, which is why neither needs the qdata stash the C
    // version carried on every row.
    let for_header = entries.clone();
    listbox.set_header_func(move |row, before| section_header(row, before, &for_header));

    let sidebar_scroll = gtk::ScrolledWindow::new();
    sidebar_scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    sidebar_scroll.set_child(Some(&listbox));
    sidebar_scroll.set_vexpand(true);

    let sidebar_tv = adw::ToolbarView::new();
    sidebar_tv.add_top_bar(&adw::HeaderBar::new());
    sidebar_tv.set_content(Some(&sidebar_scroll));
    let sidebar_page = adw::NavigationPage::new(&sidebar_tv, &tr("Preferences"));

    let content_tv = adw::ToolbarView::new();
    content_tv.add_top_bar(&adw::HeaderBar::new());
    content_tv.set_content(Some(&stack));
    let content_page = adw::NavigationPage::new(&content_tv, &tr("General"));

    let split = adw::NavigationSplitView::new();
    split.set_sidebar(Some(&sidebar_page));
    split.set_content(Some(&content_page));
    split.set_max_sidebar_width(240.0);

    // Weak throughout: these three are all inside the dialog's widget tree,
    // and the handler is owned by the list box, so strong clones would be a
    // cycle that outlives the dialog.
    let stack_weak = stack.downgrade();
    let split_weak = split.downgrade();
    let content_weak = content_page.downgrade();
    let for_select = entries.clone();
    listbox.connect_row_selected(move |_, row| {
        let Some(row) = row else {
            return;
        };
        let idx = row.index();
        let Some(entry) = usize::try_from(idx).ok().and_then(|i| for_select.get(i)) else {
            return;
        };
        if let Some(stack) = stack_weak.upgrade() {
            stack.set_visible_child_name(entry.name);
        }
        if let Some(content) = content_weak.upgrade() {
            content.set_title(&tr(entry.title));
        }
        // When collapsed, picking a category is also the navigation gesture.
        if let Some(split) = split_weak.upgrade() {
            split.set_show_content(true);
        }
    });

    dialog.set_child(Some(&split));

    // Adaptive: collapse to a single navigable pane on narrow widths.
    match adw::BreakpointCondition::parse("max-width: 500sp") {
        Ok(condition) => {
            let bp = adw::Breakpoint::new(condition);
            bp.add_setter(&split, "collapsed", Some(&true.to_value()));
            dialog.add_breakpoint(bp);
        }
        // A literal that doesn't parse is our bug, not the user's, and the
        // dialog is still usable without the breakpoint — it just won't
        // collapse on a narrow display. Complain rather than fail to open.
        Err(e) => glib::g_critical!("gtkhx", "settings breakpoint condition: {e}"),
    }

    // Select a category so the content pane isn't blank — the requested one
    // if the caller named one, else the first.
    match page {
        Some(name) => select_page(&listbox, &entries, name),
        None => {
            if let Some(first) = listbox.row_at_index(0) {
                listbox.select_row(Some(&first));
            }
        }
    }
    SIDEBAR.with_borrow_mut(|s| *s = Some(listbox.downgrade()));

    dialog.connect_closed(|_| {
        WINDOW.with(|w| *w.borrow_mut() = None);
        SIDEBAR.with_borrow_mut(|s| *s = None);
    });
    dialog.present(parent.as_ref());
}

/// Open the Settings window. Called from `toolbar.c`.
///
/// # Safety
/// Must be called on the main thread, after GTK is up.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_create_options_window() {
    present(None);
}

/// Open Settings with `page` selected. `page` is a stack child name from the
/// table above; an unknown one warns and opens on the first page.
///
/// # Safety
/// `page` is a valid NUL-terminated C string. Main thread, after GTK is up.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_open_settings_page(page: *const std::ffi::c_char) {
    let name = crate::cstr(page);
    present((!name.is_empty()).then_some(name.as_str()));
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// `GtkStack` child names have to be distinct: `add_named` with a name
    /// already in the stack drops the earlier child, so a duplicate would
    /// silently cost a whole page — and the sidebar row for it would select
    /// its twin. The C table had the same requirement with nothing checking
    /// it, which is why this exists at all. No display needed.
    #[test]
    fn stack_names_are_unique() {
        let entries = entries();
        let mut seen = std::collections::HashSet::new();
        for e in &entries {
            assert!(
                seen.insert(e.name),
                "duplicate stack child name {:?}",
                e.name
            );
        }
    }

    /// Every row draws an icon and a label, and every page needs a name to be
    /// addressable in the stack. An empty string in any of the three renders
    /// as a blank rather than failing, so nothing else would catch it.
    #[test]
    fn every_entry_is_fully_populated() {
        for e in &entries() {
            assert!(!e.name.is_empty());
            assert!(!e.title.is_empty(), "{} has no title", e.name);
            assert!(!e.icon.is_empty(), "{} has no icon", e.name);
        }
    }

    /// The header func draws a section label above any row carrying one, so
    /// the first row must carry one or the list opens with an ungrouped row
    /// floating above the first header.
    #[test]
    fn first_entry_opens_a_section() {
        assert!(entries()[0].section.is_some());
    }

    /// The Voice page exists exactly when the feature does. Getting this
    /// backwards is invisible in whichever configuration you happen to build,
    /// which is the whole reason CI builds both.
    #[test]
    fn voice_entry_tracks_the_feature() {
        let has_voice = entries().iter().any(|e| e.name == "voice");
        assert_eq!(has_voice, cfg!(feature = "voice"));
    }

    /// Build every page, the way opening Settings does.
    ///
    /// This is the test the Settings port never had. A row builder that
    /// panics — an unwrap on a missing widget, a bad `clamp` range, an
    /// adw call that asserts — used to be discoverable only by opening the
    /// dialog by hand, and only on the page you happened to open. Here one
    /// run covers all of them in both feature configurations.
    ///
    /// It does *not* check that a page looks right. The C boundary is
    /// stubbed (see `options_test_stubs`), so every value reads as empty or
    /// zero — but the *schema* lookup is real, which is what matters: each
    /// row checks its key's kind first and returns an inert placeholder on a
    /// mismatch, so a stubbed-out schema would mean almost none of this code
    /// ran. With the real one, the rows take their live path.
    ///
    /// Needs a display, and so is driven by `crate::gtk_tests` rather than
    /// being a `#[test]` of its own — see there for why the crate gets
    /// exactly one.
    pub(crate) fn check_every_page_builds() {
        for entry in &entries() {
            let page = adw::PreferencesPage::new();
            page.set_title(entry.title);
            (entry.draw)(&page);
        }
    }
}
