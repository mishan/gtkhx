//! Category-based runtime debug logging, mirroring the C-side
//! `debug.{c,h}` infrastructure.
//!
//! Same `GTKHX_DEBUG` env var as the C runtime, same comma-separated
//! category syntax, same `[<cat>] <message>` stderr output format, same
//! `all` keyword to turn every category on at once. The Rust side
//! shares the env var with the C side — `GTKHX_DEBUG=voice,voice-pipe`
//! enables both C's `voice` opcode trace and Rust's voice-pipeline
//! diagnostics in the same launch.
//!
//! ## Categories used by `hxvoice-runtime`
//!
//! - `voice-pipe` — structural events on the WebRTC pipeline:
//!   pad-added firings, receive-bin linking, on-new-transceiver
//!   codec-preferences pinning, SDP `a=mid:` lines as they arrive,
//!   stale-pad cleanup warnings. Low volume: a few lines per call
//!   setup. Useful for "did the receive leg actually wire up?"
//!   questions.
//! - `voice-flow` — buffer-counting pad probes along the send + receive
//!   chains (payloader output, webrtcbin src pad, depay sink,
//!   mulawdec src, audioresample src, autoaudiosink sink). Each probe
//!   logs buffer #1 and every 50th — at rtppcmupay's default 20 ms
//!   ptime (50 packets/sec) that's ~1 s of live audio per log line.
//!   Useful for "is audio actually flowing?" questions and pinpointing
//!   where it stops if it isn't.
//!
//! ## Usage
//!
//! ```ignore
//! use crate::debug;
//!
//! debug::log!("voice-pipe", "receive bin LINKED for mid={mid}");
//! debug::log!("voice-flow", "{bin_name} {pad_label}: buffer #{n}");
//! ```
//!
//! ## Implementation notes
//!
//! The enabled-categories set is built lazily on first `category_enabled`
//! call via `OnceLock`. We deliberately don't print the "enabled
//! categories: …" banner the C side prints — the C `debug_init` does
//! that already, and double-printing it from Rust would clutter every
//! launch. The Rust side just inherits whatever the user already set.
//!
//! All callsites go through the `log!` macro which short-circuits on a
//! single `HashSet::contains` lookup when the category is off — no
//! string formatting cost when disabled, no allocation, no GIO. Cheap
//! enough to leave permanent calls in the hot path (e.g. pad probes
//! firing every 50th buffer).

use std::collections::HashSet;
use std::sync::OnceLock;

static ENABLED: OnceLock<HashSet<String>> = OnceLock::new();

/// Initialise (lazily) and return the set of enabled categories.
///
/// Builds the set from `GTKHX_DEBUG` on first call. Subsequent calls
/// return the cached set. Returns an empty set if the env var is unset
/// or empty.
fn enabled() -> &'static HashSet<String> {
    ENABLED.get_or_init(|| {
        let mut set = HashSet::new();
        let Ok(spec) = std::env::var("GTKHX_DEBUG") else {
            return set;
        };
        if spec.is_empty() {
            return set;
        }
        for part in spec.split(',') {
            let trimmed = part.trim();
            if !trimmed.is_empty() {
                set.insert(trimmed.to_string());
            }
        }
        set
    })
}

/// Is the given category enabled at the current `GTKHX_DEBUG` setting?
///
/// True if the category itself is in the env var's comma-separated
/// list, OR if `all` is in the list. False otherwise.
///
/// Thread-safe: read-only access to a lazily-built static after the
/// first initialisation.
pub fn category_enabled(cat: &str) -> bool {
    let set = enabled();
    set.contains("all") || set.contains(cat)
}

/// Log a line under `category`, only if that category is enabled.
///
/// Output format matches the C side: `[<cat>] <formatted text>\n` to
/// stderr. Auto-newline is provided by `eprintln!`, so callers should
/// NOT include a trailing `\n` in their format string.
///
/// Cheap when the category is off — single `HashSet::contains` lookup,
/// no formatting work performed.
///
/// Example:
/// ```ignore
/// debug::log!("voice-pipe", "receive bin LINKED for mid={mid}");
/// ```
#[macro_export]
macro_rules! debug_log {
    ($cat:expr, $($arg:tt)*) => {
        if $crate::debug::category_enabled($cat) {
            eprintln!("[{}] {}", $cat, format_args!($($arg)*));
        }
    };
}

/// Re-export under `debug::log!` so callsites read `debug::log!(...)`
/// rather than the bare `debug_log!`. Matches the C `debug_log(...)`
/// shape.
pub use crate::debug_log as log;

#[cfg(test)]
mod tests {
    use super::*;

    /// `category_enabled` is False for any category when `GTKHX_DEBUG`
    /// is unset. The env var is process-global, so we can't safely
    /// mutate it during tests; this test just exercises the empty-set
    /// fall-through.
    #[test]
    fn empty_env_disables_all_categories() {
        // The OnceLock is initialised lazily. If a prior test in the
        // same process already set it (with whatever env was at that
        // moment), this assertion may read that earlier state, not
        // our current state. So we only assert the negative: an
        // unknown category should never be enabled unless the env
        // explicitly opted into it or `all`.
        //
        // Skip the assert when the developer ran the suite under
        // `GTKHX_DEBUG=all` — that legitimately enables every
        // category including our bogus one, and the test should
        // be a no-op rather than a false failure in that case.
        if enabled().contains("all") {
            return;
        }
        let bogus = "_test_category_that_no_one_should_set";
        assert!(!category_enabled(bogus));
    }
}
