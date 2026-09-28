//! Listing a local folder for the local provider, off the main thread.
//!
//! A large folder took the UI with it: enumerating 10,000 files and asking
//! GIO for each one's content-type description froze the window for the
//! whole walk. The walk now runs on GLib's worker pool and hands back plain
//! values; the main thread builds the rows from them and replaces the
//! listing in one splice.
//!
//! The provider is still C (`files_local_provider.c`) and calls
//! [`hx_files_local_list`] for every navigate and reload. The contract it
//! keeps is the provider interface's: "navigated" fires once the listing
//! holds the new folder, and "error" carries a message ready for a toast.

use std::collections::HashMap;
use std::ffi::{c_char, CStr};

use glib::translate::from_glib_none;
use gtk::gio;
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use hxmodel::files_entry::HxFileEntry;

use crate::tr::{tr, tr_fmt};

/// The provider's count of listings started, so a slow listing that
/// finishes after a newer one has started is dropped rather than shown.
const GENERATION: &str = "hx-local-list-generation";

const ATTRIBUTES: &str = "standard::name,standard::display-name,standard::type,\
                          standard::size,standard::is-hidden,standard::content-type,\
                          time::modified";

/// One entry as the worker reads it: plain values, so it can cross threads.
struct Listed {
    name: String,
    is_dir: bool,
    size: u64,
    modified: i64,
    /// The content type's description; `None` for a folder.
    kind: Option<String>,
}

/// What the worker found: the entries it read, and the error that stopped
/// it early if one did.
struct Listing {
    rows: Vec<Listed>,
    partial: Option<String>,
}

/// Read `path`. `Err` when the folder can't be opened at all.
fn enumerate(path: &str) -> Result<Listing, glib::Error> {
    let dir = gio::File::for_path(path);
    let en = dir.enumerate_children(
        ATTRIBUTES,
        gio::FileQueryInfoFlags::NONE,
        gio::Cancellable::NONE,
    )?;
    // A folder holds few distinct types, and the description is the costly
    // part of each entry.
    let mut kinds: HashMap<String, String> = HashMap::new();
    let mut rows = Vec::new();
    let mut partial = None;
    loop {
        let info = match en.next_file(gio::Cancellable::NONE) {
            Ok(Some(info)) => info,
            Ok(None) => break,
            Err(e) => {
                partial = Some(e.message().to_owned());
                break;
            }
        };
        // Dotfiles stay out, as in most file managers.
        if info.is_hidden() {
            continue;
        }
        let name = info
            .attribute_as_string("standard::display-name")
            .map(String::from)
            .unwrap_or_else(|| info.name().to_string_lossy().into_owned());
        let is_dir = info.file_type() == gio::FileType::Directory;
        let kind = (!is_dir).then(|| {
            info.attribute_as_string("standard::content-type")
                .map(|ct| {
                    kinds
                        .entry(ct.to_string())
                        .or_insert_with(|| gio::content_type_get_description(&ct).into())
                        .clone()
                })
                .unwrap_or_default()
        });
        rows.push(Listed {
            name,
            is_dir,
            size: if is_dir { 0 } else { info.size().max(0) as u64 },
            modified: info.attribute_uint64("time::modified") as i64,
            kind,
        });
    }
    Ok(Listing { rows, partial })
}

