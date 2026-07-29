//! `HxHistoryEntry` — one decoded chat-history message (fogWraith
//! `Capabilities-Chat-History.md`, `src/chat_history.h`).
//!
//! Unlike the other `boxed` types, this is **not** a registered GObject boxed
//! type: the `chat-history-batch` signal carries a `GPtrArray<HxHistoryEntry*>`
//! as a plain `G_TYPE_POINTER`, and the array's `GDestroyNotify` is
//! [`hx_history_entry_free`]. So there is no `_get_type` / `_copy` here — only
//! the `#[repr(C)]` struct, the parse (`hx_history_entry_parse`), and the free.
//!
//! The struct layout stays C-visible: `chat.c` reads `->message_id`,
//! `->timestamp`, `->flags`, `->icon_id`, `->nick`, `->message` directly, so the
//! byte layout is pinned on both sides — `_Static_assert`s in `chat_history.c`
//! against the `offset_of!` block below. Memory is glib's (`g_malloc` /
//! `g_free`), same as the deleted C bodies, so an entry built here and freed via
//! the array's `hx_history_entry_free` destructor use one allocator.

use glib::ffi::{g_free, g_malloc, g_malloc0};
use std::ffi::c_char;
use std::mem::{offset_of, size_of};
use std::os::raw::c_void;
use std::ptr;

/// `#[repr(C)]` mirror of `HxHistoryEntry` (`src/chat_history.h`). `nick` /
/// `message` are NUL-terminated glib-owned copies; the `*_len` fields carry the
/// on-wire byte lengths (which may differ from `strlen` if the payload holds an
/// interior NUL — see the copy-by-length note in [`hx_history_entry_parse`]).
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
unsafe fn dup_by_len(src: &[u8]) -> *mut c_char {
    let p = g_malloc(src.len() + 1) as *mut u8;
    if !src.is_empty() {
        ptr::copy_nonoverlapping(src.as_ptr(), p, src.len());
    }
    *p.add(src.len()) = 0;
    p as *mut c_char
}

/// `HxHistoryEntry *hx_history_entry_parse (data, len)` — decode one packed
/// `HTLS_DATA_HISTORY_ENTRY` chunk body into a heap `HxHistoryEntry`, or NULL on
/// a malformed entry (too short, or a declared length running past the buffer).
/// The packed-binary decode itself is `hotline_proto::parse::parse_history_entry`
/// (24-byte fixed header + nick + message + best-effort mini-TLV walk); this wraps
/// it with the glib allocation the entry's owner expects. Caller frees via
/// [`hx_history_entry_free`].
///
/// # Safety
/// `data` is valid for `len` bytes, or NULL (returns NULL).
#[no_mangle]
pub unsafe extern "C" fn hx_history_entry_parse(data: *const u8, len: usize) -> *mut HxHistoryEntry {
    if data.is_null() {
        return ptr::null_mut();
    }
    let s = std::slice::from_raw_parts(data, len);
    let Some(e) = hotline_proto::parse::parse_history_entry(s) else {
        return ptr::null_mut();
    };
    let entry = g_malloc0(size_of::<HxHistoryEntry>()) as *mut HxHistoryEntry;
    (*entry).message_id = e.message_id;
    (*entry).timestamp = e.timestamp;
    (*entry).flags = e.flags;
    (*entry).icon_id = e.icon_id;
    (*entry).nick_len = e.nick.len();
    (*entry).nick = dup_by_len(e.nick);
    (*entry).message_len = e.message.len();
    (*entry).message = dup_by_len(e.message);
    entry
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

    /// Build a minimal well-formed entry body: 8+8+2+2 fixed header, u16 nick_len
    /// + nick, u16 msg_len + msg.
    fn entry_body(message_id: u64, timestamp: i64, flags: u16, icon: u16, nick: &[u8], msg: &[u8]) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(&message_id.to_be_bytes());
        v.extend_from_slice(&timestamp.to_be_bytes());
        v.extend_from_slice(&flags.to_be_bytes());
        v.extend_from_slice(&icon.to_be_bytes());
        v.extend_from_slice(&(nick.len() as u16).to_be_bytes());
        v.extend_from_slice(nick);
        v.extend_from_slice(&(msg.len() as u16).to_be_bytes());
        v.extend_from_slice(msg);
        v
    }

    unsafe fn cbytes(p: *const c_char, len: usize) -> Vec<u8> {
        std::slice::from_raw_parts(p as *const u8, len).to_vec()
    }

    #[test]
    fn parses_and_frees_a_typical_entry() {
        let body = entry_body(42, 1_700_000_000, 0x0001, 7, b"alice", b"hello world");
        unsafe {
            let e = hx_history_entry_parse(body.as_ptr(), body.len());
            assert!(!e.is_null());
            assert_eq!((*e).message_id, 42);
            assert_eq!((*e).timestamp, 1_700_000_000);
            assert_eq!((*e).flags, 0x0001);
            assert_eq!((*e).icon_id, 7);
            assert_eq!((*e).nick_len, 5);
            assert_eq!(cbytes((*e).nick, 5), b"alice");
            assert_eq!(*(*e).nick.add(5), 0); // trailing NUL
            assert_eq!((*e).message_len, 11);
            assert_eq!(cbytes((*e).message, 11), b"hello world");
            hx_history_entry_free(e);
        }
    }

    #[test]
    fn preserves_interior_nul_by_length() {
        // A message with an embedded NUL: message_len must be the full wire
        // length, not truncated at the NUL (g_strndup would truncate).
        let msg = b"ab\0cd";
        let body = entry_body(1, 0, 0, 0, b"", msg);
        unsafe {
            let e = hx_history_entry_parse(body.as_ptr(), body.len());
            assert!(!e.is_null());
            assert_eq!((*e).message_len, 5);
            assert_eq!(cbytes((*e).message, 5), msg);
            assert_eq!((*e).nick_len, 0);
            assert_eq!(*(*e).nick, 0); // empty nick is just a NUL
            hx_history_entry_free(e);
        }
    }

    #[test]
    fn rejects_null_and_short() {
        unsafe {
            assert!(hx_history_entry_parse(ptr::null(), 24).is_null());
            let short = [0u8; 8];
            assert!(hx_history_entry_parse(short.as_ptr(), short.len()).is_null());
        }
    }

    #[test]
    fn free_is_null_safe() {
        unsafe { hx_history_entry_free(ptr::null_mut()) };
    }
}
