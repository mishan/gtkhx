//! The Video scenario: a room's cameras, then a screen share on top, in
//! the real Video panel's tiles.
//!
//! Frames take the path a decoded stream's take. A feeder thread stands in
//! for the receive bins' appsinks: it stores each stream's frames in the
//! runtime's own `FrameStore` at the stream's frame rate, and posts one
//! main-loop notice whenever none is already on its way. The notice does
//! what the panel's does — each tile takes its newest frame and wraps it
//! in a texture. The streams run out of step, as independent cameras do.
//!
//! What this leaves out is decoding: the frames are RGBA from the start,
//! so the numbers are the UI's cost, not VP8's. The panel is its own, in
//! its own window, tied to no connection, so nothing is subscribed and no
//! server is involved.
//!
//! Shapes and rates are the ones a publisher encodes at
//! (`Limits::target`): cameras 640×480 at 30 fps, a screen share
//! 1920×1080 at 15.
//!
//! Known-value checks: every tile ends up showing a picture of its
//! stream's size, and shows at least nine in ten of the frames sent to
//! it and no more than were sent.
//!
//! Frames only come while the window is being presented: on a locked
//! desktop the compositor holds them back, and the report says a frame
//! never came.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use hxvoice_runtime::hxvoice::VideoKind;
use hxvoice_runtime::video::{FrameStore, StreamKey, VideoFrame};

use super::{after_paint, next_frame, warm_up, Report, Stats, STALLED};
use crate::video_panel::Standalone;

/// How long each steady-state sample runs: long enough for the slowest
/// stream, 15 fps, to deliver a good number of frames whatever the
/// display's refresh rate.
const SAMPLE_US: i64 = 4_000_000;
/// The share of the frames sent that each tile must show. A UI falling
/// behind skips frames rather than queueing them, so this is where it
/// shows.
const MIN_SHOWN: f64 = 0.9;
/// Distinct pictures per stream, shown in turn.
const PICTURES: usize = 4;

/// One stream the feeder produces.
#[derive(Clone)]
struct Source {
    key: StreamKey,
    fps: u32,
    width: u32,
    height: u32,
}

impl Source {
    fn camera(uid: u16) -> Source {
        Source {
            key: StreamKey {
                user_id: uid,
                kind: VideoKind::Camera,
            },
            fps: 30,
            width: 640,
            height: 480,
        }
    }

    fn screen(uid: u16) -> Source {
        Source {
            key: StreamKey {
                user_id: uid,
                kind: VideoKind::Screen,
            },
            fps: 15,
            width: 1920,
            height: 1080,
        }
    }
}

/// `PICTURES` opaque RGBA pictures of `w`×`h`: a gray field with a light
/// bar in a different place in each, so every frame changes the tile.
fn pictures(w: u32, h: u32) -> Vec<VideoFrame> {
    let stride = w * 4;
    (0..PICTURES)
        .map(|n| {
            let bar = (w as usize * n / PICTURES)..(w as usize * (n + 1) / PICTURES);
            let mut px = vec![0u8; (stride * h) as usize];
            for row in px.chunks_exact_mut(stride as usize) {
                for (x, p) in row.as_chunks_mut::<4>().0.iter_mut().enumerate() {
                    let v = if bar.contains(&x) { 0xe0 } else { 0x50 };
                    *p = [v, v, v, 0xff];
                }
            }
            VideoFrame {
                width: w,
                height: h,
                stride,
                bytes: glib::Bytes::from_owned(px),
            }
        })
        .collect()
}

thread_local! {
    /// The main-thread half of a frames notice, while a feed runs.
    static ON_FRAMES: RefCell<Option<Box<dyn Fn()>>> = const { RefCell::new(None) };
}

