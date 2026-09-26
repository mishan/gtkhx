//! The Files scenario: populate, sort and scroll a large listing in the
//! real `files_panel`, in a window of its own.
//!
//! It drives the same panel the browser builds — column view, sorters,
//! status footer, per-row bindings — without needing a connection. The
//! panel is the next thing on the port list, so this is also the baseline
//! a Rust version will be held to.
//!
//! Two populate paths, because they are different code:
//!
//! - **Remote**: a synthetic FILE_LIST reply decoded by
//!   `gtkhx_files_populate_from_reply` — the decode and store fill a
//!   server's reply goes through — into the store the panel is showing.
//!   That store belongs to a local provider, so the rest of the panel's
//!   wiring is the local side's; the remote provider itself needs a live
//!   connection to fill anything.
//! - **Local**: a real temporary directory, listed by the local provider.
//!   Its listing is synchronous, so the call time is time the UI is
//!   frozen.
//!
//! Filtering is not measured: the panel has no filter.
//!
//! Known-value checks: the populated row count must equal the size asked
//! for, and after a sort the rows must actually be in order. A report
//! with either check failed says so, because its timings would then be
//! timing something other than what they are labeled.

use std::ffi::{c_char, c_void};
use std::path::{Path, PathBuf};

use std::cell::Cell;
use std::rc::Rc;

use glib::translate::{from_glib_full, from_glib_none};
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use hxmodel::files_entry::{
    gtkhx_files_populate_from_reply, hx_file_entry_get_size, hx_file_entry_is_dir,
};

use super::{after_paint, next_frame, warm_up, Report, Stats};

extern "C" {
    fn hx_local_files_provider_new(initial_path: *const c_char) -> *mut c_void;
    fn hx_files_provider_get_listing(provider: *mut c_void) -> *mut gtk::gio::ffi::GListModel;
    fn hx_files_provider_navigate(provider: *mut c_void, path: *const c_char);
    fn files_panel_new(
        provider: *mut c_void,
        swap_cb: Option<
            unsafe extern "C" fn(panel: *mut c_void, want_local: glib::ffi::gboolean, *mut c_void),
        >,
        user_data: *mut c_void,
    ) -> *mut c_void;
    fn files_panel_get_widget(panel: *mut c_void) -> *mut gtk::ffi::GtkWidget;
    fn files_panel_get_column_view(panel: *mut c_void) -> *mut gtk::ffi::GtkWidget;
    fn files_panel_free(panel: *mut c_void);
}

/// Frames sampled while scrolling.
const SCROLL_FRAMES: usize = 120;
/// Column positions, in the order `files_panel.c` adds them.
const COL_NAME: u32 = 0;
const COL_SIZE: u32 = 1;

/// FourCCs for the synthetic listing: a folder every tenth row, the rest a
/// spread of common types so the kind and icon paths vary.
const TYPES: [&[u8; 4]; 6] = [b"TEXT", b"JPEG", b"GIFf", b"APPL", b"SIT!", b"MP3 "];

/// A deterministic spread of sizes, so sorting has real work to do.
fn size_of(i: u32) -> u32 {
    i.wrapping_mul(2_654_435_761) % 50_000_000
}

/// FILE_LIST reply bytes for `n` entries, in the wire layout
/// `hxproto::parse::parse_file_list_entry` reads.
fn file_list_reply(n: u32) -> Vec<u8> {
    let mut buf = Vec::with_capacity(n as usize * 40);
    for i in 0..n {
        let (ftype, name): (&[u8; 4], String) = if i % 10 == 0 {
            (b"fldr", format!("Folder {i:05}"))
        } else {
            (TYPES[i as usize % TYPES.len()], format!("file-{i:05}.dat"))
        };
        let name = name.as_bytes();
        buf.extend_from_slice(&0x00c8u16.to_be_bytes()); // HTLS_DATA_FILE_LIST
        buf.extend_from_slice(&((20 + name.len()) as u16).to_be_bytes());
        buf.extend_from_slice(ftype);
        buf.extend_from_slice(b"MACR");
        buf.extend_from_slice(&size_of(i).to_be_bytes());
        buf.extend_from_slice(&0u32.to_be_bytes());
        buf.extend_from_slice(&(name.len() as u32).to_be_bytes());
        buf.extend_from_slice(name);
    }
    buf
}

/// A fresh empty directory under the temp dir.
fn scratch_dir(tag: &str) -> Option<PathBuf> {
    let base = std::env::temp_dir().join(format!("gtkhx-bench-{}-{tag}", std::process::id()));
    let _ = std::fs::remove_dir_all(&base);
    std::fs::create_dir_all(&base).ok()?;
    Some(base)
}

