//! What the public chat's and each private chat's view draws: the lines
//! people say, live and from the server's history, and the lines GtkHx
//! writes there itself.
//!
//! The `chat`, `chat-history-batch` and `chat-log-line` signals are heard
//! here. Each view keeps what it has drawn of the server's history (where
//! "Load older" asks from, and the rows that bracket it) and a
//! `LineRegistry`: the row of each line the server gave an id, so that a
//! line can be found again by that id, and so that a line drawn live while
//! the first page of history was on its way is not drawn again inside it.
//!
//! Where the session has reactions (the Reactions draft, capability bit
//! 19), a kept line's chips come from its history entry and then from
//! what the `reactions` signal says of it, with what the user has asked
//! for and not had answered drawn over them (`hxmodel::reactions`). A
//! click on a chip, or its row's menu (`reactions`), asks for one. That
//! goes for a private chat's kept lines as for the lobby's: on hxd-ng, a
//! room is shown as a private chat whose lines carry history ids.
//!
//! Where the session has replies (the Replies draft, capability bit 18), a
//! lobby line that answers another is drawn under a header quoting it:
//! from the line's own row where this view drew it, and otherwise from the
//! server, which is asked for a kept line by its id once the draw that
//! found it is done (`hxmodel::replies`). Until the quote comes, and where
//! it never can, the header says it is an earlier message, so a row's
//! height stays as it was drawn and a screen reader hears at once that it
//! is a reply.

use std::cell::RefCell;
use std::collections::{BTreeSet, HashMap, VecDeque};
use std::ffi::{c_char, c_void, CStr, CString};
use std::time::{Duration, Instant};

use glib::translate::{from_glib_none, ToGlibPtr};
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use hxmodel::reactions::{LineReactions, Next, Shown};
use hxmodel::replies::{Quote, Quoted, Refused};
use hxproto::reactions::{Key, Mine, Summary, INVALID_KEY};
use hxproto::target::Target;
use hxsession::HistoryEntry;
use rotulus::ffi::{RotulusRun, RotulusSpeaker};
use rotulus::{LoadDirection, RotulusView};
use rotulus_layout::{ColorRef, MessageId, Reaction, Reply};

use gtkhx_core::boxed::chat::{split_nick_body, HxChatEvent};
use gtkhx_core::conn::{hx_conn_name, hx_conn_sess};
use hxmodel::chat_members::hx_member_model_find_by_name;
use hxmodel::conversation::{hx_chat_media_table, hx_chat_member_model, hx_chat_view};

use crate::tr::{tr, tr_argv, trn_argv};

/// Palette slots (rotulus.h's `ROTULUS_PAL_*`).
const PAL_MUTED: i16 = 37;
const PAL_NICK_BRACKET: i16 = 41;
const PAL_SELF_BRACKET: i16 = 42;
const PAL_SYSTEM_BRACKET: i16 = 44;
const PAL_HIGHLIGHT: i16 = 45;

/// Chat History's entry flags: an emote, a line the server wrote, a line
/// taken away, and on a live line alone, an id from the chat's own count.
const ACTION: u16 = 0x0001;
const SERVER_MSG: u16 = 0x0002;
const DELETED: u16 = 0x0004;
const EPHEMERAL: u16 = 0x0008;

extern "C" {
    // chat.c
    fn chat_with_cid(sess: *mut c_void, cid: u32) -> *mut c_void;
    fn hx_gchat_output(g: *mut c_void) -> *mut gtk::ffi::GtkWidget;
    fn hx_chat_nick_color(nick: *const c_char, len: usize, is_self: glib::ffi::gboolean) -> i16;
    // proto_helpers.c
    fn hx_highlight_match(body: *const c_char, len: usize, words: *const *const c_char) -> i32;
    // notify.c
    fn gtkhx_notify_chat(htlc: *mut c_void, event: *const HxChatEvent);
    // debug.c
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
}

/// Which count a line's id is from: the chat's history, or, for a live line
/// flagged so, the chat's own.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
enum Space {
    History,
    Ephemeral,
}

/// The rows of a chat view's lines, by the id the server gave each.
///
/// The view drops its oldest live rows past its scrollback, whether they
/// hold a line or not, and never a history row. So every live row drawn
/// here is counted, a live line is let go of once that many have come
/// after its row, and history rows stay until the view is cleared. Ids
/// cannot bound it: the first page of history is drawn after the live
/// lines that came while it was on its way, and the two spaces count apart.
/// A row drawn some other way goes uncounted, which can only keep a line a
/// while after its row has gone, never the other way round.
#[derive(Default)]
struct LineRegistry {
    lines: HashMap<(Space, u64), Line>,
    /// Live lines, oldest first, each with the count of live rows drawn up
    /// to its own.
    live: VecDeque<((Space, u64), MessageId, u64)>,
    drawn: u64,
    /// The newest history ids drawn, up to [`TOLD_LINES`] of them, kept
    /// whatever trimming lets go of.
    newest: BTreeSet<u64>,
    /// The rows of replies whose header waits for what their target says,
    /// by the target as the wire names it.
    waiting: HashMap<Target, Vec<MessageId>>,
}

/// A line's row, and the reactions shown under it.
struct Line {
    row: MessageId,
    reactions: LineReactions,
    /// When the server last said what the line holds, or was asked.
    told: Instant,
    /// What a reply to it quotes.
    said: Option<Quote>,
}

impl Line {
    fn new(row: MessageId) -> Line {
        let (reactions, told) = (LineReactions::default(), Instant::now());
        Line {
            row,
            reactions,
            told,
            said: None,
        }
    }
}

/// How long what the server said of a line's reactions is taken as so.
const FRESH: Duration = Duration::from_secs(60);

/// How many of a chat's newest lines a server tells changes to (hxd-ng's
/// live window); older lines' changes may never come.
const TOLD_LINES: usize = 200;

impl LineRegistry {
    /// The row of line `id`, unless it has been let go of.
    fn row(&self, space: Space, id: u64) -> Option<MessageId> {
        self.lines.get(&(space, id)).map(|l| l.row)
    }

    /// History line `id` is drawn on `row`: its reactions, none yet.
    fn history(&mut self, id: u64, row: MessageId) -> &mut LineReactions {
        self.drew_id(Space::History, id);
        let line = self.lines.entry((Space::History, id));
        &mut line.insert_entry(Line::new(row)).into_mut().reactions
    }

    /// Whether what history line `id` holds may have changed untold by
    /// `now`: it was last told a while ago, and enough lines drawn since
    /// are newer that the server may no longer tell its changes. GtkHx
    /// draws no line the user ignores, so it counts fewer than the server
    /// and errs toward fresh.
    fn stale(&self, id: u64, now: Instant) -> bool {
        let Some(line) = self.lines.get(&(Space::History, id)) else {
            return false;
        };
        let past = self.newest.len() >= TOLD_LINES && self.newest.first() > Some(&id);
        now.saturating_duration_since(line.told) > FRESH && past
    }

    fn drew_id(&mut self, space: Space, id: u64) {
        if space == Space::History {
            self.newest.insert(id);
            if self.newest.len() > TOLD_LINES {
                self.newest.pop_first();
            }
        }
    }

    /// The history line drawn on `row`.
    fn history_on(&self, row: MessageId) -> Option<u64> {
        let mut lines = self.lines.iter();
        lines.find_map(|(&(space, id), l)| (space == Space::History && l.row == row).then_some(id))
    }

    /// `rows` more live rows were drawn, in a view that keeps `max_rows`.
    fn drew(&mut self, rows: u64, max_rows: usize) {
        self.drawn += rows;
        while let Some(&(key, row, at)) = self.live.front() {
            if max_rows == 0 || at + max_rows as u64 > self.drawn {
                break;
            }
            self.live.pop_front();
            if self.row(key.0, key.1) == Some(row) {
                self.lines.remove(&key);
            }
            self.waiting.retain(|_, rows| {
                rows.retain(|&r| r != row);
                !rows.is_empty()
            });
        }
    }

    /// Line `key` is the live row drawn last.
    fn live(&mut self, key: (Space, u64), row: MessageId) {
        // A line already drawn keeps its first row.
        if self.lines.contains_key(&key) {
            return;
        }
        self.lines.insert(key, Line::new(row));
        self.live.push_back((key, row, self.drawn));
        self.drew_id(key.0, key.1);
    }
}

/// What a chat view keeps of what it drew.
#[derive(Default)]
struct Output {
    /// The oldest history line drawn, which "Load older" asks before; 0
    /// until a page has been.
    oldest: u64,
    /// "Load older" was asked for and its page has not come.
    loading: bool,
    /// The opening divider of the history, which older pages go above.
    anchor: *mut c_void,
    /// The "Load older" row, or the one saying it is on its way.
    load_older: *mut c_void,
    lines: LineRegistry,
    /// History lines a change of reactions named while the first page was
    /// on its way, before the page drew them: the page may have been read
    /// before the change, so they are asked after again once it is drawn.
    missed: Vec<u64>,
    /// The quotes of lines asked for by id.
    quotes: hxmodel::replies::Quotes,
    /// Asking for them waits on the main loop's next idle.
    asking: bool,
}

/// The most lines [`Output::missed`] keeps.
const MISSED: usize = 200;

const OUTPUT: &str = "gtkhx-chat-output";

/// `view`'s [`Output`], made on first use.
fn output(view: &RotulusView) -> &RefCell<Output> {
    // SAFETY: the key is only ever set to a RefCell<Output>, which lives as
    // long as the view and is only reached on the main thread.
    unsafe {
        if view.data::<RefCell<Output>>(OUTPUT).is_none() {
            view.set_data(OUTPUT, RefCell::new(Output::default()));
            // The view trims at once when its scrollback shrinks, and a
            // larger one by the next draw would not bring those rows back.
            view.connect_notify_local(Some("max-lines"), |view, _| {
                output(view).borrow_mut().lines.drew(0, view.max_rows());
            });
        }
        view.data::<RefCell<Output>>(OUTPUT).unwrap().as_ref()
    }
}

/// Where a view keeps the id of the chat it shows, once heard.
const HEARD: &str = "gtkhx-chat-output-heard";

/// Hear the clicks on "Load older" in `view`, chat `cid`'s, and on its
/// chips and rows. Once.
fn hear(view: &RotulusView, cid: u32) {
    // SAFETY: the key is only ever set to a u32.
    if unsafe { view.data::<u32>(HEARD) }.is_some() {
        return;
    }
    unsafe { view.set_data(HEARD, cid) };
    crate::reactions::hear(view);
    view.connect_local("load-more", false, move |args| {
        let view = args[0].get::<RotulusView>().ok()?;
        if args[1].get::<LoadDirection>().ok()? == LoadDirection::Older {
            load_older(&view, cid);
        }
        None
    });
}

