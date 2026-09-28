//! The Media scenario: animated inline images in the real public-chat
//! view, on screen and scrolled out of view.
//!
//! It is the acceptance test for the offscreen-animation defect
//! (docs/image-decoding.md): an animated image the user has scrolled away
//! from should cost nothing. The view fills with N animated images and then
//! a few hundred lines of text, so pinned to the bottom it shows none of
//! them, and three states are measured for a few seconds each:
//!
//! - **text only**, before any image is added — the floor;
//! - **offscreen**, the images present but scrolled out of view;
//! - **on screen**, scrolled up to them.
//!
//! Each reports the frames the clock ran, the paints, and the main thread's
//! CPU time, per second. The images are built here, as textures, and handed
//! to the view the way a decoded inline image is; nothing is fetched or
//! decoded.
//!
//! Checks: on screen, the animation must actually repaint (else the
//! scenario measures nothing); offscreen, it must not. The view is cleared
//! before and after, and nothing has the keyboard focus during the run: a
//! focused text cursor blinks, repainting every frame while it fades.

use gtk::gdk;
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use hxchat_layout::{Block, Message, MessageFlags, MessageKind};
use hxchat_view::HxChatView;
use std::ffi::c_void;

use super::{next_frame, warm_up, Report};

/// How long each state is sampled.
const SAMPLE_US: i64 = 3_000_000;
/// How long the frame clock must go without a frame before the text-only
/// floor is sampled, and the longest to wait for that. Longer than a
/// scrollbar's wait before it fades, which an earlier scenario can leave
/// pending.
const QUIET_US: i64 = 3_000_000;
const QUIET_MAX_US: i64 = 8_000_000;
/// Lines of text after the images, enough to push them out of view.
const TEXT_AFTER: u32 = 400;
/// Tokens for the scenario's images, clear of the ones chat.c hands out.
const TOKEN_BASE: u32 = 0x4000_0000;
/// Frames per image and each frame's delay: 10 frames a second.
const FRAMES: u32 = 4;
const FRAME_MS: u32 = 100;
/// Repaints a second that still count as "not animating". A stray paint
/// or two (a cursor blink, a tooltip) is not the defect.
const OFFSCREEN_PAINTS_OK: f64 = 1.0;
/// Frames a second over the text-only floor that still count as the clock
/// being idle. An animation that kept the clock running without painting
/// would show here and not in the paints.
const OFFSCREEN_FRAMES_OK: f64 = 1.0;

extern "C" {
    fn gtkhx_session_htlc(sess: *mut c_void) -> *mut c_void;
}

/// One animated image: `FRAMES` solid-colored frames, 64×48.
fn frames(i: u32) -> Vec<(gdk::Texture, u32)> {
    const W: usize = 64;
    const H: usize = 48;
    (0..FRAMES)
        .map(|f| {
            let shade = ((i * 37 + f * 60) % 256) as u8;
            let px = [shade, 255 - shade, (f * 80) as u8, 255];
            let data: Vec<u8> = px.iter().copied().cycle().take(W * H * 4).collect();
            let tex = gdk::MemoryTexture::new(
                W as i32,
                H as i32,
                gdk::MemoryFormat::R8g8b8a8,
                &glib::Bytes::from_owned(data),
                W * 4,
            );
            (tex.upcast(), FRAME_MS)
        })
        .collect()
}

fn text(body: String) -> Message {
    Message {
        kind: MessageKind::Live,
        timestamp: 0,
        speaker: None,
        gutter: None,
        blocks: vec![Block::text(body)],
        flags: MessageFlags::NONE,
    }
}

/// This thread's CPU time so far, in µs (Linux's schedstat); 0 elsewhere.
fn thread_cpu_us() -> i64 {
    std::fs::read_to_string("/proc/thread-self/schedstat")
        .ok()
        .and_then(|s| s.split_whitespace().next()?.parse::<i64>().ok())
        .map_or(0, |ns| ns / 1000)
}

/// Frames, paints and CPU per second over one sample.
pub(super) struct Rates {
    frames: f64,
    paints: f64,
    cpu_ms: f64,
}

