//! Files written whole, the locks that order their writers, and the hex a
//! key is kept in.

use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};

use zeroize::Zeroizing;

/// The prefix of everything written beside its target before it takes
/// its place, which whoever next holds the lock may remove.
pub(crate) const TEMP: &str = ".tmp-";

pub(crate) fn hex(bytes: &[u8]) -> Zeroizing<String> {
    let mut s = Zeroizing::new(String::with_capacity(bytes.len() * 2));
    for b in bytes {
        s.push(char::from_digit(u32::from(b >> 4), 16).unwrap());
        s.push(char::from_digit(u32::from(b & 0xf), 16).unwrap());
    }
    s
}

pub(crate) fn unhex32(s: &str) -> Option<Zeroizing<[u8; 32]>> {
    let mut out = Zeroizing::new([0u8; 32]);
    if s.len() != 64 || !s.is_ascii() {
        return None;
    }
    for (i, b) in out.iter_mut().enumerate() {
        *b = u8::from_str_radix(&s[i * 2..i * 2 + 2], 16).ok()?;
    }
    Some(out)
}

/// The directory `path` is in, `.` for a bare file name.
pub(crate) fn dir_of(path: &Path) -> &Path {
    match path.parent() {
        Some(p) if !p.as_os_str().is_empty() => p,
        _ => Path::new("."),
    }
}

/// A directory only its owner can enter, made if it is not there.
pub(crate) fn private_dir(path: &Path) -> io::Result<()> {
    let mut b = fs::DirBuilder::new();
    b.recursive(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::DirBuilderExt;
        b.mode(0o700);
    }
    b.create(path)
}

/// A name beside `path`'s that no other writer is using.
pub(crate) fn temp_beside(path: &Path) -> PathBuf {
    static N: AtomicU32 = AtomicU32::new(0);
    let name = path.file_name().map(|n| n.to_string_lossy().into_owned());
    dir_of(path).join(format!(
        "{TEMP}{}-{}-{}",
        name.unwrap_or_default(),
        std::process::id(),
        N.fetch_add(1, Ordering::Relaxed)
    ))
}

/// The directory holding `dir` synced, so that a rename in it lasts.
pub(crate) fn sync_dir(dir: &Path) -> io::Result<()> {
    #[cfg(unix)]
    File::open(dir)?.sync_all()?;
    #[cfg(not(unix))]
    let _ = dir;
    Ok(())
}

/// `bytes` at `path`, whole whatever happens part way: written to a
/// temporary file beside it, synced, and renamed over it. `private`
/// files are the owner's alone from the moment they exist.
pub(crate) fn write_whole(path: &Path, bytes: &[u8], private: bool) -> io::Result<()> {
    let tmp = temp_beside(path);
    let mut opts = OpenOptions::new();
    opts.write(true).create_new(true);
    #[cfg(unix)]
    if private {
        use std::os::unix::fs::OpenOptionsExt;
        opts.mode(0o600);
    }
    #[cfg(not(unix))]
    let _ = private;
    let written = opts.open(&tmp).and_then(|mut f| {
        f.write_all(bytes)?;
        f.sync_all()
    });
    if let Err(e) = written.and_then(|()| fs::rename(&tmp, path)) {
        let _ = fs::remove_file(&tmp);
        return Err(e);
    }
    sync_dir(dir_of(path))
}

/// An exclusive lock on `path`, made if it is not there, held until the
/// file is dropped: what orders this process's writers and another
/// GtkHx's.
pub(crate) fn lock(path: &Path) -> io::Result<File> {
    let f = OpenOptions::new()
        .create(true)
        .truncate(false)
        .write(true)
        .open(path)?;
    f.lock()?;
    Ok(f)
}
