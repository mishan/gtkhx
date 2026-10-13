//! The files browser's requests against real servers.
//!
//! Admin-level operations run where the rig has a file-admin account (see
//! `Cap::FileAdmin`); guest-level ones run everywhere.
#![cfg(feature = "rig")]

use hx_e2e::client::{CAP_LARGE_FILES, CAP_TEXT_ENCODING};
use hx_e2e::{servers_with, Cap, Client, Scratch};
use hxrequest::files;

fn admins() -> Vec<Client> {
    servers_with(&[Cap::FileAdmin])
        .into_iter()
        .map(|s| Client::admin(s, 0))
        .collect()
}

fn names(c: &mut Client, dir: &str) -> Vec<String> {
    let mut v: Vec<String> = c
        .names(dir)
        .iter()
        .map(|n| hxproto::text::to_utf8(n))
        .collect();
    v.sort();
    v
}

/// `ok` the reply, with the server and its reason when it isn't.
#[track_caller]
fn ok(c: &Client, reply: &hx_e2e::Reply, what: &str) {
    assert!(
        !reply.is_error(),
        "{}: {what} refused: {}",
        c.server().name,
        reply.error_text()
    );
}

#[track_caller]
fn refused(c: &Client, reply: &hx_e2e::Reply, what: &str) {
    assert!(
        reply.is_error(),
        "{}: {what} wasn't refused",
        c.server().name
    );
    assert!(
        !reply.error_text().is_empty(),
        "{}: {what} refused without a reason",
        c.server().name
    );
}

fn mkdir(c: &mut Client, path: &str) {
    let r = c.request(&files::mkdir(path.as_bytes()).unwrap());
    ok(c, &r, &format!("mkdir {path}"));
}

// ---- folders ---------------------------------------------------------------

#[test]
fn a_new_folder_lists_as_an_empty_folder() {
    for mut c in admins() {
        let mut s = Scratch::new(&mut c, "mkdir");
        let dir = s.path().to_string();
        let new = s.join("new");
        mkdir(s.client(), &new);
        let entries = s.client().list(&dir);
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].name, b"new");
        assert!(entries[0].is_folder());
        // A folder's size field is its child count.
        assert_eq!(entries[0].size, 0);
    }
}

#[test]
fn a_folder_lists_its_child_count() {
    for mut c in admins() {
        let mut s = Scratch::new(&mut c, "count");
        let (dir, parent) = (s.path().to_string(), s.join("p"));
        mkdir(s.client(), &parent);
        mkdir(s.client(), &format!("{parent}/one"));
        mkdir(s.client(), &format!("{parent}/two"));
        let entries = s.client().list(&dir);
        assert_eq!(entries[0].size, 2, "{}", s.client().server().name);
    }
}

#[test]
fn mkdir_refuses_an_existing_name_and_a_missing_parent() {
    for mut c in admins() {
        let mut s = Scratch::new(&mut c, "mkdir-refused");
        let (dup, orphan) = (s.join("dup"), s.join("missing/child"));
        mkdir(s.client(), &dup);
        let c = s.client();
        let r = c.request(&files::mkdir(dup.as_bytes()).unwrap());
        refused(c, &r, "a second mkdir of the same name");
        let r = c.request(&files::mkdir(orphan.as_bytes()).unwrap());
        refused(c, &r, "mkdir under a missing parent");
    }
}

#[test]
fn a_255_byte_name_round_trips() {
    for mut c in admins() {
        let mut s = Scratch::new(&mut c, "long");
        let (dir, long) = (s.path().to_string(), "L".repeat(255));
        let path = s.join(&long);
        mkdir(s.client(), &path);
        assert_eq!(names(s.client(), &dir), [long]);
    }
}

#[test]
fn delete_removes_a_folder_and_everything_in_it() {
    for mut c in admins() {
        let mut s = Scratch::new(&mut c, "delete");
        let (dir, doomed) = (s.path().to_string(), s.join("doomed"));
        mkdir(s.client(), &doomed);
        mkdir(s.client(), &format!("{doomed}/inner"));
        let c = s.client();
        let r = c.request(&files::delete(doomed.as_bytes()).unwrap());
        ok(c, &r, "delete");
        assert!(c.names(&dir).is_empty());
    }
}

