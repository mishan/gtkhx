//! `MediaTable` — the per-chat inline-media handle table (M3).
//!
//! Straight port of `chat.c`'s `gchat->media_handles` `GHashTable` +
//! `gchat->media_next_id`, plus the three static helpers that drove them
//! (`gchat_media_value_free`, `ensure_media_handles`, `gchat_register_media`).
//!
//! When a chat line carries inline media, `output_chat_from_event`
//! [`register`](MediaTable::register)s a deep copy under a freshly-minted
//! token and embeds `hxmedia:<token>` in the placeholder row text. A click on
//! that word [`lookup`](MediaTable::lookup)s the token to pop the dialog. The
//! table owns each `HxChatMedia` copy (glib-allocated `id`/`mime`, via the
//! audited [`media_copy`]) and frees them ([`media_free`]) on drop — so the C
//! side no longer duplicates that free logic.
//!
//! C owns the table as an opaque `Box<MediaTable>` pointer: `new` boxes it,
//! `free` drops it (running `Drop`, which releases every entry). `register`
//! and `lookup` borrow it. Token 0 is reserved for "absent".

use crate::boxed::chat::{media_copy, media_free, HxChatMedia};
use std::collections::HashMap;
use std::os::raw::c_void;
use std::ptr;

/// Per-chat token → `HxChatMedia` map. Each value is a glib-owned deep copy
/// the table frees on removal / drop.
pub struct MediaTable {
    handles: HashMap<u32, *mut HxChatMedia>,
    next_id: u32,
}

impl MediaTable {
    fn new() -> Self {
        MediaTable {
            handles: HashMap::new(),
            next_id: 0,
        }
    }

    /// Deep-copy `src` under a fresh monotonic token and return it. Tokens run
    /// 1..=`u32::MAX`; 0 is never handed out (it's the "absent" sentinel).
    /// Mirrors `gchat_register_media`.
    ///
    /// Failure mode: if the counter is exhausted (2^32-1 registrations on one
    /// chat — impossible in practice), returns 0 without copying or inserting,
    /// rather than letting `next_id` wrap and hand out 0 / collide with a live
    /// token. The caller treats 0 as "no clickable token" (same as a NULL
    /// `src`), so the media line just renders non-interactive.
    ///
    /// # Safety
    /// `src` is a valid `HxChatMedia*` with glib-owned `id`/`mime`.
    unsafe fn register(&mut self, src: *const HxChatMedia) -> u32 {
        // Reserve the token first so an overflow bails before we allocate a
        // copy we'd then have to free.
        let Some(id) = self.next_id.checked_add(1) else {
            return 0;
        };
        self.next_id = id;
        let copy = media_copy(src);
        // Monotonic ids never collide, but if one somehow did, free the
        // displaced copy rather than leak it.
        if let Some(old) = self.handles.insert(id, copy) {
            media_free(old);
        }
        id
    }

    /// Borrow the copy registered under `token`, or NULL. The pointer stays
    /// valid until the entry is removed or the table dropped (the caller
    /// reads it synchronously). Mirrors the `g_hash_table_lookup`.
    fn lookup(&self, token: u32) -> *const HxChatMedia {
        self.handles.get(&token).copied().unwrap_or(ptr::null_mut()) as *const HxChatMedia
    }
}

impl Drop for MediaTable {
    fn drop(&mut self) {
        for (_, m) in self.handles.drain() {
            unsafe { media_free(m) };
        }
    }
}

/// `void *hx_media_table_new(void)` — a fresh empty table (caller
/// `hx_media_table_free`s).
#[no_mangle]
pub extern "C" fn hx_media_table_new() -> *mut c_void {
    Box::into_raw(Box::new(MediaTable::new())) as *mut c_void
}

/// `void hx_media_table_free(void *table)` — drop the table and every copy in
/// it.
///
/// # Safety
/// `table` is NULL or a pointer from [`hx_media_table_new`].
#[no_mangle]
pub unsafe extern "C" fn hx_media_table_free(table: *mut c_void) {
    if !table.is_null() {
        drop(Box::from_raw(table as *mut MediaTable));
    }
}

/// `guint hx_media_table_register(void *table, const HxChatMedia *src)` —
/// deep-copy `src` and return its token (0 on NULL args).
///
/// # Safety
/// `table` is NULL or from [`hx_media_table_new`]; `src` is NULL or a valid
/// `HxChatMedia*`.
#[no_mangle]
pub unsafe extern "C" fn hx_media_table_register(
    table: *mut c_void,
    src: *const HxChatMedia,
) -> u32 {
    if table.is_null() || src.is_null() {
        return 0;
    }
    (*(table as *mut MediaTable)).register(src)
}

/// `const HxChatMedia *hx_media_table_lookup(void *table, guint token)` —
/// borrow the entry (NULL if absent). The table retains ownership.
///
/// # Safety
/// `table` is NULL or from [`hx_media_table_new`].
#[no_mangle]
pub unsafe extern "C" fn hx_media_table_lookup(
    table: *mut c_void,
    token: u32,
) -> *const HxChatMedia {
    if table.is_null() {
        return ptr::null();
    }
    (*(table as *mut MediaTable)).lookup(token)
}

#[cfg(test)]
mod tests {
    use super::*;
    use glib::ffi::{g_malloc, g_malloc0, g_strndup};
    use std::ffi::c_char;
    use std::mem::size_of;

