//! `HxMember` + `HxMemberModel` — the membership `gio::ListModel` (Phase R5
//! M2; see `docs/rust/gchats-model-rethink.md`).
//!
//! `HxMemberModel` is the single source of truth for a chat's membership,
//! exposed as a `gio::ListModel` so the user list (`HxUserListView`) can bind
//! to it directly instead of being hand-populated by C `hx_user_list_view_add`
//! / `_remove` / `_update` calls. Each item is an `HxMember` GObject wrapping
//! the pure [`hxchat_model::Member`] data. Mutations (`upsert` / `remove` /
//! `clear`) mirror [`hxchat_model::MemberList`]'s uid-keyed, insertion-ordered
//! semantics and emit the correct `items-changed`, so a bound view diffs the
//! rows for free.
//!
//! This module is the M2 *foundation* — the model + its list-model surface,
//! unit-tested headless (gio only, no display). Binding `HxUserListView` to it
//! and pointing `rcv.c`'s membership updates at it is the next M2 increment.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;

use gio::prelude::*;
use gio::subclass::prelude::*;

use hxchat_model::{Member, NickColor};

// ---- HxMember: one member, as a GObject list item ------------------------

mod member_imp {
    use super::*;
    use glib::subclass::Signal;
    use std::sync::OnceLock;

    #[derive(Default)]
    pub struct HxMember {
        pub uid: Cell<u16>,
        pub icon: Cell<u16>,
        pub status: Cell<u16>,
        pub nick_color: Cell<NickColor>,
        pub name: RefCell<String>,
        pub ignore: Cell<bool>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for HxMember {
        const NAME: &'static str = "HxMember";
        type Type = super::HxMember;
        type ParentType = glib::Object;
    }

    impl ObjectImpl for HxMember {
        /// `"changed"` fires when the member's fields are updated in place (a
        /// rename / recolour), so a bound cell re-snapshots that one row while
        /// keeping the item's GObject identity (and thus its selection) —
        /// exactly how `HxUserRow` signals. The list model deliberately emits
        /// **no** `items-changed` for such an update: the list structure is
        /// unchanged, so a `(pos, 1, 1)` there would be a false remove+insert.
        fn signals() -> &'static [Signal] {
            static SIGNALS: OnceLock<Vec<Signal>> = OnceLock::new();
            SIGNALS.get_or_init(|| vec![Signal::builder("changed").build()])
        }
    }
}

glib::wrapper! {
    /// A chat member as a `gio::ListModel` item. Wraps [`hxchat_model::Member`]
    /// data; the display-row (`HxUserRow`) binds to it.
    pub struct HxMember(ObjectSubclass<member_imp::HxMember>);
}

impl HxMember {
    fn from_member(m: &Member) -> Self {
        let obj: Self = glib::Object::new();
        obj.store(m); // no "changed" on first construction (nothing listening)
        obj
    }

    /// Copy the member's fields into this object (no signal).
    fn store(&self, m: &Member) {
        let imp = self.imp();
        imp.uid.set(m.uid);
        imp.icon.set(m.icon);
        imp.status.set(m.status);
        imp.nick_color.set(m.nick_color);
        imp.name.replace(m.name.clone());
        imp.ignore.set(m.ignore);
    }

    /// Overwrite the member's fields in place (a rename / recolour) without
    /// creating a new object — keeping GObject identity + selection — then emit
    /// `"changed"` so a bound cell re-snapshots this one row.
    fn set_from(&self, m: &Member) {
        // An update must be for the same uid — uid is the model key + index.
        // A mismatched Member here would silently desync HxMemberModel's
        // uid→index map, so trap it in debug builds.
        debug_assert_eq!(
            self.imp().uid.get(),
            m.uid,
            "HxMember::set_from called with a different uid"
        );
        self.store(m);
        self.emit_by_name::<()>("changed", &[]);
    }

    pub fn uid(&self) -> u16 {
        self.imp().uid.get()
    }
    pub fn icon(&self) -> u16 {
        self.imp().icon.get()
    }
    pub fn status(&self) -> u16 {
        self.imp().status.get()
    }
    pub fn nick_color(&self) -> NickColor {
        self.imp().nick_color.get()
    }
    /// The name as an owned `String` (allocates). Prefer [`with_name`] on hot
    /// paths (sorting/compares, repeated cell snapshots).
    ///
    /// [`with_name`]: HxMember::with_name
    pub fn name(&self) -> String {
        self.imp().name.borrow().clone()
    }
    /// Borrow the name without allocating: `member.with_name(|n| …)`. The
    /// borrow is held only for the duration of `f`.
    pub fn with_name<R>(&self, f: impl FnOnce(&str) -> R) -> R {
        f(&self.imp().name.borrow())
    }
    pub fn ignore(&self) -> bool {
        self.imp().ignore.get()
    }
}