#[test]
fn replies_come_back_on_their_own_transaction() {
    for mut c in admins() {
        let mut s = Scratch::new(&mut c, "trans");
        let (missing, fresh) = (s.join("missing"), s.join("fresh"));
        let c = s.client();
        // Two requests in flight at once: the failure belongs to the first.
        let t1 = c.send(&files::delete(missing.as_bytes()).unwrap());
        let t2 = c.send(&files::mkdir(fresh.as_bytes()).unwrap());
        let r1 = c.reply_to(t1).unwrap();
        let r2 = c.reply_to(t2).unwrap();
        refused(c, &r1, "delete of a missing path");
        ok(c, &r2, "mkdir");
    }
}

// ---- move and rename ---------------------------------------------------------

#[test]
fn rename_in_place() {
    for mut c in admins() {
        let mut s = Scratch::new(&mut c, "rename");
        let (dir, old) = (s.path().to_string(), s.join("old"));
        mkdir(s.client(), &old);
        let new = s.join("new");
        let r = s.client().move_to(&old, &new);
        ok(s.client(), &r, "rename");
        assert_eq!(names(s.client(), &dir), ["new"]);
    }
}

#[test]
fn move_across_folders_keeps_the_name() {
    for mut c in admins() {
        let mut s = Scratch::new(&mut c, "move");
        let (a, b) = (s.join("a"), s.join("b"));
        mkdir(s.client(), &a);
        mkdir(s.client(), &b);
        mkdir(s.client(), &format!("{a}/item"));
        let r = s
            .client()
            .move_to(&format!("{a}/item"), &format!("{b}/item"));
        ok(s.client(), &r, "move");
        assert!(names(s.client(), &a).is_empty());
        assert_eq!(names(s.client(), &b), ["item"]);
    }
}

#[test]
fn move_and_rename_lands_renamed_at_the_destination() {
    for mut c in admins() {
        let mut s = Scratch::new(&mut c, "move-rename");
        let (a, b) = (s.join("a"), s.join("b"));
        mkdir(s.client(), &a);
        mkdir(s.client(), &b);
        mkdir(s.client(), &format!("{a}/old"));
        let r = s.client().move_to(&format!("{a}/old"), &format!("{b}/new"));
        ok(s.client(), &r, "move and rename");
        assert!(names(s.client(), &a).is_empty());
        assert_eq!(names(s.client(), &b), ["new"]);
    }
}

#[test]
fn a_move_onto_a_non_empty_folder_is_refused_and_both_stay() {
    for mut c in admins() {
        let mut s = Scratch::new(&mut c, "collide");
        let (a, b) = (s.join("a"), s.join("b"));
        mkdir(s.client(), &a);
        mkdir(s.client(), &format!("{a}/item"));
        mkdir(s.client(), &b);
        mkdir(s.client(), &format!("{b}/item"));
        mkdir(s.client(), &format!("{b}/item/keep"));
        let r = s
            .client()
            .move_to(&format!("{a}/item"), &format!("{b}/item"));
        refused(s.client(), &r, "a move onto a non-empty folder");
        assert_eq!(names(s.client(), &a), ["item"]);
        assert_eq!(names(s.client(), &format!("{b}/item")), ["keep"]);
    }
}

// ---- names -----------------------------------------------------------------

