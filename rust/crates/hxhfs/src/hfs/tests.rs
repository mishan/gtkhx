//! Round-trip + golden-byte tests for the native HFS-sidecar API.

use super::format::*;
use super::*;
// Explicit std imports rather than leaning on the `use super::*` glob pulling in
// hfs.rs's private `use`s (which works, but is fragile).
use std::fs::OpenOptions;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};

/// A unique temp directory for one test (no external crates). Best-effort
/// cleanup on drop.
struct TmpDir(PathBuf);

impl TmpDir {
    fn new() -> TmpDir {
        static N: AtomicUsize = AtomicUsize::new(0);
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let dir = std::env::temp_dir().join(format!(
            "hxhfs-test-{}-{}-{}",
            std::process::id(),
            N.fetch_add(1, Ordering::Relaxed),
            nanos
        ));
        std::fs::create_dir_all(&dir).unwrap();
        TmpDir(dir)
    }
    fn path(&self, name: &str) -> PathBuf {
        self.0.join(name)
    }
}

impl Drop for TmpDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn sample() -> HfsInfo {
    HfsInfo {
        type_creator: *b"TEXTMSIE",
        create_time: [0x11, 0x22, 0x33, 0x44],
        modify_time: [0x55, 0x66, 0x77, 0x88],
        rsrclen: 0,
        comment: b"a finder comment".to_vec(),
    }
}

// ---------- sidecar path computation ----------

#[test]
fn sidecar_paths() {
    assert_eq!(
        finderinfo_path(Path::new("/home/x/file.bin"), b'/').unwrap(),
        Path::new("/home/x/file.bin.fndrinfo")
    );
    assert_eq!(
        resource_path(Path::new("/home/x/file.bin"), b'/').unwrap(),
        Path::new("/home/x/file.bin.rsrc")
    );
    // No separator → "." directory.
    assert_eq!(
        finderinfo_path(Path::new("bare"), b'/').unwrap(),
        Path::new("./bare.fndrinfo")
    );
}

#[test]
fn path_too_long_is_error() {
    let long = "a".repeat(4090);
    assert!(finderinfo_path(Path::new(&long), b'/').is_err());
}

// ---------- CAP ----------

#[test]
fn cap_record_golden_bytes() {
    let rec = build_cap_record(&sample());
    assert_eq!(rec.len(), SIZEOF_CAP_INFO);
    assert_eq!(rec[CAP_OFF_MAGIC1], CAP_MAGIC1);
    assert_eq!(rec[CAP_OFF_VERSION], CAP_VERSION);
    assert_eq!(rec[CAP_OFF_MAGIC], CAP_MAGIC);
    assert_eq!(rec[CAP_OFF_DATEMAGIC], CAP_DMAGIC);
    assert_eq!(rec[CAP_OFF_DATEVALID], CAP_MDATE | CAP_CDATE);
    assert_eq!(&rec[0..8], b"TEXTMSIE");
    assert_eq!(
        &rec[CAP_OFF_CTIME..CAP_OFF_CTIME + 4],
        &[0x11, 0x22, 0x33, 0x44]
    );
    assert_eq!(
        &rec[CAP_OFF_CTIME + 4..CAP_OFF_CTIME + 8],
        &[0x55, 0x66, 0x77, 0x88]
    );
    assert_eq!(rec[CAP_OFF_COMLN] as usize, sample().comment.len());
    assert_eq!(
        &rec[CAP_OFF_COMNT..CAP_OFF_COMNT + sample().comment.len()],
        sample().comment.as_slice()
    );
}

#[test]
fn cap_roundtrip() {
    let cfg = Config::default(); // CAP
    let tmp = TmpDir::new();
    let data = tmp.path("doc.bin");

    hfsinfo_write(&cfg, &data, &sample()).unwrap();
    let got = hfsinfo_read(&cfg, &data);

    assert_eq!(got.type_creator, sample().type_creator);
    assert_eq!(got.create_time, sample().create_time);
    assert_eq!(got.modify_time, sample().modify_time);
    assert_eq!(got.comment, sample().comment);
}

#[test]
fn cap_missing_sidecar_falls_back_to_suffix_and_mtime() {
    let cfg = Config::default();
    let tmp = TmpDir::new();
    let data = tmp.path("art.png");
    std::fs::write(&data, b"data").unwrap();

    let got = hfsinfo_read(&cfg, &data);
    // No sidecar → suffix table + the data file's mtime for both dates.
    assert_eq!(&got.type_creator, b"PNGfGKON");
    assert_ne!(got.create_time, [0; 4]);
    assert_eq!(got.create_time, got.modify_time);
}

