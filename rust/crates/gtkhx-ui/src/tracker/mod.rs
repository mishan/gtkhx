//! Tracker window (ported from `src/tracker.c`).
//!
//! Exports the C ABI entry points the deleted `tracker.c` provided:
//! `create_tracker_window`, `tracker_batch_begin`, `tracker_server_create`,
//! `tracker_clear`, `tracker_search_refresh`. Everything runs on the GTK
//! main thread; window-wide state lives in the [`WIN`] thread-local, and
//! the compiled search regex in [`SEARCH`] (kept separate so the shared
//! `GtkCustomFilter` match callback can read it without re-entering a
//! `WIN` borrow while a `GListStore` append re-evaluates the filter).
//!
//! Model chain per section (one per tracker URL), mirroring the C build:
//! `GListStore<HxTrackerRow> → GtkFilterListModel (shared filter) →
//! GtkSortListModel (column-header sorter) → GtkSingleSelection →
//! GtkColumnView`.

pub mod ffi;
pub mod meta;
mod row;

use crate::ffi as cffi;
use crate::tr::{tr, trn};
use ffi::HxTrackerServer;
use gtk4 as gtk;
use libadwaita as adw;
use meta::{caps_badges, HxTrackerV3Meta};
use row::HxTrackerRow;

use adw::prelude::*;
use gtk::gdk;
use gtk::gio;
use gtk::glib;
use gtk::pango;
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::ffi::{c_char, CStr, CString};
use std::os::raw::c_void;
use std::rc::Rc;

const INVALID: u32 = gtk::INVALID_LIST_POSITION;

// ---------------------------------------------------------------------
// Small FFI-string helpers.
// ---------------------------------------------------------------------

