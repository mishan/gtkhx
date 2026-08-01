# Preferences

Settings live in `rust/crates/hxconfig`: the schema, the TOML file, the write
path, the version check, and the one-shot migration from `gtkhxrc`. C reads a
copy through `src/prefs_mirror.c` and writes only through the by-name setters in
`src/options.c`. The steps still outstanding are P6 onwards in the
decomposition at the end of this document.

The short version of what this was for: move settings ownership to Rust,
replace `gtkhxrc` with TOML, and let the per-connection identity model that
multi-connection needs fall out of that rather than be bolted onto the previous
design. See [multi-connection.md](multi-connection.md) for what depends on it.

---

## How preferences used to work

One global C struct, one static table of pointers into it, and a GKeyFile on
disk. It is described here because the defects below are what the current
design is shaped around, and because a `gtkhxrc` in the wild is still read
once, by the migration.

```c
struct cfgvar {
    const char *name;               /* the on-disk key, e.g. "FONT" */
    union { void *var; char **str; char *str32; unsigned char *uchar;
            int *integer; guint16 *uint16; time_t *timet; } variable;
    const unsigned int type : 7;    /* INT BOOLEAN STRING STRING32 UINT16 TIME_T */
    unsigned int allocated : 1;     /* STRING only: is *str heap-owned? */
    void (*changefunc) (session *); /* apply hook, or NULL */
    GtkWidget *widget;              /* the live Settings row, or NULL */
} cfgvars[] = { … };
```

Around seventy entries, sorted by key string because lookup is `bsearch`. The
sort is load-bearing enough to be enforced twice: fatally at runtime, and by a
unit test that scans the source text (`options.c` is too GTK-heavy to link into a
test binary).

Each entry's `variable` is **the address of a field** in one of three globals —
`gtkhx_prefs` for most, `hxsnd` for the sound toggles, `total_time` for the
cumulative-uptime counter — plus two entries whose storage is on the
heap-allocated connection and so cannot be filled in statically.

The file is `$CONFIG/gtkhxrc`: GKeyFile with a single `[gtkhx]` group, keys in
SHOUTING_CASE. The reader also accepts the pre-GKeyFile `KEY=VALUE` line format,
and falls back to `~/.gtkhxrc` once.

Reading is instant-apply: every Settings row change writes the field, fires the
change hook, and rewrites the whole file. Entry rows debounce by 750 ms — because
the nickname's hook puts a `USER_CHANGE` on the wire, and one packet per
keystroke was broadcasting partial nicknames.

The Settings dialog is a sidebar plus a stack, one `AdwPreferencesPage` per
entry, each built by a `draw` function pointer. Most pages are Rust builders
called through that pointer; Identity and Voice are still C because they are
custom widgets (a Mac resource-fork icon picker, a GIF avatar chooser, a
key-capture dialog) rather than declarative row sequences. Rust pages reach
preferences through a typed by-name bridge — `gtkhx_prefs_get_bool("MARKDOWN")`
and friends — with the key strings hand-mirrored into a Rust `mod cfg`.

## What was wrong with it

Not "it's C". These are concrete defects, most of them found while scoping this
work. Each is fixed by the design below unless it says otherwise.

**The on-disk format loses data.** The writer builds a fresh `GKeyFile` from the
table rather than mutating the loaded one, so an unknown key is silently dropped
on the next save — as are user comments, despite the loader passing
`G_KEY_FILE_KEEP_COMMENTS`. Hand-editing the file is a trap.

**Backslashes grow on every save.** The writer uses `g_key_file_set_string`,
which escapes; the reader uses `g_key_file_get_value`, which does not unescape.
So a value containing a backslash gains an escape level per save/load cycle. The
realistic victim is the download path on Windows.

**Malformed values are silently zero.** `INT`, `UINT16` and `TIME_T` all go
through `atoi`/`atol`, so garbage becomes 0 with no diagnostic. Booleans are
better only by accident — a dedicated first-character parser exists solely
because an earlier version accepted `0`/`1` but the writer emitted
`true`/`false`, silently reverting every boolean to its default on every startup.
The code comment records that as a shipped regression; it is why one function has
a source file and a test binary to itself.

