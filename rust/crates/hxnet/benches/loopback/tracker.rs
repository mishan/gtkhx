//! Tracker fetch: a large v3 listing from the network to the main loop.
//!
//! A fake v3 tracker on 127.0.0.1 answers with N servers. The fetch is
//! opened through `hxnet_tracker_fetch_open` and drained the way
//! `network.c` drains it — a 50 ms GLib timeout that polls until the
//! channel is empty. The drain here only counts what it's given; in the
//! app each record becomes a `tracker-server-create` signal, which since
//! the tracker window batches its rows costs little.
//!
//! Like the app's, the fetch tries TLS first; the fake tracker, like most,
//! doesn't speak it, so the first connection fails its handshake and the
//! fetch reconnects in the clear.
//!
//! Reported: the whole fetch, open to the last record; how long after the
//! tracker finished sending the last record reached the main loop; and
//! how many drain ticks that took. Known values: every server arrives, in
//! order, with its name and port intact.

use std::cell::RefCell;
use std::ffi::{c_void, CString};
use std::io::{Read, Write};
use std::net::TcpListener;
use std::time::{Duration, Instant};

use hxnet::ffi::{
    hxnet_tracker_fetch_close, hxnet_tracker_fetch_open, hxnet_tracker_fetch_poll,
    HxnetTrackerEvent, HXNET_TRK_KIND_BEGIN, HXNET_TRK_KIND_RECORD, HXNET_TRK_POLL_CLOSED,
    HXNET_TRK_POLL_EVENT,
};
use hxproto::parse::tracker_v3;

/// The app's drain interval (`network.c`).
const DRAIN_MS: u64 = 50;
/// How long the fake tracker waits for a fetch.
const SERVE_TIMEOUT: Duration = Duration::from_secs(60);
/// `HTRK_V3_FEAT_IPV6`, which the app asks for.
const FEATURES: u16 = 0x0001;

fn record(i: u32) -> Vec<u8> {
    let name = format!("Bench server {i:05}");
    let desc = format!("A server with a description of ordinary length, number {i}");
    let mut r = Vec::with_capacity(16 + name.len() + desc.len());
    r.push(tracker_v3::ADDR_IPV4);
    r.extend_from_slice(&[10, (i >> 16) as u8, (i >> 8) as u8, i as u8]);
    r.extend_from_slice(&port_of(i).to_be_bytes());
    r.extend_from_slice(&((i % 50) as u16).to_be_bytes());
    r.extend_from_slice(&(name.len() as u16).to_be_bytes());
    r.extend_from_slice(name.as_bytes());
    r.extend_from_slice(&(desc.len() as u16).to_be_bytes());
    r.extend_from_slice(desc.as_bytes());
    r.extend_from_slice(&0u16.to_be_bytes()); // no TLVs
    r
}

fn port_of(i: u32) -> u16 {
    5500 + (i % 1000) as u16
}

/// Serve one listing of `n` servers. Connections that don't open with the
/// v3 handshake — the fetch's TLS attempt — are dropped, as a tracker
/// that doesn't speak TLS drops them. Returns when the listing has been
/// written, with the time it was — or `None` if no fetch came for it
/// within `SERVE_TIMEOUT`, so a fetch that gave up can't hang the bench.
fn serve(listener: TcpListener, n: u16) -> Option<Instant> {
    let records: Vec<u8> = (0..u32::from(n)).flat_map(record).collect();
    listener.set_nonblocking(true).expect("nonblocking");
    let give_up = Instant::now() + SERVE_TIMEOUT;
    loop {
        let mut s = match listener.accept() {
            Ok((s, _)) => s,
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                if Instant::now() > give_up {
                    return None;
                }
                std::thread::sleep(Duration::from_millis(2));
                continue;
            }
            Err(e) => panic!("accept: {e}"),
        };
        s.set_nonblocking(false).expect("blocking");
        s.set_nodelay(true).ok();
        let mut hs = [0u8; tracker_v3::HANDSHAKE_LEN];
        if s.read_exact(&mut hs).is_err() || &hs[..4] != b"HTRK" {
            continue;
        }
        let mut reply = Vec::with_capacity(18 + records.len());
        reply.extend_from_slice(b"HTRK");
        reply.extend_from_slice(&3u16.to_be_bytes());
        reply.extend_from_slice(&FEATURES.to_be_bytes());
        s.write_all(&reply).expect("handshake reply");
        let mut req = [0u8; 4];
        s.read_exact(&mut req).expect("listing request");
        let mut listing = Vec::with_capacity(10 + records.len());
        listing.extend_from_slice(&tracker_v3::RESP_LIST.to_be_bytes());
        listing.extend_from_slice(&(records.len() as u32).to_be_bytes());
        listing.extend_from_slice(&n.to_be_bytes());
        listing.extend_from_slice(&n.to_be_bytes());
        listing.extend_from_slice(&records);
        s.write_all(&listing).expect("listing");
        s.flush().ok();
        let done = Instant::now();
        // Hold the socket until the client has read it all.
        let mut sink = [0u8; 64];
        let _ = s.read(&mut sink);
        return Some(done);
    }
}

