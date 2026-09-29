//! What a scrollback costs in memory, per row.
//!
//! Not a criterion benchmark: it counts bytes, not time. A counting
//! allocator wraps the system one, and the report is the heap live after
//! appending a realistic corpus, and again after every row has been laid
//! out — the worst case, reached only by scrolling through the whole
//! scrollback, since the engine lays out what is on screen.
//!
//! Run: `cargo bench -p rotulus-layout --bench memory`. The numbers are the
//! engine's alone; the widget adds a texture per inline image and nothing
//! per text row.

use rotulus_layout::markdown::parse_inline;
use rotulus_layout::{ChatBuffer, FixedMeasure, LayoutParams, Message, ParsedText, Speaker};
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering};

struct Counting;

static LIVE: AtomicUsize = AtomicUsize::new(0);
/// Live allocations. Each also costs the allocator's own header (16 bytes
/// in glibc) that `LIVE` doesn't see, so fewer is better even at equal
/// bytes.
static COUNT: AtomicUsize = AtomicUsize::new(0);

unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        LIVE.fetch_add(l.size(), Ordering::Relaxed);
        COUNT.fetch_add(1, Ordering::Relaxed);
        System.alloc(l)
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        LIVE.fetch_sub(l.size(), Ordering::Relaxed);
        COUNT.fetch_sub(1, Ordering::Relaxed);
        System.dealloc(p, l)
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, new: usize) -> *mut u8 {
        LIVE.fetch_add(new, Ordering::Relaxed);
        LIVE.fetch_sub(l.size(), Ordering::Relaxed);
        System.realloc(p, l, new)
    }
}

#[global_allocator]
static A: Counting = Counting;

const WORDS: &[&str] = &[
    "hotline",
    "server",
    "anyone",
    "around",
    "tonight",
    "the",
    "files",
    "are",
    "up",
    "again",
    "**bold**",
    "`code`",
    "https://example.org/x",
    "ok",
    "lol",
    "mac",
    "classic",
    "news",
    "brb",
];
const NICKS: &[&str] = &["al", "misha", "hx_fan_1999", "zed", "somebody_longer"];

/// One chat line of 3–19 words, the same corpus the timing benchmarks use,
/// with a nick column as an application draws it.
fn line(i: usize, seed: &mut u64) -> Message {
    let mut next = || {
        *seed = seed
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        (*seed >> 33) as usize
    };
    let words = 3 + next() % 17;
    let text: Vec<&str> = (0..words).map(|_| WORDS[next() % WORDS.len()]).collect();
    let nick = NICKS[i % NICKS.len()];
    let mut m = Message::live(
        Speaker::new((i % 64) as u64 + 1, nick),
        parse_inline(&text.join(" ")),
    );
    m.gutter = Some(ParsedText::plain(format!("<{nick}>")));
    m.with_timestamp(1_700_000_000 + i as i64)
}

fn main() {
    const VIEWPORT: u32 = 600;
    let measure = FixedMeasure::new(8);
    println!(
        "{:>9}  {:>14}  {:>9}  {:>10}  {:>18}  {:>9}",
        "rows", "after append", "per row", "allocs/row", "every row laid out", "per row"
    );
    for n in [20_000usize, 200_000] {
        let base = LIVE.load(Ordering::Relaxed);
        let base_count = COUNT.load(Ordering::Relaxed);
        let mut b = ChatBuffer::new(LayoutParams {
            width: 800,
            ..LayoutParams::default()
        });
        let mut seed = 0x6874_6b68;
        for i in 0..n {
            b.append(line(i, &mut seed), &measure);
        }
        // The frame a live view paints after the burst.
        let y = b.scroll_offset(VIEWPORT);
        b.ensure_visible(y, VIEWPORT, &measure);
        let appended = LIVE.load(Ordering::Relaxed) - base;
        let allocs = COUNT.load(Ordering::Relaxed) - base_count;

        // Scroll through everything, so every row carries a layout.
        let total = b.total_height();
        let mut y = 0;
        while y < total {
            b.scroll_to(y, VIEWPORT, 0);
            let at = b.scroll_offset(VIEWPORT);
            b.ensure_visible(at, VIEWPORT, &measure);
            y += u64::from(VIEWPORT);
        }
        let laid_out = LIVE.load(Ordering::Relaxed) - base;
        println!(
            "{:>9}  {:>11.1} MB  {:>7} B  {:>10.2}  {:>15.1} MB  {:>7} B",
            n,
            appended as f64 / 1e6,
            appended / n,
            allocs as f64 / n as f64,
            laid_out as f64 / 1e6,
            laid_out / n
        );
        drop(b);
    }
}