**The legacy reader drops a trailing line with no newline**, and truncates any
value containing `#` anywhere on the line.

**The address table is a hazard.** Every backing field has to be a stable,
addressable, non-bitfield lvalue — several booleans are `unsigned char` rather
than one-bit fields *only* so the table can point at them, and one geometry flag
that is a bitfield consequently needs a shadow field to be persistable at all.
The `allocated` ownership bit is manual and unchecked by the compiler: two change
hooks reassign a string field without touching it, and stay correct only because
every path that can reach them happens to have set the bit already — one of them
also drops the old heap pointer without freeing it. And the writer blindly
dereferences every entry, so if the identity binder has not run, saving is an
immediate null dereference — an ordering dependency the type system cannot
express.

**The Rust copy of the key names is unchecked.** The names live in a C header and
again in a Rust `mod cfg`; the value type tags are likewise mirrored by hand. One
live consequence: the sound page believed the voice chime toggles vanished from
the table in a no-voice build and gated its rows on that. They didn't — the keys
are registered unconditionally, deliberately, so a no-voice build doesn't discard
a user's saved toggles — so the chime rows rendered in a build with no voice.
That row now gates on the crate's `voice` feature. The names themselves are
still hand-mirrored; what guards them is a test that reads `cfgkeys.h` and fails
if any key it defines has no schema path.

**Some entries are not preferences.** The cumulative-uptime counter is
accumulated state that a save mutates as a side effect. Eight window-size keys —
width and height for each of the four panels that became dock panels — are
written every save and never read. Four more record whether a panel has ever been
opened, but nothing ever sets them back to zero and no Settings row exposes them,
so they are one-way latches that are 1 forever after first run. Two geometry
fields are never touched at all, as are four rate-limit fields that have no key.
One key is `#if 0`'d out.

**Derived state lives in the struct.** The tracker list is persisted as one
comma-separated string and a change hook rebuilds a `char **` array beside it,
whose element pointers are then handed across FFI.

**There is no round-trip test.** Nothing writes a `gtkhxrc` and reads it back.
That gap is exactly what let the escape asymmetry survive.

---

## The design

### One crate, `hxconfig`

Pure Rust, serde + toml, `std`-only — no glib, no gtk. The config directory is a
parameter, not a global, so it unit-tests headless against temp dirs. This is the
shape `hxbookmarks` and `hxtls-trust` already have, and it is why both are
testable without a display.

`hxconfig` owns the settings file. `hxbookmarks` keeps owning the connection
file. They share the crate's atomic-write helper, its versioning discipline, and
its error type, but not a file.

### Two files, not one

The Settings → Connections framing in
[multi-connection.md](multi-connection.md) makes connections *configuration*, so
merging the two files is the obvious move. It is still the wrong one, for four
reasons that the merged design cannot recover:

1. **The connection file holds plaintext passwords.** A single config file means
   "send me your config so I can reproduce this" and "keep my dotfiles in git"
   both leak credentials. Separate files let the credential-bearing one take
   tighter permissions — it doesn't have them today, it is written with the
   default umask — and move to a platform secret store later without dragging
   every preference with it.
2. **The two want opposite failure policies, and both are already correct.**
   A corrupt connection file *refuses to save* and surfaces the error, so a
   load-mutate-save cycle can't turn one typo into an empty server list — there
   is a test pinning exactly that. A corrupt settings file should fall back to
   defaults and let the user carry on, because settings are reconstructible and
   servers are not. One file forces one policy onto both.
3. **Blast radius.** Both are full-file rewrites. Merged, every settings toggle
   rewrites the server list, and a bad write loses both.
4. **They are separately exportable.** "Export my bookmarks in the legacy format"
   and "reset my settings without losing my servers" are both real operations,
   and both get awkward when one file holds everything.

None of this is visible to the user: Settings → Connections still presents them
together. Storage layout and UI layout are unrelated decisions.

### The schema

TOML, nested tables, `snake_case`. The nesting is not decoration — it turns two
comma-separated strings into arrays and lets a shadow field and four one-way
latches be deleted rather than carried forward.

