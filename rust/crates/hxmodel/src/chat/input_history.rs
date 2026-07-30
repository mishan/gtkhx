//! `InputHistory` — chat input line history with Up/Down navigation and
//! draft recovery (the first typed sub-struct peeled off the C
//! `struct gtkhx_chat` — see the Chat-model re-think section of
//! `docs/rust/ROADMAP.md`).
//!
//! Replaces the GNU-readline `HISTORY` the chat input used (`gchat->
//! chat_history`) plus the `chat_history_draft` Up-arrow recovery slot, folding
//! both — and the Up/Down key logic that lived inline in
//! `chat_input_key_pressed` — into one pure, unit-tested unit. No readline, no
//! glib, no widgets: the caller feeds the live buffer text in and applies the
//! returned string.
//!
//! Navigation mirrors readline's `history_offset` exactly: `offset` runs
//! `0..=entries.len()`, and `offset == entries.len()` is the "draft" position
//! (readline's `history_offset == history_length`, where `current_history()`
//! returns NULL). `record` = `add_history` + `using_history`.

/// Chat input history + navigation cursor + the Up-arrow draft.
#[derive(Debug, Default, Clone)]
pub struct InputHistory {
    entries: Vec<String>,
    /// Cursor in `0..=entries.len()`. `== entries.len()` means "at the draft"
    /// (not currently paging through history).
    offset: usize,
    /// The in-progress line snapshotted on the first Up press, restored when
    /// Down steps back past the newest entry. `None` while not navigating.
    draft: Option<String>,
}

impl InputHistory {
    pub fn new() -> Self {
        Self::default()
    }

    /// Number of stored lines.
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// Record a just-sent line and reset navigation to the bottom (readline
    /// `add_history` + `using_history`). Recorded unconditionally, matching the
    /// C, which added the buffer text even when empty.
    pub fn record(&mut self, line: &str) {
        self.entries.push(line.to_string());
        self.offset = self.entries.len();
        self.draft = None;
    }

    /// Up arrow. `current` is the live buffer text, snapshotted as the draft on
    /// the first press of a navigation cycle (bash-style: only the original
    /// draft is preserved, not edits made while paging). Returns the text the
    /// input should show, or `None` when there's nothing older to show.
    pub fn up(&mut self, current: &str) -> Option<String> {
        // At the draft position → snapshot the in-progress line first (readline
        // `current_history() == NULL`). Skip when the history is empty: there's
        // nothing to navigate to, so the draft could never be restored — no
        // point capturing (and allocating) it.
        if !self.entries.is_empty() && self.offset >= self.entries.len() {
            self.draft = Some(current.to_string());
        }
        if self.offset == 0 {
            return None; // already at the oldest entry (readline previous → NULL)
        }
        self.offset -= 1;
        Some(self.entries[self.offset].clone())
    }

    /// Down arrow. Returns the text to show, or `None` when already at the
    /// draft (nothing to do — the C returned without touching the buffer).
    /// Stepping forward past the newest entry restores the saved draft.
    pub fn down(&mut self) -> Option<String> {
        if self.offset >= self.entries.len() {
            return None; // already at the draft
        }
        self.offset += 1;
        if self.offset >= self.entries.len() {
            // Stepped past the newest entry → restore the draft (readline
            // `next_history() == NULL`). Default to empty if none was captured.
            Some(self.draft.take().unwrap_or_default())
        } else {
            Some(self.entries[self.offset].clone())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_history_navigation_is_inert() {
        let mut h = InputHistory::new();
        assert!(h.is_empty());
        assert_eq!(h.up("draft"), None); // nothing older
        assert_eq!(h.down(), None); // already at draft
    }

    #[test]
    fn up_walks_back_then_stops_at_oldest() {
        let mut h = InputHistory::new();
        h.record("a");
        h.record("b");
        // First Up snapshots the draft and shows the newest entry.
        assert_eq!(h.up("draft"), Some("b".to_string()));
        assert_eq!(h.up("ignored"), Some("a".to_string()));
        // At the oldest — further Up is a no-op (stays on "a").
        assert_eq!(h.up("ignored"), None);
    }

    #[test]
    fn down_walks_forward_and_restores_draft() {
        let mut h = InputHistory::new();
        h.record("a");
        h.record("b");
        h.up("draft"); // → "b"
        h.up("x"); // → "a"
        assert_eq!(h.down(), Some("b".to_string()));
        // Stepping past the newest entry restores the original draft.
        assert_eq!(h.down(), Some("draft".to_string()));
        // Already at the draft — further Down does nothing.
        assert_eq!(h.down(), None);
    }

    #[test]
    fn draft_snapshots_only_on_first_up_of_a_cycle() {
        let mut h = InputHistory::new();
        h.record("a");
        // Draft captured here ("first"); later Ups pass different buffers but
        // must not overwrite it (we're no longer at the draft position).
        assert_eq!(h.up("first"), Some("a".to_string()));
        assert_eq!(h.up("second"), None); // at oldest
                                          // Down past the newest restores the *first* snapshot, not "second".
        assert_eq!(h.down(), Some("first".to_string()));
    }

    #[test]
    fn record_resets_navigation_to_bottom() {
        let mut h = InputHistory::new();
        h.record("a");
        h.up("draft"); // navigate up to "a"
                       // A new send resets to the bottom + clears the draft.
        h.record("b");
        assert_eq!(h.len(), 2);
        // Down at the bottom is inert; Up starts a fresh cycle from the newest.
        assert_eq!(h.down(), None);
        assert_eq!(h.up("new-draft"), Some("b".to_string()));
        assert_eq!(h.up("x"), Some("a".to_string()));
    }

    #[test]
    fn down_from_draft_without_prior_up_is_inert() {
        let mut h = InputHistory::new();
        h.record("a");
        assert_eq!(h.down(), None); // sitting at the draft, nothing to do
    }
}