/// A folder named in Mac Roman on a server that never agreed to UTF-8 is
/// named back by the bytes its listing gave, everywhere the browser names
/// it: opened, made in, asked about, fetched, renamed and deleted.
#[test]
fn a_mac_roman_folder_is_named_back_by_its_bytes() {
    for mut c in admins() {
        assert!(!c.utf8());
        let mut s = Scratch::new(&mut c, "macroman");
        let dir = s.path().to_string();
        let c = s.client();
        // "café" in Mac Roman, made with the raw bytes.
        let r = c.request(&files::mkdir(&[dir.as_bytes(), b"/caf\x8e"].concat()).unwrap());
        ok(c, &r, "mkdir");
        let listed = c.names(&dir);
        assert_eq!(listed, [b"caf\x8e".to_vec()]);
        assert_eq!(hxproto::text::to_utf8(&listed[0]), "caf\u{e9}");
        let path = [dir.as_bytes(), b"/", &listed[0]].concat();

        let r = c.request(&files::mkdir(&[&path[..], b"/inner"].concat()).unwrap());
        ok(c, &r, "mkdir inside");
        let r = c.request(&files::list(&path).unwrap());
        ok(c, &r, "list inside");
        let inside: Vec<Vec<u8>> = r.file_list().into_iter().map(|e| e.name).collect();
        assert_eq!(inside, [b"inner".to_vec()]);

        let r = c.request(&files::get_info(dir.as_bytes(), &listed[0]).unwrap());
        ok(c, &r, "get info");
        assert_eq!(r.file_info().name, b"caf\x8e");

        let r = c.request(&files::get_folder(dir.as_bytes(), &listed[0]).unwrap());
        ok(c, &r, "folder download");
        c.cancel_transfer(hxproto::parse::parse_folder_get_reply(&r.raw, r.raw.len()).ref_);

        // A new name, typed, goes as the connection sends text.
        let typed = hxproto::text::from_utf8("na\u{ef}ve");
        let r = c.request(&files::moves(&path, &[dir.as_bytes(), b"/", &typed].concat())[0]);
        ok(c, &r, "rename");
        assert_eq!(c.names(&dir), [b"na\x95ve".to_vec()]);

        let r = c.request(&files::delete(&[dir.as_bytes(), b"/na\x95ve"].concat()).unwrap());
        ok(c, &r, "delete");
        assert!(c.names(&dir).is_empty());
    }
}

// ---- get info and comments ---------------------------------------------------

#[test]
fn get_info_describes_a_folder_and_reads_back_its_comment() {
    for mut c in admins() {
        let mut s = Scratch::new(&mut c, "info");
        let (dir, f) = (s.path().to_string(), s.join("f"));
        mkdir(s.client(), &f);
        let c = s.client();
        let utf8 = c.utf8();
        let r = c.request(&files::set_info(f.as_bytes(), None, Some(b"two\nlines"), utf8).unwrap());
        ok(c, &r, "set comment");
        let r = c.request(&files::get_info(dir.as_bytes(), b"f").unwrap());
        ok(c, &r, "get info");
        let info = r.file_info();
        assert_eq!(info.name, b"f");
        // The type string: mhxd sends the type code, Janus a display name
        // (with the code in a separate field the parser doesn't read).
        assert!(
            [&b"fldr"[..], b"Folder"].contains(&info.type_.as_slice()),
            "{}: folder type {:?}",
            c.server().name,
            String::from_utf8_lossy(&info.type_)
        );
        // Sent with CR line ends, read back with LF.
        assert_eq!(info.comment, b"two\nlines");
    }
}

// ---- folder transfers ----------------------------------------------------------

#[test]
fn folder_transfers_are_granted_a_reference() {
    for mut c in admins() {
        let mut s = Scratch::new(&mut c, "xfer");
        let (dir, tree) = (s.path().to_string(), s.join("tree"));
        mkdir(s.client(), &tree);
        mkdir(s.client(), &format!("{tree}/leaf"));
        let c = s.client();

        let r = c.request(&files::get_folder(dir.as_bytes(), b"tree").unwrap());
        ok(c, &r, "folder download");
        let get = hxproto::parse::parse_folder_get_reply(&r.raw, r.raw.len());
        assert_ne!(get.ref_, 0);
        c.cancel_transfer(get.ref_);

        let r = c.request(&files::put_folder(dir.as_bytes(), b"up", 15, 2).unwrap());
        ok(c, &r, "folder upload");
        let put = hxproto::parse::parse_folder_put_reply(&r.raw, r.raw.len());
        assert_ne!(put.ref_, 0);
        c.cancel_transfer(put.ref_);
    }
}

/// mhxd runs one download at a time for each user (`individual_downloads`),
/// and every test here connects from the same address: the tests that move
/// a file's bytes take turns.
fn transfer_turn() -> std::sync::MutexGuard<'static, ()> {
    static TURN: std::sync::Mutex<()> = std::sync::Mutex::new(());
    TURN.lock().unwrap_or_else(|p| p.into_inner())
}

