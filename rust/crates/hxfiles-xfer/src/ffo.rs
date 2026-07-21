//! FFO (Flattened File Object) / FILP frame codec + HFS fork math.
//!
//! Ported byte-for-byte from `src/xfers.c::file_recv_one` (the receive
//! path) and the fork-header encode in `src/xfers.c::file_send_one`.
//! The wire shapes here are pinned by the same oracle the C test
//! `tests/proto/test_large_file.c` uses (`pack_fork_header` /
//! `unpack_fork_length`), so this stays a faithful reimplementation
//! rather than a fresh interpretation of the format.
//!
//! Everything is pure and panic-free on hostile input: the C indexes
//! the FILP buffer blindly (it trusts the server), but a corrupt or
//! short block here returns [`FfoError::Truncated`] instead of reading
//! out of bounds — the worker maps that to an errno and aborts the
//! transfer.

/// Size of a fork header (DATA / MACR / INFO marker): a 4-byte tag, a
/// 4-byte compression/high-32 slot, 4 reserved bytes, and a 4-byte
/// data-size/low-32 slot.
pub const FORK_HEADER_LEN: usize = 16;

/// The mac(1904)↔header(2000) epoch delta, in seconds. See `src/hfs.h`:
/// `hfs_m_to_htime` subtracts it, `hfs_h_to_mtime` adds it. u32 wrapping
/// arithmetic, matching the C `ntohl(ARG) ± 3029529600U` on 32-bit
/// values.
pub const HFS_MAC_HEADER_DELTA: u32 = 3_029_529_600;

/// Errors from parsing a FILP info block.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FfoError {
    /// The info block was shorter than the fields the FILP layout
    /// requires (fork marker, type/creator, comment, or timestamps).
    Truncated,
}

/// Decode a 16-byte fork header's length.
///
/// Mirrors `HN32(&lo, &buf[..+12]); if (large) HN32(&hi, &buf[..+4])`
/// in `file_recv_one`, and `unpack_fork_length` in
/// `tests/proto/test_large_file.c`:
/// - low 32 bits: big-endian at offset 12 (the DataSize slot),
/// - high 32 bits (large-file mode only): big-endian at offset 4 (the
///   Compression slot); zero in legacy mode.
///
/// A legacy decode of a large-file-encoded header truncates to the low
/// 32 bits — the deliberate behaviour pinned by
/// `test_ffo_fork_header_legacy_decoder_truncates`.
pub fn fork_len(marker: &[u8; FORK_HEADER_LEN], large: bool) -> u64 {
    let lo = u32::from_be_bytes([marker[12], marker[13], marker[14], marker[15]]);
    let hi = if large {
        u32::from_be_bytes([marker[4], marker[5], marker[6], marker[7]])
    } else {
        0
    };
    ((hi as u64) << 32) | (lo as u64)
}

/// Encode a 16-byte fork header. Mirrors `pack_fork_header` in
/// `tests/proto/test_large_file.c` (the encode `file_send_one` runs):
/// tag at 0, high-32 at 4 (large mode only), low-32 at 12.
///
/// In legacy mode only the low 32 bits are written; a length above
/// `u32::MAX` is truncated to its low half rather than panicking (the
/// worker never packs a legacy header for an over-4-GiB fork, so this
/// is a defensive choice, not a live path).
pub fn pack_fork_header(tag: &[u8; 4], length: u64, large: bool) -> [u8; FORK_HEADER_LEN] {
    let mut buf = [0u8; FORK_HEADER_LEN];
    buf[0..4].copy_from_slice(tag);
    let lo = (length & 0xFFFF_FFFF) as u32;
    buf[12..16].copy_from_slice(&lo.to_be_bytes());
    if large {
        let hi = (length >> 32) as u32;
        buf[4..8].copy_from_slice(&hi.to_be_bytes());
    }
    buf
}

