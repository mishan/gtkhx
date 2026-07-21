//! Pure files-subsystem model helpers.
//!
//! Phase F1 of the files C→Rust migration (see
//! docs/files-rust-migration-scope.md). This first increment is the
//! file-type → icon-id mapping, ported from
//! `src/files.c::icon_of_ftype_and_name`. Pure logic, no glib/gtk, so it
//! is fully unit-tested here and the C side calls in through the
//! `#[no_mangle]` surface in `ffi.rs`.

mod ffi;

/// cicn resource ids for the bundled file-type icons. These MUST stay in
/// lockstep with the `ICON_*` #defines in `src/files.h`.
pub mod icon {
    pub const FILE: u16 = 400;
    pub const FOLDER: u16 = 401;
    pub const FOLDER_IN: u16 = 421; // drop box / upload folder
    pub const HTFT: u16 = 402;
    pub const SIT: u16 = 403;
    pub const TEXT: u16 = 404;
    pub const IMAGE: u16 = 406;
    pub const APPL: u16 = 407;
    pub const ALIS: u16 = 422;
    pub const DISK: u16 = 423;
    pub const NOTE: u16 = 424; // audio
    pub const MOOV: u16 = 425; // movie
    pub const ZIP: u16 = 426;
}

/// Pick a file-list icon id from a 4-byte Hotline file type (FourCC) and
/// the entry name.
///
/// Mirrors `icon_of_ftype_and_name` in `src/files.c` byte-for-byte:
/// - `None`/short `ftype` → generic FILE.
/// - `fldr` whose name contains "DROP BOX" or "UPLOAD" (ASCII
///   case-insensitive) → the upload-folder icon; otherwise a plain folder.
/// - a small table of well-known FourCCs; `SIT` matches on the first
///   three bytes (SIT!, SITD, SIT5, …); everything else → generic FILE.
///
/// Order matters and matches the C: the first branch that fits wins.
pub fn icon_id_for(ftype: Option<&[u8]>, name: Option<&[u8]>) -> u16 {
    let ftype = match ftype {
        Some(f) if f.len() >= 4 => f,
        _ => return icon::FILE,
    };
    let f4 = &ftype[..4];

    if f4 == b"fldr" {
        if let Some(n) = name {
            if contains_ascii_ci(n, b"DROP BOX") || contains_ascii_ci(n, b"UPLOAD") {
                return icon::FOLDER_IN;
            }
        }
        return icon::FOLDER;
    }
    if matches!(f4, b"JPEG" | b"PNGf" | b"GIFf" | b"PICT") {
        return icon::IMAGE;
    }
    if matches!(f4, b"MPEG" | b"MPG " | b"AVI " | b"MooV") {
        return icon::MOOV;
    }
    if f4 == b"MP3 " {
        return icon::NOTE;
    }
    if f4 == b"ZIP " {
        return icon::ZIP;
    }
    if &ftype[..3] == b"SIT" {
        return icon::SIT;
    }
    if f4 == b"APPL" {
        return icon::APPL;
    }
    if f4 == b"rohd" {
        return icon::DISK;
    }
    if f4 == b"HTft" {
        return icon::HTFT;
    }
    if f4 == b"alis" {
        return icon::ALIS;
    }
    if f4 == b"TEXT" {
        return icon::TEXT;
    }
    icon::FILE
}

/// ASCII case-insensitive substring test over a byte range, matching the
/// C `strcasestr_len(name, needle, name_len)` used for the drop-box check.
fn contains_ascii_ci(haystack: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() {
        return true;
    }
    if haystack.len() < needle.len() {
        return false;
    }
    haystack
        .windows(needle.len())
        .any(|w| w.eq_ignore_ascii_case(needle))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn icon(ftype: &[u8], name: &[u8]) -> u16 {
        icon_id_for(Some(ftype), Some(name))
    }

    #[test]
    fn null_and_short_ftype_are_generic_file() {
        assert_eq!(icon_id_for(None, None), icon::FILE);
        assert_eq!(icon_id_for(Some(b"SI"), None), icon::FILE); // < 4 bytes
        assert_eq!(icon_id_for(Some(b""), Some(b"x")), icon::FILE);
    }

    #[test]
    fn plain_folder() {
        assert_eq!(icon(b"fldr", b"My Stuff"), icon::FOLDER);
        assert_eq!(icon_id_for(Some(b"fldr"), None), icon::FOLDER);
    }

    #[test]
    fn upload_and_drop_box_folders_case_insensitive() {
        assert_eq!(icon(b"fldr", b"Upload"), icon::FOLDER_IN);
        assert_eq!(icon(b"fldr", b"uploads here"), icon::FOLDER_IN);
        assert_eq!(icon(b"fldr", b"Drop Box"), icon::FOLDER_IN);
        assert_eq!(icon(b"fldr", b"THE DROP BOX"), icon::FOLDER_IN);
        // substring, not prefix
        assert_eq!(icon(b"fldr", b"members upload folder"), icon::FOLDER_IN);
        // no match -> plain folder
        assert_eq!(icon(b"fldr", b"Documents"), icon::FOLDER);
    }

    #[test]
    fn images() {
        for t in [b"JPEG", b"PNGf", b"GIFf", b"PICT"] {
            assert_eq!(icon(t, b""), icon::IMAGE, "{t:?}");
        }
    }

    #[test]
    fn movies_and_audio() {
        for t in [b"MPEG", b"MPG ", b"AVI ", b"MooV"] {
            assert_eq!(icon(t, b""), icon::MOOV, "{t:?}");
        }
        assert_eq!(icon(b"MP3 ", b""), icon::NOTE);
    }

    #[test]
    fn archives_and_stuffit_prefix() {
        assert_eq!(icon(b"ZIP ", b""), icon::ZIP);
        // SIT matches on the 3-byte prefix
        assert_eq!(icon(b"SIT!", b""), icon::SIT);
        assert_eq!(icon(b"SITD", b""), icon::SIT);
        assert_eq!(icon(b"SIT5", b""), icon::SIT);
    }

    #[test]
    fn misc_types() {
        assert_eq!(icon(b"APPL", b""), icon::APPL);
        assert_eq!(icon(b"rohd", b""), icon::DISK);
        assert_eq!(icon(b"HTft", b""), icon::HTFT);
        assert_eq!(icon(b"alis", b""), icon::ALIS);
        assert_eq!(icon(b"TEXT", b""), icon::TEXT);
    }

    #[test]
    fn unknown_type_is_generic_file() {
        assert_eq!(icon(b"XXXX", b"whatever"), icon::FILE);
        assert_eq!(icon(b"PDF ", b"doc"), icon::FILE); // not in the icon table
    }

    #[test]
    fn ftype_longer_than_four_bytes_uses_first_four() {
        // Callers pass exactly 4, but be robust: only the first 4 count.
        assert_eq!(icon_id_for(Some(b"JPEGextra"), Some(b"")), icon::IMAGE);
    }
}