/// Ask for a transfer, and again while mhxd refuses one asked for just
/// after another closes ("1 at a time") until it has seen that one go.
fn granted(c: &mut Client, req: &hxrequest::Request, what: &str) -> hx_e2e::Reply {
    let mut r = c.request(req);
    for _ in 0..30 {
        if !r.is_error() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(100));
        r = c.request(req);
    }
    ok(c, &r, what);
    r
}

/// The transfer connection for `ref_`, opened the way the app opens it.
fn open_xfer(c: &Client, ref_: u32, size: u64, folder: bool) -> *mut hxnet::htxf::HtxfConn {
    use hxnet::htxf::{hxnet_htxf_connect, hxnet_htxf_pack_preamble};
    let mut pre = [0u8; 24];
    let n = unsafe {
        hxnet_htxf_pack_preamble(
            pre.as_mut_ptr(),
            pre.len(),
            ref_,
            size,
            u16::from(folder),
            0,
            0,
        )
    };
    assert!(n > 0, "a preamble");
    let server = c.server();
    let h = unsafe {
        hxnet_htxf_connect(
            server.host.as_ptr(),
            server.host.len(),
            server.xfer_port,
            std::ptr::null(),
            0,
            0,
            pre.as_ptr(),
            n,
            std::ptr::null(),
            ref_,
            None,
            std::ptr::null_mut(),
        )
    };
    assert!(!h.is_null(), "{}: transfer port", server.name);
    h
}