```toml
version = 1

[identity]
nick = "Misha"
icon = 500
nick_color = "#c061cb"       # "" for none

[appearance]
color_scheme = "system"      # system | light | dark
theme = "default"            # names a file under themes/
tray = true

[chat]
font = "Monospace 10"
word_wrap = false
scrollback_lines = 500
timestamp = false
timestamp_format = "[%H:%M:%S] "
avatars = true                # the chat gutter; see [users] for the user list
markdown = true
show_joins = true
history_initial = 50
highlight_words = ["gtkhx", "hotline"]   # also drives notification mentions
legacy_nick_completion = false

[chat.autocopy]
text = true
timestamp = false
color = false

[chat.emoji]
shortcodes = true
typeahead = true

[users]
animate_avatars = true        # GIF avatars in the user list

[notify]
chat = false
chat_highlight = true
private_message = true
private_chat = true
private_chat_highlight = true
private_chat_invite = true
news = false
transfer = true
broadcast = true
omit_focused = true

[sound]
enabled = false
chat = true
error = true
transfer = true
invite = true
join = true
leave = true
login = true
private_message = true
news = true
voice_join = true
voice_leave = true

[transfers]
download_dir = "/home/misha/Downloads"
queue = true

[trackers]
addresses = ["hltracker.com", "tracker.preterhuman.net"]
case_sensitive = true

[voice]
input_device = ""
output_device = ""
ptt_enabled = false
ptt_key = ""

[window]
toolbar_width = 1100
toolbar_height = 700
```

- **The nickname colour is written `#rrggbb`.** It was a bare decimal —
  `12607947` — which tells a reader nothing and cannot be hand-edited with any
  confidence. It is an integer everywhere else, in memory and on the wire; only
  the file spells it in hex, and the empty string means no colour, matching
  the schema's other "empty means unset" values. Reading accepts `#rgb`, either
  form without the `#`, any case, and a bare integer, so a file written before
  this or by someone reaching for a decimal still loads and heals itself on the
  next save.

Four structural changes worth calling out:

- **`highlight_words` and `trackers.addresses` become arrays.** The tracker
  change deletes the comma-splitting change hook and the derived `char **` array
  that lives beside the string today.
- **The four "panel was opened" keys are dropped, not migrated.** They look like
  user intent and aren't: nothing ever clears them, no Settings row exposes them,
  and each is set to 1 the first time its panel is constructed — so after first
  run they are permanently 1 for everyone. Dropping them also removes the reason
  the geometry struct carries a shadow field beside its one-bit "is this panel
  open" flag, which stays runtime-only where it belongs. If "reopen the panels I
  had open" is wanted later, it is a new feature and belongs with the dock layout,
  which already tracks what is open.
- **There is no `[state]` table.** The draft schema had one, holding the
  cumulative-uptime counter — the one entry that was accumulated state a save
  mutated as a side effect rather than a preference. Its only consumer was
  `/stats`, and that command and the `TIME` key were both removed before this
  work started, so the table has nothing to hold and doesn't exist.
- **Dropped entirely**: the eight panel window-size keys (vestigial — written
  every save, never read), the two never-touched geometry fields, the four
  never-read rate-limit fields, and the `#if 0`'d logging key. Logging comes back
  with the feature, under `[logging]`.

One value moved while transcribing this. The toolbar's saved size read back
through a zero sentinel — the C default was `0`, and the window fell back to
1100×700 when it saw one. Here the default *is* 1100×700, so the sentinel goes
away and "no size saved yet" and "the size we'd use anyway" stop being different
states.

### Versioning that is actually checked

The bookmark store has a version field that nothing reads — no migration code, no
forward-compat check, and a file claiming version 99 loads as-is. Don't repeat
that. `hxconfig` checks it:

- Equal → load.
- **Lower** → run the migration chain, then save at the current version.
- **Higher** → load what parses, warn, and **do not save** until the user
  acknowledges. Silently rewriting a newer file at an older schema is how a user
  who tried a newer build loses settings on downgrade.

Unknown keys inside a known version are preserved on save, not dropped.