// ---- HxMemberModel: the membership list model ----------------------------

mod model_imp {
    use super::*;

    #[derive(Default)]
    pub struct HxMemberModel {
        /// Insertion-ordered items (mirrors `MemberList::order`).
        pub items: RefCell<Vec<HxMember>>,
        /// uid → index into `items` (mirrors `MemberList::index`).
        pub index: RefCell<HashMap<u16, usize>>,
    }

    #[glib::object_subclass]
    impl ObjectSubclass for HxMemberModel {
        const NAME: &'static str = "HxMemberModel";
        type Type = super::HxMemberModel;
        type ParentType = glib::Object;
        type Interfaces = (gio::ListModel,);
    }

    impl ObjectImpl for HxMemberModel {}

    impl ListModelImpl for HxMemberModel {
        fn item_type(&self) -> glib::Type {
            HxMember::static_type()
        }
        fn n_items(&self) -> u32 {
            self.items.borrow().len() as u32
        }
        fn item(&self, position: u32) -> Option<glib::Object> {
            self.items
                .borrow()
                .get(position as usize)
                .map(|m| m.clone().upcast())
        }
    }
}

glib::wrapper! {
    /// A chat's membership as a `gio::ListModel` of [`HxMember`]s.
    pub struct HxMemberModel(ObjectSubclass<model_imp::HxMemberModel>)
        @implements gio::ListModel;
}

impl Default for HxMemberModel {
    fn default() -> Self {
        Self::new()
    }
}

impl HxMemberModel {
    pub fn new() -> Self {
        glib::Object::new()
    }

    /// Insert a member, or update the existing one with the same uid *in
    /// place* (keeping its position + GObject identity — a rename/recolour must
    /// not reorder the list, matching `MemberList::upsert`).
    ///
    /// A new member emits `items-changed(pos, 0, 1)`. An in-place update emits
    /// **no** `items-changed` — the list structure is unchanged, so a
    /// `(pos, 1, 1)` there would be a false remove+insert that resets a bound
    /// row's state. Instead the item's own `"changed"` fires (see
    /// `HxMember::set_from`), which a bound cell watches to re-snapshot the row.
    pub fn upsert(&self, m: &Member) {
        let imp = self.imp();
        let existing = imp.index.borrow().get(&m.uid).copied();
        if let Some(i) = existing {
            // Clone the item out (a cheap GObject ref-bump) so the `items`
            // borrow guard is dropped BEFORE set_from emits "changed" — a
            // handler that re-enters the model (get / n_items / …) would
            // otherwise panic on the outstanding borrow.
            let item = imp.items.borrow()[i].clone();
            item.set_from(m);
        } else {
            let pos = imp.items.borrow().len();
            imp.items.borrow_mut().push(HxMember::from_member(m));
            imp.index.borrow_mut().insert(m.uid, pos);
            self.items_changed(pos as u32, 0, 1);
        }
    }

    /// Remove the member with `uid`, re-indexing the survivors (mirrors
    /// `MemberList::remove`). No-op if absent.
    pub fn remove(&self, uid: u16) {
        let imp = self.imp();
        let Some(pos) = imp.index.borrow_mut().remove(&uid) else {
            return;
        };
        imp.items.borrow_mut().remove(pos);
        {
            let items = imp.items.borrow();
            let mut idx = imp.index.borrow_mut();
            for (j, member) in items.iter().enumerate().skip(pos) {
                idx.insert(member.uid(), j);
            }
        }
        self.items_changed(pos as u32, 1, 0);
    }

    /// Drop every member (e.g. on a chat-users-clear / reconnect).
    pub fn clear(&self) {
        let imp = self.imp();
        let n = imp.items.borrow().len();
        if n == 0 {
            return;
        }
        imp.items.borrow_mut().clear();
        imp.index.borrow_mut().clear();
        self.items_changed(0, n as u32, 0);
    }

    /// The member with `uid`, if present.
    pub fn get(&self, uid: u16) -> Option<HxMember> {
        let imp = self.imp();
        let i = *imp.index.borrow().get(&uid)?;
        Some(imp.items.borrow()[i].clone())
    }
}

#[cfg(test)]
mod tests;
