//! Cipher and hash microbenchmarks for the paths every byte of a HOPE or
//! AEAD connection goes through.
//!
//! Buffer sizes follow the wire: a small chat transaction, a large file
//! list, and the 0xf000-byte chunk the HTXF loops in `hxnet::xfer` move.
//!
//! The instrument check is the `blowfish_ofb64` group against the
//! `blowfish` crate's own block rate: OFB-64 encrypts one 8-byte block per
//! 8 bytes of stream, so its throughput should sit close to the raw block
//! cipher's. A large gap means the per-byte loop, not Blowfish, is the cost.
//!
//! Run: `cargo bench -p hxcrypto`. See docs/performance.md.

use blowfish::Blowfish;
use cipher::{Array, BlockCipherEncrypt, KeyInit};
use criterion::{criterion_group, criterion_main, BenchmarkId, Criterion, Throughput};
use hxcrypto::aead::{AeadState, AEAD_DIR_CLIENT_TO_SERVER, AEAD_LENGTH_PREFIX, AEAD_TAG_SIZE};
use hxcrypto::hash::hmac_xxx;
use hxcrypto::stream::{BlowfishOfb64State, BLOWFISH_OFB64_BLOCK_SIZE};
use std::hint::black_box;

/// A chat line, a big file-list reply, and one HTXF transfer chunk.
const SIZES: [usize; 3] = [128, 16 * 1024, 0xf000];
const KEY: [u8; 20] = *b"0123456789abcdefghij";

fn bench_blowfish(c: &mut Criterion) {
    let mut g = c.benchmark_group("blowfish_ofb64");
    for n in SIZES {
        let mut state = BlowfishOfb64State::new(&KEY).expect("valid key");
        let mut buf = vec![0x5au8; n];
        g.throughput(Throughput::Bytes(n as u64));
        g.bench_function(BenchmarkId::from_parameter(n), |b| {
            b.iter(|| state.crypt_in_place(black_box(&mut buf)));
        });
    }
    g.finish();
}

/// The raw block rate the OFB loop above is checked against: the same
/// number of block encryptions, with no per-byte XOR loop around them.
fn bench_blowfish_block(c: &mut Criterion) {
    let n = 0xf000;
    let bf: Blowfish = Blowfish::new_from_slice(&KEY).expect("valid key");
    let mut block = [0u8; BLOWFISH_OFB64_BLOCK_SIZE];
    let mut g = c.benchmark_group("blowfish_block");
    g.throughput(Throughput::Bytes(n as u64));
    g.bench_function(BenchmarkId::from_parameter(n), |b| {
        b.iter(|| {
            for _ in 0..n / BLOWFISH_OFB64_BLOCK_SIZE {
                #[allow(deprecated)]
                bf.encrypt_block(Array::from_mut_slice(&mut block));
            }
            black_box(block)
        });
    });
    g.finish();
}

/// The speculative-decode rollback: one save + restore per Hotline
/// transaction. Should be nanoseconds; if it isn't, the snapshot has
/// regressed to cloning the key schedule.
fn bench_blowfish_rollback(c: &mut Criterion) {
    let mut state = BlowfishOfb64State::new(&KEY).expect("valid key");
    let mut ivec = [0u8; BLOWFISH_OFB64_BLOCK_SIZE];
    let mut num = 0u32;
    c.bench_function("blowfish_rollback", |b| {
        b.iter(|| {
            state.save_ofb_state(&mut ivec, &mut num);
            state.restore_ofb_state(black_box(&ivec), black_box(num));
        });
    });
}

/// The HOPE rekey marker's worst case: 63 HMAC iterations and a new key
/// schedule, the shape of `hxnet::hope_blowfish::rotate_key_in_place`.
fn bench_hope_rekey(c: &mut Criterion) {
    let session_key = [0x42u8; 64];
    let mut g = c.benchmark_group("hope_rekey_63");
    for alg in ["HMAC-MD5", "HMAC-SHA1"] {
        let mut state = BlowfishOfb64State::new(&KEY).expect("valid key");
        g.bench_function(alg, |b| {
            b.iter(|| {
                let mut key = KEY.to_vec();
                let mut md = [0u8; 32];
                for _ in 0..63 {
                    let len = hmac_xxx(&mut md, &key, &session_key, alg) as usize;
                    key.clear();
                    key.extend_from_slice(&md[..len]);
                }
                black_box(state.set_key(&key))
            });
        });
    }
    g.finish();
}

fn aead_state() -> AeadState {
    AeadState {
        key: [7u8; 32],
        counter: 0,
        dir: AEAD_DIR_CLIENT_TO_SERVER,
    }
}

fn bench_aead(c: &mut Criterion) {
    let mut g = c.benchmark_group("aead_seal");
    for n in SIZES {
        let mut state = aead_state();
        let pt = vec![0x5au8; n];
        let mut out = vec![0u8; AEAD_LENGTH_PREFIX + n + AEAD_TAG_SIZE];
        g.throughput(Throughput::Bytes(n as u64));
        g.bench_function(BenchmarkId::from_parameter(n), |b| {
            b.iter(|| state.seal(black_box(&pt), &mut out).expect("sealed"));
        });
    }
    g.finish();

    let mut g = c.benchmark_group("aead_open");
    for n in SIZES {
        // Open needs a record sealed at the counter it expects, so seal a
        // run of records up front and open them in order, wrapping by
        // resetting both ends' counters together.
        const RECORDS: u64 = 64;
        let mut sealer = aead_state();
        let pt = vec![0x5au8; n];
        let frame = AEAD_LENGTH_PREFIX + n + AEAD_TAG_SIZE;
        let framed: Vec<Vec<u8>> = (0..RECORDS)
            .map(|_| {
                let mut out = vec![0u8; frame];
                sealer.seal(&pt, &mut out).expect("sealed");
                out
            })
            .collect();
        let mut opener = aead_state();
        let mut out = vec![0u8; n];
        g.throughput(Throughput::Bytes(n as u64));
        g.bench_function(BenchmarkId::from_parameter(n), |b| {
            b.iter(|| {
                let i = opener.counter % RECORDS;
                if i == 0 {
                    opener.counter = 0;
                }
                opener
                    .open(black_box(&framed[i as usize]), &mut out)
                    .expect("opened")
            });
        });
    }
    g.finish();
}

criterion_group!(
    benches,
    bench_blowfish,
    bench_blowfish_block,
    bench_blowfish_rollback,
    bench_hope_rekey,
    bench_aead
);
criterion_main!(benches);
