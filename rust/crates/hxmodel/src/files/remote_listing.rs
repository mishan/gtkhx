//! The remote file-browser listing model — the path-navigation state the
//! Hotline `HxRemoteFilesProvider` carries while the user walks a server's
//! directory tree.
//!
//! This is the model half of `src/files_remote_provider.c` (the Rust
//! re-think of its `current_path` / `listing_error` state, mirroring the
//! `hxchat-model` `Conversation` split): a [`RemoteListing`] owns the
//! canonical current path and the sticky "last listing failed" flag, and
//! computes the parent / child paths the provider used to build by hand
//! (`remote_navigate_up`, `remote_child_path`, `reset_to_root`). No glib,
//! no GTK — so the fiddly path-walk math is unit-tested here, headless,
//! while the C side keeps only the GListStore, the FILE_LIST RPC send, the
//! no-reply watchdog, and the rcv-dispatch plumbing.
//!
//! ## Path model
//!
//! Hotline directory paths use `/` as the component separator and `/` as
//! the root. The provider works in this canonical string form; the wire
//! DIR-chunk encoding (the length-prefixed component array) is a separate
//! concern handled by `path_to_hldir` on the C send path. The rules below
//! reproduce the original C byte-for-byte:
//!
//!  * `parent` (navigate up): `None` at the root; `"/foo"` → `"/"`;
//!    `"/foo/bar"` → `"/foo"`. A path with no separator is also `None`
//!    (the C `remote_navigate_up` bailed when `strrchr` found nothing).
//!  * `child`: at the root, `"/name"`; otherwise `"current/name"`. The
//!    child name is joined verbatim — Hotline entry names may legally
//!    contain `/`, and the wire path still ships the raw bytes.
//!  * `set_path`: an empty path normalizes to `"/"` (matches the C
//!    `g_strdup (path && *path ? path : "/")` on the RPC send).

use std::ffi::CString;

/// Path-navigation state for one remote provider.
pub struct RemoteListing {
    /// Canonical current path. Always non-empty; the root is `"/"`.
    current: String,
    /// Cache of `current` as a C string, kept in sync on every mutation so
    /// the FFI getter can hand out a borrowed pointer valid until the next
    /// change (same contract the C `remote_get_current_path` had).
    current_c: CString,
    /// TRUE when the most recent FILE_LIST RPC failed (task error or the
    /// no-reply watchdog). Drives the panel's empty-state hint. Cleared on
    /// the next successful listing and on `reset_to_root`.
    listing_error: bool,
}

impl Default for RemoteListing {
    fn default() -> Self {
        Self::new()
    }
}

impl RemoteListing {
    /// A fresh listing rooted at `/` with no error state.
    pub fn new() -> Self {
        RemoteListing {
            current: String::from("/"),
            current_c: CString::new("/").unwrap(),
            listing_error: false,
        }
    }

    /// Rebuild the C-string cache from `current`. A path carrying an
    /// interior NUL (impossible from real navigation, defensive against
    /// hostile wire bytes) falls back to `/`.
    fn sync_c(&mut self) {
        self.current_c =
            CString::new(self.current.as_bytes()).unwrap_or_else(|_| CString::new("/").unwrap());
    }

    /// The canonical current path (`/` at the root).
    pub fn current(&self) -> &str {
        &self.current
    }

    /// TRUE iff the current path is the server root.
    pub fn is_root(&self) -> bool {
        self.current == "/"
    }

    /// Adopt `path` as the current path. Empty normalizes to `/`. Does not
    /// touch the error flag (the caller sets/clears that around the reply).
    pub fn set_path(&mut self, path: &str) {
        self.current = if path.is_empty() {
            String::from("/")
        } else {
            path.to_string()
        };
        self.sync_c();
    }

    /// Return to the server root and clear the error flag. The provider is
    /// reused across connections, so a stale deep path from the previous
    /// server must not carry into the next one.
    pub fn reset_to_root(&mut self) {
        self.set_path("/");
        self.listing_error = false;
    }

