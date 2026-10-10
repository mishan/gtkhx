//! News as production receives it: the session handles what is added to
//! flat news (`Handled::NEWS`) and hands it on among the frames, and the
//! replies to news requests come back as the session's, expected as
//! `hxhandlers` expects them. The requests are `hxrequest::news`'s, which
//! production sends. Only what a test posts itself is checked: the
//! long-lived rig's seed news drifts.
#![cfg(feature = "rig")]

use hx_e2e::client::CAP_TEXT_ENCODING;
use hx_e2e::{servers_with, unique_name, Client, Server};
use hxnet::Event;
use hxrequest::{news, Request};
use hxsession::{Article, Expect, Handled, NewsItem};

/// A session that handles what the app's does, logged in as `login` (the
/// guest, or the server's admin), going by a name no other test uses.
fn member(server: &'static Server, login: &str, who: &str) -> (Client, String) {
    // Inside every server's 31-byte cap.
    let nick: String = unique_name(who).chars().take(31).collect();
    let c = Client::login_handling(
        server,
        login,
        None,
        CAP_TEXT_ENCODING,
        Handled::CHAT | Handled::USERS | Handled::MSG | Handled::NEWS,
        &nick,
    )
    .unwrap_or_else(|e| panic!("{e}"));
    (c, nick)
}

/// What `c` is sent until `want` makes something of it; flat news's frame
/// arriving whole fails the test.
fn until<T>(c: &mut Client, mut want: impl FnMut(&hxsession::Event) -> Option<T>) -> T {
    loop {
        match c.next_event().unwrap_or_else(|e| panic!("{e}")) {
            Event::Session(e) => {
                if let Some(t) = want(&e) {
                    return t;
                }
            }
            Event::Frame(f) if f.header.type_ == 0x66 => {
                panic!("{}: flat news came whole", c.server().name)
            }
            _ => {}
        }
    }
}

/// Send `req`, which says nothing once it worked, and the server's reason if
/// it refused: the user list asked for after it is answered after it, so
/// nothing by then means it worked.
fn change(c: &mut Client, req: &Request) -> Option<String> {
    let t = c.send_expecting(req, Some(Expect::NewsChange));
    let list = c.send_expecting(&user_list(), Some(Expect::UserList));
    until(c, |e| match e {
        hxsession::Event::Failed { trans, reason } if *trans == t => {
            Some(Some(reason.clone().unwrap_or_default()))
        }
        hxsession::Event::UserList { trans, .. } if *trans == list => Some(None),
        _ => None,
    })
}

fn user_list() -> Request {
    Request {
        opcode: 300,
        chunks: vec![],
    }
}

fn listing(c: &mut Client, path: &[u8]) -> Vec<NewsItem> {
    let t = c.send_expecting(&news::listing(path).unwrap(), Some(Expect::NewsListing));
    until(c, |e| match e {
        hxsession::Event::NewsListing { trans, items } if *trans == t => Some(items.clone()),
        _ => None,
    })
}

fn articles(c: &mut Client, path: &[u8]) -> Vec<Article> {
    let t = c.send_expecting(&news::category(path).unwrap(), Some(Expect::NewsCategory));
    until(c, |e| match e {
        hxsession::Event::NewsCategory { trans, articles } if *trans == t => Some(articles.clone()),
        _ => None,
    })
}

#[test]
fn a_flat_news_post_arrives_as_an_event_and_is_in_the_file() {
    for s in servers_with(&[]) {
        let (mut a, na) = member(s, s.admin, "na");
        let (mut b, _) = member(s, "", "nb");
        // As the app does once logged in. Its answer also says the server
        // is done with b's agree, which a post that comes first can miss.
        let t = b.send_expecting(&user_list(), Some(Expect::UserList));
        until(&mut b, |e| {
            matches!(e, hxsession::Event::UserList { trans, .. } if *trans == t).then_some(())
        });
        let text = format!("{na} posted, café");
        let post = news::post(text.as_bytes(), a.utf8()).unwrap();
        assert_eq!(change(&mut a, &post), None, "{}", s.name);
        until(&mut b, |e| match e {
            hxsession::Event::NewsPosted(t) if t.contains(&text) => Some(()),
            _ => None,
        });
        let t = b.send_expecting(&news::file(), Some(Expect::NewsFile));
        let file = until(&mut b, |e| match e {
            hxsession::Event::NewsFile { trans, text } if *trans == t => Some(text.clone()),
            _ => None,
        });
        assert!(file.contains(&text), "{}: {text:?} not in the file", s.name);
    }
}

