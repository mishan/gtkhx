//! Icon microbenchmarks over the `icons.rsrc` GtkHx ships: opening the
//! resource file, finding an icon by id, and decoding it.
//!
//! Every user-list row and private-message window resolves its icon through
//! `load_icon` in `src/cicn.c`, which looks the id up with `res_of_id` and
//! decodes the `cicn` from scratch each time — so a large login pays the
//! lookup and the decode once per user.
//!
//! The instrument check is `lookup/first`: the first id in the file is
//! found on the first step of `res_of_id`'s walk, so it should cost the
//! same as `nth_res_of_type(0)`, which does no walk at all
//! (`lookup/nth0`).
//!
//! Run: `cargo bench -p hxmacres`. See docs/performance.md.

use criterion::{criterion_group, criterion_main, BenchmarkId, Criterion, Throughput};
use hxmacres::{cicn, ResourceFork};
use std::hint::black_box;

const ICONS: &[u8] = include_bytes!("../../../../icons.rsrc");
const CICN: u32 = 0x6369_636e;

fn fork() -> ResourceFork {
    ResourceFork::parse(ICONS.to_vec()).expect("icons.rsrc parses")
}

fn bench_parse(c: &mut Criterion) {
    let mut g = c.benchmark_group("parse");
    g.throughput(Throughput::Bytes(ICONS.len() as u64));
    // Includes the copy of the file into the fork, as `macres_file_open`
    // reads it.
    g.bench_function("icons_rsrc", |b| {
        b.iter(|| ResourceFork::parse(black_box(ICONS).to_vec()))
    });
    g.finish();
}

fn bench_lookup(c: &mut Criterion) {
    let fork = fork();
    let n = fork.num_res_of_type(CICN);
    assert!(n > 1, "icons.rsrc holds cicns");
    let first = fork.nth_res_of_type(CICN, 0).unwrap().resid;
    let last = fork.nth_res_of_type(CICN, n - 1).unwrap().resid;

    let mut g = c.benchmark_group("lookup");
    g.bench_function("nth0", |b| {
        b.iter(|| {
            fork.nth_res_of_type(CICN, black_box(0))
                .map(|r| r.data.len())
        })
    });
    g.bench_function("first", |b| {
        b.iter(|| fork.res_of_id(CICN, black_box(first)).map(|r| r.data.len()))
    });
    g.bench_function(BenchmarkId::new("last", n), |b| {
        b.iter(|| fork.res_of_id(CICN, black_box(last)).map(|r| r.data.len()))
    });
    g.finish();
}

fn bench_decode(c: &mut Criterion) {
    let fork = fork();
    let n = fork.num_res_of_type(CICN);
    let all: Vec<&[u8]> = (0..n)
        .filter_map(|i| fork.nth_res_of_type(CICN, i).map(|r| r.data))
        .collect();
    // `DEFAULT_ICON` in src/cicn.c, what an unknown id falls back to.
    let default = fork.res_of_id(CICN, 135).expect("icon 135").data;

    let mut g = c.benchmark_group("decode");
    g.bench_function("icon_135", |b| b.iter(|| cicn::decode(black_box(default))));
    g.throughput(Throughput::Elements(all.len() as u64));
    g.bench_function(BenchmarkId::new("every_icon", all.len()), |b| {
        b.iter(|| {
            all.iter()
                .filter_map(|d| cicn::decode(black_box(d)))
                .count()
        })
    });
    g.finish();
}

criterion_group!(benches, bench_parse, bench_lookup, bench_decode);
criterion_main!(benches);
