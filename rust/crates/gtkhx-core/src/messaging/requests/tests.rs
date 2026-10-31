use super::*;

fn sender(login: &str, guids: &[u8]) -> Sender {
    Sender {
        login: login.as_bytes().to_vec(),
        messages: guids
            .iter()
            .map(|&g| Waiting {
                guid: [g; 16],
                body: format!("message {g}"),
                sent_at: u64::from(g),
            })
            .collect(),
    }
}

fn page(cursor: Option<&[u8]>, senders: Vec<Sender>) -> Page {
    Page {
        cursor: cursor.map(<[u8]>::to_vec),
        senders,
        ..Page::default()
    }
}

fn logins(r: &Requests) -> Vec<&[u8]> {
    r.senders.iter().map(|s| s.login.as_slice()).collect()
}

#[test]
fn a_listing_is_gathered_and_replaces_what_was_held_only_at_its_last_page() {
    let t = Instant::now();
    let mut r = Requests::default();
    r.want();
    assert_eq!(r.next(t), Some(None));
    let first = Page {
        takes: Some(true),
        dropped: Some(2),
        ..page(Some(b"1"), vec![sender("alice", &[1, 2])])
    };
    assert!(!r.page(first, t));
    // Nothing is replaced by a page alone.
    assert_eq!((r.count, logins(&r)), (0, vec![]));
    assert_eq!(r.dropped, 2);
    assert_eq!(r.next(t + PAGE_GAP), Some(Some(b"1".to_vec())));
    // An answer between the pages listed alice again, with one more.
    let last = Page {
        approvals: vec![b"carol".to_vec()],
        ..page(None, vec![sender("alice", &[2, 3]), sender("bob", &[4])])
    };
    assert!(r.page(last, t + PAGE_GAP));
    assert_eq!((r.count, logins(&r)), (2, vec![&b"alice"[..], b"bob"]));
    let guids: Vec<u8> = r.senders[0].messages.iter().map(|m| m.guid[0]).collect();
    assert_eq!(guids, [1, 2, 3]);
    assert_eq!(r.approvals, BTreeSet::from([b"carol".to_vec()]));
    assert_eq!(r.takes, Some(true));

    // The next listing replaces it, and adds what it says was dropped.
    r.want();
    assert_eq!(r.next(t + LISTING_GAP), Some(None));
    let again = Page {
        dropped: Some(1),
        ..page(None, vec![sender("dave", &[5])])
    };
    assert!(r.page(again, t + LISTING_GAP));
    assert_eq!((r.count, logins(&r)), (1, vec![&b"dave"[..]]));
    assert_eq!((r.dropped, r.takes), (3, None));
    assert!(r.approvals.is_empty());
}

#[test]
fn a_burst_of_asks_is_one_listing_at_the_earliest_instant_allowed() {
    let t = Instant::now();
    let mut r = Requests::default();
    assert_eq!((r.due(t), r.next(t)), (None, None));
    r.want();
    assert_eq!(r.next(t), Some(None));
    // Asked again while that one is out: owed, and nothing goes until
    // it has ended and the gap has passed.
    r.want();
    assert_eq!((r.due(t), r.next(t)), (None, None));
    assert!(r.page(page(None, vec![]), t));
    let due = t + LISTING_GAP;
    assert_eq!(r.due(t + PAGE_GAP), Some(due));
    for _ in 0..5 {
        r.want();
    }
    assert_eq!(r.due(t + PAGE_GAP), Some(due));
    assert_eq!(r.next(due - PAGE_GAP), None);
    assert_eq!(r.next(due), Some(None));
    assert!(r.page(page(None, vec![]), due));
    // Paid: nothing more is owed.
    assert_eq!(r.due(due + LISTING_GAP), None);
}

