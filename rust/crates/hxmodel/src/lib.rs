//! `hxmodel` — GtkHx's client-side model layer.
//!
//! One crate per domain concept was five crates (`hxmodel::chat`,
//! `hxmodel::member`, `hxmodel::news`, `hxmodel::files`, `hxmodel::files_entry`) that
//! shared their dependencies, their consumers, and their reason to exist. Two
//! of the five pairs were a pure model and its `gio::ListModel` wrapper —
//! one concept split across a crate boundary for no benefit. They are modules
//! here instead; the split survives as the module layout.
//!
//! Everything in here is *model*: state and the rules over it, no widgets. The
//! glib-flavoured parts (the `glib::subclass` GObjects and `gio::ListModel`
//! implementations) still hold no GTK, so `cargo test -p hxmodel` runs
//! headless.
//!
//! - [`chat`] — the pure conversation model: [`chat::Conversation`],
//!   [`chat::Member`], nick completion, and the readline-style
//!   [`chat::input_history`].
//! - [`member`] — chat membership as a `gio::ListModel` (`HxMember` /
//!   `HxMemberModel`) over [`chat`]. The authoritative per-chat membership
//!   store and the owner of the per-uid ignore flag.
//! - [`news`] — the 1.5 news browser's model: the pure post-threading layout
//!   plus the [`news::node`] `HxNewsNode` GObject and its `HxNewsDate`.
//! - [`files`] — files-browser presentation/navigation logic: the file-type
//!   icon/label tables and the remote provider's path-navigation model.
//! - [`files_entry`] — `HxFileEntry`, one files-browser row as a GObject.
//! - [`conversation`] — `HxConversation`: the per-chat registry entry (cid,
//!   subject, membership, and an opaque handle to its view).
//! - [`chat_members`] — the C ABI over [`chat`] + [`member`]: membership
//!   queries, the per-uid ignore flag, nick completion and input history.
//!
//! `conversation` and `chat_members` lived in `gtkhx-ui` until the step 3
//! consolidation, because the chat-model re-think (ROADMAP M4b.5) landed them
//! there. They are model, not view — they hold no widgets, and their being in
//! the UI crate forced the receive-side handlers to depend on `gtkhx-ui`,
//! which is what created the `gtkhx-ui -> hxhandlers::send::chat -> hxhandlers::recv::user` cycle
//! that blocked both the extern-to-Cargo conversion and the `hxhandlers`
//! merge.
//!
//! Each module keeps the exact C ABI its former crate exported, so the C side
//! and the `gtkhx-ffi` façade are unaffected by the consolidation.

pub mod chat;
pub mod chat_members;
pub mod conversation;
pub mod files;
pub mod files_entry;
pub mod member;
pub mod news;
