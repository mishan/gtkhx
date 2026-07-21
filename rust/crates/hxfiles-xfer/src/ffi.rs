//! C-ABI surface for the file-transfer codec. Symbols are
//! `gtkhx_ffo_*` to match the existing `gtkhx_files_*` / `gtkhx_proto_*`
//! naming.
//!
//! Phase F2, slice 1: these are the two fiddly, error-prone pieces of
//! the receive-path byte math (`src/xfers.c::file_recv_one`) — the
//! variable info-block length and the 16-byte fork-header length decode
//! with its large-file high32/low32 split. The C worker keeps its loop
//! structure and local I/O for now and calls in here for the math; the
//! full tokio-worker relocation (which will also use `parse_filp_info`
//! and the HFS epoch helpers) is a later increment.

use crate::ffo;
use core::ffi::c_int;

/// C-ABI mirror of the fields `file_recv_one` extracts from a parsed
/// FILP info block. Field order + natural alignment match the
/// `struct gtkhx_filp_info` the C side pins with `offsetof` static
/// asserts. `ok` is 1 on a successful parse, 0 when the info block was
/// truncated (the C worker then fails the transfer with EIO instead of
/// the old blind indexing).
#[repr(C)]
pub struct GtkhxFilpInfo {
    pub type_creator: [u8; 8],
    pub create_time: [u8; 4],
    pub modify_time: [u8; 4],
    pub data_fork_len: u64,
    pub comment: [u8; 256],
    pub comment_len: u32,
    pub ok: c_int,
}

// Pin the layout the C offsetof asserts also check.
const _: () = assert!(core::mem::size_of::<GtkhxFilpInfo>() == 288);
const _: () = assert!(core::mem::offset_of!(GtkhxFilpInfo, data_fork_len) == 16);
const _: () = assert!(core::mem::offset_of!(GtkhxFilpInfo, comment) == 24);
const _: () = assert!(core::mem::offset_of!(GtkhxFilpInfo, comment_len) == 280);
const _: () = assert!(core::mem::offset_of!(GtkhxFilpInfo, ok) == 284);

/// Bytes to read after the 40-byte FILP fixed header (info+comment
/// block plus the trailing 16-byte DATA marker). Shim behind
/// `len = (buf[38] ? 0x100 : 0) + buf[39]; len += 16;`.
#[no_mangle]
pub extern "C" fn gtkhx_ffo_info_block_len(b38: u8, b39: u8) -> usize {
    ffo::info_block_len(b38, b39)
}

/// Decode a 16-byte fork header's length. `large != 0` selects the
/// large-file interpretation (high 32 bits from the Compression slot at
/// offset 4). Shim behind the `HN32(&lo, ...); if (large) HN32(&hi, ...)`
/// blocks in `file_recv_one` for both the DATA and MACR markers.
///
/// # Safety
/// `marker` must point to at least `marker_len` readable bytes. A
/// `marker_len` below 16 (a caller bug) yields 0 rather than reading out
/// of bounds.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_ffo_fork_len(
    marker: *const u8,
    marker_len: usize,
    large: c_int,
) -> u64 {
    if marker.is_null() || marker_len < ffo::FORK_HEADER_LEN {
        return 0;
    }
    let mut m = [0u8; ffo::FORK_HEADER_LEN];
    // SAFETY: non-null with >= FORK_HEADER_LEN readable bytes per the
    // caller contract, just checked.
    unsafe {
        core::ptr::copy_nonoverlapping(marker, m.as_mut_ptr(), ffo::FORK_HEADER_LEN);
    }
    ffo::fork_len(&m, large != 0)
}

/// Pack a 16-byte fork header into `out` — the encode shim behind the
/// DATA / MACR marker writes in `src/xfers.c::file_send_one`. `tag`
/// supplies the 4-byte marker ("DATA" / "MACR"); `large != 0` writes the
/// high 32 bits into the Compression slot at offset 4 (else only the low
/// 32 bits at offset 12). The byte-for-byte twin of the receive-side
/// [`gtkhx_ffo_fork_len`] decode.
///
/// # Safety
/// `tag` must point to at least 4 readable bytes and `out` to at least
/// 16 writable bytes; short buffers (a caller bug) are a no-op.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_ffo_pack_fork_header(
    tag: *const u8,
    tag_len: usize,
    length: u64,
    large: c_int,
    out: *mut u8,
    out_len: usize,
) {
    if tag.is_null() || tag_len < 4 || out.is_null() || out_len < ffo::FORK_HEADER_LEN {
        return;
    }
    let mut t = [0u8; 4];
    // SAFETY: non-null with >= 4 readable bytes per the caller contract.
    unsafe {
        core::ptr::copy_nonoverlapping(tag, t.as_mut_ptr(), 4);
    }
    let hdr = ffo::pack_fork_header(&t, length, large != 0);
    // SAFETY: non-null with >= FORK_HEADER_LEN writable bytes.
    unsafe {
        core::ptr::copy_nonoverlapping(hdr.as_ptr(), out, ffo::FORK_HEADER_LEN);
    }
}

