//! The idiomatic native HFS-sidecar API.
//!
//! Explicit [`Config`], `&Path` inputs, `io::Result` / owned [`HfsInfo`]
//! outputs. Byte-faithful to `hfs.c`'s CAP / AppleDouble / Netatalk layouts.

use std::ffi::OsString;
use std::fs::{File, OpenOptions};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};

use crate::suffix::suffix_type_creator;

/// Maximum Finder comment length the on-disk formats hold.
pub const MAX_COMMENT: usize = 200;

/// The native path-length ceiling — the upper bound of the C `MAXPATHLEN`
/// (`src/compat.h` = `PATH_MAX` clamped to 4095). The native API doesn't write
/// into a fixed C buffer (it returns a heap `PathBuf`), so this is just a
/// rejection limit; the FFI shim uses the platform-exact `min(PATH_MAX, 4095)`
/// so it can never overrun a C caller's `char buf[MAXPATHLEN]`.
pub const MAXPATHLEN: usize = 4095;

/// The AppleDouble / CAP on-disk constants (`hfs.h`).
pub(crate) mod format {
    // AppleDouble header-entry ids.
    pub const HDR_COMNT: u32 = 4;
    pub const HDR_OLDI: u32 = 7;
    pub const HDR_DATES: u32 = 8;
    pub const HDR_FINFO: u32 = 9;
    pub const HDR_RSRC: u32 = 2;
    pub const HDR_MAX: u16 = 16;

    pub const DBL_MAGIC: u32 = 0x0005_1607;
    pub const HDR_VERSION_1: u32 = 0x0001_0000;
    pub const HDR_VERSION_2: u32 = 0x0002_0000;

    pub const SIZEOF_HDR_DESCR: usize = 12;
    pub const SIZEOF_DBL_HDR: usize = 26;
    pub const SIZEOF_CAP_INFO: usize = 300;

    // CAP fixed record magic bytes + date bitmap.
    pub const CAP_MAGIC1: u8 = 0xFF;
    pub const CAP_VERSION: u8 = 0x10;
    pub const CAP_MAGIC: u8 = 0xDA;
    pub const CAP_DMAGIC: u8 = 0xDA;
    pub const CAP_MDATE: u8 = 0x01;
    pub const CAP_CDATE: u8 = 0x02;

    // Offsets into the 300-byte CAP record (struct hfs_cap_info).
    pub const CAP_OFF_FNDR: usize = 0; // fi_fndr[32] — type[4]+creator[4]+…
    pub const CAP_OFF_MAGIC1: usize = 34;
    pub const CAP_OFF_VERSION: usize = 35;
    pub const CAP_OFF_MAGIC: usize = 36;
    pub const CAP_OFF_COMLN: usize = 84;
    pub const CAP_OFF_COMNT: usize = 85; // fi_comnt[200]
    pub const CAP_OFF_DATEMAGIC: usize = 285;
    pub const CAP_OFF_DATEVALID: usize = 286;
    pub const CAP_OFF_CTIME: usize = 287; // fi_ctime[4], fi_mtime[4], fi_utime[4]
}

use format::*;

/// Seconds between the Unix epoch (1970) and the "header" epoch (2000).
const HTIME_OFFSET: u32 = 946_684_800;

/// The sidecar layout a [`Config`] reads / writes.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Fork {
    /// aufs "CAP": a `.rsrc` fork file + a fixed 300-byte `.fndrinfo` record.
    Cap,
    /// AppleDouble v2 (`.fndrinfo` holds header + all forks).
    Double,
    /// Netatalk (AppleDouble v1 magic).
    Netatalk,
}

/// Reader/writer configuration. Mirrors `hfs.c`'s process-global `cfg`, but
/// passed explicitly. `dir_char` is the path separator used to locate the
/// sidecar's directory (the C global `dir_char`, default `/`).
#[derive(Clone, Debug)]
pub struct Config {
    pub fork: Fork,
    pub file_perm: u32,
    pub dir_perm: u32,
    pub comment: Option<Vec<u8>>,
    pub dir_char: u8,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            fork: Fork::Cap,
            file_perm: 0o600,
            dir_perm: 0o700,
            comment: None,
            dir_char: b'/',
        }
    }
}

