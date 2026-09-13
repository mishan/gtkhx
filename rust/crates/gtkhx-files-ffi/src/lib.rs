//! GtkHx's GTK-free C ABI over the shared safe file-transfer codec.

use core::ffi::c_int;
use hxfiles_xfer::ffo;

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

const _: () = assert!(core::mem::size_of::<GtkhxFilpInfo>() == 288);
const _: () = assert!(core::mem::offset_of!(GtkhxFilpInfo, data_fork_len) == 16);
const _: () = assert!(core::mem::offset_of!(GtkhxFilpInfo, comment) == 24);
const _: () = assert!(core::mem::offset_of!(GtkhxFilpInfo, comment_len) == 280);
const _: () = assert!(core::mem::offset_of!(GtkhxFilpInfo, ok) == 284);

#[no_mangle]
pub extern "C" fn gtkhx_ffo_info_block_len(b38: u8, b39: u8) -> usize {
    ffo::info_block_len(b38, b39)
}

/// # Safety
/// `marker` must address `marker_len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_ffo_fork_len(
    marker: *const u8,
    marker_len: usize,
    large: c_int,
) -> u64 {
    if marker.is_null() || marker_len < ffo::FORK_HEADER_LEN {
        return 0;
    }
    let mut value = [0; ffo::FORK_HEADER_LEN];
    // SAFETY: the pointer and length contract was checked above.
    unsafe {
        core::ptr::copy_nonoverlapping(marker, value.as_mut_ptr(), value.len());
    }
    ffo::fork_len(&value, large != 0)
}

/// # Safety
/// `tag` must address four readable bytes and `out` sixteen writable bytes.
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
    let mut wire_tag = [0; 4];
    // SAFETY: both pointer/length contracts were checked above.
    unsafe {
        core::ptr::copy_nonoverlapping(tag, wire_tag.as_mut_ptr(), wire_tag.len());
        let Ok(value) = ffo::pack_fork_header(&wire_tag, length, large != 0) else {
            return;
        };
        core::ptr::copy_nonoverlapping(value.as_ptr(), out, value.len());
    }
}

/// # Safety
/// `info` must address `info_len` readable bytes and `out` one writable value.
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
    let empty = GtkhxFilpInfo {
        type_creator: [0; 8],
        create_time: [0; 4],
        modify_time: [0; 4],
        data_fork_len: 0,
        comment: [0; 256],
        comment_len: 0,
        ok: 0,
    };
    // SAFETY: the caller promises a writable output value.
    unsafe { out.write(empty) };
    if info.is_null() || info_len == 0 || info_len > isize::MAX as usize {
        return;
    }
    // SAFETY: the input pointer/length contract was checked above.
    let bytes = unsafe { core::slice::from_raw_parts(info, info_len) };
    let Ok(parsed) = ffo::parse_filp_info(bytes, large != 0) else {
        return;
    };
    // SAFETY: `out` remains the writable value checked above.
    let out = unsafe { &mut *out };
    out.type_creator = parsed.type_creator;
    out.create_time = parsed.create_time;
    out.modify_time = parsed.modify_time;
    out.data_fork_len = parsed.data_fork_len;
    let count = parsed.comment.len().min(out.comment.len());
    out.comment[..count].copy_from_slice(&parsed.comment[..count]);
    out.comment_len = count as u32;
    out.ok = 1;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn abi_layout_and_fork_math_stay_compatible() {
        assert_eq!(core::mem::size_of::<GtkhxFilpInfo>(), 288);
        assert_eq!(gtkhx_ffo_info_block_len(0, 0), 16);
        assert_eq!(gtkhx_ffo_info_block_len(1, 0x23), 0x100 + 0x23 + 16);
        let marker = ffo::pack_fork_header(b"DATA", 0x1_4000_0000, true).unwrap();
        unsafe {
            assert_eq!(
                gtkhx_ffo_fork_len(marker.as_ptr(), marker.len(), 1),
                0x1_4000_0000
            );
            assert_eq!(
                gtkhx_ffo_fork_len(marker.as_ptr(), marker.len(), 0),
                0x4000_0000
            );
            assert_eq!(gtkhx_ffo_fork_len(marker.as_ptr(), 8, 1), 0);
            assert_eq!(gtkhx_ffo_fork_len(core::ptr::null(), marker.len(), 1), 0);
        }

        let mut packed = [0; ffo::FORK_HEADER_LEN];
        unsafe {
            gtkhx_ffo_pack_fork_header(
                b"MACR".as_ptr(),
                4,
                0x1_4000_0000,
                1,
                packed.as_mut_ptr(),
                packed.len(),
            )
        };
        assert_eq!(&packed[..4], b"MACR");
        assert_eq!(ffo::fork_len(&packed, true), 0x1_4000_0000);

        let mut short = [0; 8];
        unsafe {
            gtkhx_ffo_pack_fork_header(b"DATA".as_ptr(), 4, 1, 0, short.as_mut_ptr(), short.len())
        };
        assert_eq!(short, [0; 8]);
    }

    #[test]
    fn facade_parses_the_deployed_filp_layout_and_fails_closed() {
        let mut info_and_data = [0; 100];
        info_and_data[4..8].copy_from_slice(b"TEXT");
        info_and_data[8..12].copy_from_slice(b"ttxt");
        info_and_data[71] = 5;
        info_and_data[78] = 3;
        info_and_data[79..82].copy_from_slice(b"abc");
        info_and_data[84..].copy_from_slice(&ffo::pack_fork_header(b"DATA", 12, false).unwrap());
        let mut out: GtkhxFilpInfo = unsafe { core::mem::zeroed() };
        unsafe {
            gtkhx_ffo_parse_filp_info(info_and_data.as_ptr(), info_and_data.len(), 0, &mut out)
        };
        assert_eq!(out.ok, 1);
        assert_eq!(&out.type_creator, b"TEXTttxt");
        assert_eq!(out.data_fork_len, 12);
        assert_eq!(&out.comment[..3], b"abc");

        let mut truncated: GtkhxFilpInfo = unsafe { core::mem::zeroed() };
        unsafe { gtkhx_ffo_parse_filp_info([0; 8].as_ptr(), 8, 0, &mut truncated) };
        assert_eq!(truncated.ok, 0);
    }
}
