//! Plays a remote audio or video file as it downloads, and seeks in it.
//!
//! GStreamer's `playbin` reads the file through [`source`]'s element, which
//! reads it a range at a time: every place the player asks to read from is a
//! download resumed there, which every rig server honors, so little is
//! fetched that isn't played.
//! Pictures come off an `appsink` as RGBA and go into a `GtkPicture`.

mod source;

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::c_void;
use std::rc::{Rc, Weak};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use gstreamer as gst;
use gstreamer::prelude::*;
use gstreamer_app as gst_app;
use gtk4 as gtk;
use gtk4::{gdk, glib};
use libadwaita as adw;
use libadwaita::prelude::*;

use hxmodel::files_entry::HxFileEntry;

use crate::dock::{self, Bound};
use crate::tr::tr;
use source::{Endpoint, Problem, Reader};

/// How long a refused range is asked for again: mhxd refuses a download
/// while the last one it gave the user winds down, which takes a moment
/// after a seek closes it.
const RETRY_EVERY_MS: u64 = 100;
const RETRIES: u32 = 30;

/// Whether the file is one this can play: a movie or audio type, or a name
/// that says so. Files past 4 GiB are left out: the resume offset and the
/// fork length the transfer carries are 32-bit.
pub fn plays(name: &str, icon_id: u16, size: u64) -> bool {
    const EXTENSIONS: &[&str] = &[
        "mp4", "m4v", "mov", "mkv", "webm", "avi", "mpg", "mpeg", "ogv", "wmv", "flv", "3gp",
        "mp3", "m4a", "aac", "ogg", "oga", "opus", "flac", "wav", "aif", "aiff",
    ];
    if size > u64::from(u32::MAX) {
        return false;
    }
    if icon_id == hxmodel::files::icon::MOOV || icon_id == hxmodel::files::icon::NOTE {
        return true;
    }
    name.rsplit_once('.')
        .is_some_and(|(_, ext)| EXTENSIONS.iter().any(|e| ext.eq_ignore_ascii_case(e)))
}

/// The newest picture off the appsink, for the main thread to show.
#[derive(Default)]
struct Frames {
    newest: Mutex<Option<gdk::MemoryTexture>>,
    pending: AtomicBool,
}

struct Player {
    id: u64,
    bound: Bound,
    dir: Vec<u8>,
    name: Vec<u8>,
    reader: Arc<Reader>,
    frames: Arc<Frames>,
    pipeline: RefCell<Option<gst::Element>>,
    watch: RefCell<Option<gst::bus::BusWatchGuard>>,
    window: adw::Window,
    stack: gtk::Stack,
    picture: gtk::Picture,
    status: adw::StatusPage,
    play: gtk::Button,
    scale: gtk::Scale,
    time: gtk::Label,
    retries: Cell<u32>,
    started: Cell<bool>,
}

thread_local! {
    static PLAYERS: RefCell<HashMap<u64, Weak<Player>>> = RefCell::new(HashMap::new());
    static NEXT_ID: Cell<u64> = const { Cell::new(1) };
}

fn player(id: u64) -> Option<Rc<Player>> {
    PLAYERS.with_borrow(|p| p.get(&id).and_then(Weak::upgrade))
}

