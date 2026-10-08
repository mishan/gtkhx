//! The update notice. Whether a build may look for updates at all is decided
//! when it is configured (`-Dupdate_check`, see docs/updates.md); the user's
//! own switch is `updates.check` in the settings.
//!
//! Inside the Flatpak the portal's update monitor does the asking, and GtkHx
//! installs only when the user says so. The portal first asks half an hour
//! after it starts, so GtkHx also reads the feed once when it starts
//! watching, to have something to say before then.

use std::cell::RefCell;
use std::ffi::c_void;

use gtk4 as gtk;
use gtk4::gio;
use gtk4::glib;
use hxupdate::flatpak::{
    overall_percent, Notice, PortalState, MIN_PORTAL_VERSION, PROGRESS_ERROR, PROGRESS_RUNNING,
};
use libadwaita as adw;
use libadwaita::prelude::*;

use crate::tr::{tr, tr_argv};

/// `-Dupdate_check`, passed in by rust/meson.build. Off for a bare
/// `cargo build`, like a distribution's package.
pub(crate) const BUILD_ENABLED: bool = match option_env!("GTKHX_UPDATE_CHECK") {
    Some(v) => matches!(v.as_bytes(), [b'1']),
    None => false,
};

/// Only the Flatpak asks yet, and a switch for a check that never runs would
/// mislead, so Settings shows the switch only where one does.
pub(crate) fn checks_wired() -> bool {
    in_flatpak()
}

/// Inside the Flatpak the sandbox's own update machinery is asked; the feed
/// only fills in until the portal first polls.
pub(crate) fn in_flatpak() -> bool {
    std::path::Path::new("/.flatpak-info").exists()
}

const RELEASES_URL: &str = "https://github.com/mishan/gtkhx/releases";
const FEED_URL: &str = "https://dl.gtkhx.org/updates.json";
/// The portal polls every half hour from when it starts, and says nothing
/// when it finds nothing newer than what runs, so a monitor that has heard
/// nothing by this long after opening has been told there is nothing new.
const FEED_HOLDS: u32 = 32 * 60;

const FLATPAK_BUS: &str = "org.freedesktop.portal.Flatpak";
const FLATPAK_PATH: &str = "/org/freedesktop/portal/Flatpak";
const FLATPAK_IFACE: &str = "org.freedesktop.portal.Flatpak";
const MONITOR_IFACE: &str = "org.freedesktop.portal.Flatpak.UpdateMonitor";
/// `Spawn`'s `FLATPAK_SPAWN_FLAGS_LATEST_VERSION`: start the installed
/// commit rather than the one this process runs.
const SPAWN_LATEST_VERSION: u32 = 2;

extern "C" {
    /// `gtkhx.c` — save, disconnect everything and exit.
    fn hx_quit();
    /// `session_registry.c`.
    fn hx_session_count() -> u32;
    fn hx_session_at(i: u32) -> *mut c_void;
    /// `gtkhx_ui_bridge.c`.
    fn gtkhx_session_htlc(sess: *mut c_void) -> *mut c_void;
    /// `gtkhx-core` — the connection's socket, or 0 when it has none.
    fn hx_conn_fd(htlc: *const c_void) -> std::os::raw::c_int;
}

#[derive(Default)]
struct Monitor {
    banner: Option<adw::Banner>,
    state: PortalState,
    conn: Option<gio::DBusConnection>,
    /// The monitor object, once `CreateUpdateMonitor` has answered.
    handle: Option<String>,
    subscriptions: Vec<gio::SignalSubscription>,
    /// Bumped whenever the check is turned off, so a start still in flight
    /// knows to close what it opens.
    generation: u32,
    starting: bool,
    /// Counts what the monitor says, for [`watch_for_stall`].
    heard: u32,
    /// Spawn has been asked for the new version and hasn't answered.
    restarting: bool,
}

thread_local! {
    static MONITOR: RefCell<Monitor> = RefCell::new(Monitor::default());
}

fn enabled() -> bool {
    BUILD_ENABLED
        && checks_wired()
        && hxconfig::ffi::with_settings(|s| s.updates.check).unwrap_or(false)
}

