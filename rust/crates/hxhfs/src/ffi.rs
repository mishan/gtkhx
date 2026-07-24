//! Thin `#[no_mangle]` C-ABI shim preserving the exact `hfs.h` surface.
//!
//! This preserves `hfs.c`'s symbol names, the `struct hfsinfo` layout, and the
//! process-global config (`hfs_set_config`), so C callers (`xfers.c`, `rcv.c`)
//! link it unchanged.
//!
//! The shim only marshals: it converts C pointers ↔ the native
//! [`crate::hfs`] API and holds the global [`Config`]. All real work is in the
//! native module.
//!
//! Portability: the shim compiles on every target. The only platform-specific
//! surface is path decoding, the Unix file-mode / raw open(2) flags (no-ops on
//! Windows), and `resource_open`'s file-descriptor hand-off — on Unix an
//! `into_raw_fd`, on Windows a CRT descriptor from `_open_osfhandle` (see
//! [`file_into_fd`]). The `struct stat` existence probe used by the path helpers
//! stays Unix-exact; on other targets those (currently uncalled) helpers report
//! existence without filling the caller's `struct stat`.

use std::ffi::{c_char, c_int, c_long, c_void, CStr};
use std::fs::{File, OpenOptions};
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};

use crate::hfs::{self, Config, HfsInfo};

/// `errno` for the path-too-long return — the platform `ENAMETOOLONG`.
const ENAMETOOLONG: c_int = libc::ENAMETOOLONG;

/// `open(2)` access-mode mask. `libc::O_ACCMODE` on Unix; the CRT uses the same
/// low two bits (O_RDONLY=0, O_WRONLY=1, O_RDWR=2) on Windows but doesn't always
/// export the mask constant, so spell it out there.
#[cfg(unix)]
const O_ACCMODE: c_int = libc::O_ACCMODE;
#[cfg(not(unix))]
const O_ACCMODE: c_int = 0x3;

/// The C `MAXPATHLEN` — `PATH_MAX` clamped to 4095 (`compat.h`). This is a C
/// caller's `char buf[MAXPATHLEN]` size, so `write_path_buf` must not exceed it.
#[cfg(unix)]
fn c_maxpathlen() -> usize {
    (libc::PATH_MAX as usize).min(4095)
}
#[cfg(not(unix))]
fn c_maxpathlen() -> usize {
    // compat.h clamps MAXPATHLEN to 4095 on platforms without a smaller PATH_MAX.
    4095
}

/// `#[repr(C)]` mirror of C's `struct hfsinfo` (`hfs.h`). Layout pinned below.
#[repr(C)]
pub struct HfsInfoFfi {
    pub type_: [u8; 4],
    pub creator: [u8; 4],
    pub create_time: u32,
    pub modify_time: u32,
    pub rsrclen: u32,
    pub comlen: u32,
    pub comment: [u8; 200],
}

// Pin the ABI: field reorder / width change on either side is a build error.
const _: () = assert!(std::mem::size_of::<HfsInfoFfi>() == 224);
const _: () = assert!(std::mem::align_of::<HfsInfoFfi>() == 4);

fn config() -> &'static Mutex<Config> {
    static CFG: OnceLock<Mutex<Config>> = OnceLock::new();
    CFG.get_or_init(|| Mutex::new(Config::default()))
}

/// Snapshot the global config. Recovers from a poisoned mutex (`into_inner`)
/// rather than `unwrap`-panicking — an unwind across the `extern "C"` boundary
/// would be UB, and the config is plain data with no invariant to protect.
fn current_config() -> Config {
    config()
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .clone()
}

/// Copy a C string into an owned [`PathBuf`]. Owned (not a borrow) so the
/// lifetime is honest — the C buffer's validity ends with the call.
unsafe fn path_from(ptr: *const c_char) -> Option<PathBuf> {
    if ptr.is_null() {
        return None;
    }
    let bytes = CStr::from_ptr(ptr).to_bytes();
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        Some(PathBuf::from(std::ffi::OsStr::from_bytes(bytes).to_owned()))
    }
    #[cfg(not(unix))]
    {
        // GtkHx passes UTF-8 filenames on Windows (the glib convention), which
        // map to the OS's wide encoding at the filesystem boundary.
        std::str::from_utf8(bytes).ok().map(PathBuf::from)
    }
}