/// The chat `view` shows: the public chat's, 0, until it is heard.
fn cid_of(view: &RotulusView) -> u32 {
    // SAFETY: the key is only ever set to a u32.
    unsafe { view.data::<u32>(HEARD) }.map_or(0, |c| unsafe { *c.as_ref() })
}

/// How line `id` in `view` is named to the server: a lobby line by its
/// channel, and a private chat's by the chat's id (kind 4), as the Replies
/// draft has a client name any line it saw under a chat id, which on
/// hxd-ng may be a room's.
pub(crate) fn target(view: &RotulusView, id: u64) -> Target {
    match cid_of(view) {
        0 => Target::Line { channel: 0, id },
        chat => Target::Ephemeral { chat, id },
    }
}

/// The chat and line `target` names, where it is a line GtkHx names: kind
/// 4 under a private chat's id, and kind 1 under the lobby's channel or,
/// from a server that names a room as a channel, under its chat's id.
fn line_of(target: &Target) -> Option<(u32, u64)> {
    match *target {
        Target::Line { channel, id } => Some((channel, id)),
        Target::Ephemeral { chat, id } if chat != 0 => Some((chat, id)),
        _ => None,
    }
}

fn raw(view: &RotulusView) -> *mut gtk::ffi::GtkWidget {
    view.upcast_ref::<gtk::Widget>().to_glib_none().0
}

fn mark(p: *mut c_void) -> Option<MessageId> {
    (!p.is_null()).then_some(MessageId(p as u64))
}

fn run(text: &[u8], color: i16, attrs: u16) -> RotulusRun {
    RotulusRun {
        text: text.as_ptr().cast(),
        len: text.len() as i32,
        color,
        attrs,
        background: 0,
        rgb: 0,
        background_rgb: 0,
    }
}

fn plain(text: &[u8]) -> RotulusRun {
    run(text, rotulus::ffi::ROTULUS_COLOR_DEFAULT, 0)
}

fn muted(text: &str) -> RotulusRun {
    run(text.as_bytes(), PAL_MUTED, 0)
}

const NOBODY: RotulusSpeaker = RotulusSpeaker {
    key: 0,
    nick: std::ptr::null(),
    nick_len: 0,
};

/// A row to draw.
struct Row<'a> {
    kind: i32,
    flags: u32,
    stamp: i64,
    speaker: RotulusSpeaker,
    gutter: &'a [RotulusRun],
    body: &'a [RotulusRun],
}

impl Row<'_> {
    /// Draw it at the end of `view`, or above the row `before` marks. Its
    /// mark.
    fn put(&self, view: &RotulusView, before: Option<*mut c_void>) -> *mut c_void {
        let row = rotulus::ffi::RotulusRow {
            kind: self.kind,
            flags: self.flags,
            stamp: self.stamp,
            speaker: self.speaker,
            gutter: self.gutter.as_ptr(),
            n_gutter: self.gutter.len() as i32,
            body: self.body.as_ptr(),
            n_body: self.body.len() as i32,
        };
        // SAFETY: the view is live and the row's runs outlive the call.
        unsafe {
            match before {
                Some(at) => rotulus::ffi::rotulus_view_insert_before(raw(view), at, &row),
                None => rotulus::ffi::rotulus_view_append(raw(view), &row),
            }
        }
    }

    fn of(kind: i32, body: &[RotulusRun]) -> Row<'_> {
        Row {
            kind,
            flags: 0,
            stamp: 0,
            speaker: NOBODY,
            gutter: &[],
            body,
        }
    }
}

/// A line someone said, as it is drawn.
struct Said<'a> {
    /// Empty when the line has no nick column.
    nick: &'a [u8],
    body: &'a [u8],
    is_self: bool,
    /// It mentions us.
    highlight: bool,
    nick_color: i16,
    speaker: RotulusSpeaker,
}

/// Draw `s` as one row.
fn draw_said(view: &RotulusView, s: &Said) -> u64 {
    let flags = if s.is_self {
        rotulus::ffi::ROW_OUTGOING
    } else {
        0
    };
    let gutter = if s.nick.is_empty() {
        Vec::new()
    } else if s.highlight {
        vec![
            run(b"<", PAL_HIGHLIGHT, 0),
            run(s.nick, PAL_HIGHLIGHT, rotulus::ffi::ATTR_BOLD),
            run(b">", PAL_HIGHLIGHT, 0),
        ]
    } else {
        let brackets = if s.is_self {
            PAL_SELF_BRACKET
        } else {
            PAL_NICK_BRACKET
        };
        vec![
            run(b"<", brackets, 0),
            run(s.nick, s.nick_color, 0),
            run(b">", brackets, 0),
        ]
    };
    Row {
        flags,
        speaker: s.speaker,
        gutter: &gutter,
        ..Row::of(rotulus::ffi::ROW_MESSAGE, &[plain(s.body)])
    }
    .put(view, None);
    1
}

/// Draw a "[tag] body" line: one GtkHx wrote, or a broadcast.
fn draw_tagged(view: &RotulusView, tag: &[u8], color: i16, body: &[u8]) -> u64 {
    let gutter = [
        run(b"[", PAL_SYSTEM_BRACKET, 0),
        run(tag, color, 0),
        run(b"]", PAL_SYSTEM_BRACKET, 0),
    ];
    Row {
        gutter: &gutter,
        ..Row::of(rotulus::ffi::ROW_SYSTEM, &[plain(body)])
    }
    .put(view, None);
    1
}

/// Draw a line the server wrote as it is, each of its lines a row.
fn draw_prose(view: &RotulusView, line: &[u8]) -> u64 {
    let mut rows = 0;
    let mut lines = line.split(|&b| b == b'\n').peekable();
    while let Some(l) = lines.next() {
        if lines.peek().is_none() && l.is_empty() {
            break;
        }
        // SAFETY: the view is live; the text is read for the call.
        unsafe {
            rotulus::ffi::rotulus_view_append_text(raw(view), l.as_ptr().cast(), l.len() as i32, 0)
        };
        rows += 1;
    }
    rows
}

/// Draw a live line with `draw`, which says how many rows it drew, and
/// know its last row by the id `entry` gives it.
fn draw_live(view: &RotulusView, entry: Option<&HistoryEntry>, draw: impl FnOnce() -> u64) {
    let rows = draw();
    let mut out = output(view).borrow_mut();
    out.lines.drew(rows, view.max_rows());
    let Some(e) = entry.filter(|e| rows > 0 && e.message_id != 0) else {
        return;
    };
    // SAFETY: the view is live.
    let Some(row) = mark(unsafe { rotulus::ffi::rotulus_view_get_last(raw(view)) }) else {
        return;
    };
    let space = if e.flags & EPHEMERAL != 0 {
        Space::Ephemeral
    } else {
        Space::History
    };
    out.lines.live((space, e.message_id), row);
    drop(out);
    quotable(view, (space, e.message_id), e);
    head(view, row, e);
}

/// Draw live line `s`, then with `picture` the row of the picture it
/// carries, if any, which says how many rows it drew. The line is its text
/// row, as a history line is, so its chips go under its words.
fn draw_said_live(
    view: &RotulusView,
    entry: Option<&HistoryEntry>,
    s: &Said,
    picture: impl FnOnce() -> u64,
) {
    draw_live(view, entry, || draw_said(view, s));
    draw_live(view, None, picture);
}

/// Up to the first NUL, as C reads a string.
fn c_str(s: &str) -> &str {
    s.split('\0').next().unwrap_or_default()
}

/// What a chip for one of the server's own emoji is known by.
const SERVER_EMOJI: &str = "server-emoji:";

/// The chips `shown` draws: an emoji as itself, which a screen reader
/// names; one of the server's own, which GtkHx cannot draw, as a
/// placeholder named by its id.
pub(crate) fn chips(shown: &[Shown]) -> Vec<Reaction> {
    let chip = |s: &Shown| {
        let (key, text, label) = match &s.key {
            Key::Emoji(e) => (e.clone(), None, None),
            Key::Server(id) => (
                format!("{SERVER_EMOJI}{id}"),
                Some("\u{25C7}".to_owned()),
                // TRANSLATORS: what a screen reader calls a placeholder
                // for one of the server's own emoji, by its number.
                Some(tr_argv("server emoji %u", &[&id.to_string()])),
            ),
        };
        Reaction {
            key,
            text,
            count: s.count,
            mine: s.mine,
            label,
        }
    };
    shown.iter().map(chip).collect()
}

/// Draw history line `id`'s chips again, after `change` to its reactions;
/// what it returns. A line whose row has gone is let go of.
fn redraw<T>(
    view: &RotulusView,
    id: u64,
    change: impl FnOnce(&mut LineReactions) -> T,
) -> Option<T> {
    let (row, chips, done) = {
        let mut out = output(view).borrow_mut();
        let line = out.lines.lines.get_mut(&(Space::History, id))?;
        let done = change(&mut line.reactions);
        (line.row, chips(&line.reactions.shown()), done)
    };
    if !view.set_reactions(row, chips) {
        output(view)
            .borrow_mut()
            .lines
            .lines
            .remove(&(Space::History, id));
    }
    Some(done)
}

/// What history line `id` holds now: chips, or none. One not drawn while
/// the first page is on its way (`page_due`) is kept to be asked after.
fn reacted(view: &RotulusView, id: u64, reactions: Option<&(Summary, Mine)>, page_due: bool) {
    let (summary, mine) = reactions.cloned().unwrap_or_default();
    if redraw(view, id, |r| r.replace(summary, &mine)).is_some() {
        let mut out = output(view).borrow_mut();
        if let Some(line) = out.lines.lines.get_mut(&(Space::History, id)) {
            line.told = Instant::now();
        }
        return;
    }
    let missed = &mut output(view).borrow_mut().missed;
    if page_due && missed.len() < MISSED && !missed.contains(&id) {
        missed.push(id);
    }
}

/// Ask after what history line `id` holds, on `htlc`, where it may have
/// changed untold.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn refresh_if_stale(view: &RotulusView, htlc: *mut c_void, id: u64) {
    if due(view, id, Instant::now()) {
        hxhandlers::send::reactions::refresh(htlc, &[target(view, id)]);
    }
}

/// Whether history line `id` is to be asked after at `now`; asking counts
/// as told, so a menu opened again before the answer asks no more.
fn due(view: &RotulusView, id: u64, now: Instant) -> bool {
    let mut out = output(view).borrow_mut();
    if !out.lines.stale(id, now) {
        return false;
    }
    out.lines.lines.get_mut(&(Space::History, id)).unwrap().told = now;
    true
}

/// The emoji history line `id` holds, in the order its chips are drawn.
pub(crate) fn emoji_on(view: &RotulusView, id: u64) -> Vec<String> {
    let out = output(view).borrow();
    let Some(line) = out.lines.lines.get(&(Space::History, id)) else {
        return Vec::new();
    };
    (line.reactions.shown().into_iter())
        .filter_map(|s| match s.key {
            Key::Emoji(e) => Some(e),
            Key::Server(_) => None,
        })
        .collect()
}

