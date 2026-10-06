//! `gtkhx-core` — the GObject layer between the protocol and the UI.
//!
//! - [`session`] — `GtkhxSession`, the singleton every model-side file emits
//!   through, plus the `HxChatRegistry` (`cid → HxConversation`).
//! - [`boxed`] — the boxed payloads those signals carry (`HxMsgEvent`,
//!   `HxChatEvent` + `HxChatMedia`, `HxTrackerServer` + `HxTrackerV3Meta`,
//!   and the inline-media `MediaTable`).
//! - [`conn`] — `struct htlc_conn`, the per-connection state.
//!
//! ## Why `hxtask` is not in here
//!
//! The boxed payloads used to be a crate of their own, for two reasons. The
//! first — two staticlibs each bundling the boxed `_copy`/`_free`, colliding at
//! the binary link — died with the `gtkhx-ffi` façade, and the two merged. The
//! second reason survives as a constraint on *this* crate: it is deliberately
//! **glib-only with zero undefined externs**, so a Tier 2 proto test that pulls
//! `hx_msg_event_copy` can link this archive on its own. Those tests link the
//! standalone staticlib, not the façade, so the façade does nothing for them.
//!
//! `hxtask` has undefined C externs (`hx_bridge_send_frame`,
//! `proto_trace_send_begin`, …). Merging it here would make a Tier 2 test
//! that wants only a boxed `_copy` drag in the send primitive's unresolved
//! references and fail to link. That is not hypothetical: it is exactly how
//! the first attempt at this crate failed.
//!
//! The three modules here are all extern-free, so the merged archive stays
//! self-contained and the Tier 2 tests keep linking it alone. **Anything added
//! to this crate must also be extern-free**, or those tests break.

pub mod boxed;
pub mod conn;
pub mod session;
