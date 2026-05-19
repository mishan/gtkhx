# Multi-Server Test Harness — Research Spike

Spike notes drafted 2026-05-18. Two questions to answer:

1. Does Mobius support the Capabilities-Chat-History extension yet?
2. How would we structure a multi-server test setup so we can run
   integration tests against more than one Hotline server family?

Conclusion preview:

- Mobius does **not** support chat-history yet. No public server does.
- We should build a Docker-Compose-based multi-server harness with
  capability-aware test selection, and add a small mock server we
  control for chat-history coverage until real-server support exists.

---

## Part 1 — Mobius chat-history status

### What we found

**A live server already implements the extension: hotline.vespernet.net.**
Misha confirmed they've been connecting to it for chat-history testing.
We don't yet know what server software it runs — figuring that out is
a follow-up task. The vespernet.org/.net web presence describes them as
the authors of Hotline Tracker v3.0; they may have their own server
binary, or be running a private fork of Mobius / mhxd, or something
else entirely. Next-session task: capture a `GTKHX_DEBUG=proto` trace
of a login against hotline.vespernet.net and compare the login-reply
fingerprint (chunk ordering, `HTLS_DATA_VERSION`, banner-block shape)
against known families — see memory note
[[gtkhx-servers]] for the existing Mobius / mhxd / Badmoon
fingerprints we use to disambiguate.

Looking at the canonical *open-source* Mobius repo (`jhalter/mobius`),
latest release v0.21.0 (Mar 16, 2026):

- **No protocol constants** for `DATA_HISTORY_ENTRY` (0x0F05),
  `TRAN_GET_CHAT_HISTORY` (700), or `CAPABILITY_CHAT_HISTORY` (bit 4)
  in the source.
- **Issue #105** — "Send the last X lines of chat when connecting"
  (Knezzen, opened 2023-09-30) is the closest prior art. It's still
  *open*. jhalter's response: "I'll consider this as an option for
  the server config." This isn't even the full extension — it's the
  simpler "legacy broadcast" path that the spec describes as a
  pragmatic bridge for non-history clients.

So *public* Mobius doesn't ship chat-history yet. If vespernet turns
out to be running Mobius, it's a private branch or fork. If they're
running something else, that something else is the canonical
chat-history-capable server today.

### Who's likely to ship it first