/// Convert a 4-byte mac-epoch (1904) timestamp on the wire to the
/// header epoch (2000). Mirrors `hfs_m_to_htime` from `src/hfs.h`:
/// `htonl(ntohl(ARG) - 3029529600U)`, i.e. read big-endian, subtract
/// the delta with u32 wraparound, write big-endian.
pub fn hfs_m_to_htime(wire: [u8; 4]) -> [u8; 4] {
    let mac = u32::from_be_bytes(wire);
    mac.wrapping_sub(HFS_MAC_HEADER_DELTA).to_be_bytes()
}

/// Inverse of [`hfs_m_to_htime`]: header epoch (2000) → mac epoch
/// (1904). Mirrors `hfs_h_to_mtime`: `htonl(ntohl(ARG) + 3029529600U)`.
pub fn hfs_h_to_mtime(wire: [u8; 4]) -> [u8; 4] {
    let hdr = u32::from_be_bytes(wire);
    hdr.wrapping_add(HFS_MAC_HEADER_DELTA).to_be_bytes()
}

/// Number of bytes to read after the 40-byte FILP fixed header — the
/// variable info+comment block plus the trailing 16-byte DATA marker.
///
/// Mirrors `len = (buf[38] ? 0x100 : 0) + buf[39]; len += 16;` in
/// `file_recv_one`. Byte 38 is a boolean high-nibble flag (any non-zero
/// value contributes 0x100), byte 39 is the low byte.
pub fn info_block_len(b38: u8, b39: u8) -> usize {
    (if b38 != 0 { 0x100 } else { 0 }) + b39 as usize + FORK_HEADER_LEN
}

/// The fields `file_recv_one` extracts from the FILP info block after
/// the second read.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FilpInfo {
    /// `type[4]` followed by `creator[4]` — the 8 bytes at info offset 4
    /// (`memcpy(typecrea, &buf[4], 8)`).
    pub type_creator: [u8; 8],
    /// Creation time converted to the header epoch, kept big-endian —
    /// exactly what the C stashed in `struct hfsinfo::create_time`.
    pub create_time: [u8; 4],
    /// Modify time, likewise converted.
    pub modify_time: [u8; 4],
    /// Finder comment bytes (`comlen = buf[73 + buf[71]]`, comment at
    /// `buf[74 + buf[71]]`).
    pub comment: Vec<u8>,
    /// Data-fork length from the trailing 16-byte DATA marker.
    pub data_fork_len: u64,
}

