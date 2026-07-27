//! `HxChatRegistry` — the per-session `cid → conversation` map.
//!
//! Replaces the C `session->chats` `GHashTable<u32 cid, struct chat*>`. The
//! registry is deliberately *type-agnostic*: it stores each conversation as an
//! opaque `*mut c_void` and tears it down through a C destroy callback
//! (`chat_free`, registered at `hx_chats_new`). It therefore has no dependency
//! on `HxConversation` (which lives in `gtkhx-ui`) or on GTK — which is what
//! lets it live here, in the crate the headless wire-level tests already link,
//! so `network.c`'s disconnect teardown resolves against these symbols without
//! dragging in the UI crate.
//!
//! Backed by a `Vec` for stable index order (`get_at` / `cid_at` walks); lookup
//! and remove are linear scans, which is free at the handful-of-chats scale.
//! Everything runs on the GTK main thread — the raw pointers make the type
//! `!Send`/`!Sync`, which matches.

use std::os::raw::c_void;

/// C destroy-notify for one conversation (mirrors the old `GDestroyNotify`):
/// frees the attached view then the conversation handle.
pub type ChatDestroyFn = extern "C" fn(*mut c_void);

pub struct HxChatRegistry {
    entries: Vec<(u32, *mut c_void)>,
    destroy: Option<ChatDestroyFn>,
}

impl HxChatRegistry {
    fn position(&self, cid: u32) -> Option<usize> {
        self.entries.iter().position(|(c, _)| *c == cid)
    }
    fn destroy_ptr(&self, ptr: *mut c_void) {
        if let Some(d) = self.destroy {
            if !ptr.is_null() {
                d(ptr);
            }
        }
    }
}

/// Create an empty registry. `destroy` (nullable) is invoked on every
/// conversation pointer when it leaves the registry (`_remove`, replaced by a
/// same-cid `_insert`, or `_free`).
#[no_mangle]
pub extern "C" fn hx_chats_new(destroy: Option<ChatDestroyFn>) -> *mut HxChatRegistry {
    Box::into_raw(Box::new(HxChatRegistry {
        entries: Vec::new(),
        destroy,
    }))
}

/// Destroy the registry and every conversation still in it.
///
/// # Safety
/// `reg` is NULL or a pointer returned by `hx_chats_new` (consumed here).
#[no_mangle]
pub unsafe extern "C" fn hx_chats_free(reg: *mut HxChatRegistry) {
    if reg.is_null() {
        return;
    }
    let mut boxed = Box::from_raw(reg);
    let entries = std::mem::take(&mut boxed.entries);
    for (_, ptr) in entries {
        boxed.destroy_ptr(ptr);
    }
}

/// Insert (or replace) the conversation for `cid`. Replacing an existing entry
/// destroys the old conversation first — matching `g_hash_table_insert`.
///
/// # Safety
/// `reg` is NULL or a valid registry; `chat` is an opaque conversation pointer.
#[no_mangle]
pub unsafe extern "C" fn hx_chats_insert(reg: *mut HxChatRegistry, cid: u32, chat: *mut c_void) {
    let reg = match reg.as_mut() {
        Some(r) => r,
        None => return,
    };
    if let Some(pos) = reg.position(cid) {
        let old = reg.entries[pos].1;
        if old != chat {
            reg.destroy_ptr(old);
        }
        reg.entries[pos].1 = chat;
    } else {
        reg.entries.push((cid, chat));
    }
}

/// Remove and destroy the conversation for `cid` (no-op if absent).
///
/// # Safety
/// `reg` is NULL or a valid registry.
#[no_mangle]
pub unsafe extern "C" fn hx_chats_remove(reg: *mut HxChatRegistry, cid: u32) {
    let reg = match reg.as_mut() {
        Some(r) => r,
        None => return,
    };
    if let Some(pos) = reg.position(cid) {
        let (_, ptr) = reg.entries.remove(pos);
        reg.destroy_ptr(ptr);
    }
}

/// Look up the conversation for `cid`, or NULL.
///
/// # Safety
/// `reg` is NULL or a valid registry.
#[no_mangle]
pub unsafe extern "C" fn hx_chats_lookup(reg: *mut HxChatRegistry, cid: u32) -> *mut c_void {
    match reg.as_ref() {
        Some(r) => r
            .position(cid)
            .map(|pos| r.entries[pos].1)
            .unwrap_or(std::ptr::null_mut()),
        None => std::ptr::null_mut(),
    }
}