/// Start listing `path` into `store` for `provider`, and return at once.
fn list(provider: &glib::Object, store: gio::ListStore, path: String) {
    if path.is_empty() {
        provider.emit_by_name::<()>("error", &[&tr("No path to list")]);
        return;
    }
    let generation = unsafe {
        let g = provider
            .data::<u64>(GENERATION)
            .map_or(0, |p| *p.as_ref())
            .wrapping_add(1);
        provider.set_data(GENERATION, g);
        g
    };
    let weak = provider.downgrade();
    glib::spawn_future_local(async move {
        let worker_path = path.clone();
        let result = gio::spawn_blocking(move || enumerate(&worker_path)).await;
        // Closed while the worker ran: nobody is left to show it to.
        let Some(provider) = weak.upgrade() else {
            return;
        };
        let current = unsafe { provider.data::<u64>(GENERATION).map(|p| *p.as_ref()) };
        if current != Some(generation) {
            return;
        }
        let listing = match result {
            Ok(Ok(listing)) => listing,
            Ok(Err(e)) => {
                let msg = tr_fmt("Can't read %1$s: %2$s", &[&path, e.message()]);
                provider.emit_by_name::<()>("error", &[&msg]);
                return;
            }
            Err(panic) => std::panic::resume_unwind(panic),
        };
        let folder = tr("Folder");
        let rows: Vec<glib::Object> = listing
            .rows
            .iter()
            .map(|r| {
                let kind = r.kind.as_deref().unwrap_or(&folder);
                HxFileEntry::build(&r.name, r.is_dir, r.size, r.modified, kind, 0).upcast()
            })
            .collect();
        // One splice, one items-changed: each costs the panel a sort and a
        // status update.
        store.splice(0, store.n_items(), &rows);
        if let Some(e) = listing.partial {
            // Keep what was read and say what stopped it.
            let msg = tr_fmt("Error reading %1$s: %2$s", &[&path, &e]);
            provider.emit_by_name::<()>("error", &[&msg]);
        }
        provider.emit_by_name::<()>("navigated", &[&path]);
    });
}

