//! Path completion for a local path entry.
//!
//! Pops a list of the subdirectories matching what the user is typing under
//! the entry: the local panel's path bar, and the Move dialog's destination
//! when the source is local. Remote paths aren't completed — enumerating a
//! Hotline folder is a round trip to the server per keystroke.
//!
//! - **Directories only.** A file isn't somewhere to go, and Enter on one in
//!   the listing already opens it. A symlink to a directory counts.
//! - **Smart case**, as in Vim and fzf: case-insensitive until the prefix holds
//!   an uppercase letter, then case-sensitive.
//! - **Hidden names** (leading dot) appear only once the prefix starts with a
//!   dot too.
//!
//! Keys, caught on the entry while the popover is up: Down / Up move the
//! highlight; Tab inserts the highlighted name; Enter inserts it only once the
//! user has moved off the first row, so typing a whole path and pressing Enter
//! still goes there; Escape closes the popover.

use std::cell::{Cell, RefCell};
use std::path::Path;
use std::rc::{Rc, Weak};

use gtk::gdk;
use gtk::glib;
use gtk4 as gtk;
use gtk4::prelude::*;

/// How many rows the popover shows before it scrolls.
const VISIBLE_ROWS: i32 = 8;
/// A row's height, near enough, for sizing the scroll cap.
const ROW_HEIGHT: i32 = 32;

/// Split an entry's text at its last `/`: the directory to list (with its
/// trailing slash) and the name prefix typed after it. `None` without a slash —
/// a relative path isn't completed, and the local panel always shows an
/// absolute one.
fn split_path(text: &str) -> Option<(&str, &str)> {
    let slash = text.rfind('/')?;
    Some((&text[..=slash], &text[slash + 1..]))
}

/// Whether `name` belongs in the suggestions for `prefix`.
fn matches(name: &str, prefix: &str) -> bool {
    if name.is_empty() || (name.starts_with('.') && !prefix.starts_with('.')) {
        return false;
    }
    if prefix.chars().any(char::is_uppercase) {
        name.starts_with(prefix)
    } else {
        // Case folding rather than ASCII-only lowering, so `mú` finds `Música`.
        glib::casefold(name).starts_with(glib::casefold(prefix).as_str())
    }
}

/// The names of the directories directly under `dir`, sorted without regard
/// to case so the order holds still from one keystroke to the next. Empty when
/// `dir` can't be read. Names that aren't UTF-8 are left out, since the entry
/// can't hold them.
fn list_subdirs(dir: &Path) -> Vec<String> {
    let Ok(read) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut names: Vec<(glib::GString, String)> = read
        .flatten()
        // fs::metadata follows symlinks, so a link to a directory is listed.
        .filter(|e| std::fs::metadata(e.path()).is_ok_and(|m| m.is_dir()))
        .filter_map(|e| e.file_name().into_string().ok())
        .map(|n| (glib::casefold(&n), n))
        .collect();
    names.sort();
    names.into_iter().map(|(_, n)| n).collect()
}

struct Inner {
    /// Weak, and cleared when the entry is destroyed: the entry lives in the
    /// panel's or dialog's widget tree and can go before the completer is
    /// freed, which is the ordinary order when a Files window closes.
    entry: RefCell<Option<glib::WeakRef<gtk::Entry>>>,
    /// Parented to the entry, so it has to be unparented while that is still
    /// allowed: at the entry's destroy, not its finalize.
    popover: RefCell<Option<gtk::Popover>>,
    store: gtk::StringList,
    filter: gtk::CustomFilter,
    filtered: gtk::FilterListModel,
    selection: gtk::SingleSelection,
    /// The directory `store` holds, so typing within one directory refilters
    /// instead of reading it again.
    listed_dir: RefCell<Option<String>>,
    /// The prefix the filter matches, shared with the filter's closure.
    prefix: Rc<RefCell<String>>,
    /// Set while the completer itself writes the entry's text.
    updating: Cell<bool>,
}

impl Inner {
    fn entry(&self) -> Option<gtk::Entry> {
        self.entry.borrow().as_ref().and_then(|w| w.upgrade())
    }

    fn popped(&self) -> bool {
        self.popover
            .borrow()
            .as_ref()
            .is_some_and(|p| p.is_visible())
    }