    /// Build a heap `HxChatMedia` the way `hx_chat_event_attach_media` would.
    unsafe fn make_media(id: &[u8], mime: &str, w: u32, h: u32) -> *mut HxChatMedia {
        let m = g_malloc0(size_of::<HxChatMedia>()) as *mut HxChatMedia;
        (*m).id_len = id.len();
        if !id.is_empty() {
            let dst = g_malloc(id.len()) as *mut u8;
            ptr::copy_nonoverlapping(id.as_ptr(), dst, id.len());
            (*m).id = dst;
        }
        (*m).mime = g_strndup(mime.as_ptr() as *const c_char, mime.len());
        (*m).mime_len = mime.len();
        (*m).width = w;
        (*m).height = h;
        (*m).width_present = 1;
        (*m).height_present = 1;
        m
    }

    // Read the copy's fields through a checked shared reference rather than
    // repeated raw `(*ptr).field` derefs — a non-null assert + `&*` gives the
    // analyzer a validity guard and keeps the byte reads bounded by the
    // struct's own length fields.
    unsafe fn as_ref<'a>(m: *const HxChatMedia) -> &'a HxChatMedia {
        assert!(!m.is_null(), "media pointer is NULL");
        &*m
    }

    fn mime_of(m: &HxChatMedia) -> String {
        if m.mime.is_null() || m.mime_len == 0 {
            return String::new();
        }
        let bytes = unsafe { std::slice::from_raw_parts(m.mime as *const u8, m.mime_len) };
        String::from_utf8_lossy(bytes).into_owned()
    }

    fn id_bytes(m: &HxChatMedia) -> &[u8] {
        if m.id.is_null() || m.id_len == 0 {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(m.id, m.id_len) }
    }

    #[test]
    fn tokens_are_monotonic_from_one() {
        unsafe {
            let t = hx_media_table_new();
            let a = make_media(b"\x01\x02", "image/png", 10, 20);
            let b = make_media(b"\x03", "image/gif", 30, 40);
            assert_eq!(hx_media_table_register(t, a), 1);
            assert_eq!(hx_media_table_register(t, b), 2);
            hx_media_table_free(t);
            // The table deep-copied; the originals are still ours to free.
            media_free(a);
            media_free(b);
        }
    }

    #[test]
    fn lookup_returns_a_distinct_deep_copy() {
        unsafe {
            let t = hx_media_table_new();
            let src = make_media(b"\xDE\xAD\xBE\xEF", "image/png", 800, 600);
            let tok = hx_media_table_register(t, src);
            let got = hx_media_table_lookup(t, tok);
            let g = as_ref(got);
            let s = as_ref(src);
            // Distinct struct + distinct id/mime allocations.
            assert_ne!(got, src as *const HxChatMedia);
            assert_ne!(g.id as *const u8, s.id as *const u8);
            assert_ne!(g.mime as *const c_char, s.mime as *const c_char);
            // Same contents.
            assert_eq!(g.id_len, 4);
            assert_eq!(id_bytes(g), &[0xDE, 0xAD, 0xBE, 0xEF]);
            assert_eq!(mime_of(g), "image/png");
            assert_eq!(g.width, 800);
            assert_eq!(g.height_present, 1);
            // Freeing the original leaves the table's copy intact.
            media_free(src);
            let got2 = hx_media_table_lookup(t, tok);
            assert_eq!(mime_of(as_ref(got2)), "image/png");
            hx_media_table_free(t);
        }
    }

    #[test]
    fn lookup_of_absent_or_zero_token_is_null() {
        unsafe {
            let t = hx_media_table_new();
            let src = make_media(b"\x00", "image/png", 1, 1);
            hx_media_table_register(t, src);
            assert!(hx_media_table_lookup(t, 0).is_null());
            assert!(hx_media_table_lookup(t, 99).is_null());
            media_free(src);
            hx_media_table_free(t);
        }
    }

    #[test]
    fn token_counter_saturates_instead_of_wrapping_to_zero() {
        unsafe {
            // Drive the counter to the brink so the next two registers cover
            // the last valid token and the exhausted-counter failure.
            let mut t = MediaTable::new();
            t.next_id = u32::MAX - 1;
            let src = make_media(b"\x01", "image/png", 1, 1);

            // Last valid token is u32::MAX, and it's really stored.
            assert_eq!(t.register(src), u32::MAX);
            assert!(!t.lookup(u32::MAX).is_null());

            // Counter exhausted: 0 (the "absent" sentinel), never a wrapped
            // low token, and nothing new inserted.
            assert_eq!(t.register(src), 0);
            assert!(t.lookup(0).is_null());
            assert!(t.lookup(1).is_null());

            media_free(src);
            // t drops here, freeing its single stored copy (no leak from the
            // failed register — it never allocated one).
        }
    }

    #[test]
    fn null_args_are_safe() {
        unsafe {
            assert_eq!(hx_media_table_register(ptr::null_mut(), ptr::null()), 0);
            assert!(hx_media_table_lookup(ptr::null_mut(), 1).is_null());
            let t = hx_media_table_new();
            // NULL src → token 0, nothing registered.
            assert_eq!(hx_media_table_register(t, ptr::null()), 0);
            assert!(hx_media_table_lookup(t, 1).is_null());
            hx_media_table_free(t);
            hx_media_table_free(ptr::null_mut());
        }
    }
}