/// Open a player for `e`, in `dir` on the connection `htlc`. `download`
/// fetches the whole file, as the browser's Download does.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub unsafe fn open(htlc: *mut c_void, dir: &[u8], e: &HxFileEntry, download: impl Fn() + 'static) {
    crate::ensure_gtk_init();
    if gst::init().is_err() || !source::register() {
        return;
    }
    let id = NEXT_ID.with(|n| n.replace(n.get() + 1));
    // Always from an idle, never inline: `fetch` asks with its lock held,
    // and can be on the main thread, when a demuxer reads while it seeks.
    let reader = Arc::new(Reader::new(move |range, offset| {
        glib::idle_add_once(move || {
            if let Some(p) = player(id) {
                p.request(range, offset);
            }
        });
    }));
    source::publish(id, Arc::clone(&reader));

    let window = adw::Window::new();
    window.set_title(Some(&e.name()));
    window.set_default_size(720, 480);
    let view = adw::ToolbarView::new();
    let header = adw::HeaderBar::new();
    let download_btn = gtk::Button::from_icon_name("folder-download-symbolic");
    download_btn.set_tooltip_text(Some(&tr("Download")));
    download_btn.connect_clicked(move |_| download());
    header.pack_end(&download_btn);
    view.add_top_bar(&header);

    let picture = gtk::Picture::new();
    picture.set_content_fit(gtk::ContentFit::Contain);
    let status = adw::StatusPage::new();
    let spinner = adw::Spinner::new();
    spinner.set_size_request(48, 48);
    spinner.set_halign(gtk::Align::Center);
    spinner.set_valign(gtk::Align::Center);
    let stack = gtk::Stack::new();
    stack.set_vexpand(true);
    stack.add_named(&spinner, Some("loading"));
    stack.add_named(&picture, Some("video"));
    stack.add_named(&status, Some("status"));
    view.set_content(Some(&stack));

    let play = gtk::Button::from_icon_name("media-playback-pause-symbolic");
    play.set_tooltip_text(Some(&tr("Pause")));
    play.add_css_class("flat");
    let scale = gtk::Scale::with_range(gtk::Orientation::Horizontal, 0.0, 1.0, 1.0);
    scale.set_hexpand(true);
    scale.set_draw_value(false);
    let time = gtk::Label::new(None);
    time.add_css_class("numeric");
    let controls = gtk::Box::new(gtk::Orientation::Horizontal, 6);
    controls.set_margin_start(6);
    controls.set_margin_end(12);
    controls.set_margin_top(6);
    controls.set_margin_bottom(6);
    controls.append(&play);
    controls.append(&scale);
    controls.append(&time);
    view.add_bottom_bar(&controls);
    window.set_content(Some(&view));

    let p = Rc::new(Player {
        id,
        bound: dock::bind(dock::conn_key(htlc)),
        dir: dir.to_vec(),
        name: e.wire_name(),
        reader,
        frames: Arc::new(Frames::default()),
        pipeline: RefCell::new(None),
        watch: RefCell::new(None),
        window: window.clone(),
        stack,
        picture,
        status,
        play: play.clone(),
        scale: scale.clone(),
        time,
        retries: Cell::new(0),
        started: Cell::new(false),
    });
    PLAYERS.with_borrow_mut(|m| m.insert(id, Rc::downgrade(&p)));

    play.connect_clicked(move |_| {
        if let Some(p) = player(id) {
            p.toggle();
        }
    });
    scale.connect_change_value(move |_, _, value| {
        if let Some(p) = player(id) {
            p.seek_to(value);
        }
        glib::Propagation::Proceed
    });
    let tick = Rc::clone(&p);
    let ticker = glib::timeout_add_local(std::time::Duration::from_millis(250), move || {
        tick.show_position();
        glib::ControlFlow::Continue
    });
    let held = RefCell::new(Some(p));
    let ticker = RefCell::new(Some(ticker));
    window.connect_destroy(move |_| {
        if let Some(t) = ticker.take() {
            t.remove();
        }
        if let Some(p) = held.take() {
            p.close();
        }
    });
    unsafe { crate::ffi::init_keyaccel(window.upcast_ref::<gtk::Widget>().as_ptr().cast()) };
    window.present();

    if let Some(p) = player(id) {
        let range = p.reader.begin();
        p.request(range, 0);
    }
}

impl Player {
    /// Ask for range `range`, from `offset` on.
    fn request(&self, range: u64, offset: u64) {
        let Some(htlc) = dock::live_htlc(self.bound) else {
            self.fail(&tr("The connection to the server has closed."));
            return;
        };
        let id = self.id;
        let sent = unsafe {
            hxhandlers::send::files::get_range(
                htlc,
                &self.dir,
                &self.name,
                offset as u32,
                Box::new(move |r| {
                    if let Some(p) = player(id) {
                        p.answered(range, offset, r.map(|t| (t.reference, t.size)));
                    }
                }),
            )
        };
        if !sent {
            self.fail(&tr("The file's name or folder is too long to ask for."));
        }
    }

    fn answered(&self, range: u64, offset: u64, r: Result<(u32, u64), Option<String>>) {
        // A range moved on from is still opened when granted, so the server
        // lets go of it rather than waiting it out; refused, it's forgotten.
        if r.is_err() && !self.reader.is_current(range) {
            return;
        }
        match r {
            Ok((reference, size)) => {
                self.retries.set(0);
                let Some(htlc) = dock::live_htlc(self.bound) else {
                    self.fail(&tr("The connection to the server has closed."));
                    return;
                };
                let endpoint = unsafe { Endpoint::of(htlc) };
                let reader = Arc::clone(&self.reader);
                let id = self.id;
                let first = !self.started.get();
                let spawned = std::thread::Builder::new()
                    .name("media-range".into())
                    .spawn(move || {
                        let error = reader.read(range, &endpoint, reference, size, offset, |_| {
                            if first {
                                glib::MainContext::default().invoke(move || {
                                    if let Some(p) = player(id) {
                                        p.start();
                                    }
                                });
                            }
                        });
                        // Once playing, the source element reports it.
                        if let Some(problem) = error.filter(|_| first) {
                            glib::MainContext::default().invoke(move || {
                                if let Some(p) = player(id) {
                                    p.fail(&problem_text(problem));
                                }
                            });
                        }
                    });
                if spawned.is_err() {
                    self.fail(&tr("Couldn't start reading the file."));
                }
            }
            Err(Some(_)) if self.started.get() && self.retries.get() < RETRIES => {
                self.retries.set(self.retries.get() + 1);
                let id = self.id;
                glib::timeout_add_local_once(
                    std::time::Duration::from_millis(RETRY_EVERY_MS),
                    move || {
                        if let Some(p) = player(id) {
                            if p.reader.is_current(range) {
                                p.request(range, offset);
                            }
                        }
                    },
                );
            }
            Err(Some(reason)) if !reason.is_empty() => self.fail(&reason),
            Err(_) => self.fail(&tr("The server didn't send the file.")),
        }
    }

