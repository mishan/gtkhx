//! Model microbenchmarks: a chat's member list through a large login, a
//! burst of user changes and a leave; nick completion over it; and the
//! Files panel's populate from a FILE_LIST reply.
//!
//! Member counts span a busy server (100), a very large one (1,000) and
//! the wire's ceiling in practice (5,000); file counts match the Files
//! panel UI scenario.
//!
//! Two instrument checks. `files_populate` is the decode the UI scenario's
//! "remote populate" wraps, minus the sort and the GTK work, so per entry
//! it must come in below that scenario's figure. And nick completion runs
//! twice, through the C ABI the chat entry calls and over a plain
//! `MemberList`; both call the same `complete_styled`, so the difference
//! is the ABI's walk of the GObject model.
//!
//! Run: `cargo bench -p hxmodel`. See docs/performance.md.

use criterion::{criterion_group, criterion_main, BatchSize, BenchmarkId, Criterion, Throughput};
use gio::prelude::*;
use hxmodel::chat::{Conversation, Member};
use hxmodel::chat_members::hx_nick_complete;
use hxmodel::files_entry::{populate, HxFileEntry};
use hxmodel::member::HxMemberModel;
use hxsession::FileEntry;
use std::ffi::{c_void, CString};
use std::hint::black_box;
use std::os::raw::c_char;

const MEMBERS: [u16; 3] = [100, 1000, 5000];
const FILES: [usize; 2] = [1000, 10_000];

fn member(uid: u16) -> Member {
    Member {
        uid,
        icon: 128 + uid % 64,
        status: 0,
        nick_color: None,
        name: format!("user{uid:05}"),
        ignore: false,
    }
}

fn model_with(n: u16) -> HxMemberModel {
    let model = HxMemberModel::new();
    for uid in 1..=n {
        model.upsert(&member(uid));
    }
    model
}

fn bench_members(c: &mut Criterion) {
    let mut g = c.benchmark_group("member_login");
    for n in MEMBERS {
        let members: Vec<Member> = (1..=n).map(member).collect();
        g.throughput(Throughput::Elements(n as u64));
        g.bench_with_input(BenchmarkId::from_parameter(n), &members, |b, ms| {
            b.iter(|| {
                let model = HxMemberModel::new();
                for m in ms {
                    model.upsert(black_box(m));
                }
                model
            })
        });
    }
    g.finish();

    // Every member changes once — a status sweep, or a server restarting
    // its idle timers. Updates in place, so it should be flat per change.
    let mut g = c.benchmark_group("member_change");
    for n in MEMBERS {
        let model = model_with(n);
        let changed: Vec<Member> = (1..=n)
            .map(|uid| Member {
                status: 1,
                ..member(uid)
            })
            .collect();
        g.throughput(Throughput::Elements(n as u64));
        g.bench_with_input(BenchmarkId::from_parameter(n), &changed, |b, ms| {
            b.iter(|| {
                for m in ms {
                    model.upsert(black_box(m));
                }
            })
        });
    }
    g.finish();

    // The longest-connected member leaves: removing the front re-indexes
    // everyone behind it.
    let mut g = c.benchmark_group("member_leave");
    for n in MEMBERS {
        g.bench_function(BenchmarkId::from_parameter(n), |b| {
            b.iter_batched(
                || model_with(n),
                |model| {
                    model.remove(black_box(1));
                    model
                },
                BatchSize::LargeInput,
            )
        });
    }
    g.finish();
}

fn nick_complete_ffi(model: &HxMemberModel, input: &CString) -> bool {
    let mut text: *mut c_char = std::ptr::null_mut();
    let mut cursor = 0i32;
    let mut info: *mut c_char = std::ptr::null_mut();
    let ptr = model.as_ptr() as *mut c_void;
    let n = input.as_bytes().len();
    unsafe {
        let ok = hx_nick_complete(
            ptr,
            input.as_ptr(),
            n,
            0,
            ':' as u32,
            0,
            &mut text,
            &mut cursor,
            &mut info,
        );
        glib::ffi::g_free(text.cast());
        glib::ffi::g_free(info.cast());
        ok != 0
    }
}

fn bench_nick_complete(c: &mut Criterion) {
    // "user0" matches everyone below uid 10,000 — the ambiguous case that
    // lists candidates; "user00042" matches one.
    let mut g = c.benchmark_group("nick_complete");
    for n in MEMBERS {
        let model = model_with(n);
        let mut conv = Conversation::new(0);
        for uid in 1..=n {
            conv.members.upsert(member(uid));
        }
        for word in ["user0", "user00042"] {
            let input = CString::new(format!("hi {word}")).unwrap();
            assert!(nick_complete_ffi(&model, &input), "{word} completes");
            g.bench_with_input(
                BenchmarkId::new(format!("ffi/{word}"), n),
                &input,
                |b, i| b.iter(|| nick_complete_ffi(&model, black_box(i))),
            );
            let text = input.to_str().unwrap();
            let len = text.chars().count();
            g.bench_with_input(BenchmarkId::new(format!("list/{word}"), n), text, |b, t| {
                b.iter(|| conv.complete(black_box(t), len, false, ':'))
            });
        }
    }
    g.finish();
}

/// A listing of `n` entries: mostly files of a few types, one in ten a
/// folder.
fn file_list(n: usize) -> Vec<FileEntry> {
    let types: [&[u8; 4]; 4] = [b"TEXT", b"JPEG", b"SITD", b"MP3 "];
    (0..n)
        .map(|i| {
            // Mac Roman: 0x8e is é.
            let mut name_bytes = format!("file {i:05} caf").into_bytes();
            name_bytes.extend_from_slice(b"\x8e.dat");
            let (type_code, size) = if i % 10 == 0 {
                (b"fldr", (i % 50) as u64)
            } else {
                (types[i % types.len()], (i * 1013) as u64)
            };
            FileEntry {
                name: hxproto::text::to_utf8(&name_bytes),
                name_bytes,
                folder: i % 10 == 0,
                size,
                type_code: *type_code,
                creator: *b"MACR",
            }
        })
        .collect()
}

fn bench_files_populate(c: &mut Criterion) {
    let mut g = c.benchmark_group("files_populate");
    for n in FILES {
        let files = file_list(n);
        let store = gio::ListStore::with_type(HxFileEntry::static_type());
        unsafe { populate(store.as_ptr().cast(), &files) };
        assert_eq!(store.n_items() as usize, n, "every entry lists");
        g.throughput(Throughput::Elements(n as u64));
        g.bench_with_input(BenchmarkId::from_parameter(n), &files, |b, f| {
            b.iter(|| unsafe { populate(store.as_ptr().cast(), black_box(f)) })
        });
    }
    g.finish();
}

criterion_group!(
    benches,
    bench_members,
    bench_nick_complete,
    bench_files_populate
);
criterion_main!(benches);