/// The main window's update banner. Hidden until a check finds something.
pub(crate) fn banner() -> gtk::Widget {
    let banner = adw::Banner::new("");
    banner.set_revealed(false);
    banner.connect_button_clicked(on_button);
    MONITOR.with_borrow_mut(|m| {
        m.banner = Some(banner.clone());
        (m.state.dismissed, m.state.dismissed_version) = read_dismissed(&dismissed_path());
    });
    refresh();
    banner.into()
}

/// Close the monitor, or abandon one still opening.
fn close_monitor(m: &mut Monitor) {
    m.generation += 1;
    m.starting = false;
    m.subscriptions.clear();
    if let (Some(conn), Some(handle)) = (&m.conn, m.handle.take()) {
        close(conn, &handle);
    }
}

/// Start or stop watching to match the build and the user's setting.
pub(crate) fn refresh() {
    let on = enabled();
    let start = MONITOR.with_borrow_mut(|m| {
        if on {
            let start = m.handle.is_none() && !m.starting;
            m.starting |= start;
            return start.then_some(m.generation);
        }
        close_monitor(m);
        // "Later" was the user's answer, not something the portal said.
        m.state = PortalState {
            dismissed: std::mem::take(&mut m.state.dismissed),
            dismissed_version: std::mem::take(&mut m.state.dismissed_version),
            ..PortalState::default()
        };
        None
    });
    if let Some(generation) = start {
        glib::MainContext::default().spawn_local(start_monitor(generation));
    }
    render();
}

async fn start_monitor(generation: u32) {
    let started = open_monitor().await;
    let stale = MONITOR.with_borrow_mut(|m| {
        if m.generation != generation {
            return started.ok().map(|(conn, handle, _, _)| (conn, handle));
        }
        m.starting = false;
        match started {
            Ok((conn, handle, version, subscriptions)) => {
                m.state.version = version;
                m.conn = Some(conn);
                m.handle = Some(handle);
                m.subscriptions = subscriptions;
                // Only with a monitor to update through.
                glib::MainContext::default().spawn_local(read_feed(generation));
                glib::timeout_add_seconds_local_once(FEED_HOLDS, move || {
                    MONITOR.with_borrow_mut(|m| {
                        if m.generation == generation {
                            m.state.feed.clear();
                        }
                    });
                    render();
                });
            }
            Err(version) => m.state.version = version,
        }
        None
    });
    if let Some((conn, handle)) = stale {
        close(&conn, &handle);
    }
    render();
}

/// What the feed says is out on the installed branch. Any failure is
/// silent: the portal still reports in time.
async fn read_feed(generation: u32) {
    let Some(branch) = installed_branch() else {
        return;
    };
    let Ok(Ok(bytes)) =
        gio::spawn_blocking(|| hxnet::banner_http::http_get(FEED_URL, hxupdate::MAX_FEED_BYTES))
            .await
    else {
        return;
    };
    let Ok(feed) = hxupdate::parse_feed(&bytes) else {
        return;
    };
    let newer = hxupdate::flatpak::feed_newer(crate::ffi::VERSION, &branch, &feed);
    MONITOR.with_borrow_mut(|m| {
        if m.generation == generation {
            m.state.feed = newer.unwrap_or_default();
        }
    });
    render();
}

/// The Flatpak branch this instance runs, from `/.flatpak-info`.
fn installed_branch() -> Option<String> {
    let info = glib::KeyFile::new();
    info.load_from_file("/.flatpak-info", glib::KeyFileFlags::NONE)
        .ok()?;
    info.string("Instance", "branch").ok().map(String::from)
}

type Opened = (
    gio::DBusConnection,
    String,
    u32,
    Vec<gio::SignalSubscription>,
);

