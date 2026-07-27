//! Headless tests for the membership list model — gio only, no display. Pin
//! the `gio::ListModel` contract (item type, n_items, item) and the exact
//! `items-changed` (pos, removed, added) each mutation emits, so a bound view
//! diffs correctly.

use super::*;
use std::cell::RefCell;
use std::rc::Rc;

fn mem(uid: u16, name: &str) -> Member {
    Member::new(uid, name)
}

/// Attach an items-changed recorder; returns the shared log.
fn record(model: &HxMemberModel) -> Rc<RefCell<Vec<(u32, u32, u32)>>> {
    let log = Rc::new(RefCell::new(Vec::new()));
    let sink = log.clone();
    model.connect_items_changed(move |_, pos, removed, added| {
        sink.borrow_mut().push((pos, removed, added));
    });
    log
}

fn uid_at(model: &HxMemberModel, pos: u32) -> u16 {
    model
        .item(pos)
        .unwrap()
        .downcast::<HxMember>()
        .unwrap()
        .uid()
}

#[test]
fn item_type_is_hxmember() {
    let model = HxMemberModel::new();
    assert_eq!(model.item_type(), HxMember::static_type());
    assert_eq!(model.n_items(), 0);
}

#[test]
fn upsert_appends_and_signals() {
    let model = HxMemberModel::new();
    let log = record(&model);
    model.upsert(&mem(1, "alice"));
    model.upsert(&mem(2, "bob"));
    assert_eq!(model.n_items(), 2);
    assert_eq!(*log.borrow(), vec![(0, 0, 1), (1, 0, 1)]);
    let first = model.item(0).unwrap().downcast::<HxMember>().unwrap();
    assert_eq!(first.uid(), 1);
    assert_eq!(first.name(), "alice");
}

#[test]
fn upsert_existing_updates_in_place_same_object() {
    let model = HxMemberModel::new();
    model.upsert(&mem(1, "alice"));
    model.upsert(&mem(2, "bob"));
    let before = model.item(0).unwrap().downcast::<HxMember>().unwrap();
    let log = record(&model);

    // Watch the item's own "changed" signal — that's how the update is
    // announced, NOT via the model's items-changed.
    let item_changed = std::rc::Rc::new(std::cell::Cell::new(0u32));
    let ic = item_changed.clone();
    before.connect_closure(
        "changed",
        false,
        glib::closure_local!(move |_: HxMember| ic.set(ic.get() + 1)),
    );

    // Rename uid 1 — must update in place at position 0, not append.
    model.upsert(&mem(1, "alicia"));
    assert_eq!(model.n_items(), 2);
    // No structural change → the model emits no items-changed for an update.
    assert!(log.borrow().is_empty());
    // The item announced its own change exactly once.
    assert_eq!(item_changed.get(), 1);
    let after = model.item(0).unwrap();
    assert_eq!(after.clone().downcast::<HxMember>().unwrap().name(), "alicia");
    // In-place set kept the same GObject (identity + selection survive).
    assert_eq!(before.upcast::<glib::Object>(), after);
}

#[test]
fn upsert_preserves_position_on_update() {
    let model = HxMemberModel::new();
    for (u, n) in [(1, "a"), (2, "b"), (3, "c")] {
        model.upsert(&mem(u, n));
    }
    model.upsert(&mem(2, "bee")); // recolour/rename middle member
    assert_eq!(uid_at(&model, 0), 1);
    assert_eq!(uid_at(&model, 1), 2); // still in the middle
    assert_eq!(uid_at(&model, 2), 3);
    assert_eq!(model.get(2).unwrap().name(), "bee");
}

#[test]
fn upsert_update_is_reentrancy_safe() {
    let model = HxMemberModel::new();
    model.upsert(&mem(1, "alice"));
    model.upsert(&mem(2, "bob"));
    let item1 = model.item(0).unwrap().downcast::<HxMember>().unwrap();

    // A "changed" handler that re-enters the model with a *mutating* call.
    // remove() takes items.borrow_mut(); if upsert still held an items borrow
    // across set_from's signal emit, this would panic.
    let m = model.clone();
    item1.connect_closure(
        "changed",
        false,
        glib::closure_local!(move |_: HxMember| m.remove(2)),
    );

    model.upsert(&mem(1, "alicia")); // fires item1 "changed" → re-enters remove(2)
    assert_eq!(model.n_items(), 1);
    assert_eq!(uid_at(&model, 0), 1);
    assert_eq!(model.get(1).unwrap().name(), "alicia");
    assert!(model.get(2).is_none());
}

