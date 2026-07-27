//! `hxtask` — the per-session protocol transaction table (ported from
//! `tasks_table.c` + the `struct task` model half of `tasks.c`).
//!
//! Every outbound Hotline request registers a `struct task` keyed on its
//! transaction id in `session->tasks` (a `GHashTable<u32 trans, struct task*>`);
//! when the reply arrives, `rcv.c`'s dispatcher matches it by `trans` and
//! invokes the task's `rcv` callback. This crate owns that table's lifecycle
//! behind the exact C ABI the ~14 caller files + `rcv.c` link against —
//! `tasks_table_new` / `tasks_init` / `task_new` / `task_with_trans` /
//! `task_delete`.
//!
//! `struct task`'s layout stays C-owned (`protocol.h`); the `#[repr(C)]` [`Task`]
//! mirror below is pinned to it by the const-asserts here and by
//! `_Static_assert`s in `tasks_bridge.c`. The table stays a real `GHashTable`
//! (not a Rust map) because `network.c`'s teardown iterates `sess->tasks`
//! directly with `GHashTableIter` + `g_hash_table_remove_all`. This crate moves
//! the *ownership* of the factory + CRUD into Rust; the wire behaviour
//! (`g_direct_hash` + `GUINT_TO_POINTER(trans)`, `task_free` reclaiming `str`
//! plus the optional `ptr` via `ptr_free`) is byte-for-byte the old C.

use std::os::raw::{c_char, c_void};

use glib::ffi::{gpointer, GHashTable};

/// Opaque C types we only ever hold as pointers and hand back to C.
#[repr(C)]
pub struct Session {
    _private: [u8; 0],
}
#[repr(C)]
pub struct HtlcConn {
    _private: [u8; 0],
}

/// `rcv_task_fn` — `void (*)(struct htlc_conn *, void *, void *)` (protocol.h).
/// The `rcv_task_*` impls have heterogeneous arg lists cast to this shape at
/// `task_new` time; the crate only stores + hands the pointer back to C.
pub type RcvTaskFn = Option<unsafe extern "C" fn(*mut HtlcConn, *mut c_void, *mut c_void)>;

/// `#[repr(C)]` mirror of `struct task` (protocol.h). The const block pins the
/// Rust layout to the C offsets (LP64) so the two can't silently drift;
/// `tasks_bridge.c` asserts the C side against the same numbers.
#[repr(C)]
pub struct Task {
    pub trans: u32,
    pub pos: u32,
    pub len: u32,
    pub data: *mut c_void,
    pub str_: *mut c_char,
    pub ptr: *mut c_void,
    pub ptr_free: glib::ffi::GDestroyNotify,
    pub rcv: RcvTaskFn,
}

const _: () = {
    use std::mem::{align_of, offset_of, size_of};
    assert!(offset_of!(Task, trans) == 0);
    assert!(offset_of!(Task, pos) == 4);
    assert!(offset_of!(Task, len) == 8);
    assert!(offset_of!(Task, data) == 16);
    assert!(offset_of!(Task, str_) == 24);
    assert!(offset_of!(Task, ptr) == 32);
    assert!(offset_of!(Task, ptr_free) == 40);
    assert!(offset_of!(Task, rcv) == 48);
    assert!(size_of::<Task>() == 56);
    assert!(align_of::<Task>() == 8);
};

// The 32-bit trans id as a GHashTable key, exactly like the C
// `GUINT_TO_POINTER(trans)` (`(gpointer)(gulong)`).
#[inline]
fn trans_key(trans: u32) -> gpointer {
    trans as usize as gpointer
}

// ---- C environment ---------------------------------------------------------
//
// Resolved at the final C link in the real build; #[cfg(test)] doubles below
// let `cargo test` run headless. The extern block is gated out of test builds
// (the doubles take its place, same signatures) — same shape hxtls-trust uses.

#[cfg(not(test))]
use gtkhx_session::{gtkhx_session_emit_task_update, gtkhx_session_get_default};

#[cfg(not(test))]
extern "C" {
    /// The session owning this htlc — a linkable wrapper over the static-inline
    /// `sess_from_htlc` (tasks_bridge.c).
    fn hx_sess_from_htlc(htlc: *mut HtlcConn) -> *mut Session;
    /// `sess->tasks` accessor (tasks_bridge.c) — NULL until `tasks_init`.
    fn hx_session_tasks(sess: *mut Session) -> *mut GHashTable;
    /// `sess->tasks = table` (tasks_bridge.c).
    fn hx_session_set_tasks(sess: *mut Session, table: *mut GHashTable);
    /// `htlc->trans` accessor (tasks_bridge.c).
    fn hx_htlc_trans(htlc: *mut HtlcConn) -> u32;
    /// Drop the matching UI task row (view side, tasks.c) — called before the
    /// model entry is removed so the row can read the task if it needs to.
    fn gtask_delete_tsk(sess: *mut Session, trans: u32);
}

/// `GDestroyNotify` for the table: reclaim the optional `ptr` context (via its
/// registered `ptr_free`), the `str` label, and the task struct. Fires on
/// insert-replace, remove, and table destroy — so `g_hash_table_remove` /
/// `_remove_all` / `_destroy` do the full job and callers never peek inside.
unsafe extern "C" fn task_free(p: gpointer) {
    let tsk = p as *mut Task;
    if tsk.is_null() {
        return;
    }
    let t = &mut *tsk;
    if !t.ptr.is_null() {
        if let Some(free) = t.ptr_free {
            free(t.ptr);
        }
    }
    glib::ffi::g_free(t.str_ as gpointer);
    glib::ffi::g_free(tsk as gpointer);
}

