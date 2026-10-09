//! Voice and video's requests as production sends them (`hxrequest::voice`),
//! each reply expected by the session as `hxhandlers::voice` expects it: a
//! join answered with the server's offer, the rest with `VoiceDone` once they
//! worked, and a refusal as `Failed` with the server's reason. No answer
//! goes back to the offer, so no media flows.
#![cfg(feature = "rig")]

use hx_e2e::{servers_with, unique_name, Cap, Client, Server};
use hxnet::Event;
use hxproto::video::VideoKind;
use hxrequest::voice;
use hxsession::{Expect, Handled};

/// `HTLC_CAP_VOICE` and `HTLC_CAP_VIDEO`.
const CAP_VOICE: u16 = 0x0004;
const CAP_VIDEO: u16 = 0x0400;

/// A guest that agreed to voice and video, going by a name no other test
/// uses.
fn member(server: &'static Server) -> Client {
    let nick: String = unique_name("voice").chars().take(31).collect();
    let c = Client::login_handling(
        server,
        "",
        None,
        CAP_VOICE | CAP_VIDEO,
        Handled::NONE,
        &nick,
    )
    .unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(
        c.caps() & (CAP_VOICE | CAP_VIDEO),
        CAP_VOICE | CAP_VIDEO,
        "{}: voice and video not agreed",
        server.name
    );
    c
}

/// How each request on `ts` was answered: it went through, or was refused
/// with the server's reason.
fn outcomes(c: &mut Client, ts: &[u32]) -> Vec<Result<(), Option<String>>> {
    use hxsession::Event as S;
    let mut got = std::collections::HashMap::new();
    while !ts.iter().all(|t| got.contains_key(t)) {
        match c.next_event().unwrap_or_else(|e| panic!("{e}")) {
            Event::Session(S::VoiceDone { trans } | S::VoiceJoined { trans, .. }) => {
                got.insert(trans, Ok(()));
            }
            Event::Session(S::Failed { trans, reason }) => {
                got.insert(trans, Err(reason));
            }
            Event::Frame(f) if ts.iter().any(|&t| hx_e2e::is_reply(&f, t)) => {
                panic!("{}: an expected reply came whole", c.server().name)
            }
            _ => {}
        }
    }
    ts.iter().map(|t| got.remove(t).unwrap()).collect()
}

#[test]
fn a_join_is_answered_with_the_offer_and_the_room() {
    for s in servers_with(&[Cap::Voice]) {
        let mut c = member(s);
        let t = c.send_expecting(&voice::join(0).unwrap(), Some(Expect::VoiceJoin { cid: 0 }));
        let (sdp, codec, participants) = loop {
            match c.next_event().unwrap_or_else(|e| panic!("{e}")) {
                Event::Session(hxsession::Event::VoiceJoined {
                    trans,
                    cid,
                    sdp,
                    codec,
                    participants,
                }) if trans == t => {
                    assert_eq!(cid, 0, "{}", s.name);
                    break (sdp, codec, participants);
                }
                Event::Session(hxsession::Event::Failed { trans, reason }) if trans == t => {
                    panic!("{}: the join was refused: {reason:?}", s.name)
                }
                _ => {}
            }
        };
        let sum = hxproto::voice::sdp::summarize(&sdp);
        assert!(sum.has_pcmu, "{}: an offer without PCMU", s.name);
        assert_eq!(codec, b"PCMU", "{}", s.name);
        assert_eq!(participants.len() % 6, 0, "{}: {participants:?}", s.name);
        let left = c.send_expecting(&voice::leave(0).unwrap(), Some(Expect::Voice));
        assert_eq!(outcomes(&mut c, &[left]), [Ok(())], "{}", s.name);
    }
}

#[test]
fn muting_and_video_work_and_a_room_not_joined_is_refused() {
    for s in servers_with(&[Cap::Voice, Cap::Video]) {
        let mut c = member(s);
        let join = Expect::VoiceJoin { cid: 0 };
        let joined = c.send_expecting(&voice::join(0).unwrap(), Some(join));
        assert_eq!(outcomes(&mut c, &[joined]), [Ok(())], "{}", s.name);
        let worked: Vec<u32> = [
            voice::mute(0, true),
            voice::mute(0, false),
            voice::video_start(0, VideoKind::Camera),
            voice::video_state(0, VideoKind::Camera, true),
            voice::video_subscribe(0, &[]),
            voice::video_stop(0, None),
            voice::leave(0),
        ]
        .into_iter()
        .map(|r| c.send_expecting(&r.unwrap(), Some(Expect::Voice)))
        .collect();
        assert_eq!(
            outcomes(&mut c, &worked),
            vec![Ok(()); worked.len()],
            "{}",
            s.name
        );
        // Video in a room the user is not in voice in, and voice in a chat
        // that isn't theirs.
        let refused = [
            c.send_expecting(
                &voice::video_start(7, VideoKind::Screen).unwrap(),
                Some(Expect::Voice),
            ),
            c.send_expecting(
                &voice::join(99).unwrap(),
                Some(Expect::VoiceJoin { cid: 99 }),
            ),
        ];
        for (t, r) in refused.iter().zip(outcomes(&mut c, &refused)) {
            match r {
                Err(Some(why)) if !why.is_empty() => eprintln!("{}: {t} refused: {why}", s.name),
                r => panic!("{}: {t} not refused with a reason: {r:?}", s.name),
            }
        }
    }
}