    /// The parent path to navigate to, or `None` when already at the root
    /// (or the current path has no separator to walk back over).
    pub fn parent(&self) -> Option<String> {
        if self.is_root() {
            return None;
        }
        match self.current.rfind('/') {
            None => None,
            Some(0) => Some(String::from("/")),
            Some(i) => Some(self.current[..i].to_string()),
        }
    }

    /// Build a server-side child path from the current path + `name`.
    pub fn child(&self, name: &str) -> String {
        if self.is_root() {
            format!("/{name}")
        } else {
            format!("{}/{name}", self.current)
        }
    }

    /// Whether the most recent listing failed.
    pub fn listing_error(&self) -> bool {
        self.listing_error
    }

    /// Set the sticky listing-error flag.
    pub fn set_listing_error(&mut self, v: bool) {
        self.listing_error = v;
    }

    /// Borrowed C-string view of the current path, valid until the next
    /// mutation of this handle.
    pub(crate) fn current_c_ptr(&self) -> *const std::os::raw::c_char {
        self.current_c.as_ptr()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn starts_at_root() {
        let l = RemoteListing::new();
        assert_eq!(l.current(), "/");
        assert!(l.is_root());
        assert!(!l.listing_error());
    }

    #[test]
    fn parent_at_root_is_none() {
        let l = RemoteListing::new();
        assert_eq!(l.parent(), None);
    }

    #[test]
    fn parent_one_level_returns_root() {
        let mut l = RemoteListing::new();
        l.set_path("/foo");
        assert_eq!(l.parent().as_deref(), Some("/"));
    }

    #[test]
    fn parent_deep_drops_last_component() {
        let mut l = RemoteListing::new();
        l.set_path("/foo/bar/baz");
        assert_eq!(l.parent().as_deref(), Some("/foo/bar"));
    }

    #[test]
    fn parent_no_separator_is_none() {
        // Shouldn't occur from real navigation (producers always emit a
        // leading '/'), but the C bailed rather than fabricate a parent.
        let mut l = RemoteListing::new();
        l.set_path("foo");
        assert_eq!(l.parent(), None);
    }

    #[test]
    fn parent_trailing_slash_trims_it() {
        let mut l = RemoteListing::new();
        l.set_path("/foo/");
        assert_eq!(l.parent().as_deref(), Some("/foo"));
    }

    #[test]
    fn child_at_root() {
        let l = RemoteListing::new();
        assert_eq!(l.child("Uploads"), "/Uploads");
    }

    #[test]
    fn child_in_subdir() {
        let mut l = RemoteListing::new();
        l.set_path("/foo");
        assert_eq!(l.child("bar"), "/foo/bar");
    }

    #[test]
    fn child_name_with_slash_is_verbatim() {
        // Hotline names may legally contain '/'; join verbatim.
        let mut l = RemoteListing::new();
        l.set_path("/foo");
        assert_eq!(l.child("a/b"), "/foo/a/b");
    }

    #[test]
    fn set_path_empty_normalizes_to_root() {
        let mut l = RemoteListing::new();
        l.set_path("/foo");
        l.set_path("");
        assert_eq!(l.current(), "/");
        assert!(l.is_root());
    }

    #[test]
    fn navigate_up_walks_all_the_way_back() {
        // Simulate the provider's navigate_up loop: parent(), adopt, repeat.
        let mut l = RemoteListing::new();
        l.set_path("/a/b/c");
        let p = l.parent().unwrap();
        l.set_path(&p);
        assert_eq!(l.current(), "/a/b");
        let p = l.parent().unwrap();
        l.set_path(&p);
        assert_eq!(l.current(), "/a");
        let p = l.parent().unwrap();
        l.set_path(&p);
        assert_eq!(l.current(), "/");
        assert_eq!(l.parent(), None);
    }

    #[test]
    fn reset_clears_path_and_error() {
        let mut l = RemoteListing::new();
        l.set_path("/deep/path");
        l.set_listing_error(true);
        l.reset_to_root();
        assert_eq!(l.current(), "/");
        assert!(!l.listing_error());
    }

    #[test]
    fn error_flag_roundtrips() {
        let mut l = RemoteListing::new();
        assert!(!l.listing_error());
        l.set_listing_error(true);
        assert!(l.listing_error());
        l.set_listing_error(false);
        assert!(!l.listing_error());
    }
}
