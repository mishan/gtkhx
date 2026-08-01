//! Settings dialog pages (porting `src/options.c` — the largest window — to
//! Rust, Phase R5.6).
//!
//! The values and the file belong to the `hxconfig` crate; the `changed_*`
//! apply hooks and the split-view `create_options_window` framework are still
//! C. This module builds the page *content* in Rust. Its
//! `gtkhx_options_rs_page_*` exports are the `settings_entries[].draw` pointers
//! the C framework calls per page; the row builders read/write preferences
//! through the typed by-name bridge (`gtkhx_prefs_*` in options.c), which
//! refreshes the C mirror, runs the key's apply hook and arms the save timer on
//! every write, so apply semantics match the C rows exactly.
//!
//! 10 of 11 pages are ported and live. Identity (icon + GIF-avatar pickers)
//! and Voice (device combos + PTT capture) keep their C draw functions for
//! now — porting them just swaps their `settings_entries[]` pointer for a Rust
//! export like the others.

use crate::tr::tr;
use crate::{cs, cstr};
use gtk4 as gtk;
use libadwaita as adw;

use adw::prelude::*;
use gtk::glib;
use std::cell::RefCell;
use std::os::raw::{c_char, c_int, c_void};
use std::rc::Rc;

extern "C" {
    // options.c pref bridge (typed by-name; a setter also refreshes the C
    // mirror, runs the key's apply hook and arms the save timer). See
    // src/options.h.
    fn gtkhx_prefs_type(name: *const c_char) -> c_int;
    fn gtkhx_prefs_get_bool(name: *const c_char) -> c_int;
    fn gtkhx_prefs_set_bool(name: *const c_char, val: c_int);
    fn gtkhx_prefs_get_int(name: *const c_char) -> c_int;
    fn gtkhx_prefs_set_int(name: *const c_char, val: c_int);
    fn gtkhx_prefs_get_string(name: *const c_char) -> *mut c_char;
    fn gtkhx_prefs_set_string(name: *const c_char, val: *const c_char);

    // gtkhx_theme.c — snapshot enumeration for the "GtkHx theme" combo.
    fn gtkhx_theme_names_begin() -> c_int;
    fn gtkhx_theme_names_name(i: c_int) -> *const c_char;
    fn gtkhx_theme_names_display(i: c_int) -> *const c_char;
    fn gtkhx_theme_names_end();
}

// Value-kind tags, as `hxconfig_type` reports them. A row that finds a
// mismatched kind renders insensitive.
const T_INT: c_int = 1;
const T_BOOLEAN: c_int = 2;
const T_STRING: c_int = 3;
const T_UINT16: c_int = 5;

/// Entry-row apply debounce — matches the C 750 ms coalesce so typing a
/// value (e.g. the nick pref, whose changefunc sends a wire packet) doesn't
/// fire one apply per keystroke.
const ENTRY_APPLY_DEBOUNCE_MS: u64 = 750;

// ---- typed pref helpers ------------------------------------------------

fn pref_type(name: &str) -> c_int {
    unsafe { gtkhx_prefs_type(cs(name).as_ptr()) }
}
fn pref_get_bool(name: &str) -> bool {
    unsafe { gtkhx_prefs_get_bool(cs(name).as_ptr()) != 0 }
}
fn pref_set_bool(name: &str, v: bool) {
    unsafe { gtkhx_prefs_set_bool(cs(name).as_ptr(), v as c_int) }
}
fn pref_get_int(name: &str) -> i32 {
    unsafe { gtkhx_prefs_get_int(cs(name).as_ptr()) }
}
fn pref_set_int(name: &str, v: i32) {
    unsafe { gtkhx_prefs_set_int(cs(name).as_ptr(), v) }
}
fn pref_get_string(name: &str) -> String {
    unsafe {
        let p = gtkhx_prefs_get_string(cs(name).as_ptr());
        if p.is_null() {
            return String::new();
        }
        let s = cstr(p);
        glib::ffi::g_free(p as *mut c_void);
        s
    }
}
fn pref_set_string(name: &str, v: &str) {
    unsafe { gtkhx_prefs_set_string(cs(name).as_ptr(), cs(v).as_ptr()) }
}

// ---- row builders ------------------------------------------------------
//
// Each mirrors the matching pref_*_row in options.c: initialise from the
// pref's current value, render insensitive on a type mismatch, and write
// back (which applies + persists) on change.