#[test]
fn pages_go_a_second_after_the_last_arrived_and_a_cursor_that_never_ends_is_cut_off() {
    // What a page takes to come back.
    let trip = PAGE_GAP / 4;
    let mut r = Requests::default();
    r.want();
    let mut at = Instant::now();
    assert_eq!(r.next(at), Some(None));
    for n in 1..PAGE_BOUND {
        at += trip;
        let p = page(Some(b"again"), vec![sender(&format!("s{n}"), &[])]);
        assert!(!r.page(p, at), "page {n}");
        assert_eq!(r.due(at), Some(at + PAGE_GAP));
        assert_eq!(r.next(at + PAGE_GAP - trip), None);
        at += PAGE_GAP;
        assert_eq!(r.next(at), Some(Some(b"again".to_vec())));
    }
    // The last page the bound allows ends it, cursor or not, and what was
    // gathered stands.
    assert!(r.page(page(Some(b"again"), vec![]), at));
    assert_eq!(r.count, PAGE_BOUND - 1);
    assert_eq!(r.due(at), None);
}

#[test]
fn a_look_stops_at_the_first_page_until_a_listing_is_asked_for() {
    let t = Instant::now();
    let mut r = Requests::default();
    r.look();
    assert_eq!(r.next(t), Some(None));
    assert!(!r.page(page(Some(b"1"), vec![sender("alice", &[1])]), t));
    assert_eq!(r.due(t + LISTING_GAP), None);
    // Asked for, it is read again from the start, as the gap allows.
    r.want();
    assert_eq!(r.next(t + PAGE_GAP), None);
    assert_eq!(r.next(t + LISTING_GAP), Some(None));
    assert!(r.page(page(None, vec![sender("bob", &[2])]), t + LISTING_GAP));
    assert_eq!(logins(&r), [&b"bob"[..]]);
}

#[test]
fn a_change_while_a_listing_is_read_owes_another_and_keeps_its_count() {
    let t = Instant::now();
    let mut r = Requests::default();
    r.want();
    assert_eq!(r.next(t), Some(None));
    assert!(!r.page(page(Some(b"1"), vec![sender("alice", &[1])]), t));
    r.changed(1);
    assert_eq!(r.next(t + PAGE_GAP), Some(Some(b"1".to_vec())));
    assert!(r.page(page(None, vec![sender("bob", &[2])]), t + PAGE_GAP));
    assert_eq!(r.count, 1);
    assert_eq!(r.due(t + PAGE_GAP), Some(t + LISTING_GAP));

    // With none being read, the count is all it moves.
    assert_eq!(r.next(t + LISTING_GAP), Some(None));
    assert!(r.page(page(None, vec![]), t + LISTING_GAP));
    r.changed(4);
    assert_eq!((r.count, r.due(t + LISTING_GAP)), (4, None));
}

#[test]
fn a_login_listed_as_a_sender_and_an_approval_is_what_the_later_page_says() {
    let t = Instant::now();
    let approval = |login: &str, cursor| Page {
        approvals: vec![login.as_bytes().to_vec()],
        ..page(cursor, vec![])
    };
    // Approved between the pages, and then the reverse.
    for (first, second, sender_after) in [
        (
            page(Some(b"1"), vec![sender("alice", &[1])]),
            approval("alice", None),
            false,
        ),
        (
            approval("alice", Some(b"1")),
            page(None, vec![sender("alice", &[1])]),
            true,
        ),
    ] {
        let mut r = Requests::default();
        r.want();
        r.next(t);
        r.page(first, t);
        r.next(t + PAGE_GAP);
        assert!(r.page(second, t + PAGE_GAP));
        let approved = r.approvals.contains(&b"alice"[..]);
        assert_eq!(
            (r.count, approved),
            (u32::from(sender_after), !sender_after)
        );
    }
}

#[test]
fn a_failed_listing_leaves_the_one_held() {
    let t = Instant::now();
    let mut r = Requests::default();
    r.want();
    r.next(t);
    r.page(page(None, vec![sender("alice", &[1])]), t);
    r.want();
    r.next(t + LISTING_GAP);
    r.page(page(Some(b"1"), vec![sender("bob", &[2])]), t + LISTING_GAP);
    r.failed();
    assert_eq!((r.count, logins(&r)), (1, vec![&b"alice"[..]]));
    // A page nobody asked for any more is nobody's.
    assert!(!r.page(page(None, vec![sender("eve", &[3])]), t + LISTING_GAP));
    assert_eq!(logins(&r), [&b"alice"[..]]);
}
