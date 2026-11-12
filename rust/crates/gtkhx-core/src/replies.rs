//! What the Replies draft (capability bit 18) tells the view, on the
//! session's `replies` signal (`hxhandlers::recv::replies`).

use hxsession::HistoryEntry;

#[derive(Debug, Clone)]
pub enum Event {
    /// The answer to a request for chat `cid`'s lines `asked` by id: the
    /// entries of those the server gave, and whether it left some out for
    /// the frame.
    Quotes {
        cid: u32,
        asked: Vec<u64>,
        entries: Vec<HistoryEntry>,
        has_more: bool,
    },
    /// That request was refused; `slow` where the server said to slow
    /// down.
    Refused {
        cid: u32,
        asked: Vec<u64>,
        slow: bool,
    },
}
