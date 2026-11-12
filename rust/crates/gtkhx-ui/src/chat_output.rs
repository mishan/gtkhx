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
//! 19), a lobby line's chips come from its history entry and then from
//! what the `reactions` signal says of it, with what the user has asked
//! for and not had answered drawn over them (`hxmodel::reactions`). A
//! click on a chip, or its row's menu (`reactions`), asks for one.

use std::cell::RefCell;
use std::collections::{BTreeSet, HashMap, VecDeque};
use std::ffi::{c_char, c_void, CStr, CString};
use std::time::{Duration, Instant};

use glib::translate::{from_glib_none, ToGlibPtr};
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use hxmodel::reactions::{LineReactions, Next, Shown};
use hxproto::reactions::{Key, Mine, Summary, INVALID_KEY};
use hxproto::target::Target;
use hxsession::HistoryEntry;
use rotulus::ffi::{RotulusRun, RotulusSpeaker};
use rotulus::{LoadDirection, RotulusView};
use rotulus_layout::{MessageId, Reaction};

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
}

/// A line's row, and the reactions shown under it.
struct Line {
    row: MessageId,
    reactions: LineReactions,
    /// When the server last said what the line holds, or was asked.
    told: Instant,
}

impl Line {
    fn new(row: MessageId) -> Line {
        let (reactions, told) = (LineReactions::default(), Instant::now());
        Line {
            row,
            reactions,
            told,
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

const HEARD: &str = "gtkhx-chat-output-heard";

/// Hear the clicks on "Load older" in `view`, chat `cid`'s. Once.
fn hear(view: &RotulusView, cid: u32) {
    // SAFETY: the key is only ever set to ().
    if unsafe { view.data::<()>(HEARD) }.is_some() {
        return;
    }
    unsafe { view.set_data(HEARD, ()) };
    if cid == 0 {
        crate::reactions::hear(view);
    }
    view.connect_local("load-more", false, move |args| {
        let view = args[0].get::<RotulusView>().ok()?;
        if args[1].get::<LoadDirection>().ok()? == LoadDirection::Older {
            load_older(&view, cid);
        }
        None
    });
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
fn chips(shown: &[Shown]) -> Vec<Reaction> {
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
        hxhandlers::send::reactions::refresh(htlc, &[Target::Line { channel: 0, id }]);
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

/// What [`want`] and [`answered`] leave to do.
#[derive(Debug, PartialEq)]
pub(crate) enum Then {
    /// Ask for `key` on the line, an add or not.
    Send(u64, Key, bool),
    /// Take back the request for `key` on the line, if it is still to go.
    Withdraw(u64, Key),
    /// Tell the user why not.
    Say(String),
}

/// The lobby line drawn on `row`, where `limits` take reactions to it.
pub(crate) fn line_on(
    view: &RotulusView,
    row: MessageId,
    limits: &hxproto::reactions::Limits,
) -> Option<u64> {
    let id = output(view).borrow().lines.history_on(row)?;
    limits.takes(&Target::Line { channel: 0, id }).then_some(id)
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
    if key.starts_with(SERVER_EMOJI) {
        return Some(Then::Say(crate::reactions::server_emoji()));
    }
    let key = Key::Emoji(key.to_owned());
    let next = redraw(view, id, |r| {
        let on = on.unwrap_or_else(|| !r.mine(&key));
        let refusal = on && !r.mine(&key);
        match refusal.then(|| r.refusal(&key, limits.max_keys, limits.max_per_user)) {
            Some(Some(code)) => Err(code),
            _ => Ok(r.want(key.clone(), on)),
        }
    })?;
    match next {
        Err(code) => Some(Then::Say(crate::reactions::refusal(Some(code), None))),
        Ok(Next::Send(add)) => Some(Then::Send(id, key, add)),
        Ok(Next::Withdraw) => Some(Then::Withdraw(id, key)),
        Ok(Next::Nothing) => None,
    }
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
            if !send::react(htlc, Target::Line { channel: 0, id }, key.clone(), add) {
                refused(view, id, &key);
                crate::reactions::toast(&crate::reactions::refusal(Some(INVALID_KEY), None));
            }
        }
        Then::Withdraw(id, key) => {
            if send::withdraw(htlc, &Target::Line { channel: 0, id }, &key) {
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
    for e in entries {
        // A line taken away since it was drawn still gets its tombstone.
        if e.message_id != 0
            && e.flags & DELETED == 0
            && out.lines.row(Space::History, e.message_id).is_some()
        {
            continue;
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
        }
    }
    if !older {
        let text = format!("─── {} ───", tr("live messages"));
        Row::of(rotulus::ffi::ROW_DIVIDER, &[muted(&text)]).put(view, None);
    }
    let drawn = |id: &u64| out.lines.row(Space::History, *id).is_some();
    missed.into_iter().filter(drawn).collect()
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
                let lines: Vec<Target> = missed
                    .into_iter()
                    .map(|id| Target::Line { channel: 0, id })
                    .collect();
                hxhandlers::send::reactions::refresh(htlc, &lines);
            }
        });
        gtkhx_core::session::connect_reactions(|htlc, e| unsafe {
            use gtkhx_core::reactions::Event;
            let Some(view) = view_of(htlc, 0) else {
                return;
            };
            if let Event::Who { ask, nicks, count } = e {
                // Nicks are what each reacted under: whoever online goes
                // by one now is the best guess at whose icon it is.
                let who: Vec<_> = (nicks.iter())
                    .map(|n| {
                        (
                            n.clone(),
                            crate::chat_view::avatar(&view, uid_of(htlc, 0, n.as_bytes())),
                        )
                    })
                    .collect();
                return crate::reactions::heard(*ask, &who, *count);
            }
            match e {
                Event::Now {
                    target: Target::Line { channel: 0, id },
                    reactions,
                } => {
                    let page_due = hxhandlers::recv::chat::first_page_pending(htlc, 0);
                    reacted(&view, *id, reactions.as_ref(), page_due);
                }
                Event::Acted {
                    target: Target::Line { channel: 0, id },
                    key,
                    add,
                    count,
                } => {
                    if let Some(t) = answered(&view, *id, key, *add, *count) {
                        then(htlc, &view, t);
                    }
                }
                Event::PutOff {
                    target: Target::Line { channel: 0, id },
                    key,
                    add,
                } => {
                    if redraw(&view, *id, |r| r.put_off(key)) == Some(true) {
                        let line = Target::Line {
                            channel: 0,
                            id: *id,
                        };
                        hxhandlers::send::reactions::retry(htlc, line, key.clone(), *add);
                    }
                }
                Event::Refused {
                    target: Target::Line { channel: 0, id },
                    key,
                    reason,
                    error,
                } => {
                    refused(&view, *id, key);
                    crate::reactions::toast(&crate::reactions::refusal(*reason, error.as_deref()));
                }
                _ => {}
            }
        });
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