/// C `char*` → owned `String` (empty on NULL). UTF-8 lossy.
///
/// # Safety
/// `p` is NULL or a valid NUL-terminated C string.
pub(crate) unsafe fn cstr(p: *const c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

/// `&str` → `CString`, dropping any interior NUL (never fails).
fn cs(s: &str) -> CString {
    CString::new(s).unwrap_or_else(|e| {
        let mut v = e.into_vec();
        v.retain(|&b| b != 0);
        CString::new(v).unwrap()
    })
}

/// Raw `*mut GtkWidget` for an FFI call into the C util helpers.
fn wptr<W: IsA<gtk::Widget>>(w: &W) -> *mut cffi::GtkWidget {
    w.as_ref().as_ptr() as *mut cffi::GtkWidget
}

// ---------------------------------------------------------------------
// State.
// ---------------------------------------------------------------------

/// One per-tracker section: a `GtkExpander` over a scrolled
/// `GtkColumnView`, plus the model chain feeding it. Held behind `Rc`
/// so selection / activate closures can capture a `Weak` without a cycle.
struct Section {
    url: String,
    version: std::cell::Cell<u8>,
    expected: std::cell::Cell<u16>,
    store: gio::ListStore,
    filter_model: gtk::FilterListModel,
    #[allow(dead_code)]
    sort_model: gtk::SortListModel,
    selection: gtk::SingleSelection,
    dedup: RefCell<HashSet<String>>,
    expander: gtk::Expander,
    col_country: gtk::ColumnViewColumn,
    col_caps: gtk::ColumnViewColumn,
    /// Hold a strong ref to every column for the section's lifetime.
    #[allow(dead_code)]
    columns: Vec<gtk::ColumnViewColumn>,
}

struct Win {
    window: gtk::Window,
    sections_box: gtk::Box,
    search_entry: gtk::SearchEntry,
    lbl_found: gtk::Label,
    lbl_total: gtk::Label,
    filter: gtk::CustomFilter,
    order: Vec<Rc<Section>>,
    by_url: HashMap<String, Rc<Section>>,
    current: Option<Rc<Section>>,
    selected: Option<(Rc<Section>, u32)>,
    num_found_total: i32,
    num_total_total: i32,
    /// `session *` from `create_tracker_window`'s `data`, held opaquely as
    /// a raw pointer (not `usize`) so it keeps its provenance across the
    /// FFI round-trip. Main-thread only, so no `Send`/`Sync` needed.
    sess: *mut c_void,
}

thread_local! {
    static WIN: RefCell<Option<Win>> = const { RefCell::new(None) };
    static SEARCH: RefCell<Option<glib::Regex>> = const { RefCell::new(None) };
}

// ---------------------------------------------------------------------
// Matching / comparison.
// ---------------------------------------------------------------------

fn regex_hit(re: &glib::Regex, text: &str) -> bool {
    let gs = glib::GString::from(text);
    re.match_(gs.as_ref(), glib::RegexMatchFlags::empty())
        .map(|mi| mi.matches())
        .unwrap_or(false)
}

/// Shared `GtkCustomFilter` match: NULL search → match all; else name OR
/// desc. Reads only `SEARCH`, so a `GListStore` append can re-evaluate
/// this while a `WIN` borrow is live.
fn row_matches(obj: &glib::Object) -> bool {
    let Some(row) = obj.downcast_ref::<HxTrackerRow>() else {
        return false;
    };
    SEARCH.with_borrow(|s| match s {
        None => true,
        Some(re) => regex_hit(re, &row.name()) || regex_hit(re, &row.desc()),
    })
}

/// Locale-aware string compare via `g_utf8_collate` (matches the C
/// `cmp_str` so accented names sort as the user expects).
fn coll(a: &str, b: &str) -> gtk::Ordering {
    let (ca, cb) = (cs(a), cs(b));
    let r = unsafe { glib::ffi::g_utf8_collate(ca.as_ptr(), cb.as_ptr()) };
    r.cmp(&0).into()
}

fn meta_country(row: &HxTrackerRow) -> String {
    let m = row.meta();
    if m.is_null() {
        String::new()
    } else {
        unsafe { cstr((*m).country_code) }
    }
}

fn caps_of(row: &HxTrackerRow) -> String {
    unsafe { caps_badges(row.meta() as *const HxTrackerV3Meta) }
}

// ---------------------------------------------------------------------
// Right-click row detection: stash the row position on each cell's label.
// ---------------------------------------------------------------------

const POS_KEY: &str = "gtkhx-tracker-pos";

// Stash the row's model position on the cell's OWN label — a widget we
// created — never on GtkColumnView's internal cell/row widgets. Writing
// qdata onto GTK's recycled cell/row widgets during bind corrupts its
// cell lifecycle and frees a live cell (a use-after-free crash on the
// first row). On right-click we pick the widget under the pointer and
// walk up to the nearest label carrying this key. bind re-runs on every
// (re)bind, so a re-sort/filter keeps the stashed position current.
fn stash_pos(label: &gtk::Widget, pos: u32) {
    unsafe { label.set_data(POS_KEY, pos) };
}

fn find_stashed_pos(from: &gtk::Widget, top: &gtk::Widget) -> Option<u32> {
    let mut w = Some(from.clone());
    while let Some(cur) = w {
        if cur == *top {
            break;
        }
        if let Some(nn) = unsafe { cur.data::<u32>(POS_KEY) } {
            return Some(*unsafe { nn.as_ref() });
        }
        w = cur.parent();
    }
    None
}

// ---------------------------------------------------------------------
// Column construction.
// ---------------------------------------------------------------------

#[allow(clippy::too_many_arguments)]
fn make_column<B, C>(
    cv: &gtk::ColumnView,
    title: &str,
    xalign: f32,
    fixed: i32,
    expand: bool,
    bind_fn: B,
    cmp_fn: C,
) -> gtk::ColumnViewColumn
where
    B: Fn(&HxTrackerRow) -> String + 'static,
    C: Fn(&HxTrackerRow, &HxTrackerRow) -> gtk::Ordering + 'static,
{
    let factory = gtk::SignalListItemFactory::new();
    factory.connect_setup(move |_, obj| {
        let Some(item) = obj.downcast_ref::<gtk::ListItem>() else {
            return;
        };
        let lbl = gtk::Label::new(None);
        lbl.set_xalign(xalign);
        lbl.set_ellipsize(pango::EllipsizeMode::End);
        lbl.set_margin_start(6);
        lbl.set_margin_end(6);
        item.set_child(Some(&lbl));
    });
    factory.connect_bind(move |_, obj| {
        let Some(item) = obj.downcast_ref::<gtk::ListItem>() else {
            return;
        };
        let Some(lbl) = item.child().and_downcast::<gtk::Label>() else {
            return;
        };
        let text = match item.item().and_downcast::<HxTrackerRow>() {
            Some(r) => bind_fn(&r),
            None => String::new(),
        };
        lbl.set_text(&text);
        stash_pos(lbl.upcast_ref::<gtk::Widget>(), item.position());
    });

    let col = gtk::ColumnViewColumn::new(Some(title), Some(factory));
    if fixed > 0 {
        col.set_fixed_width(fixed);
    }
    col.set_expand(expand);
    col.set_resizable(true);

    let sorter = gtk::CustomSorter::new(move |a, b| {
        match (
            a.downcast_ref::<HxTrackerRow>(),
            b.downcast_ref::<HxTrackerRow>(),
        ) {
            (Some(x), Some(y)) => cmp_fn(x, y),
            _ => gtk::Ordering::Equal,
        }
    });
    col.set_sorter(Some(&sorter));
    cv.append_column(&col);
    col
}

// ---------------------------------------------------------------------
// Section construction.
// ---------------------------------------------------------------------

fn section_new(url: &str, version: u8, expected: u16, filter: &gtk::CustomFilter) -> Rc<Section> {
    crate::ensure_gtk_init();
    // Model chain: GListStore → GtkFilterListModel (shared search filter)
    // → GtkSortListModel (column-header sorter) → GtkSingleSelection.
    let store = gio::ListStore::new::<HxTrackerRow>();
    let filter_model = gtk::FilterListModel::new(Some(store.clone()), Some(filter.clone()));
    filter_model.set_incremental(false);
    let sort_model = gtk::SortListModel::new(Some(filter_model.clone()), None::<gtk::Sorter>);
    sort_model.set_incremental(false);
    let selection = gtk::SingleSelection::new(Some(sort_model.clone()));
    selection.set_autoselect(false);
    selection.set_can_unselect(true);
    selection.set_selected(INVALID);

    let column_view = gtk::ColumnView::new(Some(selection.clone()));
    unsafe { cffi::gtkhx_apply_listview_style(wptr(&column_view)) };
    column_view.set_show_column_separators(true);
    column_view.set_show_row_separators(false);

    let columns: Vec<gtk::ColumnViewColumn> = vec![
        make_column(
            &column_view,
            &tr("Name"),
            0.0,
            200,
            true,
            |r| r.name(),
            |a, b| coll(&a.name(), &b.name()),
        ),
        make_column(
            &column_view,
            &tr("Users"),
            0.5,
            76,
            false,
            |r| r.nusers().to_string(),
            |a, b| a.nusers().cmp(&b.nusers()).into(),
        ),
        make_column(
            &column_view,
            &tr("Country"),
            0.5,
            60,
            false,
            meta_country,
            |a, b| coll(&meta_country(a), &meta_country(b)),
        ),
        make_column(
            &column_view,
            &tr("Address"),
            0.0,
            150,
            false,
            |r| r.address(),
            |a, b| coll(&a.address(), &b.address()),
        ),
        make_column(
            &column_view,
            &tr("Port"),
            0.5,
            70,
            false,
            |r| r.port().to_string(),
            |a, b| a.port().cmp(&b.port()).into(),
        ),
        make_column(
            &column_view,
            &tr("Caps"),
            0.0,
            110,
            false,
            caps_of,
            |a, b| coll(&caps_of(a), &caps_of(b)),
        ),
        make_column(
            &column_view,
            &tr("Description"),
            0.0,
            280,
            true,
            |r| r.desc(),
            |a, b| coll(&a.desc(), &b.desc()),
        ),
    ];
    let col_country = columns[2].clone();
    let col_caps = columns[5].clone();

    if let Some(sorter) = column_view.sorter() {
        sort_model.set_sorter(Some(&sorter));
    }

    if version == 1 {
        col_country.set_visible(false);
        col_caps.set_visible(false);
    }

    // Vertical = NEVER + propagate-natural-height: the section sizes to
    // its full content height (all rows visible, no inner vertical scroll)
    // and the outer sections scroller does the window's vertical scrolling.
    // Horizontal = AUTOMATIC for column overflow. Giving each section its
    // own inner vertical scrollbar made column-sort clicks jump the inner
    // scroll; this matches the C tracker and keeps the outer scroll stable.
    let scroll = gtk::ScrolledWindow::new();
    scroll.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Never);
    scroll.set_has_frame(true);
    scroll.set_propagate_natural_height(true);
    scroll.set_child(Some(&column_view));

    let expander = gtk::Expander::new(None);
    expander.set_expanded(true);
    expander.set_child(Some(&scroll));

    let sec = Rc::new(Section {
        url: url.to_owned(),
        version: std::cell::Cell::new(version),
        expected: std::cell::Cell::new(expected),
        store,
        filter_model,
        sort_model,
        selection: selection.clone(),
        dedup: RefCell::new(HashSet::new()),
        expander,
        col_country,
        col_caps,
        columns,
    });

    // notify::selected → mirror into the global selection + keep the
    // cross-section single-selection invariant.
    {
        let url = sec.url.clone();
        selection.connect_selected_notify(move |_| on_selection_changed(&url));
    }
    // activate (double-click / Enter) → connect to the row's server.
    {
        let url = sec.url.clone();
        column_view.connect_activate(move |_, pos| on_section_activate(&url, pos));
    }
    // secondary-button press → row context menu.
    {
        let rclick = gtk::GestureClick::new();
        rclick.set_button(gdk::BUTTON_SECONDARY);
        rclick.set_propagation_phase(gtk::PropagationPhase::Capture);
        let url = sec.url.clone();
        rclick.connect_pressed(move |g, _n, x, y| on_secondary_press(&url, g, x, y));
        column_view.add_controller(rclick);
    }

    update_title(&sec);
    sec
}

