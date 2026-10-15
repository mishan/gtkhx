//! The identity a connection presents: its nickname and icon, resolved and
//! copied onto the connection.
//!
//! The nickname and icon preferences used to *be* the connection's wire name
//! buffer and icon field. They are ordinary settings with their own storage
//! now, and this is the one-way copy that replaces the aliasing.
//!
//! Resolution is `override ?? global ?? startup`, three deep because each
//! level answers a different question:
//!
//! - **override**: what this connection is configured to show, if anything —
//!   a bookmark's nickname or icon, armed for one connect.
//! - **global**: what everything unspecialized shows.
//! - **startup**: what a profile that has never set a nickname shows: `$USER`,
//!   recorded before the settings file is read, so a reconnect can restore it
//!   after a `/nick`.
//!
//! Per field: a connection whose nickname came from an override, or was set
//! on it alone with `/nick`, has a nickname of its own, and a change to the
//! global nickname in Settings passes it by; the icon likewise, with `/icon`.
//! Its next connect without that override makes it follow the global again.

use std::cell::RefCell;
use std::ffi::{c_char, c_int, CStr};

use gtk4::glib;
use gtkhx_core::conn::{
    hx_conn_own_icon, hx_conn_own_nick, hx_conn_set_icon, hx_conn_set_name, hx_conn_set_own_icon,
    hx_conn_set_own_nick, HtlcConn,
};

#[derive(Default)]
struct State {
    pending_nick: Option<String>,
    pending_icon: Option<u16>,
    startup_nick: String,
}

thread_local! {
    static STATE: RefCell<State> = RefCell::new(State::default());
}

/// What a connect puts on a connection.
#[derive(Debug, PartialEq, Eq)]
struct Resolved {
    /// `None` leaves the connection's name as it is: nothing at any level.
    nick: Option<String>,
    icon: u16,
    /// Whether each field is now the connection's own.
    own_nick: bool,
    own_icon: bool,
}

fn resolve(
    pending_nick: Option<&str>,
    pending_icon: Option<u16>,
    global_nick: &str,
    global_icon: u16,
    startup_nick: &str,
) -> Resolved {
    let nick = pending_nick
        .or((!global_nick.is_empty()).then_some(global_nick))
        .or((!startup_nick.is_empty()).then_some(startup_nick))
        .map(str::to_owned);
    Resolved {
        nick,
        icon: pending_icon.unwrap_or(global_icon),
        own_nick: pending_nick.is_some(),
        own_icon: pending_icon.is_some(),
    }
}

fn global() -> (String, u16) {
    hxconfig::ffi::with_settings(|s| (s.identity.nick.clone(), s.identity.icon)).unwrap_or_default()
}

unsafe fn put_nick(htlc: *mut HtlcConn, nick: &Option<String>) {
    if let Some(c) = nick.as_deref().and_then(|n| std::ffi::CString::new(n).ok()) {
        hx_conn_set_name(htlc, c.as_ptr());
    }
}

/// `void hx_identity_set_startup_default (const char *nick)`.
///
/// # Safety
/// `nick` is NULL or a C string. Main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_identity_set_startup_default(nick: *const c_char) {
    STATE.with_borrow_mut(|s| s.startup_nick = crate::cstr(nick));
}

/// `void hx_identity_set_pending_override (const char *nick, int icon)` —
/// arm an override for the next connect. A NULL or empty nick and a negative
/// icon each mean "no override"; zero cannot, being a real, blank icon.
///
/// # Safety
/// `nick` is NULL or a C string. Main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_identity_set_pending_override(nick: *const c_char, icon: c_int) {
    let nick = (!nick.is_null())
        .then(|| CStr::from_ptr(nick).to_string_lossy().into_owned())
        .filter(|n| !n.is_empty());
    STATE.with_borrow_mut(|s| {
        s.pending_nick = nick;
        s.pending_icon = u16::try_from(icon).ok();
    });
}

/// `void hx_identity_apply (struct htlc_conn *htlc)` — resolve the identity
/// for a connect and put it on `htlc`.
///
/// One connect, one override: this consumes it, so a bookmark's nickname
/// can't leak onto a later `/server` or a tracker double-click.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_identity_apply(htlc: *mut HtlcConn) {
    let (nick, icon) = global();
    let r = STATE.with_borrow_mut(|s| {
        let r = resolve(
            s.pending_nick.as_deref(),
            s.pending_icon,
            &nick,
            icon,
            &s.startup_nick,
        );
        s.pending_nick = None;
        s.pending_icon = None;
        r
    });
    put_nick(htlc, &r.nick);
    hx_conn_set_icon(htlc, r.icon);
    hx_conn_set_own_nick(htlc, r.own_nick.into());
    hx_conn_set_own_icon(htlc, r.own_icon.into());
}