/// `GHashTable *tasks_table_new (void)` — the factory tasks_init calls, and the
/// former `tasks_table.c` export. Keys are `trans` cast to pointer, so
/// `g_direct_hash` / `g_direct_equal`; no key destroy, `task_free` as value
/// destroy.
///
/// # Safety
/// Returns a fresh `GHashTable` the caller owns (destroy / unref when done).
#[no_mangle]
pub unsafe extern "C" fn tasks_table_new() -> *mut GHashTable {
    glib::ffi::g_hash_table_new_full(
        Some(glib::ffi::g_direct_hash),
        Some(glib::ffi::g_direct_equal),
        None,
        Some(task_free),
    )
}

/// `void tasks_init (session *sess)` — lazy-allocate `sess->tasks`. Idempotent:
/// only the first call constructs the table.
///
/// # Safety
/// `sess` is a valid `session *`.
#[no_mangle]
pub unsafe extern "C" fn tasks_init(sess: *mut Session) {
    if hx_session_tasks(sess).is_null() {
        let table = tasks_table_new();
        hx_session_set_tasks(sess, table);
    }
}

/// `struct task *task_new (struct htlc_conn *htlc, rcv_task_fn rcv, void *ptr,
/// void *data, const char *str)` — allocate + register a task keyed on the
/// htlc's current `trans`, and fire `task-update` so the UI sees it. `str` is
/// copied (`g_strdup`); `ptr_free` starts NULL (callers set it after if the
/// `ptr` context needs disconnect-time cleanup).
///
/// # Safety
/// `htlc` is a valid `struct htlc_conn *`; `str` is NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn task_new(
    htlc: *mut HtlcConn,
    rcv: RcvTaskFn,
    ptr: *mut c_void,
    data: *mut c_void,
    str_: *const c_char,
) -> *mut Task {
    let sess = hx_sess_from_htlc(htlc);
    let tsk = glib::ffi::g_malloc0(std::mem::size_of::<Task>()) as *mut Task;
    let t = &mut *tsk;
    t.trans = hx_htlc_trans(htlc);
    t.data = data;
    t.str_ = if str_.is_null() {
        std::ptr::null_mut()
    } else {
        glib::ffi::g_strdup(str_)
    };
    t.ptr = ptr;
    t.rcv = rcv;
    t.pos = 0;
    t.len = 1;

    glib::ffi::g_hash_table_insert(hx_session_tasks(sess), trans_key(t.trans), tsk as gpointer);
    gtkhx_session_emit_task_update(
        gtkhx_session_get_default(),
        sess as *mut c_void,
        tsk as *mut c_void,
    );
    tsk
}

/// `struct task *task_with_trans (session *sess, guint32 trans)` — O(1) lookup.
/// NULL if no task is registered for `trans` (a short/zero read leaves `trans`
/// at 0, which is a real key on a fresh connection, so this returns whatever is
/// pinned there — matching the old behaviour exactly).
///
/// # Safety
/// `sess` is a valid `session *` with an initialised task table.
#[no_mangle]
pub unsafe extern "C" fn task_with_trans(sess: *mut Session, trans: u32) -> *mut Task {
    glib::ffi::g_hash_table_lookup(hx_session_tasks(sess), trans_key(trans)) as *mut Task
}

/// `void task_delete (session *sess, struct task *tsk)` — drop the UI row then
/// remove the model entry (which fires `task_free`).
///
/// # Safety
/// `sess` is a valid `session *`; `tsk` is NULL or a pointer previously handed
/// out by `task_new` for this session.
#[no_mangle]
pub unsafe extern "C" fn task_delete(sess: *mut Session, tsk: *mut Task) {
    if tsk.is_null() {
        return;
    }
    let trans = (*tsk).trans;
    gtask_delete_tsk(sess, trans);
    glib::ffi::g_hash_table_remove(hx_session_tasks(sess), trans_key(trans));
}

// ---- test doubles for the C environment ------------------------------------
//
// A fake session/htlc is one `TestSession` struct: `sess_from_htlc` returns the
// same pointer, and the tasks table + current trans live on it. Each test owns
// its own on-stack `TestSession`, so there's no cross-test shared state.

#[cfg(test)]
#[repr(C)]
struct TestSession {
    tasks: *mut GHashTable,
    trans: u32,
}

#[cfg(test)]
unsafe fn hx_sess_from_htlc(htlc: *mut HtlcConn) -> *mut Session {
    htlc as *mut Session
}
#[cfg(test)]
unsafe fn hx_session_tasks(sess: *mut Session) -> *mut GHashTable {
    (*(sess as *mut TestSession)).tasks
}
#[cfg(test)]
unsafe fn hx_session_set_tasks(sess: *mut Session, table: *mut GHashTable) {
    (*(sess as *mut TestSession)).tasks = table;
}
#[cfg(test)]
unsafe fn hx_htlc_trans(htlc: *mut HtlcConn) -> u32 {
    (*(htlc as *mut TestSession)).trans
}
#[cfg(test)]
unsafe fn gtask_delete_tsk(_sess: *mut Session, _trans: u32) {}
#[cfg(test)]
unsafe fn gtkhx_session_get_default() -> *mut c_void {
    std::ptr::null_mut()
}
#[cfg(test)]
unsafe fn gtkhx_session_emit_task_update(
    _self_: *mut c_void,
    _sess: *mut c_void,
    _tsk: *mut c_void,
) {
}

#[cfg(test)]
mod tests;