// ---------------------------------------------------------------------
// Title + counts.
// ---------------------------------------------------------------------

fn section_num_total(sec: &Section) -> u32 {
    sec.store.n_items()
}
fn section_num_found(sec: &Section) -> u32 {
    sec.filter_model.n_items()
}

fn update_title(sec: &Section) {
    let vbuf = format!("v{}", sec.version.get());
    let url_esc = glib::markup_escape_text(&sec.url);
    let n_total = section_num_total(sec);
    let n_found = section_num_found(sec);

    let markup = if n_total == 0 {
        if sec.expected.get() != 0 {
            format!(
                "<b>{}</b>  <span alpha=\"60%\">{} · {} {}</span>",
                url_esc,
                vbuf,
                tr("expecting"),
                sec.expected.get()
            )
        } else {
            format!(
                "<b>{}</b>  <span alpha=\"60%\">{} · {}</span>",
                url_esc,
                vbuf,
                tr("no servers")
            )
        }
    } else if n_found == n_total {
        format!(
            "<b>{}</b>  <span alpha=\"60%\">{} · {} {}</span>",
            url_esc,
            vbuf,
            n_total,
            trn("server", "servers", n_total as u64)
        )
    } else {
        format!(
            "<b>{}</b>  <span alpha=\"60%\">{} · {} / {} {}</span>",
            url_esc,
            vbuf,
            n_found,
            n_total,
            trn("server", "servers", n_total as u64)
        )
    };
    sec.expander.set_use_markup(true);
    sec.expander.set_label(Some(&markup));
}

