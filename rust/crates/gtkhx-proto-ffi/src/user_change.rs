//! C ABI for `hxproto::user_change`, the `USER_CHANGE` receive decision.

use std::os::raw::{c_char, c_int};

use hxproto::user_change::{resolve, ChangeInput};

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

/// `void hx_user_change_plan_resolve (const struct hx_user_change_msg *uc,
/// gboolean old_exists, guint16 old_status, guint32 old_nick_color,
/// const char *old_name, guint16 self_uid, const char *self_name,
/// struct hx_user_change_plan *out)` — the C ABI `hx_rcv_user_change` +
/// `test_user_change.c` call. Marshals the `#[repr(C)]` structs into
/// `hxproto::user_change::resolve`.
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
    use hxproto::messages::NICK_COLOR_NONE;

    fn mk_msg(uid: u16, name: &[u8], color: u16, got_color: bool) -> HxUserChangeMsg {
        let mut buf = [0 as c_char; 32];
        for (i, &b) in name.iter().take(31).enumerate() {
            buf[i] = b as c_char;
        }
        HxUserChangeMsg {
            uid,
            icon: 412,
            color,
            got_color: c_int::from(got_color),
            nick_color: NICK_COLOR_NONE,
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
                NICK_COLOR_NONE,
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
