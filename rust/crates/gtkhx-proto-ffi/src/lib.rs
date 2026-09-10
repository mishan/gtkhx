//! GtkHx's C ABI over the shared `hxproto` crate.
//!
//! `hxproto` lives in hx-libs and is pure Rust. Every `#[no_mangle]
//! extern "C"` entry point the C tree calls into the protocol layer lives
//! here instead, as a thin marshalling shim over the hxproto API.
//!
//! The C side hand-declares the prototypes (`src/hotline_proto.h`,
//! `src/proto_helpers.h`), so signature drift surfaces as an undefined
//! symbol at link time. No cbindgen — the surface has no opaque pointers to
//! manage.
//!
//! Two consumers link it. `gtkhx-ffi` bundles the rlib into
//! `libgtkhx_ffi.a` for the binary; the standalone staticlib is what the C
//! protocol tests in `tests/meson.build` link, so they get the protocol ABI
//! without dragging in GTK.

#![allow(unsafe_op_in_unsafe_fn)]

pub mod dispatch;
pub mod ffi;
pub mod login;
pub mod user_change;