/// Whether a moderator may be offered to clear history line `id`: it
/// shows a chip, a server emoji's placeholder among them.
pub(crate) fn clearable(view: &RotulusView, id: u64) -> bool {
    let out = output(view).borrow();
    (out.lines.lines.get(&(Space::History, id))).is_some_and(|l| !l.reactions.shown().is_empty())
}

/// What [`want`] and [`answered`] leave to do, to the line `T` names, or
/// to another row reacted to as a line is.
#[derive(Debug, PartialEq)]
pub(crate) enum Then<T = u64> {
    /// Ask for `key` on the line, an add or not.
    Send(T, Key, bool),
    /// Take back the request for `key` on the line, if it is still to go.
    Withdraw(T, Key),
    /// Tell the user why not.
    Say(String),
}

/// The chip `key` as a key to ask for, or why not: a server emoji's
/// placeholder is none GtkHx may send.
pub(crate) fn chip_key<T>(key: &str) -> Result<Key, Then<T>> {
    match key.starts_with(SERVER_EMOJI) {
        true => Err(Then::Say(crate::reactions::server_emoji())),
        false => Ok(Key::Emoji(key.to_owned())),
    }
}

/// The user wants `key` on `r`, which `row` names, on, off, or (`None`)
/// the other way round; `limits` are the login's. What is left to do.
pub(crate) fn wanted<T>(
    r: &mut LineReactions,
    row: T,
    key: Key,
    on: Option<bool>,
    limits: &hxproto::reactions::Limits,
) -> Option<Then<T>> {
    let on = on.unwrap_or_else(|| !r.mine(&key));
    if on && !r.mine(&key) {
        if let Some(code) = r.refusal(&key, limits.max_keys, limits.max_per_user) {
            return Some(Then::Say(crate::reactions::refusal(Some(code), None)));
        }
    }
    match r.want(key.clone(), on) {
        Next::Send(add) => Some(Then::Send(row, key, add)),
        Next::Withdraw => Some(Then::Withdraw(row, key)),
        Next::Nothing => None,
    }
}

/// The kept line drawn on `row`, where `limits` take reactions to it. A
/// line under its chat's own count is none: hxd-ng keeps no such line in
/// the lobby, and in a private chat kind 4 cannot say which count its id
/// is from, which hxd-ng reads as a kept line's, so it would name another
/// line or none.
pub(crate) fn line_on(
    view: &RotulusView,
    row: MessageId,
    limits: &hxproto::reactions::Limits,
) -> Option<u64> {
    // SAFETY: as for `output`, which this does not make where there is none.
    let out = unsafe { view.data::<RefCell<Output>>(OUTPUT) }?;
    let id = unsafe { out.as_ref() }.borrow().lines.history_on(row)?;
    limits.takes(&target(view, id)).then_some(id)
}

/// The user wants the chip `key` on `row` on, or off, or (`None`) the
/// other way round; `limits` are the login's. A line reached by no chip of
/// GtkHx's is none of these.
pub(crate) fn want(
    view: &RotulusView,
    row: MessageId,
    key: &str,
    on: Option<bool>,
    limits: &hxproto::reactions::Limits,
) -> Option<Then> {
    let id = line_on(view, row, limits)?;
    let key = match chip_key(key) {
        Ok(key) => key,
        Err(say) => return Some(say),
    };
    redraw(view, id, |r| wanted(r, id, key, on, limits))?
}

/// The request for `key` on line `id` that `add`ed or not was made, with
/// `count` after: what the user wants since, if that is otherwise.
pub(crate) fn answered(
    view: &RotulusView,
    id: u64,
    key: &Key,
    add: bool,
    count: Option<u32>,
) -> Option<Then> {
    let again = redraw(view, id, |r| r.answered(key, add, count))??;
    Some(Then::Send(id, key.clone(), again))
}

/// The request for `key` on line `id` was refused, or taken back.
pub(crate) fn refused(view: &RotulusView, id: u64, key: &Key) {
    redraw(view, id, |r| r.refused(key));
}

/// Do what [`want`] or [`answered`] left to do, on `htlc`.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn then(htlc: *mut c_void, view: &RotulusView, t: Then) {
    use hxhandlers::send::reactions as send;
    match t {
        Then::Send(id, key, add) => {
            if !send::react(htlc, target(view, id), key.clone(), add) {
                refused(view, id, &key);
                crate::reactions::toast(&crate::reactions::refusal(Some(INVALID_KEY), None));
            }
        }
        Then::Withdraw(id, key) => {
            if send::withdraw(htlc, &target(view, id), &key) {
                refused(view, id, &key);
            }
        }
        Then::Say(text) => crate::reactions::toast(&text),
    }
}

/// Draw a page of history: the first since the view was cleared at the
/// end, between dividers, under a "Load older" row while there is more; one
/// "Load older" asked for, above what is there. The lines of
/// [`Output::missed`] a first page drew, to be asked after.
fn draw_history(view: &RotulusView, entries: &[HistoryEntry], has_more: bool) -> Vec<u64> {
    let mut out = output(view).borrow_mut();
    // The scrollback may have shrunk since the last live row, so that a
    // line held then is gone and must not be taken as drawn.
    out.lines.drew(0, view.max_rows());
    let older = std::mem::take(&mut out.loading);
    let missed = if older {
        Vec::new()
    } else {
        std::mem::take(&mut out.missed)
    };
    for e in entries.iter().filter(|e| e.message_id != 0) {
        if out.oldest == 0 || e.message_id < out.oldest {
            out.oldest = e.message_id;
        }
    }
    let load_older = std::mem::replace(&mut out.load_older, std::ptr::null_mut());
    if !load_older.is_null() {
        // SAFETY: the view is live; a mark it no longer holds is refused.
        unsafe { rotulus::ffi::rotulus_view_remove(raw(view), load_older) };
    }
    // A refused "Load older" says there is more, and its row goes back.
    if entries.is_empty() && !(older && has_more) {
        return Vec::new();
    }
    if !older {
        let n = entries.len() as u64;
        let count = trn_argv(
            "chat history (%u message)",
            "chat history (%u messages)",
            n,
            &[&n.to_string()],
        );
        out.anchor = Row::of(
            rotulus::ffi::ROW_DIVIDER,
            &[muted(&format!("─── {count} ───"))],
        )
        .put(view, None);
    }
    if has_more {
        let text = format!("─── ↑ {} ───", tr("Load older messages"));
        out.load_older =
            Row::of(rotulus::ffi::ROW_LOAD_OLDER, &[muted(&text)]).put(view, Some(out.anchor));
    }
    let at = (older && !out.anchor.is_null()).then_some(out.anchor);
    let mut drawn = Vec::new();
    for e in entries {
        // A line taken away since it was drawn still gets its tombstone.
        if e.message_id != 0
            && e.flags & DELETED == 0
            && out.lines.row(Space::History, e.message_id).is_some()
        {
            continue;
        }
        if e.flags & DELETED != 0 {
            let Output { lines, quotes, .. } = &mut *out;
            if let Some(line) = lines.lines.get_mut(&(Space::History, e.message_id)) {
                line.said = None;
                quotes.removed(e.message_id);
            }
        }
        let (nick, text) = (c_str(&e.nick), c_str(&e.text));
        let (gutter, body) = if e.flags & DELETED != 0 {
            (None, "[message removed]".to_owned())
        } else if e.flags & SERVER_MSG != 0 {
            (None, format!("*** {text}"))
        } else if e.flags & ACTION != 0 {
            (None, format!("* {nick} {text}"))
        } else {
            (Some(format!("<{nick}>")), text.to_owned())
        };
        let gutter: Vec<RotulusRun> = gutter.iter().map(|g| muted(g)).collect();
        let row = Row {
            stamp: e.timestamp,
            gutter: &gutter,
            ..Row::of(rotulus::ffi::ROW_HISTORY, &[muted(&body)])
        }
        .put(view, at);
        if let Some(row) = mark(row).filter(|_| e.message_id != 0 && e.flags & DELETED == 0) {
            let r = out.lines.history(e.message_id, row);
            if let Some((s, m)) = &e.reactions {
                r.replace(s.clone(), m);
                view.set_reactions(row, chips(&r.shown()));
            }
            drawn.push((row, e));
        }
    }
    drop(out);
    for (row, e) in drawn {
        quotable(view, (Space::History, e.message_id), e);
        head(view, row, e);
    }
    let out = output(view).borrow_mut();
    if !older {
        let text = format!("─── {} ───", tr("live messages"));
        Row::of(rotulus::ffi::ROW_DIVIDER, &[muted(&text)]).put(view, None);
    }
    let drawn = |id: &u64| out.lines.row(Space::History, *id).is_some();
    missed.into_iter().filter(drawn).collect()
}

// ---- replies -------------------------------------------------------------

/// What a reply to entry `e` quotes: who said it and what. A line the
/// server wrote is quoted as the server's.
fn quote_of(e: &HistoryEntry) -> Quote {
    let nick = match e.flags & SERVER_MSG {
        0 => c_str(&e.nick).to_owned(),
        // TRANSLATORS: who a reply's header says wrote a line the server
        // wrote, such as someone joining.
        _ => tr("server"),
    };
    Quote::new(&nick, c_str(&e.text))
}

/// The lobby's line a reply's `target` names: a kept line by its history
/// id, and one nobody keeps by the chat's own count.
fn lobby_line(target: &Target) -> Option<(Space, u64)> {
    match *target {
        Target::Line { channel: 0, id } => Some((Space::History, id)),
        Target::Ephemeral { chat: 0, id } => Some((Space::Ephemeral, id)),
        _ => None,
    }
}

/// How a reply names the lobby's line `key`.
fn lobby_target((space, id): (Space, u64)) -> Target {
    match space {
        Space::History => Target::Line { channel: 0, id },
        Space::Ephemeral => Target::Ephemeral { chat: 0, id },
    }
}

/// A reply's header: `q`'s nick in the color its own lines are drawn in.
fn quoting(view: &RotulusView, q: &Quote) -> Reply {
    let own = crate::chat_view::conn_of(view)
        .map(crate::dock::htlc_for)
        .filter(|h| !h.is_null())
        // SAFETY: main thread; a connection the dock holds is live.
        .map(|h| unsafe { own_nick(h) })
        .unwrap_or_default();
    let nick = q.nick.as_bytes();
    // SAFETY: the nick is read for the call.
    let color =
        unsafe { hx_chat_nick_color(nick.as_ptr().cast(), nick.len(), (nick == own).into()) };
    Reply::Quote {
        nick: q.nick.clone(),
        color: u8::try_from(color).map_or(ColorRef::Default, ColorRef::Palette),
        excerpt: q.excerpt.clone(),
    }
}