The spec author is **Greg Gant** (GitHub: `fuzzywalrus`), who also
maintains [Hotline Navigator](https://github.com/fuzzywalrus/Hotline-Navigator),
a modern macOS/iOS/iPadOS/Windows/Linux Hotline client. Greg runs
his own server (`hotline.semihosted.xyz`) on Mobius — confirmed via
his Dec 2025 blog post about Mobius+VPS+WireGuard. The spec lives
in a separate documentation repo (`fogWraith/Hotline`) which contains
*only* protocol docs, not code. fogWraith publishes the spec; Greg
Gant authored it.

Most likely deployment order for chat-history server support:

1. Greg Gant lands client-side support in Hotline Navigator first
   (Hotline Navigator already has a local-encrypted-vault chat
   history feature, so the model is familiar).
2. Either Greg or a Mobius contributor (possibly Knezzen, who filed
   #105) lands server-side support in Mobius.
3. Other servers (mhxd, hlserver.com's mystery codebase) follow if
   at all.

Watching `jhalter/mobius` releases and `fuzzywalrus/Hotline-Navigator`
releases is the most reliable signal for "is a real server ready
yet."

### What this means for our chat-history work

Phase 1 (wire layer + parser): land on hand-rolled wire fixtures.
Tier 2 only, no Tier 3. This is fine — we already have wire-fixture
infrastructure for `hx_chat_event_new`, `hx_rcv_news_post`, etc.

Phase 2–4 (render + scrollback + catch-up): once we've fingerprinted
vespernet and know what software they're running, use **that** for
Tier 3 instead of building a mock. If the software is open-source
and Docker-packageable, add a container to the matrix the same way
we plan to add Mobius (Phase B below). If it's a private binary, we
can either ask the vespernet operators if they'd run a pinned test
instance for us, or fall back to a mock in the meantime.

Mock-server option is officially deprioritised — we'd rather find
out what the real-world chat-history-capable server is. The
research task is queued as #322 in the task list.

---

## Part 2 — Multi-server test harness design

### Current state

`tests/integration/` runs against a single mhxd Docker container.
The container is built from the vendored `mhxd/` source. Tests
assume the mhxd-specific environment — known quirks include
`reconn_time` spam protection, the "drift-with-use" seed content
problem (see `gtkhx_mhxd_test_state.md`), and the FieldOptions panic
behavior that's a Mobius thing not mhxd.

We've manually validated several wire-format fixes against Mobius
(hotline.semihosted.xyz, MacSecret, vespernet) and Badmoon out-of-CI,
which has the right *reach* but the wrong *latency* — a Mobius
regression has historically taken weeks to show up.

### What we want

- Run a test against multiple server families in a single CI cycle.
- Tests declare which families they're compatible with (a 1.9-only
  test shouldn't run against a 1.2-only server).
- Capability-gated tests skip targets that don't advertise the
  capability.
- Locally, scope to one family for fast dev-loop iteration.
- Easy to add a fourth, fifth, sixth server later.

### Design

#### Containers (Docker Compose)

A new `tests/integration/docker-compose.yml` brings up the matrix:

| Service             | Image                                              | Port  | Notes                                       |
| ------------------- | -------------------------------------------------- | ----- | ------------------------------------------- |
| `mhxd`              | (built from vendored `mhxd/`)                      | 5500  | Current target — keep as-is.                |
| `mobius`            | `ghcr.io/jhalter/mobius-hotline-server:v0.21.0`    | 5510  | 1.9-style server, pinned tag for stability. |
| `mock-chat-history` | (built from `tests/integration/mock-server/`, Go) | 5520  | Implements just enough for chat-history.    |

Pinning tags is important — Mobius makes silent wire-format-affecting
changes between minor releases. We bump the pin deliberately, with
a test run, when we want to move forward.

Future slots:

- `mobius-history` — once Mobius merges chat-history, pinned to that
  tag, replaces the mock for the relevant test suite.
- `badmoon` if a binary becomes available.
- `fogWraith-server` if/when that surfaces.

#### Capability matrix

Each container declares which capabilities its server supports.
Today this is hardcoded in the harness, since none of these servers
advertises capabilities consistently. Sketch:

```c
/* tests/integration/server_matrix.h */
typedef struct {
    const char *name;          /* "mhxd" | "mobius" | "mock" */
    const char *host;          /* container DNS name */
    guint16     port;
    guint16     hl_version;    /* 150 | 190 | 191 etc. */
    guint32     caps;          /* CAP_LARGE_FILES | CAP_CHAT_HISTORY | ... */
    guint32     skip_flags;    /* SKIP_TLS_HANDSHAKE, etc. */
} hx_test_server;

extern const hx_test_server hx_test_servers[];
extern const size_t        hx_test_server_count;
```

The harness reads this once at startup, then exposes a helper:

```c
/* tests/integration/helpers.c */
GArray *hx_test_servers_with (guint32 required_caps);
```

A test that needs a chat-history-capable server calls
`hx_test_servers_with (CAP_CHAT_HISTORY)`, gets back the matching
subset, and parameterizes itself over those.

Test pattern:

```c
static void test_chat_history_initial (gconstpointer data)
{
    const hx_test_server *srv = data;
    /* ... connect to srv->host:srv->port, run assertions ... */
}

void hx_register_chat_history_tests (void)
{
    GArray *servers = hx_test_servers_with (CAP_CHAT_HISTORY);
    for (guint i = 0; i < servers->len; i++) {
        const hx_test_server *srv = &g_array_index (servers, hx_test_server, i);
        gchar *path = g_strdup_printf ("/integration/chat-history/initial/%s",
                                       srv->name);
        g_test_add_data_func (path, srv, test_chat_history_initial);
        g_free (path);
    }
    g_array_unref (servers);
}
```

If only the mock is chat-history-capable, the test runs once
(`/integration/chat-history/initial/mock`). When Mobius gets support,
the same test starts running twice — and we'll see if our parser
choked on something Mobius does that the mock didn't.

#### Speed knob

For dev iteration, `GTKHX_TEST_SERVERS=mhxd meson test -C build` (or
similar env var on the existing harness) scopes to one family. CI runs
the full matrix. The harness honours the env var in
`hx_test_servers_with` — filter the matrix to the listed names before
returning.

#### The mock server

Lives at `tests/integration/mock-server/main.go`. Build target in the
Compose file:

- ~300 LOC of Go.
- Accepts TRANS 107 (login), echoes `CAPABILITY_CHAT_HISTORY` plus
  whatever the client advertised.
- Handles TRAN 700 with a deterministic in-memory message log
  pre-seeded with N entries (configurable via env var or config).
- Implements *just* the chat-history wire path — no file transfers,
  no news, no real user accounts. Anything else returns "not
  implemented" task errors. This keeps the surface small.
- Supports BEFORE/AFTER/LIMIT cursors and `has_more` correctly.
- Includes a tombstoned entry, an `is_action` entry, and an
  `is_server_msg` entry in the seeded log so we exercise all the
  flags.

Why Go: matches Mobius, easy cross-compile for the test container,
and we already pull `golang:alpine` for the mhxd build (vendored
mhxd is C, but the harness scripts use shell + a tiny Go helper
for some cases). One more Go binary is no incremental cost.

Why not extend mhxd or a Mobius fork: too much surface for what is
ultimately a test fixture. The mock is *intentionally* tiny so it
stays stable while we iterate on the client side.

#### Capability assertions across servers

A useful side effect of the matrix: tests for shared base-protocol
behaviour (login, chat round-trip, user list, ping) automatically
get cross-checked against all server families. This means
adding `mobius` to the matrix would surface a Mobius-specific bug
(like the FieldOptions panic we shipped a month ago) at CI time
instead of via user reports.

#### Build/CI

Add a new `.github/workflows/integration-multi-server.yml` (or
extend the existing `tests.yml`) that:

1. Builds the mhxd image (already done).
2. Pulls the pinned Mobius image (`ghcr.io/jhalter/mobius-hotline-server:v0.21.0`).
3. Builds the mock-chat-history image from `tests/integration/mock-server/`.
4. Brings up all three via Compose.
5. Runs the multi-server integration suite.

Estimated runtime impact: ~30s extra to cold-pull Mobius + ~10s to
build the mock. CI test runtime grows roughly linearly with server
count, since most tests fan out across the matrix — figure 2.5x the
current Tier 3 wall time when all three are in. Worth it.

### Phasing

| Phase | Scope                                                                                   |
| ----- | --------------------------------------------------------------------------------------- |
| A     | Add `hx_test_server` matrix struct + helper + env-var filter; current mhxd-only target acts as the only entry. No behavior change. |
| B     | Add Mobius to the matrix. Audit failures — expect a handful of mobius-quirk surprises. Fix or skip-flag.                            |
| C     | Build mock-chat-history server. Add to matrix with `CAP_CHAT_HISTORY` set.              |
| D     | Wire chat-history tests against the mock (Phase 2/3 of the chat-history plan).         |
| E     | When a real chat-history server ships, add it to the matrix with the same caps; same tests light up against it.                     |

Phases A–B are independent of the chat-history work and pay off
immediately (catch Mobius regressions in CI). Phase C is only worth
doing once we're actively building chat-history support.

### Open questions

1. **Single-process vs multi-container.** Could we run all three
   servers as background processes in one container instead of using
   Compose? Simpler, but Compose's networking and lifecycle handling
   are worth it.
2. **Test data isolation.** Each container needs its own state dir.
   Compose volumes handle this. Reset-between-tests behaviour might
   need a `compose down && up` between failing test sessions (we
   already do similar for the mhxd drift issue).
3. **Mobius config.** Mobius needs a config dir bind-mounted with
   `-init`-generated defaults. We commit a frozen copy in
   `tests/integration/mobius-config/` to keep CI deterministic.
4. **Versioned pinning policy.** When does the Mobius pin move? Probably
   "bump explicitly when we want to validate against a newer Mobius;
   never bump silently." Same policy as the GTK 4 dependency.
5. ~~**Cost of the mock.**~~ Mock punted in favour of using
   vespernet's real server as the chat-history test target. See the
   "What this means for our chat-history work" section above.

---

## Part 3 — mhxd test config: drop the sed/dd build pipeline

Sibling cleanup to the multi-server work. Today's `tests/mhxd/`
container build does an uncomfortable amount of shell metaprogramming
to coax a working test fixture out of upstream mhxd:

**Build-time, in `Dockerfile`:**

1. Two `sed -i` lines patching a real bug in mhxd's source code
   (`rsrc_size = sizeof(pathbuf)` → `resource_len(pathbuf)`).
2. Three `sed -i` lines patching `hxd.conf`: `ident 1 → 0`,
   `version 0 → 185`, `nospam yes → no`.
3. A ~10-line `dd` + `od` + `printf` + `dd` pipeline that masks one
   byte of the binary `accounts/guest/UserData` to clear five access
   bits and keep two (`download_files`, `upload_files`).
4. Another ~10-line `dd` + octal-`printf` pipeline that ORs another
   byte of `UserData` to enable `upload_folders` + `download_folders`.

**Runtime, in `docker-entrypoint.sh`:**

5. A 3-line `sed -i` block that patches the banner block's `type` /
   `file` / `url` fields based on `BANNER_MODE` / `BANNER_URL` /
   `BANNER_FILE` env vars.

Items 1–4 each have an essay-length comment explaining what the
shell magic is doing and why — collectively those comments are
~150 lines, longer than the actual hxd.conf. Item 5 is short but
still papering over what should just be a file replacement.

### Plan

Bake the desired state into checked-in files rather than producing
it via shell at build time.

- `tests/mhxd/conf/hxd.conf` — a fully formed config file with
  `ident=0`, `version=185`, `nospam=no` already in place. URL-mode
  banner block by default (matches today's default). The Dockerfile
  `COPY`s this over `/opt/mhxd/run/hxd.conf` after the upstream
  install step. Eliminates the three field-tweak `sed`s and ~70
  lines of explanatory comments along with them.
- `tests/mhxd/conf/accounts/guest/UserData` — a 32-byte binary blob
  pre-modified to set the access bits we want. Generated *once*
  via the same `dd`/`od`/`printf` ritual that runs in the Dockerfile
  today (we can do this offline and check the result in), or via a
  short Python/Go helper script committed alongside that produces
  the blob. Dockerfile `COPY`s the blob in. Drops ~40 lines of
  shell-magic + ~60 lines of explanatory comments.
- The mhxd source-code patch (item 1 above) graduates to a real
  patch file: `tests/mhxd/patches/folder-xfer-size.patch`. Applied
  via `git apply` or `patch -p1` after the `git clone`. Easier to
  audit, easier to upstream when the project gets around to it.
- The runtime banner-block sed (item 5) is the cheapest of the lot
  and only has a few lines of logic. Two options:
  - Leave it alone (it's small and works).
  - Replace with three pre-baked snippet files
    (`banner-block-url.conf`, `banner-block-gif.conf`,
    `banner-block-jpeg.conf`) and a small `cp`/`cat` swap in the
    entrypoint. Slightly cleaner but more files.
  Lean toward leaving it alone unless we want to add more
  runtime-configurable test modes later.

### Outcome

`Dockerfile` goes from ~370 lines (most of it commentary explaining
shell magic) to maybe 150 — just the build-deps install, the
`git clone` + patch + `make`, the `COPY` of our config tree, and the
runtime stage. The shell pipelines + 150 lines of comments become
two files (`hxd.conf` + `UserData` blob) plus a patch file.

Future-proofing: when we add Mobius to the matrix (Phase B above),
we'll do the analogous thing — `tests/mobius/config/` checked in
verbatim, `COPY` into the Mobius image, no shell preprocessing
between the source-of-truth and what mhxd / Mobius actually loads
at boot.

### Phasing

Independent of the multi-server work. Can land any time. Probably:

- Step 1: Generate the `UserData` blob, check it in, replace the
  two `dd` pipelines with a `COPY`.
- Step 2: Move the three `sed` field-tweaks into a checked-in
  `hxd.conf` and `COPY` over the upstream one.
- Step 3: Turn the source-code `sed`s into a real patch file.

Each step is a clean, small commit on its own branch. Reviewer
can verify the resulting container is byte-identical to today's
(modulo the explanatory comments going away) by diffing
`docker run gtkhx-mhxd cat /opt/mhxd/run/hxd.conf` and the
`UserData` blob between branches.

### Open question

- How easily does the upstream `hxd.conf` change between mhxd
  rebuilds? The Dockerfile clones master without a pin, so we're
  betting upstream's conf format stays stable. If a future mhxd
  rev adds a new required section, our checked-in conf might be
  missing it. Mitigation: pin the mhxd clone to a specific commit
  (we should probably do this anyway). Mark it as a TODO in the
  Dockerfile header.
