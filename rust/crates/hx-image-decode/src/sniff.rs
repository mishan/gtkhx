//! Magic-byte image-format sniff.
//!
//! Pure logic over byte slices: no GLib, no GTK, no allocations,
//! bounded scan window. Mirrors the contract of the legacy
//! `inline_media_sniff` in `src/inline_media_decode.c` exactly — the
//! C unit-test suite in `tests/unit/test_inline_media_decode.c`
//! exercises the same input/output pairs against the FFI wrapper
//! and stays green across the C → Rust migration.
//!
//! Window: at most the first 32 bytes are inspected, regardless of
//! input length. This is what makes the "bounded sniff" promise the
//! header file documents — without it the SVG check could walk an
//! arbitrarily long leading-whitespace run on a malformed payload.
//!
//! Format coverage:
//! - **Allowlist** (the inline-media spec's "Supported Formats"):
//!   JPEG, PNG, GIF.
//! - **Blocklist** (recognised + rejected so the log line is honest):
//!   SVG, WebP, AVIF, HEIC, TIFF, ICO, BMP.
//! - **Unknown**: catch-all for everything else.

/// Detected image format. The blocklist variants are recognised
/// specifically so the rejection log line names the format. The
/// [`format_is_allowed`] gate is what controls accept-vs-reject.
///
/// `#[repr(u32)]` matches the C `HxInlineMediaFormat` enum width (C
/// enums default to int on every Linux ABI we ship for, which is 32
/// bits). The Rust → C cast through the FFI shim preserves the
/// numeric value.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Format {
    Unknown = 0,
    // Allowlisted.
    Jpeg = 1,
    Png = 2,
    Gif = 3,
    // Recognised and rejected (per spec).
    Svg = 4,
    Webp = 5,
    Avif = 6,
    Heic = 7,
    Tiff = 8,
    Ico = 9,
    Bmp = 10,
}

/// Inline `memcmp`-style prefix match. `needle.len() == 0` returns
/// `true` vacuously — but no caller of [`sniff`] passes an empty
/// needle, so this is a write-once helper.
#[inline]
fn prefix_matches(buf: &[u8], needle: &[u8]) -> bool {
    buf.len() >= needle.len() && &buf[..needle.len()] == needle
}

/// SVG sniff. Skips UTF-8 BOM + ASCII whitespace, then looks for
/// `<?xml` or `<svg`. Conservative — any XML-looking prefix counts
/// as SVG because the bytes are getting rejected either way (SVG is
/// not on the allowlist).
fn sniff_svg(buf: &[u8]) -> bool {
    let mut i = 0;
    // Skip BOM.
    if buf.len() >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF {
        i = 3;
    }
    // Skip leading whitespace.
    while i < buf.len()
        && (buf[i] == b' ' || buf[i] == b'\t' || buf[i] == b'\n' || buf[i] == b'\r')
    {
        i += 1;
    }
    let rest = &buf[i..];
    if rest.len() >= 5 && &rest[..5] == b"<?xml" {
        return true;
    }
    if rest.len() >= 4 && &rest[..4] == b"<svg" {
        return true;
    }
    false
}

