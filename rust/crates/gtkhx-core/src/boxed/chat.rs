//! `HxChatEvent` + nested `HxChatMedia` — chat-line value object
//! (`src/proto_helpers.h`): the boxed type, and [`chat_event_new`], which
//! builds one from the line the session decoded. The media placeholder
//! formatter stays in C and reads these `#[repr(C)]` structs, as do the
//! chat output and notification consumers.

use crate::boxed::history::dup_by_len;
use crate::boxed::register_once;
use glib::ffi::{g_free, g_malloc, g_malloc0, g_strndup, GType};
use std::ffi::c_char;
use std::mem::{offset_of, size_of};
use std::os::raw::c_void;
use std::ptr;
use std::sync::OnceLock;

/// `#[repr(C)]` mirror of `HxChatMedia` (optional inline-media metadata
/// attached to a chat event). Owned by the parent `HxChatEvent`.
#[repr(C)]
pub struct HxChatMedia {
    pub id: *mut u8, // opaque handle bytes (not NUL-terminated)
    pub id_len: usize,
    pub mime: *mut c_char, // canonical MIME (NUL-terminated)
    pub mime_len: usize,
    pub width: u32,
    pub height: u32,
    pub bytes: u32,
    pub width_present: i32,  // gboolean
    pub height_present: i32, // gboolean
    pub bytes_present: i32,  // gboolean
}

const _: () = {
    assert!(size_of::<HxChatMedia>() == 56);
    assert!(offset_of!(HxChatMedia, id) == 0);
    assert!(offset_of!(HxChatMedia, id_len) == 8);
    assert!(offset_of!(HxChatMedia, mime) == 16);
    assert!(offset_of!(HxChatMedia, mime_len) == 24);
    assert!(offset_of!(HxChatMedia, width) == 32);
    assert!(offset_of!(HxChatMedia, height) == 36);
    assert!(offset_of!(HxChatMedia, bytes) == 40);
    assert!(offset_of!(HxChatMedia, width_present) == 44);
    assert!(offset_of!(HxChatMedia, height_present) == 48);
    assert!(offset_of!(HxChatMedia, bytes_present) == 52);
};

/// `#[repr(C)]` mirror of `struct _HxChatEvent`.
#[repr(C)]
pub struct HxChatEvent {
    pub cid: u32,
    /// Sender's Hotline uid from the wire's UID chunk; 0 when the
    /// server sent none. Occupies the padding after `cid`, so the
    /// struct is still 72 bytes and no other offset moved.
    pub uid: u16,
    pub line: *mut c_char, // UTF-8, NUL-terminated, owned
    pub line_len: usize,
    pub sender_off: usize,
    pub sender_len: usize,
    pub body_off: usize,
    pub body_len: usize,
    pub is_info: i32, // gboolean
    pub is_self: i32, // gboolean
    pub media: *mut HxChatMedia,
}

const _: () = {
    assert!(size_of::<HxChatEvent>() == 72);
    assert!(offset_of!(HxChatEvent, cid) == 0);
    assert!(offset_of!(HxChatEvent, uid) == 4);
    assert!(offset_of!(HxChatEvent, line) == 8);
    assert!(offset_of!(HxChatEvent, line_len) == 16);
    assert!(offset_of!(HxChatEvent, sender_off) == 24);
    assert!(offset_of!(HxChatEvent, sender_len) == 32);
    assert!(offset_of!(HxChatEvent, body_off) == 40);
    assert!(offset_of!(HxChatEvent, body_len) == 48);
    assert!(offset_of!(HxChatEvent, is_info) == 56);
    assert!(offset_of!(HxChatEvent, is_self) == 60);
    assert!(offset_of!(HxChatEvent, media) == 64);
};

/// Where the nick and the body sit in a "nick: body" chat line, as byte
/// ranges: `(nick_off, nick_len, body_off, body_len)`. Servers pad the line
/// ahead of the nick, and a body may itself hold colons. `None` for a line
/// without that shape — an emote, a server's prose — or whose part before
/// the colon is longer than a nick can be (31 bytes), which is prose that
/// happens to hold a colon rather than a nick.
pub fn split_nick_body(line: &[u8]) -> Option<(usize, usize, usize, usize)> {
    let start = line.iter().position(|&c| c != b' ' && c != b'\t')?;
    let colon = start + line[start..].iter().position(|&c| c == b':')?;
    if colon == start || colon - start > 31 {
        return None;
    }
    let body = colon + 1 + line[colon + 1..].iter().take_while(|&&c| c == b' ').count();
    Some((start, colon - start, body, line.len() - body))
}