/// AdwSwitchRow bound to a BOOLEAN pref.
fn switch_row(cfg: &str, title: &str, subtitle: Option<&str>) -> adw::SwitchRow {
    let row = adw::SwitchRow::new();
    row.set_title(title);
    if let Some(s) = subtitle {
        if !s.is_empty() {
            row.set_subtitle(s);
        }
    }
    if pref_type(cfg) != T_BOOLEAN {
        row.set_sensitive(false);
        return row;
    }
    row.set_active(pref_get_bool(cfg));
    let name = cfg.to_owned();
    row.connect_active_notify(move |r| pref_set_bool(&name, r.is_active()));
    row
}

/// AdwSpinRow bound to an INT / UINT16 pref.
fn spin_row(
    cfg: &str,
    title: &str,
    subtitle: Option<&str>,
    min: f64,
    max: f64,
    step: f64,
) -> adw::SpinRow {
    let row = adw::SpinRow::with_range(min, max, step);
    row.set_title(title);
    if let Some(s) = subtitle {
        if !s.is_empty() {
            row.set_subtitle(s);
        }
    }
    let t = pref_type(cfg);
    if t != T_INT && t != T_UINT16 {
        row.set_sensitive(false);
        return row;
    }
    row.set_value(pref_get_int(cfg) as f64);
    let name = cfg.to_owned();
    row.connect_value_notify(move |r| {
        // Only write when the stored int actually changes — the setter
        // short-circuits an unchanged value anyway, but checking here also
        // skips the FFI round trip on a property that churns.
        let v = r.value() as i32;
        if v != pref_get_int(&name) {
            pref_set_int(&name, v);
        }
    });
    row
}

/// AdwEntryRow bound to a STRING pref, with the 750 ms apply debounce (a
/// per-row timer captured in the change closure).
fn entry_row(cfg: &str, title: &str) -> adw::EntryRow {
    let row = adw::EntryRow::new();
    row.set_title(title);
    if pref_type(cfg) != T_STRING {
        row.set_sensitive(false);
        return row;
    }
    row.set_text(&pref_get_string(cfg));

    let name = cfg.to_owned();
    // Debounce state: the pending timer + the latest text. Keeping the text
    // here lets the teardown handler flush the last edit (Settings closed
    // mid-keystroke) instead of dropping it — the Rust equivalent of the C
    // close_options_bookkeeping timer flush.
    let deb: Rc<RefCell<(Option<glib::SourceId>, String)>> =
        Rc::new(RefCell::new((None, String::new())));
    {
        let name = name.clone();
        let deb = deb.clone();
        row.connect_changed(move |r| {
            let text = r.text().to_string();
            let mut d = deb.borrow_mut();
            if let Some(id) = d.0.take() {
                id.remove();
            }
            d.1 = text;
            let name = name.clone();
            let deb2 = deb.clone();
            let id = glib::timeout_add_local_once(
                std::time::Duration::from_millis(ENTRY_APPLY_DEBOUNCE_MS),
                move || {
                    let text = {
                        let mut d = deb2.borrow_mut();
                        d.0 = None;
                        d.1.clone()
                    };
                    pref_set_string(&name, &text);
                },
            );
            d.0 = Some(id);
        });
    }
    // Flush any pending edit when the row is torn down (dialog closed).
    {
        let name = name.clone();
        let deb = deb.clone();
        row.connect_unrealize(move |_| {
            let mut d = deb.borrow_mut();
            if let Some(id) = d.0.take() {
                id.remove();
                let text = d.1.clone();
                drop(d);
                pref_set_string(&name, &text);
            }
        });
    }
    row
}

/// AdwComboRow over a fixed value list bound to a STRING pref. `labels` are
/// shown; the parallel `values` are what's stored. Selection maps by value.
fn combo_row(cfg: &str, title: &str, values: &[&str], labels: &[&str]) -> adw::ComboRow {
    let row = adw::ComboRow::new();
    row.set_title(title);
    // Values and labels are parallel; clamp to the shorter so a selection
    // index always maps to a stored value (a mismatch would otherwise let the
    // user pick a label with no value → silent no-op). Empty → insensitive.
    let n = values.len().min(labels.len());
    if pref_type(cfg) != T_STRING || n == 0 {
        row.set_sensitive(false);
        return row;
    }
    let values = &values[..n];
    let labels = &labels[..n];
    row.set_model(Some(&gtk::StringList::new(labels)));
    let current = pref_get_string(cfg);
    let sel = values.iter().position(|v| *v == current).unwrap_or(0) as u32;
    row.set_selected(sel);

    let name = cfg.to_owned();
    let values: Vec<String> = values.iter().map(|s| (*s).to_owned()).collect();
    row.connect_selected_notify(move |r| {
        if let Some(v) = values.get(r.selected() as usize) {
            pref_set_string(&name, v);
        }
    });
    row
}

