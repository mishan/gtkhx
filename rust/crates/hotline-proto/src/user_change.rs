//! Pure decision layer for the `USER_CHANGE` receive handler (ported from
//! `proto_helpers.c`'s `hx_user_change_plan_resolve`).
//!
//! Given a parsed `USER_CHANGE` and the *existing* member state (from the
//! per-chat `HxMemberModel`), decide what `hx_rcv_user_change` should do — with
//! no side effects. The C handler applies the plan (emit create/change, adopt
//! self uid, print the rename notice, mirror onto `htlc`). Keeping the decision
//! here makes the fiddly rename / colour-preserve / self-detect logic testable
//! without the GUI signal machinery.

use std::os::raw::{c_char, c_int};

/// "No nick colour" sentinel — 0xFFFFFFFF (equivalently, the DATA_COLOR chunk
/// being absent): use the theme default. A real Colored-Nicknames colour is
/// 0x00RRGGBB, so the all-ones sentinel can't collide with one. Mirrors
/// `HX_NICK_COLOR_NONE` (src/hotline.h).
const HX_NICK_COLOR_NONE: u32 = 0xffff_ffff;

/// `#[repr(C)]` mirror of the C `struct hx_user_change_msg` (proto_helpers.h).
/// The whole layout is mirrored so the C caller passes its struct straight
/// through; the `_Static_assert`s in `proto_helpers.c` pin the layout so this
/// mirror can't silently drift.
#[repr(C)]
pub struct HxUserChangeMsg {
    pub uid: u16,
    pub icon: u16,
    pub color: u16,
    pub got_color: c_int,
    pub nick_color: u32,
    pub got_nick_color: c_int,
    pub cid: u32,
    pub name: [c_char; 32],
    pub name_len: u16,
}

/// `#[repr(C)]` mirror of the C `struct hx_user_change_plan` (proto_helpers.h).
#[repr(C)]
pub struct HxUserChangePlan {
    pub adopt_self_uid: c_int,
    pub is_self: c_int,
    pub is_new: c_int,
    pub skip_self_create: c_int,
    pub do_rename_notice: c_int,
    pub eff_color: u16,
    pub eff_nick_color: u32,
}

/// Rust-native inputs to [`resolve`].
pub struct ChangeInput<'a> {
    /// The change's uid.
    pub uid: u16,
    /// The change's name (exactly `name_len` bytes; may be empty).
    pub name: &'a [u8],
    /// Whether the wire carried a status-colour chunk.
    pub got_color: bool,
    /// The wire status bitmap (only meaningful when `got_color`).
    pub color: u16,
    /// Whether the wire carried an RGB nick-colour chunk.
    pub got_nick_color: bool,
    /// The wire RGB nick colour (only meaningful when `got_nick_color`).
    pub nick_color: u32,
    /// TRUE if the member is already known (change vs create).
    pub old_exists: bool,
    /// The member's current status bitmap (preserved when the wire omits it).
    pub old_status: u16,
    /// The member's current RGB nick colour (preserved when the wire omits it).
    pub old_nick_color: u32,
    /// The member's current display name (for rename detection); `None` when
    /// the member is new.
    pub old_name: Option<&'a [u8]>,
    /// `htlc->uid` (0 if the server hasn't told us yet).
    pub self_uid: u16,
    /// Our local nick, for the SELFINFO-less self-detection some 1.9 servers
    /// force; `None` when unknown.
    pub self_name: Option<&'a [u8]>,
}

/// Rust-native output of [`resolve`].
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ChangePlan {
    /// Caller should set `htlc->uid = uid`.
    pub adopt_self_uid: bool,
    /// The change is about us.
    pub is_self: bool,
    /// Create (true) vs change (false).
    pub is_new: bool,
    /// `is_new && is_self` — don't add our own row.
    pub skip_self_create: bool,
    /// Print "X is now known as Y".
    pub do_rename_notice: bool,
    /// Status bitmap to render/store.
    pub eff_color: u16,
    /// RGB nick colour to render/store.
    pub eff_nick_color: u32,
}

