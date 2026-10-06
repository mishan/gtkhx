//! The voice and video requests, pinned against hand-written wire bytes.

use super::*;

#[test]
fn each_request_goes_as_gtkhx_has_always_sent_it() {
    let req = |opcode: u32, chunks: &[(u16, &[u8])]| Request {
        opcode,
        chunks: chunks.iter().map(|(t, d)| (*t, d.to_vec())).collect(),
    };
    let room = |cid: u8| (0x0072, [0, 0, 0, cid]);
    let (r42, r7, r3, r9, r4) = (room(42), room(7), room(3), room(9), room(4));
    let sdp = b"v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\n";
    // uid 5 camera, uid 6 screen audio (reserved), uid 7 screen.
    let subs = [0u8, 5, 0, 1, 0, 6, 0, 3, 0, 7, 0, 2];
    for (got, want) in [
        (join(42), req(600, &[(r42.0, &r42.1)])),
        (leave(7), req(601, &[(r7.0, &r7.1)])),
        (
            sdp_answer(3, sdp),
            req(603, &[(r3.0, &r3.1), (0x01f5, sdp)]),
        ),
        (ice(9, b"{}"), req(604, &[(r9.0, &r9.1), (0x01f6, b"{}")])),
        // The end of our candidates: the field, empty.
        (ice(9, b""), req(604, &[(r9.0, &r9.1), (0x01f6, b"")])),
        (mute(4, true), req(606, &[(r4.0, &r4.1), (0x01f8, &[0, 1])])),
        (
            mute(4, false),
            req(606, &[(r4.0, &r4.1), (0x01f8, &[0, 0])]),
        ),
        (
            video_start(9, VideoKind::Screen),
            req(607, &[(r9.0, &r9.1), (0x0220, &[0, 2])]),
        ),
        (
            video_start(9, VideoKind::Camera),
            req(607, &[(r9.0, &r9.1), (0x0220, &[0, 1])]),
        ),
        // No kind stops everything.
        (video_stop(4, None), req(608, &[(r4.0, &r4.1)])),
        (
            video_stop(4, Some(VideoKind::Camera)),
            req(608, &[(r4.0, &r4.1), (0x0220, &[0, 1])]),
        ),
        (
            video_state(4, VideoKind::Camera, true),
            req(609, &[(r4.0, &r4.1), (0x0220, &[0, 1]), (0x0221, &[0, 1])]),
        ),
        (
            video_state(4, VideoKind::Screen, false),
            req(609, &[(r4.0, &r4.1), (0x0220, &[0, 2]), (0x0221, &[0, 0])]),
        ),
        (
            video_subscribe(3, &subs),
            req(610, &[(r3.0, &r3.1), (0x0225, &[0, 5, 0, 1, 0, 7, 0, 2])]),
        ),
        (
            video_subscribe(3, &[]),
            req(610, &[(r3.0, &r3.1), (0x0225, b"")]),
        ),
    ] {
        assert_eq!(got, Some(want));
    }
    assert_eq!(sdp_answer(3, b""), None);
}