#[test]
fn cap_config_comment_fallback() {
    let cfg = Config {
        comment: Some(b"default comment".to_vec()),
        ..Config::default()
    };
    let tmp = TmpDir::new();
    let data = tmp.path("x.bin");
    // No sidecar and no stored comment → cfg.comment.
    let got = hfsinfo_read(&cfg, &data);
    assert_eq!(got.comment, b"default comment");
}

#[test]
fn cap_comment_len_and_write() {
    let cfg = Config::default();
    let tmp = TmpDir::new();
    let data = tmp.path("c.bin");

    comment_write(&cfg, &data, b"hello").unwrap();
    assert_eq!(comment_len(&cfg, &data), 5);
    // The comment write also seeds a suffix-derived type when fresh.
    let got = hfsinfo_read(&cfg, &data);
    assert_eq!(got.comment, b"hello");
}

#[test]
fn cap_comment_write_fresh_record_sets_magic_bytes() {
    let cfg = Config::default();
    let tmp = TmpDir::new();
    let data = tmp.path("dm.bin");
    comment_write(&cfg, &data, b"c").unwrap();

    let bytes = std::fs::read(finderinfo_path(&data, b'/').unwrap()).unwrap();
    assert_eq!(bytes[CAP_OFF_MAGIC1], CAP_MAGIC1);
    assert_eq!(bytes[CAP_OFF_MAGIC], CAP_MAGIC);
    // fi_datemagic must be set on a freshly-synthesized record (matches hfs.c).
    assert_eq!(bytes[CAP_OFF_DATEMAGIC], CAP_DMAGIC);
}

#[test]
fn cap_comment_clamped_to_max() {
    let cfg = Config::default();
    let tmp = TmpDir::new();
    let data = tmp.path("big.bin");
    let big = vec![b'x'; 500];
    comment_write(&cfg, &data, &big).unwrap();
    assert_eq!(comment_len(&cfg, &data), MAX_COMMENT);
}

#[test]
fn cap_resource_fork() {
    let cfg = Config::default();
    let tmp = TmpDir::new();
    let data = tmp.path("r.bin");

    // Write a resource fork, then read it back through resource_open + len.
    let mut write_opts = OpenOptions::new();
    write_opts.write(true).create(true).truncate(true);
    super::with_mode(&mut write_opts, 0o600);
    let mut w = resource_open(&cfg, &data, &write_opts).unwrap().unwrap();
    w.write_all(b"RESOURCEDATA").unwrap();
    w.sync_all().unwrap();
    drop(w);

    assert_eq!(resource_len(&cfg, &data), 12);
    let mut read_opts = OpenOptions::new();
    read_opts.read(true);
    let mut r = resource_open(&cfg, &data, &read_opts).unwrap().unwrap();
    let mut buf = Vec::new();
    r.read_to_end(&mut buf).unwrap();
    assert_eq!(buf, b"RESOURCEDATA");
}

#[test]
fn cap_resource_open_write_does_not_truncate() {
    // A resumed transfer opens the existing fork write-only (no create/truncate)
    // and seeks before writing — the untouched tail must survive.
    let cfg = Config::default();
    let tmp = TmpDir::new();
    let data = tmp.path("resume.bin");
    let rsrc = resource_path(&data, b'/').unwrap();
    std::fs::write(&rsrc, b"ORIGINAL-CONTENT").unwrap();

    let mut opts = OpenOptions::new();
    opts.write(true);
    let mut f = resource_open(&cfg, &data, &opts).unwrap().unwrap();
    f.seek(SeekFrom::Start(0)).unwrap();
    f.write_all(b"NEW").unwrap();
    f.sync_all().unwrap();
    drop(f);

    assert_eq!(std::fs::read(&rsrc).unwrap(), b"NEWGINAL-CONTENT");
}

#[test]
fn cap_resource_open_absent_is_none() {
    let cfg = Config::default();
    let tmp = TmpDir::new();
    let data = tmp.path("none.bin");
    let mut read_opts = OpenOptions::new();
    read_opts.read(true);
    assert!(resource_open(&cfg, &data, &read_opts).unwrap().is_none());
    assert_eq!(resource_len(&cfg, &data), 0);
}

#[test]
fn resource_open_missing_parent_dir_is_error() {
    // A missing *parent directory* (not just a missing file) is a real error,
    // not the "no fork, skip" case — it must not be swallowed as Ok(None).
    let cfg = Config::default();
    let tmp = TmpDir::new();
    let data = tmp.path("nonexistent-subdir/file.bin");
    let mut opts = OpenOptions::new();
    opts.write(true).create(true).truncate(true);
    super::with_mode(&mut opts, 0o600);
    assert!(resource_open(&cfg, &data, &opts).is_err());
}