    fn hide(&self) {
        if let Some(p) = self.popover.borrow().as_ref() {
            p.popdown();
        }
    }

    fn show(&self, entry: &gtk::Entry) {
        if let Some(p) = self.popover.borrow().as_ref() {
            let w = entry.width();
            if w > 0 {
                p.set_size_request(w, -1);
            }
            p.popup();
        }
    }

    /// Recompute the suggestions after the entry's text changed.
    fn on_text_changed(&self) {
        if self.updating.get() {
            return;
        }
        let Some(entry) = self.entry() else {
            return;
        };
        // The panel sets the text on every navigation; suggest only while the
        // user is the one typing.
        if !has_focus_within(&entry) {
            self.hide();
            return;
        }
        self.refresh(&entry);
    }

    fn refresh(&self, entry: &gtk::Entry) {
        let text = entry.text();
        let Some((dir, prefix)) = split_path(&text) else {
            self.hide();
            return;
        };
        if self.listed_dir.borrow().as_deref() != Some(dir) {
            let names = list_subdirs(Path::new(dir));
            let names: Vec<&str> = names.iter().map(String::as_str).collect();
            self.store.splice(0, self.store.n_items(), &names);
            *self.listed_dir.borrow_mut() = Some(dir.to_owned());
        }
        *self.prefix.borrow_mut() = prefix.to_owned();
        self.filter.changed(gtk::FilterChange::Different);

        if self.filtered.n_items() == 0 {
            self.hide();
            return;
        }
        // Highlight the first match, so Tab has something to insert.
        self.selection.set_selected(0);
        self.show(entry);
    }

    /// Replace the typed prefix with the suggestion at `pos`, plus a trailing
    /// slash that both marks it a directory and starts completing the next
    /// level.
    fn accept(&self, pos: u32) {
        let Some(entry) = self.entry() else {
            return;
        };
        let Some(name) = self
            .filtered
            .item(pos)
            .and_downcast::<gtk::StringObject>()
            .map(|s| s.string())
        else {
            return;
        };
        let dir = self
            .listed_dir
            .borrow()
            .clone()
            .unwrap_or_else(|| "/".into());
        self.updating.set(true);
        entry.set_text(&format!("{dir}{name}/"));
        entry.set_position(-1);
        self.updating.set(false);
        self.on_text_changed();
    }

    fn on_key(&self, key: gdk::Key) -> glib::Propagation {
        use glib::Propagation::{Proceed, Stop};
        if !self.popped() {
            return Proceed;
        }
        let n = self.selection.n_items();
        let sel = self.selection.selected();
        let none = sel == gtk::INVALID_LIST_POSITION;
        match key {
            gdk::Key::Escape => {
                self.hide();
                Stop
            }
            gdk::Key::Down if n > 0 => {
                if none {
                    self.selection.set_selected(0);
                } else if sel + 1 < n {
                    self.selection.set_selected(sel + 1);
                }
                Stop
            }
            gdk::Key::Up => {
                if !none && sel > 0 {
                    self.selection.set_selected(sel - 1);
                }
                Stop
            }
            gdk::Key::Tab if n > 0 => {
                self.accept(if none { 0 } else { sel });
                Stop
            }
            // Only once the user has moved the highlight; otherwise Enter goes
            // to the entry's own activate, which navigates to what's typed.
            gdk::Key::Return | gdk::Key::KP_Enter if !none && sel > 0 => {
                self.accept(sel);
                Stop
            }
            _ => Proceed,
        }
    }

    /// The entry is being destroyed: unparent the popover while that's still
    /// allowed, and forget the entry. Its handlers die with it.
    fn on_entry_destroy(&self) {
        if let Some(p) = self.popover.take() {
            p.unparent();
        }
        self.entry.replace(None);
    }
}

/// Whether keyboard focus is on `entry` or inside it. `has_focus` on the entry
/// itself is never true while typing: a GtkEntry hands focus to its internal
/// GtkText.
fn has_focus_within(entry: &gtk::Entry) -> bool {
    let Some(window) = entry.root().and_downcast::<gtk::Window>() else {
        return false;
    };
    let Some(focus) = gtk::prelude::GtkWindowExt::focus(&window) else {
        return false;
    };
    &focus == entry.upcast_ref::<gtk::Widget>() || focus.is_ancestor(entry)
}

