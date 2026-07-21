//! Extension → Mac type/creator fallback table (ported from
//! `suffix_type_creator` in `hfs.c`).
//!
//! When a file has no Finder-info sidecar, its Mac type + creator are guessed
//! from the filename extension. Unknown extensions get the generic `TEXTR*ch`.

/// Generic type+creator for files with no recognized extension.
const UNKNOWN: [u8; 8] = *b"TEXTR*ch";

/// The bytes after the last `.` in `path`, or `None` if there is no `.`.
fn suffix(path: &[u8]) -> Option<&[u8]> {
    path.iter().rposition(|&b| b == b'.').map(|i| &path[i + 1..])
}

/// The 8-byte type+creator derived from `path`'s extension. The match is
/// byte-exact and case-sensitive, exactly as the C `strcmp` chain.
pub fn suffix_type_creator(path: &[u8]) -> [u8; 8] {
    let Some(s) = suffix(path) else {
        return UNKNOWN;
    };
    let tc: &[u8; 8] = match s {
        b"jpg" | b"jpeg" => b"JPEGGKON",
        b"png" => b"PNGfGKON",
        b"gif" => b"GIFfGKON",
        b"mp3" | b"mp2" => b"MP3 MAmp",
        b"mpg" | b"mpeg" => b"MPEGTVOD",
        b"mov" => b"MooVTVOD",
        b"sit" => b"SITDSIT!",
        b"zip" | b"pk3" => b"ZIP ZIP ",
        b"app" | b"sea" => b"APPLpeff",
        b"img" => b"rohdWrap",
        b"pict" => b"PICTGKON",
        _ => &UNKNOWN,
    };
    *tc
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_extensions() {
        assert_eq!(&suffix_type_creator(b"photo.jpg"), b"JPEGGKON");
        assert_eq!(&suffix_type_creator(b"photo.jpeg"), b"JPEGGKON");
        assert_eq!(&suffix_type_creator(b"a.png"), b"PNGfGKON");
        assert_eq!(&suffix_type_creator(b"a.gif"), b"GIFfGKON");
        assert_eq!(&suffix_type_creator(b"song.mp3"), b"MP3 MAmp");
        assert_eq!(&suffix_type_creator(b"song.mp2"), b"MP3 MAmp");
        assert_eq!(&suffix_type_creator(b"clip.mpg"), b"MPEGTVOD");
        assert_eq!(&suffix_type_creator(b"clip.mpeg"), b"MPEGTVOD");
        assert_eq!(&suffix_type_creator(b"clip.mov"), b"MooVTVOD");
        assert_eq!(&suffix_type_creator(b"arc.sit"), b"SITDSIT!");
        assert_eq!(&suffix_type_creator(b"arc.zip"), b"ZIP ZIP ");
        assert_eq!(&suffix_type_creator(b"q3.pk3"), b"ZIP ZIP ");
        assert_eq!(&suffix_type_creator(b"prog.app"), b"APPLpeff");
        assert_eq!(&suffix_type_creator(b"prog.sea"), b"APPLpeff");
        assert_eq!(&suffix_type_creator(b"disk.img"), b"rohdWrap");
        assert_eq!(&suffix_type_creator(b"art.pict"), b"PICTGKON");
    }

    #[test]
    fn unknown_and_edge_cases() {
        assert_eq!(suffix_type_creator(b"README"), UNKNOWN); // no dot
        assert_eq!(suffix_type_creator(b"file.xyz"), UNKNOWN); // unknown ext
        assert_eq!(suffix_type_creator(b"trailing."), UNKNOWN); // empty suffix
        assert_eq!(suffix_type_creator(b"JPG.JPG"), UNKNOWN); // case-sensitive
        // Uses the *last* dot, and matches a leading-dot name.
        assert_eq!(&suffix_type_creator(b"a.tar.gif"), b"GIFfGKON");
        assert_eq!(&suffix_type_creator(b".png"), b"PNGfGKON");
    }
}