/// Translate C `open(2)` `mode` flags + `perm` into an [`OpenOptions`], so the
/// caller-provided open policy (read/write/create/truncate/append + mode)
/// passes through exactly as `hfs.c`'s `open(path, mode, perm)` did.
fn open_options_from(mode: c_int, perm: c_int) -> OpenOptions {
    let mut opts = OpenOptions::new();
    match mode & O_ACCMODE {
        libc::O_WRONLY => {
            opts.write(true);
        }
        libc::O_RDWR => {
            opts.read(true).write(true);
        }
        _ => {
            opts.read(true);
        }
    }
    if mode & libc::O_CREAT != 0 {
        opts.create(true);
        // Applies the Unix file mode; a no-op on Windows (no mode concept).
        hfs::with_mode(&mut opts, perm as u32);
    }
    if mode & libc::O_TRUNC != 0 {
        opts.truncate(true);
    }
    if mode & libc::O_APPEND != 0 {
        opts.append(true);
    }
    // Pass through any remaining raw open(2) flags OpenOptions doesn't model
    // (O_CLOEXEC, O_EXCL, O_NOFOLLOW, O_SYNC, …). Unix-only: Windows OpenOptions
    // has no custom-flags escape hatch, and the C callers pass none of these.
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        let managed = O_ACCMODE | libc::O_CREAT | libc::O_TRUNC | libc::O_APPEND;
        opts.custom_flags(mode & !managed);
    }
    opts
}

/// Write `bytes` + a NUL into a `MAXPATHLEN` caller buffer.
unsafe fn write_path_buf(dst: *mut c_char, bytes: &[u8]) -> c_int {
    // Defensive: the C contract is a valid MAXPATHLEN buffer, but a NULL here
    // would be a write-through-NULL UB at the FFI boundary — fail with EINVAL.
    if dst.is_null() {
        return libc::EINVAL;
    }
    if bytes.len() + 1 > c_maxpathlen() {
        return ENAMETOOLONG;
    }
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), dst as *mut u8, bytes.len());
    *dst.add(bytes.len()) = 0;
    0
}

/// `stat(path, statbuf)` when `statbuf` is non-NULL, returning `0` on success or
/// the failure `errno` — the existence-check half of the C `*_path` helpers.
/// `path` must be a NUL-terminated buffer (as `write_path_buf` leaves it).
#[cfg(unix)]
unsafe fn stat_if_requested(path: *const c_char, statbuf: *mut c_void) -> c_int {
    if statbuf.is_null() {
        return 0;
    }
    if libc::stat(path, statbuf as *mut libc::stat) == 0 {
        0
    } else {
        std::io::Error::last_os_error()
            .raw_os_error()
            .unwrap_or(ENAMETOOLONG)
    }
}

/// Non-Unix variant: C's `struct stat` layout isn't portable to fill here, and
/// the only callers (`finderinfo_path` / `resource_path`) are currently unused
/// by the C app. So report existence via the path and leave `statbuf` untouched.
#[cfg(not(unix))]
unsafe fn stat_if_requested(path: *const c_char, statbuf: *mut c_void) -> c_int {
    if statbuf.is_null() {
        return 0;
    }
    match path_from(path) {
        Some(p) if p.exists() => 0,
        Some(_) => libc::ENOENT,
        None => libc::EINVAL,
    }
}

// ---------- config ----------

