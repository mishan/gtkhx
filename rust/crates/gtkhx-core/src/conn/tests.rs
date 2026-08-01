//! Accessor round-trips + layout sanity for `gtkhx-core::conn`. The byte-exact C-ABI
//! layout pin lives in `hxconn_layout.h`'s `_Static_assert`s (checked at the C
//! build); these are the Rust-side behaviour checks.

use super::*;

/// Run a closure with a fresh connection, freeing it afterwards.
fn with_conn(f: impl FnOnce(*mut HtlcConn)) {
    let h = hx_conn_new();
    assert!(!h.is_null());
    f(h);
    unsafe { hx_conn_free(h) };
}

#[test]
fn new_is_zeroed() {
    with_conn(|h| unsafe {
        assert_eq!(hx_conn_trans(h), 0);
        assert_eq!(hx_conn_uid(h), 0);
        assert_eq!(hx_conn_caps(h), 0);
        assert_eq!(hx_conn_fd(h), 0);
        assert_eq!(hx_conn_logged_in(h), GFALSE);
        assert_eq!(hx_conn_post_login_fetched(h), GFALSE);
        assert!(hx_conn_sess(h).is_null());
        assert!(hx_conn_hope_aead(h).is_null());
        // empty strings
        assert_eq!(*hx_conn_name(h), 0);
        assert_eq!(*hx_conn_serverhost(h), 0);
    });
}

#[test]
fn scalar_round_trips() {
    with_conn(|h| unsafe {
        hx_conn_set_uid(h, 0xBEEF);
        hx_conn_set_icon(h, 412);
        hx_conn_set_version(h, 190);
        hx_conn_set_caps(h, 0x0000_0000_DEAD_BEEF);
        hx_conn_set_fd(h, 7);
        hx_conn_set_nick_color(h, 0x00FF_8800);
        hx_conn_set_chat_history_last_msgid(h, 0x1_0000_0002);
        assert_eq!(hx_conn_uid(h), 0xBEEF);
        assert_eq!(hx_conn_icon(h), 412);
        assert_eq!(hx_conn_version(h), 190);
        assert_eq!(hx_conn_caps(h), 0x0000_0000_DEAD_BEEF);
        assert_eq!(hx_conn_fd(h), 7);
        assert_eq!(hx_conn_nick_color(h), 0x00FF_8800);
        assert_eq!(hx_conn_chat_history_last_msgid(h), 0x1_0000_0002);
    });
}

#[test]
fn trans_post_inc() {
    with_conn(|h| unsafe {
        hx_conn_set_trans(h, 41);
        assert_eq!(hx_conn_trans_post_inc(h), 41);
        assert_eq!(hx_conn_trans(h), 42);
    });
}

#[test]
fn flags_are_independent_bits() {
    with_conn(|h| unsafe {
        hx_conn_set_logged_in(h, GTRUE);
        assert_eq!(hx_conn_logged_in(h), GTRUE);
        assert_eq!(hx_conn_post_login_fetched(h), GFALSE);
        hx_conn_set_post_login_fetched(h, GTRUE);
        assert_eq!(hx_conn_logged_in(h), GTRUE);
        hx_conn_set_logged_in(h, GFALSE);
        assert_eq!(hx_conn_logged_in(h), GFALSE);
        assert_eq!(hx_conn_post_login_fetched(h), GTRUE);
    });
}

#[test]
fn strings_truncate_and_terminate() {
    with_conn(|h| unsafe {
        // name[32] — a 40-char source must truncate to 31 + NUL.
        let long = std::ffi::CString::new("A".repeat(40)).unwrap();
        hx_conn_set_name(h, long.as_ptr());
        let got = std::ffi::CStr::from_ptr(hx_conn_name(h)).to_bytes();
        assert_eq!(got.len(), 31);
        // round-trip a normal string
        let n = std::ffi::CString::new("guest").unwrap();
        hx_conn_set_name(h, n.as_ptr());
        assert_eq!(
            std::ffi::CStr::from_ptr(hx_conn_name(h)).to_str().unwrap(),
            "guest"
        );
    });
}

#[test]
fn cipheralg_zeroes_then_sets() {
    with_conn(|h| unsafe {
        let a = std::ffi::CString::new("CHACHA20-POLY1305").unwrap();
        hx_conn_set_cipheralg(h, a.as_ptr());
        assert_eq!(
            std::ffi::CStr::from_ptr(hx_conn_cipheralg(h))
                .to_str()
                .unwrap(),
            "CHACHA20-POLY1305"
        );
        // empty string clears it
        let empty = std::ffi::CString::new("").unwrap();
        hx_conn_set_cipheralg(h, empty.as_ptr());
        assert_eq!(*hx_conn_cipheralg(h), 0);
    });
}