    /// The first range has said how long the file is: start playing.
    fn start(&self) {
        if self.started.replace(true) {
            return;
        }
        let Ok(playbin) = gst::ElementFactory::make("playbin")
            .property("uri", format!("{}://{}", source::SCHEME, self.id))
            .build()
        else {
            self.fail(&tr("GStreamer's playbin element is missing."));
            return;
        };
        let id = self.id;
        playbin.set_property("video-sink", self.video_sink());
        let bus = playbin.bus().expect("a pipeline has a bus");
        let watch = bus.add_watch_local(move |_, msg| {
            if let Some(p) = player(id) {
                p.on_message(msg);
            }
            glib::ControlFlow::Continue
        });
        self.watch.replace(watch.ok());
        let _ = playbin.set_state(gst::State::Playing);
        self.pipeline.replace(Some(playbin));
    }

    fn video_sink(&self) -> gst::Element {
        let caps = gst::Caps::builder("video/x-raw")
            .field("format", "RGBA")
            .build();
        let frames = Arc::clone(&self.frames);
        let id = self.id;
        gst_app::AppSink::builder()
            .caps(&caps)
            .max_buffers(2)
            .callbacks(
                gst_app::AppSinkCallbacks::builder()
                    .new_sample(move |sink| {
                        let sample = sink.pull_sample().map_err(|_| gst::FlowError::Eos)?;
                        if let Some(texture) = texture_of(&sample) {
                            *frames.newest.lock().unwrap_or_else(|p| p.into_inner()) =
                                Some(texture);
                            if !frames.pending.swap(true, Ordering::AcqRel) {
                                glib::MainContext::default().invoke(move || {
                                    if let Some(p) = player(id) {
                                        p.show_frame();
                                    }
                                });
                            }
                        }
                        Ok(gst::FlowSuccess::Ok)
                    })
                    .build(),
            )
            .build()
            .upcast()
    }

    fn show_frame(&self) {
        self.frames.pending.store(false, Ordering::Release);
        let newest = self
            .frames
            .newest
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .take();
        if let Some(texture) = newest {
            self.picture.set_paintable(Some(&texture));
            self.stack.set_visible_child_name("video");
        }
    }

    fn on_message(&self, msg: &gst::Message) {
        use gst::MessageView;
        match msg.view() {
            MessageView::Error(e) => self.fail(&match self.reader.problem() {
                Some(p) => problem_text(p),
                None => e.error().to_string(),
            }),
            MessageView::Eos(_) => self.set_playing(false),
            MessageView::AsyncDone(_) => {
                let videos = self
                    .pipeline
                    .borrow()
                    .as_ref()
                    .map_or(0, |p| p.property::<i32>("n-video"));
                if videos == 0 && self.stack.visible_child_name().as_deref() != Some("status") {
                    self.status.set_icon_name(Some("audio-x-generic-symbolic"));
                    self.status
                        .set_title(&self.window.title().unwrap_or_default());
                    self.stack.set_visible_child_name("status");
                }
            }
            _ => {}
        }
    }

    fn set_playing(&self, playing: bool) {
        let (icon, tip) = if playing {
            ("media-playback-pause-symbolic", tr("Pause"))
        } else {
            ("media-playback-start-symbolic", tr("Play"))
        };
        self.play.set_icon_name(icon);
        self.play.set_tooltip_text(Some(&tip));
    }

    fn toggle(&self) {
        let Some(p) = self.pipeline.borrow().clone() else {
            return;
        };
        let playing = p.current_state() == gst::State::Playing;
        if !playing {
            // Played to the end: play again from the start.
            let at_end = matches!(
                (p.query_position::<gst::ClockTime>(), p.query_duration::<gst::ClockTime>()),
                (Some(pos), Some(dur)) if pos >= dur
            );
            if at_end {
                seek(&p, gst::SeekFlags::FLUSH, gst::ClockTime::ZERO);
            }
        }
        let _ = p.set_state(if playing {
            gst::State::Paused
        } else {
            gst::State::Playing
        });
        self.set_playing(!playing);
    }

