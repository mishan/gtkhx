//! Static-library facade for GtkHx's C-facing protocol tests.
//!
//! Cargo consumes external `hxproto` as an rlib dependency, while GtkHx's
//! focused C protocol tests need its exported C ABI in a standalone archive.
//! Referencing the dependency here makes rustc bundle those symbols without
//! bringing the complete GtkHx FFI graph into the test binaries.

use hxproto as _;
