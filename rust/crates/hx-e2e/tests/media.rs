//! Inline media (the fogWraith extension) as production sends it: each
//! part's reply expected by the session as `hxhandlers::media` expects it,
//! the parts sized and the upload token echoed as it does, and a chat line
//! carrying the picture heard as the session's `Event::Chat`.
#![cfg(feature = "rig")]

use hx_e2e::{servers_with, unique_name, Cap, Client, Server};
use hxnet::Event;
use hxproto::inline_media::{extract_limits_from_message, LimitsAdvertisement, MediaErrorCode};
use hxproto::messages::tag;
use hxrequest::{media, Request};
use hxsession::{ChatMedia, Expect, Handled};

/// `HTLC_CAP_INLINE_MEDIA`.
const CAP_INLINE_MEDIA: u16 = 0x0008;
/// A part's size when the server names none, and the most it may name, as
/// `hxhandlers::media` has it.
const PART_SIZE: usize = 60_000;
/// `HX_MEDIA_DEFAULT_MAX_BYTES`: the largest picture, when the server
/// names no limit.
const MAX_BYTES: usize = 256 * 1024;
const PNG: Option<&[u8]> = Some(b"image/png");

/// A guest that agreed to inline media and whose session handles chat,
/// going by a name no other test uses.
fn member(server: &'static Server, who: &str) -> Client {
    // Inside every server's 31-byte cap.
    let nick: String = unique_name(who).chars().take(31).collect();
    let c = Client::login_handling(server, "", None, CAP_INLINE_MEDIA, Handled::CHAT, &nick)
        .unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(
        c.caps() & CAP_INLINE_MEDIA,
        CAP_INLINE_MEDIA,
        "{}: inline media not agreed",
        server.name
    );
    c
}

fn limits(c: &Client) -> LimitsAdvertisement {
    let r = c.login_reply();
    extract_limits_from_message(&r.raw, r.raw.len())
}

/// The size production sends a picture's parts in.
fn part_size(c: &Client) -> usize {
    match limits(c).chunk_size.unwrap_or(0) as usize {
        0 => PART_SIZE,
        n => n.min(PART_SIZE),
    }
}

/// A 4x4 solid red PNG.
fn small_png() -> Vec<u8> {
    encode(4, &[0xff, 0, 0].repeat(16))
}

/// A PNG of noise, which deflate can't shrink, longer than `min` bytes.
fn noise_png(min: usize) -> Vec<u8> {
    let mut dim = 128;
    loop {
        let mut r: u32 = 0xc0ff_ee01;
        let pixels: Vec<u8> = (0..dim * dim * 3)
            .map(|_| {
                r ^= r << 13;
                r ^= r >> 17;
                r ^= r << 5;
                r as u8
            })
            .collect();
        let png = encode(dim, &pixels);
        if png.len() > min {
            return png;
        }
        dim *= 2;
    }
}

fn encode(dim: u32, rgb: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut e = png::Encoder::new(&mut out, dim, dim);
    e.set_color(png::ColorType::Rgb);
    e.set_depth(png::BitDepth::Eight);
    let mut w = e.write_header().expect("a PNG header");
    w.write_image_data(rgb).expect("PNG data");
    w.finish().expect("a PNG");
    out
}

/// The session's answer to `t`.
fn answer(c: &mut Client, t: u32) -> hxsession::Event {
    use hxsession::Event as S;
    loop {
        match c.next_event().unwrap_or_else(|e| panic!("{e}")) {
            Event::Session(
                e @ (S::MediaUploading { trans, .. }
                | S::MediaUploaded { trans, .. }
                | S::MediaPart { trans, .. }
                | S::MediaFailed { trans, .. }
                | S::Failed { trans, .. }),
            ) if trans == t => return e,
            Event::Frame(f) if f.header.trans == t => {
                panic!("{}: the expected reply came whole", c.server().name)
            }
            _ => {}
        }
    }
}

