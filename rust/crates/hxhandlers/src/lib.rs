//! `hxhandlers` — the Hotline protocol handler layer.
//!
//! Ten crates that were the same crate wearing ten hats. Every one was thin
//! glue of an identical shape — parse a frame via `hotline-proto`, apply a
//! gate, emit a `GtkhxSession` signal, hand a discriminant back to the C
//! caller — and they shared their dependencies, their consumers, and their
//! position in the old hand-ordered link line. Three were under 250 lines.
//!
//! They are modules now. The split survives as the module layout:
//!
//! - [`recv`] — the receive side, one module per domain, holding the bodies
//!   that used to live in `rcv.c`.
//! - [`send`] — the wire-out senders for the domains whose send path is Rust.
//!
//! ## Why this could not happen sooner
//!
//! The merge was blocked by a dependency cycle, not by the code:
//! `gtkhx-ui → hxchat-send → hxuser-recv → gtkhx-ui`. Folding the ten crates
//! together would only have traded a 3-node cycle for a 2-node one
//! (`gtkhx-ui ↔ hxhandlers`), which Cargo rejects just as firmly.
//!
//! The actual cause was that model code was living in the UI crate: what the
//! receive handlers wanted from `gtkhx-ui` was `HxConversation` and the member
//! model. Moving `conversation` and `chat_members` into `hxmodel` removed the
//! handler layer's dependency on `gtkhx-ui` entirely, and the merge became
//! trivially clean. See `docs/rust/crate-layout.md`.
//!
//! ## What did not change
//!
//! Every module keeps the exact `#[no_mangle]` C ABI its former crate
//! exported, so `rcv.c`'s dispatch, the C send-path callers, and the
//! `gtkhx-ffi` façade are all unaffected. The `#[cfg(test)]` doubles that let
//! each handler's tests run headless moved with their module.

pub mod recv;
pub mod send;
// The file-transfer registry (the `xfers[]` list) — Y1 of the xfers.c → Rust
// migration. See docs/rust/ROADMAP.md.
pub mod xfer;