fn set_count_labels(win: &Win) {
    win.lbl_found
        .set_text(&format!("  {}", win.num_found_total));
    win.lbl_total
        .set_text(&format!(" / {}", win.num_total_total));
}

// ---------------------------------------------------------------------
// Selection handling.
// ---------------------------------------------------------------------

fn on_selection_changed(url: &str) {
    // Short WIN borrow: update the global selection + figure out which
    // other section (if any) to clear. Clearing it is done AFTER the
    // borrow drops, since set_selected re-enters this handler.
    let mut clear: Option<gtk::SingleSelection> = None;
    WIN.with_borrow_mut(|w| {
        let Some(win) = w.as_mut() else { return };
        let Some(sec) = win.by_url.get(url).cloned() else {
            return;
        };
        let pos = sec.selection.selected();
        if pos == INVALID {
            if win.selected.as_ref().map(|(s, _)| s.url == sec.url) == Some(true) {
                win.selected = None;
            }
            return;
        }
        let old = win.selected.take();
        win.selected = Some((sec.clone(), pos));
        if let Some((oldsec, _)) = old {
            if oldsec.url != sec.url {
                clear = Some(oldsec.selection.clone());
            }
        }
    });
    if let Some(sel) = clear {
        sel.set_selected(INVALID);
    }
}