/// Ask the portal for an update monitor. On failure, the portal version
/// found, if any, so a too-old portal is remembered rather than retried.
async fn open_monitor() -> Result<Opened, u32> {
    let conn = gio::bus_get_future(gio::BusType::Session)
        .await
        .map_err(|_| 0u32)?;
    let version = conn
        .call_future(
            Some(FLATPAK_BUS),
            FLATPAK_PATH,
            "org.freedesktop.DBus.Properties",
            "Get",
            Some(&(FLATPAK_IFACE, "version").to_variant()),
            None,
            gio::DBusCallFlags::NONE,
            -1,
        )
        .await
        .ok()
        .and_then(|r| r.child_value(0).as_variant())
        .and_then(|v| v.get::<u32>())
        .unwrap_or(0);
    if version < MIN_PORTAL_VERSION {
        return Err(version);
    }
    // One monitor at a time, and a closed one says nothing more, so every
    // signal on the interface is this monitor's. Subscribed before the call
    // so nothing it says early is missed.
    let subscribe = |member: &str, f: fn(&mut Monitor, &glib::VariantDict)| {
        conn.subscribe_to_signal(
            Some(FLATPAK_BUS),
            Some(MONITOR_IFACE),
            Some(member),
            None,
            None,
            gio::DBusSignalFlags::NONE,
            move |sig| {
                let Some((info,)) = sig.parameters.get::<(glib::VariantDict,)>() else {
                    return;
                };
                MONITOR.with_borrow_mut(|m| f(m, &info));
                watch_for_stall();
                render();
            },
        )
    };
    let subscriptions = vec![
        subscribe("UpdateAvailable", |m, info| {
            let commit = |key| {
                info.lookup::<String>(key)
                    .ok()
                    .flatten()
                    .unwrap_or_default()
            };
            let carried = m.state.update_available(
                commit("running-commit"),
                commit("local-commit"),
                commit("remote-commit"),
            );
            if carried {
                let s = &m.state;
                write_dismissed(&dismissed_path(), &s.dismissed, &s.dismissed_version);
            }
        }),
        subscribe("Progress", |m, info| {
            let num = |key| info.lookup::<u32>(key).ok().flatten();
            let percent = overall_percent(num("progress").unwrap_or(0), num("op"), num("n_ops"));
            m.state.progress = Some((num("status").unwrap_or(0), percent));
        }),
    ];
    let options = glib::VariantDict::new(None);
    options.insert("handle_token", format!("gtkhx{}", glib::random_int()));
    let handle = conn
        .call_future(
            Some(FLATPAK_BUS),
            FLATPAK_PATH,
            FLATPAK_IFACE,
            "CreateUpdateMonitor",
            Some(&glib::Variant::tuple_from_iter([options.end()])),
            None,
            gio::DBusCallFlags::NONE,
            -1,
        )
        .await
        .ok()
        .and_then(|r| r.child_value(0).get::<glib::variant::ObjectPath>())
        .ok_or(version)?;
    Ok((conn, handle.to_string(), version, subscriptions))
}

fn close(conn: &gio::DBusConnection, monitor: &str) {
    conn.call(
        Some(FLATPAK_BUS),
        monitor,
        MONITOR_IFACE,
        "Close",
        None,
        None,
        gio::DBusCallFlags::NONE,
        -1,
        gio::Cancellable::NONE,
        |_| {},
    );
}

fn render() {
    MONITOR.with_borrow(|m| {
        let Some(banner) = &m.banner else {
            return;
        };
        let (title, button) = match hxupdate::flatpak::notice(enabled(), &m.state) {
            Notice::None => {
                banner.set_revealed(false);
                return;
            }
            Notice::Available => (tr("A new version of GtkHx is available"), tr("Update…")),
            Notice::Updating(percent) => (
                tr_argv("Updating GtkHx… %s%%", &[&percent.to_string()]),
                String::new(),
            ),
            Notice::Failed => (
                tr(
                    "GtkHx couldn't update itself. Update it with your software center, \
                    or run “flatpak update com.nasledov.gtkhx”.",
                ),
                // The portal won't report again until a commit moves, so
                // a passing failure is retried from here.
                tr("Try Again"),
            ),
            Notice::Installed => (tr("GtkHx was updated"), tr("Restart")),
        };
        banner.set_title(&title);
        banner.set_button_label((!button.is_empty()).then_some(button.as_str()));
        banner.set_revealed(true);
    });
}

