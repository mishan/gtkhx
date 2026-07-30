//! Lifecycle tests over the real GHashTable + the crate's CRUD, driven through
//! the `TestSession` C-environment doubles. Ports the coverage of the retired
//! `tests/unit/test_task_hash.c` and adds `task_new` / `task_delete` end-to-end
//! (which the C test couldn't reach without the GTK pile).

// `s.trans = <id>` is read back inside `task_new` through the raw `htlc`
// pointer that aliases `s`, which the lint can't see — hence the false
// "value never read" on every trans assignment.
#![allow(unused_assignments)]

use super::*;
use std::os::raw::c_void;
use std::sync::atomic::{AtomicUsize, Ordering};

fn new_sess() -> TestSession {
    TestSession {
        tasks: std::ptr::null_mut(),
        trans: 0,
    }
}

/// A non-NULL `ptr` context the counting `ptr_free` callbacks never dereference
/// (they only tick a counter). Non-NULL so `task_free`'s `ptr && ptr_free`
/// guard fires.
fn sentinel_ptr() -> *mut c_void {
    std::ptr::NonNull::<u8>::dangling().as_ptr() as *mut c_void
}

/// `&mut TestSession` → the `(session*, htlc*)` pair the C ABI takes (both the
/// same pointer under the test doubles).
fn ptrs(s: &mut TestSession) -> (*mut Session, *mut HtlcConn) {
    let p = s as *mut TestSession;
    (p as *mut Session, p as *mut HtlcConn)
}

#[test]
fn init_is_idempotent() {
    unsafe {
        let mut s = new_sess();
        let (sp, _) = ptrs(&mut s);
        tasks_init(sp);
        let t1 = hx_session_tasks(sp);
        assert!(!t1.is_null());
        tasks_init(sp); // second call must not rebuild
        assert_eq!(hx_session_tasks(sp), t1);
        glib::ffi::g_hash_table_destroy(t1);
    }
}

#[test]
fn new_lookup_delete_roundtrip() {
    unsafe {
        let mut s = new_sess();
        let (sp, hp) = ptrs(&mut s);
        tasks_init(sp);

        s.trans = 0x1001;
        let tsk = task_new(
            hp,
            None,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            c"login".as_ptr(),
        );
        assert!(!tsk.is_null());
        assert_eq!((*tsk).trans, 0x1001);
        assert_eq!((*tsk).len, 1);
        // str was copied (g_strdup), not aliased.
        assert!(!(*tsk).str_.is_null());

        assert_eq!(task_with_trans(sp, 0x1001), tsk);
        assert!(task_with_trans(sp, 0x9999).is_null()); // unknown → NULL

        task_delete(sp, tsk);
        assert!(task_with_trans(sp, 0x1001).is_null());

        glib::ffi::g_hash_table_destroy(hx_session_tasks(sp));
    }
}

#[test]
fn trans_zero_is_a_real_key() {
    unsafe {
        let mut s = new_sess();
        let (sp, hp) = ptrs(&mut s);
        tasks_init(sp);
        s.trans = 0; // first transaction on a fresh connection
        let tsk = task_new(
            hp,
            None,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null(),
        );
        assert_eq!(task_with_trans(sp, 0), tsk);
        assert_eq!(glib::ffi::g_hash_table_size(hx_session_tasks(sp)), 1);
        glib::ffi::g_hash_table_destroy(hx_session_tasks(sp));
    }
}

#[test]
fn null_str_leaves_null_label() {
    unsafe {
        let mut s = new_sess();
        let (sp, hp) = ptrs(&mut s);
        tasks_init(sp);
        s.trans = 7;
        let tsk = task_new(
            hp,
            None,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null(),
        );
        assert!((*tsk).str_.is_null());
        glib::ffi::g_hash_table_destroy(hx_session_tasks(sp));
    }
}

// --- task_free lifecycle: prove ptr_free fires (own counter, no cross-test share) ---

static DUP_FREED: AtomicUsize = AtomicUsize::new(0);
unsafe extern "C" fn count_dup(_p: gpointer) {
    DUP_FREED.fetch_add(1, Ordering::SeqCst);
}

#[test]
fn duplicate_trans_replaces_and_frees_old() {
    unsafe {
        DUP_FREED.store(0, Ordering::SeqCst);
        let mut s = new_sess();
        let (sp, hp) = ptrs(&mut s);
        tasks_init(sp);
        s.trans = 0x4242;

        // Register a task with an owned ptr context + destructor (mirrors how a
        // real caller sets ptr_free after task_new). ptr is a non-NULL sentinel
        // count_dup never dereferences.
        let a = task_new(
            hp,
            None,
            sentinel_ptr(),
            std::ptr::null_mut(),
            std::ptr::null(),
        );
        (*a).ptr_free = Some(count_dup);

        // Same trans → the insert replaces `a`, firing task_free(a) → count_dup.
        let b = task_new(
            hp,
            None,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            std::ptr::null(),
        );
        assert_eq!(DUP_FREED.load(Ordering::SeqCst), 1);
        assert_eq!(glib::ffi::g_hash_table_size(hx_session_tasks(sp)), 1);
        assert_eq!(task_with_trans(sp, 0x4242), b);

        glib::ffi::g_hash_table_destroy(hx_session_tasks(sp));
    }
}

static DESTROY_FREED: AtomicUsize = AtomicUsize::new(0);
unsafe extern "C" fn count_destroy(_p: gpointer) {
    DESTROY_FREED.fetch_add(1, Ordering::SeqCst);
}

#[test]
fn destroy_frees_every_surviving_value() {
    unsafe {
        DESTROY_FREED.store(0, Ordering::SeqCst);
        let mut s = new_sess();
        let (sp, hp) = ptrs(&mut s);
        tasks_init(sp);

        for i in 100u32..132 {
            s.trans = i;
            let t = task_new(
                hp,
                None,
                sentinel_ptr(),
                std::ptr::null_mut(),
                std::ptr::null(),
            );
            (*t).ptr_free = Some(count_destroy);
        }
        assert_eq!(glib::ffi::g_hash_table_size(hx_session_tasks(sp)), 32);

        glib::ffi::g_hash_table_destroy(hx_session_tasks(sp));
        assert_eq!(DESTROY_FREED.load(Ordering::SeqCst), 32); // every ptr_free ran
    }
}

#[test]
fn high_trans_ids_stay_distinct() {
    unsafe {
        let mut s = new_sess();
        let (sp, hp) = ptrs(&mut s);
        tasks_init(sp);
        let ids = [
            0x0000_0001u32,
            0x0000_FFFF,
            0x0001_0000,
            0x7FFF_FFFF,
            0x8000_0000,
            0xFFFF_FFFE,
            0xFFFF_FFFF,
        ];
        let mut handles = Vec::new();
        for &id in &ids {
            s.trans = id;
            handles.push(task_new(
                hp,
                None,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                std::ptr::null(),
            ));
        }
        assert_eq!(
            glib::ffi::g_hash_table_size(hx_session_tasks(sp)),
            ids.len() as u32
        );
        for (i, &id) in ids.iter().enumerate() {
            assert_eq!(task_with_trans(sp, id), handles[i]);
        }
        glib::ffi::g_hash_table_destroy(hx_session_tasks(sp));
    }
}