/// Parse the FILP info block (the second read in `file_recv_one`, whose
/// length is [`info_block_len`]). `large` selects the fork-length
/// interpretation for the trailing DATA marker.
///
/// Field offsets match the C exactly: type/creator at 4, timestamps at
/// 56 / 64 (munged mac→header), comment length at `73 + info[71]` with
/// bytes following, and the DATA marker as the final 16 bytes.
pub fn parse_filp_info(info: &[u8], large: bool) -> Result<FilpInfo, FfoError> {
    // The DATA fork marker is the trailing 16 bytes (`buf[pos-16..pos]`).
    if info.len() < FORK_HEADER_LEN {
        return Err(FfoError::Truncated);
    }
    let marker_off = info.len() - FORK_HEADER_LEN;
    let mut marker = [0u8; FORK_HEADER_LEN];
    marker.copy_from_slice(&info[marker_off..]);
    let data_fork_len = fork_len(&marker, large);

    let type_creator: [u8; 8] = info
        .get(4..12)
        .ok_or(FfoError::Truncated)?
        .try_into()
        .expect("slice is exactly 8 bytes");

    // Comment: length at info[73 + info[71]], bytes at info[74 + off..].
    let coff = *info.get(71).ok_or(FfoError::Truncated)? as usize;
    let comlen = *info.get(73 + coff).ok_or(FfoError::Truncated)? as usize;
    let cstart = 74 + coff;
    let comment = info
        .get(cstart..cstart + comlen)
        .ok_or(FfoError::Truncated)?
        .to_vec();

    let create_raw: [u8; 4] = info
        .get(56..60)
        .ok_or(FfoError::Truncated)?
        .try_into()
        .expect("slice is exactly 4 bytes");
    let modify_raw: [u8; 4] = info
        .get(64..68)
        .ok_or(FfoError::Truncated)?
        .try_into()
        .expect("slice is exactly 4 bytes");

    Ok(FilpInfo {
        type_creator,
        create_time: hfs_m_to_htime(create_raw),
        modify_time: hfs_m_to_htime(modify_raw),
        comment,
        data_fork_len,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    // ---- fork-length decode/encode — mirrors test_large_file.c ----

    #[test]
    fn fork_header_legacy_32bit() {
        let buf = pack_fork_header(b"DATA", 0x1234_5678, false);
        assert_eq!(&buf[0..4], b"DATA");
        // Compression slot + reserved zero in legacy mode.
        assert_eq!(&buf[4..12], &[0u8; 8]);
        // DataSize big-endian in the last four bytes.
        assert_eq!(&buf[12..16], &[0x12, 0x34, 0x56, 0x78]);
        assert_eq!(fork_len(&buf, false), 0x1234_5678);
    }

    #[test]
    fn fork_header_large_mode_under_4gib() {
        let buf = pack_fork_header(b"DATA", 0x1234_5678, true);
        // High half zero, value in the low half.
        assert_eq!(&buf[4..8], &[0u8; 4]);
        assert_eq!(&buf[12..16], &[0x12, 0x34, 0x56, 0x78]);
        assert_eq!(fork_len(&buf, true), 0x1234_5678);
    }

    #[test]
    fn fork_header_large_mode_over_4gib() {
        // 5 GiB = 0x1_4000_0000 → high=1, low=0x4000_0000.
        let size = 0x1_4000_0000u64;
        let buf = pack_fork_header(b"DATA", size, true);
        assert_eq!(&buf[4..8], &[0x00, 0x00, 0x00, 0x01]);
        assert_eq!(&buf[12..16], &[0x40, 0x00, 0x00, 0x00]);
        assert_eq!(fork_len(&buf, true), size);
    }

    #[test]
    fn legacy_decoder_truncates_large_payload() {
        // Pin the spec's "legacy readers drop the high half" behaviour.
        let buf = pack_fork_header(b"DATA", 0x1_4000_0000, true);
        assert_eq!(fork_len(&buf, false), 0x4000_0000);
    }

    #[test]
    fn macr_and_data_share_layout() {
        let macr = pack_fork_header(b"MACR", 0x100, true);
        let data = pack_fork_header(b"DATA", 0x200, true);
        assert_eq!(&macr[0..4], b"MACR");
        assert_eq!(&data[0..4], b"DATA");
        assert_eq!(fork_len(&macr, true), 0x100);
        assert_eq!(fork_len(&data, true), 0x200);
    }

    #[test]
    fn fork_len_roundtrips() {
        for &v in &[0u64, 1, 0xFFFF_FFFF, 0x1_0000_0000, 0x1_4000_0066, u64::MAX] {
            assert_eq!(fork_len(&pack_fork_header(b"DATA", v, true), true), v);
        }
        // Legacy round-trips values that fit in 32 bits.
        for &v in &[0u64, 1, 0x1234, 0xFFFF_FFFF] {
            assert_eq!(fork_len(&pack_fork_header(b"DATA", v, false), false), v);
        }
    }

    // ---- HFS epoch conversion — mirrors src/hfs.h macros ----

    #[test]
    fn hfs_epoch_delta_matches_hfs_h() {
        // The exact constant from hfs.h (`3029529600U`).
        assert_eq!(HFS_MAC_HEADER_DELTA, 3_029_529_600);
    }

    #[test]
    fn hfs_mac_to_header_known_vector() {
        // mac == the delta → header epoch 0.
        let mac = HFS_MAC_HEADER_DELTA.to_be_bytes();
        assert_eq!(hfs_m_to_htime(mac), 0u32.to_be_bytes());
        // A concrete positive value: 3_100_000_000 mac → 70_470_400 hdr.
        let mac = 3_100_000_000u32.to_be_bytes();
        assert_eq!(u32::from_be_bytes(hfs_m_to_htime(mac)), 70_470_400);
    }

    #[test]
    fn hfs_epoch_roundtrips_with_wraparound() {
        for &v in &[0u32, 1, 946_684_800, 2_082_844_800, 3_029_529_600, u32::MAX] {
            let wire = v.to_be_bytes();
            assert_eq!(hfs_h_to_mtime(hfs_m_to_htime(wire)), wire);
            assert_eq!(hfs_m_to_htime(hfs_h_to_mtime(wire)), wire);
        }
    }

    // ---- info-block length — mirrors file_recv_one ----

    #[test]
    fn info_block_len_matches_c_formula() {
        assert_eq!(info_block_len(0, 0), 16);
        assert_eq!(info_block_len(0, 0x50), 0x50 + 16);
        assert_eq!(info_block_len(1, 0), 0x100 + 16);
        assert_eq!(info_block_len(1, 0x23), 0x100 + 0x23 + 16);
        // Byte 38 is boolean: any non-zero value contributes 0x100.
        assert_eq!(info_block_len(0xFF, 0), 0x100 + 16);
    }

    // ---- FILP info block parse — hand-assembled fixture ----

    fn build_info_fixture(coff: u8, comment: &[u8], mac_c: u32, mac_m: u32) -> Vec<u8> {
        // 84 bytes of info + a 16-byte trailing DATA marker = 100.
        let mut info = vec![0u8; 100];
        info[4..8].copy_from_slice(b"TEXT");
        info[8..12].copy_from_slice(b"ttxt");
        info[56..60].copy_from_slice(&mac_c.to_be_bytes());
        info[64..68].copy_from_slice(&mac_m.to_be_bytes());
        info[71] = coff;
        info[73 + coff as usize] = comment.len() as u8;
        let cstart = 74 + coff as usize;
        info[cstart..cstart + comment.len()].copy_from_slice(comment);
        let marker = pack_fork_header(b"DATA", 0x1000, false);
        info[84..100].copy_from_slice(&marker);
        info
    }

    #[test]
    fn parse_filp_info_extracts_all_fields() {
        let mac_c = 3_100_000_000u32;
        let mac_m = 3_200_000_000u32;
        let info = build_info_fixture(5, b"abc", mac_c, mac_m);

        let fi = parse_filp_info(&info, false).unwrap();
        assert_eq!(&fi.type_creator, b"TEXTttxt");
        assert_eq!(fi.comment, b"abc");
        assert_eq!(fi.data_fork_len, 0x1000);
        assert_eq!(fi.create_time, hfs_m_to_htime(mac_c.to_be_bytes()));
        assert_eq!(fi.modify_time, hfs_m_to_htime(mac_m.to_be_bytes()));
        assert_eq!(u32::from_be_bytes(fi.create_time), 70_470_400);
    }

    #[test]
    fn parse_filp_info_large_mode_data_fork() {
        let mut info = build_info_fixture(0, b"", 0, 0);
        // Re-encode the trailing marker as a >4 GiB large-file fork.
        let marker = pack_fork_header(b"DATA", 0x1_4000_0000, true);
        info[84..100].copy_from_slice(&marker);
        let fi = parse_filp_info(&info, true).unwrap();
        assert_eq!(fi.data_fork_len, 0x1_4000_0000);
    }

    #[test]
    fn parse_filp_info_rejects_truncation() {
        // Shorter than a fork marker.
        assert_eq!(parse_filp_info(&[0u8; 8], false), Err(FfoError::Truncated));
        // Long enough for the marker but not for the comment it claims.
        let mut info = vec![0u8; 100];
        info[71] = 5;
        info[78] = 200; // comment length that runs off the end
        assert_eq!(parse_filp_info(&info, false), Err(FfoError::Truncated));
    }
}
