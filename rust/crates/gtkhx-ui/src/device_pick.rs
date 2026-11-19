//! The device key a connect proves with: the picker the Connect dialog and
//! a saved connection's editor show, and the device armed for the open
//! (`hxnet::device::arm`), which takes it first.

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;

use gtk4 as gtk;
use gtk4::prelude::*;
use hxkeys::{Error, Fingerprint, Store};
use hxnet::device::Armed;
use libadwaita as adw;
use libadwaita::prelude::*;

use crate::options_device_keys::{pins_beside, plain_error};
use crate::tr::{tr, tr_argv};

/// A row choosing one of the identities whose device is kept here, or none.
#[derive(Clone)]
pub(crate) struct Picker {
    pub(crate) row: adw::ComboRow,
    /// Each choice's identity, in the order of the row's labels.
    choices: Rc<RefCell<Vec<Option<String>>>>,
}

impl Picker {
    /// The devices kept at `root`, and proving with none, chosen.
    pub(crate) fn new(root: &Path) -> Picker {
        let row = adw::ComboRow::new();
        row.set_title(&tr("Device key"));
        row.set_subtitle(&tr("Identity to prove on this server"));
        // The popover's rows take the width they need: a name cut short
        // there would hide the fingerprint that tells two names apart.
        let factory = gtk::SignalListItemFactory::new();
        factory.connect_setup(|_, item| {
            let Some(item) = item.downcast_ref::<gtk::ListItem>() else {
                return;
            };
            let label = gtk::Label::new(None);
            label.set_xalign(0.0);
            let check = gtk::Image::from_icon_name("object-select-symbolic");
            item.bind_property("selected", &check, "opacity")
                .transform_to(|_, selected: bool| Some(if selected { 1.0 } else { 0.0 }))
                .sync_create()
                .build();
            let line = gtk::Box::new(gtk::Orientation::Horizontal, 6);
            line.append(&label);
            line.append(&check);
            item.set_child(Some(&line));
        });
        factory.connect_bind(|_, item| {
            let Some(item) = item.downcast_ref::<gtk::ListItem>() else {
                return;
            };
            let label = item
                .child()
                .and_then(|line| line.first_child())
                .and_downcast::<gtk::Label>();
            let text = item.item().and_downcast::<gtk::StringObject>();
            if let (Some(label), Some(text)) = (label, text) {
                label.set_text(&text.string());
            }
        });
        row.set_list_factory(Some(&factory));
        // TRANSLATORS: the choice of a connection that proves no device.
        let labels = gtk::StringList::new(&[&tr("No device key")]);
        let mut choices = vec![None];
        for (fp, device) in Store::new(root).list().unwrap_or_default() {
            labels.append(&match device {
                Ok(d) => format!("{} ({})", d.card.name, fp.short()),
                // TRANSLATORS: %s is the start of an identity's fingerprint.
                Err(_) => tr_argv("%s (unreadable)", &[&fp.short()]),
            });
            choices.push(Some(fp.to_string()));
        }
        row.set_model(Some(&labels));
        Picker {
            row,
            choices: Rc::new(RefCell::new(choices)),
        }
    }

    pub(crate) fn chosen(&self) -> Option<String> {
        let at = self.row.selected() as usize;
        self.choices.borrow().get(at).cloned().flatten()
    }

    /// Choose `identity`. One whose device is no longer kept here stays
    /// chosen, said so, rather than being dropped from what it was saved
    /// with.
    pub(crate) fn choose(&self, identity: Option<&str>) {
        let found = self
            .choices
            .borrow()
            .iter()
            .position(|c| c.as_deref() == identity);
        let at = match (found, identity) {
            (Some(at), _) => at,
            (None, None) => 0,
            (None, Some(id)) => {
                let short: String = id.chars().take(8).collect();
                if let Some(labels) = self.row.model().and_downcast::<gtk::StringList>() {
                    // TRANSLATORS: %s is the start of an identity's fingerprint.
                    labels.append(&tr_argv("%s (missing)", &[&short]));
                }
                let mut choices = self.choices.borrow_mut();
                choices.push(Some(id.to_owned()));
                choices.len() - 1
            }
        };
        self.row.set_selected(at as u32);
    }
}

/// Run `open`, the connect to `host` and `port`, with `identity`'s device
/// kept at `root` armed for it, and nothing armed after, whether or not it
/// opened: a connect that returns before its open must leave nothing for
/// the next. Why the device could not be armed, in plain words.
pub(crate) fn around(
    root: &Path,
    identity: Option<&str>,
    host: &str,
    port: u16,
    open: impl FnOnce(),
) -> Result<(), String> {
    let armed = identity.map(|id| armed(root, id)).transpose();
    match &armed {
        Ok(Some(a)) => hxnet::device::arm(host, port, a.clone()),
        _ => hxnet::device::disarm(),
    }
    open();
    hxnet::device::disarm();
    armed.map(drop)
}

/// `identity`'s device kept at `root`, with the pins beside it.
fn armed(root: &Path, identity: &str) -> Result<Armed, String> {
    let device = Fingerprint::parse(identity)
        .ok_or(Error::NotFound)
        .and_then(|fp| Store::new(root).get(&fp))
        .map_err(|e| plain_error(&e))?;
    Ok(Armed {
        device,
        pins: pins_beside(root),
    })
}

#[cfg(test)]
#[path = "device_pick_tests.rs"]
pub(crate) mod tests;
