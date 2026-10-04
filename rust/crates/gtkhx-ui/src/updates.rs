//! The update notice. Whether a build may look for updates at all is decided
//! when it is configured (`-Dupdate_check`, see docs/updates.md); the user's
//! own switch is `updates.check` in the settings.

use gtk4 as gtk;
use libadwaita as adw;

/// `-Dupdate_check`, passed in by rust/meson.build. Off for a bare
/// `cargo build`, like a distribution's package.
pub(crate) const BUILD_ENABLED: bool = match option_env!("GTKHX_UPDATE_CHECK") {
    Some(v) => matches!(v.as_bytes(), [b'1']),
    None => false,
};

/// Nothing asks yet, and a switch for a check that never runs would mislead,
/// so Settings shows the switch only once something does.
pub(crate) const CHECKS_WIRED: bool = false;

/// Inside the Flatpak the sandbox's own update machinery is asked, not the
/// feed on dl.gtkhx.org.
pub(crate) fn in_flatpak() -> bool {
    std::path::Path::new("/.flatpak-info").exists()
}

/// The main window's update banner. Hidden until a check finds something.
pub(crate) fn banner() -> gtk::Widget {
    let banner = adw::Banner::new("");
    banner.set_revealed(false);
    banner.into()
}
