//! Unit tests ported from `tests/unit/test_voice_model.c`.
//!
//! Runs under plain `cargo test` — a `glib::subclass` GObject + its signal
//! work without GTK or a main loop. `play_sound` is stubbed here (the real
//! sound.c isn't linked into the test binary); the stub records each id so the
//! join/leave gating test can assert on the sequence.

use super::*;
use std::cell::RefCell;
use std::rc::Rc;

// ObjectExt (connect_local) — the emit-recording handler in install_recorder.
use glib::prelude::ObjectExt;

// ---- play_sound stub (lib.rs imports this under cfg(test)) ----------

thread_local! {
    static SOUND_CALLS: RefCell<Vec<c_int>> = const { RefCell::new(Vec::new()) };
}

/// # Safety
/// Trivially safe; `unsafe` only to match the real `extern "C"` signature the
/// non-test build links, so the call sites read identically.
pub(crate) unsafe fn play_sound(sound: c_int) {
    SOUND_CALLS.with(|c| c.borrow_mut().push(sound));
}

fn sound_reset() {
    SOUND_CALLS.with(|c| c.borrow_mut().clear());
}

fn sound_count(sound: c_int) -> usize {
    SOUND_CALLS.with(|c| c.borrow().iter().filter(|&&s| s == sound).count())
}

// ---- wire-blob helper (packed 6-byte BE records) --------------------

fn blob(ents: &[(u16, u16)]) -> Vec<u8> {
    let mut v = Vec::with_capacity(ents.len() * 6);
    for (uid, flags) in ents {
        v.extend_from_slice(&uid.to_be_bytes());
        v.extend_from_slice(&flags.to_be_bytes());
        v.extend_from_slice(&0u16.to_be_bytes()); // codec_id (PCMU)
    }
    v
}

// ---- signal recorder ------------------------------------------------

type Records = Rc<RefCell<Vec<(u32, u32)>>>;

fn install_recorder(m: &HxVoiceModel) -> Records {
    let recs: Records = Rc::new(RefCell::new(Vec::new()));
    let sink = recs.clone();
    m.connect_local("indicator-changed", false, move |args| {
        let uid = args[1].get::<u32>().unwrap();
        let ind = args[2].get::<u32>().unwrap();
        sink.borrow_mut().push((uid, ind));
        None
    });
    recs
}

fn saw(recs: &Records, uid: u32, ind: u32) -> bool {
    recs.borrow().iter().any(|&(u, i)| u == uid && i == ind)
}

// ---- tests ----------------------------------------------------------

#[test]
fn empty() {
    let m = HxVoiceModel::new();
    assert_eq!(m.get_indicator(0), INDICATOR_NONE);
    assert_eq!(m.get_indicator(12345), INDICATOR_NONE);
}

#[test]
fn ingest_basic() {
    let m = HxVoiceModel::new();
    m.ingest_participants(&blob(&[(13, 0x0000)]));
    assert_eq!(m.get_indicator(13), INDICATOR_IN_VOICE);
}

#[test]
fn ingest_muted() {
    let m = HxVoiceModel::new();
    m.ingest_participants(&blob(&[(13, 0x0001)]));
    assert_eq!(m.get_indicator(13), INDICATOR_MUTED);
}

#[test]
fn speaking_overlay() {
    let m = HxVoiceModel::new();
    m.ingest_participants(&blob(&[(13, 0x0000)]));

    // Above-threshold VAD reading → SPEAKING, and back to IN_VOICE.
    m.set_speaking(13, true);
    assert_eq!(m.get_indicator(13), INDICATOR_SPEAKING);
    m.set_speaking(13, false);
    assert_eq!(m.get_indicator(13), INDICATOR_IN_VOICE);

    // MUTED beats the speaking flag.
    m.ingest_participants(&blob(&[(13, 0x0001)]));
    m.set_speaking(13, true);
    assert_eq!(m.get_indicator(13), INDICATOR_MUTED);
}