/// `gboolean hx_chat_split_nick_body (line, line_len, *name_offset,
/// *name_len, *body_offset, *body_len)` — [`split_nick_body`] for C; each
/// out-param may be NULL.
///
/// # Safety
/// `line` is NULL or valid for `line_len` bytes; each out-param is NULL or
/// a valid `gsize *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_split_nick_body(
    line: *const c_char,
    line_len: usize,
    name_offset: *mut usize,
    name_len: *mut usize,
    body_offset: *mut usize,
    body_len: *mut usize,
) -> i32 {
    if line.is_null() {
        return 0;
    }
    let bytes = std::slice::from_raw_parts(line as *const u8, line_len);
    let Some(parts) = split_nick_body(bytes) else {
        return 0;
    };
    for (out, v) in [
        (name_offset, parts.0),
        (name_len, parts.1),
        (body_offset, parts.2),
        (body_len, parts.3),
    ] {
        if !out.is_null() {
            *out = v;
        }
    }
    1
}

/// A heap `HxChatEvent` for a chat line the session decoded, freed by
/// [`hx_chat_event_free`]. With `shortcodes`, `:shortcode:`s become their
/// emoji across the whole line before it is split: some servers format a
/// line with no "nick:", and a body-only decode would take a shortcode's
/// own colon for the separator. A nick is never a shortcode — those are
/// lowercase and colon-delimited on both sides. `self_nick` is the name
/// this connection goes by, which marks the line as its own when the
/// sender is exactly that.
pub fn chat_event_new(
    cid: u32,
    uid: u16,
    text: &str,
    media: Option<&hxsession::ChatMedia>,
    self_nick: &[u8],
    shortcodes: bool,
) -> *mut HxChatEvent {
    let decoded;
    let line = if shortcodes {
        decoded = hxproto::emoji::shortcodes_to_emoji(text);
        decoded.as_bytes()
    } else {
        text.as_bytes()
    };
    let split = split_nick_body(line);
    // SAFETY: the allocation is zeroed and sized for the struct, and every
    // pointer stored in it is a fresh glib allocation.
    unsafe {
        let e = g_malloc0(size_of::<HxChatEvent>()) as *mut HxChatEvent;
        (*e).cid = cid;
        (*e).uid = uid;
        (*e).line = dup_by_len(line);
        (*e).line_len = line.len();
        if let Some((so, sl, bo, bl)) = split {
            (*e).sender_off = so;
            (*e).sender_len = sl;
            (*e).body_off = bo;
            (*e).body_len = bl;
            (*e).is_self = i32::from(!self_nick.is_empty() && &line[so..so + sl] == self_nick);
        }
        (*e).media = media_new(media);
        e
    }
}

/// A heap `HxChatMedia` for the picture the session read off a line, or
/// NULL when there is none or it names no picture (no id or no type).
/// Freed by [`media_free`].
pub(crate) fn media_new(media: Option<&hxsession::ChatMedia>) -> *mut HxChatMedia {
    let Some(m) = media.filter(|m| !m.id.is_empty() && !m.mime.is_empty()) else {
        return ptr::null_mut();
    };
    // SAFETY: the allocation is zeroed and sized for the struct, and the
    // id and mime stored in it are fresh glib copies.
    unsafe {
        let c = g_malloc0(size_of::<HxChatMedia>()) as *mut HxChatMedia;
        (*c).id_len = m.id.len();
        let id = g_malloc(m.id.len()) as *mut u8;
        ptr::copy_nonoverlapping(m.id.as_ptr(), id, m.id.len());
        (*c).id = id;
        (*c).mime_len = m.mime.len();
        (*c).mime = g_strndup(m.mime.as_ptr() as *const c_char, m.mime.len());
        (*c).width = m.width.unwrap_or(0);
        (*c).width_present = i32::from(m.width.is_some());
        (*c).height = m.height.unwrap_or(0);
        (*c).height_present = i32::from(m.height.is_some());
        (*c).bytes = m.bytes.unwrap_or(0);
        (*c).bytes_present = i32::from(m.bytes.is_some());
        c
    }
}

