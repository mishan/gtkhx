//! What the Reactions draft (capability bit 19) tells the view, on the
//! session's `reactions` signal (`hxhandlers::recv::reactions`).

use hxproto::reactions::{Key, Mine, Summary};
use hxproto::target::Target;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    /// What `target` holds now, as the server said: its summary and which
    /// of its keys are ours, or nothing, where it cleared them or answered
    /// without the target.
    Now {
        target: Target,
        reactions: Option<(Summary, Mine)>,
    },
    /// The user's reaction with `key` on `target` was made, where it
    /// `add`ed, or taken back; `count` hold the key after, where the server
    /// says.
    Acted {
        target: Target,
        key: Key,
        add: bool,
        count: Option<u32>,
    },
    /// The server said to slow down: the view asks again with
    /// `send::reactions::retry` where the user still wants it.
    PutOff { target: Target, key: Key, add: bool },
    /// It was refused, with the draft's `reason` code and the server's own
    /// `error` text where each was given.
    Refused {
        target: Target,
        key: Key,
        reason: Option<u16>,
        error: Option<String>,
    },
}