/// Fill `dir` with `n` sparse files of varied extensions and sizes. Setup,
/// not measured.
fn fill_dir(dir: &Path, n: u32) -> std::io::Result<()> {
    const EXT: [&str; 6] = ["txt", "jpg", "gif", "pdf", "zip", "mp3"];
    for i in 0..n {
        let f = std::fs::File::create(
            dir.join(format!("file-{i:05}.{}", EXT[i as usize % EXT.len()])),
        )?;
        f.set_len(u64::from(size_of(i) % 1_000_000))?;
    }
    Ok(())
}

fn cstr(p: &Path) -> std::ffi::CString {
    std::ffi::CString::new(p.to_string_lossy().as_bytes()).unwrap_or_default()
}

/// Whether the rows, in display order, are sorted by size in `order`
/// within each kind (the panel keeps folders and files apart).
fn sorted_by_size(cv: &gtk::ColumnView, order: gtk::SortType) -> bool {
    let Some(model) = cv.model() else {
        return false;
    };
    let mut prev: Option<(bool, u64)> = None;
    for i in 0..model.n_items() {
        let Some(item) = model.item(i) else {
            return false;
        };
        let p = item.as_ptr() as *mut c_void;
        let row = unsafe { (hx_file_entry_is_dir(p) != 0, hx_file_entry_get_size(p)) };
        if let Some((dir, size)) = prev {
            let in_order = match order {
                gtk::SortType::Descending => size >= row.1,
                _ => size <= row.1,
            };
            if dir == row.0 && !in_order {
                return false;
            }
        }
        prev = Some(row);
    }
    true
}

/// Sort by `col`. Returns the time the sort call took — the sort model
/// sorts synchronously, so this is time the UI is frozen — and the time
/// until the sorted rows had been painted.
async fn sort_by(cv: &gtk::ColumnView, col: u32, order: gtk::SortType) -> (i64, i64) {
    let column = cv
        .columns()
        .item(col)
        .and_downcast::<gtk::ColumnViewColumn>();
    let t = glib::monotonic_time();
    cv.sort_by_column(column.as_ref(), order);
    let call = glib::monotonic_time() - t;
    let painted = after_paint(cv).await - t;
    (call, painted)
}

pub(super) async fn run(n: u32) {
    let (Some(empty), Some(full)) = (scratch_dir("empty"), scratch_dir("full")) else {
        glib::g_warning!(
            "gtkhx",
            "GTKHX_BENCH files: can't create scratch directories"
        );
        return;
    };
    match fill_dir(&full, n) {
        Ok(()) => measure(n, &empty, &full).await,
        Err(e) => glib::g_warning!("gtkhx", "GTKHX_BENCH files: can't fill {full:?}: {e}"),
    }
    let _ = std::fs::remove_dir_all(&empty);
    let _ = std::fs::remove_dir_all(&full);
}

