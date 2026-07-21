//! File-transfer worker port — codec layer.
//!
//! See `docs/files-rust-migration-scope.md`. The goal is to move the
//! four HTXF worker state machines (`get_thread`, `put_thread`,
//! `folder_get_thread`, `folder_put_thread`) out of `src/xfers.c` into
//! Rust. This crate starts with the fiddliest, highest-risk part: the
//! FFO / FILP frame codec and the HFS fork math (`ffo` module), ported
//! byte-for-byte from `src/xfers.c::file_recv_one` and pinned against
//! the same wire-shape oracle the C test `tests/proto/test_large_file.c`
//! uses.
//!
//! It is deliberately pure and dependency-free so the byte math is
//! fully unit-tested without a socket or a live server. The worker
//! state machines (on the hxbridge tokio pool) and the `gtkhx_xfer_*`
//! C-ABI surface arrive in later slice-1 increments.

pub mod ffi;
pub mod ffo;
