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
}
