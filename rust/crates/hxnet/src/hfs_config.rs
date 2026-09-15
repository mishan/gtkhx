//! GtkHx-owned runtime configuration for shared HFS sidecar codecs.

use std::path::Path;
use std::sync::{Mutex, OnceLock};

use hxhfs::{hfs, Config};

fn state() -> &'static Mutex<Config> {
    static CONFIG: OnceLock<Mutex<Config>> = OnceLock::new();
    CONFIG.get_or_init(|| Mutex::new(Config::default()))
}

/// Snapshot the process-wide sidecar policy used by transfer workers.
pub fn current() -> Config {
    state()
        .lock()
        .unwrap_or_else(|error| error.into_inner())
        .clone()
}

/// Replace the process-wide sidecar policy.
pub fn replace(config: Config) {
    *state().lock().unwrap_or_else(|error| error.into_inner()) = config;
}

pub fn resource_len(path: &Path) -> u64 {
    hfs::resource_len(&current(), path)
}

pub fn comment_len(path: &Path) -> usize {
    hfs::comment_len(&current(), path)
}
