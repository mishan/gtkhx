//! Get Device Challenge's answer read, and Prove Device's proof signed.

use hl_identity::proof::canonical_host;
use hl_identity::{DeviceKey, DeviceProof, ServerChallenge};

use crate::Trusted;

/// The clock skew allowed between this machine and a server, in seconds,
/// as hxd-ng allows it the other way.
pub const SKEW: u64 = 300;

/// The TLS 1.3 exporter a proof on a TLS connection is bound with, with
/// an empty context and 32 bytes.
pub const EXPORTER_LABEL: &[u8] = b"EXPERIMENTAL-hl-identity-device-proof";

/// The host Get Device Challenge names: `host` as dialed, in the identity
/// spec's form, always with its `port`, 80 and 443 included, since the
/// server compares it with the listener the request arrived on. `None`
/// for something that is not a host.
pub fn proof_host(host: &str, port: u16) -> Option<String> {
    let bracketed = match host.contains(':') && !host.starts_with('[') {
        true => format!("[{host}]"),
        false => host.to_owned(),
    };
    let canonical = canonical_host(&format!("{bracketed}:{port}"))?;
    Some(match port {
        // The form leaves these two off; the request does not.
        80 | 443 => format!("{canonical}:{port}"),
        _ => canonical,
    })
}

/// The challenge a Get Device Challenge for `sent` was answered with, as
/// a client may sign for it: under the TRTP domain, signed by the key it
/// names, for that host, and not expired at `now`. Which key that should
/// be is the pin's ([`crate::Pins`]).
pub fn read_challenge(
    bytes: &[u8],
    sent: &str,
    now: u64,
) -> Result<ServerChallenge, hl_identity::Error> {
    let c = ServerChallenge::parse_trtp(bytes)?;
    c.check(&c.challenge, &c.server_key, sent, now, SKEW)?;
    Ok(c)
}

/// Prove Device's proof of `key` for `challenge`, whose key the pins
/// trust, at `now`, bound to the connection by its TLS 1.3 `exporter`
/// where there is one.
pub fn answer(
    key: &DeviceKey,
    challenge: &Trusted,
    exporter: Option<&[u8; 32]>,
    now: u64,
) -> Result<Vec<u8>, hl_identity::Error> {
    let challenge = challenge.challenge();
    DeviceProof::sign(
        key,
        &challenge.challenge,
        &challenge.server_key,
        &challenge.host,
        now,
        exporter,
    )
}
