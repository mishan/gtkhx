//! Private messages.

use hxproto::build::{self, HxChunk, MsgRequest};
use hxproto::messages::{tag, ClientHdr};
use hxproto::target::Target;

use crate::Request;

/// SEND_MSG (108): `body`, already encoded for the wire, to user `uid`,
/// with the picture `media` names (its handle and type) and the message
/// `reply` answers. `None` for a body or a media field past a field's
/// length, or a reply that has no packed form.
pub fn send(
    uid: u16,
    body: &[u8],
    media: Option<(&[u8], &[u8])>,
    reply: Option<&Target>,
) -> Option<Request> {
    let mut chunks = [HxChunk::EMPTY; 2];
    let mut scratch = [0u8; 2];
    let hc = build::build_msg_chunks(&MsgRequest { uid, body }, &mut chunks, &mut scratch);
    let mut r = Request::from_built(ClientHdr::Msg as u32, &chunks, hc)?;
    if let Some((id, mime)) = media {
        if id.len().max(mime.len()) > usize::from(u16::MAX) {
            return None;
        }
        r.chunks.push((tag::CHAT_MEDIA_ID, id.to_vec()));
        r.chunks.push((tag::CHAT_MEDIA_TYPE, mime.to_vec()));
    }
    if let Some(t) = reply {
        r.chunks.push((tag::REPLY_TO, t.pack()?));
    }
    Some(r)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_message_carries_its_picture_then_its_reply_after_its_text() {
        let to = Target::Pm {
            stored: true,
            reference: [4; 16],
        };
        let head = [(0x67, vec![2, 1]), (0x65, b"yes".to_vec())];
        let picture = [(0x0202, b"h1".to_vec()), (0x0201, b"image/png".to_vec())];
        let re = [(0x0f21, [&[5, 1][..], &[4; 16]].concat())];
        let pic: (&[u8], &[u8]) = (b"h1", b"image/png");
        for (case, media, reply, tail) in [
            ("text alone", None, None, vec![]),
            ("a picture", Some(pic), None, picture.to_vec()),
            ("a reply", None, Some(&to), re.to_vec()),
            (
                "both",
                Some(pic),
                Some(&to),
                [&picture[..], &re[..]].concat(),
            ),
        ] {
            let r = send(513, b"yes", media, reply).unwrap();
            assert_eq!(r.opcode, 108, "{case}");
            assert_eq!(r.chunks, [&head[..], &tail[..]].concat(), "{case}");
        }
    }

    #[test]
    fn what_the_wire_cannot_carry_is_refused() {
        let most = vec![0u8; usize::from(u16::MAX)];
        let past = vec![0u8; usize::from(u16::MAX) + 1];
        let nobody = Target::Im {
            guid: [1; 16],
            sender: Vec::new(),
        };
        for (case, body, media, reply, sent) in [
            (
                "every field at the most",
                &most,
                Some((&most, &most)),
                None,
                true,
            ),
            ("a body past it", &past, None, None, false),
            ("a handle past it", &most, Some((&past, &most)), None, false),
            ("a type past it", &most, Some((&most, &past)), None, false),
            (
                "a reply with no packed form",
                &most,
                None,
                Some(&nobody),
                false,
            ),
        ] {
            let media = media.map(|(id, mime)| (&id[..], &mime[..]));
            assert_eq!(send(1, body, media, reply).is_some(), sent, "{case}");
        }
    }
}
