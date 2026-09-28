//! How many decodes run at once.
//!
//! Every decode is a D-Bus call to glycin's pooled loader, and each call
//! waiting on its reply looks at every message the connection receives.
//! With N calls in flight that is N² work, all of it on the main thread.
//! Handed a GIF icon for every user at once — which is what a GIF-icons
//! server's ICON_GETLIST reply does at login — a few dozen icons froze the
//! UI for most of a second, a couple of hundred for seconds, and a few
//! hundred never finished decoding at all (docs/performance.md).
//!
//! So decodes take a slot first, a few at a time, in the order they were
//! asked for. The rest wait without costing anything.
//!
//! Main thread only, like every decode future.

use std::cell::{Cell, RefCell};
use std::collections::VecDeque;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Context, Poll, Waker};

/// Decodes in flight at once. A few keep the loader busy while the reply
/// matching stays cheap.
pub(crate) const MAX_IN_FLIGHT: usize = 4;

/// One decode waiting for a slot.
struct Waiter {
    /// Set when a finishing decode hands this one its slot.
    granted: Cell<bool>,
    waker: RefCell<Option<Waker>>,
}

#[derive(Default)]
struct Slots {
    in_flight: usize,
    queue: VecDeque<Rc<Waiter>>,
}

thread_local! {
    static SLOTS: RefCell<Slots> = RefCell::new(Slots::default());
}

/// A held slot. Dropping it passes the slot to the longest waiter, or
/// frees it.
pub(crate) struct Slot(());

impl Drop for Slot {
    fn drop(&mut self) {
        release();
    }
}

fn release() {
    let next = SLOTS.with(|s| {
        let mut s = s.borrow_mut();
        match s.queue.pop_front() {
            Some(w) => Some(w),
            None => {
                s.in_flight -= 1;
                None
            }
        }
    });
    // The slot moves to the waiter without in_flight changing. Wake it
    // outside the borrow: a waker may poll straight away.
    if let Some(w) = next {
        w.granted.set(true);
        if let Some(waker) = w.waker.take() {
            waker.wake();
        }
    }
}

/// Wait for a slot.
pub(crate) fn acquire() -> Acquire {
    Acquire { waiter: None }
}

pub(crate) struct Acquire {
    waiter: Option<Rc<Waiter>>,
}

impl Future for Acquire {
    type Output = Slot;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Slot> {
        if let Some(w) = &self.waiter {
            if w.granted.get() {
                self.waiter = None;
                return Poll::Ready(Slot(()));
            }
            *w.waker.borrow_mut() = Some(cx.waker().clone());
            return Poll::Pending;
        }
        let waiter = SLOTS.with(|s| {
            let mut s = s.borrow_mut();
            if s.in_flight < MAX_IN_FLIGHT {
                s.in_flight += 1;
                return None;
            }
            let w = Rc::new(Waiter {
                granted: Cell::new(false),
                waker: RefCell::new(Some(cx.waker().clone())),
            });
            s.queue.push_back(w.clone());
            Some(w)
        });
        match waiter {
            None => Poll::Ready(Slot(())),
            Some(w) => {
                self.waiter = Some(w);
                Poll::Pending
            }
        }
    }
}

impl Drop for Acquire {
    /// A wait given up: leave the queue, or pass on a slot already handed
    /// over.
    fn drop(&mut self) {
        let Some(w) = self.waiter.take() else {
            return;
        };
        if w.granted.get() {
            release();
        } else {
            SLOTS.with(|s| s.borrow_mut().queue.retain(|q| !Rc::ptr_eq(q, &w)));
        }
    }
}

#[cfg(test)]
fn in_flight() -> usize {
    SLOTS.with(|s| s.borrow().in_flight)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::task::Wake;

    struct Flag(AtomicBool);
    impl Flag {
        fn woken(&self) -> bool {
            self.0.load(Ordering::SeqCst)
        }
    }
    impl Wake for Flag {
        fn wake(self: Arc<Self>) {
            self.0.store(true, Ordering::SeqCst);
        }
    }

    fn waker() -> (Waker, Arc<Flag>) {
        let f = Arc::new(Flag(AtomicBool::new(false)));
        (Waker::from(f.clone()), f)
    }

    fn poll(a: &mut Acquire, w: &Waker) -> Poll<Slot> {
        Pin::new(a).poll(&mut Context::from_waker(w))
    }

    #[test]
    fn slots_are_limited_and_handed_on_in_order() {
        let (w, _) = waker();
        let held: Vec<Slot> = (0..MAX_IN_FLIGHT)
            .map(|_| match poll(&mut acquire(), &w) {
                Poll::Ready(s) => s,
                Poll::Pending => panic!("a free slot was refused"),
            })
            .collect();
        assert_eq!(in_flight(), MAX_IN_FLIGHT);

        let (w1, f1) = waker();
        let (w2, f2) = waker();
        let mut first = acquire();
        let mut second = acquire();
        assert!(poll(&mut first, &w1).is_pending());
        assert!(poll(&mut second, &w2).is_pending());

        let mut held = held.into_iter();
        drop(held.next());
        assert!(f1.woken() && !f2.woken(), "the longest waiter goes first");
        let s1 = match poll(&mut first, &w1) {
            Poll::Ready(s) => s,
            Poll::Pending => panic!("a woken waiter wasn't given its slot"),
        };
        assert!(poll(&mut second, &w2).is_pending());
        assert_eq!(in_flight(), MAX_IN_FLIGHT);

        drop(s1);
        assert!(f2.woken());
        let s2 = match poll(&mut second, &w2) {
            Poll::Ready(s) => s,
            Poll::Pending => panic!("the second waiter wasn't given its slot"),
        };
        drop(s2);
        drop(held);
        assert_eq!(in_flight(), 0);
    }

    #[test]
    fn a_waiter_given_up_frees_its_place() {
        let (w, _) = waker();
        let held: Vec<Slot> = (0..MAX_IN_FLIGHT)
            .map(|_| match poll(&mut acquire(), &w) {
                Poll::Ready(s) => s,
                Poll::Pending => panic!("a free slot was refused"),
            })
            .collect();

        // Given up while still queued.
        let mut queued = acquire();
        assert!(poll(&mut queued, &w).is_pending());
        drop(queued);

        // Given up after a slot was handed to it but before it looked.
        let mut granted = acquire();
        assert!(poll(&mut granted, &w).is_pending());
        let mut held = held.into_iter();
        drop(held.next());
        drop(granted);

        drop(held);
        assert_eq!(in_flight(), 0);
    }
}