async fn measure(n: u32, empty: &Path, full: &Path) {
    // The panel starts on an empty local directory, so its own initial
    // reload leaves an empty store for the remote-shaped populate to fill.
    let (provider, win, cv) = unsafe {
        let raw = hx_local_files_provider_new(cstr(empty).as_ptr());
        // Keep our own reference for the whole run. The panel holds one
        // too, but drops it in files_panel_free, which runs when the
        // panel's widgets are disposed — not something to rely on while
        // this code still calls into the provider.
        let provider: glib::Object = from_glib_full(raw as *mut glib::gobject_ffi::GObject);
        let panel = files_panel_new(raw, None, std::ptr::null_mut());
        let widget: gtk::Widget = from_glib_none(files_panel_get_widget(panel));
        let cv: gtk::Widget = from_glib_none(files_panel_get_column_view(panel));
        let cv = cv
            .downcast::<gtk::ColumnView>()
            .expect("files_panel_get_column_view returns a GtkColumnView");
        let win = gtk::Window::new();
        win.set_title(Some("GtkHx benchmark: Files"));
        win.set_default_size(1000, 640);
        win.set_child(Some(&widget));
        // Free the panel as its tree comes down, the way the browser does,
        // so no binding outlives the struct it reads.
        let p = panel as usize;
        widget.connect_destroy(move |_| files_panel_free(p as *mut c_void));
        win.present();
        (provider, win, cv)
    };
    let prov = provider.as_ptr() as *mut c_void;
    let closed = Rc::new(Cell::new(false));
    // close-request, not destroy: this function holds the window, so it
    // is never disposed while we run and "destroy" would not fire.
    win.connect_close_request({
        let closed = closed.clone();
        move |_| {
            closed.set(true);
            glib::Propagation::Proceed
        }
    });

    let idle = warm_up(&cv).await;
    let mut r = Report::new("files panel", idle);
    r.line("entries", &format!("{n:9}"), "");
    // Closed mid-run: the panel is gone, so stop here and say so.
    macro_rules! bail_if_closed {
        () => {
            if closed.get() {
                r.line("CHECK FAILED", "", "the window was closed mid-run");
                r.print();
                return;
            }
        };
    }
    bail_if_closed!();

    // ---- remote-shaped populate -----------------------------------------
    let reply = file_list_reply(n);
    let listing: gtk::gio::ListModel =
        unsafe { from_glib_none(hx_files_provider_get_listing(prov)) };
    let t = glib::monotonic_time();
    unsafe {
        let store = listing.as_ptr() as *mut gtk::gio::ffi::GListStore;
        gtkhx_files_populate_from_reply(store, reply.as_ptr(), reply.len());
    }
    let populate = glib::monotonic_time() - t;
    let t = glib::monotonic_time();
    let first_paint = after_paint(&cv).await - t;
    bail_if_closed!();
    let rows = listing.n_items();
    r.ms("remote populate", populate, "decode + append, UI frozen");
    r.ms("  first paint", first_paint, "");
    r.ms(
        "  populate + paint",
        populate + first_paint,
        "compare this one",
    );
    if rows != n {
        r.line(
            "  CHECK FAILED",
            &format!("{rows:9}"),
            &format!("rows shown, expected {n}"),
        );
    }

    // ---- sort -----------------------------------------------------------
    let (call, painted) = sort_by(&cv, COL_SIZE, gtk::SortType::Descending).await;
    bail_if_closed!();
    r.ms("sort by size", call, "sort call, UI frozen");
    r.ms("  sort + paint", painted, "");
    if !sorted_by_size(&cv, gtk::SortType::Descending) {
        r.line("  CHECK FAILED", "", "rows are not in size order");
    }
    let (call, painted) = sort_by(&cv, COL_NAME, gtk::SortType::Ascending).await;
    bail_if_closed!();
    r.ms("sort by name", call, "sort call, UI frozen");
    r.ms("  sort + paint", painted, "");

    // ---- scroll ---------------------------------------------------------
    let adj = cv
        .parent()
        .and_downcast::<gtk::ScrolledWindow>()
        .map(|sw| sw.vadjustment());
    let mut scroll = Vec::with_capacity(SCROLL_FRAMES);
    let mut last = next_frame(&cv).await;
    for _ in 0..SCROLL_FRAMES {
        bail_if_closed!();
        if let Some(adj) = &adj {
            let page = adj.page_size();
            let mut v = adj.value() + page / 3.0;
            if v > adj.upper() - page {
                v = 0.0;
            }
            adj.set_value(v);
        }
        cv.queue_draw();
        let now = next_frame(&cv).await;
        scroll.push(now - last);
        last = now;
    }
    bail_if_closed!();
    r.stats("scroll", &Stats::of(&scroll));

    // ---- local listing --------------------------------------------------
    let t = glib::monotonic_time();
    unsafe { hx_files_provider_navigate(prov, cstr(full).as_ptr()) };
    let list = glib::monotonic_time() - t;
    let t = glib::monotonic_time();
    let first_paint = after_paint(&cv).await - t;
    bail_if_closed!();
    let rows = listing.n_items();
    r.ms("local listing", list, "synchronous, UI frozen");
    r.ms("  first paint", first_paint, "");
    if rows != n {
        r.line(
            "  CHECK FAILED",
            &format!("{rows:9}"),
            &format!("rows shown, expected {n}"),
        );
    }

    r.print();
    // Let go of every widget and model reference before the window comes
    // down, so nothing we hold outlives files_panel_free; the provider,
    // which the panel no longer needs, goes last.
    drop((adj, listing, cv));
    win.destroy();
    drop(provider);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn synthetic_reply_parses_back_to_every_entry() {
        let buf = file_list_reply(250);
        let mut off = 0;
        let mut seen = 0;
        while let Some((e, next)) = hxproto::parse::parse_file_list_entry(&buf, off) {
            assert_eq!(e.fsize, size_of(seen));
            seen += 1;
            off = next;
        }
        assert_eq!(seen, 250);
        assert_eq!(off, buf.len());
    }
}