/// AdwActionRow with a GtkColorDialogButton + Clear, bound to an INT pref
/// packed as 0x00RRGGBB (−1 = unset). Mirrors pref_nick_color_row.
// Used only by the Identity page, which is still C — unused until that page
// is ported to Rust.
#[allow(dead_code)]
fn nick_color_row(cfg: &str) -> adw::ActionRow {
    let row = adw::ActionRow::new();
    row.set_title(&tr("Nickname color"));
    row.set_subtitle(&tr(
        "Optional RGB color shown on servers that support the Colored-Nicknames \
         extension",
    ));
    if pref_type(cfg) != T_INT {
        row.set_sensitive(false);
        return row;
    }

    let dialog = gtk::ColorDialog::new();
    dialog.set_title(&tr("Pick Nickname Color"));
    let picker = gtk::ColorDialogButton::new(Some(dialog));
    picker.set_valign(gtk::Align::Center);

    let cur = pref_get_int(cfg);
    if cur != -1 {
        let p = cur as u32;
        picker.set_rgba(&gtk::gdk::RGBA::new(
            ((p >> 16) & 0xff) as f32 / 255.0,
            ((p >> 8) & 0xff) as f32 / 255.0,
            (p & 0xff) as f32 / 255.0,
            1.0,
        ));
    }

    let name = cfg.to_owned();
    let handler = {
        let name = name.clone();
        picker.connect_rgba_notify(move |b| {
            let c = b.rgba();
            let r = (c.red() * 255.0 + 0.5) as u32;
            let g = (c.green() * 255.0 + 0.5) as u32;
            let bl = (c.blue() * 255.0 + 0.5) as u32;
            // Only write when the packed 0x00RRGGBB changes — the rgba property
            // can churn (rounding) without the stored value moving, and the C
            // row guards the same way to skip a redundant apply.
            let packed = ((r << 16) | (g << 8) | bl) as i32;
            if packed != pref_get_int(&name) {
                pref_set_int(&name, packed);
            }
        })
    };

    let clear = gtk::Button::with_label(&tr("Clear"));
    clear.set_valign(gtk::Align::Center);
    {
        let picker = picker.clone();
        clear.connect_clicked(move |_| {
            // Already unset — nothing to apply (matches the C early return).
            if pref_get_int(&name) == -1 {
                return;
            }
            pref_set_int(&name, -1);
            // Reset the swatch to black without re-packing 0x000000 into the
            // pref (block the rgba handler around the synthetic set).
            picker.block_signal(&handler);
            picker.set_rgba(&gtk::gdk::RGBA::new(0.0, 0.0, 0.0, 1.0));
            picker.unblock_signal(&handler);
        });
    }

    row.add_suffix(&picker);
    row.add_suffix(&clear);
    row
}

// ---- config keys (mirror src/cfgkeys.h) --------------------------------

