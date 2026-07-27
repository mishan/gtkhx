//! Receive-side handlers — the `rcv_task_*` / notification bodies that used to
//! live in `rcv.c`, one module per protocol domain.
//!
//! Each was its own crate before the step 3 consolidation. They share the same
//! shape: parse the frame (natively via `hotline-proto`, or take an
//! already-parsed one from the C dispatcher), consult the model, emit the
//! matching `GtkhxSession` signal, and return a discriminant telling the C
//! caller which branch was taken.

pub mod agreement;
pub mod chat;
pub mod files;
pub mod icon;
pub mod msg;
pub mod news;
pub mod user;
pub mod xfer;