/// `void hfs_set_config (long fork, long file_perm, long dir_perm, char *comment)`
///
/// # Safety
/// `comment` is NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn hfs_set_config(
    fork: c_long,
    file_perm: c_long,
    dir_perm: c_long,
    comment: *mut c_char,
) {
    let fork = match fork {
        2 => hfs::Fork::Double,
        3 => hfs::Fork::Netatalk,
        _ => hfs::Fork::Cap,
    };
    let comment = if comment.is_null() {
        None
    } else {
        Some(CStr::from_ptr(comment).to_bytes().to_vec())
    };
    let mut cfg = config().lock().unwrap_or_else(|e| e.into_inner());
    cfg.fork = fork;
    cfg.file_perm = file_perm as u32;
    cfg.dir_perm = dir_perm as u32;
    cfg.comment = comment;
}

// ---------- path helpers ----------

/// `int finderinfo_path (char *infopath, const char *path, struct stat *statbuf)`
///
/// Writes the sidecar path into `infopath`; when `statbuf` is non-NULL, also
/// `stat`s it (the existence check the C callers use) and returns the failure
/// `errno` if that fails. Returns 0 on success, `ENAMETOOLONG` if the path
/// doesn't fit.
///
/// # Safety
/// `infopath` points at a `MAXPATHLEN` buffer; `path` is a valid C string;
/// `statbuf` is NULL or a valid `struct stat *`.
#[no_mangle]
pub unsafe extern "C" fn finderinfo_path(
    infopath: *mut c_char,
    path: *const c_char,
    statbuf: *mut c_void,
) -> c_int {
    let Some(p) = path_from(path) else {
        return libc::EINVAL;
    };
    let Ok(pb) = hfs::finderinfo_path(&p, current_config().dir_char) else {
        return ENAMETOOLONG;
    };
    let rc = write_path_buf(infopath, pb.as_os_str().as_encoded_bytes());
    if rc != 0 {
        return rc;
    }
    stat_if_requested(infopath, statbuf)
}

/// `int resource_path (char *rsrcpath, const char *path, struct stat *statbuf)`
///
/// # Safety
/// `rsrcpath` points at a `MAXPATHLEN` buffer; `path` is a valid C string;
/// `statbuf` is NULL or a valid `struct stat *`.
#[no_mangle]
pub unsafe extern "C" fn resource_path(
    rsrcpath: *mut c_char,
    path: *const c_char,
    statbuf: *mut c_void,
) -> c_int {
    let Some(p) = path_from(path) else {
        return libc::EINVAL;
    };
    let Ok(pb) = hfs::resource_path(&p, current_config().dir_char) else {
        return ENAMETOOLONG;
    };
    let rc = write_path_buf(rsrcpath, pb.as_os_str().as_encoded_bytes());
    if rc != 0 {
        return rc;
    }
    stat_if_requested(rsrcpath, statbuf)
}

// ---------- resource fork ----------

/// `int resource_open (const char *path, int mode, int perm)` — returns an
/// owned fd (caller closes) or -1.
///
/// Divergence from `hfs.c`: its AppleDouble branch returns **0** (i.e. fd 0 /
/// stdin) when the `.fndrinfo` open fails, which is a quirk with downstream
/// workarounds in `xfers.c`. This shim intentionally normalizes every "no
/// resource fork" outcome to **-1** (POSIX-like) to avoid the stdin-fd failure
/// mode. When wiring this in as a drop-in, adjust those `xfers.c` sites (which
/// treat `< 0` as "skip the fork" already) accordingly.
///
/// # Safety
/// `path` is a valid C string.
#[no_mangle]
pub unsafe extern "C" fn resource_open(path: *const c_char, mode: c_int, perm: c_int) -> c_int {
    let Some(p) = path_from(path) else {
        return -1;
    };
    let opts = open_options_from(mode, perm);
    match hfs::resource_open(&current_config(), &p, &opts) {
        Ok(Some(f)) => file_into_fd(f, mode),
        _ => -1,
    }
}

