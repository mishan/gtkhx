//! Send-path unit tests for the files RPC senders. The native `hxproto::build`
//! builders run for real; the C send-path primitives — `path_to_hldir`, the text
//! encoder, the capability probe, the task table, the write primitive and the
//! transfer constructors — are stubbed here, recording what each sender handed
//! them, so the cargo-test build links no C. Mirrors hxhandlers::send::news.

use super::*;
use std::cell::RefCell;

// Data-chunk tags the assertions pin (hotline.h).
const TAG_HTXF_SIZE: u16 = 0x006c;
const TAG_FILE_NAME: u16 = 0x00c9;
const TAG_DIR: u16 = 0x00ca;
const TAG_FILE_RENAME: u16 = 0x00d3;
const TAG_DIR_RENAME: u16 = 0x00d4;
const TAG_FILE_NFILES: u16 = 0x00dc;

type Chunks = Vec<(u16, Vec<u8>)>;

#[derive(Debug)]
struct Task {
    rcv: bool,
    ptr: usize,
    label: String,
}

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
    static TASKS: RefCell<Vec<Task>> = const { RefCell::new(Vec::new()) };
    static XFERS: RefCell<Vec<Xfer>> = const { RefCell::new(Vec::new()) };
    static HANDLES: RefCell<Vec<*mut HtxfHandle>> = const { RefCell::new(Vec::new()) };
}

// ---- C send-path stubs (files.rs imports these under cfg(test)) ------------

/// Encodes a path as `<is_file>:<path>` so the assertions can see both what
/// the sender asked for and which flavor.
pub(crate) unsafe fn path_to_hldir(
    path: *const c_char,
    hldirlen: *mut u16,
    is_file: c_int,
) -> *mut u8 {
    let mut v = format!("{is_file}:").into_bytes();
    v.extend_from_slice(CStr::from_ptr(path).to_bytes());
    let buf = glib::ffi::g_malloc(v.len()) as *mut u8;
    std::ptr::copy_nonoverlapping(v.as_ptr(), buf, v.len());
    *hldirlen = v.len() as u16;
    buf
}

/// Uppercases, so the tests can tell wire bytes from raw ones.
pub(crate) unsafe fn gtkhx_text_for_wire(
    text: *const c_char,
    len: usize,
    _utf8_mode: glib::ffi::gboolean,
    _is_body: glib::ffi::gboolean,
    out_len: *mut usize,
) -> *mut c_char {
    let v: Vec<u8> = slice_bytes(text, len).to_ascii_uppercase();
    let buf = glib::ffi::g_malloc(v.len() + 1) as *mut u8;
    std::ptr::copy_nonoverlapping(v.as_ptr(), buf, v.len());
    *buf.add(v.len()) = 0;
    *out_len = v.len();
    buf as *mut c_char
}

pub(crate) unsafe fn hx_conn_has_cap(_htlc: *const c_void, _cap: u64) -> glib::ffi::gboolean {
    glib::ffi::GTRUE
}

pub(crate) unsafe fn task_new(
    _htlc: *mut c_void,
    rcv: hxtask::RcvTaskFn,
    ptr: *mut c_void,
    _data: *mut c_void,
    label: *const c_char,
) -> *mut c_void {
    let label = CStr::from_ptr(label).to_string_lossy().into_owned();
    TASKS.with_borrow_mut(|t| {
        t.push(Task {
            rcv: rcv.is_some(),
            ptr: ptr as usize,
            label,
        })
    });
    std::ptr::null_mut()
}