mod cfg {
    // Chat output / appearance
    pub const TIMESTAMP: &str = "TIMESTAMP";
    pub const CHAT_AVATARS: &str = "CHATAVATARS";
    pub const MARKDOWN: &str = "MARKDOWN";
    pub const WORDWRAP: &str = "WORDWRAP";
    pub const XBUF_MAX: &str = "XBUF_MAX";
    pub const STAMP_FORMAT: &str = "TIMESTAMPFORMAT";
    pub const FONT: &str = "FONT";
    // Appearance / general
    pub const THEME: &str = "THEME";
    pub const THEME_SYSTEM: &str = "system";
    pub const THEME_LIGHT: &str = "light";
    pub const THEME_DARK: &str = "dark";
    pub const THEME_NAME: &str = "THEMENAME";
    pub const TRAY: &str = "TRAY";
    // Chat behaviour / output
    pub const SHOWJOIN: &str = "SHOWJOIN";
    pub const OLD_NICKCOMP: &str = "OLD_NICKCOMPLETION";
    pub const AUTOCOPY_TEXT: &str = "AUTOCOPYTEXT";
    pub const AUTOCOPY_STAMP: &str = "AUTOCOPYSTAMP";
    pub const AUTOCOPY_COLOR: &str = "AUTOCOPYCOLOR";
    pub const HIGHLIGHT_WORDS: &str = "HIGHLIGHTWORDS";
    pub const CHAT_HISTORY_INITIAL: &str = "CHATHISTORYINITIAL";
    pub const EMOJI_SHORTCODES: &str = "EMOJISHORTCODES";
    pub const EMOJI_TYPEAHEAD: &str = "EMOJITYPEAHEAD";
    // Sounds
    pub const SOUNDS_ON: &str = "SOUNDSON";
    pub const SND_INVITE: &str = "SOUNDINVITE";
    pub const SND_CHAT: &str = "SOUNDCHAT";
    pub const SND_ERROR: &str = "SOUNDERROR";
    pub const SND_FILE: &str = "SOUNDFILE";
    pub const SND_JOIN: &str = "SOUNDJOIN";
    pub const SND_LOGIN: &str = "SOUNDLOGIN";
    pub const SND_MSG: &str = "SOUNDMSG";
    pub const SND_NEWS: &str = "SOUNDNEWS";
    pub const SND_PART: &str = "SOUNDPART";
    pub const SND_VOICE_JOIN: &str = "SOUNDVOICEJOIN";
    pub const SND_VOICE_LEAVE: &str = "SOUNDVOICELEAVE";
    // Paths / transfers
    pub const DOWNLOAD: &str = "DOWNLOAD";
    pub const QUEUEDL: &str = "QUEUEDL";
    // Trackers (comma-separated list string; CFG_TRACKER's changefunc
    // re-parses it into gtkhx_prefs.tracker[]).
    pub const TRACKER: &str = "TRACKER";
    pub const TRACKER_CASE: &str = "TRACKER_CASE";
    // Notifications
    pub const NOTIFY_MSG: &str = "NOTIFYMSG";
    pub const NOTIFY_PCHAT_INVITE: &str = "NOTIFYPCHATINVITE";
    pub const NOTIFY_CHAT_HIGHLIGHT: &str = "NOTIFYCHATHIGHLIGHT";
    pub const NOTIFY_PCHAT_HIGHLIGHT: &str = "NOTIFYPCHATHIGHLIGHT";
    pub const NOTIFY_CHAT: &str = "NOTIFYCHAT";
    pub const NOTIFY_PCHAT: &str = "NOTIFYPCHAT";
    pub const NOTIFY_NEWS: &str = "NOTIFYNEWS";
    pub const NOTIFY_XFER: &str = "NOTIFYXFER";
    pub const NOTIFY_BROADCAST: &str = "NOTIFYBROADCAST";
    pub const NOTIFY_OMIT_FOCUSED: &str = "NOTIFYOMITFOCUSED";
}

/// Convenience: a titled AdwPreferencesGroup.
fn group(title: &str) -> adw::PreferencesGroup {
    let g = adw::PreferencesGroup::new();
    g.set_title(title);
    g
}

// ---- pages (pure builder sequences) ------------------------------------
//
// Each mirrors the matching settings_page_* draw function in options.c.
// Signature matches the C `void (*draw)(AdwPreferencesPage *)` so the page
// table can call them uniformly once create_options_window lands.

/// Open a font chooser seeded from the FONT pref and write the picked font
/// description back into `entry` (which applies via the debounce).
fn font_browse(entry: &adw::EntryRow, anchor: &impl IsA<gtk::Widget>) {
    let dialog = gtk::FontDialog::new();
    dialog.set_title(&tr("Browse Fonts"));
    let parent = anchor.root().and_downcast::<gtk::Window>();
    let cur = pref_get_string(cfg::FONT);
    let initial = (!cur.is_empty()).then(|| gtk::pango::FontDescription::from_string(&cur));
    let entry = entry.clone();
    dialog.choose_font(
        parent.as_ref(),
        initial.as_ref(),
        gtk::gio::Cancellable::NONE,
        move |res| {
            if let Ok(desc) = res {
                entry.set_text(&desc.to_str());
            }
        },
    );
}

/// The "GtkHx theme" combo, populated from the C theme-list snapshot.
fn gtkhx_theme_combo() -> adw::ComboRow {
    let (values, labels): (Vec<String>, Vec<String>) = unsafe {
        // Clamp defensively: the C side already caps at G_MAXINT, but never
        // let a negative count become a huge with_capacity / range.
        let n = gtkhx_theme_names_begin().max(0);
        let mut v = Vec::with_capacity(n as usize);
        let mut l = Vec::with_capacity(n as usize);
        for i in 0..n {
            v.push(cstr(gtkhx_theme_names_name(i)));
            l.push(cstr(gtkhx_theme_names_display(i)));
        }
        gtkhx_theme_names_end();
        (v, l)
    };
    let vrefs: Vec<&str> = values.iter().map(String::as_str).collect();
    let lrefs: Vec<&str> = labels.iter().map(String::as_str).collect();
    combo_row(cfg::THEME_NAME, &tr("GtkHx theme"), &vrefs, &lrefs)
}

