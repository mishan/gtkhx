//! The Reactions draft's requests (capability bit 19), sent only where the
//! session has reactions: Send Reaction, for the user's own, and Get
//! Reactions, for lines the view asks after. What a reply says arrives on
//! the `reactions` signal (`recv::reactions`).
//!
//! A connection's go out in turn at the draft's default rate, a burst and
//! then one a second, which is what a server holds an account's reactions
//! to; each costs spam points too, and a session past them is kicked and
//! banned. Past the rate they wait.

use std::cell::{Cell, RefCell};
use std::collections::{HashMap, VecDeque};
use std::os::raw::{c_int, c_void};
use std::time::{Duration, Instant};

use hxmodel::reactions::same;
use hxproto::reactions::{Key, ADD, CLEAR, REMOVE};
use hxproto::target::Target;
use hxrequest::reactions::{self as req, MAX_TARGETS};
use hxrequest::Request;

use crate::recv::reactions::{asked, limits, Asked};

#[cfg(test)]
use crate::send::friends::tests::{hlwrite_chunks, next_trans, wake};
#[cfg(not(test))]
use crate::send::wake;
#[cfg(not(test))]
use hxtask::send::{hlwrite_chunks, next_trans};

/// The draft's default burst; one a second follows.
const BURST: f64 = 10.0;

/// One connection's requests waiting their turn, and its allowance.
struct Pace {
    tokens: f64,
    at: Instant,
    waiting: VecDeque<(Request, Asked)>,
}

impl Pace {
    /// One more may go at `now`, or the next may at the time given.
    fn take(&mut self, now: Instant) -> Result<(), Instant> {
        let earned = now.saturating_duration_since(self.at).as_secs_f64();
        (self.tokens, self.at) = ((self.tokens + earned).min(BURST), now);
        if self.tokens < 1.0 {
            return Err(now + Duration::from_secs_f64(1.0 - self.tokens));
        }
        self.tokens -= 1.0;
        Ok(())
    }
}

thread_local! {
    static PACE: RefCell<HashMap<usize, Pace>> = RefCell::new(HashMap::new());
    static ASKS: Cell<u64> = const { Cell::new(0) };
}

/// The user's reaction with `key` on `target`, made (`add`) or taken back.
/// Whether the wire can carry it.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub unsafe fn react(htlc: *mut c_void, target: Target, key: Key, add: bool) -> bool {
    ask(htlc, target, None, key, add, false)
}

/// Ask again, before the rest waiting, what the server said to slow down
/// for (`Event::PutOff`). Refused again, it is the user's to hear.
///
/// # Safety
/// See [`react`].
pub unsafe fn retry(htlc: *mut c_void, target: Target, key: Key, add: bool) -> bool {
    ask(htlc, target, None, key, add, true)
}

/// The user's reaction with `key` on `target`, an instant message in the
/// conversation with `with`, made (`add`) or taken back. Whether the wire
/// can carry it.
///
/// # Safety
/// See [`react`].
pub unsafe fn react_im(
    htlc: *mut c_void,
    target: Target,
    with: &[u8],
    key: Key,
    add: bool,
) -> bool {
    ask(htlc, target, Some(with), key, add, false)
}

/// As [`retry`], for [`react_im`].
///
/// # Safety
/// See [`react`].
pub unsafe fn retry_im(
    htlc: *mut c_void,
    target: Target,
    with: &[u8],
    key: Key,
    add: bool,
) -> bool {
    ask(htlc, target, Some(with), key, add, true)
}

/// # Safety
/// See [`react`].
unsafe fn ask(
    htlc: *mut c_void,
    target: Target,
    with: Option<&[u8]>,
    key: Key,
    add: bool,
    again: bool,
) -> bool {
    let action = if add { ADD } else { REMOVE };
    let Some(r) = req::send(&target, action, Some(&key), with) else {
        return false;
    };
    queue(
        htlc,
        r,
        Asked::Send {
            target,
            key,
            add,
            again,
        },
        again,
    );
    true
}

/// A moderator's clearing every reaction on `target`, a line. Whether the
/// wire can carry it.
///
/// # Safety
/// See [`react`].
pub unsafe fn clear(htlc: *mut c_void, target: Target) -> bool {
    clear_on(htlc, target, false)
}