/// Decoded Finder metadata. `type_creator` is the 8-byte type+creator pair;
/// `create_time` / `modify_time` are the raw 4-byte "header" (2000-epoch,
/// big-endian) timestamps as stored on disk; `comment` is at most
/// [`MAX_COMMENT`] bytes.
#[derive(Clone, Default, Debug, PartialEq, Eq)]
pub struct HfsInfo {
    pub type_creator: [u8; 8],
    pub create_time: [u8; 4],
    pub modify_time: [u8; 4],
    pub rsrclen: u32,
    pub comment: Vec<u8>,
}

// ---------- Sidecar path computation ----------

fn sidecar(path: &Path, dir_char: u8, suffix: &[u8]) -> io::Result<PathBuf> {
    let p = path.as_os_str().as_bytes();
    // hfs.c: `if (len + 16 >= MAXPATHLEN) return ENAMETOOLONG;`
    if p.len() + 16 >= MAXPATHLEN {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "path too long for HFS sidecar",
        ));
    }
    // Split at the last `dir_char` in indices [1, len) — matches the C loop
    // `for (i = len-1; i > 0; i--)`, which never inspects index 0.
    let split = (1..p.len()).rev().find(|&i| p[i] == dir_char);
    let (dir, name): (&[u8], &[u8]) = match split {
        Some(i) => (&p[..i], &p[i + 1..]),
        None => (b".", p),
    };
    // The join separator is always `/`, regardless of dir_char (as in hfs.c).
    let mut out = Vec::with_capacity(dir.len() + 1 + name.len() + suffix.len());
    out.extend_from_slice(dir);
    out.push(b'/');
    out.extend_from_slice(name);
    out.extend_from_slice(suffix);
    Ok(PathBuf::from(OsString::from_vec(out)))
}

/// The `.fndrinfo` sidecar path for `path`.
pub fn finderinfo_path(path: &Path, dir_char: u8) -> io::Result<PathBuf> {
    sidecar(path, dir_char, b".fndrinfo")
}

/// The `.rsrc` sidecar path for `path`.
pub fn resource_path(path: &Path, dir_char: u8) -> io::Result<PathBuf> {
    sidecar(path, dir_char, b".rsrc")
}

// ---------- AppleDouble descriptor table ----------

#[derive(Clone, Copy)]
struct Descr {
    id: u32,
    offset: u32,
    length: u32,
}

impl Descr {
    fn from_bytes(b: &[u8]) -> Descr {
        Descr {
            id: u32::from_be_bytes([b[0], b[1], b[2], b[3]]),
            offset: u32::from_be_bytes([b[4], b[5], b[6], b[7]]),
            length: u32::from_be_bytes([b[8], b[9], b[10], b[11]]),
        }
    }
    fn write_bytes(&self, b: &mut [u8]) {
        b[0..4].copy_from_slice(&self.id.to_be_bytes());
        b[4..8].copy_from_slice(&self.offset.to_be_bytes());
        b[8..12].copy_from_slice(&self.length.to_be_bytes());
    }
}

/// Read the AppleDouble descriptor table from an open `.fndrinfo`. Returns the
/// entry count (clamped to `HDR_MAX`) and the descriptors, or `None` if the
/// fixed header can't be read.
fn read_dbl_descrs(f: &mut File) -> io::Result<Option<Vec<Descr>>> {
    let mut hdr = [0u8; SIZEOF_DBL_HDR];
    if read_exact_or_none(f, &mut hdr)?.is_none() {
        return Ok(None);
    }
    let mut entries = u16::from_be_bytes([hdr[24], hdr[25]]);
    if entries > HDR_MAX {
        entries = HDR_MAX;
    }
    let mut tbl = vec![0u8; SIZEOF_HDR_DESCR * entries as usize];
    if read_exact_or_none(f, &mut tbl)?.is_none() {
        return Ok(None);
    }
    let descrs = (0..entries as usize)
        .map(|i| Descr::from_bytes(&tbl[SIZEOF_HDR_DESCR * i..]))
        .collect();
    Ok(Some(descrs))
}