/// General (Appearance theme combos + tray).
fn page_general(page: &adw::PreferencesPage) {
    let appearance = group(&tr("Appearance"));
    appearance.set_description(Some(&tr(
        "Color scheme. \"Follow system\" tracks the desktop's light/dark preference.",
    )));
    let theme_labels = [tr("Follow system"), tr("Light"), tr("Dark")];
    appearance.add(&combo_row(
        cfg::THEME,
        &tr("Theme"),
        &[cfg::THEME_SYSTEM, cfg::THEME_LIGHT, cfg::THEME_DARK],
        &[
            theme_labels[0].as_str(),
            theme_labels[1].as_str(),
            theme_labels[2].as_str(),
        ],
    ));
    appearance.add(&gtkhx_theme_combo());
    page.add(&appearance);

    let system = group(&tr("System Integration"));
    system.add(&switch_row(
        cfg::TRAY,
        &tr("Show tray icon"),
        Some(&tr(
            "Display a status icon in the system tray. Closing the main window \
             hides to tray; click the icon to toggle GtkHx's windows.",
        )),
    ));
    page.add(&system);
}

/// Chat → Appearance (output toggles + scrollback + timestamp format + font).
fn page_chat_appearance(page: &adw::PreferencesPage) {
    let output = group(&tr("Chat output"));
    output.add(&switch_row(cfg::TIMESTAMP, &tr("Show timestamps"), None));
    output.add(&switch_row(
        cfg::CHAT_AVATARS,
        &tr("Show user icons"),
        Some(&tr(
            "Shows the speaker's icon beside the first message of each run",
        )),
    ));
    output.add(&switch_row(
        cfg::MARKDOWN,
        &tr("Render markdown"),
        Some(&tr(
            "Formats **bold**, *italic*, `code`, quotes and links in received \
             messages. What you send is unchanged.",
        )),
    ));
    output.add(&switch_row(cfg::WORDWRAP, &tr("Word wrap"), None));
    output.add(&spin_row(
        cfg::XBUF_MAX,
        &tr("Scrollback lines"),
        Some(&tr("0 keeps unlimited scrollback")),
        0.0,
        0xffff as f64,
        1.0,
    ));
    page.add(&output);

    let stamp = group(&tr("Timestamp format"));
    stamp.set_description(Some(&tr(
        "strftime(3) format string. Default: \"[%H:%M:%S] \". See `man 3 \
         strftime` for the full list of conversion specifiers.",
    )));
    stamp.add(&entry_row(cfg::STAMP_FORMAT, &tr("Format")));
    page.add(&stamp);

    let font_grp = group(&tr("Font"));
    font_grp.set_description(Some(&tr("Pango font description, e.g. \"Monospace 11\"")));
    let font_entry = entry_row(cfg::FONT, &tr("Font"));
    let browse = gtk::Button::with_label(&tr("Browse"));
    browse.set_valign(gtk::Align::Center);
    {
        let font_entry = font_entry.clone();
        browse.connect_clicked(move |b| font_browse(&font_entry, b));
    }
    font_entry.add_suffix(&browse);
    font_grp.add(&font_entry);
    page.add(&font_grp);
}

/// Audio → Sound.
fn page_sound(page: &adw::PreferencesPage) {
    let master = group(&tr("Sounds"));
    master.add(&switch_row(
        cfg::SOUNDS_ON,
        &tr("Play sounds"),
        Some(&tr("Master switch for chat and transfer alerts")),
    ));
    page.add(&master);

    let events = group(&tr("Events"));
    events.add(&switch_row(cfg::SND_INVITE, &tr("Chat invitation"), None));
    events.add(&switch_row(cfg::SND_CHAT, &tr("Chat message"), None));
    events.add(&switch_row(cfg::SND_ERROR, &tr("Error"), None));
    events.add(&switch_row(cfg::SND_FILE, &tr("Transfer complete"), None));
    events.add(&switch_row(cfg::SND_JOIN, &tr("Join"), None));
    events.add(&switch_row(cfg::SND_LOGIN, &tr("Login"), None));
    events.add(&switch_row(cfg::SND_MSG, &tr("Private message"), None));
    events.add(&switch_row(cfg::SND_NEWS, &tr("News post"), None));
    events.add(&switch_row(cfg::SND_PART, &tr("Leave"), None));
    // Voice chimes only make sense when voice is compiled in, so the rows are
    // gated on this crate's `voice` feature — the same switch that compiles
    // the C voice sources. Gating on whether the setting exists would never
    // fire: the two keys are registered unconditionally, deliberately, so a
    // no-voice build doesn't discard a user's saved toggles.
    if cfg!(feature = "voice") {
        events.add(&switch_row(
            cfg::SND_VOICE_JOIN,
            &tr("Voice chat join"),
            None,
        ));
        events.add(&switch_row(
            cfg::SND_VOICE_LEAVE,
            &tr("Voice chat leave"),
            None,
        ));
    }
    page.add(&events);
}