/// `HX_IDENTITY_NICK` / `HX_IDENTITY_ICON` (`options.h`).
const FIELD_NICK: c_int = 1;
const FIELD_ICON: c_int = 2;

/// `gboolean hx_identity_apply_global (struct htlc_conn *htlc, int fields)` —
/// put the global nickname and/or icon on `htlc`, each only if the
/// connection follows it, for the load and a Settings change. TRUE when it
/// applied any, so the caller knows whether there is anything to tell the
/// server.
///
/// Touches neither an armed override nor which fields are the connection's
/// own.
///
/// # Safety
/// `htlc` is a live connection. Main thread.
#[no_mangle]
pub unsafe extern "C" fn hx_identity_apply_global(
    htlc: *mut HtlcConn,
    fields: c_int,
) -> glib::ffi::gboolean {
    let (nick, icon) = global();
    let r = STATE.with_borrow(|s| resolve(None, None, &nick, icon, &s.startup_nick));
    let nick = fields & FIELD_NICK != 0 && hx_conn_own_nick(htlc) == glib::ffi::GFALSE;
    let icon = fields & FIELD_ICON != 0 && hx_conn_own_icon(htlc) == glib::ffi::GFALSE;
    if nick {
        put_nick(htlc, &r.nick);
    }
    if icon {
        hx_conn_set_icon(htlc, r.icon);
    }
    (nick || icon).into()
}

#[cfg(test)]
mod tests {
    use super::*;
    use gtkhx_core::conn::{hx_conn_icon, hx_conn_name, hx_conn_new};

    #[test]
    fn resolves_override_then_global_then_startup() {
        let cases = [
            // (pending nick, pending icon, global nick) → (nick, icon, own nick, own icon)
            (
                Some("Alice"),
                None,
                "Global",
                (Some("Alice"), 410, true, false),
            ),
            (None, Some(7), "Global", (Some("Global"), 7, false, true)),
            (
                Some("Alice"),
                Some(7),
                "Global",
                (Some("Alice"), 7, true, true),
            ),
            (None, None, "Global", (Some("Global"), 410, false, false)),
            (None, None, "", (Some("user"), 410, false, false)),
        ];
        for (pnick, picon, gnick, (nick, icon, own_nick, own_icon)) in cases {
            assert_eq!(
                resolve(pnick, picon, gnick, 410, "user"),
                Resolved {
                    nick: nick.map(str::to_owned),
                    icon,
                    own_nick,
                    own_icon
                },
                "{pnick:?} {picon:?} {gnick:?}"
            );
        }
        assert_eq!(resolve(None, None, "", 0, "").nick, None);
    }

    fn name(htlc: *const HtlcConn) -> String {
        unsafe { CStr::from_ptr(hx_conn_name(htlc)) }
            .to_string_lossy()
            .into_owned()
    }

    #[test]
    fn a_global_change_reaches_only_the_fields_a_connection_follows() {
        let htlc = hx_conn_new();
        unsafe {
            hx_identity_set_startup_default(c"user".as_ptr());

            const BOTH: c_int = FIELD_NICK | FIELD_ICON;

            // A nick-only override keeps its nick, and a nickname change has
            // nothing to send; it still follows the icon.
            hx_identity_set_pending_override(c"Alice".as_ptr(), -1);
            hx_identity_apply(htlc);
            hx_conn_set_icon(htlc, 999);
            assert_eq!(
                hx_identity_apply_global(htlc, FIELD_NICK),
                glib::ffi::GFALSE
            );
            assert_ne!(
                hx_identity_apply_global(htlc, FIELD_ICON),
                glib::ffi::GFALSE
            );
            assert_eq!((name(htlc), hx_conn_icon(htlc)), ("Alice".into(), 0));

            // Both its own: nothing to apply.
            hx_conn_set_own_icon(htlc, glib::ffi::GTRUE);
            hx_conn_set_icon(htlc, 999);
            assert_eq!(hx_identity_apply_global(htlc, BOTH), glib::ffi::GFALSE);
            assert_eq!(hx_conn_icon(htlc), 999);

            // The override was one connect's; the next one follows again.
            hx_identity_apply(htlc);
            assert_ne!(
                hx_identity_apply_global(htlc, FIELD_NICK),
                glib::ffi::GFALSE
            );
            assert_eq!(name(htlc), "user");

            // A Settings change doesn't consume an armed override.
            hx_identity_set_pending_override(std::ptr::null(), 9);
            hx_identity_apply_global(htlc, BOTH);
            hx_identity_apply(htlc);
            assert_eq!(hx_conn_icon(htlc), 9);

            gtkhx_core::conn::hx_conn_free(htlc);
        }
    }
}