fn on_button(banner: &adw::Banner) {
    let notice = MONITOR.with_borrow(|m| hxupdate::flatpak::notice(enabled(), &m.state));
    match notice {
        Notice::Available => ask_to_update(banner),
        Notice::Installed if any_connected() => confirm_restart(banner),
        Notice::Installed => restart(),
        Notice::Failed => update_now(),
        _ => {}
    }
}

fn ask_to_update(banner: &adw::Banner) {
    let dialog = adw::AlertDialog::new(
        Some(&tr("Update GtkHx?")),
        Some(&tr("A new version of GtkHx is available.")),
    );
    // A link rather than a response, which would close the dialog.
    dialog.set_extra_child(Some(&gtk::LinkButton::with_label(
        RELEASES_URL,
        &tr("What's New"),
    )));
    dialog.add_response("later", &tr("_Later"));
    dialog.add_response("update", &tr("Update _Now"));
    dialog.set_response_appearance("update", adw::ResponseAppearance::Suggested);
    dialog.set_default_response(Some("update"));
    dialog.set_close_response("later");
    dialog.connect_response(None, |_, response| match response {
        "update" => update_now(),
        _ => {
            let (remote, version) = MONITOR.with_borrow_mut(|m| {
                m.state.dismissed = m.state.remote.clone();
                m.state.dismissed_version = m.state.feed.clone();
                (m.state.remote.clone(), m.state.feed.clone())
            });
            write_dismissed(&dismissed_path(), &remote, &version);
            render();
        }
    });
    dialog.present(Some(banner));
}

fn update_now() {
    let Some((conn, handle)) = MONITOR.with_borrow(|m| Some((m.conn.clone()?, m.handle.clone()?)))
    else {
        return;
    };
    conn.call(
        Some(FLATPAK_BUS),
        &handle,
        MONITOR_IFACE,
        "Update",
        Some(&glib::Variant::tuple_from_iter([
            "".to_variant(),
            glib::VariantDict::new(None).end(),
        ])),
        None,
        gio::DBusCallFlags::NONE,
        -1,
        gio::Cancellable::NONE,
        |result| {
            // Refused outright: no Progress will follow to say so.
            if result.is_err() {
                MONITOR.with_borrow_mut(|m| m.state.progress = Some((PROGRESS_ERROR, 0)));
                render();
            }
        },
    );
    MONITOR.with_borrow_mut(|m| m.state.progress = Some((PROGRESS_RUNNING, 0)));
    watch_for_stall();
    render();
}

/// Give up on an update that has gone quiet this long, so the banner falls
/// back to the commits and Update can be tried again.
const STALL: u32 = 300;

/// Arm the stall timer afresh: any word from the monitor shows it is alive.
fn watch_for_stall() {
    let serial = MONITOR.with_borrow_mut(|m| {
        m.heard += 1;
        m.heard
    });
    glib::timeout_add_seconds_local_once(STALL, move || {
        let stalled = MONITOR.with_borrow_mut(|m| {
            let stalled =
                m.heard == serial && matches!(m.state.progress, Some((PROGRESS_RUNNING, _)));
            if stalled {
                // The portal refuses another Update on this monitor while
                // the first is installing; closing it cancels that.
                close_monitor(m);
                m.state.progress = None;
            }
            stalled
        });
        if stalled {
            refresh();
        }
    });
}

fn any_connected() -> bool {
    unsafe {
        (0..hx_session_count()).any(|i| {
            let sess = hx_session_at(i);
            !sess.is_null() && hx_conn_fd(gtkhx_session_htlc(sess)) != 0
        })
    }
}

fn confirm_restart(banner: &adw::Banner) {
    let dialog = adw::AlertDialog::new(
        Some(&tr("Restart GtkHx?")),
        Some(&tr(
            "Restarting will disconnect from every server, cancel transfers in progress \
             and start the new version.",
        )),
    );
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("restart", &tr("_Restart"));
    dialog.set_response_appearance("restart", adw::ResponseAppearance::Destructive);
    dialog.set_default_response(Some("cancel"));
    dialog.set_close_response("cancel");
    dialog.connect_response(Some("restart"), |_, _| restart());
    dialog.present(Some(banner));
}

