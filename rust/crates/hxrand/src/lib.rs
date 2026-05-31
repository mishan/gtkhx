//! Cryptographic RNG for GtkHx.
//!
//! Replaces the C `rand.c` implementation. Exposes `gtkhx_random_bytes()` with
//! the same semantics as the old `random_bytes()`: fills `buf` with `nbytes`
//! of cryptographically-secure random data, returns `nbytes` on success or 0
//! on failure.

use std::ffi::c_uint;
use std::slice;

/// Fill `buf` with cryptographically-secure random data.
///
/// Returns `buf.len()` on success, 0 on failure.
pub fn random_bytes(buf: &mut [u8]) -> usize {
    if buf.is_empty() {
        return 0;
    }
    match getrandom::getrandom(buf) {
        Ok(()) => buf.len(),
        Err(_) => 0,
    }
}

/// Fill `buf` with `nbytes` of cryptographically-secure random data.
///
/// Returns `nbytes` on success, 0 on failure.
///
/// # Safety
///
/// `buf` must point to at least `nbytes` bytes of writable memory.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_random_bytes(buf: *mut u8, nbytes: c_uint) -> c_uint {
    if buf.is_null() || nbytes == 0 {
        return 0;
    }
    let slice = unsafe { slice::from_raw_parts_mut(buf, nbytes as usize) };
    random_bytes(slice) as c_uint
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fills_buffer_with_random_bytes() {
        let mut buf = [0u8; 32];
        let ret = random_bytes(&mut buf);
        assert_eq!(ret, 32);
        // Extremely unlikely that 32 random bytes are all zeros
        assert_ne!(buf, [0u8; 32]);
    }

    #[test]
    fn returns_zero_on_empty() {
        let mut buf = [0u8; 0];
        let ret = random_bytes(&mut buf);
        assert_eq!(ret, 0);
    }
}
