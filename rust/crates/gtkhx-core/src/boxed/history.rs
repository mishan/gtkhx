//! `HxHistoryEntry` — one decoded chat-history message (fogWraith
//! `Capabilities-Chat-History.md`, `src/chat_history.h`).
//!
//! Unlike the other `boxed` types, this is **not** a registered GObject boxed
//! type: the `chat-history-batch` signal carries a `GPtrArray<HxHistoryEntry*>`
//! as a plain `G_TYPE_POINTER`, and the array's `GDestroyNotify` is
//! [`hx_history_entry_free`]. So there is no `_get_type` / `_copy` here — only
//! the `#[repr(C)]` struct, its construction from the session's entry, and the
//! free.
//!
//! The struct layout stays C-visible: `chat.c` reads `->message_id`,
//! `->timestamp`, `->flags`, `->icon_id`, `->nick`, `->message` directly, so the
//! byte layout is pinned on both sides — `_Static_assert`s in `chat_history.c`
//! against the `offset_of!` block below. Memory is glib's (`g_malloc` /
//! `g_free`), so an entry built here and freed via the array's
//! `hx_history_entry_free` destructor use one allocator.

use glib::ffi::{g_free, g_malloc, g_malloc0};
use std::ffi::c_char;
use std::mem::{offset_of, size_of};
use std::os::raw::c_void;
use std::ptr;

/// `#[repr(C)]` mirror of `HxHistoryEntry` (`src/chat_history.h`). `nick` /
/// `message` are NUL-terminated glib-owned UTF-8 copies of the session's
/// entry (`hxsession::HistoryEntry`), decoded the way a live chat line is,
/// with its `\r` line breaks as `\n`: a raw `\r` reaches Pango as a paragraph
/// break the chat layout doesn't count, so a multi-line entry would draw over
/// the rows below it. The `*_len` fields carry their byte lengths (which may
/// differ from `strlen` if the payload holds an interior NUL — see the
/// copy-by-length note on `dup_by_len`).
#[repr(C)]
pub struct HxHistoryEntry {
    pub message_id: u64,
    pub timestamp: i64, // i64 on the wire (Unix epoch UTC seconds)
    pub flags: u16,
    pub icon_id: u16,
    pub nick: *mut c_char,
    pub nick_len: usize,
    pub message: *mut c_char,
    pub message_len: usize,
}

const _: () = {
    assert!(size_of::<HxHistoryEntry>() == 56);
    assert!(offset_of!(HxHistoryEntry, message_id) == 0);
    assert!(offset_of!(HxHistoryEntry, timestamp) == 8);
    assert!(offset_of!(HxHistoryEntry, flags) == 16);
    assert!(offset_of!(HxHistoryEntry, icon_id) == 18);
    assert!(offset_of!(HxHistoryEntry, nick) == 24);
    assert!(offset_of!(HxHistoryEntry, nick_len) == 32);
    assert!(offset_of!(HxHistoryEntry, message) == 40);
    assert!(offset_of!(HxHistoryEntry, message_len) == 48);
};

/// Copy `src` into a fresh `g_malloc(src.len() + 1)` buffer with a trailing NUL,
/// returning `(ptr, len)`. Copy **by length**, not `g_strndup`: `g_strndup`
/// stops at the first embedded NUL and allocates only what it copied, but the
/// wire payload may carry interior NULs (the server has no obligation to scrub
/// them) and downstream length-aware readers (`g_strstr_len`) use the recorded
/// length — a shorter allocation would let them walk past the buffer. `g_malloc`
/// + copy + trailing NUL keeps allocation length and recorded length in lockstep.
pub(crate) unsafe fn dup_by_len(src: &[u8]) -> *mut c_char {
    let p = g_malloc(src.len() + 1) as *mut u8;
    if !src.is_empty() {
        ptr::copy_nonoverlapping(src.as_ptr(), p, src.len());
    }
    *p.add(src.len()) = 0;
    p as *mut c_char
}

/// A heap `HxHistoryEntry` holding `e`, freed by [`hx_history_entry_free`].
pub fn history_entry_new(e: &hxsession::HistoryEntry) -> *mut HxHistoryEntry {
    // SAFETY: the allocation is zeroed and sized for the struct, and every
    // pointer stored in it is a fresh glib allocation.
    unsafe {
        let entry = g_malloc0(size_of::<HxHistoryEntry>()) as *mut HxHistoryEntry;
        (*entry).message_id = e.message_id;
        (*entry).timestamp = e.timestamp;
        (*entry).flags = e.flags;
        (*entry).icon_id = e.icon;
        (*entry).nick_len = e.nick.len();
        (*entry).nick = dup_by_len(e.nick.as_bytes());
        (*entry).message_len = e.text.len();
        (*entry).message = dup_by_len(e.text.as_bytes());
        entry
    }
}

/// `void hx_history_entry_free (entry)` — release an entry and its glib-owned
/// `nick` / `message` (the `GDestroyNotify` for the `chat-history-batch`
/// `GPtrArray`). NULL-safe.
///
/// # Safety
/// `entry` is NULL or a valid `HxHistoryEntry*` with glib-owned `nick`/`message`.
#[no_mangle]
pub unsafe extern "C" fn hx_history_entry_free(entry: *mut HxHistoryEntry) {
    if entry.is_null() {
        return;
    }
    g_free((*entry).nick as *mut c_void);
    g_free((*entry).message as *mut c_void);
    g_free(entry as *mut c_void);
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe fn cbytes(p: *const c_char, len: usize) -> Vec<u8> {
        std::slice::from_raw_parts(p as *const u8, len).to_vec()
    }

    /// Every field crosses, and each string is copied by its length with a
    /// trailing NUL: an empty one is just the NUL, and an interior NUL
    /// doesn't cut one short.
    #[test]
    fn an_entry_copies_each_string_by_its_length() {
        let src = hxsession::HistoryEntry {
            message_id: 42,
            timestamp: -1,
            flags: 0x0007,
            icon: 7,
            nick: String::new(),
            text: "ab\0cd".into(),
        };
        unsafe {
            let e = history_entry_new(&src);
            assert_eq!((*e).message_id, 42);
            assert_eq!((*e).timestamp, -1);
            assert_eq!((*e).flags, 0x0007);
            assert_eq!((*e).icon_id, 7);
            assert_eq!(cbytes((*e).nick, (*e).nick_len + 1), b"\0");
            assert_eq!(cbytes((*e).message, (*e).message_len + 1), b"ab\0cd\0");
            hx_history_entry_free(e);
        }
    }

    #[test]
    fn free_is_null_safe() {
        unsafe { hx_history_entry_free(ptr::null_mut()) };
    }
}