/// Path completion attached to one entry. Dropping it detaches it.
pub struct PathComplete {
    inner: Rc<Inner>,
    handlers: Vec<glib::SignalHandlerId>,
    keys: gtk::EventControllerKey,
}

impl PathComplete {
    pub fn attach(entry: &gtk::Entry) -> Self {
        let store = gtk::StringList::new(&[]);
        let prefix = Rc::new(RefCell::new(String::new()));
        let filter = {
            let prefix = prefix.clone();
            gtk::CustomFilter::new(move |item| {
                item.downcast_ref::<gtk::StringObject>()
                    .is_some_and(|s| matches(&s.string(), &prefix.borrow()))
            })
        };
        let filtered = gtk::FilterListModel::new(Some(store.clone()), Some(filter.clone()));
        let selection = gtk::SingleSelection::new(Some(filtered.clone()));
        // Selection is driven by hand, so "nothing chosen yet" stays distinct
        // from "the first row".
        selection.set_autoselect(false);
        selection.set_can_unselect(true);

        let factory = gtk::SignalListItemFactory::new();
        factory.connect_setup(|_, item| {
            let label = gtk::Label::new(None);
            label.set_xalign(0.0);
            label.set_margin_start(8);
            label.set_margin_end(8);
            label.set_margin_top(4);
            label.set_margin_bottom(4);
            item.downcast_ref::<gtk::ListItem>()
                .expect("list item")
                .set_child(Some(&label));
        });
        factory.connect_bind(|_, item| {
            let item = item.downcast_ref::<gtk::ListItem>().expect("list item");
            if let (Some(label), Some(s)) = (
                item.child().and_downcast::<gtk::Label>(),
                item.item().and_downcast::<gtk::StringObject>(),
            ) {
                label.set_text(&s.string());
            }
        });
        let listview = gtk::ListView::new(Some(selection.clone()), Some(factory));
        listview.set_single_click_activate(true);

        let scrolled = gtk::ScrolledWindow::new();
        scrolled.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
        scrolled.set_max_content_height(ROW_HEIGHT * VISIBLE_ROWS);
        scrolled.set_propagate_natural_height(true);
        scrolled.set_child(Some(&listview));

        // Autohide off, so clicking a row leaves focus in the entry and the
        // user can keep typing. Escape closes it, and so does the next text
        // change made while the entry doesn't have focus.
        let popover = gtk::Popover::new();
        popover.set_has_arrow(false);
        popover.set_autohide(false);
        popover.set_position(gtk::PositionType::Bottom);
        popover.set_parent(entry);
        popover.set_child(Some(&scrolled));
        popover.add_css_class("menu");

        let inner = Rc::new(Inner {
            entry: RefCell::new(Some(entry.downgrade())),
            popover: RefCell::new(Some(popover)),
            store,
            filter,
            filtered,
            selection,
            listed_dir: RefCell::new(None),
            prefix,
            updating: Cell::new(false),
        });

        let weak = Rc::downgrade(&inner);
        listview.connect_activate(move |_, pos| {
            if let Some(inner) = weak.upgrade() {
                inner.accept(pos);
            }
        });

        let with = |f: fn(&Inner)| {
            let weak: Weak<Inner> = Rc::downgrade(&inner);
            move || {
                if let Some(inner) = weak.upgrade() {
                    f(&inner);
                }
            }
        };
        let on_text = with(Inner::on_text_changed);
        let on_destroy = with(Inner::on_entry_destroy);
        let handlers = vec![
            entry.connect_text_notify(move |_| on_text()),
            entry.connect_destroy(move |_| on_destroy()),
        ];

        // Capture phase, ahead of the entry's own bindings: it would move the
        // cursor on Up / Down and activate on Enter.
        let keys = gtk::EventControllerKey::new();
        keys.set_propagation_phase(gtk::PropagationPhase::Capture);
        let weak = Rc::downgrade(&inner);
        keys.connect_key_pressed(move |_, key, _, _| {
            weak.upgrade()
                .map_or(glib::Propagation::Proceed, |inner| inner.on_key(key))
        });
        entry.add_controller(keys.clone());

        PathComplete {
            inner,
            handlers,
            keys,
        }
    }
}