/// The header of a reply whose target the view could not quote itself.
fn unquoted(q: Option<&Quoted>) -> Reply {
    Reply::Unavailable(match q {
        // TRANSLATORS: a reply's header, where the line it answers has
        // been taken away.
        Some(Quoted::Removed) => tr("message removed"),
        // TRANSLATORS: a reply's header, where the line it answers was
        // said by someone the user ignores.
        Some(Quoted::Hidden) => tr("a message you've hidden"),
        // TRANSLATORS: a reply's header, where the line it answers is not
        // shown here, or not yet.
        _ => tr("earlier message"),
    })
}

/// Line `key`, drawn from entry `e`, is one a reply quotes as `e` says;
/// the replies already drawn that wait for it are headed with it.
fn quotable(view: &RotulusView, key: (Space, u64), e: &HistoryEntry) {
    let q = quote_of(e);
    let rows = {
        let mut out = output(view).borrow_mut();
        let Some(line) = out.lines.lines.get_mut(&key) else {
            return;
        };
        line.said.get_or_insert(q.clone());
        let lobby = cid_of(view) == 0;
        let rows = lobby.then(|| out.lines.waiting.remove(&lobby_target(key)));
        rows.flatten().unwrap_or_default()
    };
    let reply = quoting(view, &q);
    for row in rows {
        view.set_reply(row, Some(reply.clone()));
    }
}

/// Head `row`, entry `e`'s, with the line it answers, where it is a reply
/// to a lobby line: quoted from the view's own row of it, or from what the
/// server said of it; and until then, or where nothing can be said, as an
/// earlier message, a kept line asked for by id once the draw is done.
fn head(view: &RotulusView, row: MessageId, e: &HistoryEntry) {
    let Some(target) = e.reply_to.as_ref().filter(|_| cid_of(view) == 0) else {
        return;
    };
    let Some(key) = lobby_line(target) else {
        return;
    };
    let quote = {
        let mut out = output(view).borrow_mut();
        let drawn = out.lines.lines.get(&key).and_then(|l| l.said.clone());
        if drawn.is_none() {
            out.lines
                .waiting
                .entry(target.clone())
                .or_default()
                .push(row);
        }
        match (drawn, key) {
            (Some(q), _) => Ok(q),
            (None, (Space::Ephemeral, _)) => Err(None),
            (None, (Space::History, id)) => match out.quotes.get(id).cloned() {
                Some(Quoted::Line(q)) => Ok(q),
                Some(q @ (Quoted::Removed | Quoted::Hidden | Quoted::Unavailable)) => Err(Some(q)),
                Some(Quoted::Later) | None => {
                    out.quotes.want(id);
                    Err(None)
                }
            },
        }
    };
    let reply = match &quote {
        Ok(q) => quoting(view, q),
        Err(q) => unquoted(q.as_ref()),
    };
    if !view.set_reply(row, Some(reply)) {
        let mut out = output(view).borrow_mut();
        if let Some(rows) = out.lines.waiting.get_mut(target) {
            rows.retain(|&r| r != row);
            if rows.is_empty() {
                out.lines.waiting.remove(target);
            }
        }
    }
}

/// The quotes to ask for next, those the view has drawn since, or no
/// longer holds a reply to, aside.
fn due_quotes(view: &RotulusView) -> Option<Vec<u64>> {
    let mut out = output(view).borrow_mut();
    let Output { lines, quotes, .. } = &mut *out;
    quotes.next(|id| {
        let target = Target::Line { channel: 0, id };
        lines.row(Space::History, id).is_some() || !lines.waiting.contains_key(&target)
    })
}

/// Ask for the quotes the view wants once the main loop is idle, so that
/// a page's replies, or a burst of live ones, go in one request. Called
/// where what the server says is heard, never from a draw.
fn ask_soon(view: &RotulusView) {
    if std::mem::replace(&mut output(view).borrow_mut().asking, true) {
        return;
    }
    let view = view.downgrade();
    glib::idle_add_local_once(move || {
        if let Some(view) = view.upgrade() {
            output(&view).borrow_mut().asking = false;
            ask(&view);
        }
    });
}

/// The connection `view` shows, or NULL.
fn htlc_of(view: &RotulusView) -> *mut c_void {
    crate::chat_view::conn_of(view).map_or(std::ptr::null_mut(), crate::dock::htlc_for)
}

/// Ask for the quotes the view wants, unless the first page is on its way,
/// which most likely holds them and asks once it is drawn.
fn ask(view: &RotulusView) {
    let htlc = htlc_of(view);
    if htlc.is_null() || hxhandlers::recv::chat::first_page_pending(htlc, 0) {
        return;
    }
    let Some(ids) = due_quotes(view) else {
        return;
    };
    // SAFETY: main thread; the dock's connection is live.
    if !unsafe { hxhandlers::send::chat_history::quotes(htlc, 0, &ids) } {
        let r = output(view).borrow_mut().quotes.refused(false);
        settled(view, r);
    }
}

/// What lines asked for by id hold now: the replies waiting for them are
/// headed so, and those that can be quoted wait no more.
fn quoted(view: &RotulusView, now: Vec<(u64, Quoted)>) {
    for (id, q) in now {
        let target = Target::Line { channel: 0, id };
        let rows = {
            let mut out = output(view).borrow_mut();
            match q {
                Quoted::Unavailable | Quoted::Later => out.lines.waiting.get(&target).cloned(),
                _ => out.lines.waiting.remove(&target),
            }
        };
        let reply = match &q {
            Quoted::Line(line) => quoting(view, line),
            q => unquoted(Some(q)),
        };
        let gone: Vec<_> = (rows.unwrap_or_default().into_iter())
            .filter(|&row| !view.set_reply(row, Some(reply.clone())))
            .collect();
        let mut out = output(view).borrow_mut();
        if let Some(rows) = out.lines.waiting.get_mut(&target) {
            rows.retain(|r| !gone.contains(r));
            if rows.is_empty() {
                out.lines.waiting.remove(&target);
            }
        }
    }
}

fn settled(view: &RotulusView, r: Refused) {
    if let Refused::Settled(now) = r {
        quoted(view, now);
    }
}

/// Whether someone who now goes by `nick` is ignored on `htlc`.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
unsafe fn ignored(htlc: *mut c_void, nick: &str) -> bool {
    let conv = chat_with_cid(hx_conn_sess(htlc.cast()), 0);
    let uid = uid_of(htlc, 0, nick.as_bytes());
    uid != 0
        && hxmodel::chat_members::hx_member_model_get_ignore(hx_chat_member_model(conv.cast()), uid)
            != 0
}

/// What the `replies` signal says: the answer to a request for quotes, or
/// its refusal, which is never shown; one put off for the rate is asked
/// again after a second.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
unsafe fn replies(htlc: *mut c_void, e: &gtkhx_core::replies::Event) {
    use gtkhx_core::replies::Event;
    let (Event::Quotes { cid, asked, .. } | Event::Refused { cid, asked, .. }) = e;
    let Some(view) = view_of(htlc, *cid) else {
        return;
    };
    if !output(&view).borrow().quotes.in_flight(asked) {
        return;
    }
    match e {
        Event::Quotes {
            entries, has_more, ..
        } => {
            let got = (entries.iter())
                .map(|e| {
                    let q = if e.flags & DELETED != 0 {
                        Quoted::Removed
                    } else if ignored(htlc, &e.nick) {
                        Quoted::Hidden
                    } else {
                        Quoted::Line(quote_of(e))
                    };
                    (e.message_id, q)
                })
                .collect();
            let now = output(&view).borrow_mut().quotes.answered(got, *has_more);
            quoted(&view, now);
            ask_soon(&view);
        }
        Event::Refused { slow, .. } => {
            let r = output(&view).borrow_mut().quotes.refused(*slow);
            if r != Refused::Retry {
                return settled(&view, r);
            }
            let view = view.downgrade();
            glib::timeout_add_local_once(Duration::from_secs(1), move || {
                let Some(view) = view.upgrade() else {
                    return;
                };
                let (htlc, ids) = (htlc_of(&view), output(&view).borrow().quotes.again());
                let Some(ids) = ids.filter(|_| !htlc.is_null()) else {
                    return;
                };
                // SAFETY: main thread; the dock's connection is live.
                if !unsafe { hxhandlers::send::chat_history::quotes(htlc, 0, &ids) } {
                    let r = output(&view).borrow_mut().quotes.refused(false);
                    settled(&view, r);
                }
            });
        }
    }
}

/// "Load older" was asked for: its row says so until the page comes.
fn loading_older(view: &RotulusView, out: &mut Output) {
    out.loading = true;
    if out.load_older.is_null() {
        return;
    }
    // SAFETY: the view is live; a mark it no longer holds is refused.
    unsafe { rotulus::ffi::rotulus_view_remove(raw(view), out.load_older) };
    let text = format!("─── ↑ {} ───", tr("Loading older messages..."));
    out.load_older =
        Row::of(rotulus::ffi::ROW_DIVIDER, &[muted(&text)]).put(view, Some(out.anchor));
}

/// A click on "Load older" in chat `cid`: ask for the page before the
/// oldest line drawn, one at a time.
fn load_older(view: &RotulusView, cid: u32) {
    let (oldest, loading) = {
        let out = output(view).borrow();
        (out.oldest, out.loading)
    };
    if loading {
        debug(&format!(
            "Load-older click: fetch already in flight for cid={cid}"
        ));
        return;
    }
    let htlc = crate::chat_view::conn_of(view).map_or(std::ptr::null_mut(), crate::dock::htlc_for);
    // SAFETY: main thread; a NULL or closed connection sends nothing.
    if unsafe { hxhandlers::send::chat_history::hx_chat_history_fetch_older(htlc, cid, oldest) }
        == 0
    {
        return;
    }
    loading_older(view, &mut output(view).borrow_mut());
}

fn debug(line: &str) {
    if let Ok(line) = CString::new(line) {
        unsafe { debug_log_str(c"chat-history".as_ptr(), line.as_ptr()) };
    }
}

/// Empty `view` and forget what it drew.
pub(crate) fn clear(view: &RotulusView) {
    view.clear();
    *output(view).borrow_mut() = Output::default();
}

/// `void gtkhx_chat_output_clear (GtkWidget *view)`.
///
/// # Safety
/// `view` is NULL or a `RotulusView`. Main thread.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_chat_output_clear(view: *mut gtk::ffi::GtkWidget) {
    if let Some(view) = rotulus_view(view) {
        clear(&view);
    }
}

/// # Safety
/// `w` is NULL or a live widget.
unsafe fn rotulus_view(w: *mut gtk::ffi::GtkWidget) -> Option<RotulusView> {
    if w.is_null() {
        return None;
    }
    // A plain reference, not from_glib_none: that sinks a floating one.
    let obj: glib::Object =
        glib::translate::from_glib_full(glib::gobject_ffi::g_object_ref(w as *mut _));
    obj.downcast().ok()
}