/// Chat → Behavior.
fn page_chat_behavior(page: &adw::PreferencesPage) {
    let behavior = group(&tr("Behavior"));
    behavior.add(&switch_row(
        cfg::SHOWJOIN,
        &tr("Show join / leave in chat"),
        None,
    ));
    behavior.add(&switch_row(
        cfg::OLD_NICKCOMP,
        &tr("Old-style nick completion"),
        Some(&tr(
            "Match against the most recently typed prefix instead of all users",
        )),
    ));
    page.add(&behavior);

    let autocopy = group(&tr("Auto Copy Behavior"));
    autocopy.set_description(Some(&tr(
        "Drag-select in chat / news / private message text to populate the \
         clipboard. Ctrl-V or middle-click pastes the selection elsewhere.",
    )));
    autocopy.add(&switch_row(
        cfg::AUTOCOPY_TEXT,
        &tr("Automatically copy selected text"),
        None,
    ));
    autocopy.add(&switch_row(
        cfg::AUTOCOPY_STAMP,
        &tr("Automatically include timestamps"),
        None,
    ));
    autocopy.add(&switch_row(
        cfg::AUTOCOPY_COLOR,
        &tr("Automatically include color information"),
        None,
    ));
    page.add(&autocopy);

    let hl = group(&tr("Highlight"));
    hl.set_description(Some(&tr(
        "Comma-separated words to highlight in chat (in addition to your own \
         nick). Matches are case-insensitive at word boundaries.",
    )));
    hl.add(&entry_row(cfg::HIGHLIGHT_WORDS, &tr("Words")));
    page.add(&hl);
}

/// Chat → History.
fn page_chat_history(page: &adw::PreferencesPage) {
    let hist = group(&tr("Chat history"));
    hist.set_description(Some(&tr(
        "Servers that implement the chat-history extension (e.g. Janus) replay \
         recent chat to you on login. 0 disables the initial pull; the \"Load \
         older messages\" link in chat still works to fetch on demand.",
    )));
    hist.add(&spin_row(
        cfg::CHAT_HISTORY_INITIAL,
        &tr("Initial messages to fetch"),
        Some(&tr(
            "Also used as the page size for \"Load older messages\"",
        )),
        0.0,
        0xffff as f64,
        1.0,
    ));
    page.add(&hist);
}

/// Chat → Emoji.
fn page_chat_emoji(page: &adw::PreferencesPage) {
    let emoji = group(&tr("Emoji"));
    emoji.set_description(Some(&tr(
        "When conversion is on, emoji you send on servers that don't support \
         Unicode go out as text shortcodes like \":joy:\" instead of \"?\", and \
         incoming shortcodes are shown as emoji on every server.",
    )));
    emoji.add(&switch_row(
        cfg::EMOJI_SHORTCODES,
        &tr("Convert emoji to/from :shortcodes:"),
        None,
    ));
    emoji.add(&switch_row(
        cfg::EMOJI_TYPEAHEAD,
        &tr("Suggest shortcodes as you type"),
        Some(&tr("Show a popup of matching emoji when you type \":\"")),
    ));
    page.add(&emoji);
}

