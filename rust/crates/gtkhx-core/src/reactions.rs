//! What the Reactions draft (capability bit 19) tells the view, on the
//! session's `reactions` signal (`hxhandlers::recv::reactions`).

use hxproto::reactions::{Mine, Summary};
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
}