// ---- what reaches it ------------------------------------------------------

/// The output of chat `cid` on `htlc`'s connection, while it has a window.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
unsafe fn view_of(htlc: *mut c_void, cid: u32) -> Option<RotulusView> {
    let sess = hx_conn_sess(htlc.cast());
    if sess.is_null() {
        return None;
    }
    let conv = chat_with_cid(sess, cid);
    if conv.is_null() {
        return None;
    }
    let view = rotulus_view(hx_gchat_output(hx_chat_view(conv.cast())))?;
    hear(&view, cid);
    Some(view)
}

/// Our own nick on `htlc`, or empty.
unsafe fn own_nick(htlc: *mut c_void) -> Vec<u8> {
    let p = hx_conn_name(htlc.cast());
    if p.is_null() {
        Vec::new()
    } else {
        CStr::from_ptr(p).to_bytes().to_vec()
    }
}

/// Whether `body` mentions our nick or a word the user listed.
unsafe fn mentions(htlc: *mut c_void, body: &[u8]) -> bool {
    let own = own_nick(htlc);
    let listed =
        hxconfig::ffi::with_settings(|s| s.chat.highlight_words.clone()).unwrap_or_default();
    let words: Vec<CString> = std::iter::once(&own[..])
        .chain(
            listed
                .iter()
                .flat_map(|w| w.split(','))
                .map(|w| w.trim_ascii().as_bytes()),
        )
        .filter(|w| !w.is_empty())
        .filter_map(|w| CString::new(w).ok())
        .collect();
    let mut ptrs: Vec<*const c_char> = words.iter().map(|w| w.as_ptr()).collect();
    ptrs.push(std::ptr::null());
    hx_highlight_match(body.as_ptr().cast(), body.len(), ptrs.as_ptr()) != 0
}

/// Who said a line: the uid the server sent with it, or else whoever in
/// chat `cid` goes by its nick. Nobody when that misses too, rather than a
/// guess that would show someone else's avatar.
unsafe fn speaker(htlc: *mut c_void, cid: u32, uid: u16, nick: &[u8]) -> RotulusSpeaker {
    if nick.is_empty() {
        return NOBODY;
    }
    let key = match uid {
        0 => uid_of(htlc, cid, nick),
        uid => uid,
    };
    if key == 0 {
        return NOBODY;
    }
    RotulusSpeaker {
        key: u64::from(key),
        nick: nick.as_ptr().cast(),
        nick_len: nick.len() as i32,
    }
}

/// Whoever in chat `cid` goes by `nick`, or 0.
unsafe fn uid_of(htlc: *mut c_void, cid: u32, nick: &[u8]) -> u16 {
    let conv = chat_with_cid(hx_conn_sess(htlc.cast()), cid);
    if conv.is_null() {
        return 0;
    }
    let name = CString::new(nick.split(|&b| b == 0).next().unwrap_or_default()).unwrap_or_default();
    hx_member_model_find_by_name(hx_chat_member_model(conv.cast()), name.as_ptr())
}

/// A line `nick` said in chat `cid`, as it is drawn.
unsafe fn said<'a>(
    htlc: *mut c_void,
    cid: u32,
    uid: u16,
    nick: &'a [u8],
    body: &'a [u8],
    is_self: bool,
) -> Said<'a> {
    Said {
        nick,
        body,
        is_self,
        highlight: !nick.is_empty() && !is_self && !body.is_empty() && mentions(htlc, body),
        nick_color: hx_chat_nick_color(nick.as_ptr().cast(), nick.len(), is_self.into()),
        speaker: speaker(htlc, cid, uid, nick),
    }
}

/// A multi-line body as one, without the "nick:" the server put again at
/// the head of each line after the first.
fn join_body(body: &[u8], sender: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(body.len());
    let mut lines = body.split(|&b| b == b'\n').enumerate().peekable();
    while let Some((i, l)) = lines.next() {
        if i == 0 {
            out.extend_from_slice(l);
            continue;
        }
        // A final newline ends the body; it starts no line.
        if lines.peek().is_none() && l.is_empty() {
            break;
        }
        out.push(b'\n');
        match split_nick_body(l) {
            Some((so, sl, bo, bl)) if !sender.is_empty() && &l[so..so + sl] == sender => {
                out.extend_from_slice(&l[bo..bo + bl])
            }
            _ => out.extend_from_slice(l),
        }
    }
    out
}

/// A chat line.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
unsafe fn chat(htlc: *mut c_void, e: &HxChatEvent, entry: Option<&HistoryEntry>) {
    if let Some(view) = view_of(htlc, e.cid) {
        let line = std::slice::from_raw_parts(e.line as *const u8, e.line_len);
        if e.sender_len == 0 {
            draw_live(&view, entry, || draw_prose(&view, line));
        } else {
            let sender = &line[e.sender_off..e.sender_off + e.sender_len];
            let mut body = join_body(&line[e.body_off..e.body_off + e.body_len], sender);
            // An attached picture with no caption is sent as "[image]",
            // which the picture's own row says better.
            if !e.media.is_null() && body == b"[image]" {
                body.clear();
            }
            let is_self = e.is_self != 0;
            let s = said(htlc, e.cid, e.uid, sender, &body, is_self);
            draw_said_live(&view, entry, &s, || {
                if e.media.is_null() {
                    return 0;
                }
                let conv = chat_with_cid(hx_conn_sess(htlc.cast()), e.cid);
                crate::inline_media_row::hx_inline_media_row_append(
                    raw(&view),
                    hx_chat_media_table(conv.cast()),
                    e.media,
                    htlc,
                );
                1
            });
            // A private chat's tab asks for attention; the public chat,
            // shown by default and busy, doesn't.
            if e.cid != 0 && !is_self {
                crate::chat_tabs::gtkhx_chat_tabs_set_attention_pchat(
                    htlc,
                    e.cid,
                    glib::ffi::GTRUE,
                );
            }
        }
        ask_soon(&view);
    }
    gtkhx_notify_chat(htlc, e);
}

/// A line GtkHx writes into chat `cid`: each of its lines a row, tagged
/// when `tag` is not empty.
///
/// # Safety
/// Main thread; `htlc` is a live connection; `tag` is NULL or a C string.
unsafe fn log_line(htlc: *mut c_void, cid: u32, tag: *const c_char, color: i16, text: &CStr) {
    let Some(view) = view_of(htlc, cid) else {
        return;
    };
    let tag = if tag.is_null() {
        &[][..]
    } else {
        CStr::from_ptr(tag).to_bytes()
    };
    for line in log_lines(text) {
        let line = hxproto::text::to_utf8(line);
        let line = line.as_bytes();
        draw_live(&view, None, || {
            if !tag.is_empty() {
                return draw_tagged(&view, tag, color, line);
            }
            if let Some((so, sl, bo, bl)) = split_nick_body(line) {
                let nick = &line[so..so + sl];
                let is_self = !nick.is_empty() && nick == own_nick(htlc);
                return draw_said(
                    &view,
                    &said(htlc, cid, 0, nick, &line[bo..bo + bl], is_self),
                );
            }
            draw_said(
                &view,
                &Said {
                    nick: &[],
                    body: line,
                    is_self: false,
                    highlight: false,
                    nick_color: 0,
                    speaker: NOBODY,
                },
            )
        });
    }
}

/// The lines of a log line's text, each a row: none after a final newline,
/// and an empty one drawn from the byte after it, the newline or the end,
/// so that it still takes a row.
fn log_lines(text: &CStr) -> Vec<&[u8]> {
    let bytes = text.to_bytes_with_nul();
    let end = bytes.len() - 1;
    let mut lines = Vec::new();
    let mut start = 0;
    loop {
        let stop = bytes[start..end]
            .iter()
            .position(|&b| b == b'\n')
            .map(|i| start + i);
        let len = stop.unwrap_or(end) - start;
        lines.push(&bytes[start..start + len.max(1)]);
        match stop {
            Some(nl) if nl + 1 < end => start = nl + 1,
            _ => return lines,
        }
    }
}

/// What the `reactions` signal says, told to the view of the chat it is
/// about, where that chat has one.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
unsafe fn reactions(htlc: *mut c_void, e: &gtkhx_core::reactions::Event) {
    use gtkhx_core::reactions::Event;
    let target = match e {
        Event::Who { ask, nicks, count } => {
            // Nicks are what each reacted under: whoever online goes by
            // one now is the best guess at whose icon it is.
            let lobby = view_of(htlc, 0);
            let who: Vec<_> = (nicks.iter())
                .map(|n| {
                    let uid = uid_of(htlc, 0, n.as_bytes());
                    let icon = lobby
                        .as_ref()
                        .and_then(|v| crate::chat_view::avatar(v, uid));
                    (n.clone(), icon)
                })
                .collect();
            return crate::reactions::heard(*ask, &who, *count);
        }
        Event::Now { target, .. }
        | Event::Acted { target, .. }
        | Event::PutOff { target, .. }
        | Event::Refused { target, .. }
        | Event::Changed { target, .. } => target,
    };
    if let Target::Im { guid, .. } = target {
        return crate::msg::reacted(htlc, guid, e);
    }
    let Some((cid, id)) = line_of(target) else {
        return;
    };
    let view = view_of(htlc, cid);
    // The user hears why their own act was refused even once the chat it
    // was in has closed.
    if let Event::Refused {
        key, reason, error, ..
    } = e
    {
        let text = match key {
            Some(key) => {
                view.inspect(|v| refused(v, id, key));
                crate::reactions::refusal(*reason, error.as_deref())
            }
            None => crate::reactions::clear_refusal(*reason, error.as_deref()),
        };
        return crate::reactions::toast(&text);
    }
    let Some(view) = view else {
        return;
    };
    match e {
        Event::Now { reactions, .. } => {
            let page_due = hxhandlers::recv::chat::first_page_pending(htlc, cid);
            reacted(&view, id, reactions.as_ref(), page_due);
        }
        Event::Acted {
            key, add, count, ..
        } => {
            if let Some(t) = answered(&view, id, key, *add, *count) {
                then(htlc, &view, t);
            }
        }
        Event::PutOff { key, add, .. } => {
            if redraw(&view, id, |r| r.put_off(key)) == Some(true) {
                hxhandlers::send::reactions::retry(htlc, target.clone(), key.clone(), *add);
            }
        }
        Event::Refused { .. } | Event::Who { .. } | Event::Changed { .. } => {}
    }
}

