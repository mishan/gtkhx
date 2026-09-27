//! The servers in the rig (tests/COMPOSE.md) and what each can do. The Rust
//! counterpart of `tests/integration/server_matrix.c`, and scoped by the same
//! `GTKHX_TEST_SERVERS` variable (a comma-separated list of names).

/// What a test may need of a server.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Cap {
    /// An account allowed to manage files: make, rename, move and delete
    /// folders and files anywhere under the root.
    FileAdmin,
    /// Takes a file or folder name of the full 255 bytes a path item can
    /// carry. Janus panics on a name of 253 bytes or more and never replies
    /// (docs/janus-bugs.md).
    LongNames,
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
        caps: &[Cap::FileAdmin, Cap::LongNames],
    },
    Server {
        name: "janus",
        host: "127.0.0.1",
        port: 5510,
        xfer_port: 5511,
        // tests/janus gives `admin` the empty password.
        admin: "admin",
        caps: &[Cap::FileAdmin, Cap::TextEncoding, Cap::LargeFiles],
    },
    Server {
        name: "hlservd",
        host: "127.0.0.1",
        port: 5530,
        xfer_port: 5531,
        // hlservd makes `admin` on its first start with no password, which
        // logs in from the machine running the server: over host
        // networking, every test connection.
        admin: "admin",
        caps: &[Cap::FileAdmin, Cap::LongNames],
    },
];

/// The servers that have every one of `caps`, narrowed by
/// `GTKHX_TEST_SERVERS` when it's set.
///
/// With no narrowing, an empty result panics: a suite that runs against no
/// server has tested nothing, and must not pass. A narrowing that leaves a
/// suite nothing is the caller's own choice, and runs it against nothing.
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
        !out.is_empty() || only.is_some(),
        "no server in the rig has {caps:?}"
    );
    out
}