#[test]
fn access_bitmap_big_endian() {
    with_conn(|h| unsafe {
        // bit 0 = MSB of byte 0; bit 20 (READ_NEWS) = byte 2, mask 0x08.
        let mut bytes = [0u8; 8];
        bytes[0] = 0x80; // bit 0
        bytes[2] = 0x08; // bit 20
        hx_conn_set_access(h, bytes.as_ptr());
        assert_eq!(hx_conn_access_has(h, 0), GTRUE);
        assert_eq!(hx_conn_access_has(h, 20), GTRUE);
        assert_eq!(hx_conn_access_has(h, 1), GFALSE);
        assert_eq!(hx_conn_access_has(h, 63), GFALSE);
        // out of range
        assert_eq!(hx_conn_access_has(h, -1), GFALSE);
        assert_eq!(hx_conn_access_has(h, 64), GFALSE);
    });
}

#[test]
fn access_permits_all_zero_is_permissive() {
    with_conn(|h| unsafe {
        // all-zero bitmap (legacy server) → permits everything.
        assert_eq!(hx_conn_access_permits(h, 20), GTRUE);
        assert_eq!(hx_conn_access_permits(h, 22), GTRUE);
        // once any bit is set, only set bits permit.
        let mut bytes = [0u8; 8];
        bytes[0] = 0x80; // bit 0 only
        hx_conn_set_access(h, bytes.as_ptr());
        assert_eq!(hx_conn_access_permits(h, 0), GTRUE);
        assert_eq!(hx_conn_access_permits(h, 20), GFALSE);
    });
}

#[test]
fn media_limits_reset() {
    with_conn(|h| unsafe {
        hx_conn_set_media_max_bytes(h, 1000);
        hx_conn_set_media_chunk_size(h, 60000);
        assert_eq!(hx_conn_media_max_bytes(h), 1000);
        hx_conn_reset_media_limits(h);
        assert_eq!(hx_conn_media_max_bytes(h), 0);
        assert_eq!(hx_conn_media_chunk_size(h), 0);
    });
}

#[test]
fn has_cap_masks() {
    with_conn(|h| unsafe {
        hx_conn_set_caps(h, 0b1010);
        assert_eq!(hx_conn_has_cap(h, 0b0010), GTRUE);
        assert_eq!(hx_conn_has_cap(h, 0b0100), GFALSE);
        assert_eq!(hx_conn_has_cap(h, 0b1000), GTRUE);
    });
}

#[test]
fn layout_is_pinned() {
    assert_eq!(std::mem::size_of::<HtlcConn>(), HXCONN_SIZEOF);
    assert_eq!(std::mem::align_of::<HtlcConn>(), 8);
}

#[test]
fn a_long_name_is_cut_on_a_character_boundary() {
    // The wire name field is 32 bytes. A byte-wise cut at 31 splits a
    // multi-byte character in half and puts an invalid sequence on the wire,
    // where it lands in every other client's user list.
    let conn = hx_conn_new();

    // 16 two-byte characters = 32 bytes: one too many for the field, and the
    // naive cut falls exactly between the halves of the 16th.
    let long = "é".repeat(16);
    let c = std::ffi::CString::new(long).unwrap();
    unsafe { hx_conn_set_name(conn, c.as_ptr()) };
    let got = unsafe { std::ffi::CStr::from_ptr(hx_conn_name(conn)) }
        .to_str()
        .expect("a truncated name must still be valid UTF-8");
    assert_eq!(got, "é".repeat(15), "{} bytes", got.len());

    // A three-byte character straddling the limit from a different offset.
    let mixed = format!("{}{}", "a".repeat(30), "→");
    let c = std::ffi::CString::new(mixed).unwrap();
    unsafe { hx_conn_set_name(conn, c.as_ptr()) };
    let got = unsafe { std::ffi::CStr::from_ptr(hx_conn_name(conn)) }
        .to_str()
        .expect("valid UTF-8");
    assert_eq!(got, "a".repeat(30));

    // And a name that fits is untouched, including one that exactly fills it.
    let exact = "a".repeat(31);
    let c = std::ffi::CString::new(exact.clone()).unwrap();
    unsafe { hx_conn_set_name(conn, c.as_ptr()) };
    let got = unsafe { std::ffi::CStr::from_ptr(hx_conn_name(conn)) };
    assert_eq!(got.to_str().unwrap(), exact);

    unsafe { hx_conn_free(conn) };
}
