# New-user experience — scoping

Status: design draft. Nothing implemented yet.

## Who is the new user?

GtkHx is a Hotline client. In 2026, "Hotline" is a protocol almost
nobody under 35 has heard of. The new-user audience we need to design
for splits roughly into:

1. **Returning Hotliner.** Already knows what Hotline is, used HX or
   Hotline Client back in the day, found GtkHx via a forum link or
   bookmark search. Just wants to connect and chat. Probably already
   knows at least one server address.
2. **Nostalgic explorer.** Encountered "Hotline" in a retro-tech
   article or YouTube video, came looking for a way to try it. Has
   zero idea where to point the client. May not even know a server
   has to be online for them to do anything.
3. **Friend-of-a-Hotliner.** A returning Hotliner shared a server
   URL with them. Has one address to type in, no other context.

The current empty-state for all three audiences is the same: launch
GtkHx → see an empty toolbar window → puzzlement. The Connect dialog
exists but isn't obvious; bookmarks are empty; the Tracker tab gives
you a list of servers but only if you know to look there *and* if a
tracker URL is configured.

Audience #1 doesn't need much help — they'll find Connect. Audience
#2 and #3 are the ones we're designing the welcome flow for.

## First-run detection

The natural sentinel for "first run" is the absence of `gtkhxrc` in
`gtkhx_config_dir()` (`$XDG_CONFIG_HOME/gtkhx/gtkhxrc`). It's written
on app exit by `prefs_write()`, so any prior run has produced it.
Other candidates considered and rejected:

- Bookmarks dir empty — false negative when the user explicitly
  deleted bookmarks but is otherwise a regular user.
- A dedicated "welcome shown" key inside `gtkhxrc` — chicken-and-egg
  on a fresh install, and forces us to design what happens between
  prefs_read (which would set the key) and the welcome dialog
  (which wants to see the original value). Cleaner to key off
  prefs-file existence directly.
- A `.first-run-done` empty file under `$CONFIG/gtkhx/` — adds a
  second moving piece. The gtkhxrc presence is already exactly the
  signal we want.

Detection happens in `gtkhx_activate` *before* `prefs_read()` runs,
so we can capture the original state and stash it on a static flag.
The welcome dialog fires after the toolbar window has been
constructed and presented (so it has a parent).

## UX shapes we're choosing between

### Option A — single Welcome dialog

A one-screen `AdwDialog` over the toolbar:

```
┌─────────────────────────────────────────────┐
│   [logo]                                    │
│                                             │
│   Welcome to GtkHx                          │
│   A modern GTK client for Hotline.          │
│                                             │
│   ┌──────────────────────────────────────┐  │
│   │ Hotline is a chat + file-sharing     │  │
│   │ protocol from the late 1990s. Pick   │  │
│   │ a server below to start exploring,   │  │
│   │ or open the tracker to browse the    │  │
│   │ live server directory.               │  │
│   └──────────────────────────────────────┘  │
│                                             │
│   [ Connect to mhxd test server ]           │
│   [ Browse public trackers      ]           │
│   [ Pick a nickname & icon      ]           │
│                                             │
│   [ ] Don't show this again                 │
│   [ Maybe later ]   [ Get started ]         │
└─────────────────────────────────────────────┘
```

Pros: lowest implementation cost, easy to dismiss, no commitment
from the user. Pros: discoverability is upper-bounded by what we can
fit in one screen.

Cons: doesn't actually set anything up — the user still has to
configure nickname / pick a server manually after dismissing.

### Option B — multi-step wizard

`AdwDialog` with an `AdwNavigationView` (or just a manual page
swap), walking through:

1. **Welcome.** Logo, one-line explanation of Hotline.
2. **Your identity.** AdwEntryRow for nickname (prefilled with
   `$USER`), 56 px icon grid for picking a user icon (same picker
   we use in Settings). Bound straight to the same prefs keys.