The shipped mechanism goes further than the two options originally weighed. The
crate holds the parsed `toml_edit` document and **edits it in place**, so a save
preserves unknown keys, unknown *tables*, key order, formatting, and — the part
neither `toml::Table` nor `#[serde(flatten)]` would have given — the user's
comments, including a trailing comment on a line whose value just changed. The
old loader passed `G_KEY_FILE_KEEP_COMMENTS` and then threw the comments away
anyway, because the writer built a fresh `GKeyFile` from the table rather than
mutating the loaded one. Editing in place is what actually makes hand-editing
safe, and it is why the crate's one dependency is `toml_edit` rather than `toml`.

A load-modify-save changes exactly the lines it had to, and a load-save with no
modification is byte-identical — including for a file the client never wrote.
That last part needs the unchanged-value check to compare *meaning* rather than
rendered text, because TOML has more ways to spell a value than the writer
emits: `'single quoted'`, `1_000`, `0x0066ccff`, an array across several lines
with a comment beside each element. A textual comparison would call every one of
those a change and normalise it on a save that never touched it. A line the user
*does* change does come back in the writer's spelling; that is the boundary.

Reading and writing both come out of a **single field table** — one list mapping
each dotted path to the struct field behind it, expanded by a macro into a
reader and a writer. Two hand-written parallel functions drift, and a drifted
pair is precisely the failure the old system had between its C key macros and
the Rust copy of them. Here, adding a field to one direction is not expressible.

Loading never fails. Every failure mode degrades to defaults and records a
diagnostic naming the path and what was wrong with it, so a malformed number is
*reported* rather than becoming a silent zero the way `atoi` made it. One bad
element of a list costs that element, not the list. A file that does not parse
at all falls back to defaults — settings are reconstructible — but the next save
moves the unreadable original aside to `gtkhx.toml.corrupt` first, because a
hand-edited file with one typo in it is still the user's work.

### Writes

Atomic, `O_EXCL` temp in the same directory, `fsync` before rename, `0600`.
That is `hxtls-trust`'s write path, which is the strongest of the three
precedents in the tree — the bookmark store uses a *fixed* temp filename, so two
concurrent writers race on it, and skips the `fsync`. `hxconfig` should ship the
hardened version and the bookmark store should adopt it.

Debounced, not per-change. Instant-apply stays as the UI behaviour, but the file
write coalesces on a short timer with a synchronous flush at exit, the way the
dock layout already does it. Today every toggle rewrites the file.

### The C mirror

`gtkhx_prefs` is read field-by-field from around a hundred sites across nineteen
C files, including model-side code. Converting all of them is the right end state
and the wrong first move — those files are being ported to Rust anyway, and doing
both at once means the preferences work lands inside every one of them.

So: **Rust owns the values and the file; C keeps a mirror struct it may read but
never write.**

This was drafted the other way round, with Rust owning the mirror's storage as a
`#[repr(C)]` struct and `_Static_assert`s pinning the layout on both sides — the
idiom the connection struct and the boxed signal payloads use. What shipped
keeps the storage in C (`src/prefs_mirror.c`) and repopulates it through by-name
getters after every change. That buys the same two properties — one write path,
and a hundred C read sites that keep compiling untouched — and couples the two
languages not at all: there is no shared layout, so there is nothing to pin and
nothing to get wrong when a field is added.

What that buys: the `cfgvars[]` address table, the `allocated` ownership bit, the
null-dereference-if-unbound hazard and the addressability constraint on every
field all disappear at once, and roughly a hundred C read sites keep compiling
untouched. Writes have exactly one path, through Rust.

The mirror is deleted when its last reader is ported. Each file that moves to
Rust drops its direct reads as it goes.

The mirror could not be read-only until six C write sites outside the settings
code were dealt with, and four of them landed on persisted state rather than
runtime scratch:

- The chat, tasks, users and news panels each set their "is this panel open"
  runtime flag *and* its persisted shadow field when the panel is constructed.
  The shadow field was the storage for the four latch keys dropped above, so
  once those keys went these four sites only touched the runtime flag, which
  never belonged in the preferences struct in the first place. It moved out with
  them: four sites resolved by a deletion.
