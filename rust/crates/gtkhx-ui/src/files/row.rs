//! What a listing row shows: its size and date text, its icon, and the
//! column sort order.

use std::cmp::Ordering;

use gtk::glib;
use gtk4 as gtk;
use hxmodel::files_entry::HxFileEntry;

use crate::tr::trn_argv;

// Mac cicn IDs (`files.h`).
pub const ICON_FILE: u16 = 400;
pub const ICON_FOLDER: u16 = 401;
const ICON_FILE_HTFT: u16 = 402;
const ICON_FILE_SIT: u16 = 403;
const ICON_FILE_TEXT: u16 = 404;
const ICON_FILE_IMAGE: u16 = 406;
const ICON_FILE_APPL: u16 = 407;
const ICON_FILE_SITP: u16 = 409;
const ICON_FOLDER_IN: u16 = 421;
const ICON_FILE_ALIS: u16 = 422;
const ICON_FILE_DISK: u16 = 423;
const ICON_FILE_NOTE: u16 = 424;
const ICON_FILE_MOOV: u16 = 425;
const ICON_FILE_ZIP: u16 = 426;

/// The pixmap resource for an icon ID, or `None` for an ID without one of its
/// own, which shows as a plain file.
pub fn icon_resource(icon_id: u16) -> Option<&'static str> {
    Some(match icon_id {
        ICON_FOLDER => "/com/nasledov/gtkhx/pixmaps/folder.png",
        ICON_FOLDER_IN => "/com/nasledov/gtkhx/pixmaps/folder_dropbox.png",
        ICON_FILE => "/com/nasledov/gtkhx/pixmaps/file.png",
        ICON_FILE_HTFT => "/com/nasledov/gtkhx/pixmaps/file_html.png",
        ICON_FILE_SIT | ICON_FILE_SITP => "/com/nasledov/gtkhx/pixmaps/file_sit.png",
        ICON_FILE_IMAGE => "/com/nasledov/gtkhx/pixmaps/file_image.png",
        ICON_FILE_APPL => "/com/nasledov/gtkhx/pixmaps/file_app.png",
        ICON_FILE_ALIS => "/com/nasledov/gtkhx/pixmaps/file_alias.png",
        ICON_FILE_DISK => "/com/nasledov/gtkhx/pixmaps/file_disk.png",
        ICON_FILE_NOTE => "/com/nasledov/gtkhx/pixmaps/file_note.png",
        ICON_FILE_MOOV => "/com/nasledov/gtkhx/pixmaps/file_movie.png",
        ICON_FILE_TEXT => "/com/nasledov/gtkhx/pixmaps/file_text.png",
        ICON_FILE_ZIP => "/com/nasledov/gtkhx/pixmaps/file_zip.png",
        _ => return None,
    })
}

/// The Size column. A folder's size is its child count when the server sent
/// one (Hotline carries it there), and an em dash otherwise — a local folder's
/// byte size would be a meaningless 4096. Files are rounded; the exact count
/// is the cell's tooltip ([`exact_size`]).
pub fn size_text(is_dir: bool, size: u64) -> String {
    if is_dir {
        if size == 0 {
            return "—".into();
        }
        // TRANSLATORS: How many files a folder holds, shown in the Size
        // column of the file browser. %s is the count.
        return trn_argv("(%s item)", "(%s items)", size, &[&size.to_string()]);
    }
    glib::format_size_full(size, glib::FormatSizeFlags::IEC_UNITS).into()
}

/// A file's exact size, for the Size cell's tooltip.
pub fn exact_size(size: u64) -> String {
    glib::format_size_full(
        size,
        glib::FormatSizeFlags::IEC_UNITS | glib::FormatSizeFlags::LONG_FORMAT,
    )
    .into()
}

/// The Modified column: the time within the last day, the day within this
/// year, the full date before that, and nothing when unknown.
pub fn modified_text(modified: i64) -> String {
    modified_text_at(modified, glib::DateTime::now_local().ok())
}

