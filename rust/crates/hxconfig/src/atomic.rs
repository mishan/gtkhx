//! Atomic file replacement.
//!
//! Ported from `hxtls-trust`, which has the strongest write path of the three
//! config stores in the tree: an `O_EXCL` temp in the same directory with a
//! retry loop, `fsync` before the rename, mode 0600, and the temp cleaned up
//! on every error exit. The bookmark store's version uses a *fixed* temp
//! filename — so two concurrent writers race on it — and skips the `fsync`;
//! it should adopt this one.

use std::path::{Path, PathBuf};

/// Apply a Unix file mode to `opts` on platforms that model one; a no-op on
/// Windows, whose `OpenOptions` has no mode concept (the temp file inherits
/// the config directory's ACL there instead of an explicit 0600).
#[cfg(unix)]
fn with_mode(opts: &mut std::fs::OpenOptions, mode: u32) {
    use std::os::unix::fs::OpenOptionsExt;
    opts.mode(mode);
}
#[cfg(not(unix))]
fn with_mode(_opts: &mut std::fs::OpenOptions, _mode: u32) {}

fn next_seq() -> u64 {
    use std::sync::atomic::{AtomicU64, Ordering};
    static SEQ: AtomicU64 = AtomicU64::new(0);
    SEQ.fetch_add(1, Ordering::Relaxed)
}

/// Replace `path` with `data`, atomically. The config directory is assumed to
/// exist — it is created at app startup — so there is no `mkdir` here.
pub(crate) fn write_atomic(path: &Path, data: &[u8]) -> std::io::Result<()> {
    use std::io::{ErrorKind, Write};

    let dir = path.parent().unwrap_or_else(|| Path::new("."));
    let file_name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| crate::FILE_NAME.into());

    // Same directory, because rename must not cross a filesystem. O_EXCL so we
    // never clobber a concurrent writer's temp. The name mixes pid, a
    // nanosecond stamp and a monotonic counter, but a stale temp left by a
    // crashed run whose pid got reused could still collide, so retry a few
    // times on AlreadyExists — each attempt draws a fresh stamp and sequence —
    // rather than failing the save outright.
    let mut f = None;
    let mut tmp = PathBuf::new();
    let mut last_err = None;
    for _ in 0..16 {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        tmp = dir.join(format!(
            ".{file_name}.tmp.{}.{}.{}",
            std::process::id(),
            nanos,
            next_seq()
        ));
        let mut opts = std::fs::OpenOptions::new();
        opts.write(true).create_new(true);
        with_mode(&mut opts, 0o600);
        match opts.open(&tmp) {
            Ok(handle) => {
                f = Some(handle);
                break;
            }
            Err(e) if e.kind() == ErrorKind::AlreadyExists => {
                last_err = Some(e);
                continue;
            }
            Err(e) => return Err(e),
        }
    }
    let Some(mut f) = f else {
        return Err(last_err.unwrap_or_else(|| {
            std::io::Error::new(ErrorKind::AlreadyExists, "settings temp file collision")
        }));
    };

    if let Err(e) = f.write_all(data).and_then(|_| f.sync_all()) {
        let _ = std::fs::remove_file(&tmp);
        return Err(e);
    }
    drop(f);
    if let Err(e) = std::fs::rename(&tmp, path) {
        let _ = std::fs::remove_file(&tmp);
        return Err(e);
    }
    // The data is durable, but the rename that publishes it isn't until the
    // directory entry is flushed too. Best-effort: a filesystem that won't let
    // us open the directory is not a reason to report a failed save.
    sync_dir(dir);
    Ok(())
}

#[cfg(unix)]
fn sync_dir(dir: &Path) {
    if let Ok(handle) = std::fs::File::open(dir) {
        let _ = handle.sync_all();
    }
}

/// Windows has no directory handle to fsync through `std`; `MoveFileEx` without
/// `WRITE_THROUGH` is the same best-effort the rest of the platform gives.
#[cfg(not(unix))]
fn sync_dir(_dir: &Path) {}