/// Parse a FILP info block into the C-ABI struct — the shim behind the
/// field extraction in `file_recv_one` (type/creator, timestamps munged
/// mac→header, comment, and the trailing DATA fork length). `large != 0`
/// selects the large-file fork interpretation.
///
/// The comment is clamped to the 256-byte inline buffer (real comments
/// are <200 bytes; a hostile length can't overflow). On a truncated
/// block `out.ok` is set to 0 and the rest is zeroed.
///
/// # Safety
/// `info` must point to at least `info_len` readable bytes; `out` must
/// point to a writable `GtkhxFilpInfo`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_ffo_parse_filp_info(
    info: *const u8,
    info_len: usize,
    large: c_int,
    out: *mut GtkhxFilpInfo,
) {
    if out.is_null() {
        return;
    }
    // SAFETY: caller guarantees `out` is writable.
    let out = unsafe { &mut *out };
    *out = GtkhxFilpInfo {
        type_creator: [0; 8],
        create_time: [0; 4],
        modify_time: [0; 4],
        data_fork_len: 0,
        comment: [0; 256],
        comment_len: 0,
        ok: 0,
    };
    if info.is_null() || info_len == 0 || info_len > isize::MAX as usize {
        return;
    }
    // SAFETY: non-null with `info_len` (<= isize::MAX) readable bytes.
    let slice = unsafe { core::slice::from_raw_parts(info, info_len) };
    let Ok(fi) = ffo::parse_filp_info(slice, large != 0) else {
        return;
    };
    out.type_creator = fi.type_creator;
    out.create_time = fi.create_time;
    out.modify_time = fi.modify_time;
    out.data_fork_len = fi.data_fork_len;
    let n = fi.comment.len().min(out.comment.len());
    out.comment[..n].copy_from_slice(&fi.comment[..n]);
    out.comment_len = n as u32;
    out.ok = 1;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn info_block_len_ffi_matches_pure() {
        assert_eq!(gtkhx_ffo_info_block_len(0, 0), 16);
        assert_eq!(gtkhx_ffo_info_block_len(1, 0x23), 0x100 + 0x23 + 16);
    }

    #[test]
    fn fork_len_ffi_matches_pure() {
        let marker = ffo::pack_fork_header(b"DATA", 0x1_4000_0000, true);
        unsafe {
            assert_eq!(
                gtkhx_ffo_fork_len(marker.as_ptr(), marker.len(), 1),
                0x1_4000_0000
            );
            // legacy decode truncates the high half
            assert_eq!(
                gtkhx_ffo_fork_len(marker.as_ptr(), marker.len(), 0),
                0x4000_0000
            );
            // undersized / null guard → 0
            assert_eq!(gtkhx_ffo_fork_len(marker.as_ptr(), 8, 1), 0);
            assert_eq!(gtkhx_ffo_fork_len(core::ptr::null(), 16, 1), 0);
        }
    }

    #[test]
    fn pack_fork_header_ffi_roundtrips_with_decode() {
        let mut out = [0u8; 16];
        unsafe {
            gtkhx_ffo_pack_fork_header(
                b"MACR".as_ptr(),
                4,
                0x1_4000_0000,
                1,
                out.as_mut_ptr(),
                out.len(),
            );
            assert_eq!(&out[0..4], b"MACR");
            // decode via the receive-side shim must recover the length
            assert_eq!(gtkhx_ffo_fork_len(out.as_ptr(), out.len(), 1), 0x1_4000_0000);
        }
        // short out buffer is a no-op (leaves it zeroed)
        let mut small = [0u8; 8];
        unsafe {
            gtkhx_ffo_pack_fork_header(b"DATA".as_ptr(), 4, 1, 0, small.as_mut_ptr(), small.len());
        }
        assert_eq!(small, [0u8; 8]);
    }

    #[test]
    fn parse_filp_info_ffi_fills_struct() {
        // Hand-assemble an info block: type/creator at 4, comment via
        // the buf[71]/buf[73+off] offsets, DATA marker as the last 16.
        let mut info = [0u8; 100];
        info[4..8].copy_from_slice(b"TEXT");
        info[8..12].copy_from_slice(b"ttxt");
        info[71] = 5;
        info[78] = 3; // comment length at 73 + 5
        info[79..82].copy_from_slice(b"abc");
        info[84..100].copy_from_slice(&ffo::pack_fork_header(b"DATA", 0x1000, false));

        let mut out: GtkhxFilpInfo = unsafe { core::mem::zeroed() };
        unsafe { gtkhx_ffo_parse_filp_info(info.as_ptr(), info.len(), 0, &mut out) };
        assert_eq!(out.ok, 1);
        assert_eq!(&out.type_creator, b"TEXTttxt");
        assert_eq!(out.data_fork_len, 0x1000);
        assert_eq!(out.comment_len, 3);
        assert_eq!(&out.comment[..3], b"abc");

        // Truncated block → ok = 0.
        let mut bad: GtkhxFilpInfo = unsafe { core::mem::zeroed() };
        unsafe { gtkhx_ffo_parse_filp_info([0u8; 8].as_ptr(), 8, 0, &mut bad) };
        assert_eq!(bad.ok, 0);
    }
}