#[test]
fn remove_reindexes_and_signals() {
    let model = HxMemberModel::new();
    for (u, n) in [(10, "a"), (20, "b"), (30, "c")] {
        model.upsert(&mem(u, n));
    }
    let log = record(&model);
    model.remove(20);
    assert_eq!(model.n_items(), 2);
    assert_eq!(*log.borrow(), vec![(1, 1, 0)]);
    assert_eq!(uid_at(&model, 0), 10);
    assert_eq!(uid_at(&model, 1), 30);
    // The uid→index map still resolves survivors after the shift.
    assert_eq!(model.get(30).unwrap().name(), "c");
    assert!(model.get(20).is_none());
}

#[test]
fn remove_absent_is_noop() {
    let model = HxMemberModel::new();
    model.upsert(&mem(1, "a"));
    let log = record(&model);
    model.remove(999);
    assert!(log.borrow().is_empty());
    assert_eq!(model.n_items(), 1);
}

#[test]
fn clear_empties_and_signals_once() {
    let model = HxMemberModel::new();
    model.upsert(&mem(1, "a"));
    model.upsert(&mem(2, "b"));
    let log = record(&model);
    model.clear();
    assert_eq!(model.n_items(), 0);
    assert_eq!(*log.borrow(), vec![(0, 2, 0)]);
    // clear on an already-empty model emits nothing.
    model.clear();
    assert_eq!(*log.borrow(), vec![(0, 2, 0)]);
}

#[test]
fn member_carries_model_fields() {
    let model = HxMemberModel::new();
    let mut m = Member::new(7, "carol");
    m.icon = 42;
    m.status = 3;
    m.nick_color = Some(0x00abcdef);
    m.ignore = true;
    model.upsert(&m);
    let got = model.get(7).unwrap();
    assert_eq!(got.uid(), 7);
    assert_eq!(got.icon(), 42);
    assert_eq!(got.status(), 3);
    assert_eq!(got.nick_color(), Some(0x00abcdef));
    assert!(got.ignore());
    // Non-allocating name accessor borrows in place.
    assert_eq!(got.with_name(|n| n.len()), 5); // "carol"
    assert!(got.with_name(|n| n == "carol"));
}

#[test]
fn ignore_defaults_false_and_round_trips() {
    let model = HxMemberModel::new();
    model.upsert(&mem(1, "alice"));
    // A wire-created member starts un-ignored.
    assert!(!model.get_ignore(1));
    assert!(model.set_ignore(1, true));
    assert!(model.get_ignore(1));
    assert!(model.set_ignore(1, false));
    assert!(!model.get_ignore(1));
}

#[test]
fn ignore_survives_a_presence_update() {
    // The whole point of moving ignore onto the model: a USER_CHANGE
    // (rename/recolour → upsert of the same uid) must NOT clear it.
    let model = HxMemberModel::new();
    model.upsert(&mem(1, "alice"));
    model.set_ignore(1, true);
    model.upsert(&mem(1, "alicia")); // rename — in-place update
    assert_eq!(model.get(1).unwrap().name(), "alicia");
    assert!(model.get_ignore(1), "ignore cleared by a presence update");
}

#[test]
fn ignore_on_absent_uid_is_false_and_noop() {
    let model = HxMemberModel::new();
    assert!(!model.get_ignore(42));
    assert!(!model.set_ignore(42, true)); // no member → returns false
    assert!(!model.get_ignore(42));
    // toggle on an absent uid stays false (nothing to flip).
    assert!(!model.toggle_ignore(42));
}

#[test]
fn toggle_ignore_flips_and_returns_new_state() {
    let model = HxMemberModel::new();
    model.upsert(&mem(5, "dave"));
    assert!(model.toggle_ignore(5)); // false → true
    assert!(model.get_ignore(5));
    assert!(!model.toggle_ignore(5)); // true → false
    assert!(!model.get_ignore(5));
}

#[test]
fn ignore_dropped_when_member_removed_and_readded() {
    let model = HxMemberModel::new();
    model.upsert(&mem(1, "alice"));
    model.set_ignore(1, true);
    model.remove(1);
    model.upsert(&mem(1, "alice")); // a fresh join is a fresh member
    assert!(!model.get_ignore(1));
}