- The toolbar's resize handler and the save-at-quit path both wrote the toolbar
  window's width and height. Those are genuinely persisted and genuinely written
  by C, and there were exactly two of them; both are `gtkhx_prefs_set_int` calls
  now.

A handful of unit tests still define their own `gtkhx_prefs` and write it
directly, which is fine — they link neither the mirror nor the crate behind it,
and are only asking `gtkhx_theme.c` to see a theme name.

### Change notification

A hook is one of three things, and the type says which. Every hook used to take
a `session *`: five walked that session's live views, thirteen ignored the
argument entirely, and two reached past it for whichever session happened to be
focused. A parameter that most callees ignore stops being read as information,
which is how the third group hid in the second.

| Flavour | Takes | For |
|---|---|---|
| `PREF_HOOK_VIEW` | the session | re-applying to every live view in it — font, word wrap, scrollback, timestamps, avatars |
| `PREF_HOOK_GLOBAL` | nothing | a process-wide side effect: the tray, the theme, the download directory, the voice devices |
| `PREF_HOOK_CONN` | the connection | pushing the value onto a connection, which means onto the wire — the nickname, the icon, the nickname colour |

**`PREF_HOOK_CONN` is the one that matters later.** Those hooks used to call
`hx_active_session()` from inside their bodies, eight times between them, which
is a routing bug the moment more than one connection exists — a preference
change would go to whichever window had focus. They now take the connection as
an argument, and exactly one function chooses it. At one connection the answer
is still "the focused one" and nothing behaves differently; the point is that
multi-connection has a single typed seam to change instead of a grep for
`hx_active_session`.

~~Load-time application is accidental~~ — **fixed in P3.** Hooks used to fire
only for keys whose file value *differed* from the compiled-in default, so a
separate hand-written function existed to re-apply the ones that were skipped.
Every hook now runs once after the load, which deleted both the bug and the
compensator.

---

## Identity, and how M1 falls out of this

The two identity preferences do not have backing fields in the preferences
struct. They bind directly to storage on the connection: the nickname
preference *is* the connection's 32-byte wire name buffer, and the icon
preference *is* its icon field. A binder patches those two table slots at
startup with raw interior pointers.

That is the mechanism behind the bug where `/nick` on any server rewrites your
stored global nickname — it writes the buffer that the preference is bound to, so
the next save persists it. It is also a stale-pointer hazard the moment
connections can be destroyed, a write hazard if a settings dialog stays open
across a connection switch, and simply undefined once there is more than one
connection.

The new model, decided in [multi-connection.md](multi-connection.md):

- `[identity]` in the settings file is the **global default**, an ordinary
  preference with ordinary storage.
- Each connection entry carries **optional overrides**; absent means inherit.
- Effective identity at connect is `override ?? global`, resolved once and
  **copied into** the connection. Nothing aliases anything.
- `/nick` and `/icon` change the live connection only and never persist.

The binder is deleted. That is the whole of M1's identity half, and it is a net
simplification at one connection — which is why it landed before any of the
multi-connection work.

**Where the resolution happens, and why there.** Inside the connect preamble in
`network.c`, beside where the login is already stamped. Eight things reach
`hx_connect` — the Connect dialog, a bookmark, a `hotline://` URL,
reconnect-last, a tracker double-click, `/server`, the `--server` CLI bootstrap
— and only that preamble is on all eight paths. Resolving in the Rust connect
dialog would have been the tidier data flow, since a bookmark is already in hand
there, but the tracker, `/server` and the CLI never pass through it.

An override reaches the preamble as a one-shot armed immediately before the
connect, consumed when applied. One connect, one override: a bookmark's
nickname cannot leak onto a later `/server`.

**This is also what makes `/nick` a runtime command rather than a sticky one.**
It changed the connection and nothing reset it, so the name survived a
reconnect — which contradicted "as if the command had never been typed" even
after the binder was gone. Every connect now re-resolves, so it doesn't.

The chain is three deep, and each level answers a different question: the
override is what this connection is configured to show, the global is what
everything unspecialised shows, and the startup value is what a profile that
has never set a nickname shows — `$USER`, stamped before the settings file is
read and remembered so a reconnect can restore it.