/// Deep-copy an `HxChatMedia` (mirrors the deleted C static
/// `hx_chat_media_copy`). `id` is raw bytes (`g_malloc` + copy), `mime`
/// is NUL-terminated (`g_strndup`). `pub(crate)` so [`crate::boxed::media_table`]
/// can deep-copy into its per-chat token table.
///
/// # Safety
/// `m` is NULL or a valid `HxChatMedia*` with glib-owned `id`/`mime`.
pub(crate) unsafe fn media_copy(m: *const HxChatMedia) -> *mut HxChatMedia {
    if m.is_null() {
        return ptr::null_mut();
    }
    let c = g_malloc0(size_of::<HxChatMedia>()) as *mut HxChatMedia;
    (*c).id_len = (*m).id_len;
    if (*m).id_len != 0 {
        let dst = g_malloc((*m).id_len) as *mut u8;
        ptr::copy_nonoverlapping((*m).id as *const u8, dst, (*m).id_len);
        (*c).id = dst;
    }
    (*c).mime_len = (*m).mime_len;
    if !(*m).mime.is_null() {
        (*c).mime = g_strndup((*m).mime, (*m).mime_len);
    }
    (*c).width = (*m).width;
    (*c).height = (*m).height;
    (*c).bytes = (*m).bytes;
    (*c).width_present = (*m).width_present;
    (*c).height_present = (*m).height_present;
    (*c).bytes_present = (*m).bytes_present;
    c
}

/// Free an `HxChatMedia`. `pub(crate)` so
/// [`crate::boxed::media_table`] can release its entries.
///
/// # Safety
/// `m` is NULL or a valid glib-owned `HxChatMedia*`.
pub(crate) unsafe fn media_free(m: *mut HxChatMedia) {
    if m.is_null() {
        return;
    }
    g_free((*m).id as *mut c_void);
    g_free((*m).mime as *mut c_void);
    g_free(m as *mut c_void);
}

/// Boxed copy func. Mirrors the deleted C `hx_chat_event_copy`: shallow
/// copy the scalar fields, then deep-copy `line` (`g_strndup`) and
/// `media` (`media_copy`).
///
/// # Safety
/// `e` is NULL or a valid `HxChatEvent*` with glib-owned `line`/`media`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_event_copy(e: *mut HxChatEvent) -> *mut HxChatEvent {
    if e.is_null() {
        return ptr::null_mut();
    }
    let c = g_malloc0(size_of::<HxChatEvent>()) as *mut HxChatEvent;
    (*c).cid = (*e).cid;
    (*c).uid = (*e).uid;
    (*c).line_len = (*e).line_len;
    (*c).sender_off = (*e).sender_off;
    (*c).sender_len = (*e).sender_len;
    (*c).body_off = (*e).body_off;
    (*c).body_len = (*e).body_len;
    (*c).is_info = (*e).is_info;
    (*c).is_self = (*e).is_self;
    (*c).line = g_strndup((*e).line, (*e).line_len);
    (*c).media = media_copy((*e).media);
    c
}

/// Boxed free func. Mirrors the deleted C `hx_chat_event_free`.
///
/// # Safety
/// `e` is NULL or a valid `HxChatEvent*` with glib-owned `line`/`media`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_event_free(e: *mut HxChatEvent) {
    if e.is_null() {
        return;
    }
    g_free((*e).line as *mut c_void);
    media_free((*e).media);
    g_free(e as *mut c_void);
}

/// `GBoxedCopyFunc`-shaped shim: matches `unsafe extern "C" fn(gpointer)
/// -> gpointer` exactly and delegates to the typed [`hx_chat_event_copy`],
/// so the boxed-type registration needs no `transmute`.
///
/// # Safety
/// `p` is NULL or a valid `HxChatEvent*`.
unsafe extern "C" fn boxed_copy(p: *mut c_void) -> *mut c_void {
    hx_chat_event_copy(p as *mut HxChatEvent) as *mut c_void
}

/// `GBoxedFreeFunc`-shaped shim — see [`boxed_copy`].
///
/// # Safety
/// `p` is NULL or a valid `HxChatEvent*`.
unsafe extern "C" fn boxed_free(p: *mut c_void) {
    hx_chat_event_free(p as *mut HxChatEvent);
}

