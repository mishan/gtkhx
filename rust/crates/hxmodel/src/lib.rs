//! `hxmodel` — GtkHx's client-side model layer.
//!
//! One crate per domain concept was five crates (`hxchat-model`,
//! `hxmember-model`, `hxnews-model`, `hxfiles-model`, `hxfiles-entry`) that
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
//!
//! Each module keeps the exact C ABI its former crate exported, so the C side
//! and the `gtkhx-ffi` façade are unaffected by the consolidation.

pub mod chat;
pub mod files;
pub mod files_entry;
pub mod member;
pub mod news;