/// A new local directory of the test's own.
fn local_dir(c: &Client, what: &str) -> std::path::PathBuf {
    let dir = std::env::temp_dir().join(hx_e2e::unique_name(&format!(
        "hx-e2e-{what}-{}",
        c.server().name
    )));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

unsafe extern "C" fn no_progress(_u: *mut std::ffi::c_void, _d: u64) {}

/// The params of a solo transfer of the local file `path`, no resume.
fn xfer_params(
    hx: *mut hxnet::htxf::HtxfConn,
    path: &std::ffi::CStr,
    budget: u64,
) -> hxnet::xfer::HxnetXferParams {
    hxnet::xfer::HxnetXferParams {
        hx,
        path: path.as_ptr(),
        file_budget: budget,
        data_pos: 0,
        rsrc_pos: 0,
        opt_preview: 0,
        opt_folder: 0,
        opt_large: 0,
        preview: std::ptr::null_mut(),
        user_data: std::ptr::null_mut(),
        progress: Some(no_progress),
        preview_chunk: None,
        preview_set_info: None,
        preview_done: None,
        data_size: 0,
        rsrc_size: 0,
    }
}

/// Download the file `name` in the root through the app's download worker
/// and return its data fork: as saved, or as a preview is fed it.
fn download_file(c: &mut Client, name: &[u8], preview: bool) -> Vec<u8> {
    use std::ffi::{c_char, c_void, CString};

    unsafe extern "C" fn set_info(
        _p: *mut c_void,
        _t: *const c_char,
        _c: *const c_char,
        len: u64,
    ) -> u64 {
        len
    }
    unsafe extern "C" fn chunk(p: *mut c_void, data: *const c_char, len: usize) {
        let shown = &mut *(p as *mut Vec<u8>);
        shown.extend_from_slice(std::slice::from_raw_parts(data as *const u8, len));
    }

    let r = granted(c, &files::get_from(b"/", name, 0).unwrap(), "a download");
    let get = hxproto::parse::parse_file_get_reply(&r.raw, r.raw.len());
    let budget = u64::from(get.size);
    let h = open_xfer(c, get.ref_, budget, false);
    let dir = local_dir(c, "get");
    let path = dir.join(hxproto::text::to_utf8(name));
    let path_c = CString::new(path.to_str().unwrap()).unwrap();
    let mut shown = Vec::new();
    let mut p = xfer_params(h, &path_c, budget);
    if preview {
        p.opt_preview = 1;
        p.preview = &mut shown as *mut Vec<u8> as *mut c_void;
        p.preview_set_info = Some(set_info);
        p.preview_chunk = Some(chunk);
    }
    let rv = unsafe { hxnet::xfer::hxnet_xfer_file_recv_one(&p) };
    unsafe { hxnet::htxf::hxnet_htxf_close(h) };
    let saved = std::fs::read(&path);
    let _ = std::fs::remove_dir_all(&dir);
    assert_eq!(rv, 0, "{}: the download failed", c.server().name);
    if preview {
        shown
    } else {
        saved.unwrap_or_else(|e| panic!("{}: the downloaded file: {e}", c.server().name))
    }
}

/// Upload `bytes` as the file `name` in `dir` through the app's upload worker.
fn upload_file(c: &mut Client, dir: &str, name: &str, bytes: &[u8]) {
    let local = local_dir(c, "put");
    let path = local.join(name);
    std::fs::write(&path, bytes).unwrap();
    let path_c = std::ffi::CString::new(path.to_str().unwrap()).unwrap();
    upload(c, dir, name.as_bytes(), &path_c, bytes.len() as u64);
    let _ = std::fs::remove_dir_all(&local);
}

/// What a folder download left on disk: each relative path, with a file's
/// contents.
type Arrived = Vec<(String, Option<Vec<u8>>)>;

/// Download the folder `name` in `dir` the way the app does — its reply's
/// item count handed to the connection — into a scratch directory, and
/// return the worker's result, how long it took and what arrived, less the
/// sidecars that keep a file's Finder data.
fn download_folder(c: &mut Client, dir: &str, name: &[u8]) -> (i32, std::time::Duration, Arrived) {
    use hxnet::htxf::{hxnet_htxf_close, hxnet_htxf_set_folder_items};
    use hxnet::xfer::{hxnet_xfer_folder_recv_all, HxnetFolderParams};
    use std::ffi::CString;

    let r = granted(
        c,
        &files::get_folder(dir.as_bytes(), name).unwrap(),
        "folder download",
    );
    let get = hxproto::parse::parse_folder_get_reply(&r.raw, r.raw.len());
    let h = open_xfer(c, get.ref_, u64::from(get.size), true);
    unsafe { hxnet_htxf_set_folder_items(h, get.nfiles) };

    let dest = local_dir(c, "folder");
    let dest_c = CString::new(dest.to_str().unwrap()).unwrap();
    let fp = HxnetFolderParams {
        hx: h,
        base_path: dest_c.as_ptr(),
        opt_preview: 0,
        opt_folder: 1,
        opt_large: 0,
        user_data: std::ptr::null_mut(),
        progress: Some(no_progress),
    };
    let t0 = std::time::Instant::now();
    let rv = unsafe { hxnet_xfer_folder_recv_all(&fp) };
    let took = t0.elapsed();
    unsafe { hxnet_htxf_close(h) };

    fn walk(root: &std::path::Path, at: &std::path::Path, out: &mut Arrived) {
        for e in std::fs::read_dir(at).into_iter().flatten().flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if n.ends_with(".fndrinfo") || n.ends_with(".rsrc") {
                continue;
            }
            let p = e.path();
            let rel = p.strip_prefix(root).unwrap().display().to_string();
            out.push((rel, p.is_file().then(|| std::fs::read(&p).unwrap())));
            walk(root, &p, out);
        }
    }
    let mut got = Vec::new();
    walk(&dest, &dest, &mut got);
    got.sort();
    let _ = std::fs::remove_dir_all(&dest);
    (rv, took, got)
}

/// A folder download ends when the tree has arrived — however the server
/// ends it, and however it counts. Janus closes only on a timeout of its
/// own, ten seconds on; mhxd counts the top level alone.
#[test]
fn a_folder_download_arrives_whole_and_ends_promptly() {
    let _turn = transfer_turn();
    for mut c in admins() {
        let mut s = Scratch::new(&mut c, "fget");
        let (dir, tree) = (s.path().to_string(), s.join("tree"));
        for p in ["", "/a", "/a/x", "/a/x/y", "/b"] {
            mkdir(s.client(), &format!("{tree}{p}"));
        }
        upload_file(
            s.client(),
            &format!("{tree}/a/x"),
            "f.txt",
            b"in a folder\n",
        );
        let c = s.client();
        let (rv, took, got) = download_folder(c, &dir, b"tree");
        let name = c.server().name;
        assert_eq!(rv, 0, "{name}: the download failed");
        let folder = |p: &str| (p.to_string(), None);
        assert_eq!(
            got,
            [
                folder("a"),
                folder("a/x"),
                ("a/x/f.txt".to_string(), Some(b"in a folder\n".to_vec())),
                folder("a/x/y"),
                folder("b"),
            ],
            "{name}: the tree"
        );
        assert!(
            took < std::time::Duration::from_secs(5),
            "{name}: took {took:?} to end"
        );
    }
}