/// Sample `view`'s frame clock for [`SAMPLE_US`] with nothing else going on.
pub(super) async fn sample(view: &gtk::Widget) -> Option<Rates> {
    // Let work the last step started (layout of new rows) finish first.
    glib::timeout_future(std::time::Duration::from_secs(1)).await;
    let clock = view.frame_clock()?;
    let counts = std::rc::Rc::new(std::cell::Cell::new((0u32, 0u32)));
    let frames_id = clock.connect_update({
        let counts = counts.clone();
        move |_| {
            let (f, p) = counts.get();
            counts.set((f + 1, p));
        }
    });
    let paints_id = clock.connect_paint({
        let counts = counts.clone();
        move |_| {
            let (f, p) = counts.get();
            counts.set((f, p + 1));
        }
    });
    let (t0, cpu0) = (glib::monotonic_time(), thread_cpu_us());
    glib::timeout_future(std::time::Duration::from_micros(SAMPLE_US as u64)).await;
    let (t1, cpu1) = (glib::monotonic_time(), thread_cpu_us());
    clock.disconnect(frames_id);
    clock.disconnect(paints_id);
    let secs = (t1 - t0) as f64 / 1e6;
    let (f, p) = counts.get();
    Some(Rates {
        frames: f as f64 / secs,
        paints: p as f64 / secs,
        cpu_ms: (cpu1 - cpu0) as f64 / 1000.0 / secs,
    })
}

/// Wait until `view`'s frame clock has gone [`QUIET_US`] without a frame,
/// or [`QUIET_MAX_US`] has passed. What an earlier scenario left moving,
/// or about to move — a scrollbar due to fade out, say — then settles
/// before the floor is taken.
async fn until_quiet(view: &gtk::Widget) {
    let Some(clock) = view.frame_clock() else {
        return;
    };
    let last = std::rc::Rc::new(std::cell::Cell::new(glib::monotonic_time()));
    let id = clock.connect_update({
        let last = last.clone();
        move |_| last.set(glib::monotonic_time())
    });
    let give_up = glib::monotonic_time() + QUIET_MAX_US;
    while glib::monotonic_time() - last.get() < QUIET_US && glib::monotonic_time() < give_up {
        glib::timeout_future(std::time::Duration::from_millis(100)).await;
    }
    clock.disconnect(id);
}

pub(super) fn report(r: &mut Report, label: &str, rates: &Rates) {
    r.line(label, "", "");
    r.line(
        "  frames",
        &format!("{:9.1} /s", rates.frames),
        "frame clock ticks",
    );
    r.line("  paints", &format!("{:9.1} /s", rates.paints), "");
    r.line(
        "  main-thread CPU",
        &format!("{:9.1} ms/s", rates.cpu_ms),
        "",
    );
}

pub(super) async fn run(view: &gtk::Widget, n: u32) {
    let Some(chat) = view.downcast_ref::<HxChatView>() else {
        glib::g_warning!("gtkhx", "GTKHX_BENCH media: not a chat view");
        return;
    };
    // The scenario clears the public chat before and after: a real
    // server's scrollback with it.
    let sess = unsafe { crate::ffi::hx_active_session() };
    let htlc = if sess.is_null() {
        std::ptr::null_mut()
    } else {
        unsafe { gtkhx_session_htlc(sess) }
    };
    if !htlc.is_null() && unsafe { gtkhx_core::conn::hx_conn_fd(htlc.cast()) } != 0 {
        glib::g_warning!(
            "gtkhx",
            "GTKHX_BENCH media: the session is connected; run with a scratch configuration"
        );
        return;
    }
    // The chat input's cursor fades in and out on the frame clock, a
    // repaint every frame while it blinks, which would swamp what this
    // measures. It blinks for a while after anything moves the focus or
    // types, which an earlier scenario may just have done, and turning the
    // blink setting off doesn't stop a blink already running. So nothing
    // has the focus for the run; it goes back after.
    let root = view.root();
    let focus = root.as_ref().and_then(|r| r.focus());
    if let Some(r) = &root {
        r.set_focus(None::<&gtk::Widget>);
    }
    measure(chat, view, n).await;
    if let Some(w) = &focus {
        w.grab_focus();
    }
    chat.clear();
}