/// Pull the row at `pos` in the section's filtered/selection view.
fn section_row_at(sec: &Section, pos: u32) -> Option<HxTrackerRow> {
    sec.selection.item(pos).and_downcast::<HxTrackerRow>()
}

// ---------------------------------------------------------------------
// Search.
// ---------------------------------------------------------------------

fn rerun_search() {
    // Recompile the regex into SEARCH (short borrow), then re-notify the
    // shared filter. Selection clearing + title refresh happen outside
    // any WIN borrow because set_selected re-enters the selection
    // handler.
    let (filter, entry_text, sections): (gtk::CustomFilter, String, Vec<Rc<Section>>) = match WIN
        .with_borrow(|w| {
            w.as_ref().map(|win| {
                (
                    win.filter.clone(),
                    win.search_entry.text().to_string(),
                    win.order.clone(),
                )
            })
        }) {
        Some(v) => v,
        None => return,
    };

    // Compile.
    {
        let mut compiled: Option<glib::Regex> = None;
        if !entry_text.is_empty() {
            let mut flags = glib::RegexCompileFlags::empty();
            if unsafe { cffi::gtkhx_tracker_pref_case() } == 0 {
                flags |= glib::RegexCompileFlags::CASELESS;
            }
            match glib::Regex::new(&entry_text, flags, glib::RegexMatchFlags::empty()) {
                Ok(Some(re)) => compiled = Some(re),
                Ok(None) => {}
                Err(e) => {
                    let msg = format!("Tracker regex: {}\n", e.message());
                    unsafe { cffi::gtkhx_tracker_log_info(cs(&msg).as_ptr()) };
                }
            }
        }
        SEARCH.with_borrow_mut(|s| *s = compiled);
    }

    // One re-evaluation pass across every section.
    filter.changed(gtk::FilterChange::Different);

    // Clear all selections (outside WIN borrow).
    WIN.with_borrow_mut(|w| {
        if let Some(win) = w.as_mut() {
            win.selected = None;
        }
    });
    for sec in &sections {
        sec.selection.set_selected(INVALID);
    }

    // Roll up found counts + refresh subtitles.
    let mut found = 0i32;
    for sec in &sections {
        update_title(sec);
        found += section_num_found(sec) as i32;
    }
    WIN.with_borrow_mut(|w| {
        if let Some(win) = w.as_mut() {
            win.num_found_total = found;
            win.lbl_found.set_text(&format!("  {}", found));
        }
    });
}