fn modified_text_at(modified: i64, now: Option<glib::DateTime>) -> String {
    if modified <= 0 {
        return String::new();
    }
    let (Ok(dt), Some(now)) = (glib::DateTime::from_unix_local(modified), now) else {
        return String::new();
    };
    let delta = now.difference(&dt).as_microseconds();
    let format = if (0..glib::ffi::G_TIME_SPAN_DAY).contains(&delta) {
        "%H:%M"
    } else if dt.year() == now.year() {
        "%b %e"
    } else {
        "%Y-%m-%d"
    };
    dt.format(format).map(String::from).unwrap_or_default()
}

fn folders_first(a: &HxFileEntry, b: &HxFileEntry) -> Ordering {
    b.is_dir().cmp(&a.is_dir())
}

fn collate(a: &std::ffi::CStr, b: &std::ffi::CStr) -> Ordering {
    unsafe { glib::ffi::g_utf8_collate(a.as_ptr(), b.as_ptr()) }.cmp(&0)
}

// Every column puts folders first, as orthodox file managers do, and because a
// folder's size is a count: a 7-item folder ranked against a 7-byte file means
// nothing.

pub fn cmp_name(a: &HxFileEntry, b: &HxFileEntry) -> Ordering {
    folders_first(a, b).then_with(|| collate(&a.name_c(), &b.name_c()))
}

pub fn cmp_size(a: &HxFileEntry, b: &HxFileEntry) -> Ordering {
    folders_first(a, b).then_with(|| a.size().cmp(&b.size()))
}

pub fn cmp_modified(a: &HxFileEntry, b: &HxFileEntry) -> Ordering {
    folders_first(a, b).then_with(|| a.modified().cmp(&b.modified()))
}

pub fn cmp_kind(a: &HxFileEntry, b: &HxFileEntry) -> Ordering {
    folders_first(a, b).then_with(|| collate(&a.kind_c(), &b.kind_c()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn folder_sizes_are_counts() {
        assert_eq!(size_text(true, 0), "—");
        assert_eq!(size_text(true, 1), "(1 item)");
        assert_eq!(size_text(true, 7), "(7 items)");
    }

    #[test]
    fn file_sizes_round() {
        assert_eq!(size_text(false, 0), "0 bytes");
        // GLib puts a no-break space before the unit.
        assert_eq!(size_text(false, 2048).replace('\u{a0}', " "), "2.0 KiB");
        // The exact count groups its digits under a locale that does —
        // "2,048" in en_US — and the display-backed test's gtk::init()
        // switches to the environment's locale, from its own thread, while
        // this test runs. Compare the digits alone.
        let digits: String = exact_size(2048)
            .chars()
            .filter(char::is_ascii_digit)
            .collect();
        assert!(digits.ends_with("2048"), "{}", exact_size(2048));
    }

    #[test]
    fn modified_by_age() {
        let now = glib::DateTime::from_local(2026, 9, 26, 12, 0, 0.0).unwrap();
        let at = |y, mo, d, h, mi| {
            glib::DateTime::from_local(y, mo, d, h, mi, 0.0)
                .unwrap()
                .to_unix()
        };
        assert_eq!(modified_text_at(0, Some(now.clone())), "");
        assert_eq!(
            modified_text_at(at(2026, 9, 26, 9, 5), Some(now.clone())),
            "09:05"
        );
        let march = at(2026, 3, 5, 9, 5);
        let want = glib::DateTime::from_unix_local(march)
            .unwrap()
            .format("%b %e")
            .unwrap();
        assert_eq!(modified_text_at(march, Some(now.clone())), want.as_str());
        assert_eq!(
            modified_text_at(at(2024, 8, 12, 9, 5), Some(now)),
            "2024-08-12"
        );
    }

    #[test]
    fn unknown_icons_have_no_resource() {
        assert!(icon_resource(ICON_FOLDER).is_some());
        assert!(icon_resource(ICON_FILE).is_some());
        assert!(icon_resource(9999).is_none());
    }
}