// ---- as a guest, on every server ---------------------------------------------

#[test]
fn a_guest_can_list_the_root_and_get_info_on_what_is_there() {
    for s in servers_with(&[]) {
        for caps in [0, CAP_TEXT_ENCODING] {
            let mut c = Client::guest(s, caps);
            let entries = c.list("/");
            assert!(!entries.is_empty(), "{}: empty root", s.name);
            for e in entries.iter().filter(|e| !e.is_folder()).take(3) {
                // By the name as the server sent it, as the browser names it.
                let r = c.request(&files::get_info(b"/", &e.name).unwrap());
                ok(
                    &c,
                    &r,
                    &format!("get info {:?}", hxproto::text::to_utf8(&e.name)),
                );
                assert_eq!(r.file_info().name, e.name);
            }
        }
    }
}

#[test]
fn a_guest_is_refused_file_management() {
    for s in servers_with(&[]) {
        let mut c = Client::guest(s, 0);
        let path = format!("/{}", hx_e2e::unique_name("guest"));
        let r = c.request(&files::mkdir(path.as_bytes()).unwrap());
        refused(&c, &r, "a guest mkdir");
    }
}

#[test]
fn utf8_and_large_files_are_negotiated_where_offered() {
    for s in servers_with(&[Cap::TextEncoding, Cap::LargeFiles]) {
        let c = Client::guest(s, CAP_TEXT_ENCODING | CAP_LARGE_FILES);
        assert_eq!(c.caps(), CAP_TEXT_ENCODING | CAP_LARGE_FILES, "{}", s.name);
        assert!(c.utf8());
    }
    for s in servers_with(&[]) {
        assert_eq!(Client::guest(s, 0).caps(), 0, "{}", s.name);
    }
}

/// The data fork of `name` in the root, from `from` on, as a download
/// resumed there delivers it.
fn data_from(c: &mut Client, name: &[u8], from: u32) -> Vec<u8> {
    use std::io::{Read, Write};
    let r = granted(c, &files::get_from(b"/", name, from).unwrap(), "a download");
    let get = hxproto::parse::parse_file_get_reply(&r.raw, r.raw.len());
    let mut pre = [0u8; 24];
    let n =
        hxproto::build::build_htxf_preamble(&mut pre, get.ref_, u64::from(get.size), 0, 0, false);
    let server = c.server();
    let mut sock = std::net::TcpStream::connect((server.host, server.xfer_port))
        .unwrap_or_else(|e| panic!("{}: transfer port: {e}", server.name));
    sock.set_read_timeout(Some(std::time::Duration::from_secs(15)))
        .unwrap();
    sock.write_all(&pre[..n]).unwrap();
    let mut read = |len: usize| {
        let mut b = vec![0u8; len];
        sock.read_exact(&mut b)
            .unwrap_or_else(|e| panic!("{}: the transfer: {e}", server.name));
        b
    };
    let header = read(24);
    assert_eq!(&header[..4], b"FILP", "{}", server.name);
    for _ in 0..u16::from_be_bytes([header[22], header[23]]) {
        let fork = read(16);
        let len = u32::from_be_bytes([fork[12], fork[13], fork[14], fork[15]]) as usize;
        let bytes = read(len);
        if &fork[..4] == b"DATA" {
            return bytes;
        }
    }
    Vec::new()
}

/// What a media preview's seeks rest on: a download resumed at an offset
/// starts its data fork there, on every server.
#[test]
fn a_resumed_download_starts_where_it_was_asked_to() {
    let _turn = transfer_turn();
    for s in servers_with(&[]) {
        let mut c = Client::guest(s, 0);
        let files: Vec<_> = c.list("/").into_iter().filter(|e| !e.is_folder()).collect();
        let (file, whole) = files
            .iter()
            .map(|f| (f, data_from(&mut c, &f.name, 0)))
            .find(|(_, whole)| whole.len() >= 2)
            .unwrap_or_else(|| panic!("{}: no file in the root with data", s.name));
        let half = (whole.len() / 2) as u32;
        assert_eq!(
            data_from(&mut c, &file.name, half),
            whole[half as usize..],
            "{}",
            s.name
        );
    }
}

