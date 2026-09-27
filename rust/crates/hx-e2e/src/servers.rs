//! The servers in the rig (tests/COMPOSE.md) and what each can do. The Rust
//! counterpart of `tests/integration/server_matrix.c`, and scoped by the same
//! `GTKHX_TEST_SERVERS` variable (a comma-separated list of names).

/// What a test may need of a server.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Cap {
    /// An account allowed to manage files: make, rename, move and delete
    /// folders and files anywhere under the root.
    FileAdmin,
    /// Negotiates UTF-8 names (`HTLC_CAP_TEXT_ENCODING`).
    TextEncoding,
    /// Negotiates 64-bit file sizes (`HTLC_CAP_LARGE_FILES`).
    LargeFiles,
}

#[derive(Debug)]
pub struct Server {
    pub name: &'static str,
    pub host: &'static str,
    pub port: u16,
    pub xfer_port: u16,
    /// Login of the account with every file privilege, where the server has
    /// one (`Cap::FileAdmin`). The rig gives it no password.
    pub admin: &'static str,
    pub caps: &'static [Cap],
}

impl Server {
    pub fn has(&self, cap: Cap) -> bool {
        self.caps.contains(&cap)
    }
}

pub const SERVERS: &[Server] = &[
    Server {
        name: "mhxd",
        host: "127.0.0.1",
        port: 5500,
        xfer_port: 5501,
        // mhxd ships `admin` with no password and every access bit.
        admin: "admin",
        caps: &[Cap::FileAdmin],
    },
    Server {
        name: "janus",
        host: "127.0.0.1",
        port: 5510,
        xfer_port: 5511,
        // No file-admin account yet: the password tests/janus seeds for
        // `admin` doesn't log in, and the guest can't make folders.
        admin: "",
        caps: &[Cap::TextEncoding, Cap::LargeFiles],
    },
];

/// The servers that have every one of `caps`, narrowed by
/// `GTKHX_TEST_SERVERS` when it's set. Panics if nothing is left: a suite that
/// runs against no server has tested nothing, and must not pass.
pub fn servers_with(caps: &[Cap]) -> Vec<&'static Server> {
    let only = std::env::var("GTKHX_TEST_SERVERS").ok();
    let wanted = |name: &str| {
        only.as_deref()
            .is_none_or(|l| l.split(',').any(|n| n.trim() == name))
    };
    let out: Vec<_> = SERVERS
        .iter()
        .filter(|s| wanted(s.name) && caps.iter().all(|c| s.has(*c)))
        .collect();
    assert!(
        !out.is_empty(),
        "no server in the rig has {caps:?} (GTKHX_TEST_SERVERS={only:?})"
    );
    out
}
