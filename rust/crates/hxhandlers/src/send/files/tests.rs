//! Send-path unit tests for the files RPC senders. The request bytes are
//! `hxrequest`'s, pinned there; these check what each sender does around them —
//! the reply it expects, the transfer it starts, the request it writes. The C
//! send-path primitives (the capability probe, the write primitive, the
//! transfer constructors) are stubbed, recording what they were handed, so the
//! cargo-test build links no C.

use super::*;
use std::cell::RefCell;

const TAG_HTXF_SIZE: u16 = 0x006c;

type Chunks = Vec<(u16, Vec<u8>)>;

#[derive(Debug)]
struct Xfer {
    folder: bool,
    lpath: String,
    rdir: String,
    name: Vec<u8>,
    ty: u16,
}

thread_local! {
    static SENDS: RefCell<Vec<(u32, Chunks)>> = const { RefCell::new(Vec::new()) };
    static XFERS: RefCell<Vec<Xfer>> = const { RefCell::new(Vec::new()) };
    static HANDLES: RefCell<Vec<*mut HtxfHandle>> = const { RefCell::new(Vec::new()) };
}

// ---- C send-path stubs (files.rs imports these under cfg(test)) ------------

pub(crate) unsafe fn hx_conn_has_cap(_htlc: *const c_void, _cap: u64) -> glib::ffi::gboolean {
    glib::ffi::GTRUE
}

pub(crate) unsafe fn hlwrite_chunks(
    _htlc: *mut c_void,
    ty: u32,
    _flag: u32,
    chunks: *const hxproto::build::HxChunk,
    hc: c_int,
) {
    let v = (0..hc as usize)
        .map(|i| {
            let ch = &*chunks.add(i);
            let data = if ch.data.is_null() || ch.len == 0 {
                Vec::new()
            } else {
                std::slice::from_raw_parts(ch.data, ch.len as usize).to_vec()
            };
            (ch.tag, data)
        })
        .collect();
    SENDS.with_borrow_mut(|s| s.push((ty, v)));
}

unsafe fn record_xfer(
    folder: bool,
    lpath: *const c_char,
    rdir: *const c_char,
    name: *const c_char,
    name_len: usize,
    ty: u16,
) -> *mut HtxfHandle {
    XFERS.with_borrow_mut(|x| {
        x.push(Xfer {
            folder,
            lpath: CStr::from_ptr(lpath).to_string_lossy().into_owned(),
            rdir: CStr::from_ptr(rdir).to_string_lossy().into_owned(),
            name: slice_bytes(name, name_len).to_vec(),
            ty,
        })
    });
    let h = hxnet::xfer_handle::hx_htxf_new();
    HANDLES.with_borrow_mut(|v| v.push(h));
    h
}

#[allow(clippy::too_many_arguments)]
pub(crate) unsafe fn xfer_new(
    _htlc: *mut c_void,
    lpath: *const c_char,
    rdir: *const c_char,
    name: *const c_char,
    name_len: usize,
    ty: u16,
    _preview: c_int,
    _srv_data_size: u32,
) -> *mut HtxfHandle {
    record_xfer(false, lpath, rdir, name, name_len, ty)
}

pub(crate) unsafe fn xfer_new_folder(
    _htlc: *mut c_void,
    lpath: *const c_char,
    rdir: *const c_char,
    name: *const c_char,
    name_len: usize,
    ty: u16,
) -> *mut HtxfHandle {
    record_xfer(true, lpath, rdir, name, name_len, ty)
}

// ---- helpers ------------------------------------------------------------------

fn reset() {
    SENDS.with_borrow_mut(Vec::clear);
    crate::send::expected::take();
    XFERS.with_borrow_mut(Vec::clear);
    HANDLES.with_borrow_mut(|v| {
        for h in v.drain(..) {
            unsafe { hxnet::xfer_handle::hx_htxf_free(h) };
        }
    });
}

fn sends() -> Vec<(u32, Chunks)> {
    SENDS.with_borrow_mut(std::mem::take)
}

/// What each request expected, in order.
fn expected() -> Vec<Expect> {
    crate::send::expected::take()
        .into_iter()
        .map(|(_, e)| e)
        .collect()
}

/// The trans the last request went out on.
fn last_trans() -> u32 {
    crate::send::expected::SAID.with(|s| s.borrow().last().unwrap().0)
}

fn htlc() -> *mut c_void {
    std::ptr::NonNull::<u8>::dangling().as_ptr() as *mut c_void
}

fn cs(s: &str) -> CString {
    CString::new(s).unwrap()
}

fn sent(req: Option<Request>) -> (u32, Chunks) {
    let req = req.unwrap();
    (req.opcode, req.chunks)
}

// ---- tests ----------------------------------------------------------------------