/// `files_local_provider.c` — list `path` into `listing` for `provider`. The
/// listing is replaced and "navigated" emitted later, on the main thread;
/// a listing overtaken by a newer one for the same provider is dropped.
///
/// # Safety
/// `provider` is a live GObject with the files provider's "error" and
/// "navigated" signals, `listing` a live `GListStore` of `HxFileEntry`, and
/// `path` NULL or a NUL-terminated string. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn hx_files_local_list(
    provider: *mut glib::gobject_ffi::GObject,
    listing: *mut gio::ffi::GListStore,
    path: *const c_char,
) {
    let provider: glib::Object = from_glib_none(provider);
    let store: gio::ListStore = from_glib_none(listing);
    let path = if path.is_null() {
        String::new()
    } else {
        CStr::from_ptr(path).to_string_lossy().into_owned()
    };
    list(&provider, store, path);
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;
    use std::path::Path;
    use std::rc::Rc;
    use std::sync::OnceLock;

    use glib::subclass::prelude::*;
    use glib::subclass::Signal;

    use super::*;

    /// A stand-in carrying the provider's two signals.
    mod imp {
        use super::*;

        #[derive(Default)]
        pub struct FakeProvider;

        #[glib::object_subclass]
        impl ObjectSubclass for FakeProvider {
            const NAME: &'static str = "HxTestLocalListProvider";
            type Type = super::FakeProvider;
            type ParentType = glib::Object;
        }

        impl ObjectImpl for FakeProvider {
            fn signals() -> &'static [Signal] {
                static SIGNALS: OnceLock<Vec<Signal>> = OnceLock::new();
                SIGNALS.get_or_init(|| {
                    vec![
                        Signal::builder("error")
                            .param_types([String::static_type()])
                            .build(),
                        Signal::builder("navigated")
                            .param_types([String::static_type()])
                            .build(),
                    ]
                })
            }
        }
    }

    glib::wrapper! {
        pub struct FakeProvider(ObjectSubclass<imp::FakeProvider>);
    }

    fn scratch(tag: &str) -> std::path::PathBuf {
        let dir =
            std::env::temp_dir().join(format!("gtkhx-local-list-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn write(dir: &Path, name: &str, bytes: usize) {
        std::fs::write(dir.join(name), vec![b'x'; bytes]).unwrap();
    }

    #[test]
    fn enumerate_reads_entries_and_skips_dotfiles() {
        let dir = scratch("enum");
        write(&dir, "a.txt", 12);
        write(&dir, "b.txt", 0);
        write(&dir, ".hidden", 3);
        std::fs::create_dir(dir.join("sub")).unwrap();

        let mut listing = enumerate(&dir.to_string_lossy()).unwrap();
        listing.rows.sort_by(|a, b| a.name.cmp(&b.name));
        let names: Vec<&str> = listing.rows.iter().map(|r| r.name.as_str()).collect();
        assert_eq!(names, ["a.txt", "b.txt", "sub"]);
        assert!(listing.partial.is_none());

        let a = &listing.rows[0];
        assert!(!a.is_dir);
        assert_eq!(a.size, 12);
        assert!(a.modified > 0);
        assert!(a.kind.is_some());
        let sub = &listing.rows[2];
        assert!(sub.is_dir);
        assert_eq!(sub.size, 0);
        assert!(sub.kind.is_none());

        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn enumerate_fails_on_a_missing_folder() {
        let dir = scratch("missing").join("not-there");
        assert!(enumerate(&dir.to_string_lossy()).is_err());
    }

    /// Run `ctx` until `done`, or fail after a generous wait.
    fn run_until(ctx: &glib::MainContext, done: impl Fn() -> bool) {
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
        while !done() {
            assert!(std::time::Instant::now() < deadline, "listing never landed");
            ctx.iteration(true);
        }
    }

    #[test]
    fn a_listing_overtaken_by_a_newer_one_is_dropped() {
        let first = scratch("first");
        let second = scratch("second");
        write(&first, "old.txt", 1);
        write(&second, "new-1.txt", 1);
        write(&second, "new-2.txt", 1);

        let ctx = glib::MainContext::new();
        ctx.with_thread_default(|| {
            let provider: FakeProvider = glib::Object::new();
            let store = gio::ListStore::new::<HxFileEntry>();
            let navigated = Rc::new(RefCell::new(Vec::<String>::new()));
            provider.connect_local("navigated", false, {
                let navigated = navigated.clone();
                move |args| {
                    navigated.borrow_mut().push(args[1].get().unwrap());
                    None
                }
            });

            let p = provider.upcast_ref::<glib::Object>();
            list(p, store.clone(), first.to_string_lossy().into_owned());
            list(p, store.clone(), second.to_string_lossy().into_owned());
            run_until(&ctx, || !navigated.borrow().is_empty());
            // Let the first listing's completion run too, if it hasn't.
            for _ in 0..50 {
                ctx.iteration(false);
                std::thread::sleep(std::time::Duration::from_millis(2));
            }

            assert_eq!(*navigated.borrow(), [second.to_string_lossy().into_owned()]);
            let mut names: Vec<String> = (0..store.n_items())
                .map(|i| store.item(i).and_downcast::<HxFileEntry>().unwrap().name())
                .collect();
            names.sort();
            assert_eq!(names, ["new-1.txt", "new-2.txt"]);
        })
        .unwrap();

        std::fs::remove_dir_all(&first).unwrap();
        std::fs::remove_dir_all(&second).unwrap();
    }

    #[test]
    fn an_unreadable_folder_reports_an_error_and_keeps_the_listing() {
        let missing = scratch("unreadable").join("not-there");
        let ctx = glib::MainContext::new();
        ctx.with_thread_default(|| {
            let provider: FakeProvider = glib::Object::new();
            let store = gio::ListStore::new::<HxFileEntry>();
            store.append(&HxFileEntry::build("kept", false, 0, 0, "", 0));
            let events = Rc::new(RefCell::new(Vec::<&str>::new()));
            for name in ["error", "navigated"] {
                let events = events.clone();
                provider.connect_local(name, false, move |_| {
                    events.borrow_mut().push(name);
                    None
                });
            }

            list(
                provider.upcast_ref(),
                store.clone(),
                missing.to_string_lossy().into_owned(),
            );
            run_until(&ctx, || !events.borrow().is_empty());

            assert_eq!(*events.borrow(), ["error"]);
            assert_eq!(store.n_items(), 1);
        })
        .unwrap();
    }
}