/// Detect the image format from the leading bytes. See module-level
/// docs for the recognised set; everything else returns
/// [`Format::Unknown`].
///
/// Inspects at most 32 bytes; safe to call with shorter input
/// (returns `Unknown` early when too short).
pub fn sniff(bytes: &[u8]) -> Format {
    if bytes.is_empty() {
        return Format::Unknown;
    }

    // Bound the sniff window. Every magic signature in the allowlist
    // + blocklist below fits in the first 12 bytes; the SVG check
    // additionally scans past leading whitespace. Clamping the slice
    // is what enforces the "bounded hot path" contract — without it
    // sniff_svg could walk an arbitrarily long leading-whitespace
    // run, defeating the O(1)-sniff promise.
    let buf = if bytes.len() > 32 {
        &bytes[..32]
    } else {
        bytes
    };

    // JPEG: SOI marker FF D8 FF (then any APPn / SOI marker). The
    // third FF byte rules out an accidental 0xFFD8 in random data.
    if prefix_matches(buf, &[0xFF, 0xD8, 0xFF]) {
        return Format::Jpeg;
    }

    // PNG: 8-byte signature 89 50 4E 47 0D 0A 1A 0A.
    if prefix_matches(buf, b"\x89PNG\r\n\x1A\n") {
        return Format::Png;
    }

    // GIF: ASCII "GIF87a" or "GIF89a".
    if prefix_matches(buf, b"GIF87a") || prefix_matches(buf, b"GIF89a") {
        return Format::Gif;
    }

    // RIFF...WEBP. RIFF is 4-byte magic at offset 0; WEBP is 4 ASCII
    // bytes at offset 8 (after the RIFF + 4-byte size).
    if buf.len() >= 12 && &buf[..4] == b"RIFF" && &buf[8..12] == b"WEBP" {
        return Format::Webp;
    }

    // ISO BMFF container: ....ftyp<brand> at offset 4. AVIF brands
    // are "avif" / "avis"; HEIC brands cover the common variants
    // ("heic" / "heix" / "hevc" / "hevx" / "heim" / "heis" / "hevm"
    // / "hevs" / "mif1").
    if buf.len() >= 12 && &buf[4..8] == b"ftyp" {
        let brand = &buf[8..12];
        if brand == b"avif" || brand == b"avis" {
            return Format::Avif;
        }
        const HEIC_BRANDS: &[&[u8; 4]] = &[
            b"heic", b"heix", b"hevc", b"hevx", b"heim", b"heis", b"hevm", b"hevs",
            b"mif1",
        ];
        if HEIC_BRANDS.iter().any(|b| brand == b.as_slice()) {
            return Format::Heic;
        }
    }

    // TIFF: 49 49 2A 00 (little-endian) or 4D 4D 00 2A (big-endian).
    if prefix_matches(buf, &[0x49, 0x49, 0x2A, 0x00])
        || prefix_matches(buf, &[0x4D, 0x4D, 0x00, 0x2A])
    {
        return Format::Tiff;
    }

    // ICO: 00 00 01 00 (reserved + image type).
    if prefix_matches(buf, &[0x00, 0x00, 0x01, 0x00]) {
        return Format::Ico;
    }

    // BMP: "BM" at offset 0.
    if prefix_matches(buf, b"BM") {
        return Format::Bmp;
    }

    // SVG check last — it requires scanning past whitespace and is
    // more expensive than the prefix-equality checks above.
    if sniff_svg(buf) {
        return Format::Svg;
    }

    Format::Unknown
}

/// True for the formats that are allowed under the inline-media cap
/// bit (JPEG / PNG / GIF). False for everything else, including
/// the explicitly-blocked formats and `Unknown`. Defence-in-depth:
/// even if the server canonical-MIMEs an image as something we're
/// expected to render, the sniff + this gate run first.
pub fn format_is_allowed(f: Format) -> bool {
    matches!(f, Format::Jpeg | Format::Png | Format::Gif)
}