impl Drop for PathComplete {
    fn drop(&mut self) {
        if let Some(entry) = self.inner.entry() {
            for h in self.handlers.drain(..) {
                entry.disconnect(h);
            }
            entry.remove_controller(&self.keys);
        }
        if let Some(p) = self.inner.popover.take() {
            p.unparent();
        }
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    #[test]
    fn splits_at_the_last_slash() {
        assert_eq!(split_path("/"), Some(("/", "")));
        assert_eq!(split_path("/ho"), Some(("/", "ho")));
        assert_eq!(split_path("/home/mi"), Some(("/home/", "mi")));
        assert_eq!(split_path("/home/"), Some(("/home/", "")));
        assert_eq!(split_path("home"), None);
        assert_eq!(split_path(""), None);
    }

    #[test]
    fn smart_case() {
        assert!(matches("Music", "mu"));
        assert!(matches("music", "mu"));
        assert!(matches("Music", "Mu"));
        assert!(!matches("music", "Mu"));
        assert!(matches("Música", "mú"));
        assert!(matches("anything", ""));
        assert!(!matches("Music", "Musics"));
        assert!(!matches("", ""));
    }

    #[test]
    fn hidden_names_need_a_dot() {
        assert!(!matches(".config", ""));
        assert!(!matches(".config", "c"));
        assert!(matches(".config", "."));
        assert!(matches(".config", ".co"));
    }

    /// A scratch directory under the system temp dir, removed on drop.
    struct Scratch(std::path::PathBuf);

    impl Scratch {
        fn new(tag: &str) -> Self {
            let dir =
                std::env::temp_dir().join(format!("gtkhx-complete-{tag}-{}", std::process::id()));
            let _ = std::fs::remove_dir_all(&dir);
            for d in ["beta", "Alpha", "alpine", ".hidden"] {
                std::fs::create_dir_all(dir.join(d)).unwrap();
            }
            std::fs::write(dir.join("afile"), b"").unwrap();
            #[cfg(unix)]
            {
                std::os::unix::fs::symlink(dir.join("beta"), dir.join("linked")).unwrap();
                std::os::unix::fs::symlink(dir.join("gone"), dir.join("broken")).unwrap();
            }
            Scratch(dir)
        }
    }

    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn lists_directories_sorted_without_case() {
        let s = Scratch::new("list");
        let mut want = vec!["Alpha", "alpine", "beta", ".hidden"];
        if cfg!(unix) {
            want.push("linked");
        }
        want.sort_by_key(|n| glib::casefold(n));
        assert_eq!(list_subdirs(&s.0), want);
        assert!(list_subdirs(&s.0.join("missing")).is_empty());
    }

    /// Display-backed: the popover's model follows the typed text, accepting
    /// a row inserts it, and freeing after the entry is destroyed is safe. Called from
    /// `gtk_tests::display_backed`.
    pub(crate) fn check_completes_and_survives_entry_destroy() {
        let s = Scratch::new("gtk");
        let root = format!("{}/", s.0.display());
        let window = gtk::Window::new();
        let entry = gtk::Entry::new();
        window.set_child(Some(&entry));
        let c = PathComplete::attach(&entry);
        let shown = |text: &str| -> Vec<String> {
            entry.set_text(text);
            c.inner.refresh(&entry);
            (0..c.inner.filtered.n_items())
                .filter_map(|i| c.inner.filtered.item(i).and_downcast::<gtk::StringObject>())
                .map(|s| s.string().to_string())
                .collect()
        };
        assert_eq!(shown(&format!("{root}al")), ["Alpha", "alpine"]);
        assert_eq!(shown(&format!("{root}Al")), ["Alpha"]);
        assert_eq!(shown(&format!("{root}.")), [".hidden"]);
        assert_eq!(shown(&format!("{root}zz")), Vec::<String>::new());

        shown(&format!("{root}alpi"));
        c.inner.accept(0);
        assert_eq!(entry.text(), format!("{root}alpine/"));

        let c2 = PathComplete::attach(&entry);
        drop(c);
        // As in the app, only the window holds the entry, so disposing the
        // window takes the entry with it while `c2` is still attached.
        drop(entry);
        window.destroy();
        drop(window);
        assert!(
            c2.inner.popover.borrow().is_none(),
            "the entry's destroy should unparent the popover"
        );
        assert!(c2.inner.entry().is_none());
        drop(c2);
    }
}
