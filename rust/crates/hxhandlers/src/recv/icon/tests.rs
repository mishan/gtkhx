//! Headless signal-behaviour tests for the icon-change handler, driven through
//! the `test_env` doubles for the parse / emit C ABIs.

use super::*;

fn recv() {
    // buf/len/htlc are opaque to the doubles; the parse result is driven by
    // test_env::PARSE_UID.
    unsafe { hx_icon_change_recv(std::ptr::null_mut(), std::ptr::null(), 0) };
}

#[test]
fn emits_gif_icon_changed_with_parsed_uid() {
    test_env::reset();
    test_env::PARSE_UID.with(|c| c.set(Some(4242)));

    recv();

    assert_eq!(test_env::EMITTED.with(|c| c.take()), Some(4242));
}

#[test]
fn drops_frame_with_no_uid() {
    test_env::reset();
    test_env::PARSE_UID.with(|c| c.set(None)); // malformed / uid absent

    recv();

    assert_eq!(test_env::EMITTED.with(|c| c.take()), None);
}

/// A real, GIF-signed payload forwards verbatim (non-null ptr, same len).
#[test]
fn valid_gif_forwards_bytes() {
    test_env::reset();
    test_env::IS_GIF.with(|c| c.set(true));
    let bytes = [0u8; 16];
    unsafe { hx_icon_data_recv(std::ptr::null_mut(), 7, bytes.as_ptr(), 16) };
    assert_eq!(
        test_env::DATA_EMITTED.with(|c| c.take()),
        Some((7, /*ptr_is_null=*/ false, 16))
    );
}

/// A zero-length payload is a cleared avatar → forward (NULL, 0), never
/// consulting the signature check.
#[test]
fn empty_payload_forwards_cleared() {
    test_env::reset();
    unsafe { hx_icon_data_recv(std::ptr::null_mut(), 7, std::ptr::null(), 0) };
    assert_eq!(
        test_env::DATA_EMITTED.with(|c| c.take()),
        Some((7, /*ptr_is_null=*/ true, 0))
    );
}

/// A non-empty payload failing the GIF signature is coerced to cleared.
#[test]
fn non_gif_payload_coerced_to_cleared() {
    test_env::reset();
    test_env::IS_GIF.with(|c| c.set(false));
    let bytes = [0xFFu8; 8];
    unsafe { hx_icon_data_recv(std::ptr::null_mut(), 9, bytes.as_ptr(), 8) };
    assert_eq!(
        test_env::DATA_EMITTED.with(|c| c.take()),
        Some((9, /*ptr_is_null=*/ true, 0))
    );
}

// ---- the replies ---------------------------------------------------------

const GIF87: &[u8] = b"GIF87a\x00\x00";
const HTLC: *mut std::os::raw::c_void = 0x10 as *mut _;

fn icon_of(uid: u16, gif: &[u8]) -> Icon {
    Icon {
        uid,
        gif: gif.to_vec(),
    }
}

/// A user's icon says the server has GIF icons, and is published.
#[test]
fn an_icon_marks_the_server_capable_and_is_published() {
    test_env::reset();
    unsafe { icon(HTLC, &icon_of(7, GIF87)) };
    assert_eq!(test_env::STATE.with(|c| c.get()), GIF_ICONS_SUPPORTED);
    assert_eq!(
        test_env::DATA_EMITTED.with(|c| c.take()),
        Some((7, /*ptr_is_null=*/ false, GIF87.len() as u32))
    );
}

/// The listing settles the probe: capable, watchdog disarmed, our saved
/// avatar sent, every listed icon published, an empty one as cleared.
#[test]
fn the_icon_list_settles_the_probe_and_publishes_each_icon() {
    test_env::reset();
    test_env::PROBE_TIMER.with(|c| c.set(99));
    asked(HTLC, 4, Asked::Probe);
    unsafe {
        listed(
            HTLC,
            4,
            &[icon_of(1, GIF87), icon_of(2, GIF87), icon_of(3, b"")],
        )
    };
    assert_eq!(test_env::STATE.with(|c| c.get()), GIF_ICONS_SUPPORTED);
    assert_eq!(test_env::SOURCE_REMOVED.with(|c| c.get()), Some(99));
    assert_eq!(test_env::PROBE_TIMER.with(|c| c.get()), 0);
    assert!(test_env::SEND_SAVED.with(|c| c.get()));
    assert_eq!(test_env::DATA_COUNT.with(|c| c.get()), 3);
    assert_eq!(
        test_env::DATA_EMITTED.with(|c| c.get()),
        Some((3, /*ptr_is_null=*/ true, 0))
    );
    // Answered: a later failure on the trans is no longer the probe's.
    assert!(!unsafe { failed(HTLC, 4, None) });
}

/// Which refusals the user hears of: not the probe's, which marks the
/// server as without GIF icons, nor the saved avatar's, which is logged;
/// anything else, yes. Each only on the connection and trans it went out on.
#[test]
fn only_the_requests_the_user_made_are_refused_aloud() {
    for (what, reason, quiet, state, logged) in [
        (Some(Asked::Probe), Some("Unknown transaction"), true, GIF_ICONS_UNSUPPORTED, None),
        (
            Some(Asked::Saved),
            Some("guests can't \u{2014} sorry"),
            true,
            0,
            Some("server refused the saved avatar: guests can't \u{2014} sorry; not re-sending on this connection"),
        ),
        (
            Some(Asked::Saved),
            None,
            true,
            0,
            Some("server refused the saved avatar; not re-sending on this connection"),
        ),
        (None, Some("Not allowed."), false, 0, None),
    ] {
        test_env::reset();
        test_env::PROBE_TIMER.with(|c| c.set(42));
        if let Some(what) = what {
            asked(HTLC, 9, what);
        }
        let other = 0x20 as *mut std::os::raw::c_void;
        assert!(!unsafe { failed(other, 9, reason) }, "{what:?}");
        assert_eq!(unsafe { failed(HTLC, 9, reason) }, quiet, "{what:?}");
        assert_eq!(test_env::STATE.with(|c| c.get()), state, "{what:?}");
        let lines = test_env::DEBUG_LINES.with(|c| c.take());
        let lines: Vec<_> = lines.iter().map(|(c, l)| (c.as_str(), l.as_str())).collect();
        assert_eq!(lines, logged.map(|l| ("icon", l)).into_iter().collect::<Vec<_>>());
        assert!(!test_env::SEND_SAVED.with(|c| c.get()));
    }
}

/// What a closed connection asked for is let go of.
#[test]
fn a_forgotten_probe_is_no_longer_quiet() {
    test_env::reset();
    asked(HTLC, 5, Asked::Probe);
    forget(HTLC);
    assert!(!unsafe { failed(HTLC, 5, None) });
}