/// Send `pic` as production does: whole if it fits in a part, otherwise
/// in parts on the token the first part's reply gives.
fn upload(c: &mut Client, pic: &[u8], mime: Option<&[u8]>) -> Result<ChatMedia, MediaErrorCode> {
    let parts: Vec<&[u8]> = pic.chunks(part_size(c)).collect();
    let count = u16::try_from(parts.len()).expect("a part count the wire can carry");
    let mut token: Option<Vec<u8>> = None;
    for (i, part) in parts.into_iter().enumerate() {
        let i = i as u16;
        let last = i + 1 == count;
        let req = match (i, &token) {
            (0, _) if last => media::upload(part, mime),
            (0, _) => media::upload_first(part, mime, count),
            (_, Some(t)) => media::upload_next(t, part, i, last),
            (_, None) => panic!("{}: no upload token", c.server().name),
        }
        .expect("an upload request");
        let t = c.send_expecting(&req, Some(Expect::MediaUpload { last }));
        match answer(c, t) {
            hxsession::Event::MediaUploading { token: got, .. } if !last => {
                // Only the first reply need carry it.
                if let Some(got) = got.filter(|g| !g.is_empty()) {
                    token = Some(got);
                }
            }
            hxsession::Event::MediaUploaded { media, .. } if last => return Ok(media),
            hxsession::Event::MediaFailed { code, .. } => return Err(code),
            e => panic!("{}: part {i} of {count}: {e:?}", c.server().name),
        }
    }
    unreachable!("the last part's answer returns")
}

/// Fetch the picture `id` names, part by part as production does; its
/// bytes and how many parts it came in.
fn download(c: &mut Client, id: &[u8]) -> Result<(Vec<u8>, u16), MediaErrorCode> {
    let mut bytes = Vec::new();
    let mut next = None;
    loop {
        let req = media::download(id, next).expect("a download request");
        let t = c.send_expecting(&req, Some(Expect::MediaDownload));
        match answer(c, t) {
            hxsession::Event::MediaPart { part, .. } => {
                bytes.extend_from_slice(&part.payload);
                if part.last {
                    return Ok((bytes, part.parts));
                }
                let n = next.map_or(1, |n| n + 1);
                assert!(
                    n < part.parts,
                    "{}: part {n} of {} and none was the last",
                    c.server().name,
                    part.parts
                );
                next = Some(n);
            }
            hxsession::Event::MediaFailed { code, .. } => return Err(code),
            e => panic!("{}: {e:?}", c.server().name),
        }
    }
}

/// A public chat line carrying `m`, as `hx_send_chat_with_media` sends it.
fn chat_with(text: &str, m: &ChatMedia) -> Request {
    Request {
        opcode: 105,
        chunks: vec![
            (tag::STYLE, 0u16.to_be_bytes().to_vec()),
            (tag::BODY, text.as_bytes().to_vec()),
            (tag::CHAT_MEDIA_ID, m.id.clone()),
            (tag::CHAT_MEDIA_TYPE, m.mime.clone()),
        ],
    }
}

/// The picture of the chat line holding `marker` that `c` hears. Janus
/// sends chat as from uid 0, so the line is known by its text.
fn heard(c: &mut Client, marker: &str) -> Option<ChatMedia> {
    loop {
        if let Event::Session(hxsession::Event::Chat { text, media, .. }) =
            c.next_event().unwrap_or_else(|e| panic!("{e}"))
        {
            if text.contains(marker) {
                return media;
            }
        }
    }
}

/// Upload `pic` and announce it in a chat line: a server lets only those
/// the line reached fetch it, the sender included.
fn shared(c: &mut Client, pic: &[u8]) -> ChatMedia {
    let m = upload(c, pic, PNG).unwrap_or_else(|e| panic!("{}: {e:?}", c.server().name));
    let marker = unique_name("shared");
    c.send(&chat_with(&marker, &m));
    heard(c, &marker);
    m
}

fn is_picture(bytes: &[u8]) -> bool {
    [
        &b"\x89PNG\r\n\x1a\n"[..],
        b"\xff\xd8\xff",
        b"GIF87a",
        b"GIF89a",
    ]
    .iter()
    .any(|magic| bytes.starts_with(magic))
}

#[test]
fn the_server_says_its_limits() {
    for s in servers_with(&[Cap::InlineMedia]) {
        let l = limits(&member(s, "ml"));
        // Each is optional, but a server that agrees must name some.
        assert!(l.any(), "{}: no limits", s.name);
        // Any it names is plausible.
        for (name, value, floor) in [
            ("max bytes", l.max_bytes, 1024),
            ("max dimension", l.max_dimension, 64),
            ("max pixels", l.max_pixels, 4096),
            ("chunk size", l.chunk_size, 256),
            ("max frames", l.max_frames, 1),
            ("max duration", l.max_duration_ms, 100),
        ] {
            if let Some(v) = value {
                assert!(v >= floor, "{}: {name} {v}", s.name);
            }
        }
    }
}

