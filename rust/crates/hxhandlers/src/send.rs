//! Wire-out senders for the domains whose send path has moved to Rust.
//!
//! Built over `hotline-proto`'s native builders. The remaining C senders
//! (`hx_kick_user`, `hx_get_user_info`, …) are unaffected — these modules keep
//! the exact C ABI their former crates exported.

pub mod chat;
pub mod news;