/// A thread storing frames for `sources`, as the appsinks do, until
/// dropped.
struct Feeder {
    stop: Arc<AtomicBool>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Feeder {
    fn start(store: Arc<FrameStore>, sources: Vec<Source>) -> Feeder {
        let stop = Arc::new(AtomicBool::new(false));
        let thread = std::thread::Builder::new()
            .name("bench-video-feed".into())
            .spawn({
                let stop = stop.clone();
                move || feed(&store, &sources, &stop)
            })
            .expect("spawn the video feeder");
        Feeder {
            stop,
            thread: Some(thread),
        }
    }
}

impl Drop for Feeder {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

fn feed(store: &Arc<FrameStore>, sources: &[Source], stop: &AtomicBool) {
    // One set of pictures per shape, shared by the streams of that shape.
    let mut by_shape: HashMap<(u32, u32), Vec<VideoFrame>> = HashMap::new();
    let pics: Vec<Vec<VideoFrame>> = sources
        .iter()
        .map(|s| {
            by_shape
                .entry((s.width, s.height))
                .or_insert_with(|| pictures(s.width, s.height))
                .clone()
        })
        .collect();
    let start = Instant::now();
    // Stream i's k-th frame is due at (k + i/n) intervals: each stream at
    // its own rate, out of step with the others.
    let n = sources.len().max(1) as f64;
    let due = |i: usize, k: u64| {
        let period = 1.0 / sources[i].fps as f64;
        Duration::from_secs_f64(period * (k as f64 + i as f64 / n))
    };
    let mut sent = vec![0u64; sources.len()];
    while !stop.load(Ordering::Relaxed) {
        let (i, when) = (0..sources.len())
            .map(|i| (i, due(i, sent[i])))
            .min_by_key(|&(_, t)| t)
            .expect("at least one source");
        if let Some(wait) = when.checked_sub(start.elapsed()) {
            std::thread::sleep(wait);
        }
        let frame = pics[i][sent[i] as usize % PICTURES].clone();
        sent[i] += 1;
        if store.put(sources[i].key, frame) {
            let store = store.clone();
            glib::MainContext::default().invoke(move || {
                store.clear_pending();
                ON_FRAMES.with(|f| {
                    if let Some(f) = f.borrow().as_ref() {
                        f();
                    }
                });
            });
        }
    }
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

/// What the notices did while a phase ran.
#[derive(Default)]
struct Tally {
    notices: Cell<u64>,
    busy_us: Cell<i64>,
    shown: RefCell<HashMap<StreamKey, u64>>,
}

/// Run `sources` in `panel` for `SAMPLE_FRAMES` frames and report it under
/// `label`.
async fn phase(r: &mut Report, label: &str, panel: &Rc<Standalone>, sources: &[Source]) {
    let tiles: Vec<(StreamKey, String)> = sources
        .iter()
        .map(|s| (s.key, format!("bench {}", s.key.user_id)))
        .collect();
    let new = tiles.iter().filter(|(k, _)| !panel.has_tile(*k)).count();
    let t0 = glib::monotonic_time();
    panel.set_tiles(&tiles);
    let t1 = after_paint(&panel.root).await;
    r.ms(
        &format!("{label}: tiles up"),
        t1 - t0,
        &format!("{} tiles, {new} of them new, to first paint", tiles.len()),
    );

    let store = Arc::new(FrameStore::default());
    let tally = Rc::new(Tally::default());
    ON_FRAMES.with(|f| {
        let panel = panel.clone();
        let store = store.clone();
        let tally = tally.clone();
        *f.borrow_mut() = Some(Box::new(move || {
            let t = glib::monotonic_time();
            panel.pull_frames(|key| {
                let frame = store.take(key)?;
                *tally.shown.borrow_mut().entry(key).or_default() += 1;
                Some(frame)
            });
            tally.notices.set(tally.notices.get() + 1);
            tally
                .busy_us
                .set(tally.busy_us.get() + glib::monotonic_time() - t);
        }));
    });
    let feeder = Feeder::start(store.clone(), sources.to_vec());

    // A second for every stream to be running and the first textures to
    // be uploaded, then the sample, counted from its own start.
    let settle = glib::monotonic_time() + 1_000_000;
    while glib::monotonic_time() < settle && !STALLED.get() {
        next_frame(&panel.root).await;
    }
    let mut last = next_frame(&panel.root).await;
    let start = last;
    // Counted from the tick the wall time is.
    let sent0: Vec<u64> = sources.iter().map(|s| store.count(s.key)).collect();
    tally.shown.borrow_mut().clear();
    tally.notices.set(0);
    tally.busy_us.set(0);
    let cpu0 = thread_cpu_us();
    let mut gaps = Vec::new();
    while last - start < SAMPLE_US && !STALLED.get() {
        let now = next_frame(&panel.root).await;
        gaps.push(now - last);
        last = now;
    }
    let wall = last - start;
    let cpu1 = thread_cpu_us();
    drop(feeder);
    ON_FRAMES.with(|f| f.borrow_mut().take());

    r.stats(&format!("{label}: frame"), &Stats::of(&gaps));
    if let (Some(a), Some(b)) = (cpu0, cpu1) {
        r.line(
            "  main-thread CPU",
            &format!("{:9.1} %", 100.0 * (b - a) as f64 / wall.max(1) as f64),
            "of the sample's wall time",
        );
    }
    let notices = tally.notices.get();
    r.line(
        "  notices",
        &format!("{:9.1} /s", notices as f64 * 1e6 / wall.max(1) as f64),
        &format!(
            "{:.3} ms each on the main thread",
            tally.busy_us.get() as f64 / 1000.0 / notices.max(1) as f64
        ),
    );

    let secs = wall.max(1) as f64 / 1e6;
    let shown = tally.shown.borrow();
    let mut kinds: Vec<VideoKind> = Vec::new();
    for s in sources {
        if !kinds.contains(&s.key.kind) {
            kinds.push(s.key.kind);
        }
    }
    for kind in kinds {
        let rates: Vec<f64> = sources
            .iter()
            .filter(|s| s.key.kind == kind)
            .map(|s| shown.get(&s.key).copied().unwrap_or(0) as f64 / secs)
            .collect();
        let fps = sources
            .iter()
            .find(|s| s.key.kind == kind)
            .map_or(0, |s| s.fps);
        let worst = rates.iter().copied().fold(f64::INFINITY, f64::min);
        let mean = rates.iter().sum::<f64>() / rates.len() as f64;
        let what = match kind {
            VideoKind::Camera => "  camera shown",
            VideoKind::Screen => "  screen shown",
        };
        r.line(
            what,
            &format!("{mean:9.1} fps"),
            &format!("sent at {fps}; slowest tile {worst:.1}"),
        );
    }

    for (i, s) in sources.iter().enumerate() {
        let sent = store.count(s.key) - sent0[i];
        let got = shown.get(&s.key).copied().unwrap_or(0);
        if (got as f64) < MIN_SHOWN * sent as f64 || got == 0 {
            r.line(
                "CHECK FAILED",
                "",
                &format!(
                    "{label}: tile {} showed {got} frames of {sent} sent",
                    s.key.user_id
                ),
            );
            return;
        }
        // One more than counted can show: a frame stored just before the
        // sample began and taken just after.
        if got > sent + 1 {
            r.line(
                "CHECK FAILED",
                "",
                &format!(
                    "{label}: tile {} showed {got} frames of {sent} sent",
                    s.key.user_id
                ),
            );
            return;
        }
        let size = panel
            .paintable(s.key)
            .map(|p| (p.intrinsic_width(), p.intrinsic_height()));
        if size != Some((s.width as i32, s.height as i32)) {
            r.line(
                "CHECK FAILED",
                "",
                &format!(
                    "{label}: tile {} shows {size:?}, not {}×{}",
                    s.key.user_id, s.width, s.height
                ),
            );
            return;
        }
    }
}

pub(super) async fn run(n: u32) {
    let panel = Rc::new(Standalone::new());
    let win = gtk::Window::new();
    win.set_title(Some("GtkHx benchmark: video"));
    win.set_default_size(1000, 800);
    win.set_child(Some(&panel.root));
    win.present();

    let idle = warm_up(&panel.root).await;
    let mut r = Report::new(&format!("video (n={n})"), idle);

    // Uids from 1; below u16::MAX, which the grid's sort reserves for a
    // tile it can't name.
    let n = n.min(u32::from(u16::MAX) - 1) as u16;
    let cameras: Vec<Source> = (1..=n).map(Source::camera).collect();
    phase(&mut r, "cameras", &panel, &cameras).await;

    let mut with_screen = cameras.clone();
    with_screen.push(Source::screen(1));
    phase(&mut r, "+ screen", &panel, &with_screen).await;

    r.print();
    panel.set_tiles(&[]);
    win.destroy();
}