// ---- pause and resume ----------------------------------------------------------

/// Open the transfer subchannel for `ref_` as the app's worker does, with a
/// cancellation token armed on it.
fn open_transfer(
    c: &Client,
    ref_: u32,
    size: u64,
) -> (*mut hxnet::htxf::HtxfConn, *const hxnet::htxf::HtxfAbort) {
    use hxnet::htxf::{hxnet_htxf_abort_arm, hxnet_htxf_abort_new};
    let h = open_xfer(c, ref_, size, false);
    let abort = hxnet_htxf_abort_new();
    unsafe { hxnet_htxf_abort_arm(h, abort) };
    (h, abort)
}

/// Upload the local file `path` into `dir` as `name`, as the app's upload does.
fn upload(c: &mut Client, dir: &str, name: &[u8], path: &std::ffi::CStr, len: u64) {
    use hxnet::htxf::{hxnet_htxf_abort_free, hxnet_htxf_close, hxnet_htxf_finish_send};
    let r = c.request(&files::put(dir.as_bytes(), name, len as u32).unwrap());
    ok(c, &r, "an upload");
    let ref_ = hxproto::parse::parse_file_put_reply(&r.raw, r.raw.len()).ref_;
    // The FILP header and the data fork's marker, then the data.
    let (h, abort) = open_transfer(c, ref_, 133 + len);
    let mut p = xfer_params(h, path, 0);
    p.data_size = len;
    assert_eq!(
        unsafe { hxnet::xfer::hxnet_xfer_file_send_one(&p) },
        0,
        "{}: the upload",
        c.server().name
    );
    unsafe {
        hxnet_htxf_finish_send(h);
        hxnet_htxf_close(h);
        hxnet_htxf_abort_free(abort);
    }
}

/// What the progress hook of a download paused partway needs: the token to
/// pull, and when.
struct PauseAt {
    abort: *const hxnet::htxf::HtxfAbort,
    received: u64,
    at: u64,
}

unsafe extern "C" fn pause_when_due(user: *mut std::ffi::c_void, delta: u64) {
    let p = &mut *(user as *mut PauseAt);
    p.received += delta;
    if p.received >= p.at {
        hxnet::htxf::hxnet_htxf_abort(p.abort);
    }
}

/// Download `name` in `dir` into `path` from what is already there, as the
/// app asks for a download it resumes, stopping as a pause does once `pause_at`
/// bytes have come. The worker's result.
fn download_from(
    c: &mut Client,
    dir: &str,
    name: &[u8],
    path: &std::ffi::CStr,
    pause_at: u64,
    (tries, wait_ms): (u32, u64),
) -> i32 {
    use hxnet::htxf::{hxnet_htxf_abort_free, hxnet_htxf_close};
    let local = std::fs::metadata(path.to_str().unwrap()).map_or(0, |m| m.len()) as u32;
    // mhxd refuses a download asked for just after one closes ("1 at a
    // time") until it has seen that one go.
    let req = files::get_from(dir.as_bytes(), name, local).unwrap();
    let mut r = c.request(&req);
    for _ in 0..tries {
        if !r.is_error() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(wait_ms));
        r = c.request(&req);
    }
    ok(c, &r, "a download");
    let get = hxproto::parse::parse_file_get_reply(&r.raw, r.raw.len());
    let (h, abort) = open_transfer(c, get.ref_, u64::from(get.size));
    let mut at = PauseAt {
        abort,
        received: 0,
        at: pause_at,
    };
    let mut p = xfer_params(h, path, u64::from(get.size));
    p.data_pos = u64::from(local);
    p.user_data = (&mut at as *mut PauseAt).cast();
    p.progress = Some(pause_when_due);
    let rv = unsafe { hxnet::xfer::hxnet_xfer_file_recv_one(&p) };
    unsafe {
        hxnet_htxf_close(h);
        hxnet_htxf_abort_free(abort);
    }
    rv
}

/// How often, and how far apart, a refused download is asked for again: the
/// app's for one its queue started (`QUEUE_TRIES`, `QUEUE_RETRY_MS` in
/// hxhandlers), and a patient one for the rest.
const QUEUE_RETRIES: (u32, u64) = (5, 500);
const PATIENT: (u32, u64) = (30, 100);