/// A category is made, posted to, read back and deleted, named back by the
/// bytes its listing gave, as the browser names it: Mac Roman ones where
/// the connection is not UTF-8 (mhxd, hlservd), which a path decoded and
/// encoded again would not name.
#[test]
fn threaded_news_is_made_read_and_deleted_through_its_replies() {
    for s in servers_with(&[]) {
        let (mut a, na) = member(s, s.admin, "ta");
        let utf8 = a.utf8();
        let cat = format!("{na} café");
        let made = news::create_category(b"/", cat.as_bytes(), utf8).unwrap();
        assert_eq!(change(&mut a, &made), None, "{}", s.name);
        let items = listing(&mut a, b"/");
        let item = items
            .iter()
            .find(|i| i.name == cat && !i.bundle)
            .unwrap_or_else(|| panic!("{}: {cat:?} not in {items:?}", s.name));
        // Where the connection is not UTF-8, the name came as Mac Roman.
        assert_eq!(item.name_bytes == cat.as_bytes(), utf8, "{}", s.name);
        let path = [&b"/"[..], &item.name_bytes].concat();

        // Short: hlservd cuts a subject at 31 bytes. The category is the
        // test's own, so it need not name the test.
        let subject = "Café".to_string();
        let post = news::post_article(
            &path,
            0,
            subject.as_bytes(),
            "one\ntwo, café".as_bytes(),
            utf8,
        )
        .unwrap();
        assert_eq!(change(&mut a, &post), None, "{}", s.name);
        let listed = articles(&mut a, &path);
        let first = listed
            .iter()
            .find(|x| x.subject == subject)
            .unwrap_or_else(|| panic!("{}: {subject:?} not in {listed:?}", s.name));
        assert_eq!(first.mime, b"text/plain", "{}", s.name);

        let t = a.send_expecting(
            &news::article(&path, first.id, &first.mime).unwrap(),
            Some(Expect::NewsArticle),
        );
        let text = until(&mut a, |e| match e {
            hxsession::Event::NewsArticle { trans, text } if *trans == t => Some(text.clone()),
            _ => None,
        });
        // mhxd ends it with a line break of its own.
        assert_eq!(text.trim_end(), "one\ntwo, café", "{}", s.name);

        let gone = news::delete_article(&path, first.id).unwrap();
        assert_eq!(change(&mut a, &gone), None, "{}", s.name);
        assert!(articles(&mut a, &path).is_empty(), "{}", s.name);
        assert_eq!(
            change(&mut a, &news::delete(&path).unwrap()),
            None,
            "{}",
            s.name
        );
        assert!(
            listing(&mut a, b"/").iter().all(|i| i.name != cat),
            "{}: {cat:?} is still there",
            s.name
        );
    }
}

/// A refusal comes back as the session's, with a reason the view can show:
/// a guest's deletion, and a listing of a category that is not there.
#[test]
fn a_refusal_comes_with_a_reason() {
    for s in servers_with(&[]) {
        let (mut a, na) = member(s, "", "tm");
        let nowhere = format!("/{na} nowhere");
        let reason = change(&mut a, &news::delete(nowhere.as_bytes()).unwrap());
        assert!(reason.is_some_and(|r| !r.is_empty()), "{}", s.name);
        let t = a.send_expecting(
            &news::category(nowhere.as_bytes()).unwrap(),
            Some(Expect::NewsCategory),
        );
        let got = until(&mut a, |e| match e {
            hxsession::Event::Failed { trans, .. }
            | hxsession::Event::NewsCategory { trans, .. }
                if *trans == t =>
            {
                Some(e.clone())
            }
            _ => None,
        });
        assert!(
            matches!(&got, hxsession::Event::Failed { reason: Some(r), .. } if !r.is_empty()),
            "{}: {got:?}",
            s.name
        );
    }
}