3. **Pick a server.** Curated list of 3–6 known-live public
   servers + an "I have an address" custom-entry row. Selecting
   one creates a bookmark + offers to connect.
4. **All set.** Brief summary of where the main features live
   (Tracker, Users, Files, Chat). Closes the wizard.

Pros: actually walks the user through configuration. Sets nickname,
icon, and at least one bookmark on first run. Discoverable.

Cons: more code. Curated server list ages — we need a sane policy
for who's on it and how it gets refreshed (probably a hardcoded
constant, refreshed on each release; not pulled at runtime to avoid
phoning home).

### Option C — overlay tour

After dismissing a one-screen welcome, highlight key toolbar
buttons one at a time with a small popover ("This opens the
Connect dialog", "This is the tracker — public-server directory",
etc.). Lightweight, doesn't change any state.

Pros: discoverability without commitment.

Cons: tour overlays are well-known to be the *least*-engaged-with
form of onboarding; users dismiss them as fast as they appear.

### Recommendation: hybrid A + B

Single dialog that starts as a one-screen welcome (Option A
content), with one of the three CTAs being **Set up GtkHx** that
swaps the dialog content to the wizard (Option B pages 2–4). Users
who just want to dismiss get out fast; users who want help get
walked through.

The wizard pages use the existing widgets we've already built —
the icon picker in Settings, the Connect dialog's server entry
shape — so per-page implementation is light.

## Implementation sketch

### Step 1 — first-run detection

Add `gtkhx_is_first_run()` returning the value captured before
`prefs_read()` runs:

```c
/* gtkhx.c — fired from gtkhx_activate before prefs_read */
static gboolean first_run_state;
static gboolean first_run_state_known;

void
gtkhx_first_run_probe (void)
{
    char *path;
    if (first_run_state_known) {
        return;
    }
    path = g_build_filename (gtkhx_config_dir (), "gtkhxrc", NULL);
    first_run_state = !g_file_test (path, G_FILE_TEST_EXISTS);
    first_run_state_known = TRUE;
    g_free (path);
}

gboolean
gtkhx_is_first_run (void)
{
    return first_run_state_known && first_run_state;
}
```

Call `gtkhx_first_run_probe()` first thing in `gtkhx_activate`,
before any other config-touching code.

### Step 2 — welcome module

New file `src/welcome.{c,h}` for the dialog. Public API:

```c
extern void welcome_show_if_first_run (GtkWindow *parent);
```

Internal layout: `AdwDialog` with content built dynamically. The
dialog presents over the toolbar after the toolbar has been mapped
(use `g_idle_add` if needed to land it on the next main-loop
iteration so the toolbar paints first).

Pages laid out as a stack of `AdwBin` containers swapped by a
`GtkStack`; the "back" / "next" / "skip" buttons live on the
AdwHeaderBar and update which page is visible.

### Step 3 — content

Page 1 — welcome (always shown first):

- Logo (`gtkhx.png` from gresource)
- Title, one-sentence subtitle, two-paragraph body
- Three action buttons:
  - **Set up GtkHx** — switches to page 2 (identity)
  - **Browse servers** — closes dialog, opens Tracker window
  - **Skip for now** — closes dialog

Page 2 — identity:

- AdwEntryRow "Nickname", prefilled from `$USER`
- AdwEntryRow "Color" (combo: red/blue/green/etc.) — same options
  Settings exposes
- Icon picker grid (reuse `options.c::list_icons` infrastructure)
- **Back** / **Next** / **Skip rest**

Page 3 — server:

- A small static GListModel-backed list of curated servers
  (name, address, one-line description). Hardcoded `static const
  struct { ... } welcome_servers[]`.
- AdwEntryRow "Or enter a server address"
- **Back** / **Connect** — picking a row OR filling the entry
  enables Connect; clicking it writes a bookmark + closes dialog +
  fires the Connect flow.

Page 4 — done (optional):

- "You're all set." Brief paragraph pointing at Tracker / Users /
  Files / Chat buttons in the toolbar. Single **Close** button.