// ---------------------------------------------------------------------
// C ABI entry points.
// ---------------------------------------------------------------------

/// `void tracker_batch_begin(const char *url, guint8 version, guint16
/// expected)` — set up (or recycle) the section that subsequent
/// `tracker_server_create` records land in.
///
/// # Safety
/// `url` must be a valid pointer to a NUL-terminated C string that stays
/// live for the duration of the call. Called from the C tracker bridge.
#[no_mangle]
pub unsafe extern "C" fn tracker_batch_begin(url: *const c_char, version: u8, expected: u16) {
    let url = unsafe { cstr(url) };
    // Recycle path: compute counter deltas + mark current under a short
    // borrow, then do the store/selection mutations OUTSIDE it — both
    // set_selected(INVALID) and remove_all can re-enter the selection
    // handler (which borrows WIN).
    let recycled = WIN.with_borrow_mut(|w| {
        let win = w.as_mut()?;
        let sec = win.by_url.get(&url).cloned()?;
        let old_total = section_num_total(&sec) as i32;
        let old_found = section_num_found(&sec) as i32;
        sec.version.set(version);
        sec.expected.set(expected);
        if win.selected.as_ref().map(|(s, _)| s.url == sec.url) == Some(true) {
            win.selected = None;
        }
        win.num_total_total -= old_total;
        win.num_found_total -= old_found;
        set_count_labels(win);
        win.current = Some(sec.clone());
        Some(sec)
    });

    if let Some(sec) = recycled {
        sec.selection.set_selected(INVALID);
        sec.store.remove_all();
        sec.dedup.borrow_mut().clear();
        sec.col_country.set_visible(version != 1);
        sec.col_caps.set_visible(version != 1);
        update_title(&sec);
        return;
    }

    // Fresh section.
    WIN.with_borrow_mut(|w| {
        let Some(win) = w.as_mut() else { return };
        let sec = section_new(&url, version, expected, &win.filter);
        win.sections_box.append(&sec.expander);
        win.by_url.insert(url.clone(), sec.clone());
        win.order.push(sec.clone());
        win.current = Some(sec);
    });
}

/// `void tracker_server_create(HxTrackerServer *event)` — append a
/// (dedup'd) record to the current section.
///
/// # Safety
/// `event` is NULL or a valid `HxTrackerServer*`.
#[no_mangle]
pub unsafe extern "C" fn tracker_server_create(event: *mut HxTrackerServer) {
    if event.is_null() {
        return;
    }
    let e = &*event;
    let address = cstr(e.address);
    if address.is_empty() {
        return;
    }
    let dedup_key = format!("{}:{}", address, e.port);

    WIN.with_borrow_mut(|w| {
        let Some(win) = w.as_mut() else { return };
        let Some(sec) = win.current.clone() else {
            return;
        };
        if !sec.dedup.borrow_mut().insert(dedup_key) {
            return; // already present
        }
        let old_found = section_num_found(&sec) as i32;
        let row = HxTrackerRow::from_event(event);
        sec.store.append(&row);
        let new_found = section_num_found(&sec) as i32;

        win.num_total_total += 1;
        if new_found != old_found {
            win.num_found_total += new_found - old_found;
        }
        update_title(&sec);
        win.lbl_total
            .set_text(&format!(" / {}", win.num_total_total));
        if new_found != old_found {
            win.lbl_found
                .set_text(&format!("  {}", win.num_found_total));
        }
    });
}

/// `void tracker_clear(void)` — drop every section + reset counts.
#[no_mangle]
pub extern "C" fn tracker_clear() {
    // Detach the sections under a short borrow, then unparent the
    // expanders outside it (the widget teardown can re-enter handlers).
    let detached = WIN.with_borrow_mut(|w| {
        let win = w.as_mut()?;
        let order = std::mem::take(&mut win.order);
        win.by_url.clear();
        win.current = None;
        win.selected = None;
        win.num_found_total = 0;
        win.num_total_total = 0;
        Some((win.sections_box.clone(), order))
    });
    if let Some((box_, order)) = detached {
        for sec in order {
            box_.remove(&sec.expander);
        }
    }
}

