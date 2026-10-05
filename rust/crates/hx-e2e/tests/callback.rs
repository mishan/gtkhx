//! The connection the app opens: `hxnet`'s callback entry point, which runs
//! the session with chat and users handled and hands its events to
//! `on_session` on the GLib main loop, as `hxnet_bridge.c` receives them.
//! What the bridge then does with an event is `hxhandlers`', tested on its
//! own; this suite stops at the callback, which is as far as a binary
//! without the app's link goes.
#![cfg(feature = "rig")]

use std::cell::{Cell, RefCell};
use std::ffi::c_void;
use std::time::{Duration, Instant};

use hx_e2e::client::CAP_TEXT_ENCODING;
use hx_e2e::{servers_with, unique_name, Cap, Client, Server};
use hxnet::ffi::{
    connection_expect, hxnet_connection_agree, hxnet_connection_destroy,
    hxnet_connection_open_plaintext, hxnet_connection_send_frame, hxnet_connection_take_trans,
    hxnet_frame_free, HxnetConnection, HxnetFrame,
};
use hxnet::ConnectionState;
use hxproto::messages::tag;
use hxrequest::Request;
use hxsession::{Event, Expect};

const CAP_CHAT_HISTORY: u16 = 0x0010;
const WAIT: Duration = Duration::from_secs(15);

thread_local! {
    static READY: Cell<bool> = const { Cell::new(false) };
    static HEARD: RefCell<Vec<Event>> = const { RefCell::new(Vec::new()) };
}

unsafe extern "C" fn on_event(c: *mut HxnetConnection, f: *mut HxnetFrame, _u: *mut c_void) {
    // An agreement is agreed to, as a user would.
    if (*f).type_ == 0x6d {
        hxnet_connection_agree(c, b"cb".as_ptr(), 2, 414);
    }
    hxnet_frame_free(f);
}

unsafe extern "C" fn on_shutdown(_c: *mut HxnetConnection, _why: i32, _u: *mut c_void) {}

unsafe extern "C" fn on_state(_c: *mut HxnetConnection, state: u32, _u: *mut c_void) {
    if state == ConnectionState::LoginReady as u32 {
        READY.with(|r| r.set(true));
    }
}

unsafe extern "C" fn on_session(_c: *mut HxnetConnection, ev: *const c_void, _u: *mut c_void) {
    let ev = (*(ev as *const Event)).clone();
    HEARD.with(|h| h.borrow_mut().push(ev));
}

/// Run the main loop until `done` finds what it wants in what was heard.
fn until<T>(ctx: &glib::MainContext, what: &str, done: impl Fn(&Event) -> Option<T>) -> T {
    let deadline = Instant::now() + WAIT;
    let tick = glib::timeout_add_local(Duration::from_millis(100), || glib::ControlFlow::Continue);
    loop {
        if let Some(t) = HEARD.with(|h| h.borrow().iter().find_map(&done)) {
            tick.remove();
            return t;
        }
        assert!(Instant::now() < deadline, "no {what} within {WAIT:?}");
        ctx.iteration(true);
    }
}

unsafe fn send(h: *mut HxnetConnection, req: &Request, expect: Option<Expect>) -> u32 {
    let trans = hxnet_connection_take_trans(h);
    if let Some(what) = expect {
        connection_expect(h, trans, what);
    }
    let frame = req.pack(trans);
    assert_eq!(
        hxnet_connection_send_frame(h, frame.as_ptr(), frame.len() as u32),
        0
    );
    trans
}

fn request(opcode: u32, chunks: &[(u16, &[u8])]) -> Request {
    Request {
        opcode,
        chunks: chunks.iter().map(|(t, d)| (*t, d.to_vec())).collect(),
    }
}

fn session_events_through_the_callback(s: &'static Server) {
    let ctx = glib::MainContext::new();
    ctx.with_thread_default(|| {
        READY.with(|r| r.set(false));
        HEARD.with(|h| h.borrow_mut().clear());
        let nick = unique_name("cb");
        // SAFETY: every pointer handed over is valid for the call, and the
        // callbacks for the connection's life.
        let h = unsafe {
            hxnet_connection_open_plaintext(
                s.host.as_ptr(),
                s.host.len(),
                s.port,
                std::ptr::null(),
                0,
                std::ptr::null(),
                0,
                nick.as_ptr(),
                nick.len(),
                414,
                hxnet::login::CLIENT_VERSION,
                CAP_TEXT_ENCODING | CAP_CHAT_HISTORY,
                1,
                std::ptr::null(),
                0,
                Some(on_event),
                Some(on_shutdown),
                Some(on_state),
                Some(on_session),
                std::ptr::null_mut(),
            )
        };
        assert!(!h.is_null(), "{}: no connection", s.name);
        let deadline = Instant::now() + WAIT;
        while !READY.with(|r| r.get()) {
            assert!(
                Instant::now() < deadline,
                "{}: the login never settled",
                s.name
            );
            ctx.iteration(true);
        }
        // The user list, as the app asks for it: hlservd sends no chat
        // before.
        let t = unsafe { send(h, &request(300, &[]), Some(Expect::UserList)) };
        let users = until(&ctx, "the user list", |e| match e {
            Event::UserList { trans, users, .. } if *trans == t => Some(users.clone()),
            _ => None,
        });
        assert!(!users.is_empty(), "{}: not even us in the list", s.name);

        let mut other = Client::guest(s, CAP_TEXT_ENCODING);
        let line = format!("{nick} through the callback");
        other.send(&request(105, &[(tag::BODY, line.as_bytes())]));
        until(&ctx, "the chat line", |e| match e {
            Event::Chat { text, .. } if text.contains(&line) => Some(()),
            _ => None,
        });

        // History, where the server keeps it; where it does not, the
        // server refuses the request, and with a reason the view can show.
        // Either way it comes back as the session's, not as a frame.
        let limit = 50u16.to_be_bytes();
        let ask = request(
            700,
            &[
                (tag::CHANNEL_ID, &[0, 0, 0, 0]),
                (tag::HISTORY_LIMIT, &limit),
            ],
        );
        let t = unsafe { send(h, &ask, Some(Expect::ChatHistory { cid: 0 })) };
        let answer = until(&ctx, "the history request's answer", |e| match e {
            Event::ChatHistory { trans, .. } | Event::Failed { trans, .. } if *trans == t => {
                Some(e.clone())
            }
            _ => None,
        });
        match answer {
            Event::ChatHistory { entries, .. } if s.has(Cap::ChatHistory) => assert!(
                entries.iter().any(|e| e.text.contains(&line)),
                "{}: {line:?} not in the history",
                s.name
            ),
            Event::Failed {
                reason: Some(_), ..
            } if !s.has(Cap::ChatHistory) => {}
            other => panic!("{}: {other:?}", s.name),
        }
        unsafe { hxnet_connection_destroy(h) };
    })
    .expect("a new context is this thread's to own");
}

#[test]
fn the_app_s_connection_hands_chat_and_users_to_on_session() {
    for s in servers_with(&[]) {
        session_events_through_the_callback(s);
    }
}
