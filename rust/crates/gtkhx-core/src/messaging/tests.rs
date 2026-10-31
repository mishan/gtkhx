use super::*;

fn friend(login: &str, state: RosterState) -> Friend {
    Friend {
        login: login.as_bytes().to_vec(),
        nickname: None,
        state: Some(state),
        presence: PresenceState::Online,
        status: None,
        name: None,
        note: None,
    }
}

#[test]
fn an_entry_replaces_what_is_held_and_removed_ends_it() {
    let mut r = Roster::default();
    r.replace([
        friend("alice", RosterState::Accepted),
        friend("bob", RosterState::PendingOut),
    ]);
    let mut alias = friend("alice", RosterState::Accepted);
    alias.nickname = Some("Al".into());
    alias.presence = PresenceState::Away;
    r.entry(alias.clone());
    assert_eq!(r.get(b"alice"), Some(&alias));
    r.entry(friend("bob", RosterState::Removed));
    assert_eq!(r.get(b"bob"), None);
    r.replace([friend("carol", RosterState::Blocked)]);
    let held: Vec<_> = r.entries().map(|f| (f.login.as_slice(), f.state)).collect();
    assert_eq!(held, [(&b"carol"[..], Some(RosterState::Blocked))]);
}

#[test]
fn a_presence_change_moves_the_presence_alone_and_only_for_an_entry() {
    let mut r = Roster::default();
    let mut alice = friend("alice", RosterState::Accepted);
    alice.nickname = Some("Al".into());
    r.entry(alice);
    let mut seen = friend("alice", RosterState::Removed);
    seen.state = None;
    seen.presence = PresenceState::Busy;
    seen.status = Some("in a meeting".into());
    assert!(r.presence(&seen));
    assert!(!r.presence(&friend("stranger", RosterState::Accepted)));
    let a = r.get(b"alice").unwrap();
    assert_eq!(
        (
            a.nickname.as_deref(),
            a.state,
            a.presence,
            a.status.as_deref()
        ),
        (
            Some("Al"),
            Some(RosterState::Accepted),
            PresenceState::Busy,
            Some("in a meeting")
        )
    );
    assert_eq!(r.get(b"stranger"), None);
}

#[test]
fn a_request_is_held_as_pending_in() {
    let mut r = Roster::default();
    let mut asks = friend("dave", RosterState::Accepted);
    asks.state = None;
    asks.note = Some("hi".into());
    r.request(asks);
    let dave = r.get(b"dave").unwrap();
    assert_eq!(
        (dave.state, dave.note.as_deref()),
        (Some(RosterState::PendingIn), Some("hi"))
    );
}

#[test]
fn a_message_only_moves_forward_and_only_by_its_recipient() {
    use Delivery::*;
    let mut r = Roster::default();
    let g = [1; GUID_LEN];
    r.sending(g, b"bob");
    for (from, now, moved) in [
        (None, Sent, true),
        // Someone else's receipt for it is nobody's.
        (Some(&b"eve"[..]), Read, false),
        (Some(&b"bob"[..]), Read, true),
        // Read goes no further: a late Delivered never takes it back.
        (Some(&b"bob"[..]), Delivered, false),
    ] {
        let got = r.delivered(&g, from, now);
        assert_eq!(got.is_some(), moved, "{from:?} {now:?}");
    }
    // Read, it is let go of.
    assert_eq!(r.delivery(&g), None);
    assert_eq!(r.delivered(&[2; GUID_LEN], None, Sent), None);
}

#[test]
fn a_closing_connection_fails_only_what_was_never_answered() {
    let htlc = 0x5150 as *const c_void;
    let [unanswered, taken, failed] = [[1; GUID_LEN], [2; GUID_LEN], [3; GUID_LEN]];
    with_roster_mut(htlc, |r| {
        for g in [unanswered, taken, failed] {
            r.sending(g, b"bob");
        }
        r.delivered(&taken, None, Delivery::Queued);
        r.delivered(&failed, None, Delivery::Failed);
    });
    assert_eq!(forget(htlc), [(unanswered, b"bob".to_vec())]);
    assert_eq!(forget(htlc), []);
}

#[test]
fn what_a_sends_reason_and_a_receipt_say() {
    for (got, want) in [
        (Delivery::sent(Some(ReasonCode::Ok)), Delivery::Sent),
        (Delivery::sent(None), Delivery::Sent),
        (
            Delivery::sent(Some(ReasonCode::OfflineQueued)),
            Delivery::Queued,
        ),
        (Delivery::acked(AckType::Delivered), Delivery::Delivered),
        (Delivery::acked(AckType::Read), Delivery::Read),
    ] {
        assert_eq!(got, want);
    }
}

#[test]
fn our_presence_keeps_its_status_unless_given_one() {
    let mut r = Roster::default();
    r.set_own_presence(PresenceState::Away, Some("lunch".into()));
    r.set_own_presence(PresenceState::Online, None);
    assert_eq!(
        r.own_presence(),
        Some((PresenceState::Online, Some("lunch")))
    );
    // Only whitespace clears it, as the server takes it.
    r.set_own_presence(PresenceState::Online, Some("  ".into()));
    assert_eq!(r.own_presence(), Some((PresenceState::Online, None)));
}