pub(crate) unsafe fn hlwrite_chunks(
    _htlc: *mut c_void,
    ty: u32,
    _flag: u32,
    chunks: *const HxChunk,
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
    TASKS.with_borrow_mut(|t| {
        for task in t.drain(..) {
            // hx_file_info hands the task a g_strdup'd label.
            if task.label == "finfo" {
                unsafe { glib::ffi::g_free(task.ptr as *mut c_void) };
            }
        }
    });
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

fn task_labels() -> Vec<String> {
    TASKS.with_borrow(|t| t.iter().map(|t| t.label.clone()).collect())
}

fn htlc() -> *mut c_void {
    std::ptr::NonNull::<u8>::dangling().as_ptr() as *mut c_void
}

fn cs(s: &str) -> CString {
    CString::new(s).unwrap()
}

fn chunk(tag: u16, data: &[u8]) -> (u16, Vec<u8>) {
    (tag, data.to_vec())
}

// ---- tests ----------------------------------------------------------------------

#[test]
fn mkdir_sends_the_whole_path_as_a_directory() {
    reset();
    let p = cs("/a/new");
    unsafe { hx_make_dir(htlc(), p.as_ptr() as *mut c_char) };
    assert_eq!(
        sends(),
        vec![(HTLC_HDR_FILE_MKDIR, vec![chunk(TAG_DIR, b"0:/a/new")])]
    );
    assert_eq!(task_labels(), ["mkdir"]);
    reset();
}

#[test]
fn delete_splits_the_name_off_the_directory() {
    reset();
    let p = cs("/a/b/file");
    unsafe { hx_file_delete(htlc(), p.as_ptr() as *mut c_char) };
    assert_eq!(
        sends(),
        vec![(
            HTLC_HDR_FILE_DELETE,
            vec![
                chunk(TAG_FILE_NAME, b"FILE"),
                chunk(TAG_DIR, b"1:/a/b/file")
            ]
        )]
    );
    assert_eq!(task_labels(), ["rm"]);
    reset();
}

#[test]
fn delete_of_a_bare_name_sends_no_directory() {
    reset();
    let p = cs("file");
    unsafe { hx_file_delete(htlc(), p.as_ptr() as *mut c_char) };
    assert_eq!(
        sends(),
        vec![(HTLC_HDR_FILE_DELETE, vec![chunk(TAG_FILE_NAME, b"FILE")])]
    );
    reset();
}

#[test]
fn getinfo_labels_the_task_with_the_full_path() {
    reset();
    let dir = cs("/pub");
    unsafe { hx_file_info(htlc(), dir.as_ptr(), c"song".as_ptr(), 4) };
    assert_eq!(
        sends(),
        vec![(
            HTLC_HDR_FILE_GETINFO,
            vec![chunk(TAG_FILE_NAME, b"SONG"), chunk(TAG_DIR, b"0:/pub")]
        )]
    );
    TASKS.with_borrow(|t| {
        assert_eq!(t.len(), 1);
        assert!(t[0].rcv);
        let label = unsafe { CStr::from_ptr(t[0].ptr as *const c_char) };
        assert_eq!(label.to_bytes(), b"/pub/song");
    });
    reset();
}

#[test]
fn getinfo_at_the_root_sends_no_directory() {
    reset();
    let dir = cs("/");
    // The name is a byte run, not NUL-terminated: only its length counts.
    unsafe { hx_file_info(htlc(), dir.as_ptr(), c"songs".as_ptr(), 4) };
    assert_eq!(
        sends(),
        vec![(HTLC_HDR_FILE_GETINFO, vec![chunk(TAG_FILE_NAME, b"SONG")])]
    );
    TASKS.with_borrow(|t| {
        let label = unsafe { CStr::from_ptr(t[0].ptr as *const c_char) };
        assert_eq!(label.to_bytes(), b"song");
    });
    reset();
}

#[test]
fn put_file_splits_the_remote_path_for_the_transfer() {
    reset();
    let l = cs("/home/u/a.txt");
    for (rpath, rdir) in [("/up/a.txt", "/up"), ("/a.txt", "/"), ("a.txt", "")] {
        let r = cs(rpath);
        unsafe { hx_put_file(htlc(), l.as_ptr() as *mut c_char, r.as_ptr() as *mut c_char) };
        XFERS.with_borrow(|x| {
            let x = x.last().unwrap();
            assert!(!x.folder);
            assert_eq!(x.lpath, "/home/u/a.txt");
            assert_eq!(x.rdir, rdir, "rpath {rpath}");
            assert_eq!(x.name, b"a.txt");
            assert_eq!(x.ty, XFER_PUT);
        });
    }
    // The transfer sends its own FILE_PUT.
    assert!(sends().is_empty());
    reset();
}

#[test]
fn get_folder_downloads_into_a_folder_of_its_name() {
    reset();
    let root = cs("/home/u/Downloads");
    let rdir = cs("/pub");
    unsafe { hx_get_folder(htlc(), root.as_ptr(), rdir.as_ptr(), c"Album".as_ptr(), 5) };
    XFERS.with_borrow(|x| {
        assert_eq!(x.len(), 1);
        assert!(x[0].folder);
        assert_eq!(x[0].lpath, "/home/u/Downloads/Album");
        assert_eq!(x[0].rdir, "/pub");
        assert_eq!(x[0].name, b"Album");
        assert_eq!(x[0].ty, XFER_GET);
    });
    assert_eq!(
        sends(),
        vec![(
            HTLC_HDR_FILE_GETFOLDER,
            vec![chunk(TAG_FILE_NAME, b"ALBUM"), chunk(TAG_DIR, b"0:/pub")]
        )]
    );
    TASKS.with_borrow(|t| {
        assert_eq!(t[0].label, "xfer_go_folder");
        assert!(t[0].rcv);
        assert_eq!(t[0].ptr, HANDLES.with_borrow(|h| h[0] as usize));
    });
    reset();
}

#[test]
fn get_folder_does_not_double_the_separator_or_send_the_root() {
    reset();
    let root = cs("/tmp/");
    let rdir = cs("/");
    unsafe { hx_get_folder(htlc(), root.as_ptr(), rdir.as_ptr(), c"A".as_ptr(), 1) };
    XFERS.with_borrow(|x| assert_eq!(x[0].lpath, "/tmp/A"));
    assert_eq!(
        sends(),
        vec![(HTLC_HDR_FILE_GETFOLDER, vec![chunk(TAG_FILE_NAME, b"A")])]
    );
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
    std::fs::create_dir_all(tmp.join("sub")).unwrap();
    std::fs::write(tmp.join("a"), [0u8; 10]).unwrap();
    std::fs::write(tmp.join("sub/b"), [0u8; 5]).unwrap();

    let l = cs(tmp.to_str().unwrap());
    let rdir = cs("/up");
    unsafe { hx_put_folder(htlc(), l.as_ptr(), rdir.as_ptr(), c"Tree".as_ptr(), 4) };
    std::fs::remove_dir_all(&tmp).unwrap();

    assert_eq!(
        sends(),
        vec![(
            HTLC_HDR_FILE_PUTFOLDER,
            vec![
                chunk(TAG_FILE_NAME, b"TREE"),
                chunk(TAG_DIR, b"0:/up"),
                chunk(TAG_HTXF_SIZE, &15u32.to_be_bytes()),
                chunk(TAG_FILE_NFILES, &2u32.to_be_bytes()),
            ]
        )]
    );
    HANDLES.with_borrow(|h| assert_eq!(unsafe { (*h[0]).total_size }, 15));
    XFERS.with_borrow(|x| assert_eq!(x[0].ty, XFER_PUT));
    reset();
}

#[test]
fn put_folder_of_an_empty_tree_keeps_a_nonzero_total() {
    reset();
    let tmp = std::env::temp_dir().join(format!("hx-put-empty-{}", std::process::id()));
    std::fs::create_dir_all(&tmp).unwrap();
    let l = cs(tmp.to_str().unwrap());
    let rdir = cs("");
    unsafe { hx_put_folder(htlc(), l.as_ptr(), rdir.as_ptr(), c"E".as_ptr(), 1) };
    std::fs::remove_dir_all(&tmp).unwrap();
    HANDLES.with_borrow(|h| assert_eq!(unsafe { (*h[0]).total_size }, 1));
    let s = sends();
    assert_eq!(s[0].1[1], chunk(TAG_HTXF_SIZE, &0u32.to_be_bytes()));
    reset();
}

#[test]
fn move_across_directories_sends_only_a_move() {
    reset();
    let src = cs("/a/f");
    let dst = cs("/b/f");
    unsafe {
        hx_file_move(
            htlc(),
            src.as_ptr() as *mut c_char,
            dst.as_ptr() as *mut c_char,
        )
    };
    assert_eq!(
        sends(),
        vec![(
            HTLC_HDR_FILE_MOVE,
            vec![
                chunk(TAG_FILE_NAME, b"F"),
                chunk(TAG_DIR, b"1:/a/f"),
                chunk(TAG_DIR_RENAME, b"1:/b/f"),
            ]
        )]
    );
    assert_eq!(task_labels(), ["mv"]);
    reset();
}

#[test]
fn rename_in_place_sends_only_a_setinfo() {
    reset();
    let src = cs("/a/old");
    let dst = cs("/a/new");
    unsafe {
        hx_file_move(
            htlc(),
            src.as_ptr() as *mut c_char,
            dst.as_ptr() as *mut c_char,
        )
    };
    assert_eq!(
        sends(),
        vec![(
            HTLC_HDR_FILE_SETINFO,
            vec![
                chunk(TAG_FILE_NAME, b"OLD"),
                chunk(TAG_FILE_RENAME, b"NEW"),
                chunk(TAG_DIR, b"1:/a/old"),
            ]
        )]
    );
    reset();
}

#[test]
fn move_and_rename_sends_both_the_rename_against_the_source() {
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
    let s = sends();
    assert_eq!(s.len(), 2);
    assert_eq!(s[0].0, HTLC_HDR_FILE_MOVE);
    assert_eq!(s[1].0, HTLC_HDR_FILE_SETINFO);
    assert_eq!(s[1].1[2], chunk(TAG_DIR, b"1:/a/old"));
    assert_eq!(task_labels(), ["mv", "mv"]);
    reset();
}

#[test]
fn move_to_the_same_path_sends_nothing() {
    reset();
    let p = cs("/a/f");
    unsafe { hx_file_move(htlc(), p.as_ptr() as *mut c_char, p.as_ptr() as *mut c_char) };
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
        hx_put_file(htlc(), std::ptr::null_mut(), c"/a".as_ptr() as *mut c_char);
        hx_file_move(htlc(), c"/a".as_ptr() as *mut c_char, std::ptr::null_mut());
    }
    assert!(sends().is_empty());
    assert!(XFERS.with_borrow(Vec::is_empty));
    reset();
}
