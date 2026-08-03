//! The crate's display-backed tests, all of them, in one `#[test]`.
//!
//! **Why one.** GTK 4 asserts main-thread affinity inside every widget
//! constructor, and cargo runs each `#[test]` on its own thread. Whichever
//! test reaches `gtk::init()` first claims GTK for its thread; a second test
//! then fails twice over — `gtk::init()` returns `Err` from the wrong thread,
//! and any widget call it makes anyway aborts with "GTK may only be used from
//! the main thread". A mutex does not help, because the problem is thread
//! identity rather than concurrency.
//!
//! So the crate gets exactly one test that owns the GTK thread, and everything
//! needing a display hangs off it as a plain function. New display-backed
//! checks go here as another call, not as another `#[test]`.
//!
//! **A display is required, never optional.** GTK 4 has no headless backend,
//! so CI runs the suite under `xvfb-run` with `GDK_BACKEND=x11`. A missing
//! display fails; it must never skip, because a skip is indistinguishable
//! from a pass in a CI log.

use gtk4 as gtk;
use libadwaita as adw;

#[test]
fn display_backed() {
    assert!(
        gtk::init().is_ok(),
        "no display: run under xvfb-run with GDK_BACKEND=x11"
    );
    adw::init().expect("libadwaita init");

    crate::options_window::tests::check_every_page_builds();
    crate::conn_tabs::tests::check_strip_indexes_connections();
}