#[test]
fn cap_type_creator_prefers_sidecar() {
    let cfg = Config::default();
    let tmp = TmpDir::new();
    // .png extension, but the sidecar says TEXTMSIE — sidecar wins.
    let data = tmp.path("thing.png");
    hfsinfo_write(&cfg, &data, &sample()).unwrap();
    assert_eq!(&type_creator(&cfg, &data), b"TEXTMSIE");

    // No sidecar → extension.
    let other = tmp.path("other.gif");
    assert_eq!(&type_creator(&cfg, &other), b"GIFfGKON");
}

// ---------- AppleDouble / Netatalk ----------

#[test]
fn appledouble_header_golden_bytes() {
    let cfg = Config {
        fork: Fork::Double,
        ..Config::default()
    };
    let tmp = TmpDir::new();
    let data = tmp.path("ad.bin");
    hfsinfo_write(&cfg, &data, &sample()).unwrap();

    let bytes = std::fs::read(finderinfo_path(&data, b'/').unwrap()).unwrap();
    assert_eq!(&bytes[0..4], DBL_MAGIC.to_be_bytes());
    assert_eq!(&bytes[4..8], HDR_VERSION_2.to_be_bytes());
    assert_eq!(u16::from_be_bytes([bytes[24], bytes[25]]), 4);
}

#[test]
fn netatalk_uses_version_1() {
    let cfg = Config {
        fork: Fork::Netatalk,
        ..Config::default()
    };
    let tmp = TmpDir::new();
    let data = tmp.path("na.bin");
    hfsinfo_write(&cfg, &data, &sample()).unwrap();
    let bytes = std::fs::read(finderinfo_path(&data, b'/').unwrap()).unwrap();
    assert_eq!(&bytes[4..8], HDR_VERSION_1.to_be_bytes());
}

#[test]
fn appledouble_updates_existing_comment() {
    // A second write hits the existing-header path — the comment must still be
    // written (a same-length replacement keeps the descriptor offsets valid).
    let cfg = Config {
        fork: Fork::Double,
        ..Config::default()
    };
    let tmp = TmpDir::new();
    let data = tmp.path("upd.bin");

    hfsinfo_write(&cfg, &data, &sample()).unwrap(); // "a finder comment" (16 B)
    let mut fi2 = sample();
    fi2.comment = b"XXXXXXXXXXXXXXXX".to_vec(); // same length
    hfsinfo_write(&cfg, &data, &fi2).unwrap();

    assert_eq!(hfsinfo_read(&cfg, &data).comment, b"XXXXXXXXXXXXXXXX");
}

#[test]
fn appledouble_resource_fork_roundtrip() {
    // The AppleDouble resource fork lives inside the .fndrinfo at the RSRC
    // descriptor's offset. Create the sidecar, write fork bytes there, record
    // the length via a second hfsinfo_write, and read it back.
    let cfg = Config {
        fork: Fork::Double,
        ..Config::default()
    };
    let tmp = TmpDir::new();
    let data = tmp.path("adr.bin");

    let mut fi = sample();
    fi.rsrclen = 0;
    hfsinfo_write(&cfg, &data, &fi).unwrap(); // RSRC descr present, length 0

    // Write fork bytes at the RSRC offset. read+write, NOT truncate: the
    // AppleDouble fork is embedded, so resource_open must read the container
    // header to find the RSRC offset (a write-only fd couldn't), and the header
    // + other entries must survive.
    let mut wopts = OpenOptions::new();
    wopts.read(true).write(true);
    let mut w = resource_open(&cfg, &data, &wopts).unwrap().unwrap();
    w.write_all(b"RSRCBYTES").unwrap();
    w.sync_all().unwrap();
    drop(w);

    // Record the fork length in the RSRC descriptor (existing-header path).
    fi.rsrclen = 9;
    hfsinfo_write(&cfg, &data, &fi).unwrap();

    assert_eq!(resource_len(&cfg, &data), 9);
    // The Finder metadata still round-trips too.
    assert_eq!(hfsinfo_read(&cfg, &data).comment, sample().comment);

    let mut ropts = OpenOptions::new();
    ropts.read(true);
    let mut r = resource_open(&cfg, &data, &ropts).unwrap().unwrap();
    let mut buf = Vec::new();
    r.read_to_end(&mut buf).unwrap();
    assert_eq!(buf, b"RSRCBYTES");
}

#[test]
fn appledouble_roundtrip() {
    let cfg = Config {
        fork: Fork::Double,
        ..Config::default()
    };
    let tmp = TmpDir::new();
    let data = tmp.path("ad2.bin");

    hfsinfo_write(&cfg, &data, &sample()).unwrap();
    let got = hfsinfo_read(&cfg, &data);

    assert_eq!(got.type_creator, sample().type_creator);
    assert_eq!(got.create_time, sample().create_time);
    assert_eq!(got.modify_time, sample().modify_time);
    assert_eq!(got.comment, sample().comment);
}
