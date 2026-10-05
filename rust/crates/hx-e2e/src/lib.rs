//! `hx-e2e` — end-to-end tests against the servers in the Docker rig.
//!
//! The suites send the requests GtkHx itself builds (`hxrequest`) over the
//! production connection stack (`hxnet`'s connect, magic and LOGIN), and read
//! the replies with the parsers the receive handlers use (`hxproto::parse`).
//! So a test pins what the client really puts on the wire and what it makes of
//! the answer, on every server that supports the operation — the part the C
//! Tier 3 suite, which builds its frames by hand, can't reach.
//!
//! The harness is deliberately synchronous: each [`Client`] owns a small tokio
//! runtime and blocks on it, so a test reads top to bottom.
//!
//! Isolation: the rig's servers are long-lived and shared by parallel tests, so
//! a test works inside its own [`Scratch`] folder, named uniquely and deleted on
//! drop, and never asserts on anything outside it.

pub mod client;
mod servers;

pub use client::{Client, Entry, Reply};
pub use servers::{servers_with, Cap, Server};

use std::sync::atomic::{AtomicU32, Ordering};

/// A name no other test, process or earlier run is using: `prefix`, the process
/// id, a per-process counter and the clock.
pub fn unique_name(prefix: &str) -> String {
    static N: AtomicU32 = AtomicU32::new(0);
    let n = N.fetch_add(1, Ordering::Relaxed);
    let t = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.subsec_nanos())
        .unwrap_or(0);
    format!("{prefix}-{}-{n}-{t:08x}", std::process::id())
}

/// A folder of the test's own at the server's root, deleted (with whatever the
/// test left in it) when dropped.
pub struct Scratch<'c> {
    client: &'c mut Client,
    path: String,
}

impl<'c> Scratch<'c> {
    /// Make the folder. Panics if the server refuses: every suite that uses a
    /// scratch folder needs an account allowed to make one.
    pub fn new(client: &'c mut Client, prefix: &str) -> Self {
        let path = format!("/{}", unique_name(prefix));
        let reply = client.request(&hxrequest::files::mkdir(path.as_bytes()).unwrap());
        assert!(
            !reply.is_error(),
            "{}: can't make scratch folder {path}: {}",
            client.server().name,
            reply.error_text()
        );
        Scratch { client, path }
    }

    /// The folder's remote path.
    pub fn path(&self) -> &str {
        &self.path
    }

    /// `name` inside the folder.
    pub fn join(&self, name: &str) -> String {
        format!("{}/{name}", self.path)
    }

    pub fn client(&mut self) -> &mut Client {
        self.client
    }
}

impl Drop for Scratch<'_> {
    fn drop(&mut self) {
        if let Some(req) = hxrequest::files::delete(self.path.as_bytes()) {
            // Best effort: a failed cleanup mustn't mask the test's own
            // failure, and a leftover folder is harmless to other tests.
            let _ = self.client.try_request(&req);
        }
    }
}