async fn measure(chat: &HxChatView, view: &gtk::Widget, n: u32) {
    chat.clear();
    let idle = warm_up(view).await;
    let mut r = Report::new("media", idle);
    r.line("animated images", &format!("{n:9}"), "");

    // ---- text only --------------------------------------------------------
    for i in 0..TEXT_AFTER {
        chat.append(text(format!("filler line {i}, before any image")));
    }
    chat.scroll_to_bottom();
    next_frame(view).await;
    until_quiet(view).await;
    let Some(floor) = sample(view).await else {
        r.line("CHECK FAILED", "", "the view has no frame clock");
        r.print();
        return;
    };
    report(&mut r, "text only", &floor);
    let floor_frames = floor.frames;

    // ---- images, scrolled out of view ---------------------------------------
    chat.clear();
    for i in 0..n {
        let token = TOKEN_BASE + i;
        chat.append(Message {
            kind: MessageKind::Live,
            timestamp: 0,
            speaker: None,
            gutter: None,
            blocks: vec![Block::Image {
                token,
                size: None,
                alt: format!("animated image {i}"),
            }],
            flags: MessageFlags::NONE,
        });
        chat.set_media_frames(token, frames(i));
        chat.append(text(format!("a line under image {i}")));
    }
    for i in 0..TEXT_AFTER {
        chat.append(text(format!("filler line {i}, below every image")));
    }
    chat.scroll_to_bottom();
    next_frame(view).await;
    next_frame(view).await;
    let offscreen = sample(view).await;

    // ---- images on screen -----------------------------------------------------
    chat.scroll_to_extreme(false);
    next_frame(view).await;
    next_frame(view).await;
    let onscreen = sample(view).await;

    let (Some(offscreen), Some(onscreen)) = (offscreen, onscreen) else {
        r.line("CHECK FAILED", "", "the view lost its frame clock");
        r.print();
        return;
    };
    report(&mut r, "offscreen", &offscreen);
    report(&mut r, "on screen", &onscreen);
    if onscreen.paints < OFFSCREEN_PAINTS_OK {
        r.line(
            "  CHECK FAILED",
            &format!("{:9.1} /s", onscreen.paints),
            "the on-screen animation didn't repaint",
        );
    }
    if offscreen.paints > OFFSCREEN_PAINTS_OK {
        r.line(
            "  CHECK FAILED",
            &format!("{:9.1} /s", offscreen.paints),
            "animation out of view still repaints",
        );
    }
    if offscreen.frames > floor_frames + OFFSCREEN_FRAMES_OK {
        r.line(
            "  CHECK FAILED",
            &format!("{:9.1} /s", offscreen.frames),
            "animation out of view keeps the frame clock running",
        );
    }

    r.print();
}

#[cfg(test)]
mod tests {
    use super::*;

    /// This thread's CPU time from the C library, in µs: the known value
    /// the schedstat reading is held to. `struct timespec` is two 64-bit
    /// fields on the 64-bit targets this is gated to.
    #[cfg(all(target_os = "linux", target_pointer_width = "64"))]
    fn clock_thread_cpu_us() -> i64 {
        #[repr(C)]
        struct Timespec {
            sec: i64,
            nsec: i64,
        }
        extern "C" {
            fn clock_gettime(clock: i32, ts: *mut Timespec) -> i32;
        }
        const CLOCK_THREAD_CPUTIME_ID: i32 = 3;
        let mut ts = Timespec { sec: 0, nsec: 0 };
        assert_eq!(
            unsafe { clock_gettime(CLOCK_THREAD_CPUTIME_ID, &mut ts) },
            0
        );
        ts.sec * 1_000_000 + ts.nsec / 1000
    }

    #[cfg(all(target_os = "linux", target_pointer_width = "64"))]
    #[test]
    fn schedstat_counts_this_threads_cpu_time() {
        // The kernel adds a thread's running time up when it is switched
        // out, so a short sleep before each read brings it up to date.
        let settle = || std::thread::sleep(std::time::Duration::from_millis(2));
        settle();
        let a = thread_cpu_us();
        // 20 ms of this thread's own CPU time, however long that takes on
        // a loaded machine: spinning for 20 ms of wall time could be
        // preempted for most of it.
        let until = clock_thread_cpu_us() + 20_000;
        let mut x = 0u64;
        while clock_thread_cpu_us() < until {
            x = x.wrapping_add(std::hint::black_box(x) ^ 7);
        }
        std::hint::black_box(x);
        settle();
        let b = thread_cpu_us();
        assert!(b - a >= 18_000, "20 ms of CPU read as {} µs", b - a);
    }
}
