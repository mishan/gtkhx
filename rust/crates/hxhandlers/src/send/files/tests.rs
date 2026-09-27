//! Send-path unit tests for the files RPC senders. The request bytes are
//! `hxrequest`'s, pinned there; these check what each sender does around them —
//! the task it registers, the transfer it starts, the request it writes. The C
//! send-path primitives (the capability probe, the task table, the write
//! primitive, the transfer constructors) are stubbed, recording what they were
//! handed, so the cargo-test build links no C.

use super::*;
use std::cell::RefCell;

const TAG_HTXF_SIZE: u16 = 0x006c;

type Chunks = Vec<(u16, Vec<u8>)>;

#[derive(Debug)]
struct Task {
    rcv: hxtask::RcvTaskFn,
    ptr: usize,
    label: String,
    /// The task struct handed back, which a sender may fill in further.
    task: *mut hxtask::Task,
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
    let task = Box::into_raw(Box::new(hxtask::Task {
        trans: 0,
        pos: 0,
        len: 0,
        data: std::ptr::null_mut(),
        str_: std::ptr::null_mut(),
        ptr,
        ptr_free: None,
        rcv,
    }));
    TASKS.with_borrow_mut(|t| {
        t.push(Task {
            rcv,
            ptr: ptr as usize,
            label,
            task,
        })
    });
    task.cast()
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
    let tasks = TASKS.with_borrow_mut(std::mem::take);
    for task in tasks {
        // What hxtask's task_free does: the task's ptr_free, then the task.
        let t = unsafe { Box::from_raw(task.task) };
        if let Some(free) = t.ptr_free {
            unsafe { free(t.ptr) };
        } else if task.label == "finfo" {
            // hx_file_info hands the reply handler a g_strdup'd label.
            unsafe { glib::ffi::g_free(task.ptr as *mut c_void) };
        }
    }
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

fn sent(req: Option<Request>) -> (u32, Chunks) {
    let req = req.unwrap();
    (req.opcode, req.chunks)
}

// ---- tests ----------------------------------------------------------------------

#[test]
fn mkdir_writes_the_request_under_a_reply_less_task() {
    reset();
    let p = cs("/a/new");
    unsafe { hx_make_dir(htlc(), p.as_ptr() as *mut c_char) };
    assert_eq!(sends(), vec![sent(files::mkdir(b"/a/new"))]);
    assert_eq!(task_labels(), ["mkdir"]);
    TASKS.with_borrow(|t| assert!(t[0].rcv.is_none()));
    reset();
}

#[test]
fn delete_writes_the_request() {
    reset();
    let p = cs("/a/b/file");
    unsafe { hx_file_delete(htlc(), p.as_ptr() as *mut c_char) };
    assert_eq!(sends(), vec![sent(files::delete(b"/a/b/file", true))]);
    assert_eq!(task_labels(), ["rm"]);
    reset();
}

#[test]
fn getinfo_labels_the_task_with_the_full_path() {
    reset();
    let dir = cs("/pub");
    unsafe { hx_file_info(htlc(), dir.as_ptr(), c"song".as_ptr(), 4) };
    assert_eq!(sends(), vec![sent(files::get_info(b"/pub", b"song", true))]);
    TASKS.with_borrow(|t| {
        assert_eq!(t.len(), 1);
        assert!(t[0].rcv.is_some());
        let label = unsafe { CStr::from_ptr(t[0].ptr as *const c_char) };
        assert_eq!(label.to_bytes(), b"/pub/song");
    });
    reset();
}

#[test]
fn getinfo_at_the_root_labels_with_the_name_alone() {
    reset();
    let dir = cs("/");
    // The name is a byte run, not NUL-terminated: only its length counts.
    unsafe { hx_file_info(htlc(), dir.as_ptr(), c"songs".as_ptr(), 4) };
    assert_eq!(sends(), vec![sent(files::get_info(b"/", b"song", true))]);
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
        vec![sent(files::get_folder(b"/pub", b"Album", true))]
    );
    TASKS.with_borrow(|t| {
        assert_eq!(t[0].label, "xfer_go_folder");
        assert!(t[0].rcv.is_some());
        assert_eq!(t[0].ptr, HANDLES.with_borrow(|h| h[0] as usize));
    });
    reset();
}