#[derive(Default)]
struct Drain {
    begun: bool,
    records: u32,
    wrong: u32,
    ticks: u32,
    closed: bool,
    last_at: Option<Instant>,
}

thread_local! {
    static DRAIN: RefCell<Drain> = RefCell::new(Drain::default());
}

unsafe fn slice<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() {
        &[]
    } else {
        std::slice::from_raw_parts(p, n)
    }
}

/// One drain tick, as `network.c`'s `tracker_fetch_drain`: poll until
/// empty or closed.
fn drain_tick(handle: *mut hxnet::ffi::HxnetTrackerFetch) -> glib::ControlFlow {
    DRAIN.with(|d| d.borrow_mut().ticks += 1);
    loop {
        let mut ev = std::mem::MaybeUninit::<HxnetTrackerEvent>::zeroed();
        let rc = unsafe { hxnet_tracker_fetch_poll(handle, ev.as_mut_ptr()) };
        if rc == HXNET_TRK_POLL_EVENT {
            let ev = unsafe { ev.assume_init_ref() };
            DRAIN.with(|d| {
                let mut d = d.borrow_mut();
                match ev.kind {
                    HXNET_TRK_KIND_BEGIN => d.begun = true,
                    HXNET_TRK_KIND_RECORD => {
                        let i = d.records;
                        let name = unsafe { slice(ev.name_ptr, ev.name_len) };
                        if name != format!("Bench server {i:05}").as_bytes()
                            || ev.port != port_of(i)
                        {
                            d.wrong += 1;
                        }
                        d.records += 1;
                        d.last_at = Some(Instant::now());
                    }
                    _ => {}
                }
            });
            continue;
        }
        if rc == HXNET_TRK_POLL_CLOSED {
            DRAIN.with(|d| d.borrow_mut().closed = true);
            return glib::ControlFlow::Break;
        }
        return glib::ControlFlow::Continue;
    }
}

pub struct Fetch {
    pub total: Duration,
    /// From the tracker's last write to the last record on the main loop.
    pub tail: Duration,
    pub ticks: u32,
    pub failures: Vec<String>,
}

pub fn fetch(n: u16) -> Fetch {
    DRAIN.with(|d| *d.borrow_mut() = Drain::default());
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let port = listener.local_addr().expect("port").port();
    let server = std::thread::spawn(move || serve(listener, n));

    let url = CString::new(format!("127.0.0.1:{port}")).expect("url");
    let urls = [url.as_ptr()];
    let t0 = Instant::now();
    let handle = unsafe {
        hxnet_tracker_fetch_open(
            urls.as_ptr(),
            1,
            FEATURES,
            2_000,
            std::ptr::null(),
            None,
            std::ptr::null_mut::<c_void>(),
        )
    };
    assert!(!handle.is_null(), "tracker fetch open");
    let handle_addr = handle as usize;
    let source = glib::timeout_add_local(Duration::from_millis(DRAIN_MS), move || {
        drain_tick(handle_addr as *mut _)
    });
    let ctx = glib::MainContext::default();
    let deadline = Instant::now() + Duration::from_secs(60);
    let mut timed_out = false;
    while !DRAIN.with(|d| d.borrow().closed) {
        if Instant::now() > deadline {
            timed_out = true;
            break;
        }
        ctx.iteration(true);
    }
    if timed_out {
        source.remove();
    }
    unsafe { hxnet_tracker_fetch_close(handle) };
    let sent = server.join().expect("tracker thread");

    let d = DRAIN.with(|d| std::mem::take(&mut *d.borrow_mut()));
    let mut failures = Vec::new();
    if timed_out {
        failures.push("the fetch never finished".into());
    }
    if !d.begun || d.records != u32::from(n) {
        failures.push(format!("{} of {n} servers arrived", d.records));
    }
    if d.wrong > 0 {
        failures.push(format!("{} servers out of order or damaged", d.wrong));
    }
    let Some(sent) = sent else {
        failures.push("the tracker was never asked for its listing".into());
        return Fetch {
            total: Duration::ZERO,
            tail: Duration::ZERO,
            ticks: d.ticks,
            failures,
        };
    };
    let last = d.last_at.unwrap_or(t0);
    Fetch {
        total: last - t0,
        tail: last.saturating_duration_since(sent),
        ticks: d.ticks,
        failures,
    }
}
