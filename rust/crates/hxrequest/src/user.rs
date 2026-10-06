//! User requests: a user's info, a kick, an admin's broadcast, and the
//! accounts an admin reads, makes, changes and deletes.
//!
//! An account's login and password go obfuscated, every byte inverted, but
//! in the read, which names the login as it is. They and the account's
//! name are bytes: what the user typed, as UTF-8, or what a read gave back.
//!
//! Each builder returns `None` for input the wire can't carry: a chunk
//! longer than its u16 length allows.

use hxproto::build::{self, AccountModifyRequest, BroadcastRequest, HxChunk, UserKickRequest};
use hxproto::messages::ClientHdr;

use crate::Request;

/// USER_GETINFO: what the server says of `uid`.
pub fn info(uid: u16) -> Option<Request> {
    let mut chunks = [HxChunk::EMPTY; 1];
    let mut scratch = [0u8; 2];
    let hc = build::build_user_getinfo_chunks(uid, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::UserGetInfo as u32, &chunks, hc)
}

/// USER_KICK: disconnect `uid`, and ban them too when `ban`.
pub fn kick(uid: u16, ban: bool) -> Option<Request> {
    let req = UserKickRequest {
        uid,
        ban: u16::from(ban),
    };
    let mut chunks = [HxChunk::EMPTY; 2];
    let mut scratch = [0u8; 4];
    let hc = build::build_user_kick_chunks(&req, &mut chunks, &mut scratch);
    Request::from_built(ClientHdr::UserKick as u32, &chunks, hc)
}

/// MSG_BROADCAST: `text` to everyone on the server.
pub fn broadcast(text: &[u8], utf8: bool) -> Option<Request> {
    let body = hxtext::for_wire(text, utf8, true);
    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_broadcast_chunks(&BroadcastRequest { body: &body }, &mut chunks);
    Request::from_built(ClientHdr::MsgBroadcast as u32, &chunks, hc)
}

fn obfuscate(b: &[u8]) -> Vec<u8> {
    b.iter().map(|x| !x).collect()
}

/// ACCOUNT_READ: the account `login` names.
pub fn account_read(login: &[u8]) -> Option<Request> {
    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_account_read_chunks(login, &mut chunks);
    Request::from_built(ClientHdr::AccountRead as u32, &chunks, hc)
}

/// ACCOUNT_CREATE: a new account. An empty password goes as a single zero
/// byte, as in a save. `access` is the bitmap as the wire carries it.
pub fn account_create(
    login: &[u8],
    password: &[u8],
    name: &[u8],
    access: [u8; 8],
) -> Option<Request> {
    account(ClientHdr::AccountCreate, login, password, name, access)
}

/// ACCOUNT_MODIFY: what the account `login` names holds, replaced. An empty
/// password goes as a single zero byte, which leaves the one it had.
pub fn account_save(
    login: &[u8],
    password: &[u8],
    name: &[u8],
    access: [u8; 8],
) -> Option<Request> {
    account(ClientHdr::AccountModify, login, password, name, access)
}

fn account(
    opcode: ClientHdr,
    login: &[u8],
    password: &[u8],
    name: &[u8],
    access: [u8; 8],
) -> Option<Request> {
    let login = obfuscate(login);
    let password = if password.is_empty() {
        vec![0]
    } else {
        obfuscate(password)
    };
    let req = AccountModifyRequest {
        login: &login,
        password: &password,
        name,
        access,
    };
    let mut chunks = [HxChunk::EMPTY; 4];
    let mut scratch = [0u8; 8];
    let hc = build::build_account_modify_chunks(&req, &mut chunks, &mut scratch);
    Request::from_built(opcode as u32, &chunks, hc)
}

/// ACCOUNT_DELETE: the account `login` names.
pub fn account_delete(login: &[u8]) -> Option<Request> {
    let login = obfuscate(login);
    let mut chunks = [HxChunk::EMPTY; 1];
    let hc = build::build_account_delete_chunks(&login, &mut chunks);
    Request::from_built(ClientHdr::AccountDelete as u32, &chunks, hc)
}

#[cfg(test)]
mod tests;