/// `void tracker_search_refresh(void)` — re-run the filter (called from
/// options.c when the Settings case toggle flips).
#[no_mangle]
pub extern "C" fn tracker_search_refresh() {
    rerun_search();
}

/// `void create_tracker_window(GtkWidget *widget, gpointer data)` —
/// `data` is a `session *`.
#[no_mangle]
pub extern "C" fn create_tracker_window(_widget: *mut cffi::GtkWidget, data: *mut c_void) {
    let already_open = WIN.with_borrow(|w| w.is_some());
    if already_open {
        return;
    }
    build_window(data);
}

// ---------------------------------------------------------------------
// Window construction.
// ---------------------------------------------------------------------

fn build_window(sess: *mut c_void) {
    crate::ensure_gtk_init();
    let window = gtk::Window::new();
    window.set_title(Some(&tr("Tracker")));
    window.set_default_size(860, 500);
    window.connect_close_request(|_| {
        on_close();
        glib::Propagation::Proceed
    });

    // Register with the application so dialogs parent correctly.
    let app_ptr = unsafe { cffi::gtkhx_get_application() };
    if !app_ptr.is_null() {
        let app: gio::Application = unsafe { glib::translate::from_glib_none(app_ptr) };
        if let Ok(gtkapp) = app.downcast::<gtk::Application>() {
            gtkapp.add_window(&window);
        }
    }

    let filter = gtk::CustomFilter::new(row_matches);

    // Search row.
    let search_entry = gtk::SearchEntry::new();
    search_entry.set_placeholder_text(Some(&tr("Search trackers")));
    search_entry.connect_search_changed(|_| rerun_search());
    search_entry.connect_activate(|_| rerun_search());

    let case_btn = gtk::ToggleButton::with_label("Aa");
    case_btn.set_tooltip_text(Some(&tr("Match case in tracker search")));
    case_btn.add_css_class("flat");
    case_btn.set_active(unsafe { cffi::gtkhx_tracker_pref_case() } != 0);
    case_btn.connect_toggled(|b| {
        let active = if b.is_active() { 1 } else { 0 };
        // Route through the generic prefs setter (keeps the Settings
        // switch in lockstep); CFG_TRACKER_CASE = "TRACKER_CASE".
        unsafe { cffi::gtkhx_prefs_set_bool(cs("TRACKER_CASE").as_ptr(), active) };
        rerun_search();
    });

    let lbl_found = gtk::Label::new(Some("0"));
    let lbl_total = gtk::Label::new(Some(" / 0"));
    lbl_found.add_css_class("dim-label");
    lbl_total.add_css_class("dim-label");

    // Header bar action buttons.
    let refreshbtn = pixmap_button(
        "/com/nasledov/gtkhx/pixmaps/refresh.png",
        &tr("Refresh tracker list"),
    );
    refreshbtn.connect_clicked(|_| tracker_getlist());
    let connbtn = pixmap_button(
        "/com/nasledov/gtkhx/pixmaps/connect.png",
        &tr("Connect to selected server"),
    );
    connbtn.connect_clicked(|_| tracker_connect());
    let bookmarkbtn = gtk::Button::from_icon_name("bookmark-new-symbolic");
    bookmarkbtn.set_tooltip_text(Some(&tr("Save selected server as a bookmark")));
    bookmarkbtn.connect_clicked(|_| tracker_save_bookmark());
    let detailsbtn = pixmap_button(
        "/com/nasledov/gtkhx/pixmaps/info.png",
        &tr("Show details for selected server"),
    );
    detailsbtn.connect_clicked(|_| tracker_details());

    let header = adw::HeaderBar::new();
    header.pack_start(&refreshbtn);
    header.pack_start(&connbtn);
    header.pack_start(&bookmarkbtn);
    header.pack_start(&detailsbtn);

    let count_box = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    count_box.append(&lbl_found);
    count_box.append(&lbl_total);
    header.pack_end(&count_box);
    window.set_titlebar(Some(&header));

    // Sections stack.
    let sections_box = gtk::Box::new(gtk::Orientation::Vertical, 8);
    let sections_scroll = gtk::ScrolledWindow::new();
    sections_scroll.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Automatic);
    sections_scroll.set_has_frame(false);
    sections_scroll.set_vexpand(true);
    sections_scroll.set_child(Some(&sections_box));

    let vbox = gtk::Box::new(gtk::Orientation::Vertical, 8);
    vbox.set_margin_start(8);
    vbox.set_margin_end(8);
    vbox.set_margin_top(8);
    vbox.set_margin_bottom(8);

    let searchhbox = gtk::Box::new(gtk::Orientation::Horizontal, 6);
    search_entry.set_hexpand(true);
    searchhbox.append(&search_entry);
    searchhbox.append(&case_btn);
    vbox.append(&searchhbox);
    vbox.append(&sections_scroll);

    window.set_child(Some(&vbox));
    unsafe { cffi::init_keyaccel(wptr(&window)) };

    WIN.with_borrow_mut(|w| {
        *w = Some(Win {
            window: window.clone(),
            sections_box,
            search_entry: search_entry.clone(),
            lbl_found,
            lbl_total,
            filter,
            order: Vec::new(),
            by_url: HashMap::new(),
            current: None,
            selected: None,
            num_found_total: 0,
            num_total_total: 0,
            sess,
        });
    });
    SEARCH.with_borrow_mut(|s| *s = None);

    window.present();
    search_entry.grab_focus();

    tracker_getlist();
}

