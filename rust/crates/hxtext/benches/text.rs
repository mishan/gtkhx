//! Text-encoding microbenchmarks: the decode every payload from the server
//! goes through, the encode every line sent goes through, and the
//! `:shortcode:` work on both sides of it.
//!
//! Line sizes follow the wire: a chat line, a long news post, and a large
//! agreement or file comment.
//!
//! The instrument check is `to_utf8/utf8`: already-valid UTF-8 takes the
//! fast path, which is a validate and a copy, so it should run at the rate
//! of `str::from_utf8` plus a `memcpy` (`to_utf8/validate_copy`). A large
//! gap means the wrapper, not the validation, is the cost.
//!
//! Run: `cargo bench -p hxtext`. See docs/performance.md.

use criterion::{criterion_group, criterion_main, BenchmarkId, Criterion, Throughput};
use hxtext::{for_wire, gtkhx_text_to_utf8};
use std::hint::black_box;
use std::os::raw::c_char;

const SIZES: [usize; 3] = [80, 4096, 64 * 1024];

/// `n` bytes of UTF-8 text with a sprinkling of accented letters.
fn utf8_text(n: usize) -> Vec<u8> {
    let unit = "Hello, café crème — naïve résumé text. ".as_bytes();
    let mut v: Vec<u8> = unit.iter().copied().cycle().take(n).collect();
    // Don't end mid-character.
    while std::str::from_utf8(&v).is_err() {
        v.pop();
    }
    v
}

/// `n` bytes of Mac Roman: the same text with its accents as single high
/// bytes, which is not valid UTF-8 and so takes the decode path.
fn mac_roman_text(n: usize) -> Vec<u8> {
    let unit = b"Hello, caf\x8e cr\x8fme \xd1 na\x95ve r\x8esum\x8e text. ";
    unit.iter().copied().cycle().take(n).collect()
}

/// A chat line of about `n` bytes with an emoji every few words.
fn emoji_text(n: usize) -> String {
    let unit = "nice one 👍 see you later 😀 ok ";
    unit.chars()
        .cycle()
        .scan(0, |len, c| {
            *len += c.len_utf8();
            (*len <= n).then_some(c)
        })
        .collect()
}

fn to_utf8(input: &[u8]) -> usize {
    let mut out_len = 0;
    unsafe {
        let p = gtkhx_text_to_utf8(input.as_ptr() as *const c_char, input.len(), &mut out_len);
        glib::ffi::g_free(p.cast());
    }
    out_len
}

fn bench_to_utf8(c: &mut Criterion) {
    let mut g = c.benchmark_group("to_utf8");
    for n in SIZES {
        let utf8 = utf8_text(n);
        let mac = mac_roman_text(n);
        g.throughput(Throughput::Bytes(n as u64));
        g.bench_with_input(BenchmarkId::new("utf8", n), &utf8, |b, s| {
            b.iter(|| to_utf8(black_box(s)))
        });
        g.bench_with_input(BenchmarkId::new("validate_copy", n), &utf8, |b, s| {
            b.iter(|| {
                std::str::from_utf8(black_box(s)).unwrap();
                s.to_vec()
            })
        });
        g.bench_with_input(BenchmarkId::new("mac_roman", n), &mac, |b, s| {
            b.iter(|| to_utf8(black_box(s)))
        });
    }
    g.finish();
}

fn bench_for_wire(c: &mut Criterion) {
    let mut g = c.benchmark_group("for_wire");
    for n in SIZES {
        let plain = utf8_text(n);
        let emoji = emoji_text(n).into_bytes();
        g.throughput(Throughput::Bytes(n as u64));
        g.bench_with_input(BenchmarkId::new("utf8_mode", n), &plain, |b, s| {
            b.iter(|| for_wire(black_box(s), true, true))
        });
        g.bench_with_input(BenchmarkId::new("mac_roman", n), &plain, |b, s| {
            b.iter(|| for_wire(black_box(s), false, true))
        });
        g.bench_with_input(BenchmarkId::new("mac_roman_emoji", n), &emoji, |b, s| {
            b.iter(|| for_wire(black_box(s), false, true))
        });
    }
    g.finish();
}

/// The receive side of the shortcode rewrite, and the completion popup's
/// lookup, which runs on every keystroke after a `:`.
fn bench_shortcodes(c: &mut Criterion) {
    let mut g = c.benchmark_group("shortcodes");
    let line = "nice one :thumbsup: see you later :grinning: ok ".repeat(2);
    g.throughput(Throughput::Bytes(line.len() as u64));
    g.bench_function("to_emoji", |b| {
        b.iter(|| hxproto::emoji::shortcodes_to_emoji(black_box(&line)))
    });
    g.finish();

    // The popup shows eight (`TA_MAX_MATCHES` in gtkhx-ui's emoji.rs). A
    // one-letter prefix matches the most names; a longer one, few.
    let mut g = c.benchmark_group("shortcode_matches");
    for prefix in ["s", "smi", "thumbsup"] {
        g.bench_with_input(BenchmarkId::from_parameter(prefix), prefix, |b, p| {
            b.iter(|| hxproto::emoji::shortcode_matches(black_box(p), 8))
        });
    }
    g.finish();
}

criterion_group!(benches, bench_to_utf8, bench_for_wire, bench_shortcodes);
criterion_main!(benches);