/// `void gtkhx_chat_output_install (void)` — hear what the chat views
/// draw. Once.
#[no_mangle]
pub extern "C" fn gtkhx_chat_output_install() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        gtkhx_core::session::connect_chat(|htlc, e, entry| unsafe { chat(htlc, e, entry) });
        gtkhx_core::session::connect_chat_history(|htlc, cid, entries, has_more| unsafe {
            if let Some(view) = view_of(htlc, cid) {
                let missed = draw_history(&view, entries, has_more);
                ask_soon(&view);
                let lines: Vec<Target> = missed.into_iter().map(|id| target(&view, id)).collect();
                hxhandlers::send::reactions::refresh(htlc, &lines);
            }
        });
        gtkhx_core::session::connect_reactions(|htlc, e| unsafe { reactions(htlc, e) });
        gtkhx_core::session::connect_replies(|htlc, e| unsafe { replies(htlc, e) });
        let session: glib::Object = unsafe {
            from_glib_none(
                gtkhx_core::session::gtkhx_session_get_default() as *mut glib::gobject_ffi::GObject
            )
        };
        session.connect_local("chat-log-line", false, |args| {
            let ptr = |i: usize| args[i].get::<*mut c_void>().unwrap_or(std::ptr::null_mut());
            let (htlc, tag, text) = (ptr(1), ptr(3), ptr(5));
            if !htlc.is_null() && !text.is_null() {
                let cid = args[2].get::<u32>().unwrap_or(0);
                let color = args[4].get::<i32>().unwrap_or(0) as i16;
                unsafe { log_line(htlc, cid, tag.cast(), color, CStr::from_ptr(text.cast())) };
            }
            None
        });
    });
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    fn entry(message_id: u64, flags: u16, nick: &str, text: &str) -> HistoryEntry {
        HistoryEntry {
            message_id,
            timestamp: 1_000_000_000 + message_id as i64 * 86_400,
            flags,
            icon: 0,
            nick: nick.into(),
            text: text.into(),
            reactions: None,
            reply_to: None,
        }
    }

    /// The day `entry(id, ..)` is stamped with, as the test view shows it.
    fn day(id: u64) -> String {
        let ts = 1_000_000_000 + id as i64 * 86_400;
        let dt = glib::DateTime::from_unix_local(ts).unwrap();
        dt.format("%F ").unwrap().to_string()
    }

    fn today() -> String {
        glib::DateTime::now_local()
            .unwrap()
            .format("%F ")
            .unwrap()
            .to_string()
    }

    /// A view that copies each row with its day in front.
    fn view() -> RotulusView {
        let view = RotulusView::new();
        view.set_copy_timestamps(true);
        view.set_stamp_format("%F ");
        view
    }

    /// What the view holds, a row a line, as it copies.
    fn rows(view: &RotulusView) -> String {
        view.select_all();
        view.selected_text()
    }

    fn say(view: &RotulusView, nick: &str, body: &str) -> u64 {
        draw_said(
            view,
            &Said {
                nick: nick.as_bytes(),
                body: body.as_bytes(),
                is_self: false,
                highlight: false,
                nick_color: PAL_NICK_BRACKET,
                speaker: NOBODY,
            },
        )
    }

    /// The first page between its dividers under "Load older", each later
    /// page above what is there, the "Load older" row on top while there is
    /// more, and "Load older" asking before the oldest line drawn.
    pub(crate) fn check_older_pages_go_above_the_first() {
        let view = view();
        let now = today();
        draw_tagged(&view, b"hx", 43, b"login successful");
        let first = [
            entry(10, 0, "alice", "ten"),
            entry(11, ACTION, "bob", "waves"),
            entry(12, SERVER_MSG, "", "news"),
            entry(13, DELETED, "", ""),
            entry(14, 0, "carol", "four\nteen"),
        ];
        draw_history(&view, &first, true);
        say(&view, "dave", "live");
        let older = |rows: &str| {
            format!(
                "{now}[hx] login successful\n\
                 {rows}\
                 {now}─── chat history (5 messages) ───\n\
                 {}<alice> ten\n\
                 {}* bob waves\n\
                 {}*** news\n\
                 {}[message removed]\n\
                 {}<carol> four\nteen\n\
                 {now}─── live messages ───\n\
                 {now}<dave> live",
                day(10),
                day(11),
                day(12),
                day(13),
                day(14)
            )
        };
        assert_eq!(
            rows(&view),
            older(&format!("{now}─── ↑ Load older messages ───\n"))
        );
        assert_eq!(output(&view).borrow().oldest, 10);

        loading_older(&view, &mut output(&view).borrow_mut());
        assert_eq!(
            rows(&view),
            older(&format!("{now}─── ↑ Loading older messages... ───\n"))
        );
        // Refused: the row comes back, to be clicked again.
        draw_history(&view, &[], true);
        assert_eq!(
            rows(&view),
            older(&format!("{now}─── ↑ Load older messages ───\n"))
        );

        loading_older(&view, &mut output(&view).borrow_mut());
        draw_history(
            &view,
            &[entry(8, 0, "eve", "eight"), entry(9, 0, "eve", "nine")],
            true,
        );
        let page = format!("{}<eve> eight\n{}<eve> nine\n", day(8), day(9));
        assert_eq!(
            rows(&view),
            older(&format!("{now}─── ↑ Load older messages ───\n{page}"))
        );
        assert_eq!(output(&view).borrow().oldest, 8);

        loading_older(&view, &mut output(&view).borrow_mut());
        draw_history(&view, &[entry(7, 0, "eve", "seven")], false);
        // Every older page goes just above the first page's divider, so a
        // third page lands below the second.
        assert_eq!(
            rows(&view),
            older(&format!("{page}{}<eve> seven\n", day(7)))
        );
    }

    /// Server prose is a row a line, and a line said with no nick has no
    /// nick column.
    pub(crate) fn check_server_prose_takes_a_row_a_line() {
        let view = view();
        let now = today();
        draw_prose(&view, b" *** alice dances\n\nmore\n");
        say(&view, "", "a server's words");
        assert_eq!(
            rows(&view),
            format!("{now} *** alice dances\n{now}\n{now}more\n{now}a server's words")
        );
    }

    /// A line drawn live while the first page was on its way is not drawn
    /// again by the page, unless it has been taken away since, and each
    /// line's row can be found by its id: a history id's among the
    /// history's, a chat's own count's apart.
    pub(crate) fn check_a_line_has_one_row_found_by_its_id() {
        let view = view();
        let now = today();
        draw_live(&view, Some(&entry(7, 0, "a", "seven")), || {
            say(&view, "a", "seven")
        });
        draw_live(&view, Some(&entry(8, EPHEMERAL, "a", "own")), || {
            say(&view, "a", "own")
        });
        draw_live(&view, Some(&entry(9, 0, "a", "nine")), || {
            say(&view, "a", "nine")
        });
        let live = output(&view).borrow().lines.row(Space::History, 7);
        assert!(live.is_some());
        draw_history(
            &view,
            &[
                entry(6, 0, "a", "six"),
                entry(7, 0, "a", "seven"),
                entry(8, 0, "a", "eight"),
                entry(9, DELETED, "", ""),
            ],
            false,
        );
        assert_eq!(
            rows(&view),
            format!(
                "{now}<a> seven\n{now}<a> own\n{now}<a> nine\n\
                 {now}─── chat history (4 messages) ───\n\
                 {}<a> six\n{}<a> eight\n{}[message removed]\n\
                 {now}─── live messages ───",
                day(6),
                day(8),
                day(9)
            )
        );
        let out = output(&view).borrow();
        assert_eq!(out.lines.row(Space::History, 7), live);
        for (space, id, held) in [
            (Space::History, 6, true),
            (Space::History, 8, true),
            (Space::Ephemeral, 8, true),
            (Space::History, 9, true),
            (Space::Ephemeral, 7, false),
        ] {
            assert_eq!(out.lines.row(space, id).is_some(), held, "{space:?} {id}");
        }
        assert_ne!(
            out.lines.row(Space::History, 8),
            out.lines.row(Space::Ephemeral, 8)
        );
    }

    /// Past the scrollback the view drops its oldest live rows, those that
    /// hold no line among them, and the lines' ids go with their rows, at
    /// once when the scrollback shrinks; history rows stay.
    pub(crate) fn check_trimmed_live_rows_are_let_go() {
        let view = view();
        view.set_max_rows(3);
        draw_history(&view, &[entry(1, 0, "a", "kept")], false);
        let line = |id: u64| {
            draw_live(&view, Some(&entry(id, 0, "a", "")), || {
                say(&view, "a", &format!("line {id}"))
            });
        };
        // Each line is held while its row is shown, and only then.
        let agree = || {
            let shown = rows(&view);
            assert!(shown.contains("<a> kept"), "{shown}");
            let out = output(&view).borrow();
            assert!(out.lines.row(Space::History, 1).is_some());
            for id in 10..14 {
                let on_screen = shown.contains(&format!("<a> line {id}"));
                let held = out.lines.row(Space::History, id).is_some();
                assert_eq!(held, on_screen, "{id}: {shown}");
            }
        };
        line(10);
        draw_live(&view, None, || draw_prose(&view, b"two\nrows\n"));
        agree();
        for id in 11..14 {
            line(id);
            agree();
        }

        // A line trimmed by a smaller scrollback is drawn again by a page.
        let shrunk = super::tests::view();
        shrunk.set_max_rows(5);
        draw_live(&shrunk, Some(&entry(20, 0, "a", "")), || {
            say(&shrunk, "a", "twenty")
        });
        draw_live(&shrunk, None, || draw_prose(&shrunk, b"three\nmore\nrows"));
        shrunk.set_max_rows(3);
        shrunk.set_max_rows(5);
        draw_history(&shrunk, &[entry(20, 0, "a", "twenty")], false);
        assert!(rows(&shrunk).contains("<a> twenty"), "{}", rows(&shrunk));
    }

    /// Clearing the view starts it over: the next page is a first page,
    /// even one asked for as an older one before the clear.
    pub(crate) fn check_a_cleared_view_starts_over() {
        let view = view();
        let now = today();
        draw_history(&view, &[entry(5, 0, "a", "five")], true);
        loading_older(&view, &mut output(&view).borrow_mut());
        clear(&view);
        assert_eq!(output(&view).borrow().lines.row(Space::History, 5), None);
        draw_history(&view, &[entry(5, 0, "a", "five")], false);
        assert_eq!(
            rows(&view),
            format!(
                "{now}─── chat history (1 message) ───\n{}<a> five\n{now}─── live messages ───",
                day(5)
            )
        );
    }

    fn thumbs(n: u32) -> (Summary, Mine) {
        let summary = Summary {
            entries: vec![(Key::Emoji("\u{1F44D}".into()), n)],
        };
        (summary, Mine::default())
    }

    /// A change of reactions reaches a line drawn; while the first page is
    /// on its way, one to a line not drawn yet is kept, at most so many,
    /// and asked after again once the page draws it, and never for an older
    /// page. A line whose row has gone is let go of.
    pub(crate) fn check_a_page_read_before_a_change_is_asked_again() {
        let view = view();
        draw_live(&view, Some(&entry(5, 0, "a", "five")), || {
            say(&view, "a", "five")
        });
        for (id, due) in [(5, true), (7, true), (7, true), (99, true), (8, false)] {
            reacted(&view, id, Some(&thumbs(2)), due);
        }
        assert_eq!(output(&view).borrow().missed, [7, 99]);
        let page = [
            entry(5, 0, "a", "five"),
            entry(7, 0, "a", "seven"),
            entry(8, 0, "a", ""),
        ];
        assert_eq!(draw_history(&view, &page, true), [7]);
        assert_eq!(output(&view).borrow().missed, [0u64; 0]);
        reacted(&view, 3, None, true);
        loading_older(&view, &mut output(&view).borrow_mut());
        assert_eq!(
            draw_history(&view, &[entry(3, 0, "a", "")], false),
            [0u64; 0]
        );

        let seven = output(&view).borrow().lines.row(Space::History, 7).unwrap();
        view.remove(seven);
        reacted(&view, 7, None, false);
        assert_eq!(output(&view).borrow().lines.row(Space::History, 7), None);

        let full = super::tests::view();
        for id in 0..MISSED as u64 + 1 {
            reacted(&full, id, None, true);
        }
        assert_eq!(output(&full).borrow().missed.len(), MISSED);
    }

    /// A line's chips go on its own row: a history line's from its entry,
    /// and a live one's on its words, not on the picture under them.
    pub(crate) fn check_chips_go_on_the_lines_row() {
        let view = view();
        let seeded = HistoryEntry {
            reactions: Some(thumbs(2)),
            ..entry(4, 0, "a", "four")
        };
        draw_history(&view, &[seeded], false);
        let five = Said {
            nick: b"a",
            body: b"five",
            is_self: false,
            highlight: false,
            nick_color: PAL_NICK_BRACKET,
            speaker: NOBODY,
        };
        draw_said_live(&view, Some(&entry(5, 0, "a", "five")), &five, || {
            draw_prose(&view, b"[picture]")
        });
        reacted(&view, 5, Some(&thumbs(3)), false);
        let counts = |row| {
            view.reactions_of(row)
                .iter()
                .map(|r| r.count)
                .collect::<Vec<_>>()
        };
        let row = |id| {
            output(&view)
                .borrow()
                .lines
                .row(Space::History, id)
                .unwrap()
        };
        assert_eq!(counts(row(4)), [2]);
        assert_eq!(counts(row(5)), [3]);
        // SAFETY: the view is live.
        let picture = mark(unsafe { rotulus::ffi::rotulus_view_get_last(raw(&view)) }).unwrap();
        assert_eq!(counts(picture), [0u32; 0]);
    }

    /// A click on a chip asks for it and shows it at once, a second while
    /// it is asked takes it back, and the answer asks for what the user
    /// wants since; past the login's limits, and on a server emoji's
    /// placeholder, the user is told why not instead. A row no line is on,
    /// or one the server takes no reactions to, is none of this.
    pub(crate) fn check_a_chip_clicked_asks_for_it_and_shows_it() {
        use hxproto::reactions::{Limits, TARGETS_LINES, TOO_MANY_KEYS};
        let (up, heart) = ("\u{1F44D}", "\u{2764}\u{FE0F}");
        let key = |e: &str| Key::Emoji(e.into());
        let limits = |max_keys| Limits {
            targets: TARGETS_LINES,
            max_keys,
            max_per_user: None,
        };
        let view = view();
        let seeded = HistoryEntry {
            reactions: Some(thumbs(2)),
            ..entry(4, 0, "a", "four")
        };
        draw_history(&view, &[seeded], false);
        let row = output(&view).borrow().lines.row(Space::History, 4).unwrap();
        let shown = || {
            (view.reactions_of(row).into_iter())
                .map(|r| (r.key, r.count, r.mine))
                .collect::<Vec<_>>()
        };
        let ups = |n, mine| vec![(up.to_owned(), n, mine)];
        let any = limits(None);

        assert_eq!(
            want(&view, row, up, None, &any),
            Some(Then::Send(4, key(up), true))
        );
        assert_eq!(shown(), ups(3, true));
        assert_eq!(
            want(&view, row, up, None, &any),
            Some(Then::Withdraw(4, key(up)))
        );
        assert_eq!(shown(), ups(2, false));
        let again = answered(&view, 4, &key(up), true, Some(3));
        assert_eq!(again, Some(Then::Send(4, key(up), false)));
        assert_eq!(shown(), ups(2, false));
        assert_eq!(answered(&view, 4, &key(up), false, Some(2)), None);

        let full = Then::Say(crate::reactions::refusal(Some(TOO_MANY_KEYS), None));
        assert_eq!(
            want(&view, row, heart, Some(true), &limits(Some(1))),
            Some(full)
        );
        let placeholder = want(&view, row, "server-emoji:9", None, &any);
        assert_eq!(
            placeholder,
            Some(Then::Say(crate::reactions::server_emoji()))
        );
        assert_eq!(shown(), ups(2, false));

        assert_eq!(
            want(&view, row, heart, Some(true), &any),
            Some(Then::Send(4, key(heart), true))
        );
        refused(&view, 4, &key(heart));
        assert_eq!(shown(), ups(2, false));

        draw_live(&view, None, || say(&view, "a", "no id"));
        // SAFETY: the view is live.
        let bare = mark(unsafe { rotulus::ffi::rotulus_view_get_last(raw(&view)) }).unwrap();
        let none = Limits { targets: 0, ..any };
        for (row, limits) in [(bare, &any), (row, &none)] {
            assert_eq!(want(&view, row, up, None, limits), None);
            assert_eq!(line_on(&view, row, limits), None);
        }
    }

    /// A private chat's kept line, a room's on hxd-ng, is named kind 4
    /// under the chat's id, and is a line to react to where the login takes
    /// kind 4, as a lobby line is where it takes kind 1; a line under its
    /// chat's own count is none, in either.
    pub(crate) fn check_a_private_chats_kept_line_is_kind_4() {
        use hxproto::reactions::{Limits, TARGETS_EPHEMERAL, TARGETS_LINES};
        let (lobby, room) = (view(), view());
        // As `hear` keeps it, without hearing what would send.
        unsafe { room.set_data(HEARD, 7u32) };
        for v in [&lobby, &room] {
            draw_live(v, Some(&entry(5, 0, "a", "kept")), || say(v, "a", "kept"));
            draw_live(v, Some(&entry(6, EPHEMERAL, "a", "own")), || {
                say(v, "a", "own")
            });
        }
        let row = |v: &RotulusView, space, id| output(v).borrow().lines.row(space, id).unwrap();
        let takes = |targets| Limits {
            targets,
            max_keys: None,
            max_per_user: None,
        };
        let (lines, rooms, both) = (
            TARGETS_LINES,
            TARGETS_EPHEMERAL,
            TARGETS_LINES | TARGETS_EPHEMERAL,
        );
        for (case, v, space, id, targets, want) in [
            (
                "a lobby line",
                &lobby,
                Space::History,
                5,
                lines,
                Some(Target::Line { channel: 0, id: 5 }),
            ),
            (
                "a lobby line, kind 4 alone",
                &lobby,
                Space::History,
                5,
                rooms,
                None,
            ),
            (
                "a room's line",
                &room,
                Space::History,
                5,
                rooms,
                Some(Target::Ephemeral { chat: 7, id: 5 }),
            ),
            (
                "a room's line, kind 1 alone",
                &room,
                Space::History,
                5,
                lines,
                None,
            ),
            ("a room's own count", &room, Space::Ephemeral, 6, both, None),
            (
                "the lobby's own count",
                &lobby,
                Space::Ephemeral,
                6,
                both,
                None,
            ),
        ] {
            let got = line_on(v, row(v, space, id), &takes(targets)).map(|id| target(v, id));
            assert_eq!(got, want, "{case}");
        }
    }

    #[test]
    fn a_target_names_the_chat_its_line_is_shown_in() {
        let im = Target::Im {
            guid: [1; 16],
            sender: b"bob".to_vec(),
        };
        for (target, want) in [
            (Target::Line { channel: 0, id: 5 }, Some((0, 5))),
            (Target::Ephemeral { chat: 7, id: 5 }, Some((7, 5))),
            (Target::Line { channel: 7, id: 5 }, Some((7, 5))),
            (Target::Ephemeral { chat: 0, id: 5 }, None),
            (im, None),
        ] {
            assert_eq!(line_of(&target), want, "{target:?}");
        }
    }

    #[test]
    fn a_summary_draws_a_chip_a_key_in_its_order_ours_marked() {
        let (thumbs, heart) = ("\u{1F44D}", "\u{2764}\u{FE0F}");
        let summary = Summary {
            entries: vec![
                (Key::Emoji(thumbs.into()), 3),
                (Key::Server(17), 1),
                (Key::Server(18), 4),
                (Key::Emoji(heart.into()), 2),
            ],
        };
        let chip = |key: &str, text: Option<&str>, count, mine, label: Option<&str>| Reaction {
            key: key.into(),
            text: text.map(Into::into),
            count,
            mine,
            label: label.map(Into::into),
        };
        let placeholder = Some("\u{25C7}");
        let mut shown = LineReactions::default();
        shown.replace(summary, &Mine(vec![3, 1]));
        assert_eq!(
            chips(&shown.shown()),
            [
                chip(thumbs, None, 3, false, None),
                chip(
                    "server-emoji:17",
                    placeholder,
                    1,
                    true,
                    Some("server emoji 17")
                ),
                chip(
                    "server-emoji:18",
                    placeholder,
                    4,
                    false,
                    Some("server emoji 18")
                ),
                chip(heart, None, 2, true, None),
            ]
        );
    }

    #[test]
    fn a_line_is_stale_once_told_a_while_ago_and_past_the_newest_told() {
        let now = Instant::now();
        let old = now - FRESH - Duration::from_secs(1);
        let fresh = now - FRESH + Duration::from_secs(1);
        for (case, newer, told, want) in [
            ("old, past the newest", TOLD_LINES, old, true),
            ("told a moment ago", TOLD_LINES, fresh, false),
            ("among the newest", TOLD_LINES - 1, old, false),
        ] {
            let mut lines = LineRegistry::default();
            lines.history(10, MessageId(10));
            // Live lines count as history ones do; older ones and another
            // space's do not.
            for id in 11..11 + newer as u64 {
                match id % 2 {
                    0 => drop(lines.history(id, MessageId(id))),
                    _ => lines.live((Space::History, id), MessageId(id)),
                }
            }
            lines.history(9, MessageId(9));
            lines.live((Space::Ephemeral, 1000), MessageId(1000));
            // Newer lines trimmed from a short scrollback still count.
            lines.drew(newer as u64, 1);
            lines.lines.get_mut(&(Space::History, 10)).unwrap().told = told;
            assert_eq!(lines.stale(10, now), want, "{case}");
        }
        assert!(!LineRegistry::default().stale(10, now), "a line not drawn");
    }

    /// What the server says of a line makes it fresh again, and so does
    /// asking after it.
    pub(crate) fn check_a_line_told_of_is_fresh_again() {
        let view = view();
        let page: Vec<HistoryEntry> = (1..=TOLD_LINES as u64 + 1)
            .map(|id| entry(id, 0, "a", ""))
            .collect();
        draw_history(&view, &page, false);
        let now = Instant::now();
        let line = (Space::History, 1);
        output(&view)
            .borrow_mut()
            .lines
            .lines
            .get_mut(&line)
            .unwrap()
            .told = now - FRESH * 2;
        assert!(output(&view).borrow().lines.stale(1, now));
        reacted(&view, 1, Some(&thumbs(1)), false);
        assert!(!output(&view).borrow().lines.stale(1, now));

        let later = now + FRESH * 2;
        assert!(due(&view, 1, later), "stale again");
        assert!(!due(&view, 1, later), "asked already");
    }

    /// Who reacted lists a line's emoji alone: a server emoji's
    /// placeholder can't be asked about.
    pub(crate) fn check_who_reacted_lists_emoji_alone() {
        let view = view();
        let summary = Summary {
            entries: vec![(Key::Server(3), 1), (Key::Emoji("\u{1F44D}".into()), 2)],
        };
        let seeded = HistoryEntry {
            reactions: Some((summary, Mine::default())),
            ..entry(4, 0, "a", "four")
        };
        draw_history(&view, &[seeded], false);
        assert_eq!(emoji_on(&view, 4), ["\u{1F44D}"]);
        assert_eq!(emoji_on(&view, 5), [""; 0], "a line not drawn");
    }

    /// A line showing a chip, a server emoji's placeholder alone among
    /// them, is offered to clear to an account that may disconnect users
    /// and to no other, and once cleared has nothing more to clear.
    pub(crate) fn check_a_moderator_is_offered_to_clear_a_line_with_chips() {
        use gtkhx_core::conn::{hx_conn_free, hx_conn_new, hx_conn_set_access};
        let view = view();
        let held = |id, key| HistoryEntry {
            reactions: Some((
                Summary {
                    entries: vec![(key, 1)],
                },
                Mine::default(),
            )),
            ..entry(id, 0, "a", "")
        };
        let page = [
            held(4, Key::Server(3)),
            held(5, Key::Emoji("\u{1F44D}".into())),
            entry(6, 0, "a", ""),
        ];
        draw_history(&view, &page, false);
        // Bit 22, and bit 9 alone.
        let conn = |access: [u8; 8]| {
            let c = hx_conn_new();
            // SAFETY: a live connection.
            unsafe { hx_conn_set_access(c, access.as_ptr()) };
            c
        };
        let (moderator, reader) = (
            conn([0, 0, 0x02, 0, 0, 0, 0, 0]),
            conn([0, 0x40, 0, 0, 0, 0, 0, 0]),
        );
        // SAFETY: both connections are live until freed below.
        let clears = |c: *mut gtkhx_core::conn::HtlcConn, id| unsafe {
            crate::reactions::clears(c.cast(), &view, id)
        };
        for (case, c, id, want) in [
            ("a placeholder alone", moderator, 4, true),
            ("an emoji", moderator, 5, true),
            ("no chips", moderator, 6, false),
            ("a line not drawn", moderator, 7, false),
            ("not a moderator", reader, 5, false),
        ] {
            assert_eq!(clears(c, id), want, "{case}");
        }
        reacted(&view, 5, None, false);
        assert!(!clears(moderator, 5), "cleared");
        // SAFETY: neither is used again.
        for c in [moderator, reader] {
            unsafe { hx_conn_free(c) };
        }
    }

    #[test]
    fn a_line_drawn_again_live_keeps_its_first_row() {
        let mut lines = LineRegistry::default();
        lines.history(5, MessageId(1));
        for id in [5, 10, 11, 12] {
            lines.drew(1, 3);
            lines.live((Space::History, id), MessageId(id));
        }
        assert_eq!(lines.row(Space::History, 5), Some(MessageId(1)));
    }

    /// What a screen reader is read of `view`, a row a line.
    fn spoken(view: &RotulusView) -> String {
        use gtk::ffi::{
            gtk_accessible_text_get_type, GtkAccessibleText, GtkAccessibleTextInterface,
        };
        // SAFETY: the view implements GtkAccessibleText, whose interface
        // struct this reads the view's own function from.
        unsafe {
            let obj = raw(view) as *mut glib::gobject_ffi::GTypeInstance;
            let iface = glib::gobject_ffi::g_type_interface_peek(
                (*obj).g_class as glib::ffi::gpointer,
                gtk_accessible_text_get_type(),
            ) as *const GtkAccessibleTextInterface;
            let bytes =
                ((*iface).get_contents.unwrap())(obj as *mut GtkAccessibleText, 0, u32::MAX);
            let bytes: glib::Bytes = glib::translate::from_glib_full(bytes);
            String::from_utf8_lossy(&bytes).into_owned()
        }
    }

    /// The headers a screen reader is read of `view`, in order.
    fn headers(view: &RotulusView) -> Vec<String> {
        (spoken(view).lines())
            .filter_map(|l| l.find("replying to").map(|i| l[i..].to_owned()))
            .collect()
    }

    fn reply(id: u64, flags: u16, text: &str, to: Target) -> HistoryEntry {
        HistoryEntry {
            reply_to: Some(to),
            ..entry(id, flags, "bob", text)
        }
    }

    fn kept(id: u64) -> Target {
        Target::Line { channel: 0, id }
    }

    const EARLIER: &str = "replying to a message: earlier message";

    /// A reply to a line the view drew quotes it, one line, as a reader
    /// hears it; to a line it did not, it is an earlier message, a kept
    /// line asked for by id and a line nobody keeps not; and a reply to a
    /// line elsewhere has no header. A line drawn later, live or in an
    /// older page, fills the headers that wait for it.
    pub(crate) fn check_a_reply_is_headed_with_what_it_answers() {
        let view = view();
        let notice = |id| Target::Ephemeral { chat: 0, id };
        draw_history(
            &view,
            &[
                entry(4, 0, "alice", "four\nlines"),
                reply(5, 0, "yes", kept(4)),
                reply(6, 0, "and that", kept(2)),
                reply(7, 0, "hello", notice(19)),
                reply(8, 0, "there", Target::Line { channel: 3, id: 1 }),
                entry(9, SERVER_MSG, "", "carol joined"),
            ],
            true,
        );
        assert_eq!(
            headers(&view),
            ["replying to alice: four lines", EARLIER, EARLIER]
        );
        assert_eq!(due_quotes(&view), Some(vec![2]));

        draw_live(
            &view,
            Some(&entry(19, EPHEMERAL, "carol", "nineteen")),
            || say(&view, "carol", "nineteen"),
        );
        draw_live(&view, Some(&reply(10, 0, "ok", kept(9))), || {
            say(&view, "bob", "ok")
        });
        loading_older(&view, &mut output(&view).borrow_mut());
        draw_history(&view, &[entry(2, ACTION, "dave", "waves")], false);
        assert_eq!(
            headers(&view),
            [
                "replying to alice: four lines",
                "replying to dave: waves",
                "replying to carol: nineteen",
                "replying to server: carol joined",
            ]
        );
    }

    /// What the server says of a line asked for by id heads the replies
    /// that wait for it: its words, that it was taken away, or that it is
    /// still an earlier message where it was left out; and a quote held is
    /// not asked for again.
    pub(crate) fn check_a_quote_asked_for_heads_its_replies() {
        let view = view();
        draw_history(
            &view,
            &[
                reply(10, 0, "a", kept(1)),
                reply(11, 0, "b", kept(2)),
                reply(12, 0, "c", kept(3)),
            ],
            false,
        );
        draw_live(&view, Some(&reply(13, 0, "d", kept(1))), || {
            say(&view, "bob", "d")
        });
        assert_eq!(headers(&view), [EARLIER; 4]);
        assert_eq!(due_quotes(&view), Some(vec![1, 2, 3]));
        let now = output(&view).borrow_mut().quotes.answered(
            vec![
                (1, Quoted::Line(Quote::new("erin", "one"))),
                (2, Quoted::Removed),
            ],
            false,
        );
        quoted(&view, now);
        let one = "replying to erin: one";
        let removed = "replying to a message: message removed";
        assert_eq!(headers(&view), [one, removed, EARLIER, one]);
        draw_live(&view, Some(&reply(14, 0, "e", kept(1))), || {
            say(&view, "bob", "e")
        });
        assert_eq!(headers(&view), [one, removed, EARLIER, one, one]);
        assert_eq!(due_quotes(&view), None);
    }

    /// A line drawn live and then shown taken away by a page is quoted as
    /// removed, not as what it said.
    pub(crate) fn check_a_line_taken_away_is_quoted_as_removed() {
        let view = view();
        draw_live(&view, Some(&entry(3, 0, "alice", "oops")), || {
            say(&view, "alice", "oops")
        });
        draw_history(&view, &[entry(3, DELETED, "", "")], false);
        draw_live(&view, Some(&reply(4, 0, "what", kept(3))), || {
            say(&view, "bob", "what")
        });
        assert_eq!(headers(&view), ["replying to a message: message removed"]);
    }

    /// A reply the scrollback let go of before its quote was asked for
    /// asks for it no more.
    pub(crate) fn check_a_trimmed_reply_asks_for_no_quote() {
        let view = view();
        view.set_max_rows(3);
        draw_live(&view, Some(&reply(10, 0, "a", kept(1))), || {
            say(&view, "bob", "a")
        });
        draw_live(&view, Some(&reply(11, 0, "b", kept(2))), || {
            say(&view, "bob", "b")
        });
        draw_live(&view, None, || draw_prose(&view, b"two\nrows"));
        assert_eq!(due_quotes(&view), Some(vec![2]));
    }

    #[test]
    fn a_log_line_is_a_row_a_line() {
        for (text, want) in [
            (c"one", &[&b"one"[..]][..]),
            (c"one\ntwo\n", &[b"one", b"two"]),
            (c"one\n\ntwo", &[b"one", b"\n", b"two"]),
            (c"", &[b"\0"]),
        ] {
            assert_eq!(log_lines(text), want, "{text:?}");
        }
    }

    #[test]
    fn a_multi_line_body_is_one_without_its_repeated_sender() {
        for (body, want) in [
            ("one", "one"),
            ("one\nbob:  two\nbob: three", "one\ntwo\nthree"),
            ("one\ncarol:  two", "one\ncarol:  two"),
            ("one\n\nbob:  two\n", "one\n\ntwo"),
            ("\nbob:  two", "\ntwo"),
            ("", ""),
        ] {
            assert_eq!(
                join_body(body.as_bytes(), b"bob"),
                want.as_bytes(),
                "{body:?}"
            );
        }
    }
}