fn on_close() {
    tracker_clear();
    WIN.with_borrow_mut(|w| *w = None);
    SEARCH.with_borrow_mut(|s| *s = None);
}

fn pixmap_button(resource: &str, tooltip: &str) -> gtk::Button {
    let ptr = unsafe {
        cffi::gtkhx_pixmap_button(
            cs(resource).as_ptr(),
            cs(tooltip).as_ptr(),
            cffi::GTKHX_SCALE_WINDOW_BUTTONS,
            std::ptr::null(),
            std::ptr::null_mut(),
        )
    };
    // gtkhx_pixmap_button returns a *floating* GtkButton (gtk_button_new,
    // never sunk). `from_glib_none` is correct here — for objects it does
    // g_object_ref_sink ("takes ownership of floating references", per
    // glib's own impl), so the wrapper owns a single ref with the floating
    // flag cleared; pack_start then adds the header's ref and the wrapper
    // drop leaves that. NOT from_glib_full: that skips the sink, so the
    // wrapper drop would free the button out from under the header bar.
    let w: gtk::Widget = unsafe { glib::translate::from_glib_none(ptr) };
    w.downcast::<gtk::Button>()
        .expect("gtkhx_pixmap_button returns a GtkButton")
}

fn tracker_getlist() {
    let sess = WIN.with_borrow(|w| w.as_ref().map(|win| win.sess));
    unsafe { ffi::tracker_kill_threads() };
    tracker_clear();
    WIN.with_borrow(|w| {
        if let Some(win) = w.as_ref() {
            win.lbl_found.set_text("  0");
            win.lbl_total.set_text(" / 0");
        }
    });
    if let Some(sess) = sess {
        unsafe { ffi::hx_tracker_list_async(sess) };
    }
}

// actions (connect / bookmark / details / context menu) live here too.
mod actions;
use actions::{
    on_secondary_press, on_section_activate, tracker_connect, tracker_details,
    tracker_save_bookmark,
};
