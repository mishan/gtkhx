//! Tracker row actions: connect (double-click + button), save bookmark,
//! server-details dialog, and the right-click context menu. Ported from
//! the back half of `src/tracker.c`.

use super::row::HxTrackerRow;
use super::{cs, cstr, section_row_at, WIN};
use crate::bookmark_store;
use crate::ffi as cffi;
use crate::tr::{tr, trc};
use gtk4 as gtk;
use hxbookmarks::{cipher, Bookmark};
use libadwaita as adw;

use adw::prelude::*;
use gtk::gdk;
use gtk::glib;
use std::ffi::c_char;
use std::os::raw::c_void;

// ---------------------------------------------------------------------
// v3 security picking.
// ---------------------------------------------------------------------

/// (port, tls, secure, cipher_byte) for connecting to `row`, honouring
/// the v3 TLVs: TLS on `tls_port` wins; else HOPE with the strongest
/// advertised cipher; else plaintext. Mirrors
/// `tracker_row_pick_security` + `hope_ciphers_offers`.
fn pick_security(row: &HxTrackerRow) -> (u16, bool, bool, u8) {
    let mut port = row.port();
    let mut tls = false;
    let mut secure = false;
    let mut cipher_byte = cipher::NONE;

    let m = row.meta();
    if m.is_null() {
        return (port, tls, secure, cipher_byte);
    }
    let m = unsafe { &*m };

    if m.supports_tls != 0 && m.tls_port != 0 {
        return (m.tls_port, true, false, cipher::NONE);
    }
    if m.supports_hope != 0 {
        secure = true;
        let list = unsafe { cstr(m.hope_ciphers) };
        if hope_offers(&list, "CHACHA20-POLY1305") {
            cipher_byte = cipher::CHACHA20_POLY1305;
        } else if hope_offers(&list, "BLOWFISH") {
            cipher_byte = cipher::BLOWFISH;
        }
    }
    let _ = (&mut port, &mut tls);
    (port, tls, secure, cipher_byte)
}

/// Whole-token, case-insensitive membership in a comma/space/semicolon
/// separated cipher list (mirrors `hope_ciphers_offers`).
fn hope_offers(list: &str, needle: &str) -> bool {
    if list.is_empty() || needle.is_empty() {
        return false;
    }
    let needle = needle.to_ascii_lowercase();
    list.split([',', ' ', ';'])
        .map(|t| t.trim().to_ascii_lowercase())
        .any(|t| t == needle)
}

// ---------------------------------------------------------------------
// Selection accessors.
// ---------------------------------------------------------------------

fn selected_row() -> Option<HxTrackerRow> {
    WIN.with_borrow(|w| {
        let win = w.as_ref()?;
        let (sec, pos) = win.selected.as_ref()?;
        section_row_at(sec, *pos)
    })
}

// ---------------------------------------------------------------------
// Connect (double-click / Enter).
// ---------------------------------------------------------------------

pub(super) fn on_section_activate(url: &str, pos: u32) {
    let row = WIN.with_borrow(|w| {
        let win = w.as_ref()?;
        let sec = win.by_url.get(url)?;
        section_row_at(sec, pos)
    });
    let Some(row) = row else { return };
    let (port, tls, secure, cipher_byte) = pick_security(&row);
    // Stable cipher byte → cipher-name C string (NULL = no cipher). Keep the
    // CString alive across the call.
    let cname = cipher::name(cipher_byte).map(cs);
    let cipher_name = cname.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());
    let addr = cs(&row.address());
    unsafe {
        cffi::gtkhx_tracker_connect_apply(
            addr.as_ptr(),
            port,
            secure as c_char,
            tls as c_char,
            cipher_name,
        )
    };
}

// ---------------------------------------------------------------------
// Connect button / context "Connect" — pre-fill the Connect dialog.
// ---------------------------------------------------------------------

pub(super) fn tracker_connect() {
    let Some(row) = selected_row() else {
        return;
    };

    // M0 seam: open the Connect dialog on the ACTIVE session (the focused
    // connection), not the session captured when the tracker window was
    // opened — matching the pre-port tracker.c after multi-conn M0
    // (create_connect_window(0, hx_active_session())).
    unsafe { cffi::create_connect_window(std::ptr::null_mut(), cffi::hx_active_session()) };

    let (port, tls, secure, cipher_byte) = pick_security(&row);
    let addr = cs(&row.address());
    let portc = cs(&port.to_string());
    let empty = cs("");
    unsafe {
        cffi::set_the_entries(
            addr.as_ptr() as *mut c_char,
            empty.as_ptr() as *mut c_char,
            empty.as_ptr() as *mut c_char,
            portc.as_ptr() as *mut c_char,
            secure as c_char,
            0,
            cipher_byte as c_char,
            tls as c_char,
        )
    };
}