**Icon zero is a real icon.** The copy used to skip a zero icon as "nothing
stored", so choosing the blank icon in Settings silently did nothing. It is set
unconditionally now, and the per-connection override is an `Option` rather than
a sentinel for the same reason.

**The UI is one row, and only for the nickname.** `AdwEntryRow` has no
placeholder — the title *is* the placeholder — so the inherited value goes in
the title, and an empty field reads as "you will appear as this". Clearing it
goes back to inheriting, because empty is stored as absent rather than as an
empty string. The icon override is storage-only for now: zero is a legal icon
so it cannot double as "unset" in a spin row, and the picker is still the C
settings page.

---

## What this does not absorb

Deliberately out of scope, each with a reason:

- **`dock-layout.ini`** — the split tree, divider positions and undocked panel
  sizes. The split is clean in the code — the layout file owns everything
  *inside* the window, the settings file owns the toolbar window's outer size,
  and the layout writer never touches the preferences struct. The only coupling
  is save ordering at quit. (The layout header contradicts itself on this: its
  summary claims it saves the toolbar window size, and a later comment correctly
  says that lives in the settings file. Worth fixing.) Converting it to TOML under the same crate is a reasonable follow-on,
  but it is not a prerequisite and bundling it doubles the risk.
- **`known_hosts`** — TLS pins. Its SSH-shaped format is a deliberate interop
  choice and its comment-preserving rewrite is more careful than anything here.
- **Theme files** — read-only, and a shareable per-theme format is the point.
  See [theming-file-format.md](theming-file-format.md).
