//! The files browser's requests against real servers.
//!
//! Admin-level operations run where the rig has a file-admin account (see
//! `Cap::FileAdmin`); guest-level ones run everywhere.
#![cfg(feature = "rig")]

use hx_e2e::client::{CAP_LARGE_FILES, CAP_TEXT_ENCODING};
use hx_e2e::{servers_with, Cap, Client, Scratch};
use hxrequest::files;

/// `HTLS_DATA_HTXF_REF`.
const TAG_HTXF_REF: u16 = 0x006b;

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
    for s in servers_with(&[Cap::FileAdmin, Cap::LongNames]) {
        let mut c = Client::admin(s, 0);
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
        let utf8 = c.utf8();
        let r = c.request(&files::delete(doomed.as_bytes(), utf8).unwrap());
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
        let utf8 = c.utf8();
        // Two requests in flight at once: the failure belongs to the first.
        let t1 = c.send(&files::delete(missing.as_bytes(), utf8).unwrap());
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

#[test]
fn a_mac_roman_name_round_trips_through_its_display_form() {
    for mut c in admins() {
        assert!(!c.utf8());
        let mut s = Scratch::new(&mut c, "macroman");
        let dir = s.path().to_string();
        // "café" in Mac Roman, made with the raw bytes.
        let mut raw = s.join("caf").into_bytes();
        raw.push(0x8e);
        let r = s.client().request(&files::mkdir(&raw).unwrap());
        ok(s.client(), &r, "mkdir");

        // The browser shows the listed name decoded, and sends it back encoded.
        let listed = s.client().names(&dir);
        assert_eq!(listed, [b"caf\x8e".to_vec()]);
        let shown = hxproto::text::to_utf8(&listed[0]);
        assert_eq!(shown, "caf\u{e9}");
        let c = s.client();
        let r = c.request(&files::get_info(dir.as_bytes(), shown.as_bytes(), false).unwrap());
        ok(c, &r, "get info by the shown name");
        assert_eq!(r.file_info().name, b"caf\x8e");
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
        let r = c.request(&files::set_info(f.as_bytes(), b"f", Some(b"two\nlines"), utf8).unwrap());
        ok(c, &r, "set comment");
        let r = c.request(&files::get_info(dir.as_bytes(), b"f", utf8).unwrap());
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
        let utf8 = c.utf8();

        let r = c.request(&files::get_folder(dir.as_bytes(), b"tree", utf8).unwrap());
        ok(c, &r, "folder download");
        let get = hxproto::parse::parse_folder_get_reply(&r.raw, r.raw.len());
        assert_ne!(get.ref_, 0);

        let r = c.request(&files::put_folder(dir.as_bytes(), b"up", 15, 2, utf8).unwrap());
        ok(c, &r, "folder upload");
        assert_eq!(r.chunk(TAG_HTXF_REF).map(<[u8]>::len), Some(4));
    }
}

// ---- as a guest, on every server ---------------------------------------------

#[test]
fn a_guest_can_list_the_root_and_get_info_on_what_is_there() {
    for s in servers_with(&[]) {
        for caps in [0, CAP_TEXT_ENCODING] {
            let mut c = Client::guest(s, caps);
            let utf8 = c.utf8();
            let entries = c.list("/");
            assert!(!entries.is_empty(), "{}: empty root", s.name);
            for e in entries.iter().filter(|e| !e.is_folder()).take(3) {
                // By the name as the browser shows it.
                let shown = hxproto::text::to_utf8(&e.name);
                let r = c.request(&files::get_info(b"/", shown.as_bytes(), utf8).unwrap());
                ok(&c, &r, &format!("get info {shown:?}"));
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