#[test]
fn get_folder_does_not_double_the_separator() {
    reset();
    let root = cs("/tmp/");
    let rdir = cs("/");
    unsafe { hx_get_folder(htlc(), root.as_ptr(), rdir.as_ptr(), c"A".as_ptr(), 1) };
    XFERS.with_borrow(|x| assert_eq!(x[0].lpath, "/tmp/A"));
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
    #[cfg(unix)]
    std::os::unix::fs::symlink(tmp.join("a"), tmp.join("link")).unwrap();

    let l = cs(tmp.to_str().unwrap());
    let rdir = cs("/up");
    unsafe { hx_put_folder(htlc(), l.as_ptr(), rdir.as_ptr(), c"Tree".as_ptr(), 4) };
    std::fs::remove_dir_all(&tmp).unwrap();

    // The symlink is neither followed nor counted.
    assert_eq!(
        sends(),
        vec![sent(files::put_folder(b"/up", b"Tree", 15, 2, true))]
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
    assert_eq!(s[0].1[1], (TAG_HTXF_SIZE, 0u32.to_be_bytes().to_vec()));
    reset();
}

#[test]
fn a_plain_move_or_rename_is_one_request() {
    for (src, dst) in [("/a/f", "/b/f"), ("/a/old", "/a/new")] {
        reset();
        let (s, d) = (cs(src), cs(dst));
        unsafe { hx_file_move(htlc(), s.as_ptr() as *mut c_char, d.as_ptr() as *mut c_char) };
        let want: Vec<_> = files::moves(src.as_bytes(), dst.as_bytes(), true)
            .into_iter()
            .map(|r| (r.opcode, r.chunks))
            .collect();
        assert_eq!(sends(), want);
        assert_eq!(task_labels(), ["mv"]);
        TASKS.with_borrow(|t| assert!(t[0].rcv.is_none()));
    }
    reset();
}

/// A TASK reply frame, flagged as an error or not.
fn reply(error: bool) -> Vec<u8> {
    let mut f = vec![0u8; 22];
    f[0..4].copy_from_slice(&0x0001_0000u32.to_be_bytes());
    f[8..12].copy_from_slice(&u32::from(error).to_be_bytes());
    f[12..16].copy_from_slice(&2u32.to_be_bytes());
    f[16..20].copy_from_slice(&2u32.to_be_bytes());
    f
}

/// Start a move-and-rename; returns the requests it should come to.
fn start_move_and_rename() -> Vec<(u32, Chunks)> {
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
    files::moves(b"/a/old", b"/bb/new", true)
        .into_iter()
        .map(|r| (r.opcode, r.chunks))
        .collect()
}

/// Deliver `frame` as the reply to the first task, as hx_rcv_task would.
fn answer_first_task(frame: &[u8]) {
    let (rcv, ptr) = TASKS.with_borrow(|t| (t[0].rcv.unwrap(), t[0].ptr));
    unsafe {
        rcv(
            htlc(),
            frame.as_ptr() as *const c_void,
            frame.len(),
            ptr as *mut c_void,
            std::ptr::null_mut(),
        )
    };
}

#[test]
fn a_move_and_rename_renames_only_after_the_move_succeeds() {
    let want = start_move_and_rename();
    assert_eq!(want.len(), 2);
    // Only the move goes out at first, its task carrying the rename.
    assert_eq!(sends(), want[..1]);
    TASKS.with_borrow(|t| {
        assert_eq!(t.len(), 1);
        assert!(unsafe { (*t[0].task).ptr_free }.is_some());
    });

    answer_first_task(&reply(false));
    assert_eq!(sends(), want[1..]);
    assert_eq!(task_labels(), ["mv", "mv"]);
    // reset() frees the carried rename through the task's ptr_free.
    reset();
}

#[test]
fn a_failed_move_sends_no_rename() {
    start_move_and_rename();
    sends();
    answer_first_task(&reply(true));
    assert!(sends().is_empty());
    assert_eq!(task_labels(), ["mv"]);
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
