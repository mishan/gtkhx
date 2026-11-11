//! The Reactions draft (capability bit 19): Send Reaction (710) and Get
//! Reactions (712), on the Replies draft's packed targets. Each builder
//! returns `None` for what the wire can't carry.

use hxproto::messages::tag;
use hxproto::reactions::{Key, ADD, CLEAR, GET, REMOVE, SEND};
use hxproto::target::Target;

use crate::Request;

/// The most targets one Get Reactions names.
pub const MAX_TARGETS: usize = 50;

/// SEND_REACTION (710): `action` on `target`, `ADD` or `REMOVE` with a
/// `key`, or a moderator's `CLEAR` with none; for an instant message,
/// `with` is the conversation's other party's Login.
pub fn send(
    target: &Target,
    action: u16,
    key: Option<&Key>,
    with: Option<&[u8]>,
) -> Option<Request> {
    let mut chunks = vec![
        (tag::TARGET, target.pack()?),
        (tag::REACTION_ACTION, action.to_be_bytes().to_vec()),
    ];
    match (action, key) {
        (ADD | REMOVE, Some(k)) => chunks.push((tag::REACTION_KEY, k.pack()?)),
        (CLEAR, None) => {}
        _ => return None,
    }
    chunks.extend(with.map(|w| (tag::FRIEND_LOGIN, w.to_vec())));
    Some(Request {
        opcode: SEND,
        chunks,
    })
}

/// GET_REACTIONS (712): the reactions on 1 to [`MAX_TARGETS`] targets,
/// and beside exactly one, who holds `key`.
pub fn get(targets: &[Target], key: Option<&Key>) -> Option<Request> {
    if !(1..=MAX_TARGETS).contains(&targets.len()) || (key.is_some() && targets.len() != 1) {
        return None;
    }
    let mut chunks = targets
        .iter()
        .map(|t| Some((tag::TARGET, t.pack()?)))
        .collect::<Option<Vec<_>>>()?;
    if let Some(k) = key {
        chunks.push((tag::REACTION_KEY, k.pack()?));
    }
    Some(Request {
        opcode: GET,
        chunks,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn each_request_goes_as_the_draft_has_it_or_not_at_all() {
        let line = |id: u64| Target::Line { channel: 0, id };
        let packed = |id: u64| line(id).pack().unwrap();
        let thumbs = Key::Emoji("\u{1F44D}".into());
        let key = thumbs.pack().unwrap();
        let req = |opcode: u32, chunks: Vec<(u16, Vec<u8>)>| Some(Request { opcode, chunks });
        let many: Vec<Target> = (1..=51).map(line).collect();
        for (case, got, want) in [
            (
                "an add",
                send(&line(9), ADD, Some(&thumbs), None),
                req(
                    710,
                    vec![
                        (0x0f24, packed(9)),
                        (0x0f26, vec![0, 1]),
                        (0x0f25, key.clone()),
                    ],
                ),
            ),
            (
                "an instant message's removal",
                send(
                    &Target::Im {
                        guid: [7; 16],
                        sender: b"bob".to_vec(),
                    },
                    REMOVE,
                    Some(&thumbs),
                    Some(b"bob"),
                ),
                req(
                    710,
                    vec![
                        (0x0f24, [&[2][..], &[7; 16], &[3], b"bob"].concat()),
                        (0x0f26, vec![0, 2]),
                        (0x0f25, key.clone()),
                        (0x0600, b"bob".to_vec()),
                    ],
                ),
            ),
            (
                "a clear",
                send(&line(9), CLEAR, None, None),
                req(710, vec![(0x0f24, packed(9)), (0x0f26, vec![0, 3])]),
            ),
            ("an add with no key", send(&line(9), ADD, None, None), None),
            (
                "a clear with a key",
                send(&line(9), CLEAR, Some(&thumbs), None),
                None,
            ),
            (
                "the most targets",
                get(&many[..50], None),
                req(712, (1..=50).map(|id| (0x0f24, packed(id))).collect()),
            ),
            (
                "who holds a key",
                get(&many[..1], Some(&thumbs)),
                req(712, vec![(0x0f24, packed(1)), (0x0f25, key.clone())]),
            ),
            ("no target", get(&[], None), None),
            ("past the most", get(&many, None), None),
            ("a key beside two", get(&many[..2], Some(&thumbs)), None),
        ] {
            assert_eq!(got, want, "{case}");
        }
    }
}
