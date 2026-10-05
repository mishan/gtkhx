//! The browser's four dialogs: Rename, Move, New Folder and Delete.
//!
//! Each captures the names it acts on when it opens, not the rows: it's
//! asynchronous, and the listing can change underneath it.

use std::cell::RefCell;
use std::rc::Rc;

use gtk::gio;
use gtk::glib;
use gtk4 as gtk;
use hxmodel::files_entry::HxFileEntry;
use libadwaita as adw;
use libadwaita::prelude::*;

use super::browser::{send_move, Browser, Side, HL_ACCESS_MOVE_FILES};
use super::complete::PathComplete;
use super::panel::join;
use super::provider::{join_bytes, Provider};
use crate::tr::{tr, tr1, trn_argv};

/// An alert with Cancel and one other response, closing on Cancel.
fn alert(heading: &str, body: &str, id: &str, label: &str, destructive: bool) -> adw::AlertDialog {
    let dialog = adw::AlertDialog::new(Some(heading), Some(body));
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response(id, label);
    dialog.set_response_appearance(
        id,
        if destructive {
            adw::ResponseAppearance::Destructive
        } else {
            adw::ResponseAppearance::Suggested
        },
    );
    // A destructive answer is never the default.
    dialog.set_default_response(Some(if destructive { "cancel" } else { id }));
    dialog.set_close_response("cancel");
    unsafe {
        crate::ffi::gtkhx_dialog_add_close_shortcuts(dialog.upcast_ref::<gtk::Widget>().as_ptr())
    };
    dialog
}

/// An entry that Enter answers the dialog from.
fn text_entry(text: &str) -> gtk::Entry {
    let entry = gtk::Entry::new();
    entry.set_activates_default(true);
    entry.set_text(text);
    entry
}

/// Present `dialog` over the browser and put the cursor in `entry`, all of it
/// selected. The focus has to wait for the present: a dialog isn't realized
/// before it.
fn present_with(br: &Browser, dialog: &adw::AlertDialog, entry: &gtk::Entry, select: bool) {
    dialog.present(Some(br.content()));
    entry.grab_focus();
    if select {
        entry.select_region(0, -1);
    }
}

/// Rename the entry from a dialog: F2 and the header bar. Clicking a selected
/// name renames in place instead (`Panel`).
pub fn rename(br: &Rc<Browser>, side: Side, e: &HxFileEntry) {
    let (old, wire) = (e.name(), e.wire_name());
    let dialog = alert(
        &tr("Rename"),
        &tr1("Rename “%s” to:", &old),
        "rename",
        &tr("_Rename"),
        false,
    );
    let entry = text_entry(&old);
    dialog.set_extra_child(Some(&entry));
    let weak = Rc::downgrade(br);
    let field = entry.clone();
    dialog.connect_response(None, move |_, response| {
        let Some(br) = weak.upgrade().filter(|_| response == "rename") else {
            return;
        };
        let new = field.text();
        if new.is_empty() || new == old {
            return;
        }
        let Some(prov) = br.panel(side).provider() else {
            return;
        };
        if let Err(err) = prov.rename(&wire, &new) {
            let msg = err.message();
            br.toast(&if msg.is_empty() {
                tr("Rename failed.")
            } else {
                msg.to_owned()
            });
        }
    });
    present_with(br, &dialog, &entry, true);
}