/// A download paused partway — its connection ended, its partial file kept —
/// lets the next one go, and resumed from what is on disk arrives whole, byte
/// for byte. The pause and the resume are this test's own, built from the
/// pieces the app's are (the transfer's abort token, a resume FILE_GET);
/// hxhandlers' unit tests cover the app's state around them.
#[test]
fn a_paused_download_resumes_to_the_same_bytes() {
    let _turn = transfer_turn();
    const LEN: usize = 3 << 20;
    let bytes: Vec<u8> = (0..LEN as u32)
        .map(|i| (i.wrapping_mul(2_654_435_761) >> 13) as u8)
        .collect();
    for mut c in admins() {
        let name = c.server().name;
        let tmp = std::env::temp_dir().join(hx_e2e::unique_name("hx-e2e-pause"));
        std::fs::create_dir_all(&tmp).unwrap();
        let (src, dst, next) = (tmp.join("src"), tmp.join("dst"), tmp.join("next"));
        std::fs::write(&src, &bytes).unwrap();
        let src_c = std::ffi::CString::new(src.to_str().unwrap()).unwrap();
        let dst_c = std::ffi::CString::new(dst.to_str().unwrap()).unwrap();
        let next_c = std::ffi::CString::new(next.to_str().unwrap()).unwrap();

        let mut s = Scratch::new(&mut c, "pause");
        let dir = s.path().to_string();
        upload(s.client(), &dir, b"big", &src_c, LEN as u64);

        upload(s.client(), &dir, b"next", &src_c, LEN as u64);

        let rv = download_from(s.client(), &dir, b"big", &dst_c, LEN as u64 / 3, PATIENT);
        assert_ne!(rv, 0, "{name}: the pause didn't stop the download");
        let kept = std::fs::metadata(&dst).map_or(0, |m| m.len()) as usize;
        assert!(0 < kept && kept < LEN, "{name}: kept {kept} of {LEN}");
        assert_eq!(
            std::fs::read(&dst).unwrap(),
            bytes[..kept],
            "{name}: the part kept"
        );

        // The next one in the queue goes as soon as the paused one's
        // connection has closed, which mhxd may not have seen yet.
        let rv = download_from(s.client(), &dir, b"next", &next_c, u64::MAX, QUEUE_RETRIES);
        assert_eq!(rv, 0, "{name}: the next download failed");
        assert!(
            std::fs::read(&next).unwrap() == bytes,
            "{name}: next download's bytes differ"
        );

        let rv = download_from(s.client(), &dir, b"big", &dst_c, u64::MAX, PATIENT);
        assert_eq!(rv, 0, "{name}: the resumed download failed");
        assert!(
            std::fs::read(&dst).unwrap() == bytes,
            "{name}: resumed bytes differ"
        );
        drop(s);
        let _ = std::fs::remove_dir_all(&tmp);
    }
}

/// The rig's seed `test.txt`, downloaded through the app's own worker on
/// every server: saved whole, and fed whole to a preview.
#[test]
fn a_file_downloads_whole_and_previews_whole() {
    let _turn = transfer_turn();
    for s in servers_with(&[Cap::SeedFile]) {
        let mut c = Client::guest(s, 0);
        assert_eq!(
            download_file(&mut c, b"test.txt", false),
            b"hello world\n",
            "{}",
            s.name
        );
        assert_eq!(
            download_file(&mut c, b"test.txt", true),
            b"hello world\n",
            "{} preview",
            s.name
        );
    }
}

#[test]
fn the_seed_folder_downloads_whole() {
    let _turn = transfer_turn();
    for s in servers_with(&[Cap::SeedFolder]) {
        let mut c = Client::guest(s, 0);
        let (rv, _, got) = download_folder(&mut c, "/", b"test_folder");
        assert_eq!(rv, 0, "{}: the download failed", s.name);
        let file = |p: &str, b: &[u8]| (p.to_string(), Some(b.to_vec()));
        assert_eq!(
            got,
            [
                file("leaf_a.txt", b"folder-leaf-A\n"),
                file("leaf_b.txt", b"folder-leaf-B\n"),
            ],
            "{}",
            s.name
        );
    }
}