/// `HX_TYPE_CHAT_EVENT` accessor — the C ABI the old
/// `G_DEFINE_BOXED_TYPE (HxChatEvent, hx_chat_event, …)` exported.
#[no_mangle]
pub extern "C" fn hx_chat_event_get_type() -> GType {
    static TYPE: OnceLock<usize> = OnceLock::new();
    unsafe {
        register_once(
            &TYPE,
            c"HxChatEvent".as_ptr(),
            Some(boxed_copy),
            Some(boxed_free),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe fn cstr(p: *const c_char, len: usize) -> String {
        String::from_utf8_lossy(std::slice::from_raw_parts(p as *const u8, len)).into_owned()
    }

    /// Build a heap `HxChatEvent` the way a C producer would.
    unsafe fn make_event(cid: u32, line: &str, with_media: bool) -> *mut HxChatEvent {
        let e = g_malloc0(size_of::<HxChatEvent>()) as *mut HxChatEvent;
        assert!(!e.is_null(), "g_malloc0 returned NULL");
        (*e).cid = cid;
        (*e).line = g_strndup(line.as_ptr() as *const c_char, line.len());
        (*e).line_len = line.len();
        // Pretend "nick: body" split with nick = first 4 bytes.
        (*e).sender_off = 0;
        (*e).sender_len = 4;
        (*e).body_off = 6;
        (*e).body_len = line.len().saturating_sub(6);
        (*e).is_self = 1;
        if with_media {
            let m = g_malloc0(size_of::<HxChatMedia>()) as *mut HxChatMedia;
            let id: [u8; 4] = [0xDE, 0xAD, 0xBE, 0xEF];
            (*m).id_len = 4;
            let dst = g_malloc(4) as *mut u8;
            ptr::copy_nonoverlapping(id.as_ptr(), dst, 4);
            (*m).id = dst;
            (*m).mime = g_strndup(b"image/png".as_ptr() as *const c_char, 9);
            (*m).mime_len = 9;
            (*m).width = 800;
            (*m).height = 600;
            (*m).width_present = 1;
            (*m).height_present = 1;
            (*e).media = m;
        }
        e
    }

    /// An event's line, and its sender and body where it split.
    unsafe fn read(e: *const HxChatEvent) -> (String, Option<(String, String)>, bool) {
        let line = cstr((*e).line, (*e).line_len);
        let part = |off: usize, len: usize| line[off..off + len].to_string();
        let split = ((*e).sender_len > 0).then(|| {
            (
                part((*e).sender_off, (*e).sender_len),
                part((*e).body_off, (*e).body_len),
            )
        });
        (line, split, (*e).is_self != 0)
    }

    #[test]
    fn a_line_splits_into_its_sender_and_body() {
        let split = |s: &str, b: &str| Some((s.to_string(), b.to_string()));
        let cases = [
            (
                " misha:  hello world",
                "",
                split("misha", "hello world"),
                false,
            ),
            (
                "  Alice Cooper:  rock",
                "",
                split("Alice Cooper", "rock"),
                false,
            ),
            (
                " bob:  see http://x.org",
                "",
                split("bob", "see http://x.org"),
                false,
            ),
            (
                "misha: héllo wörld",
                "",
                split("misha", "héllo wörld"),
                false,
            ),
            (" misha:  ", "", split("misha", ""), false),
            ("misha: hi all", "misha", split("misha", "hi all"), true),
            ("alice: hi all", "misha", split("alice", "hi all"), false),
            ("misha: hi", "mish", split("misha", "hi"), false),
            ("*** misha waves", "", None, false),
            (" : oops", "", None, false),
            ("      ", "", None, false),
            (
                " the long preamble I wrote before: was here",
                "",
                None,
                false,
            ),
            ("", "misha", None, false),
        ];
        for (line, me, want, mine) in cases {
            unsafe {
                let e = chat_event_new(3, 4242, line, None, me.as_bytes(), false);
                assert_eq!(read(e), (line.to_string(), want, mine), "{line:?}");
                assert_eq!(((*e).cid, (*e).uid), (3, 4242));
                assert!((*e).media.is_null());
                hx_chat_event_free(e);
            }
        }
    }

    #[test]
    fn shortcodes_become_emoji_when_asked() {
        let cases = [
            (
                " misha:  :tada: party",
                true,
                " misha:  🎉 party",
                Some("misha"),
            ),
            (" bob:  hi :fire:", true, " bob:  hi 🔥", Some("bob")),
            (":tada: everyone", true, "🎉 everyone", None),
            (
                " m:  at 10:30, 4:3, :notacode:",
                true,
                " m:  at 10:30, 4:3, :notacode:",
                Some("m"),
            ),
            (
                " misha:  :tada: party",
                false,
                " misha:  :tada: party",
                Some("misha"),
            ),
        ];
        for (text, shortcodes, want, sender) in cases {
            unsafe {
                let e = chat_event_new(0, 0, text, None, b"", shortcodes);
                let (line, split, _) = read(e);
                assert_eq!(line, want, "{text:?}");
                assert_eq!(split.map(|s| s.0).as_deref(), sender, "{text:?}");
                hx_chat_event_free(e);
            }
        }
    }

    #[test]
    fn media_rides_along_when_it_names_a_picture() {
        let png = hxsession::ChatMedia {
            id: vec![0xAB, 0xCD],
            mime: b"image/png".to_vec(),
            width: Some(800),
            height: None,
            bytes: Some(124_000),
        };
        let no_id = hxsession::ChatMedia {
            id: vec![],
            ..png.clone()
        };
        unsafe {
            let e = chat_event_new(0, 0, "alice: look", Some(&png), b"", false);
            let m = &*(*e).media;
            assert_eq!(std::slice::from_raw_parts(m.id, m.id_len), [0xAB, 0xCD]);
            assert_eq!(cstr(m.mime, m.mime_len), "image/png");
            assert_eq!((m.width, m.width_present, m.height_present), (800, 1, 0));
            assert_eq!((m.bytes, m.bytes_present), (124_000, 1));
            hx_chat_event_free(e);

            let e = chat_event_new(0, 0, "alice: look", Some(&no_id), b"", false);
            assert!((*e).media.is_null());
            hx_chat_event_free(e);
        }
    }

    #[test]
    fn get_type_is_a_registered_boxed_type() {
        let t = hx_chat_event_get_type();
        assert_ne!(t, 0);
        assert_eq!(t, hx_chat_event_get_type());
        let ty: glib::Type = unsafe { glib::translate::from_glib(t) };
        assert!(ty.is_a(glib::Type::BOXED));
    }

    #[test]
    fn copy_deep_copies_line_and_media() {
        unsafe {
            let a = make_event(7, "alice: hello there", true);
            assert!(!a.is_null());
            let b = hx_chat_event_copy(a);
            assert!(!b.is_null());
            assert_ne!(a, b);
            // line is a distinct allocation with the same bytes.
            assert_ne!((*a).line, (*b).line);
            assert_eq!(cstr((*b).line, (*b).line_len), "alice: hello there");
            // scalar fields carried.
            assert_eq!((*b).cid, 7);
            assert_eq!((*b).sender_len, 4);
            assert_eq!((*b).body_off, 6);
            assert_eq!((*b).is_self, 1);
            // media deep-copied: distinct struct + distinct id/mime.
            assert!(!(*b).media.is_null());
            assert_ne!((*a).media, (*b).media);
            let ma = &*(*a).media;
            let mb = &*(*b).media;
            assert_ne!(ma.id, mb.id);
            assert_ne!(ma.mime, mb.mime);
            assert_eq!(mb.id_len, 4);
            assert_eq!(
                std::slice::from_raw_parts(mb.id, 4),
                &[0xDE, 0xAD, 0xBE, 0xEF]
            );
            assert_eq!(cstr(mb.mime, mb.mime_len), "image/png");
            assert_eq!(mb.width, 800);
            assert_eq!(mb.height_present, 1);
            // Free original; copy must remain intact.
            hx_chat_event_free(a);
            assert_eq!(cstr((*b).line, (*b).line_len), "alice: hello there");
            assert_eq!(
                cstr((*(*b).media).mime, (*(*b).media).mime_len),
                "image/png"
            );
            hx_chat_event_free(b);
        }
    }

    #[test]
    fn copy_handles_media_absent() {
        unsafe {
            let a = make_event(0, "no media here", false);
            assert!(!a.is_null());
            let b = hx_chat_event_copy(a);
            assert!(!b.is_null());
            assert!((*b).media.is_null());
            assert_eq!(cstr((*b).line, (*b).line_len), "no media here");
            hx_chat_event_free(a);
            hx_chat_event_free(b);
        }
    }

    #[test]
    fn copy_and_free_are_null_safe() {
        unsafe {
            assert!(hx_chat_event_copy(ptr::null_mut()).is_null());
            hx_chat_event_free(ptr::null_mut());
        }
    }

    #[test]
    fn g_boxed_copy_roundtrips_through_the_registered_funcs() {
        unsafe {
            let t = hx_chat_event_get_type();
            let a = make_event(3, "bob: hi", true);
            assert!(!a.is_null());
            let b = glib::gobject_ffi::g_boxed_copy(t, a as *mut c_void) as *mut HxChatEvent;
            assert!(!b.is_null());
            assert_ne!(a, b);
            assert_eq!(cstr((*b).line, (*b).line_len), "bob: hi");
            assert_eq!(
                cstr((*(*b).media).mime, (*(*b).media).mime_len),
                "image/png"
            );
            glib::gobject_ffi::g_boxed_free(t, b as *mut c_void);
            hx_chat_event_free(a);
        }
    }
}