/// A clear, before the rest waiting `again`, as after the server said to
/// slow down.
///
/// # Safety
/// See [`react`].
pub(crate) unsafe fn clear_on(htlc: *mut c_void, target: Target, again: bool) -> bool {
    let Some(r) = req::send(&target, CLEAR, None, None) else {
        return false;
    };
    queue(htlc, r, Asked::Clear { target, again }, again);
    true
}

/// Take back the reaction with `key`, in any spelling the line's model
/// takes for it, on `target` still waiting its turn: whether one was.
///
/// # Safety
/// See [`react`].
pub unsafe fn withdraw(htlc: *mut c_void, target: &Target, key: &Key) -> bool {
    PACE.with_borrow_mut(|p| {
        let Some(p) = p.get_mut(&(htlc as usize)) else {
            return false;
        };
        let n = p.waiting.len();
        p.waiting.retain(
            |(_, a)| !matches!(a, Asked::Send { target: t, key: k, .. } if t == target && same(k, key)),
        );
        p.waiting.len() < n
    })
}

/// Ask after the reactions on `targets` of a kind the server takes, in as
/// many Get Reactions as it takes.
///
/// # Safety
/// See [`react`].
pub unsafe fn refresh(htlc: *mut c_void, targets: &[Target]) {
    let Some(l) = limits(htlc) else {
        return;
    };
    let taken: Vec<Target> = targets.iter().filter(|t| l.takes(t)).cloned().collect();
    for batch in taken.chunks(MAX_TARGETS) {
        get(htlc, batch);
    }
}

/// One Get Reactions after `targets`.
///
/// # Safety
/// See [`react`].
pub(crate) unsafe fn get(htlc: *mut c_void, targets: &[Target]) {
    if let Some(r) = req::get(targets, None) {
        queue(htlc, r, Asked::Get(targets.to_vec()), false);
    }
}

/// Ask who holds `key` on `target`, where the server takes reactions to
/// it: what the answer's `Event::Who` will be known by.
///
/// # Safety
/// See [`react`].
pub unsafe fn who(htlc: *mut c_void, target: Target, key: Key) -> Option<u64> {
    if !limits(htlc)?.takes(&target) {
        return None;
    }
    let r = req::get(std::slice::from_ref(&target), Some(&key))?;
    let ask = ASKS.get() + 1;
    ASKS.set(ask);
    queue(htlc, r, Asked::Who { target, key, ask }, false);
    Some(ask)
}

/// Send `r` in its turn, or before the rest where `first`.
///
/// # Safety
/// See [`react`].
pub(crate) unsafe fn queue(htlc: *mut c_void, r: Request, what: Asked, first: bool) {
    if limits(htlc).is_none() {
        return;
    }
    let now = Instant::now();
    PACE.with_borrow_mut(|p| {
        let p = p.entry(htlc as usize).or_insert_with(|| Pace {
            tokens: BURST,
            at: now,
            waiting: VecDeque::new(),
        });
        match first {
            true => p.waiting.push_front((r, what)),
            false => p.waiting.push_back((r, what)),
        }
    });
    pace(htlc, now);
}

/// The server said to slow down: nothing more goes for a second.
pub(crate) fn slow(htlc: *mut c_void, now: Instant) {
    PACE.with_borrow_mut(|p| {
        if let Some(p) = p.get_mut(&(htlc as usize)) {
            (p.tokens, p.at) = (0.0, now);
        }
    });
}

/// Send what may go at `now`; a timer sends the rest in their turn.
///
/// # Safety
/// See [`react`].
pub(crate) unsafe fn pace(htlc: *mut c_void, now: Instant) {
    loop {
        let next = PACE.with_borrow_mut(|p| {
            let p = p
                .get_mut(&(htlc as usize))
                .filter(|p| !p.waiting.is_empty())?;
            Some(p.take(now).map(|()| p.waiting.pop_front().unwrap()))
        });
        match next {
            None => return,
            Some(Err(at)) => return wake(htlc, at, pace),
            Some(Ok((r, what))) => {
                let trans = next_trans(htlc.cast());
                asked(htlc, trans, what);
                r.with_hx_chunks(|c| {
                    hlwrite_chunks(htlc.cast(), r.opcode, 0, c.as_ptr(), c.len() as c_int)
                });
            }
        }
    }
}

/// Let go of what `htlc` has waiting, and its allowance.
pub(crate) fn forget(htlc: *mut c_void) {
    PACE.with_borrow_mut(|p| p.remove(&(htlc as usize)));
}