/// Move the entries to another folder on the same side, defaulting to
/// `dest`'s, the other panel's. A remote move answers later — a refusal arrives as a task
/// error — so its toast says the move was requested; a local one is done by
/// the time the toast shows.
pub fn move_to(br: &Rc<Browser>, side: Side, entries: &[HxFileEntry], dest: &Provider) {
    let names: Vec<String> = entries.iter().map(HxFileEntry::name).collect();
    let wires: Vec<Vec<u8>> = entries.iter().map(HxFileEntry::wire_name).collect();
    let body = match names.as_slice() {
        [one] => tr1("Move “%s” to:", one),
        _ => {
            let n = names.len() as u64;
            trn_argv(
                "Move %u item to:",
                "Move %u items to:",
                n,
                &[&n.to_string()],
            )
        }
    };
    let dialog = alert(&tr("Move"), &body, "move", &tr("_Move"), false);
    let entry = text_entry(&dest.current_path());
    let dest_prov = dest.clone();
    dialog.set_extra_child(Some(&entry));

    let Some(prov) = br.panel(side).provider() else {
        return;
    };
    // Completion for a local destination only: a remote folder can't be
    // listed per keystroke. Dropped with the answer, while the entry lives.
    let complete = RefCell::new(prov.is_local().then(|| PathComplete::attach(&entry)));

    let weak = Rc::downgrade(br);
    let field = entry.clone();
    dialog.connect_response(None, move |_, response| {
        complete.replace(None);
        let Some(br) = weak.upgrade().filter(|_| response == "move") else {
            return;
        };
        let dest = field.text();
        if dest.is_empty() {
            return;
        }
        let (src_dir, dest_dir) = (prov.path(), dest_prov.encode_path(&dest));
        let (mut moved, mut last_err) = (0u64, None::<String>);
        for (name, wire) in names.iter().zip(&wires) {
            let result = if prov.is_local() {
                let (from, to) = (join(&prov.current_path(), name), join(&dest, name));
                gio::File::for_path(&from)
                    .move_(
                        &gio::File::for_path(&to),
                        gio::FileCopyFlags::NONE,
                        None::<&gio::Cancellable>,
                        None,
                    )
                    .map_err(|e| e.message().to_owned())
            } else {
                match prov.conn() {
                    Some(c) if !c.connected() => Err(tr("Not connected to a server.")),
                    None => Err(tr("Not connected to a server.")),
                    Some(c) if !c.access(HL_ACCESS_MOVE_FILES) => {
                        Err(tr("You don't have permission to move files on the server."))
                    }
                    Some(c) => {
                        send_move(
                            c.ptr(),
                            &join_bytes(&src_dir, wire),
                            &join_bytes(&dest_dir, wire),
                        );
                        Ok(())
                    }
                }
            };
            match result {
                Ok(()) => moved += 1,
                Err(e) => last_err = Some(e),
            }
        }
        prov.reload();
        let msg = match last_err {
            Some(e) => e,
            None if prov.is_remote() => trn_argv(
                "Move requested for %u item.",
                "Move requested for %u items.",
                moved,
                &[&moved.to_string()],
            ),
            None => trn_argv(
                "Moved %u item.",
                "Moved %u items.",
                moved,
                &[&moved.to_string()],
            ),
        };
        br.toast(&msg);
    });
    present_with(br, &dialog, &entry, true);
}

pub fn mkdir(br: &Rc<Browser>, side: Side) {
    let dialog = alert(
        &tr("New Folder"),
        &tr("Enter a name for the new folder."),
        "create",
        &tr("C_reate"),
        false,
    );
    let entry = text_entry("");
    dialog.set_extra_child(Some(&entry));
    let weak = Rc::downgrade(br);
    let field = entry.clone();
    dialog.connect_response(None, move |_, response| {
        let Some(br) = weak.upgrade().filter(|_| response == "create") else {
            return;
        };
        let name = field.text();
        if name.is_empty() {
            return;
        }
        if let Some(prov) = br.panel(side).provider() {
            if let Err(e) = prov.mkdir(&name) {
                glib::g_warning!("gtkhx", "mkdir failed: {e}");
            }
        }
    });
    present_with(br, &dialog, &entry, false);
}

pub fn delete(br: &Rc<Browser>, side: Side, entries: &[HxFileEntry]) {
    let names: Vec<String> = entries.iter().map(HxFileEntry::name).collect();
    // The name when there's one, so the user can check it; a count otherwise.
    let body = match names.as_slice() {
        [one] => tr1("Delete “%s”? This cannot be undone.", one),
        _ => {
            let n = names.len() as u64;
            trn_argv(
                "Delete %u item? This cannot be undone.",
                "Delete %u items? This cannot be undone.",
                n,
                &[&n.to_string()],
            )
        }
    };
    let dialog = alert(&tr("Delete"), &body, "delete", &tr("_Delete"), true);
    let entries = entries.to_vec();
    let weak = Rc::downgrade(br);
    dialog.connect_response(None, move |_, response| {
        let Some(br) = weak.upgrade().filter(|_| response == "delete") else {
            return;
        };
        let Some(prov) = br.panel(side).provider() else {
            return;
        };
        for e in &entries {
            if let Err(err) = prov.delete(e) {
                glib::g_warning!("gtkhx", "delete {}: {err}", e.name());
            }
        }
    });
    dialog.present(Some(br.content()));
}