/// Notifications → Events.
fn page_notify_events(page: &adw::PreferencesPage) {
    let events = group(&tr("Events"));
    events.set_description(Some(&tr(
        "Show a desktop notification when these events happen.",
    )));
    events.add(&switch_row(
        cfg::NOTIFY_MSG,
        &tr("Private message"),
        Some(&tr("Someone sends you a 1-to-1 message")),
    ));
    events.add(&switch_row(
        cfg::NOTIFY_PCHAT_INVITE,
        &tr("Private chat invitation"),
        Some(&tr("Someone invites you to a private chat")),
    ));
    events.add(&switch_row(
        cfg::NOTIFY_CHAT_HIGHLIGHT,
        &tr("Mention in public chat"),
        Some(&tr(
            "Your name or a highlight word appears in a chat message",
        )),
    ));
    events.add(&switch_row(
        cfg::NOTIFY_PCHAT_HIGHLIGHT,
        &tr("Mention in private chat"),
        Some(&tr(
            "Your name or a highlight word appears in a private chat",
        )),
    ));
    events.add(&switch_row(
        cfg::NOTIFY_CHAT,
        &tr("Every public chat message"),
        Some(&tr("Noisy — only useful on quiet servers")),
    ));
    events.add(&switch_row(
        cfg::NOTIFY_PCHAT,
        &tr("Every private chat message"),
        None,
    ));
    events.add(&switch_row(cfg::NOTIFY_NEWS, &tr("New news post"), None));
    events.add(&switch_row(
        cfg::NOTIFY_XFER,
        &tr("File transfer complete"),
        None,
    ));
    events.add(&switch_row(
        cfg::NOTIFY_BROADCAST,
        &tr("Server broadcast"),
        Some(&tr("Admin-issued announcement to every user")),
    ));
    page.add(&events);
}

/// General → File Transfers.
fn page_file_transfers(page: &adw::PreferencesPage) {
    let grp = group(&tr("Downloads"));
    grp.add(&entry_row(cfg::DOWNLOAD, &tr("Download directory")));
    grp.add(&switch_row(
        cfg::QUEUEDL,
        &tr("Queue file transfers"),
        Some(&tr("Run downloads one at a time instead of in parallel")),
    ));
    page.add(&grp);
}

/// Serialise the tracker list-store back to the comma-separated CFG_TRACKER
/// value. The schema stores a real array; the comma is a boundary format the
/// setter splits on, and the write re-derives the C mirror's `tracker[]` and
/// persists.
fn persist_trackers(store: &gtk::gio::ListStore) {
    let mut parts: Vec<String> = Vec::new();
    for i in 0..store.n_items() {
        if let Some(o) = store.item(i).and_downcast::<gtk::StringObject>() {
            parts.push(o.string().to_string());
        }
    }
    pref_set_string(cfg::TRACKER, &parts.join(","));
}

/// Network → Trackers: an editable list of tracker URLs (GtkColumnView over a
/// GListStore) + Add/Remove + the case-sensitive-search switch.
fn page_tracker(page: &adw::PreferencesPage) {
    let grp = group(&tr("Trackers"));
    grp.set_description(Some(&tr("Servers polled when the Tracker window opens")));

    let store = gtk::gio::ListStore::new::<gtk::StringObject>();
    for t in pref_get_string(cfg::TRACKER)
        .split(',')
        .filter(|s| !s.is_empty())
    {
        store.append(&gtk::StringObject::new(t));
    }
    let selection = gtk::SingleSelection::new(Some(store.clone()));
    selection.set_autoselect(false);
    selection.set_can_unselect(true);
    selection.set_selected(gtk::INVALID_LIST_POSITION);

    let colview = gtk::ColumnView::new(Some(selection.clone()));
    colview.set_show_column_separators(false);
    colview.set_show_row_separators(false);
    let factory = gtk::SignalListItemFactory::new();
    factory.connect_setup(|_, item| {
        let item = item.downcast_ref::<gtk::ListItem>().unwrap();
        let lbl = gtk::Label::new(None);
        lbl.set_xalign(0.0);
        lbl.set_ellipsize(gtk::pango::EllipsizeMode::End);
        lbl.set_margin_start(6);
        lbl.set_margin_end(6);
        item.set_child(Some(&lbl));
    });
    factory.connect_bind(|_, item| {
        let item = item.downcast_ref::<gtk::ListItem>().unwrap();
        if let Some(lbl) = item.child().and_downcast::<gtk::Label>() {
            let s = item
                .item()
                .and_downcast::<gtk::StringObject>()
                .map(|o| o.string())
                .unwrap_or_default();
            lbl.set_text(&s);
        }
    });
    let col = gtk::ColumnViewColumn::new(Some(&tr("URL")), Some(factory));
    col.set_expand(true);
    col.set_resizable(true);
    colview.append_column(&col);

    let scroll = gtk::ScrolledWindow::new();
    scroll.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Always);
    scroll.set_child(Some(&colview));
    scroll.set_size_request(-1, 220);

    let vbox = gtk::Box::new(gtk::Orientation::Vertical, 6);
    vbox.set_margin_top(6);
    vbox.set_margin_bottom(6);
    vbox.set_margin_start(6);
    vbox.set_margin_end(6);
    vbox.append(&scroll);

    let ent_hbox = gtk::Box::new(gtk::Orientation::Horizontal, 6);
    let entry = gtk::Entry::new();
    entry.set_hexpand(true);
    ent_hbox.append(&gtk::Label::new(Some(&tr("Address:"))));
    ent_hbox.append(&entry);
    vbox.append(&ent_hbox);

    let btnhbox = gtk::Box::new(gtk::Orientation::Horizontal, 6);
    btnhbox.set_halign(gtk::Align::End);
    let remove_btn = gtk::Button::with_label(&tr("Remove"));
    remove_btn.add_css_class("destructive-action");
    let add_btn = gtk::Button::with_label(&tr("Add"));
    add_btn.add_css_class("suggested-action");
    btnhbox.append(&remove_btn);
    btnhbox.append(&add_btn);
    vbox.append(&btnhbox);

    {
        let store = store.clone();
        let entry = entry.clone();
        add_btn.connect_clicked(move |_| {
            let t = entry.text();
            if t.is_empty() {
                return;
            }
            store.append(&gtk::StringObject::new(&t));
            entry.set_text("");
            persist_trackers(&store);
        });
    }
    {
        let store = store.clone();
        let selection = selection.clone();
        remove_btn.connect_clicked(move |_| {
            let pos = selection.selected();
            if pos == gtk::INVALID_LIST_POSITION {
                return;
            }
            store.remove(pos);
            persist_trackers(&store);
        });
    }

    let row = adw::PreferencesRow::new();
    row.set_selectable(false);
    row.set_activatable(false);
    row.set_child(Some(&vbox));
    grp.add(&row);
    page.add(&grp);

    let search = group(&tr("Search"));
    search.add(&switch_row(
        cfg::TRACKER_CASE,
        &tr("Case-sensitive tracker search"),
        None,
    ));
    page.add(&search);
}