/// Number of conversations currently in the registry.
///
/// # Safety
/// `reg` is NULL or a valid registry.
#[no_mangle]
pub unsafe extern "C" fn hx_chats_count(reg: *mut HxChatRegistry) -> u32 {
    match reg.as_ref() {
        Some(r) => r.entries.len() as u32,
        None => 0,
    }
}

/// The conversation at index `i` (`0..count`), or NULL. Index order is stable
/// between mutations, so a `count` + `get_at` walk is a faithful iteration.
///
/// # Safety
/// `reg` is NULL or a valid registry.
#[no_mangle]
pub unsafe extern "C" fn hx_chats_get_at(reg: *mut HxChatRegistry, i: u32) -> *mut c_void {
    match reg.as_ref() {
        Some(r) => r
            .entries
            .get(i as usize)
            .map(|(_, p)| *p)
            .unwrap_or(std::ptr::null_mut()),
        None => std::ptr::null_mut(),
    }
}

/// The cid at index `i` (`0..count`), or 0. Lets a caller iterate without
/// reaching into the (gtkhx-ui-owned) conversation accessors for its cid.
///
/// # Safety
/// `reg` is NULL or a valid registry.
#[no_mangle]
pub unsafe extern "C" fn hx_chats_cid_at(reg: *mut HxChatRegistry, i: u32) -> u32 {
    match reg.as_ref() {
        Some(r) => r.entries.get(i as usize).map(|(c, _)| *c).unwrap_or(0),
        None => 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;

    thread_local! {
        static FREED: RefCell<Vec<usize>> = const { RefCell::new(Vec::new()) };
    }

    extern "C" fn record_free(ptr: *mut c_void) {
        FREED.with(|f| f.borrow_mut().push(ptr as usize));
    }

    fn p(n: usize) -> *mut c_void {
        n as *mut c_void
    }

    #[test]
    fn insert_lookup_count_get_at() {
        unsafe {
            let reg = hx_chats_new(None);
            hx_chats_insert(reg, 0, p(10));
            hx_chats_insert(reg, 5, p(50));
            assert_eq!(hx_chats_count(reg), 2);
            assert_eq!(hx_chats_lookup(reg, 0), p(10));
            assert_eq!(hx_chats_lookup(reg, 5), p(50));
            assert_eq!(hx_chats_lookup(reg, 99), std::ptr::null_mut());
            // Stable index order: insertion order.
            assert_eq!(hx_chats_cid_at(reg, 0), 0);
            assert_eq!(hx_chats_get_at(reg, 0), p(10));
            assert_eq!(hx_chats_cid_at(reg, 1), 5);
            assert_eq!(hx_chats_get_at(reg, 1), p(50));
            assert_eq!(hx_chats_get_at(reg, 2), std::ptr::null_mut());
            hx_chats_free(reg);
        }
    }

    #[test]
    fn remove_destroys_and_frees_rest_on_free() {
        FREED.with(|f| f.borrow_mut().clear());
        unsafe {
            let reg = hx_chats_new(Some(record_free));
            hx_chats_insert(reg, 0, p(10));
            hx_chats_insert(reg, 1, p(20));
            hx_chats_insert(reg, 2, p(30));
            hx_chats_remove(reg, 1); // frees p(20)
            assert_eq!(hx_chats_count(reg), 2);
            assert_eq!(hx_chats_lookup(reg, 1), std::ptr::null_mut());
            hx_chats_free(reg); // frees p(10) + p(30)
        }
        FREED.with(|f| {
            let v = f.borrow();
            assert_eq!(v.as_slice(), &[20usize, 10, 30]);
        });
    }

    #[test]
    fn insert_same_cid_destroys_old() {
        FREED.with(|f| f.borrow_mut().clear());
        unsafe {
            let reg = hx_chats_new(Some(record_free));
            hx_chats_insert(reg, 7, p(70));
            hx_chats_insert(reg, 7, p(71)); // replaces, frees p(70)
            assert_eq!(hx_chats_count(reg), 1);
            assert_eq!(hx_chats_lookup(reg, 7), p(71));
            hx_chats_free(reg); // frees p(71)
        }
        FREED.with(|f| assert_eq!(f.borrow().as_slice(), &[70usize, 71]));
    }
}