// ---------------------------------------------------------------------
// Save bookmark.
// ---------------------------------------------------------------------

const MAX_BOOKMARK_NAME: usize = 96;

/// Sanitise a tracker server name into a filesystem-safe filename
/// (mirrors `tracker_safe_bookmark_filename`).
fn safe_bookmark_filename(raw: &str, fallback: &str) -> String {
    let raw = if raw.is_empty() {
        if fallback.is_empty() {
            "server"
        } else {
            fallback
        }
    } else {
        raw
    };

    let mut out = String::new();
    for ch in raw.chars() {
        let c = ch as u32;
        if c < 0x20 || c == 0x7F {
            continue;
        }
        match ch {
            '/' | '\\' | ':' | '*' | '?' | '"' | '<' | '>' | '|' => out.push('_'),
            _ => out.push(ch),
        }
    }
    // Strip leading whitespace + dots.
    let trimmed_start: String = out
        .trim_start_matches([' ', '\t', '.'])
        .trim_end_matches([' ', '\t'])
        .to_owned();
    let mut out = trimmed_start;

    if out.is_empty() || out == "." || out == ".." {
        out = if fallback.is_empty() {
            "server".to_owned()
        } else {
            fallback.to_owned()
        };
    }
    // Cap at MAX_BOOKMARK_NAME bytes, snapping to a char boundary.
    if out.len() > MAX_BOOKMARK_NAME {
        let mut end = MAX_BOOKMARK_NAME;
        while end > 0 && !out.is_char_boundary(end) {
            end -= 1;
        }
        out.truncate(end);
    }
    out
}

pub(super) fn tracker_save_bookmark() {
    let Some(row) = selected_row() else { return };
    save_bookmark_for_row(&row);
}

fn save_bookmark_for_row(row: &HxTrackerRow) {
    // Bookmark names are stored verbatim now (TOML keys, not filenames).
    let name = safe_bookmark_filename(&row.name(), &row.address());
    if name.is_empty() {
        toast(&tr("Couldn't pick a bookmark name for this server."));
        return;
    }

    // Refuse to clobber an existing bookmark.
    if bookmark_store::exists(&name) {
        toast(&format!(
            "{} \"{}\"",
            tr("There is already a connection with that name; edit it in Settings → Connections:"),
            name
        ));
        return;
    }

    let (port, tls, secure, cipher_byte) = pick_security(row);
    let bm = Bookmark {
        name: name.clone(),
        server: row.address(),
        port: port.to_string(),
        hope: secure,
        cipher: cipher_byte,
        tls,
        ..Default::default()
    };

    match bookmark_store::upsert(bm) {
        Ok(()) => {
            unsafe { cffi::toolbar_refresh_bookmarks() };
            toast(&format!("{} \"{}\"", tr("Saved bookmark"), name));
        }
        Err(e) => toast(&format!("{}: {}", tr("Couldn't save bookmark"), e)),
    }
}

fn toast(text: &str) {
    unsafe { cffi::toolbar_show_toast(cs(text).as_ptr()) };
}

// ---------------------------------------------------------------------
// Right-click context menu.
// ---------------------------------------------------------------------

