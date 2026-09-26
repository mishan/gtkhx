//! In-app UI benchmarks: scenarios timed through the real frame clock.
//!
//! These measure what a user feels — layout, paint and GTK's own
//! compositing together — in the real widgets, in the running app. That
//! is why they live here rather than in `cargo bench`: the widgets they
//! drive link against C that only the full binary has. The CPU-only
//! benchmarks are the criterion benches in the crates themselves; see
//! docs/performance.md for how the tiers divide.
//!
//! ```sh
//! GTKHX_BENCH=chat=20000,files=10000 GTKHX_BENCH_QUIT=1 ./build/src/gtkhx
//! ```
//!
//! `GTKHX_BENCH` lists scenarios to run in order, each with an optional
//! size. `GTKHX_BENCH_QUIT` exits once the last report is printed, which
//! is what makes a run scriptable; `tools/uibench.sh` does the repeats.
//!
//! **Every report leads with the idle frame interval**, measured before
//! the scenario does anything. It is the known value these numbers are
//! checked against: on a real display it is the refresh interval (16.7 ms
//! at 60 Hz), and no frame can be shorter. A phase that reports exactly
//! one idle interval did no measurable work — or the benchmark failed to
//! trigger the work it claims to time, which is how both of the failures
//! recorded in docs/chat-view-benchmark.md looked.
//!
//! Frame timings include the compositor and depend on the display, the
//! window size and the theme. Compare runs on one machine only, and read
//! the spread across repeats rather than a single run.

mod chat;
mod files;

use std::cell::RefCell;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Context, Poll, Waker};

use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;

/// Frames to let settle before timing anything. The first frames after a
/// window maps are dominated by one-off GTK work (CSS, the font map, the
/// GL context) that belongs to no scenario.
const WARMUP_FRAMES: usize = 20;

/// One requested scenario and its size.
#[derive(Debug, Clone, PartialEq, Eq)]
struct Request {
    name: String,
    size: Option<u32>,
}

/// Parse `GTKHX_BENCH`: `name[=size]` items separated by commas.
fn parse_requests(spec: &str) -> Result<Vec<Request>, String> {
    let mut out = Vec::new();
    for item in spec.split(',').map(str::trim).filter(|s| !s.is_empty()) {
        let (name, size) = match item.split_once('=') {
            Some((n, s)) => {
                let size = s
                    .trim()
                    .parse::<u32>()
                    .ok()
                    .filter(|&v| v > 0)
                    .ok_or_else(|| format!("'{item}': the size must be a positive number"))?;
                (n.trim(), Some(size))
            }
            None => (item, None),
        };
        if !matches!(name, "chat" | "files") {
            return Err(format!("unknown scenario '{name}' (known: chat, files)"));
        }
        out.push(Request {
            name: name.to_string(),
            size,
        });
    }
    Ok(out)
}

/// Start whatever `GTKHX_BENCH` asks for, once the main window's chat view
/// exists. A no-op when the variable is unset, so the cost to a normal run
/// is one `getenv`.
///
/// # Safety
/// Called on the GTK main thread with `chat_view` a live `HxChatView *`
/// (or NULL, which skips the chat scenario).
#[no_mangle]
pub unsafe extern "C" fn hx_bench_maybe_start(chat_view: *mut gtk::ffi::GtkWidget) {
    let Ok(spec) = std::env::var("GTKHX_BENCH") else {
        return;
    };
    let quit = std::env::var_os("GTKHX_BENCH_QUIT").is_some();
    let requests = match parse_requests(&spec) {
        Ok(r) => r,
        Err(e) => {
            // A scripted run waits for the app to exit; with nothing to
            // run, it never would.
            if quit {
                eprintln!("GTKHX_BENCH: {e}");
                std::process::exit(2);
            }
            glib::g_warning!("gtkhx", "GTKHX_BENCH: {e}");
            return;
        }
    };
    crate::ensure_gtk_init();
    let chat_view: Option<gtk::Widget> = if chat_view.is_null() {
        None
    } else {
        Some(glib::translate::from_glib_none(chat_view))
    };
    glib::spawn_future_local(async move {
        for r in requests {
            match r.name.as_str() {
                "chat" => match &chat_view {
                    Some(v) => chat::run(v, r.size.unwrap_or(20_000)).await,
                    None => glib::g_warning!("gtkhx", "GTKHX_BENCH: no chat view to measure"),
                },
                "files" => files::run(r.size.unwrap_or(10_000)).await,
                _ => unreachable!("parse_requests only admits known names"),
            }
        }
        if quit {
            if let Some(app) = gtk::gio::Application::default() {
                app.quit();
            } else {
                std::process::exit(0);
            }
        }
    });
}

