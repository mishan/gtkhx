//! Hotline 8-byte wire timestamp decode.
//!
//! Two formats exist, auto-detected by the year field (Capabilities.md "Date
//! Format Selection"):
//!
//!   - **Mac 1904 epoch** (`year == 1904`): `secs` is total seconds since
//!     1904-01-01 00:00:00 UTC. Vintage Mac servers, mhxd, Mobius default.
//!   - **Modern** (`year != 1904`): `secs` is seconds since Jan 1 of `year` in
//!     *local* time. Servers that see DATA_CAPABILITIES from us switch to this
//!     because the 1904 epoch's u32 seconds field overflows in 2040.
//!
//! Wire layout (big-endian): `year:u16 / msecs:u16 (ignored) / secs:u32`.
//!
//! This is the pure *decode*. Turning the result into an absolute instant — the
//! modern branch needs the host's local timezone — and formatting it for
//! display are the view's job (it has a calendar library, e.g. `glib::DateTime`),
//! so they deliberately don't live here.

/// Seconds between the Mac 1904 epoch (1904-01-01 UTC) and the Unix epoch.
pub const MAC_TO_UNIX_EPOCH_OFFSET: u32 = 2_082_844_800;

/// A decoded Hotline timestamp, still in its wire-format flavour.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HlDate {
    /// Mac 1904 epoch: `secs` since 1904-01-01 00:00:00 UTC. The absolute Unix
    /// time is `secs - MAC_TO_UNIX_EPOCH_OFFSET`.
    Mac1904 { secs: u32 },
    /// Modern: `secs` since Jan 1 `year` 00:00:00 *local* time.
    Modern { year: u16, secs: u32 },
}

/// Decode an 8-byte wire timestamp.
///
/// Returns `None` for the all-zero "no timestamp set" sentinel (`secs == 0`,
/// which both formats agree on), a modern `year` outside the plausible Hotline
/// range `[1970, 2200]`, or a buffer shorter than 8 bytes.
pub fn parse_hl_date(bytes: &[u8]) -> Option<HlDate> {
    if bytes.len() < 8 {
        return None;
    }
    let year = u16::from_be_bytes([bytes[0], bytes[1]]);
    let secs = u32::from_be_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]);

    // "No timestamp" sentinel — both formats agree on zero.
    if secs == 0 {
        return None;
    }
    if year == 1904 {
        return Some(HlDate::Mac1904 { secs });
    }
    // Modern format. Refuse years that can't be made sense of rather than let
    // the view do nonsense calendar arithmetic.
    if !(1970..=2200).contains(&year) {
        return None;
    }
    Some(HlDate::Modern { year, secs })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pack(year: u16, msecs: u16, secs: u32) -> [u8; 8] {
        let mut b = [0u8; 8];
        b[0..2].copy_from_slice(&year.to_be_bytes());
        b[2..4].copy_from_slice(&msecs.to_be_bytes());
        b[4..8].copy_from_slice(&secs.to_be_bytes());
        b
    }

    #[test]
    fn mac_1904_epoch_decodes_to_secs() {
        // year=1904, secs=the 1904→1970 offset → the view resolves this to
        // Unix t=0.
        assert_eq!(
            parse_hl_date(&pack(1904, 0, MAC_TO_UNIX_EPOCH_OFFSET)),
            Some(HlDate::Mac1904 {
                secs: MAC_TO_UNIX_EPOCH_OFFSET
            })
        );
        assert_eq!(
            parse_hl_date(&pack(1904, 0, MAC_TO_UNIX_EPOCH_OFFSET + 86400)),
            Some(HlDate::Mac1904 {
                secs: MAC_TO_UNIX_EPOCH_OFFSET + 86400
            })
        );
    }

    #[test]
    fn modern_format_decodes_year_and_secs() {
        assert_eq!(
            parse_hl_date(&pack(2026, 0, 1)),
            Some(HlDate::Modern {
                year: 2026,
                secs: 1
            })
        );
        assert_eq!(
            parse_hl_date(&pack(1990, 0, 30 * 86400 + 3600)),
            Some(HlDate::Modern {
                year: 1990,
                secs: 30 * 86400 + 3600
            })
        );
    }

    #[test]
    fn zero_seconds_is_the_no_timestamp_sentinel() {
        assert_eq!(parse_hl_date(&pack(1904, 0, 0)), None);
        assert_eq!(parse_hl_date(&pack(2026, 0, 0)), None);
        assert_eq!(parse_hl_date(&[0u8; 8]), None);
    }

    #[test]
    fn year_out_of_range_refused() {
        assert_eq!(parse_hl_date(&pack(1969, 0, 1)), None);
        assert_eq!(parse_hl_date(&pack(2201, 0, 1)), None);
        assert_eq!(parse_hl_date(&pack(0, 0, 1)), None);
    }

    #[test]
    fn short_buffer_refused() {
        assert_eq!(parse_hl_date(&[0u8; 7]), None);
        assert_eq!(parse_hl_date(&[]), None);
    }
}