/// Hand an open resource-fork [`File`] to C as an owned descriptor the caller
/// closes. On Unix that's the underlying fd. On Windows there is no fd, so wrap
/// the OS `HANDLE` in a CRT descriptor via `_open_osfhandle` — usable with the
/// C runtime's `_read` / `_write` / `_lseek` / `_close` (which MinGW aliases to
/// the POSIX names the C callers use). This relies on Rust and the C app sharing
/// one CRT, which holds under MSYS2 UCRT64 (both link ucrtbase). Returns -1 if
/// the descriptor can't be created.
#[cfg(unix)]
fn file_into_fd(f: File, _mode: c_int) -> c_int {
    use std::os::unix::io::IntoRawFd;
    f.into_raw_fd()
}
#[cfg(windows)]
fn file_into_fd(f: File, mode: c_int) -> c_int {
    use std::os::windows::io::{FromRawHandle, IntoRawHandle};

    extern "C" {
        // <io.h>: allocate a CRT file descriptor for an existing OS HANDLE.
        fn _open_osfhandle(osfhandle: isize, flags: c_int) -> c_int;
    }
    // Descriptors are binary by default (no _O_TEXT); carry O_APPEND through as
    // the CRT's _O_APPEND (0x0008). Access is governed by the HANDLE itself.
    let flags = if mode & libc::O_APPEND != 0 { 0x0008 } else { 0 };
    let handle = f.into_raw_handle();
    let fd = unsafe { _open_osfhandle(handle as isize, flags) };
    if fd == -1 {
        // Ownership didn't transfer to the CRT — reclaim and close the HANDLE.
        drop(unsafe { File::from_raw_handle(handle) });
    }
    fd
}

/// `size_t resource_len (const char *path)`
///
/// # Safety
/// `path` is a valid C string.
#[no_mangle]
pub unsafe extern "C" fn resource_len(path: *const c_char) -> usize {
    match path_from(path) {
        Some(p) => hfs::resource_len(&current_config(), &p) as usize,
        None => 0,
    }
}

// ---------- type / creator ----------

/// `void type_creator (u_int8_t *buf, const char *path)` — writes 8 bytes.
///
/// # Safety
/// `buf` points at 8 writable bytes; `path` is a valid C string.
#[no_mangle]
pub unsafe extern "C" fn type_creator(buf: *mut u8, path: *const c_char) {
    if buf.is_null() {
        return;
    }
    let tc = match path_from(path) {
        Some(p) => hfs::type_creator(&current_config(), &p),
        None => *b"TEXTR*ch",
    };
    std::ptr::copy_nonoverlapping(tc.as_ptr(), buf, 8);
}

// ---------- Finder info ----------

/// `void hfsinfo_read (const char *path, struct hfsinfo *fi)`
///
/// # Safety
/// `fi` points at a writable `struct hfsinfo`; `path` is a valid C string.
#[no_mangle]
pub unsafe extern "C" fn hfsinfo_read(path: *const c_char, fi: *mut HfsInfoFfi) {
    if fi.is_null() {
        return;
    }
    let native = match path_from(path) {
        Some(p) => hfs::hfsinfo_read(&current_config(), &p),
        None => HfsInfo::default(),
    };
    native_to_ffi(&native, &mut *fi);
}

/// `void hfsinfo_write (const char *path, struct hfsinfo *fi)`
///
/// # Safety
/// `fi` points at a valid `struct hfsinfo`; `path` is a valid C string.
#[no_mangle]
pub unsafe extern "C" fn hfsinfo_write(path: *const c_char, fi: *mut HfsInfoFfi) {
    let (Some(p), false) = (path_from(path), fi.is_null()) else {
        return;
    };
    let native = ffi_to_native(&*fi);
    let _ = hfs::hfsinfo_write(&current_config(), &p, &native);
}

// ---------- comment ----------

/// `size_t comment_len (const char *path)`
///
/// # Safety
/// `path` is a valid C string.
#[no_mangle]
pub unsafe extern "C" fn comment_len(path: *const c_char) -> usize {
    match path_from(path) {
        Some(p) => hfs::comment_len(&current_config(), &p),
        None => 0,
    }
}