pub(super) fn on_secondary_press(url: &str, gesture: &gtk::GestureClick, x: f64, y: f64) {
    let cv = match gesture.widget().and_downcast::<gtk::ColumnView>() {
        Some(cv) => cv,
        None => return,
    };
    // Find the row under the pointer via the position stashed on the
    // cell's label (a widget we own — not GTK's cell/row widgets).
    let cv_widget = cv.clone().upcast::<gtk::Widget>();
    let Some(picked) = cv_widget.pick(x, y, gtk::PickFlags::DEFAULT) else {
        return;
    };
    let Some(pos) = super::find_stashed_pos(&picked, &cv_widget) else {
        return;
    };
    if pos == super::INVALID {
        return;
    }

    // Select the row first so the action handlers act on it.
    let sel = WIN.with_borrow(|w| w.as_ref()?.by_url.get(url).map(|s| s.selection.clone()));
    let Some(sel) = sel else { return };
    sel.set_selected(pos);

    // Build a flat-button popover (same shape users.c uses).
    let popover = gtk::Popover::new();
    popover.set_has_arrow(false);
    popover.set_halign(gtk::Align::Start);
    popover.set_pointing_to(Some(&gdk::Rectangle::new(x as i32, y as i32, 1, 1)));
    popover.set_parent(&cv_widget);
    popover.connect_closed(|p| p.unparent());

    let vbox = gtk::Box::new(gtk::Orientation::Vertical, 0);
    vbox.set_margin_start(4);
    vbox.set_margin_end(4);
    vbox.set_margin_top(4);
    vbox.set_margin_bottom(4);
    popover.set_child(Some(&vbox));

    vbox.append(&menu_button(&tr("Connect"), &popover, tracker_connect));
    vbox.append(&menu_button(
        &tr("Save Bookmark"),
        &popover,
        tracker_save_bookmark,
    ));
    vbox.append(&menu_button(&tr("Get Info"), &popover, tracker_details));

    popover.popup();
}

fn menu_button(label: &str, popover: &gtk::Popover, action: fn()) -> gtk::Button {
    let btn = gtk::Button::with_label(label);
    btn.set_has_frame(false);
    if let Some(child) = btn.child() {
        child.set_halign(gtk::Align::Start);
    }
    btn.set_hexpand(true);
    let popover = popover.clone();
    btn.connect_clicked(move |_| {
        action();
        popover.popdown();
    });
    btn
}

// ---------------------------------------------------------------------
// Server-details dialog.
// ---------------------------------------------------------------------

pub(super) fn tracker_details() {
    let Some(row) = selected_row() else { return };
    show_server_details(&row);
}

fn add_str(grp: &adw::PreferencesGroup, title: &str, value: &str) -> i32 {
    if value.is_empty() {
        return 0;
    }
    let r = adw::ActionRow::new();
    r.set_title(title);
    r.set_subtitle(value);
    r.set_subtitle_selectable(true);
    grp.add(&r);
    1
}

fn add_uint(grp: &adw::PreferencesGroup, title: &str, val: u32) -> i32 {
    if val == 0 {
        return 0;
    }
    add_str(grp, title, &val.to_string())
}

fn add_bool(grp: &adw::PreferencesGroup, title: &str, val: bool) -> i32 {
    if !val {
        return 0;
    }
    add_str(grp, title, &tr("Yes"))
}

fn add_uptime(grp: &adw::PreferencesGroup, title: &str, secs: u32) -> i32 {
    if secs == 0 {
        return 0;
    }
    let days = secs / 86400;
    let hours = (secs / 3600) % 24;
    let mins = (secs / 60) % 60;
    let s = if days != 0 {
        format!("{}d {}h {}m", days, hours, mins)
    } else if hours != 0 {
        format!("{}h {}m", hours, mins)
    } else if mins != 0 {
        format!("{}m", mins)
    } else {
        format!("{}s", secs)
    };
    add_str(grp, title, &s)
}

fn add_timestamp(grp: &adw::PreferencesGroup, title: &str, unix_ts: u32) -> i32 {
    if unix_ts == 0 {
        return 0;
    }
    let Ok(dt) = glib::DateTime::from_unix_utc(unix_ts as i64) else {
        return 0;
    };
    let Ok(s) = dt.format("%Y-%m-%d %H:%M UTC") else {
        return 0;
    };
    add_str(grp, title, s.as_str())
}

fn add_bytes(grp: &adw::PreferencesGroup, title: &str, bytes: u64) -> i32 {
    if bytes == 0 {
        return 0;
    }
    add_str(grp, title, glib::format_size(bytes).as_str())
}

fn finish_group(content: &gtk::Box, grp: adw::PreferencesGroup, count: i32) {
    if count > 0 {
        content.append(&grp);
    }
    // Otherwise the group just drops (unref'd) — nothing appended.
}

fn maturity_label(m: i32) -> String {
    match m {
        x if x == super::meta::MATURITY_TEEN => tr("Teen"),
        x if x == super::meta::MATURITY_MATURE => tr("Mature"),
        x if x == super::meta::MATURITY_ADULT => tr("Adult"),
        // TRANSLATORS: An age rating, the mildest of Teen/Mature/Adult —
        // "suitable for everyone", no age restriction. Not the server
        // category "General" below, and not the "General" preferences page.
        _ => trc("maturity rating", "General"),
    }
}

