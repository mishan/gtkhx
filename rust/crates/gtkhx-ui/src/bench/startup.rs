//! The Startup scenario: from launch to a usable main window.
//!
//! Three moments, each timed from the process's launch:
//!
//! - **chat panel built** — the bench hook, which runs while the main
//!   window's chat panel is being put together, before the main loop starts
//!   and the rest of the window is laid out;
//! - **first paint** — the chat view's first frame on screen;
//! - **settled** — the first time the main loop has nothing more urgent to
//!   do than a low-priority idle, once that frame is up.
//!
//! Beside them, the main thread's CPU time up to the first paint: where it
//! is well under the wall time, startup is waiting (on disk, D-Bus, the
//! display) rather than working.
//!
//! The launch time comes from `GTKHX_BENCH_T0`, the wall-clock time in µs
//! that `tools/uibench.sh` sets just before it starts the app. Without it,
//! from the kernel's record of when the process started, which is only good
//! to 10 ms.
//!
//! Startup can only be measured once per process, so this scenario always
//! runs first, whatever order `GTKHX_BENCH` lists it in.
//!
//! Checks: the moments come in order, and the launch time was found.

use gtk::glib;
use gtk4 as gtk;

use super::{after_paint, Report};

/// When the bench hook ran, as the time since launch (if known, with where
/// the launch time came from) and the monotonic clock at that moment.
#[derive(Clone, Copy)]
pub(super) struct Hooked {
    since_launch: Option<(i64, &'static str)>,
    at: i64,
}

impl Hooked {
    /// Now. Called first thing in the hook, before any bench work.
    pub(super) fn now() -> Hooked {
        Hooked {
            since_launch: since_launch_us(),
            at: glib::monotonic_time(),
        }
    }
}

/// Microseconds since the process was launched, and where that came from.
fn since_launch_us() -> Option<(i64, &'static str)> {
    if let Some(t0) = std::env::var("GTKHX_BENCH_T0")
        .ok()
        .and_then(|v| v.trim().parse::<i64>().ok())
    {
        return Some((glib::real_time() - t0, "from the launch timestamp"));
    }
    proc_since_start_us().map(|us| (us, "from /proc, to 10 ms"))
}

/// From `/proc`: uptime now, less the process's start time since boot.
/// Both count from boot, in 10 ms steps.
fn proc_since_start_us() -> Option<i64> {
    let uptime: f64 = std::fs::read_to_string("/proc/uptime")
        .ok()?
        .split_whitespace()
        .next()?
        .parse()
        .ok()?;
    let stat = std::fs::read_to_string("/proc/self/stat").ok()?;
    // The command name can hold spaces and parentheses; fields resume after
    // the last ')'. starttime is field 22, the 20th after it.
    let rest = &stat[stat.rfind(')')? + 1..];
    let start_ticks: f64 = rest.split_whitespace().nth(19)?.parse().ok()?;
    // USER_HZ, which Linux fixes at 100 for /proc.
    let start = start_ticks / 100.0;
    Some(((uptime - start) * 1e6) as i64)
}

/// This thread's CPU time so far, in µs (Linux's schedstat).
fn thread_cpu_us() -> Option<i64> {
    std::fs::read_to_string("/proc/thread-self/schedstat")
        .ok()?
        .split_whitespace()
        .next()?
        .parse::<i64>()
        .ok()
        .map(|ns| ns / 1000)
}

/// Resolve once the main loop runs a low-priority source: nothing more
/// urgent is ready.
async fn settled() -> i64 {
    glib::MainContext::default()
        .spawn_local_with_priority(glib::Priority::LOW, async { glib::monotonic_time() })
        .await
        .unwrap_or_else(|_| glib::monotonic_time())
}

pub(super) async fn run(view: &gtk::Widget, hooked: Hooked) {
    let paint = after_paint(view).await;
    let cpu = thread_cpu_us();
    let idle = settled().await;

    // No idle-frame line: measuring it would mean waiting, and startup is
    // over by then.
    let mut r = Report {
        title: "startup".into(),
        lines: Vec::new(),
    };
    let Some((built, source)) = hooked.since_launch else {
        r.line(
            "CHECK FAILED",
            "",
            "no launch time: neither GTKHX_BENCH_T0 nor /proc",
        );
        r.print();
        return;
    };
    let first_paint = built + (paint - hooked.at);
    let settle = built + (idle - hooked.at);
    r.ms("chat panel built", built, source);
    r.ms("first paint", first_paint, "");
    r.ms("settled", settle, "main loop idle");
    if let Some(cpu) = cpu {
        r.ms("  main-thread CPU", cpu, "to first paint");
    }
    if !(0 < built && built <= first_paint && first_paint <= settle) {
        r.line("CHECK FAILED", "", "the moments are out of order");
    }
    r.print();
}