#[test]
fn leavers_cleared() {
    let m = HxVoiceModel::new();
    m.ingest_participants(&blob(&[(13, 0), (14, 0)]));
    assert_eq!(m.get_indicator(13), INDICATOR_IN_VOICE);
    assert_eq!(m.get_indicator(14), INDICATOR_IN_VOICE);

    // 14 is gone; 13 remains.
    m.ingest_participants(&blob(&[(13, 0)]));
    assert_eq!(m.get_indicator(13), INDICATOR_IN_VOICE);
    assert_eq!(m.get_indicator(14), INDICATOR_NONE);

    // Empty blob — everyone's gone.
    m.ingest_participants(&[]);
    assert_eq!(m.get_indicator(13), INDICATOR_NONE);
}

#[test]
fn speaking_unknown_uid() {
    let m = HxVoiceModel::new();
    // No participants blob yet — set_speaking is a no-op.
    m.set_speaking(99, true);
    assert_eq!(m.get_indicator(99), INDICATOR_NONE);
}

#[test]
fn clear_resets_all() {
    let m = HxVoiceModel::new();
    m.ingest_participants(&blob(&[(13, 0), (14, 0x0001)]));
    m.set_speaking(13, true);

    m.clear();
    assert_eq!(m.get_indicator(13), INDICATOR_NONE);
    assert_eq!(m.get_indicator(14), INDICATOR_NONE);
}

#[test]
fn signal_emitted() {
    let m = HxVoiceModel::new();
    let recs = install_recorder(&m);

    // uid 13 joins → (13, IN_VOICE).
    m.ingest_participants(&blob(&[(13, 0)]));
    assert!(saw(&recs, 13, INDICATOR_IN_VOICE));

    // Same blob again: no flip, no emit.
    let prev = recs.borrow().len();
    m.ingest_participants(&blob(&[(13, 0)]));
    assert_eq!(recs.borrow().len(), prev);

    // Speaking flip: IN_VOICE → SPEAKING, exactly one emit.
    let prev = recs.borrow().len();
    m.set_speaking(13, true);
    assert_eq!(recs.borrow().len(), prev + 1);
    assert!(saw(&recs, 13, INDICATOR_SPEAKING));

    // Leaver sweep → (13, NONE).
    m.ingest_participants(&[]);
    assert!(saw(&recs, 13, INDICATOR_NONE));
}

#[test]
fn join_leave_sounds() {
    sound_reset();
    let m = HxVoiceModel::new();
    m.set_self_uid(13); // we are uid 13

    // Initial roster (us + a peer): seeds silently.
    m.ingest_participants(&blob(&[(13, 0), (14, 0)]));
    assert_eq!(sound_count(VOICE_JOIN), 0);
    assert_eq!(sound_count(VOICE_LEAVE), 0);

    // Peer 15 joins after the seed: one VOICE_JOIN.
    m.ingest_participants(&blob(&[(13, 0), (14, 0), (15, 0)]));
    assert_eq!(sound_count(VOICE_JOIN), 1);
    assert_eq!(sound_count(VOICE_LEAVE), 0);

    // Peer 15 leaves: one VOICE_LEAVE.
    sound_reset();
    m.ingest_participants(&blob(&[(13, 0), (14, 0)]));
    assert_eq!(sound_count(VOICE_JOIN), 0);
    assert_eq!(sound_count(VOICE_LEAVE), 1);

    // Our own leave is silent (self-exclusion).
    sound_reset();
    m.ingest_participants(&blob(&[(14, 0)]));
    assert_eq!(sound_count(VOICE_LEAVE), 0);

    // Our own re-join is likewise silent.
    sound_reset();
    m.ingest_participants(&blob(&[(13, 0), (14, 0)]));
    assert_eq!(sound_count(VOICE_JOIN), 0);

    // clear() re-arms the seed gate: next roster seeds silently.
    sound_reset();
    m.clear();
    m.ingest_participants(&blob(&[(13, 0), (14, 0), (16, 0)]));
    assert_eq!(sound_count(VOICE_JOIN), 0);
}