/// A change expects nothing but whether it went through, and has no Tasks row.
#[test]
fn changes_write_their_request_and_expect_a_change() {
    reset();
    let (dir, file) = (cs("/a/new"), cs("/a/b/caf\u{e9}"));
    unsafe {
        hx_make_dir(htlc(), dir.as_ptr() as *mut c_char);
        hx_file_delete(htlc(), file.as_ptr() as *mut c_char);
    }
    assert_eq!(
        sends(),
        vec![
            sent(files::mkdir(b"/a/new")),
            sent(files::delete("/a/b/caf\u{e9}".as_bytes())),
        ]
    );
    assert_eq!(expected(), [Expect::FileChange, Expect::FileChange]);
    reset();
}

#[test]
fn a_listing_is_expected_for_its_provider() {
    reset();
    let dir = cs("/pub");
    unsafe { hx_list_dir(htlc(), dir.as_ptr(), std::ptr::null_mut()) };
    assert_eq!(sends(), vec![sent(files::list(b"/pub"))]);
    assert_eq!(expected(), [Expect::FileList]);
    reset();
}

/// Get Info's dialog is for the file the request named: its folder and its
/// name, or the name alone at the root. The name is a byte run, not
/// NUL-terminated: only its length counts.
#[test]
fn getinfo_answers_into_a_dialog_for_the_path_it_asked_about() {
    let info = hxsession::FileInfo {
        name: "song".into(),
        kind: String::new(),
        creator: String::new(),
        comment: String::new(),
        size: 0,
        created: [0; 8],
        modified: [0; 8],
    };
    for (dir, label) in [("/pub", &b"/pub/song"[..]), ("/", b"song")] {
        reset();
        crate::recv::xfer::test_env::reset();
        let d = cs(dir);
        unsafe { hx_file_info(htlc(), d.as_ptr(), c"songs".as_ptr(), 4) };
        assert_eq!(
            sends(),
            vec![sent(files::get_info(dir.as_bytes(), b"song"))]
        );
        let trans = last_trans();
        assert_eq!(expected(), [Expect::FileInfo]);
        unsafe { crate::recv::files::info(htlc(), trans, &info) };
        let shown = crate::recv::xfer::test_env::FILE_INFO.with(|c| c.borrow_mut().take());
        assert_eq!(shown.unwrap().0, label);
    }
    reset();
}

/// A local file uploads under its own name, encoded as the connection sends
/// text; the transfer sends its own FILE_PUT.
#[test]
fn put_file_names_the_upload_after_the_local_file() {
    reset();
    let l = cs("/home/u/caf\u{e9}.txt");
    for rdir in ["/up", "/", ""] {
        let r = cs(rdir);
        unsafe { hx_put_file(htlc(), l.as_ptr(), r.as_ptr()) };
        XFERS.with_borrow(|x| {
            let x = x.last().unwrap();
            assert!(!x.folder);
            assert_eq!(x.lpath, "/home/u/caf\u{e9}.txt");
            assert_eq!(x.rdir, rdir);
            // The stub says the connection negotiated UTF-8.
            assert_eq!(x.name, "caf\u{e9}.txt".as_bytes());
            assert_eq!(x.ty, XFER_PUT);
        });
    }
    assert!(sends().is_empty());
    reset();
}

#[test]
fn get_folder_downloads_into_the_local_folder_it_is_given() {
    reset();
    let lpath = cs("/home/u/Downloads/Album");
    let rdir = cs("/pub");
    unsafe { hx_get_folder(htlc(), lpath.as_ptr(), rdir.as_ptr(), c"Album".as_ptr(), 5) };
    XFERS.with_borrow(|x| {
        assert_eq!(x.len(), 1);
        assert!(x[0].folder);
        assert_eq!(x[0].lpath, "/home/u/Downloads/Album");
        assert_eq!(x[0].rdir, "/pub");
        assert_eq!(x[0].name, b"Album");
        assert_eq!(x[0].ty, XFER_GET);
    });
    assert_eq!(sends(), vec![sent(files::get_folder(b"/pub", b"Album"))]);
    assert_eq!(expected(), [Expect::Transfer]);
    reset();
}

#[test]
fn get_folder_with_no_name_does_nothing() {
    reset();
    let root = cs("/tmp");
    unsafe { hx_get_folder(htlc(), root.as_ptr(), root.as_ptr(), c"".as_ptr(), 0) };
    assert!(sends().is_empty());
    assert!(XFERS.with_borrow(Vec::is_empty));
    reset();
}