#[test]
fn a_picture_goes_up_whole() {
    for s in servers_with(&[Cap::InlineMedia]) {
        let mut c = member(s, "mu");
        let m = upload(&mut c, &small_png(), PNG).unwrap_or_else(|e| panic!("{}: {e:?}", s.name));
        assert!(!m.id.is_empty(), "{}: no handle", s.name);
        assert!(m.mime.starts_with(b"image/"), "{}: {m:?}", s.name);
        assert_eq!((m.width, m.height), (Some(4), Some(4)), "{}", s.name);
    }
}

#[test]
fn a_picture_goes_up_in_parts() {
    for s in servers_with(&[Cap::InlineMedia]) {
        let mut c = member(s, "mp");
        let pic = noise_png(part_size(&c) + 8 * 1024);
        let m = upload(&mut c, &pic, PNG).unwrap_or_else(|e| panic!("{}: {e:?}", s.name));
        assert!(!m.id.is_empty(), "{}: no handle", s.name);
        assert!(m.mime.starts_with(b"image/"), "{}: {m:?}", s.name);
    }
}

#[test]
fn a_picture_over_the_limit_is_refused() {
    for s in servers_with(&[Cap::InlineMedia]) {
        let mut c = member(s, "mt");
        let max = limits(&c).max_bytes.map_or(MAX_BYTES, |n| n as usize);
        let pic = noise_png(max + 64 * 1024);
        let got = upload(&mut c, &pic, PNG);
        assert!(got.is_err(), "{}: {} bytes taken", s.name, pic.len());
    }
}

#[test]
fn bytes_that_are_no_picture_are_refused() {
    let svg = br#"<?xml version="1.0"?><svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"><rect width="1" height="1"/></svg>"#;
    for s in servers_with(&[Cap::InlineMedia]) {
        let mut c = member(s, "mg");
        for (what, pic, mime) in [
            ("zeros", &[0u8; 65_500][..], None),
            ("SVG", &svg[..], Some(&b"image/svg+xml"[..])),
        ] {
            let got = upload(&mut c, pic, mime);
            assert!(got.is_err(), "{}: {what} taken: {got:?}", s.name);
        }
    }
}

#[test]
fn a_picture_reaches_others_on_a_chat_line() {
    for s in servers_with(&[Cap::InlineMedia]) {
        let mut a = member(s, "ma");
        let mut b = member(s, "mb");
        let m = upload(&mut a, &small_png(), PNG).unwrap_or_else(|e| panic!("{}: {e:?}", s.name));
        let marker = unique_name("see attached");
        a.send(&chat_with(&marker, &m));
        let got = heard(&mut b, &marker).unwrap_or_else(|| panic!("{}: no picture", s.name));
        assert_eq!((got.id, got.mime), (m.id, m.mime), "{}", s.name);
    }
}

#[test]
fn a_picture_comes_down() {
    for s in servers_with(&[Cap::InlineMedia]) {
        let mut c = member(s, "md");
        let part = part_size(&c);
        // The noise is large enough that the server's own encoding of it
        // still needs more than one part.
        for (pic, in_parts) in [(small_png(), false), (noise_png(part * 3), true)] {
            let m = shared(&mut c, &pic);
            let (bytes, parts) =
                download(&mut c, &m.id).unwrap_or_else(|e| panic!("{}: {e:?}", s.name));
            assert!(
                is_picture(&bytes),
                "{}: {:x?}",
                s.name,
                &bytes[..bytes.len().min(8)]
            );
            if in_parts {
                assert!(parts >= 2, "{}: {} bytes in one part", s.name, bytes.len());
                assert!(bytes.len() >= part, "{}: {} bytes", s.name, bytes.len());
            }
        }
    }
}

#[test]
fn a_picture_never_announced_is_not_given() {
    for s in servers_with(&[Cap::InlineMedia]) {
        let mut c = member(s, "mn");
        let got = download(&mut c, unique_name("no such picture").as_bytes());
        // Expired and not allowed read alike, so handles can't be guessed.
        assert!(
            matches!(
                got,
                Err(MediaErrorCode::Generic | MediaErrorCode::NotAuthorized)
            ),
            "{}: {got:?}",
            s.name
        );
    }
}