/// The pure decision — mirrors `proto_helpers.c::hx_user_change_plan_resolve`.
pub fn resolve(inp: &ChangeInput) -> ChangePlan {
    let mut out = ChangePlan::default();

    // SELFINFO-less self-detection: some 1.9-style servers (The Mobius Strip)
    // omit USER_LIST from SELFINFO, so self_uid stays 0 after login. The first
    // USER_CHANGE is the server echoing our own post-SELFINFO USER_CHANGE back —
    // its name matches our nick and carries our freshly-assigned uid. Adopt it.
    let mut eff_self = inp.self_uid;
    if inp.self_uid == 0 && inp.uid != 0 && !inp.name.is_empty() && inp.self_name == Some(inp.name)
    {
        out.adopt_self_uid = true;
        eff_self = inp.uid;
    }

    out.is_self = inp.uid != 0 && inp.uid == eff_self;
    out.is_new = !inp.old_exists;
    // Don't add our own row on a create — the USER_LIST reply (or a later
    // broadcast) creates it in the right position; adding it here would put us
    // at the top and spam a "join: <us>" line.
    out.skip_self_create = out.is_new && out.is_self;

    // Colour: the wire value wins when present; otherwise keep the member's
    // current status (a rename-only USER_CHANGE mustn't reset it). A new member
    // has no old status, so it takes the wire value (possibly 0).
    out.eff_color = if inp.got_color {
        inp.color
    } else if inp.old_exists {
        inp.old_status
    } else {
        inp.color
    };
    // RGB nick colour: same preserve rule; a new member with no wire colour
    // defaults to "none".
    out.eff_nick_color = if inp.got_nick_color {
        inp.nick_color
    } else if inp.old_exists {
        inp.old_nick_color
    } else {
        HX_NICK_COLOR_NONE
    };

    // "X is now known as Y" only on a real rename of someone other than us.
    out.do_rename_notice = inp.old_exists
        && inp.uid != eff_self
        && !inp.name.is_empty()
        && matches!(inp.old_name, Some(old) if old != inp.name);

    out
}

/// `void hx_user_change_plan_resolve (const struct hx_user_change_msg *uc,
/// gboolean old_exists, guint16 old_status, guint32 old_nick_color,
/// const char *old_name, guint16 self_uid, const char *self_name,
/// struct hx_user_change_plan *out)` — the C ABI `hx_rcv_user_change` +
/// `test_user_change.c` call. Marshals the `#[repr(C)]` structs into [`resolve`].
///
/// A NULL `out` is a no-op; a NULL `uc` leaves `*out` fully zeroed (a safe
/// no-op plan) so a caller that ignores the failure still reads defined fields.
///
/// # Safety
/// `uc` is NULL or points to a valid `HxUserChangeMsg`; `old_name` / `self_name`
/// are NULL or valid C strings; `out` is NULL or points to a writable
/// `HxUserChangePlan`.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn hx_user_change_plan_resolve(
    uc: *const HxUserChangeMsg,
    old_exists: c_int,
    old_status: u16,
    old_nick_color: u32,
    old_name: *const c_char,
    self_uid: u16,
    self_name: *const c_char,
    out: *mut HxUserChangePlan,
) {
    let Some(out) = out.as_mut() else {
        return;
    };
    // Zeroed plan is a safe no-op; write it first so a NULL-uc early return
    // still leaves *out well-defined.
    *out = HxUserChangePlan {
        adopt_self_uid: 0,
        is_self: 0,
        is_new: 0,
        skip_self_create: 0,
        do_rename_notice: 0,
        eff_color: 0,
        eff_nick_color: 0,
    };
    let Some(uc) = uc.as_ref() else {
        return;
    };

    let name_len = (uc.name_len as usize).min(uc.name.len());
    let name = std::slice::from_raw_parts(uc.name.as_ptr() as *const u8, name_len);

    let plan = resolve(&ChangeInput {
        uid: uc.uid,
        name,
        got_color: uc.got_color != 0,
        color: uc.color,
        got_nick_color: uc.got_nick_color != 0,
        nick_color: uc.nick_color,
        old_exists: old_exists != 0,
        old_status,
        old_nick_color,
        old_name: cstr_opt(old_name),
        self_uid,
        self_name: cstr_opt(self_name),
    });

    out.adopt_self_uid = c_int::from(plan.adopt_self_uid);
    out.is_self = c_int::from(plan.is_self);
    out.is_new = c_int::from(plan.is_new);
    out.skip_self_create = c_int::from(plan.skip_self_create);
    out.do_rename_notice = c_int::from(plan.do_rename_notice);
    out.eff_color = plan.eff_color;
    out.eff_nick_color = plan.eff_nick_color;
}