- **`avatar.gif`** — binary, correctly its own file.
- **The Settings *dialog* framework.** Moving the last two C pages (Identity's
  resource-fork icon picker, Voice's device combos and key capture) is real work
  and is independent of where the values live. It should follow, not block.

---

## Migration

One-shot, on first run of a build that has `hxconfig`, the way the legacy
bookmark import already works:

1. No `gtkhx.toml`, but a `gtkhxrc` exists → read it, map old keys to new paths,
   and hand back the result. Saving is the caller's decision, so a first run
   that falls over doesn't leave a half-migrated file behind.
2. Leave `gtkhxrc` in place. Do not delete, do not rename. If the new build is
   abandoned, the old one still works.
3. Neither file → defaults, and the first-run path continues to do whatever the
   new-user experience work decides.

A `gtkhx.toml` that exists but is *corrupt* deliberately does **not** fall back
to the old file. The salvage path preserves it and the user carries on at
defaults; silently reverting to a years-old `gtkhxrc` because today's file has a
typo in it would be a stranger thing to do than starting fresh.

The read side is reimplemented in Rust rather than borrowed from C, because the
crate has to stay linkable-by-nothing and testable headless. That means owning
both on-disk forms — the GKeyFile `[gtkhx]` group and the pre-GKeyFile
`KEY=VALUE` lines — and the inverse of `g_key_file_set_string`'s escaping, which
turns out to cover only four things: `\` → `\\`, LF, CR, and *leading*
whitespace. Not the list separator, not commas, `#`, `=`, brackets, interior
tabs, or trailing spaces. Anything more aggressive would corrupt values that
were never escaped to begin with — the default timestamp format ends in a space
and survives precisely because trailing space is not escaped.

A legacy file **need not be UTF-8**, and treating that as unreadable would reset
every setting a user had, once, with no way back. The C reader knows this — its
`STRING32` arm validates and, on failure, runs the bytes through Mac Roman,
because a nickname typed on a Mac-era client breaks GTK's input method
otherwise — and `g_key_file_set_string` never validated, so those bytes have
round-tripped through every save since. Unix paths are bytes rather than text
too. So: try UTF-8, fall back to Mac Roman for the whole file, and say so. Mac
Roman rather than a lossy decode because every byte maps to a defined character,
so nothing becomes U+FFFD.

**One bug in the old line parser is deliberately not reproduced**: it dropped a
final line with no trailing newline. That is a `fgets` loop mistake with no
rationale behind it, so reading the line can only recover a setting that was
being thrown away.

Its `#` truncation, which looks like the same class of thing, *is* reproduced —
and only in the line form. `#` was the comment convention that format's only
parser ever defined, so cutting there is the format being parsed correctly
rather than a value being lost: a hand-written `XBUF_MAX=1000 # lines` means a
thousand lines, and refusing to truncate would turn it into an unparseable
number and silently fall back to the default. The GKeyFile form has no inline
comments at all — `NICK=bob # hi` really is the value `bob # hi` — so it must
not truncate, and doesn't.

**The escape asymmetry can only be undone once.** Each save/load cycle doubled
every backslash, and nothing on disk records how many cycles happened. The
migration unescapes one level and, if the result still contains a doubled
backslash, hands the value over *with a diagnostic naming the old key* rather
than guessing at the rest. The realistic victim is a Windows download path.

That diagnostic is scoped to the GKeyFile form. Nothing ever wrote the line
form through `g_key_file_set_string`, so nothing escaped it and nothing
unescapes it; a doubled backslash there is a UNC path, and warning about it
would explain a bug that cannot have happened to that file.

The C boolean parser is reproduced exactly, first-character semantics and all,
so `tarantino` is true and `nautical` is false. That is silly, and it is also
what every existing file was written and read against — including the era when
the writer emitted `true`/`false` while the reader accepted only `0`/`1`, which
reverted every toggle to its default on each startup. Reproducing the parser is
what stops a migration from repeating that.

The mapping is mechanical for almost everything. The cases that are not:

| Old | New | Note |
|---|---|---|
| `TRACKER` | `trackers.addresses` | split on comma, trim |
| `HIGHLIGHTWORDS` | `chat.highlight_words` | same |
| `NICK` / `ICON` | `identity.*` | read from the connection buffers they alias today |
| `CHATXSIZE`, `CHATYSIZE`, `NEWSXSIZE`, `NEWSYSIZE`, `TASKXSIZE`, `TASKYSIZE`, `USERXSIZE`, `USERYSIZE` | — | dropped; never read. **Not** `TOOLXSIZE`/`TOOLYSIZE`, which are live and map to `window.*` |
| `OPENCHAT`, `OPENNEWS`, `OPENTASKS`, `OPENUSERS` | — | dropped; one-way latches, no UI, never cleared |
| `TIME` | — | dropped; it went with `/stats` |
| `FILE_SAMEWINDOW`, `NEWS_SAMEWINDOW` | — | dropped; retired before the current table and already ignored on load, but named so an old profile doesn't trip the unknown-key diagnostic |
| everything else | its table | rename only |

`LOGGING` is deliberately *not* in the map. It has a macro in `cfgkeys.h`, but
its table entry has always been `#if 0`'d, so the writer never emitted it and no
real file contains it. If one somehow does, it lands in the unknown-key
diagnostic, which is where a key nobody recognises belongs.

"Rename only" is doing some work in that last row, because the renames are not
all mechanical — the schema expands abbreviations the old keys compressed.
`SOUNDMSG` and `NOTIFYMSG` become `sound.private_message` and
`notify.private_message`; everything spelled `PCHAT` becomes `private_chat`;
`SOUNDFILE` and `NOTIFYXFER` become `sound.transfer` and `notify.transfer`;
`SOUNDPART` becomes `sound.leave`; `XBUF_MAX` becomes `chat.scrollback_lines`;
`OLD_NICKCOMPLETION` becomes `chat.legacy_nick_completion`; `DOWNLOAD` becomes
`transfers.download_dir`; `THEME` and `THEMENAME` become
`appearance.color_scheme` and `appearance.theme`, which is the pair most likely
to be transposed by someone working quickly. The implementation wants the full
mapping written out once as a table in the migration module, where it can be
tested, rather than inferred per key. `hxconfig::PATHS` is the other half of
that test: it lists every path the schema knows, in file order, so the assertion
can run both ways.

Migration is a pure function from a key/value map to a *document*, not to the
typed struct — so the type conversion, the range checks and the diagnostics are
the ordinary load path doing its ordinary job, rather than a second
implementation of them that could disagree. It tests without touching a
filesystem, and does so against a real `gtkhxrc` captured from a live profile,
committed as a fixture beside the crate.

Coverage is asserted in three directions, so a key added to one side and
forgotten on the other fails the test rather than the user: every key the C
table has resolves to something here; every target is a path the schema really
has; and every path the schema has is fed by some old key or is named in
`NEW_PATHS` to say that defaulting is intended. The third is the one that
catches a *new* setting silently defaulting for everyone who upgrades.

---

## A plausible decomposition

Illustrative, not committed. Each step ships on its own.

| Step | Scope | Depends on |
|---|---|---|
| ~~P1~~ | **Done.** `hxconfig` crate: typed schema, TOML load/save through `toml_edit`, atomic write, version check, defaults. Pure Rust, unit-tested, linked by nothing. | — |
| ~~P2~~ | **Done.** Migration: the legacy reader for both on-disk forms, the key mapping, the one-shot import, and a round-trip test against a captured real `gtkhxrc`. Still linked by nothing. | P1 |
| ~~P3~~ | **Done.** The C mirror and the read/write ABI. `hxconfig` owns the values from startup; the settings table, the `allocated` bit and the identity binder are deleted; the mirror is refreshed after every change. The panel-open latches went with their keys, and the two toolbar-size writes are setter calls. The by-name bridge is reimplemented on `hxconfig`, so both the C and the Rust Settings pages work unchanged. Change hooks are applied uniformly after load, which absorbed most of P4. | P2 |
| ~~P4~~ | **Done.** Change notification: hooks split into view / global / connection flavours, with the connection chosen once at the dispatch site. The "applied uniformly after load" half landed with P3. | P3 |
| ~~P5~~ | **Done.** Identity: global default plus per-connection overrides, resolved and copied in the connect preamble. Delivers M1's identity half. | P3 |
| P6 | Port the Identity and Voice settings pages to Rust; retire the `draw` pointer framework. | P3 |
| P7 | Drop the mirror per file as each C reader is ported. Ongoing, not a milestone. | P3 |

P1 and P2 were self-contained and landed with no risk to a running build —
nothing linked the crate, so the binary was byte-identical either way. P3 was
the one with a real blast radius, and landed alone.

---

## Settled

1. **Unknown-key preservation** — preserve, by editing the loaded document in
   place. Neither of the two candidates weighed (a `toml::Table` beside the
   typed struct, or a `#[serde(flatten)]` catch-all) would have kept comments;
   `toml_edit` keeps those too. The cost stands: the file can accumulate junk
   from an abandoned newer build, and that is the right trade against making
   hand-editing safe. See "Versioning that is actually checked" above.
2. **`[state]`** — gone, along with the `/stats` command and the `TIME` key that
   fed it. Nothing else was ever in the table.

## Open questions

1. **Config directory resolution.** It is C-owned today and both Rust config
   crates call back into C for it; `hxconfig` takes it as a parameter, which is
   what lets it unit-test headless, and deliberately does not decide who
   resolves it. The question is still whether to keep the current hand-rolled
   resolution — `$GTKHX_PATH`, then `$XDG_CONFIG_HOME/gtkhx`, then
   `$HOME/.config/gtkhx` on *all three platforms* — or move to platform-native
   paths. Moving is a migration in itself and would strand existing macOS and
   Windows profiles. Needs answering by P3, not before.
2. **Should the bookmark store adopt `hxconfig`'s write helper** in the same
   change, or separately? Its fixed temp filename and missing `fsync` are real,
   if small. `hxconfig` has the hardened copy now, so this is a lift-and-share
   rather than a rewrite.
3. **Per-connection settings beyond identity.** Once connections are
   configuration, other things become plausibly per-connection — download
   directory, notification preferences, autojoin. Not for v1, but the schema
   should not make it awkward.

## What this is *not*

- Not a change to the wire protocol.
- Not a redesign of the Settings dialog's information architecture. Same pages,
  same grouping, plus Connections.
- Not a migration of the dock layout, themes, TLS pins or the avatar.
- Not a commitment to porting the last two C settings pages as part of this.