/// `void comment_write (const char *path, char *comment, int comlen)`
///
/// # Safety
/// `path` is a valid C string; `comment` is valid for `comlen` bytes.
#[no_mangle]
pub unsafe extern "C" fn comment_write(path: *const c_char, comment: *const c_char, comlen: c_int) {
    let Some(p) = path_from(path) else {
        return;
    };
    let bytes = if comment.is_null() || comlen <= 0 {
        &[][..]
    } else {
        std::slice::from_raw_parts(comment as *const u8, comlen as usize)
    };
    let _ = hfs::comment_write(&current_config(), &p, bytes);
}

// ---------- struct conversion ----------

fn native_to_ffi(native: &HfsInfo, ffi: &mut HfsInfoFfi) {
    ffi.type_.copy_from_slice(&native.type_creator[0..4]);
    ffi.creator.copy_from_slice(&native.type_creator[4..8]);
    // Preserve the raw on-disk bytes exactly (matches the C memcpy semantics).
    ffi.create_time = u32::from_ne_bytes(native.create_time);
    ffi.modify_time = u32::from_ne_bytes(native.modify_time);
    ffi.rsrclen = native.rsrclen;
    let n = native.comment.len().min(200);
    ffi.comlen = n as u32;
    ffi.comment = [0; 200];
    ffi.comment[..n].copy_from_slice(&native.comment[..n]);
}