Could skip page 4 entirely — once a server is connected, the user
sees the real UI and figures it out.

### Step 4 — curated server list

Hardcoded constant for now:

```c
static const struct {
    const char *name;
    const char *address;
    const char *port;        /* "" = 5500 */
    const char *description;
} welcome_servers[] = {
    { "VesperNet",     "hotline.vespernet.net", "",     "Active community chat server" },
    { "Hotline Bar",   "hl.hlbar.com",          "",     "Public hangout — file sharing + chat" },
    /* … 2–3 more, picked by Misha … */
    { NULL, NULL, NULL, NULL }
};
```

Decision needed: which servers go on this list? Picking servers
that may shut down 6 months later is the worst UX. Conservative
pick: only servers run by people we know (e.g. someone on the
hxd-server mailing list or a known long-term operator), with a
fallback to the tracker if none of the listed ones answers.

Open question: should the welcome dialog ping each server in the
background to flag dead ones with a 🔴 indicator? Adds complexity;
probably worth it. The tracker code already has the connection
infrastructure (`tracker.c::hx_tracker_list`).

### Step 5 — "I already have prefs" path

If `gtkhx_is_first_run()` returns FALSE we never construct the
dialog. There's also a hamburger-menu entry **Help → Welcome…** so
returning users can re-open it on demand (also useful for
screenshotting + iteration).

## Don't-show-again behaviour

First-run welcome dismisses without writing a "do not show again"
flag. The signal that suppresses next time is just: `gtkhxrc` now
exists because `prefs_write()` will run on app exit (or earlier
when the user edits anything). Even **Skip for now** is a one-shot
— the next launch with a populated gtkhxrc won't fire it.

If we ever want a user to re-trigger the wizard, they delete
gtkhxrc OR use the menu entry described above.

## Open questions

1. **Tone.** GtkHx's other text has a fairly dry / technical tone.
   Should the welcome adopt the same voice ("Connects to Hotline
   servers — chat, file transfer, threaded news. Pick a server
   below.") or warm up a little ("Welcome! Hotline is a friendly
   way to chat and share files…")? Lean dry.
2. **Should we ship icon-picker default?** If the user skips the
   identity page, they get whichever icon `gtkhxrc`'s schema
   defaults to. Worth picking a deliberate default that doesn't
   look "I forgot to set this".
3. **Mac-classic context.** Some users will want a one-sentence
   "Hotline was originally a Mac OS 8/9 app from 1997; this is a
   GTK port of an open-source client" framing. Other users will
   roll their eyes at that. Put it behind a small "About Hotline"
   expander on page 1?
4. **Tracker browsing as an entry point.** Page-1 button **Browse
   servers** opens the Tracker — should it pre-fill a tracker
   address if `tracker.geo.list[0].address` isn't already set? Or
   trust the default schema?
5. **Translatable strings.** Welcome copy is heavy in text. Should
   land in `po/`. French translation can defer until the rest of
   the dialog is settled.

## Out of scope (deliberately)

- Theming / light-vs-dark welcome look. Use Adwaita default.
- Animations / transitions between pages. Adwaita's NavigationView
  fades are fine.
- Tour-style overlay highlighting the toolbar buttons (Option C
  above) — explicitly rejected for now.
- Auto-connect to a default server. Too presumptuous; the user
  should always pick.
- Telemetry / "tell us you launched it" phoning home. Never.

## Estimated effort

- Step 1 (first-run probe): 30 minutes.
- Step 2 + 3 (welcome.c module with all four pages): one good
  afternoon. The page content is the bulk; the dialog scaffolding
  is shared.
- Step 4 (curated server list + dead-server pinging): half a day
  if we ship dead-server detection. Couple hours without.
- Step 5 (menu entry to re-open + first-run gate): half an hour.

Rough total: a single focused day for the core, plus another half
to polish copy / pick icons / curate the server list.