/// `read` exactly `buf.len()` bytes, or `None` if the file was shorter (the C
/// `if (r != SIZEOF_…) goto funkdat;` pattern — a short read is "no record",
/// not an error).
fn read_exact_or_none(f: &mut File, buf: &mut [u8]) -> io::Result<Option<()>> {
    let mut got = 0;
    while got < buf.len() {
        match f.read(&mut buf[got..]) {
            Ok(0) => return Ok(None),
            Ok(n) => got += n,
            Err(ref e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    Ok(Some(()))
}

// ---------- type / creator ----------

/// The 8-byte type+creator for `path`: read from the sidecar's Finder info if
/// present + non-empty, else derived from the file extension.
pub fn type_creator(cfg: &Config, path: &Path) -> [u8; 8] {
    if let Ok(info) = finderinfo_path(path, cfg.dir_char) {
        if let Ok(mut f) = File::open(&info) {
            if let Some(tc) = read_type_creator(cfg.fork, &mut f) {
                if tc[0..4] != [0; 4] && tc[4..8] != [0; 4] {
                    return tc;
                }
            }
        }
    }
    suffix_type_creator(path.as_os_str().as_bytes())
}

fn read_type_creator(fork: Fork, f: &mut File) -> Option<[u8; 8]> {
    match fork {
        Fork::Cap => {
            let mut buf = [0u8; 8];
            read_exact_or_none(f, &mut buf).ok()??;
            Some(buf)
        }
        Fork::Double | Fork::Netatalk => {
            let descrs = read_dbl_descrs(f).ok()??;
            for d in descrs {
                if d.id == HDR_FINFO {
                    f.seek(SeekFrom::Start(d.offset as u64)).ok()?;
                    let mut buf = [0u8; 8];
                    read_exact_or_none(f, &mut buf).ok()??;
                    return Some(buf);
                }
            }
            None
        }
    }
}

// ---------- Finder info read ----------

/// Read the Finder metadata for `path`, applying the same fallbacks as
/// `hfsinfo_read`: extension-derived type/creator when absent, the data file's
/// mtime for missing dates, and `cfg.comment` for a missing comment.
pub fn hfsinfo_read(cfg: &Config, path: &Path) -> HfsInfo {
    let mut fi = HfsInfo::default();

    if let Ok(info) = finderinfo_path(path, cfg.dir_char) {
        if let Ok(mut f) = File::open(&info) {
            match cfg.fork {
                Fork::Cap => read_cap_info(&mut f, &mut fi),
                Fork::Double | Fork::Netatalk => read_dbl_info(&mut f, &mut fi),
            }
        }
    }

    // Fallbacks.
    if fi.type_creator[0..4] == [0; 4] || fi.type_creator[4..8] == [0; 4] {
        fi.type_creator = suffix_type_creator(path.as_os_str().as_bytes());
    }
    if fi.create_time == [0; 4] || fi.modify_time == [0; 4] {
        if let Ok(meta) = std::fs::metadata(path) {
            let mtime = meta_mtime_secs(&meta);
            let htime = mtime.wrapping_sub(HTIME_OFFSET).to_be_bytes();
            if fi.create_time == [0; 4] {
                fi.create_time = htime;
            }
            if fi.modify_time == [0; 4] {
                fi.modify_time = htime;
            }
        }
    }
    if fi.comment.is_empty() {
        if let Some(c) = &cfg.comment {
            let n = c.len().min(MAX_COMMENT);
            fi.comment = c[..n].to_vec();
        }
    }
    fi
}

fn meta_mtime_secs(meta: &std::fs::Metadata) -> u32 {
    use std::os::unix::fs::MetadataExt;
    meta.mtime() as u32
}

fn read_cap_info(f: &mut File, fi: &mut HfsInfo) {
    let mut buf = [0u8; SIZEOF_CAP_INFO];
    if read_exact_or_none(f, &mut buf).ok().flatten().is_none() {
        return;
    }
    fi.type_creator.copy_from_slice(&buf[CAP_OFF_FNDR..CAP_OFF_FNDR + 8]);
    let datevalid = buf[CAP_OFF_DATEVALID];
    if datevalid & CAP_CDATE != 0 {
        fi.create_time.copy_from_slice(&buf[CAP_OFF_CTIME..CAP_OFF_CTIME + 4]);
    }
    if datevalid & CAP_MDATE != 0 {
        fi.modify_time
            .copy_from_slice(&buf[CAP_OFF_CTIME + 4..CAP_OFF_CTIME + 8]);
    }
    let comln = (buf[CAP_OFF_COMLN] as usize).min(MAX_COMMENT);
    fi.comment = buf[CAP_OFF_COMNT..CAP_OFF_COMNT + comln].to_vec();
}

fn read_dbl_info(f: &mut File, fi: &mut HfsInfo) {
    let Ok(Some(descrs)) = read_dbl_descrs(f) else {
        return;
    };
    for d in descrs {
        if f.seek(SeekFrom::Start(d.offset as u64)).is_err() {
            continue;
        }
        match d.id {
            HDR_COMNT => {
                let n = (d.length as usize).min(MAX_COMMENT);
                let mut c = vec![0u8; n];
                if read_exact_or_none(f, &mut c).ok().flatten().is_some() {
                    fi.comment = c;
                }
            }
            HDR_OLDI | HDR_DATES => {
                let mut t = [0u8; 8];
                if read_exact_or_none(f, &mut t).ok().flatten().is_some() {
                    fi.create_time.copy_from_slice(&t[0..4]);
                    fi.modify_time.copy_from_slice(&t[4..8]);
                }
            }
            HDR_FINFO => {
                let mut tc = [0u8; 8];
                if read_exact_or_none(f, &mut tc).ok().flatten().is_some() {
                    fi.type_creator = tc;
                }
            }
            HDR_RSRC => fi.rsrclen = d.length,
            _ => {}
        }
    }
}

// ---------- Finder info write ----------

/// Build the 300-byte CAP `.fndrinfo` record for `fi`.
fn build_cap_record(fi: &HfsInfo) -> [u8; SIZEOF_CAP_INFO] {
    let mut b = [0u8; SIZEOF_CAP_INFO];
    b[CAP_OFF_MAGIC1] = CAP_MAGIC1;
    b[CAP_OFF_VERSION] = CAP_VERSION;
    b[CAP_OFF_MAGIC] = CAP_MAGIC;
    b[CAP_OFF_DATEMAGIC] = CAP_DMAGIC;
    b[CAP_OFF_DATEVALID] = CAP_MDATE | CAP_CDATE;
    b[CAP_OFF_FNDR..CAP_OFF_FNDR + 8].copy_from_slice(&fi.type_creator);
    b[CAP_OFF_CTIME..CAP_OFF_CTIME + 4].copy_from_slice(&fi.create_time);
    b[CAP_OFF_CTIME + 4..CAP_OFF_CTIME + 8].copy_from_slice(&fi.modify_time);
    let comln = fi.comment.len().min(MAX_COMMENT);
    b[CAP_OFF_COMLN] = comln as u8;
    b[CAP_OFF_COMNT..CAP_OFF_COMNT + comln].copy_from_slice(&fi.comment[..comln]);
    b
}

/// Write the Finder metadata for `path` into its sidecar.
pub fn hfsinfo_write(cfg: &Config, path: &Path, fi: &HfsInfo) -> io::Result<()> {
    let info = finderinfo_path(path, cfg.dir_char)?;
    match cfg.fork {
        Fork::Cap => {
            // Like hfs.c: O_RDWR|O_CREAT (no O_TRUNC) — overwrite the first 300
            // bytes from offset 0, leaving any trailing bytes intact.
            let mut f = open_rw_create(&info, cfg.file_perm)?;
            f.write_all(&build_cap_record(fi))?;
            f.sync_all()?;
            Ok(())
        }
        Fork::Double | Fork::Netatalk => write_dbl_info(cfg, &info, fi),
    }
}

fn write_dbl_info(cfg: &Config, info: &Path, fi: &HfsInfo) -> io::Result<()> {
    const NENTRIES: u16 = 4;
    let comlen = fi.comment.len().min(MAX_COMMENT) as u32;

    let mut f = open_rw_create(info, cfg.file_perm)?;
    // Read the existing header, or synthesize the default 4-entry table.
    let existing = read_dbl_descrs(&mut f)?;
    let mut descrs: Vec<Descr> = match &existing {
        Some(d) => d.clone(),
        None => {
            let base = (SIZEOF_DBL_HDR + SIZEOF_HDR_DESCR * NENTRIES as usize) as u32;
            vec![
                Descr { id: HDR_COMNT, offset: base, length: comlen },
                Descr { id: HDR_DATES, offset: base + comlen, length: 8 },
                Descr { id: HDR_FINFO, offset: base + comlen + 8, length: 8 },
                Descr { id: HDR_RSRC, offset: base + comlen + 8 + 8, length: 0 },
            ]
        }
    };

    // Write each entry's payload at its offset (mirrors hfsinfo_write's loop).
    for d in descrs.iter_mut() {
        if f.seek(SeekFrom::Start(d.offset as u64)).is_err() {
            continue;
        }
        match d.id {
            // Write the comment on every call (both the fresh and the existing-
            // header path). hfs.c has a `if (r == SIZEOF_HFS_DBL_HDR) break;`
            // guard here, but `r` is the descriptor-read result (12*entries) on
            // the existing path and the header-read result on the fresh path —
            // never exactly 26 at this point — so the guard is dead and the C
            // always writes the comment.
            HDR_COMNT => {
                d.length = comlen;
                f.write_all(&fi.comment[..comlen as usize])?;
            }
            HDR_OLDI | HDR_DATES => {
                if d.length < 8 {
                    d.length = 8;
                }
                let mut t = [0u8; 8];
                t[0..4].copy_from_slice(&fi.create_time);
                t[4..8].copy_from_slice(&fi.modify_time);
                f.write_all(&t)?;
            }
            HDR_FINFO => {
                if d.length < 8 {
                    d.length = 8;
                }
                f.write_all(&fi.type_creator)?;
            }
            HDR_RSRC => d.length = fi.rsrclen,
            _ => {}
        }
    }

    // Header (magic + version + descriptor table) last.
    let version = if cfg.fork == Fork::Netatalk {
        HDR_VERSION_1
    } else {
        HDR_VERSION_2
    };
    let mut hdr = vec![0u8; SIZEOF_DBL_HDR + SIZEOF_HDR_DESCR * descrs.len()];
    hdr[0..4].copy_from_slice(&DBL_MAGIC.to_be_bytes());
    hdr[4..8].copy_from_slice(&version.to_be_bytes());
    hdr[24..26].copy_from_slice(&(descrs.len() as u16).to_be_bytes());
    for (i, d) in descrs.iter().enumerate() {
        d.write_bytes(&mut hdr[SIZEOF_DBL_HDR + SIZEOF_HDR_DESCR * i..]);
    }
    f.seek(SeekFrom::Start(0))?;
    f.write_all(&hdr)?;
    f.sync_all()?;
    Ok(())
}

// ---------- comment ----------

/// The Finder comment length stored for `path` (`<= MAX_COMMENT`), falling back
/// to `cfg.comment`'s length.
pub fn comment_len(cfg: &Config, path: &Path) -> usize {
    let mut len = 0usize;
    if let Ok(info) = finderinfo_path(path, cfg.dir_char) {
        if let Ok(mut f) = File::open(&info) {
            len = match cfg.fork {
                Fork::Cap => {
                    let mut buf = [0u8; SIZEOF_CAP_INFO];
                    if read_exact_or_none(&mut f, &mut buf).ok().flatten().is_some() {
                        (buf[CAP_OFF_COMLN] as usize).min(MAX_COMMENT)
                    } else {
                        0
                    }
                }
                Fork::Double | Fork::Netatalk => read_dbl_descrs(&mut f)
                    .ok()
                    .flatten()
                    .map(|ds| {
                        ds.iter()
                            .filter(|d| d.id == HDR_COMNT)
                            .map(|d| (d.length as usize).min(MAX_COMMENT))
                            .next_back()
                            .unwrap_or(0)
                    })
                    .unwrap_or(0),
            };
        }
    }
    if len == 0 {
        if let Some(c) = &cfg.comment {
            len = c.len().min(MAX_COMMENT);
        }
    }
    len
}

/// Write a Finder comment for `path`. Like `hfs.c`, only the CAP layout is
/// supported (AppleDouble / Netatalk are a no-op).
pub fn comment_write(cfg: &Config, path: &Path, comment: &[u8]) -> io::Result<()> {
    if cfg.fork != Fork::Cap {
        return Ok(());
    }
    let info = finderinfo_path(path, cfg.dir_char)?;
    let comlen = comment.len().min(MAX_COMMENT);

    let mut f = open_rw_create(&info, cfg.file_perm)?;
    // Preserve an existing record, or synthesize a fresh one with suffix-derived
    // type/creator (matching hfs.c's comment_write).
    let mut buf = [0u8; SIZEOF_CAP_INFO];
    if read_exact_or_none(&mut f, &mut buf).ok().flatten().is_none() {
        buf = [0u8; SIZEOF_CAP_INFO];
        buf[CAP_OFF_MAGIC1] = CAP_MAGIC1;
        buf[CAP_OFF_VERSION] = CAP_VERSION;
        buf[CAP_OFF_MAGIC] = CAP_MAGIC;
        buf[CAP_OFF_DATEMAGIC] = CAP_DMAGIC;
        let tc = suffix_type_creator(path.as_os_str().as_bytes());
        buf[CAP_OFF_FNDR..CAP_OFF_FNDR + 8].copy_from_slice(&tc);
    }
    buf[CAP_OFF_COMLN] = comlen as u8;
    buf[CAP_OFF_COMNT..CAP_OFF_COMNT + comlen].copy_from_slice(&comment[..comlen]);
    f.seek(SeekFrom::Start(0))?;
    f.write_all(&buf)?;
    f.sync_all()?;
    Ok(())
}

// ---------- resource fork ----------

/// The resource fork length for `path` (CAP: the `.rsrc` file size; AppleDouble:
/// the RSRC descriptor's length).
pub fn resource_len(cfg: &Config, path: &Path) -> u64 {
    match cfg.fork {
        Fork::Cap => resource_path(path, cfg.dir_char)
            .ok()
            .and_then(|p| std::fs::metadata(p).ok())
            .map(|m| m.len())
            .unwrap_or(0),
        Fork::Double | Fork::Netatalk => {
            let Ok(info) = finderinfo_path(path, cfg.dir_char) else {
                return 0;
            };
            let Ok(mut f) = File::open(&info) else {
                return 0;
            };
            read_dbl_descrs(&mut f)
                .ok()
                .flatten()
                .and_then(|ds| ds.iter().find(|d| d.id == HDR_RSRC).map(|d| d.length as u64))
                .unwrap_or(0)
        }
    }
}

/// Open the resource fork for `path` using the caller-provided `opts`,
/// returning a [`File`] positioned at the start of the fork bytes, or `None` if
/// there's no resource fork. For AppleDouble the returned handle is the
/// `.fndrinfo` file seeked to the RSRC descriptor's offset.
///
/// `opts` carries the full open policy (read / write / create / truncate /
/// append / mode) exactly like the `mode` + `perm` `open(2)` args the C
/// `resource_open` passed through — so a resumed transfer can open without
/// truncating and seek before writing. A missing file (when `opts` doesn't
/// create) yields `None`, not an error (the C returns -1 and the caller skips
/// the fork).
///
/// Note: for AppleDouble / Netatalk the fork is embedded in the `.fndrinfo`
/// container, so `opts` must grant **read** access even when opening to write —
/// this call has to parse the header to find the RSRC descriptor's offset.
pub fn resource_open(cfg: &Config, path: &Path, opts: &OpenOptions) -> io::Result<Option<File>> {
    match cfg.fork {
        Fork::Cap => {
            let rsrc = resource_path(path, cfg.dir_char)?;
            open_or_none(opts, &rsrc)
        }
        Fork::Double | Fork::Netatalk => {
            let info = finderinfo_path(path, cfg.dir_char)?;
            let Some(mut f) = open_or_none(opts, &info)? else {
                return Ok(None);
            };
            let Some(descrs) = read_dbl_descrs(&mut f)? else {
                return Ok(None);
            };
            for d in descrs {
                if d.id == HDR_RSRC {
                    f.seek(SeekFrom::Start(d.offset as u64))?;
                    return Ok(Some(f));
                }
            }
            Ok(None)
        }
    }
}

/// Open `path` with `opts`, mapping a missing *target file* to `Ok(None)` — the
/// "no resource fork, skip" case. A `NotFound` whose *parent directory* is what's
/// missing is a real error (a wrong path, or a create that can't land) and is
/// propagated rather than silently swallowed.
fn open_or_none(opts: &OpenOptions, path: &Path) -> io::Result<Option<File>> {
    match opts.open(path) {
        Ok(f) => Ok(Some(f)),
        Err(e) if e.kind() == io::ErrorKind::NotFound => match path.parent() {
            // Parent exists (or is the implicit cwd) → the file itself is
            // missing: the documented skip case.
            Some(dir) if !dir.as_os_str().is_empty() && !dir.exists() => Err(e),
            _ => Ok(None),
        },
        Err(e) => Err(e),
    }
}

// ---------- open helpers ----------

fn open_rw_create(path: &Path, perm: u32) -> io::Result<File> {
    OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .mode(perm)
        .open(path)
}

#[cfg(test)]
mod tests;