fn ffi_to_native(ffi: &HfsInfoFfi) -> HfsInfo {
    let mut type_creator = [0u8; 8];
    type_creator[0..4].copy_from_slice(&ffi.type_);
    type_creator[4..8].copy_from_slice(&ffi.creator);
    let n = (ffi.comlen as usize).min(200);
    HfsInfo {
        type_creator,
        create_time: ffi.create_time.to_ne_bytes(),
        modify_time: ffi.modify_time.to_ne_bytes(),
        rsrclen: ffi.rsrclen,
        comment: ffi.comment[..n].to_vec(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Exercises the full FFI resource_open path including the platform
    // descriptor hand-off (into_raw_fd on Unix, _open_osfhandle on Windows), and
    // proves the returned descriptor works with the C runtime's read / write /
    // close — exactly how xfers.c drives it. Portable: libc wraps the CRT on
    // Windows, and both descriptors here live in the test binary's own CRT.
    #[test]
    fn resource_open_descriptor_roundtrips() {
        use std::ffi::CString;

        let dir = std::env::temp_dir().join(format!("hxhfs-rsrc-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let cpath = CString::new(dir.join("song").to_str().unwrap()).unwrap();
        let data = b"RSRCDATA";

        // Default config = CAP fork → resource_open manages "<path>.rsrc".
        let fd = unsafe { resource_open(cpath.as_ptr(), libc::O_CREAT | libc::O_WRONLY, 0o600) };
        assert!(fd >= 0, "resource_open create returned {fd}");
        let n = unsafe { libc::write(fd, data.as_ptr() as *const c_void, data.len() as _) };
        assert_eq!(n as usize, data.len());
        assert_eq!(unsafe { libc::close(fd) }, 0);

        assert_eq!(unsafe { resource_len(cpath.as_ptr()) }, data.len());

        let fd = unsafe { resource_open(cpath.as_ptr(), libc::O_RDONLY, 0) };
        assert!(fd >= 0, "resource_open read returned {fd}");
        let mut buf = [0u8; 16];
        let n = unsafe { libc::read(fd, buf.as_mut_ptr() as *mut c_void, buf.len() as _) };
        assert!(n >= 0, "read returned {n}");
        assert_eq!(&buf[..n as usize], data);
        unsafe { libc::close(fd) };

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn open_options_translation_honors_flags() {
        use std::io::Write as _;
        let dir = std::env::temp_dir().join(format!("hxhfs-ffi-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let f = dir.join("t");
        std::fs::write(&f, b"LONG-EXISTING-CONTENT").unwrap();

        // O_WRONLY | O_CREAT | O_TRUNC → truncates on open.
        let opts = open_options_from(libc::O_WRONLY | libc::O_CREAT | libc::O_TRUNC, 0o600);
        opts.open(&f).unwrap().write_all(b"hi").unwrap();
        assert_eq!(std::fs::read(&f).unwrap(), b"hi");

        // O_WRONLY alone → no truncate; the tail survives a short write.
        std::fs::write(&f, b"ORIGINAL").unwrap();
        let opts = open_options_from(libc::O_WRONLY, 0);
        opts.open(&f).unwrap().write_all(b"NEW").unwrap();
        assert_eq!(std::fs::read(&f).unwrap(), b"NEWGINAL");

        // O_RDONLY on a missing file → NotFound (not created).
        let opts = open_options_from(libc::O_RDONLY, 0);
        assert!(opts.open(dir.join("absent")).is_err());

        let _ = std::fs::remove_dir_all(&dir);
    }

    // Exercises the `struct stat` existence probe, which is Unix-exact (the
    // non-Unix `stat_if_requested` doesn't fill the caller's struct).
    #[cfg(unix)]
    #[test]
    fn finderinfo_path_stats_when_requested() {
        use std::ffi::CString;
        let dir = std::env::temp_dir().join(format!("hxhfs-stat-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("f.fndrinfo"), b"12345").unwrap();

        let data = CString::new(dir.join("f").to_str().unwrap()).unwrap();
        let mut buf = [0 as c_char; 4096];
        let mut st: libc::stat = unsafe { std::mem::zeroed() };

        // Existing sidecar → 0, statbuf filled.
        let rc = unsafe {
            finderinfo_path(
                buf.as_mut_ptr(),
                data.as_ptr(),
                &mut st as *mut libc::stat as *mut c_void,
            )
        };
        assert_eq!(rc, 0);
        assert_eq!(st.st_size, 5);

        // Missing sidecar → the stat errno.
        let missing = CString::new(dir.join("nope").to_str().unwrap()).unwrap();
        let rc = unsafe {
            finderinfo_path(
                buf.as_mut_ptr(),
                missing.as_ptr(),
                &mut st as *mut libc::stat as *mut c_void,
            )
        };
        assert_eq!(rc, libc::ENOENT);

        // NULL statbuf → 0 without stat.
        let rc = unsafe { finderinfo_path(buf.as_mut_ptr(), data.as_ptr(), std::ptr::null_mut()) };
        assert_eq!(rc, 0);

        // NULL path → EINVAL (invalid input, not a length error).
        let rc = unsafe {
            finderinfo_path(buf.as_mut_ptr(), std::ptr::null(), std::ptr::null_mut())
        };
        assert_eq!(rc, libc::EINVAL);

        // NULL output buffer → EINVAL, no write-through-NULL.
        let rc = unsafe { finderinfo_path(std::ptr::null_mut(), data.as_ptr(), std::ptr::null_mut()) };
        assert_eq!(rc, libc::EINVAL);

        // type_creator tolerates a NULL buffer (no-op, no crash).
        unsafe { type_creator(std::ptr::null_mut(), data.as_ptr()) };

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn struct_conversion_roundtrips() {
        let native = HfsInfo {
            type_creator: *b"TEXTMSIE",
            create_time: [1, 2, 3, 4],
            modify_time: [5, 6, 7, 8],
            rsrclen: 42,
            comment: b"hi".to_vec(),
        };
        let mut ffi = HfsInfoFfi {
            type_: [0; 4],
            creator: [0; 4],
            create_time: 0,
            modify_time: 0,
            rsrclen: 0,
            comlen: 0,
            comment: [0; 200],
        };
        native_to_ffi(&native, &mut ffi);
        assert_eq!(&ffi.type_, b"TEXT");
        assert_eq!(&ffi.creator, b"MSIE");
        assert_eq!(ffi.comlen, 2);
        assert_eq!(ffi_to_native(&ffi), native);
    }
}