fn category_label(c: i32) -> String {
    use super::meta::*;
    match c {
        // TRANSLATORS: A server category, alongside Development, Gaming,
        // Media and so on — a server with no particular specialism. Not the
        // age rating "General" above, and not the preferences page.
        x if x == CATEGORY_GENERAL => trc("server category", "General"),
        x if x == CATEGORY_DEVELOPMENT => tr("Development"),
        x if x == CATEGORY_ARCHIVE => tr("Archive"),
        x if x == CATEGORY_WAREZ => tr("Warez"),
        x if x == CATEGORY_GAMING => tr("Gaming"),
        x if x == CATEGORY_MEDIA => tr("Media"),
        x if x == CATEGORY_EDUCATION => tr("Education"),
        x if x == CATEGORY_RESEARCH => tr("Research"),
        x if x == CATEGORY_FILE_SHARING => tr("File Sharing"),
        x if x == CATEGORY_SOCIAL => tr("Social"),
        x if x == CATEGORY_SECURITY => tr("Security"),
        x if x == CATEGORY_CREATIVE => tr("Creative"),
        _ => tr("Unspecified"),
    }
}

fn show_server_details(row: &HxTrackerRow) {
    let name = row.name();
    let address = row.address();
    let desc = row.desc();
    let port = row.port();
    let nusers = row.nusers();

    let dlg = adw::Dialog::new();
    let title_name = if !name.is_empty() {
        name.clone()
    } else if !address.is_empty() {
        address.clone()
    } else {
        "?".to_owned()
    };
    dlg.set_title(&format!("{} — {}", tr("Server details"), title_name));
    dlg.set_content_width(520);
    dlg.set_content_height(640);
    unsafe { cffi::gtkhx_dialog_add_close_shortcuts(dlg.as_ptr() as *mut cffi::GtkWidget) };

    let header = adw::HeaderBar::new();
    let toolbar_view = adw::ToolbarView::new();
    toolbar_view.add_top_bar(&header);

    let content = gtk::Box::new(gtk::Orientation::Vertical, 18);
    content.set_margin_top(18);
    content.set_margin_bottom(18);
    content.set_margin_start(18);
    content.set_margin_end(18);

    let m = row.meta();
    let m = if m.is_null() {
        None
    } else {
        Some(unsafe { &*m })
    };

    // --- Server (always) ---
    let grp = adw::PreferencesGroup::new();
    grp.set_title(&tr("Server"));
    let mut n = 0;
    n += add_str(&grp, &tr("Name"), &name);
    if !address.is_empty() {
        let addrport = unsafe {
            let p = cffi::gtkhx_join_host_port(cs(&address).as_ptr(), port);
            let s = cstr(p);
            glib::ffi::g_free(p as *mut c_void);
            s
        };
        n += add_str(&grp, &tr("Address"), &addrport);
    }
    n += add_str(&grp, &tr("Description"), &desc);
    n += add_str(&grp, &tr("Users online"), &nusers.to_string());
    if let Some(m) = m {
        if m.has_max_users != 0 {
            n += add_str(&grp, &tr("Maximum users"), &m.max_users.to_string());
        }
    }
    finish_group(&content, grp, n);

    if let Some(m) = m {
        // --- Identity ---
        let grp = adw::PreferencesGroup::new();
        // TRANSLATORS: Heading for a group of facts about the server —
        // its software, country, region, language and tags. The other
        // "Identity" in this catalog is the user's own name and icon.
        grp.set_title(&trc("server details", "Identity"));
        let mut n = 0;
        n += add_str(&grp, &tr("Software"), unsafe { &cstr(m.server_software) });
        n += add_str(&grp, &tr("Country"), unsafe { &cstr(m.country_code) });
        n += add_str(&grp, &tr("Region"), unsafe { &cstr(m.region) });
        n += add_str(&grp, &tr("Language"), unsafe { &cstr(m.language) });
        n += add_str(&grp, &tr("Tags"), unsafe { &cstr(m.tags) });
        if m.maturity != super::meta::MATURITY_GENERAL {
            n += add_str(&grp, &tr("Maturity"), &maturity_label(m.maturity));
        }
        if m.listing_category != super::meta::CATEGORY_UNSPECIFIED {
            n += add_str(&grp, &tr("Category"), &category_label(m.listing_category));
        }
        n += add_str(&grp, &tr("Contact"), unsafe { &cstr(m.contact_url) });
        n += add_str(&grp, &tr("Rules"), unsafe { &cstr(m.rules_url) });
        n += add_str(&grp, &tr("Banner URL"), unsafe { &cstr(m.banner_url) });
        n += add_str(&grp, &tr("Icon URL"), unsafe { &cstr(m.icon_url) });
        if m.has_timezone_offset != 0 {
            let mins = m.timezone_offset_min as i32;
            let sign = if mins < 0 { '-' } else { '+' };
            let absm = mins.abs();
            n += add_str(
                &grp,
                &tr("Timezone"),
                &format!("UTC{}{:02}:{:02}", sign, absm / 60, absm % 60),
            );
        }
        n += add_timestamp(&grp, &tr("Server launched"), m.server_launched);
        n += add_uptime(&grp, &tr("Uptime"), m.uptime_secs);
        finish_group(&content, grp, n);

        // --- Capabilities ---
        let grp = adw::PreferencesGroup::new();
        grp.set_title(&tr("Capabilities"));
        let mut n = 0;
        if m.protocol_version != 0 {
            n += add_str(
                &grp,
                &tr("Protocol version"),
                &format!("0x{:04x} ({})", m.protocol_version, m.protocol_version),
            );
        }
        if m.min_proto_version != 0 {
            n += add_str(
                &grp,
                &tr("Minimum client protocol"),
                &format!("0x{:04x} ({})", m.min_proto_version, m.min_proto_version),
            );
        }
        n += add_bool(&grp, &tr("HOPE encryption"), m.supports_hope != 0);
        n += add_str(&grp, &tr("HOPE ciphers"), unsafe { &cstr(m.hope_ciphers) });
        n += add_bool(&grp, &tr("TLS"), m.supports_tls != 0);
        if m.supports_tls != 0 && m.tls_port != 0 {
            n += add_str(&grp, &tr("TLS port"), &m.tls_port.to_string());
        }
        n += add_bool(&grp, &tr("IPv6"), m.supports_ipv6 != 0);
        n += add_bool(&grp, &tr("Inline media"), m.supports_inline_media != 0);
        n += add_bool(&grp, &tr("Voice"), m.supports_voice != 0);
        n += add_bool(&grp, &tr("Large files"), m.supports_large_files != 0);
        n += add_uint(&grp, &tr("Link down (Mbit/s)"), m.link_down_mbit);
        n += add_uint(&grp, &tr("Link up (Mbit/s)"), m.link_up_mbit);
        finish_group(&content, grp, n);

        // --- Activity ---
        let grp = adw::PreferencesGroup::new();
        grp.set_title(&tr("Activity"));
        let mut n = 0;
        n += add_uint(&grp, &tr("Peak users (24h)"), m.peak_24h as u32);
        n += add_uint(&grp, &tr("Average users (24h)"), m.avg_24h as u32);
        n += add_uint(&grp, &tr("News posts"), m.news_count);
        n += add_uint(&grp, &tr("Message-board posts"), m.msgboard_count);
        n += add_uint(&grp, &tr("Files"), m.files_count);
        n += add_bytes(&grp, &tr("Total file size"), m.total_file_size as u64);
        n += add_timestamp(&grp, &tr("Last news activity"), m.last_news_timestamp);
        n += add_timestamp(&grp, &tr("Last public chat"), m.last_chat_timestamp);
        finish_group(&content, grp, n);

        // --- Tracker ---
        let grp = adw::PreferencesGroup::new();
        grp.set_title(&tr("Tracker"));
        let mut n = 0;
        n += add_bool(&grp, &tr("Promoted"), m.is_promoted != 0);
        n += add_bool(&grp, &tr("Verified online"), m.verified_online != 0);
        n += add_bool(&grp, &tr("Private listing"), m.private_listing != 0);
        n += add_bool(&grp, &tr("Language strict"), m.language_strict != 0);
        n += add_timestamp(&grp, &tr("First seen"), m.first_seen);
        n += add_timestamp(&grp, &tr("Last heartbeat"), m.last_heartbeat);
        finish_group(&content, grp, n);
    }

    let clamp = adw::Clamp::new();
    clamp.set_child(Some(&content));
    let scrolled = gtk::ScrolledWindow::new();
    scrolled.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    scrolled.set_child(Some(&clamp));
    toolbar_view.set_content(Some(&scrolled));
    dlg.set_child(Some(&toolbar_view));

    let parent = WIN.with_borrow(|w| w.as_ref().map(|win| win.window.clone()));
    dlg.present(parent.as_ref().map(|w| w.upcast_ref::<gtk::Widget>()));
}