/// Start the installed version through the portal, then quit. Quitting
/// waits for the portal to answer, so a start the portal refuses leaves
/// GtkHx running.
fn restart() {
    // GtkHx isn't a unique application, so a second click while the portal
    // is still answering would start a second copy.
    let Some(conn) = MONITOR.with_borrow_mut(|m| {
        let conn = m.conn.clone().filter(|_| !m.restarting)?;
        m.restarting = true;
        Some(conn)
    }) else {
        return;
    };
    let cwd = std::env::current_dir().unwrap_or_else(|_| "/".into());
    let bytes = |s: &[u8]| {
        let mut v = s.to_vec();
        v.push(0);
        glib::Variant::array_from_fixed_array(&v)
    };
    let argv =
        glib::Variant::array_from_iter_with_type(glib::VariantTy::BYTE_STRING, [bytes(b"gtkhx")]);
    let args = glib::Variant::tuple_from_iter([
        bytes(cwd.as_os_str().as_encoded_bytes()),
        argv,
        glib::Variant::array_from_iter_with_type(
            glib::VariantTy::new("{uh}").expect("valid type"),
            [] as [glib::Variant; 0],
        ),
        std::collections::HashMap::<String, String>::new().to_variant(),
        SPAWN_LATEST_VERSION.to_variant(),
        glib::VariantDict::new(None).end(),
    ]);
    // The portal starts the new commit on this instance's runtime, so an
    // update to a new runtime branch needs a manual relaunch (docs/updates.md).
    glib::MainContext::default().spawn_local(async move {
        let spawned = conn
            .call_future(
                Some(FLATPAK_BUS),
                FLATPAK_PATH,
                FLATPAK_IFACE,
                "Spawn",
                Some(&args),
                None,
                gio::DBusCallFlags::NONE,
                -1,
            )
            .await;
        match spawned {
            Ok(_) => unsafe { hx_quit() },
            Err(_) => {
                MONITOR.with_borrow_mut(|m| m.restarting = false);
                let msg = crate::cs(&tr("Couldn't restart GtkHx. Quit and start it again."));
                unsafe { crate::ffi::toolbar_show_toast(msg.as_ptr()) };
            }
        }
    });
}

/// The remote commit and the feed's version the user answered "Later" to,
/// a line each, kept across restarts.
/// State rather than a setting, so it lives with the cache, not the config.
fn dismissed_path() -> std::path::PathBuf {
    glib::user_cache_dir().join("gtkhx").join("update-later")
}

fn read_dismissed(path: &std::path::Path) -> (String, String) {
    let text = std::fs::read_to_string(path).unwrap_or_default();
    let mut lines = text.lines().map(|l| l.trim().to_owned());
    (
        lines.next().unwrap_or_default(),
        lines.next().unwrap_or_default(),
    )
}

/// Best effort: failing to remember means asking again next launch.
fn write_dismissed(path: &std::path::Path, commit: &str, version: &str) {
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let _ = std::fs::write(path, format!("{commit}\n{version}\n"));
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    #[test]
    fn later_survives_a_restart() {
        let dir = std::env::temp_dir().join(format!("gtkhx-later-{}", std::process::id()));
        let path = dir.join("gtkhx").join("update-later");
        assert_eq!(
            read_dismissed(&path),
            (String::new(), String::new()),
            "nothing dismissed yet"
        );
        write_dismissed(&path, "abc123", "1.5.0b1");
        assert_eq!(read_dismissed(&path), ("abc123".into(), "1.5.0b1".into()));
        // As an older build wrote it: the commit alone.
        std::fs::write(&path, "abc123").unwrap();
        assert_eq!(read_dismissed(&path), ("abc123".into(), String::new()));
        std::fs::remove_dir_all(&dir).unwrap();
    }

    /// Nothing has been heard from a portal, so there is nothing to show.
    pub(crate) fn check_banner_starts_hidden() {
        let banner = banner().downcast::<adw::Banner>().expect("a banner");
        assert!(!banner.is_revealed());
    }
}
