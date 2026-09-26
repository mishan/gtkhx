//! Layout-engine microbenchmarks: the CPU half of what `src/chat_bench.c`
//! measures through the real frame clock.
//!
//! These run headless against [`FixedMeasure`], so they time the engine —
//! wrapping, the height index, anchoring, search — and not Pango's text
//! shaping. A regression here is a regression in our code; a regression
//! that only shows in `tools/chatbench.sh` is somewhere else.
//!
//! The scrollback sizes are the instrument check. The engine's central
//! claim is that a frame costs O(visible), not O(scrollback), so
//! `first_paint`, `relayout`, `scroll_walk` and `live_at_cap` should report
//! about the same time at 2,000 and 20,000 messages. Where they scale with
//! the size, either the claim is broken there or the benchmark stopped
//! measuring what it says; docs/performance.md records which is which.
//!
//! Run: `cargo bench -p hxchat-layout`. See docs/performance.md.

use criterion::{criterion_group, criterion_main, BatchSize, BenchmarkId, Criterion, Throughput};
use hxchat_layout::markdown::parse_inline;
use hxchat_layout::{ChatBuffer, FixedMeasure, LayoutParams, Message, Speaker};
use std::hint::black_box;

const WIDTH: u32 = 800;
const VIEWPORT: u32 = 600;
const SIZES: [usize; 2] = [2_000, 20_000];

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
/// Five nick widths, as `chat_bench.c` cycles, so the gutter settles early
/// and the wrap path sees varied lengths.
const NICKS: &[&str] = &["al", "misha", "hx_fan_1999", "zed", "somebody_longer"];

/// A small deterministic PRNG so every run sees the same corpus.
struct Lcg(u64);

impl Lcg {
    fn next(&mut self) -> u64 {
        self.0 = self
            .0
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        self.0 >> 33
    }
}

/// `n` chat lines of 3–19 words.
fn corpus(n: usize) -> Vec<Message> {
    let mut rng = Lcg(0x6874_6b68);
    (0..n)
        .map(|i| {
            let words = 3 + (rng.next() % 17) as usize;
            let text: Vec<&str> = (0..words)
                .map(|_| WORDS[rng.next() as usize % WORDS.len()])
                .collect();
            let nick = NICKS[i % NICKS.len()];
            Message::live(
                Speaker::new(i as u16 % 64, nick),
                parse_inline(&text.join(" ")),
            )
        })
        .collect()
}

fn params() -> LayoutParams {
    LayoutParams {
        width: WIDTH,
        ..LayoutParams::default()
    }
}

fn filled(n: usize, measure: &FixedMeasure) -> ChatBuffer {
    let mut b = ChatBuffer::new(params());
    for m in corpus(n) {
        b.append(m, measure);
    }
    b
}

/// Lay out whatever the anchor says is on screen — one frame's work.
fn paint(b: &mut ChatBuffer, measure: &FixedMeasure) -> usize {
    let y = b.scroll_offset(VIEWPORT);
    b.ensure_visible(y, VIEWPORT, measure).len()
}

fn bench_ingest(c: &mut Criterion) {
    let measure = FixedMeasure::new(8);
    let mut g = c.benchmark_group("ingest");
    for n in SIZES {
        g.throughput(Throughput::Elements(n as u64));
        g.bench_with_input(BenchmarkId::from_parameter(n), &n, |bch, &n| {
            bch.iter_batched(
                || corpus(n),
                |msgs| {
                    let mut b = ChatBuffer::new(params());
                    for m in msgs {
                        b.append(m, &measure);
                    }
                    b
                },
                BatchSize::LargeInput,
            );
        });
    }
    g.finish();
}