/// Borrow a NUL-terminated C string as bytes (without the NUL), or `None` if the
/// pointer is NULL.
unsafe fn cstr_opt<'a>(p: *const c_char) -> Option<&'a [u8]> {
    if p.is_null() {
        None
    } else {
        Some(std::ffi::CStr::from_ptr(p).to_bytes())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A base input: a colour-carrying change for a *new* user, with a known
    /// self (uid 5, "Me"). Tests override just the fields they exercise via
    /// struct-update syntax.
    fn base(uid: u16, name: &[u8]) -> ChangeInput<'_> {
        ChangeInput {
            uid,
            name,
            got_color: true,
            color: 3,
            got_nick_color: false,
            nick_color: 0,
            old_exists: false,
            old_status: 0,
            old_nick_color: HX_NICK_COLOR_NONE,
            old_name: None,
            self_uid: 5,
            self_name: Some(b"Me"),
        }
    }

    #[test]
    fn new_user_is_create() {
        let p = resolve(&base(42, b"Bob"));
        assert!(p.is_new);
        assert!(!p.is_self);
        assert!(!p.skip_self_create);
        assert!(!p.do_rename_notice);
        assert!(!p.adopt_self_uid);
        assert_eq!(p.eff_color, 3);
        assert_eq!(p.eff_nick_color, HX_NICK_COLOR_NONE);
    }

    #[test]
    fn rename_fires_notice() {
        let p = resolve(&ChangeInput {
            old_exists: true,
            old_status: 3,
            old_name: Some(b"Bob"),
            ..base(42, b"Bobby")
        });
        assert!(!p.is_new);
        assert!(p.do_rename_notice);
    }

    #[test]
    fn same_name_no_notice() {
        // An icon/colour-only USER_CHANGE (name unchanged) must not print
        // "X is now known as X".
        let p = resolve(&ChangeInput {
            old_exists: true,
            old_status: 3,
            old_name: Some(b"Bob"),
            ..base(42, b"Bob")
        });
        assert!(!p.do_rename_notice);
    }

    #[test]
    fn color_preserved_when_absent() {
        // !got_color on an existing user → keep the old status bitmap.
        let p = resolve(&ChangeInput {
            got_color: false,
            color: 0,
            old_exists: true,
            old_status: 2,
            old_name: Some(b"Bob"),
            ..base(42, b"Bob")
        });
        assert_eq!(p.eff_color, 2);
    }

    #[test]
    fn color_taken_when_present() {
        let p = resolve(&ChangeInput {
            color: 1,
            old_exists: true,
            old_status: 2,
            old_name: Some(b"Bob"),
            ..base(42, b"Bob")
        });
        assert_eq!(p.eff_color, 1);
    }

    #[test]
    fn nick_color_preserved_when_absent() {
        // !got_nick_color on an existing user → keep the old RGB colour (a
        // rename mustn't wipe someone's custom colour).
        let p = resolve(&ChangeInput {
            old_exists: true,
            old_status: 3,
            old_nick_color: 0x00ab_cdef,
            old_name: Some(b"Bob"),
            ..base(42, b"Bob")
        });
        assert_eq!(p.eff_nick_color, 0x00ab_cdef);
    }

    #[test]
    fn nick_color_taken_when_present() {
        let p = resolve(&ChangeInput {
            got_nick_color: true,
            nick_color: 0x0011_2233,
            old_exists: true,
            old_status: 3,
            old_nick_color: 0x00ab_cdef,
            old_name: Some(b"Bob"),
            ..base(42, b"Bob")
        });
        assert_eq!(p.eff_nick_color, 0x0011_2233);
    }

    #[test]
    fn self_change_no_rename() {
        // uid == self_uid → is_self, and never a rename notice about ourselves.
        let p = resolve(&ChangeInput {
            uid: 5,
            old_exists: true,
            old_status: 3,
            old_name: Some(b"Me"),
            ..base(5, b"MeNew")
        });
        assert!(p.is_self);
        assert!(!p.do_rename_notice);
    }

    #[test]
    fn self_uid_adoption() {
        // self_uid==0 (SELFINFO didn't carry it) + the broadcast name matches
        // our nick → adopt uc.uid as ours.
        let p = resolve(&ChangeInput {
            self_uid: 0,
            ..base(77, b"Me")
        });
        assert!(p.adopt_self_uid);
        assert!(p.is_self);
        assert!(p.skip_self_create); // new + self → don't add our own row
    }

    #[test]
    fn no_self_adoption_on_name_mismatch() {
        let p = resolve(&ChangeInput {
            self_uid: 0,
            ..base(77, b"Stranger")
        });
        assert!(!p.adopt_self_uid);
        assert!(!p.is_self);
        assert!(!p.skip_self_create);
    }

    // ---- FFI boundary (repr(C) marshalling + NULL-safety) -------------------

    fn mk_msg(uid: u16, name: &[u8], color: u16, got_color: bool) -> HxUserChangeMsg {
        let mut buf = [0 as std::os::raw::c_char; 32];
        for (i, &b) in name.iter().take(31).enumerate() {
            buf[i] = b as std::os::raw::c_char;
        }
        HxUserChangeMsg {
            uid,
            icon: 412,
            color,
            got_color: c_int::from(got_color),
            nick_color: HX_NICK_COLOR_NONE,
            got_nick_color: 0,
            cid: 0,
            name: buf,
            name_len: name.len().min(31) as u16,
        }
    }

    #[test]
    fn ffi_round_trips_a_rename() {
        // Drive the real C-ABI entry point through the repr(C) structs.
        let uc = mk_msg(42, b"Bobby", 3, true);
        let old = std::ffi::CString::new("Bob").unwrap();
        let me = std::ffi::CString::new("Me").unwrap();
        let mut out: HxUserChangePlan = unsafe { std::mem::zeroed() };
        unsafe {
            hx_user_change_plan_resolve(
                &uc,
                /*old_exists=*/ 1,
                /*old_status=*/ 3,
                HX_NICK_COLOR_NONE,
                old.as_ptr(),
                /*self_uid=*/ 5,
                me.as_ptr(),
                &mut out,
            );
        }
        assert_eq!(out.is_new, 0);
        assert_eq!(out.do_rename_notice, 1);
        assert_eq!(out.eff_color, 3);
    }

    #[test]
    fn ffi_null_args_are_safe() {
        // NULL uc must leave *out fully zeroed (a safe no-op plan); NULL out
        // must not crash.
        let mut out = HxUserChangePlan {
            adopt_self_uid: 1,
            is_self: 1,
            is_new: 1,
            skip_self_create: 1,
            do_rename_notice: 1,
            eff_color: 9,
            eff_nick_color: 9,
        };
        let x = std::ffi::CString::new("X").unwrap();
        unsafe {
            hx_user_change_plan_resolve(
                std::ptr::null(),
                1,
                0,
                0,
                x.as_ptr(),
                1,
                x.as_ptr(),
                &mut out,
            );
        }
        assert_eq!(out.is_self, 0);
        assert_eq!(out.is_new, 0);
        assert_eq!(out.skip_self_create, 0);
        assert_eq!(out.do_rename_notice, 0);
        assert_eq!(out.adopt_self_uid, 0);
        assert_eq!(out.eff_color, 0);
        assert_eq!(out.eff_nick_color, 0);

        let uc = mk_msg(1, b"X", 0, false);
        unsafe {
            // NULL out — must not crash.
            hx_user_change_plan_resolve(
                &uc,
                1,
                0,
                0,
                x.as_ptr(),
                1,
                x.as_ptr(),
                std::ptr::null_mut(),
            );
        }
    }
}