    fn seek_to(&self, seconds: f64) {
        if let Some(p) = self.pipeline.borrow().as_ref() {
            let at = gst::ClockTime::from_nseconds((seconds.max(0.0) * 1e9) as u64);
            seek(p, gst::SeekFlags::FLUSH | gst::SeekFlags::KEY_UNIT, at);
        }
    }

    fn show_position(&self) {
        let Some(p) = self.pipeline.borrow().clone() else {
            return;
        };
        let pos = p.query_position::<gst::ClockTime>();
        let dur = p.query_duration::<gst::ClockTime>();
        if let Some(dur) = dur {
            self.scale.set_range(0.0, dur.seconds_f64().max(1.0));
        }
        if let Some(pos) = pos {
            self.scale.set_value(pos.seconds_f64());
        }
        self.time.set_text(&match (pos, dur) {
            (Some(pos), Some(dur)) => format!("{} / {}", clock(pos), clock(dur)),
            (Some(pos), None) => clock(pos),
            _ => String::new(),
        });
    }

    fn fail(&self, why: &str) {
        self.reader.close();
        if let Some(p) = self.pipeline.borrow().as_ref() {
            let _ = p.set_state(gst::State::Null);
        }
        self.status.set_icon_name(Some("dialog-warning-symbolic"));
        self.status.set_title(&tr("Can't Play This File"));
        self.status.set_description(Some(why));
        self.stack.set_visible_child_name("status");
        self.play.set_sensitive(false);
        self.scale.set_sensitive(false);
    }

    fn close(&self) {
        self.reader.close();
        self.watch.take();
        if let Some(p) = self.pipeline.take() {
            let _ = p.set_state(gst::State::Null);
        }
        source::withdraw(self.id);
        PLAYERS.with_borrow_mut(|m| m.remove(&self.id));
    }
}

/// A sample's picture, as a texture.
fn texture_of(sample: &gst::Sample) -> Option<gdk::MemoryTexture> {
    let s = sample.caps()?.structure(0)?;
    let width = s.get::<i32>("width").ok()?;
    let height = s.get::<i32>("height").ok()?;
    if width <= 0 || height <= 0 {
        return None;
    }
    let buffer = sample.buffer_owned()?;
    // An appsink offers no video meta, so rows are packed.
    let stride = width as usize * 4;
    if buffer.size() < stride * height as usize {
        return None;
    }
    let bytes = glib::Bytes::from_owned(buffer.into_mapped_buffer_readable().ok()?);
    Some(gdk::MemoryTexture::new(
        width,
        height,
        gdk::MemoryFormat::R8g8b8a8,
        &bytes,
        stride,
    ))
}

/// Seek on another thread: a demuxer can read while it seeks, on the
/// seeking thread, and reading waits on the network.
fn seek(p: &gst::Element, flags: gst::SeekFlags, at: gst::ClockTime) {
    let p = p.clone();
    std::thread::spawn(move || {
        let _ = p.seek_simple(flags, at);
    });
}

fn problem_text(p: Problem) -> String {
    match p {
        Problem::Connect => tr("Couldn't connect to the server for the file."),
        Problem::NotAFile => tr("The server sent something other than the file."),
        Problem::Stopped => tr("The server stopped sending the file."),
        Problem::NoResume => tr("The server can only send this file from its start."),
    }
}

/// A position as the time label shows it: m:ss, or h:mm:ss.
fn clock(t: gst::ClockTime) -> String {
    let s = t.seconds();
    if s >= 3600 {
        format!("{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60)
    } else {
        format!("{}:{:02}", s / 60, s % 60)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plays_movies_and_audio_by_type_or_name() {
        use hxmodel::files::icon;
        for (name, icon_id, size, want) in [
            ("clip.mp4", icon::FILE, 10, true),
            ("Song.MP3", icon::FILE, 10, true),
            ("untitled", icon::MOOV, 10, true),
            ("untitled", icon::NOTE, 10, true),
            ("notes.txt", icon::TEXT, 10, false),
            ("mp4", icon::FILE, 10, false),
            ("types.ts", icon::FILE, 10, false),
            ("huge.mkv", icon::MOOV, u64::from(u32::MAX) + 1, false),
        ] {
            assert_eq!(plays(name, icon_id, size), want, "{name}");
        }
    }

    #[test]
    fn clock_reads_minutes_or_hours() {
        assert_eq!(clock(gst::ClockTime::from_seconds(65)), "1:05");
        assert_eq!(clock(gst::ClockTime::from_seconds(3725)), "1:02:05");
    }
}