/// Ingest's honest partner: the first frame after a burst of appends,
/// which pays for whatever layout ingest deferred.
fn bench_first_paint(c: &mut Criterion) {
    let measure = FixedMeasure::new(8);
    let mut g = c.benchmark_group("first_paint");
    for n in SIZES {
        g.bench_with_input(BenchmarkId::from_parameter(n), &n, |bch, &n| {
            bch.iter_batched(
                || filled(n, &measure),
                // Hand the buffer back so criterion drops it outside the
                // timed region; freeing the scrollback isn't a paint cost.
                |mut b| {
                    let rows = paint(&mut b, &measure);
                    (b, rows)
                },
                BatchSize::LargeInput,
            );
        });
    }
    g.finish();
}

/// A font change invalidates every cached wrap; the next frame should only
/// re-wrap what is on screen.
fn bench_relayout(c: &mut Criterion) {
    let measure = FixedMeasure::new(8);
    let mut g = c.benchmark_group("relayout");
    for n in SIZES {
        let mut b = filled(n, &measure);
        paint(&mut b, &measure);
        let mut font = 0;
        g.bench_function(BenchmarkId::from_parameter(n), |bch| {
            bch.iter(|| {
                font += 1;
                b.set_font_generation(font);
                black_box(paint(&mut b, &measure))
            });
        });
    }
    g.finish();
}

/// Walk from the top a third of a viewport per frame, so every frame lands
/// on rows that have not been laid out yet.
fn bench_scroll_walk(c: &mut Criterion) {
    const FRAMES: u64 = 120;
    let measure = FixedMeasure::new(8);
    let mut g = c.benchmark_group("scroll_walk");
    g.throughput(Throughput::Elements(FRAMES));
    for n in SIZES {
        g.bench_with_input(BenchmarkId::from_parameter(n), &n, |bch, &n| {
            bch.iter_batched(
                || filled(n, &measure),
                |mut b| {
                    let mut rows = 0;
                    for f in 0..FRAMES {
                        let y = f * u64::from(VIEWPORT / 3);
                        b.scroll_to(y, VIEWPORT, 0);
                        rows += paint(&mut b, &measure);
                    }
                    (b, rows)
                },
                BatchSize::LargeInput,
            );
        });
    }
    g.finish();
}

/// Steady state of a long-lived chat: the scrollback is at its cap, so
/// every new message trims the oldest one, then the frame paints.
fn bench_live_at_cap(c: &mut Criterion) {
    let measure = FixedMeasure::new(8);
    let mut g = c.benchmark_group("live_at_cap");
    for n in SIZES {
        let mut b = filled(n, &measure);
        b.set_max_rows(n, &measure);
        paint(&mut b, &measure);
        let mut more = corpus(1024).into_iter().cycle();
        g.bench_function(BenchmarkId::from_parameter(n), |bch| {
            bch.iter_batched(
                || more.next().expect("cycle"),
                |m| {
                    b.append(m, &measure);
                    paint(&mut b, &measure)
                },
                BatchSize::SmallInput,
            );
        });
    }
    g.finish();
}

/// Search reads the model, not the layout, so this one is expected to
/// scale with the scrollback.
fn bench_search(c: &mut Criterion) {
    let measure = FixedMeasure::new(8);
    let mut g = c.benchmark_group("search");
    for n in SIZES {
        let b = filled(n, &measure);
        g.throughput(Throughput::Elements(n as u64));
        g.bench_function(BenchmarkId::from_parameter(n), |bch| {
            bch.iter(|| black_box(b.search(black_box("Classic"), false).len()));
        });
    }
    g.finish();
}

fn bench_markdown(c: &mut Criterion) {
    let line = "the **files** are up again, see `news` or https://example.org/x ~~lol~~ *brb*";
    c.bench_function("parse_inline", |bch| {
        bch.iter(|| black_box(parse_inline(black_box(line))));
    });
}

criterion_group!(
    benches,
    bench_ingest,
    bench_first_paint,
    bench_relayout,
    bench_scroll_walk,
    bench_live_at_cap,
    bench_search,
    bench_markdown
);
criterion_main!(benches);