#[test]
fn put_folder_reports_the_tree_size_and_file_count() {
    reset();
    let tmp = std::env::temp_dir().join(format!("hx-put-folder-{}", std::process::id()));
    let tree = tmp.join("Tree");
    std::fs::create_dir_all(tree.join("sub")).unwrap();
    std::fs::write(tree.join("a"), [0u8; 10]).unwrap();
    std::fs::write(tree.join("sub/b"), [0u8; 5]).unwrap();
    #[cfg(unix)]
    std::os::unix::fs::symlink(tree.join("a"), tree.join("link")).unwrap();

    let l = cs(tree.to_str().unwrap());
    let rdir = cs("/up");
    unsafe { hx_put_folder(htlc(), l.as_ptr(), rdir.as_ptr()) };
    std::fs::remove_dir_all(&tmp).unwrap();

    // The symlink is neither followed nor counted.
    assert_eq!(
        sends(),
        vec![sent(files::put_folder(b"/up", b"Tree", 15, 2))]
    );
    assert_eq!(expected(), [Expect::Transfer]);
    HANDLES.with_borrow(|h| assert_eq!(unsafe { (*h[0]).total_size }, 15));
    XFERS.with_borrow(|x| assert_eq!((x[0].ty, &x[0].name[..]), (XFER_PUT, &b"Tree"[..])));
    reset();
}

#[test]
fn put_folder_of_an_empty_tree_keeps_a_nonzero_total() {
    reset();
    let tmp = std::env::temp_dir().join(format!("hx-put-empty-{}", std::process::id()));
    std::fs::create_dir_all(&tmp).unwrap();
    let l = cs(tmp.to_str().unwrap());
    let rdir = cs("");
    unsafe { hx_put_folder(htlc(), l.as_ptr(), rdir.as_ptr()) };
    std::fs::remove_dir_all(&tmp).unwrap();
    HANDLES.with_borrow(|h| assert_eq!(unsafe { (*h[0]).total_size }, 1));
    let s = sends();
    assert_eq!(s[0].1[1], (TAG_HTXF_SIZE, 0u32.to_be_bytes().to_vec()));
    reset();
}

#[test]
fn a_plain_move_or_rename_is_one_request() {
    for (src, dst) in [("/a/f", "/b/f"), ("/a/old", "/a/new")] {
        reset();
        let (s, d) = (cs(src), cs(dst));
        unsafe { hx_file_move(htlc(), s.as_ptr() as *mut c_char, d.as_ptr() as *mut c_char) };
        let want: Vec<_> = files::moves(src.as_bytes(), dst.as_bytes())
            .into_iter()
            .map(|r| (r.opcode, r.chunks))
            .collect();
        assert_eq!(sends(), want);
        assert_eq!(expected(), [Expect::FileChange]);
    }
    reset();
}

/// Start a move-and-rename; returns the requests it should come to, and the
/// trans the move went out on.
fn start_move_and_rename() -> (Vec<(u32, Chunks)>, u32) {
    reset();
    let src = cs("/a/old");
    let dst = cs("/bb/new");
    unsafe {
        hx_file_move(
            htlc(),
            src.as_ptr() as *mut c_char,
            dst.as_ptr() as *mut c_char,
        )
    };
    let want = files::moves(b"/a/old", b"/bb/new")
        .into_iter()
        .map(|r| (r.opcode, r.chunks))
        .collect();
    (want, last_trans())
}

#[test]
fn a_move_and_rename_renames_only_after_the_move_goes_through() {
    let (want, trans) = start_move_and_rename();
    assert_eq!(want.len(), 2);
    // Only the move goes out at first.
    assert_eq!(sends(), want[..1]);

    unsafe { crate::recv::files::changed(htlc(), trans) };
    assert_eq!(sends(), want[1..]);
    assert_eq!(expected(), [Expect::FileChange, Expect::FileChange]);
    reset();
}

#[test]
fn a_refused_move_sends_no_rename() {
    let (_, trans) = start_move_and_rename();
    sends();
    unsafe {
        crate::recv::files::failed(htlc(), trans);
        crate::recv::files::changed(htlc(), trans);
    }
    assert!(sends().is_empty());
    reset();
}

#[test]
fn null_arguments_send_nothing() {
    reset();
    unsafe {
        hx_make_dir(std::ptr::null_mut(), c"/a".as_ptr() as *mut c_char);
        hx_file_delete(htlc(), std::ptr::null_mut());
        hx_file_info(std::ptr::null_mut(), c"/".as_ptr(), c"a".as_ptr(), 1);
        hx_put_file(htlc(), std::ptr::null(), c"/a".as_ptr());
        hx_file_move(htlc(), c"/a".as_ptr() as *mut c_char, std::ptr::null_mut());
        hx_list_dir(htlc(), std::ptr::null(), std::ptr::null_mut());
    }
    assert!(sends().is_empty());
    assert!(XFERS.with_borrow(Vec::is_empty));
    reset();
}