/// Canonical MIME for the format. Returns a static literal — the C
/// shim passes the pointer through as a borrowed `const char *`,
/// caller doesn't free. `Unknown` returns `None` (mapped to NULL at
/// the FFI boundary).
pub fn format_to_mime(f: Format) -> Option<&'static str> {
    match f {
        Format::Jpeg => Some("image/jpeg"),
        Format::Png => Some("image/png"),
        Format::Gif => Some("image/gif"),
        Format::Svg => Some("image/svg+xml"),
        Format::Webp => Some("image/webp"),
        Format::Avif => Some("image/avif"),
        Format::Heic => Some("image/heic"),
        Format::Tiff => Some("image/tiff"),
        Format::Ico => Some("image/x-icon"),
        Format::Bmp => Some("image/bmp"),
        Format::Unknown => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn jpeg() {
        // SOI + APP0 + length byte. Matches the C test_sniff_jpeg
        // fixture.
        let jpg = [0xFFu8, 0xD8, 0xFF, 0xE0, 0x00];
        assert_eq!(sniff(&jpg), Format::Jpeg);
        assert!(format_is_allowed(Format::Jpeg));
        assert_eq!(format_to_mime(Format::Jpeg), Some("image/jpeg"));
    }

    #[test]
    fn png() {
        let png = [
            0x89u8, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00,
        ];
        assert_eq!(sniff(&png), Format::Png);
        assert!(format_is_allowed(Format::Png));
        assert_eq!(format_to_mime(Format::Png), Some("image/png"));
    }

    #[test]
    fn gif() {
        assert_eq!(sniff(b"GIF87a"), Format::Gif);
        assert_eq!(sniff(b"GIF89a"), Format::Gif);
        assert!(format_is_allowed(Format::Gif));
        assert_eq!(format_to_mime(Format::Gif), Some("image/gif"));
    }

    #[test]
    fn webp() {
        let mut webp = vec![0u8; 16];
        webp[..4].copy_from_slice(b"RIFF");
        // 4-byte size (ignored by sniff)
        webp[8..12].copy_from_slice(b"WEBP");
        assert_eq!(sniff(&webp), Format::Webp);
        assert!(!format_is_allowed(Format::Webp));
        assert_eq!(format_to_mime(Format::Webp), Some("image/webp"));
    }

    #[test]
    fn avif_brand() {
        let mut buf = vec![0u8; 16];
        buf[4..8].copy_from_slice(b"ftyp");
        buf[8..12].copy_from_slice(b"avif");
        assert_eq!(sniff(&buf), Format::Avif);
        buf[8..12].copy_from_slice(b"avis");
        assert_eq!(sniff(&buf), Format::Avif);
        assert!(!format_is_allowed(Format::Avif));
    }

    #[test]
    fn heic_brands() {
        for brand in [
            b"heic", b"heix", b"hevc", b"hevx", b"heim", b"heis", b"hevm", b"hevs",
            b"mif1",
        ] {
            let mut buf = vec![0u8; 16];
            buf[4..8].copy_from_slice(b"ftyp");
            buf[8..12].copy_from_slice(brand);
            assert_eq!(sniff(&buf), Format::Heic, "brand {:?}", brand);
        }
        assert!(!format_is_allowed(Format::Heic));
    }

    #[test]
    fn tiff() {
        assert_eq!(sniff(&[0x49u8, 0x49, 0x2A, 0x00, 0]), Format::Tiff);
        assert_eq!(sniff(&[0x4Du8, 0x4D, 0x00, 0x2A, 0]), Format::Tiff);
        assert!(!format_is_allowed(Format::Tiff));
    }

    #[test]
    fn ico() {
        assert_eq!(sniff(&[0x00u8, 0x00, 0x01, 0x00, 0]), Format::Ico);
        assert!(!format_is_allowed(Format::Ico));
    }

    #[test]
    fn bmp() {
        assert_eq!(sniff(b"BM\x00\x00"), Format::Bmp);
        assert!(!format_is_allowed(Format::Bmp));
    }

    #[test]
    fn svg_xml_prolog() {
        assert_eq!(sniff(b"<?xml version=\"1.0\"?><svg/>"), Format::Svg);
        assert!(!format_is_allowed(Format::Svg));
    }

    #[test]
    fn svg_bare_tag() {
        assert_eq!(sniff(b"<svg xmlns=\"...\"/>"), Format::Svg);
    }

    #[test]
    fn svg_with_bom_and_whitespace() {
        let mut buf = vec![0xEFu8, 0xBB, 0xBF, b' ', b'\t', b'\n'];
        buf.extend_from_slice(b"<svg/>");
        assert_eq!(sniff(&buf), Format::Svg);
    }

    #[test]
    fn empty() {
        assert_eq!(sniff(&[]), Format::Unknown);
        assert!(!format_is_allowed(Format::Unknown));
        assert_eq!(format_to_mime(Format::Unknown), None);
    }

    #[test]
    fn random_bytes() {
        assert_eq!(sniff(b"hello, this is not an image"), Format::Unknown);
    }

    #[test]
    fn sniff_window_is_bounded() {
        // A 1 MiB input must not slow sniff appreciably. We don't
        // time here — just verify it returns and doesn't allocate /
        // panic.
        let big = vec![0u8; 1 << 20];
        assert_eq!(sniff(&big), Format::Unknown);
    }

    #[test]
    fn short_jpeg_three_bytes_recognised() {
        // The 3-byte SOI prefix is enough; sniff doesn't require
        // the APP0 follow-up.
        assert_eq!(sniff(&[0xFFu8, 0xD8, 0xFF]), Format::Jpeg);
    }
}
