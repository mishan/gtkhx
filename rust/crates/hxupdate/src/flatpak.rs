//! What the Flatpak portal's update monitor has said, and which banner that
//! calls for. Inside the Flatpak there are no versions to compare, only the
//! commits the portal reports.

/// The portal's `version` property from which `CreateUpdateMonitor` and
/// `Update` exist.
pub const MIN_PORTAL_VERSION: u32 = 2;

/// Everything the portal has told us so far.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PortalState {
    /// `org.freedesktop.portal.Flatpak`'s `version`; 0 until read, or when
    /// there is no portal.
    pub version: u32,
    /// From the last `UpdateAvailable`: the commit this process runs, the
    /// one installed, and the newest in the remote. Empty until it arrives.
    pub running: String,
    pub local: String,
    pub remote: String,
    /// The `status` and `progress` of the last `Progress`, once the user has
    /// asked for the update.
    pub progress: Option<(u32, u32)>,
    /// The remote commit the user answered "Later" to.
    pub dismissed: String,
}

/// `Progress`'s `status`.
pub const PROGRESS_RUNNING: u32 = 0;
pub const PROGRESS_DONE: u32 = 2;
pub const PROGRESS_ERROR: u32 = 3;

impl PortalState {
    /// An `UpdateAvailable`, which the portal sends when the installed or
    /// remote commit changes. Unless an update is still running, the commits
    /// decide again, so an update the software center made shows here.
    pub fn update_available(&mut self, running: String, local: String, remote: String) {
        if !matches!(self.progress, Some((PROGRESS_RUNNING, _))) {
            self.progress = None;
        }
        self.running = running;
        self.local = local;
        self.remote = remote;
    }
}

/// The whole transaction's percent from a `Progress`: `progress` is the
/// current operation's own, and `op` counts from 0 of `n_ops`.
pub fn overall_percent(progress: u32, op: Option<u32>, n_ops: Option<u32>) -> u32 {
    let progress = progress.min(100);
    match (op, n_ops) {
        (Some(op), Some(n)) if op < n => (op.saturating_mul(100) + progress) / n,
        _ => progress,
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Notice {
    None,
    /// A newer commit than the installed one is in the remote.
    Available,
    /// The update is being installed; percent done.
    Updating(u32),
    /// The portal couldn't install it, for one because the new version asks
    /// for permissions this one doesn't have.
    Failed,
    /// A newer commit is installed than the one running: a restart picks it up.
    Installed,
}

/// Which banner to show. `enabled` is the build's and the user's say-so
/// together.
pub fn notice(enabled: bool, s: &PortalState) -> Notice {
    if !enabled || s.version < MIN_PORTAL_VERSION {
        return Notice::None;
    }
    match s.progress {
        Some((PROGRESS_RUNNING, percent)) => return Notice::Updating(percent.min(100)),
        Some((PROGRESS_DONE, _)) => return Notice::Installed,
        Some((PROGRESS_ERROR, _)) => return Notice::Failed,
        // Empty: nothing was installed, so the commits still say it all.
        _ => {}
    }
    // An update that is out wins over one already installed: installing it
    // ends with a restart anyway.
    if !s.remote.is_empty() && s.remote != s.local && s.remote != s.dismissed {
        Notice::Available
    } else if !s.local.is_empty() && !s.running.is_empty() && s.local != s.running {
        Notice::Installed
    } else {
        Notice::None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn notice_follows_the_portal() {
        let state = |version, (running, local, remote): (&str, &str, &str), progress| PortalState {
            version,
            running: running.into(),
            local: local.into(),
            remote: remote.into(),
            progress,
            dismissed: String::new(),
        };
        let current = ("a", "a", "a");
        let available = ("a", "a", "b");
        let installed = ("a", "b", "b");
        let both = ("a", "b", "c");
        let cases = [
            (true, state(1, available, None), Notice::None),
            (true, state(0, both, None), Notice::None),
            (false, state(2, available, None), Notice::None),
            (
                false,
                state(2, installed, Some((PROGRESS_DONE, 100))),
                Notice::None,
            ),
            (true, state(2, ("", "", ""), None), Notice::None),
            (true, state(2, current, None), Notice::None),
            (true, state(2, available, None), Notice::Available),
            (true, state(2, installed, None), Notice::Installed),
            (true, state(2, both, None), Notice::Available),
            (
                true,
                state(2, available, Some((PROGRESS_RUNNING, 40))),
                Notice::Updating(40),
            ),
            (
                true,
                state(2, available, Some((PROGRESS_RUNNING, 400))),
                Notice::Updating(100),
            ),
            (true, state(2, available, Some((1, 0))), Notice::Available),
            (true, state(2, current, Some((1, 0))), Notice::None),
            (
                true,
                state(2, available, Some((PROGRESS_DONE, 100))),
                Notice::Installed,
            ),
            (
                true,
                state(2, available, Some((PROGRESS_ERROR, 0))),
                Notice::Failed,
            ),
            (true, state(2, ("", "a", "b"), None), Notice::Available),
            (true, state(2, ("", "a", "a"), None), Notice::None),
        ];
        for (enabled, s, expected) in cases {
            assert_eq!(notice(enabled, &s), expected, "enabled {enabled}, {s:?}");
        }
    }

    #[test]
    fn update_available_lets_the_commits_decide_again() {
        // (progress before, commits reported, expected notice)
        let cases = [
            (
                Some((PROGRESS_ERROR, 0)),
                ("a", "b", "b"),
                Notice::Installed,
            ),
            (
                Some((PROGRESS_ERROR, 0)),
                ("a", "a", "c"),
                Notice::Available,
            ),
            (Some((PROGRESS_DONE, 100)), ("a", "a", "a"), Notice::None),
            (
                Some((PROGRESS_RUNNING, 30)),
                ("a", "a", "b"),
                Notice::Updating(30),
            ),
        ];
        for (progress, (running, local, remote), expected) in cases {
            let mut s = PortalState {
                version: 2,
                progress,
                ..Default::default()
            };
            s.update_available(running.into(), local.into(), remote.into());
            assert_eq!(notice(true, &s), expected, "{progress:?}");
        }
    }

    #[test]
    fn percent_spans_every_operation() {
        for (progress, op, n_ops, expected) in [
            (50, None, None, 50),
            (150, None, None, 100),
            (50, Some(0), Some(2), 25),
            (50, Some(1), Some(2), 75),
            (100, Some(2), Some(3), 100),
            (40, Some(0), Some(0), 40),
            (40, Some(5), Some(2), 40),
        ] {
            assert_eq!(
                overall_percent(progress, op, n_ops),
                expected,
                "{op:?}/{n_ops:?}"
            );
        }
    }

    #[test]
    fn later_holds_only_for_the_commit_it_answered() {
        let mut s = PortalState {
            version: 2,
            running: "a".into(),
            local: "a".into(),
            remote: "b".into(),
            dismissed: "b".into(),
            ..Default::default()
        };
        assert_eq!(notice(true, &s), Notice::None);
        s.remote = "d".into();
        assert_eq!(notice(true, &s), Notice::Available);
    }
}
