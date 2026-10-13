//! The pictures a 1.5 news article lists after its text, shown below it.
//!
//! A server lists each picture as a part of its own, named by its MIME type
//! (hxd-ng does, for the pictures attached on its ng wire), and serves it to
//! a GETTHREAD naming that type. Each is fetched the first time its post is
//! shown, decoded under the inline-media limits, and kept on the post's node
//! for as long as the tree holds it.

use std::cell::RefCell;
use std::ffi::{c_char, c_void, CStr};

use glib::translate::from_glib_none;
use gtk::prelude::*;
use gtk::{gdk, glib};
use gtk4 as gtk;

use hx_image_decode::ffi::HxInlineMediaCaps;
use hx_image_decode::{decode_first_frame_async, ImageDecodeOutcome};
use hxmodel::news::node::HxNewsNode;

use crate::dock::{self, ConnKey};
use crate::tr::tr;

/// The widest a picture is shown, however wide the pane.
const MAX_WIDTH: i32 = 480;
const PICTURES: &str = "hx-news-pictures";

extern "C" {
    fn hx_news_node_postid(node: *mut c_void) -> u32;
    fn hx_news_node_path(node: *mut c_void) -> *const c_char;
    fn gtkhx_active_htlc() -> *mut c_void;
}

enum Picture {
    Unfetched,
    Fetching,
    Shown(gdk::Texture),
    Failed,
}

/// The node's pictures, one for each type it lists.
fn pictures(node: &HxNewsNode) -> &RefCell<Vec<Picture>> {
    // SAFETY: the key is only ever set to this type, here.
    unsafe {
        if node.data::<RefCell<Vec<Picture>>>(PICTURES).is_none() {
            let all: Vec<Picture> = node.pictures().iter().map(|_| Picture::Unfetched).collect();
            node.set_data(PICTURES, RefCell::new(all));
        }
        node.data::<RefCell<Vec<Picture>>>(PICTURES)
            .unwrap()
            .as_ref()
    }
}

/// Append `node`'s pictures below the text in `buffer`, fetching any not yet
/// asked for.
///
/// # Safety
/// Main thread; `node` is a live `HxNewsNode *`.
pub unsafe fn append(view: &gtk::TextView, node: *mut c_void) {
    let obj: glib::Object = from_glib_none(node as *mut glib::gobject_ffi::GObject);
    let Ok(n) = obj.downcast::<HxNewsNode>() else {
        return;
    };
    let types = n.pictures();
    if types.is_empty() {
        return;
    }
    let buffer = view.buffer();
    for (i, mime) in types.iter().enumerate() {
        let mut end = buffer.end_iter();
        if buffer.char_count() > 0 {
            buffer.insert(&mut end, "\n\n");
        }
        let shown = match &pictures(&n).borrow()[i] {
            Picture::Shown(t) => Some(t.clone()),
            _ => None,
        };
        match shown {
            Some(texture) => {
                let anchor = buffer.create_child_anchor(&mut end);
                let room = view.width() - view.left_margin() - view.right_margin();
                view.add_child_at_anchor(&fitted(&texture, room), &anchor);
            }
            None => {
                let failed = matches!(pictures(&n).borrow()[i], Picture::Failed);
                buffer.insert(
                    &mut end,
                    &if failed {
                        tr("Couldn't show this picture.")
                    } else {
                        tr("Loading picture…")
                    },
                );
                fetch(&n, i, mime);
            }
        }
    }
}

/// `texture` no wider than `room` or [`MAX_WIDTH`], never scaled up: a
/// picture wider than the view would be cut off at its edge.
fn fitted(texture: &gdk::Texture, room: i32) -> gtk::Picture {
    let picture = gtk::Picture::for_paintable(texture);
    let (w, h) = (texture.width().max(1), texture.height().max(1));
    let width = w.min(MAX_WIDTH).min(room.max(32));
    picture.set_size_request(width, h * width / w);
    picture.set_can_shrink(true);
    picture.update_property(&[gtk::accessible::Property::Label(&tr("Picture"))]);
    picture
}

/// Ask for picture `i` of `node`, of type `mime`, unless it has been.
unsafe fn fetch(node: &HxNewsNode, i: usize, mime: &CStr) {
    if !matches!(pictures(node).borrow()[i], Picture::Unfetched) {
        return;
    }
    let raw = node.as_ptr() as *mut c_void;
    let path = hx_news_node_path(raw);
    let htlc = gtkhx_active_htlc();
    if path.is_null() || htlc.is_null() {
        return;
    }
    let conn = dock::conn_key(htlc);
    let weak = node.downgrade();
    let done = Box::new(move |data: Option<&[u8]>| {
        let Some(node) = weak.upgrade() else {
            return;
        };
        let Some(data) = data else {
            settle(conn, &node, i, Picture::Failed);
            return;
        };
        let weak = node.downgrade();
        decode_first_frame_async(data, HxInlineMediaCaps::default(), move |outcome| {
            if let Some(node) = weak.upgrade() {
                let picture = match outcome {
                    ImageDecodeOutcome::Ok(t) => Picture::Shown(t),
                    ImageDecodeOutcome::Err { .. } => Picture::Failed,
                };
                settle(conn, &node, i, picture);
            }
        });
    });
    if hxhandlers::send::news::get_part(
        htlc,
        CStr::from_ptr(path).to_bytes(),
        hx_news_node_postid(raw),
        mime.to_bytes(),
        done,
    ) {
        pictures(node).borrow_mut()[i] = Picture::Fetching;
    }
}

/// Picture `i` of `node` came to `picture`: show it if the post is.
fn settle(conn: ConnKey, node: &HxNewsNode, i: usize, picture: Picture) {
    pictures(node).borrow_mut()[i] = picture;
    crate::news_browser::rerender_if_selected(conn, node.as_ptr() as *mut c_void);
}
