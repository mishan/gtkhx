//! Unit tests ported from `tests/unit/test_voice_model.c`.
//!
//! Runs under plain `cargo test` — a `glib::subclass` GObject + its signal
//! work without GTK or a main loop. Presence chimes are no longer a direct
//! `play_sound` FFI call; the model emits the `voice-presence-chime` signal
//! instead, so the join/leave gating test records that signal and asserts on
//! the (uid, joined) sequence.

use super::*;
use std::cell::RefCell;
use std::rc::Rc;

// ObjectExt (connect_local) — the emit-recording handlers below.
use glib::prelude::ObjectExt;

// ---- voice-presence-chime recorder ---------------------------------

type Chimes = Rc<RefCell<Vec<(u32, bool)>>>;

/// Subscribe to `voice-presence-chime` and record every (uid, joined) pair.
fn install_chime_recorder(m: &HxVoiceModel) -> Chimes {
    let chimes: Chimes = Rc::new(RefCell::new(Vec::new()));
    let sink = chimes.clone();
    m.connect_local("voice-presence-chime", false, move |args| {
        let uid = args[1].get::<u32>().unwrap();
        let joined = args[2].get::<bool>().unwrap();
        sink.borrow_mut().push((uid, joined));
        None
    });
    chimes
}

/// Count recorded chimes matching `joined` (true = join, false = leave).
fn chime_count(chimes: &Chimes, joined: bool) -> usize {
    chimes.borrow().iter().filter(|&&(_, j)| j == joined).count()
}

fn chime_reset(chimes: &Chimes) {
    chimes.borrow_mut().clear();
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
    let m = HxVoiceModel::new();
    let chimes = install_chime_recorder(&m);
    m.set_self_uid(13); // we are uid 13

    // Initial roster (us + a peer): seeds silently.
    m.ingest_participants(&blob(&[(13, 0), (14, 0)]));
    assert_eq!(chime_count(&chimes, true), 0);
    assert_eq!(chime_count(&chimes, false), 0);

    // Peer 15 joins after the seed: one join chime.
    m.ingest_participants(&blob(&[(13, 0), (14, 0), (15, 0)]));
    assert_eq!(chime_count(&chimes, true), 1);
    assert_eq!(chime_count(&chimes, false), 0);

    // Peer 15 leaves: one leave chime.
    chime_reset(&chimes);
    m.ingest_participants(&blob(&[(13, 0), (14, 0)]));
    assert_eq!(chime_count(&chimes, true), 0);
    assert_eq!(chime_count(&chimes, false), 1);

    // Our own leave is silent (self-exclusion).
    chime_reset(&chimes);
    m.ingest_participants(&blob(&[(14, 0)]));
    assert_eq!(chime_count(&chimes, false), 0);

    // Our own re-join is likewise silent.
    chime_reset(&chimes);
    m.ingest_participants(&blob(&[(13, 0), (14, 0)]));
    assert_eq!(chime_count(&chimes, true), 0);

    // clear() re-arms the seed gate: next roster seeds silently.
    chime_reset(&chimes);
    m.clear();
    m.ingest_participants(&blob(&[(13, 0), (14, 0), (16, 0)]));
    assert_eq!(chime_count(&chimes, true), 0);
}