// ---- waiting on the frame clock ------------------------------------------

/// How long to wait for a frame before giving up on it. Generous: a frame
/// normally comes within a refresh interval, and even the slowest paint
/// measured so far is a fraction of this.
const FRAME_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(5);

thread_local! {
    /// Set when a frame wait timed out, and reported as a failed check.
    /// Frames stop for an unmapped widget — a window closed or minimized,
    /// a pane in a hidden tab — and a wait that hung there would stall
    /// every later scenario and the exit.
    static STALLED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

#[derive(Default)]
struct FrameState {
    at: Option<i64>,
    waker: Option<Waker>,
    /// The tick, after-paint or timeout that got here first; the others
    /// then do nothing.
    fired: bool,
    /// The after-paint connection, so a timeout can drop it.
    paint: Option<(gtk::gdk::FrameClock, glib::SignalHandlerId)>,
}

impl FrameState {
    fn fire(&mut self) {
        if self.fired {
            return;
        }
        self.fired = true;
        self.at = Some(glib::monotonic_time());
        if let Some(w) = self.waker.take() {
            w.wake();
        }
    }
}

/// Which point of a frame to wait for.
#[derive(Clone, Copy)]
enum FramePoint {
    /// The tick, in the frame's update phase — *before* its layout and
    /// paint.
    Tick,
    /// The frame clock's `after-paint` — once layout and paint are done.
    AfterPaint,
}

/// Resolves at the next `point` of `widget`'s frame clock, with the
/// monotonic time it happened.
///
/// Code after the `.await` runs once the main loop gets back to this task,
/// not inside the frame. That matters for what a measurement includes:
///
/// - Between two `next_frame` ticks lies one whole frame, including its
///   layout and paint and any work done between the two awaits. Right for
///   sampling frame intervals.
/// - Work done after an await is laid out and painted in the *following*
///   frame, whose tick comes before that paint. Timing such work to the
///   next tick measures almost nothing — the harness's own first run did
///   exactly that, and the idle-frame check caught it. Time it to
///   `after_paint` instead.
struct FrameWait {
    widget: gtk::Widget,
    point: FramePoint,
    state: Rc<RefCell<FrameState>>,
    armed: bool,
}

impl Future for FrameWait {
    type Output = i64;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<i64> {
        // One wait has already timed out in this scenario: the report will
        // say so, and waiting out a timeout per remaining frame would only
        // delay it.
        if !self.armed && STALLED.get() {
            return Poll::Ready(glib::monotonic_time());
        }
        {
            let mut st = self.state.borrow_mut();
            if let Some(t) = st.at.take() {
                return Poll::Ready(t);
            }
            st.waker = Some(cx.waker().clone());
        }
        if !self.armed {
            self.armed = true;
            match self.point {
                FramePoint::Tick => {
                    let state = self.state.clone();
                    self.widget.add_tick_callback(move |_, _| {
                        state.borrow_mut().fire();
                        glib::ControlFlow::Break
                    });
                }
                FramePoint::AfterPaint => {
                    if let Some(clock) = self.widget.frame_clock() {
                        let state = self.state.clone();
                        let handler = clock.connect_after_paint(move |clock| {
                            let mut st = state.borrow_mut();
                            st.fire();
                            if let Some((_, id)) = st.paint.take() {
                                clock.disconnect(id);
                            }
                        });
                        self.state.borrow_mut().paint = Some((clock, handler));
                        self.widget.queue_draw();
                    }
                    // Unrealized: no frame will come, and the timeout
                    // below reports it.
                }
            }
            let state = self.state.clone();
            glib::timeout_add_local_once(FRAME_TIMEOUT, move || {
                let mut st = state.borrow_mut();
                if st.fired {
                    return;
                }
                if let Some((clock, id)) = st.paint.take() {
                    clock.disconnect(id);
                }
                STALLED.set(true);
                st.fire();
            });
        }
        Poll::Pending
    }
}

fn next_frame(widget: &impl IsA<gtk::Widget>) -> FrameWait {
    frame_wait(widget, FramePoint::Tick)
}

/// Resolves once the next frame has been laid out and painted.
fn after_paint(widget: &impl IsA<gtk::Widget>) -> FrameWait {
    frame_wait(widget, FramePoint::AfterPaint)
}

fn frame_wait(widget: &impl IsA<gtk::Widget>, point: FramePoint) -> FrameWait {
    FrameWait {
        widget: widget.clone().upcast(),
        point,
        state: Rc::default(),
        armed: false,
    }
}

/// Let the window settle, and return the median idle frame interval in µs:
/// the known value every report is checked against.
async fn warm_up(widget: &impl IsA<gtk::Widget>) -> i64 {
    let mut last = next_frame(widget).await;
    let mut gaps = Vec::with_capacity(WARMUP_FRAMES);
    for _ in 0..WARMUP_FRAMES {
        widget.queue_draw();
        let now = next_frame(widget).await;
        gaps.push(now - last);
        last = now;
    }
    Stats::of(&gaps).median
}

/// Summary of a run of frame intervals, in µs.
#[derive(Debug, Clone, Copy, PartialEq)]
struct Stats {
    n: usize,
    mean: f64,
    median: i64,
    p95: i64,
    worst: i64,
    total: i64,
}

impl Stats {
    fn of(samples: &[i64]) -> Stats {
        if samples.is_empty() {
            return Stats {
                n: 0,
                mean: 0.0,
                median: 0,
                p95: 0,
                worst: 0,
                total: 0,
            };
        }
        let mut s = samples.to_vec();
        s.sort_unstable();
        let total: i64 = s.iter().sum();
        Stats {
            n: s.len(),
            mean: total as f64 / s.len() as f64,
            median: s[s.len() / 2],
            p95: s[(s.len() * 95 / 100).min(s.len() - 1)],
            worst: s[s.len() - 1],
            total,
        }
    }
}

fn ms(us: i64) -> f64 {
    us as f64 / 1000.0
}

/// Print a report block. Lines are `label value unit  (note)`, aligned, so
/// `tools/uibench.sh` and a human can both read it.
struct Report {
    title: String,
    lines: Vec<String>,
}

impl Report {
    fn new(title: &str, idle_us: i64) -> Report {
        let mut r = Report {
            title: title.to_string(),
            lines: Vec::new(),
        };
        r.ms(
            "idle frame",
            idle_us,
            "known value: the display's refresh interval",
        );
        r
    }

    fn ms(&mut self, label: &str, us: i64, note: &str) {
        self.line(label, &format!("{:9.2} ms", ms(us)), note);
    }

    fn stats(&mut self, label: &str, s: &Stats) {
        self.line(
            &format!("{label} mean"),
            &format!("{:9.2} ms", s.mean / 1000.0),
            &format!("{} frames", s.n),
        );
        self.ms(&format!("{label} p95"), s.p95, "");
        self.ms(&format!("{label} worst"), s.worst, "");
    }

    fn line(&mut self, label: &str, value: &str, note: &str) {
        let note = if note.is_empty() {
            String::new()
        } else {
            format!("   ({note})")
        };
        self.lines.push(format!("{label:<24}{value}{note}"));
    }

    /// Print the report. A frame wait that timed out during the scenario
    /// is reported here as a failed check, and cleared for the next one.
    fn print(&mut self) {
        if STALLED.replace(false) {
            self.line(
                "CHECK FAILED",
                "",
                "a frame never came; timings after it are meaningless",
            );
        }
        let rule = "=".repeat(60);
        println!();
        println!("=== {} {}", self.title, &rule[self.title.len().min(55)..]);
        for l in &self.lines {
            println!("{l}");
        }
        println!("{rule}");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_names_and_sizes() {
        assert_eq!(
            parse_requests("chat=20000, files").unwrap(),
            vec![
                Request {
                    name: "chat".into(),
                    size: Some(20_000)
                },
                Request {
                    name: "files".into(),
                    size: None
                },
            ]
        );
        assert!(parse_requests("").unwrap().is_empty());
    }

    #[test]
    fn rejects_unknown_names_and_bad_sizes() {
        assert!(parse_requests("chat,nope").is_err());
        assert!(parse_requests("files=0").is_err());
        assert!(parse_requests("files=lots").is_err());
    }

    #[test]
    fn stats_on_known_samples() {
        let s = Stats::of(&[10, 40, 20, 30]);
        assert_eq!((s.n, s.median, s.worst, s.total), (4, 30, 40, 100));
        assert_eq!(s.mean, 25.0);
        assert_eq!(Stats::of(&[]).n, 0);
    }
}
