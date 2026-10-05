//! What the session's file events become, through recording doubles of the
//! file-list emit and the provider's error hook.

use super::doubles::test_env;
use super::*;

const A: *mut c_void = 0xA0 as *mut c_void;
const B: *mut c_void = 0xB0 as *mut c_void;
const PROVIDER: *mut c_void = 0xDA7A as *mut c_void;

fn entry(name: &[u8]) -> FileEntry {
    FileEntry {
        name: hxproto::text::to_utf8(name),
        name_bytes: name.to_vec(),
        folder: false,
        size: 1,
        type_code: *b"TEXT",
        creator: *b"ttxt",
    }
}

fn listing(path: &[u8]) -> Asked {
    listing_for(PROVIDER, path)
}

fn listing_for(provider: *mut c_void, path: &[u8]) -> Asked {
    Asked::Listing {
        provider: unsafe { Provider::new(provider) },
        path: CString::new(path).unwrap(),
    }
}

/// A listing reaches the provider that asked for it, with the folder it
/// asked about, only on its own connection and trans, and only once.
#[test]
fn a_listing_reaches_only_what_asked_for_it() {
    asked(A, 5, listing(b"/caf\x8e"));
    unsafe {
        listed(B, 5, &[entry(b"x")]);
        listed(A, 6, &[entry(b"x")]);
    }
    assert!(test_env::take().is_empty());
    unsafe { listed(A, 5, &[entry(b"a"), entry(b"b\x8e")]) };
    assert_eq!(
        test_env::take(),
        [(
            PROVIDER as usize,
            b"/caf\x8e".to_vec(),
            Some(vec![b"a".to_vec(), b"b\x8e".to_vec()])
        )]
    );
    unsafe { listed(A, 5, &[]) };
    assert!(test_env::take().is_empty());
}

#[test]
fn a_refused_listing_tells_the_provider_which_folder() {
    asked(A, 7, listing(b"/Drop Box"));
    unsafe { failed(A, 7) };
    assert_eq!(
        test_env::take(),
        [(PROVIDER as usize, b"/Drop Box".to_vec(), None)]
    );
}

/// A new login numbers its requests afresh: what the last one asked for on
/// that connection is let go, and another connection's is kept.
#[test]
fn forgetting_a_connection_lets_go_of_only_its_requests() {
    asked(A, 9, listing(b"/a"));
    asked(B, 9, listing(b"/b"));
    forget(A);
    unsafe {
        listed(A, 9, &[]);
        listed(B, 9, &[]);
    }
    assert_eq!(
        test_env::take(),
        [(PROVIDER as usize, b"/b".to_vec(), Some(vec![]))]
    );
}

/// A provider that asks again has left the folder it asked for first: that
/// reply is not shown, on any connection.
#[test]
fn a_new_listing_replaces_the_provider_s_last() {
    const OTHER: *mut c_void = 0x07E5 as *mut c_void;
    asked(A, 1, listing(b"/old"));
    asked(A, 2, listing_for(OTHER, b"/other"));
    asked(B, 3, listing(b"/new"));
    unsafe {
        listed(A, 1, &[]);
        listed(A, 2, &[]);
        listed(B, 3, &[]);
    }
    assert_eq!(
        test_env::take(),
        [
            (OTHER as usize, b"/other".to_vec(), Some(vec![])),
            (PROVIDER as usize, b"/new".to_vec(), Some(vec![])),
        ]
    );
}