/// Notifications → Behavior.
fn page_notify_behavior(page: &adw::PreferencesPage) {
    let behavior = group(&tr("Behavior"));
    behavior.add(&switch_row(
        cfg::NOTIFY_OMIT_FOCUSED,
        &tr("Don't notify when the relevant window is focused"),
        Some(&tr("If a chat or private message window is already active, don't pop a notification on top of it")),
    ));
    page.add(&behavior);

    let mentions = group(&tr("Mention Words"));
    mentions.set_description(Some(&tr(
        "Comma-separated. Your nickname is always matched in addition to this \
         list. The same list drives chat message highlighting.",
    )));
    mentions.add(&entry_row(cfg::HIGHLIGHT_WORDS, &tr("Highlight words")));
    page.add(&mentions);
}

// ---- C ABI page exports ------------------------------------------------
//
// options.c's create_options_window creates each AdwPreferencesPage and calls
// its settings_entries[].draw; for the ported pages that pointer is one of
// these exports, which build the page content in Rust. The page is C-owned
// (still floating at draw time), so from_glib_borrow — dropping the wrapper
// must not touch its refcount.

macro_rules! rs_page_export {
    ($export:ident => $inner:ident) => {
        /// # Safety
        /// `page` is a valid AdwPreferencesPage owned by the caller.
        #[no_mangle]
        pub unsafe extern "C" fn $export(page: *mut adw::ffi::AdwPreferencesPage) {
            // GTK is initialised from C, so gtk4-rs's own init flag isn't set;
            // building adw widgets without this trips libadwaita-rs's
            // assert_initialized_main_thread! (this is a C→Rust entry point,
            // and Settings may be the first Rust window opened).
            crate::ensure_gtk_init();
            let page = glib::translate::from_glib_borrow::<_, adw::PreferencesPage>(page);
            $inner(&page);
        }
    };
}

rs_page_export!(gtkhx_options_rs_page_general => page_general);
rs_page_export!(gtkhx_options_rs_page_file_transfers => page_file_transfers);
rs_page_export!(gtkhx_options_rs_page_chat_appearance => page_chat_appearance);
rs_page_export!(gtkhx_options_rs_page_chat_behavior => page_chat_behavior);
rs_page_export!(gtkhx_options_rs_page_chat_history => page_chat_history);
rs_page_export!(gtkhx_options_rs_page_chat_emoji => page_chat_emoji);
rs_page_export!(gtkhx_options_rs_page_notify_events => page_notify_events);
rs_page_export!(gtkhx_options_rs_page_notify_behavior => page_notify_behavior);
rs_page_export!(gtkhx_options_rs_page_sound => page_sound);
rs_page_export!(gtkhx_options_rs_page_tracker => page_tracker);
