# Multi-Server Test Harness — Research Spike

Spike notes drafted 2026-05-18, revised same day after the
vespernet fingerprinting question was answered. Two questions:

1. Does Mobius support the Capabilities-Chat-History extension yet?
2. How would we structure a multi-server test setup so we can run
   integration tests against more than one Hotline server family?

Conclusion preview:

- Mobius does **not** support chat-history yet. The server that
  does is **Janus** (closed-source, runs on hotline.vespernet.net,
  publicly-downloadable Linux amd64 binary at get.vespernet.net).
- We should build a Docker-Compose-based multi-server harness with
  capability-aware test selection, with Janus packaged as our
  chat-history test target. The mock-server option from the original
  draft is no longer needed.

---

## Part 1 — Mobius chat-history status

### What we found

**The chat-history-capable server is Janus, VesperNet's own
closed-source Hotline server, running on `hotline.vespernet.net`.**

Janus is part of VesperNet's broader Hotline stack:

- **janus** — server (v2.0.7 as of May 2026)
- **argus** — tracker (v1.0.2)
- **hermes** — client (v0.1.6, Mac arm64 + Windows only)
- **Mnemosyne** — content-sync / indexing service
- **agora.vespernet.net** — directory site + downloads

Source is closed, but every binary is publicly downloadable at
`get.vespernet.net`. Linux amd64 archive is ~9.5 MB. The release
page at `agora.vespernet.net/janus` lists SQLite-backed chat
persistence implementing the fogWraith spec — Janus is the only
public server we know of that ships this extension today.

Other Janus features that matter to GtkHx tests: full Hotline
protocol, HOPE ChaCha20-Poly1305, native TLS on a separate port,
large-file support, Lua plugin system, IRC bridge, NewsBridge
(NNTP ↔ Hotline), tracker v3 registration. Voice chat via WebRTC
SFU is out of scope for GtkHx.

For comparison, the canonical *open-source* Mobius
(`jhalter/mobius`) v0.21.0 (Mar 2026) has:

- **No protocol constants** for `DATA_HISTORY_ENTRY` (0x0F05),
  `TRAN_GET_CHAT_HISTORY` (700), or `CAPABILITY_CHAT_HISTORY`
  (bit 4) in the source.
- **Issue #105** — "Send the last X lines of chat when connecting"
  (Knezzen, opened 2023-09-30) is the closest prior art. Still
  *open*. jhalter's response: "I'll consider this as an option for
  the server config." This isn't even the full extension — it's
  the simpler "legacy broadcast" path that the spec describes as
  a pragmatic bridge for non-history clients.

So Mobius will likely add chat-history *eventually*, but Janus is
the only target available today.

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

Phase 2–4 (render + scrollback + catch-up): use Janus for Tier 3.
The Janus Linux amd64 binary is publicly downloadable from
`get.vespernet.net` — we wrap it in a Docker container the same way
we already wrap mhxd, and add a row to the test-server matrix with
the chat-history cap bit set. Mock-server option (originally Phase C
of this plan) is no longer needed.

Janus container shape (Phase C below):

- Base image: `debian:bookworm-slim` (the closed-source binary is
  a statically-linked Go-like single binary — minimal runtime deps).
- Build step: `curl -fsSL https://get.vespernet.net/janus-linux-amd64.tar.gz`
  + sha256 verify against a checked-in expected digest, untar to
  `/opt/janus`.
- Config: checked-in `tests/janus/conf/` directory copied into the
  container — exact format TBD on first build but should mirror the
  `tests/mhxd/conf/` layout (server config + accounts + initial
  files).
- We are NOT redistributing — Docker pull happens at image-build
  time, same legal posture as the placehold.co banner fetches in
  tests/mhxd/.
- Pin the upstream URL by sha256, not by version string. If a new
  Janus release breaks our config, we'll see it as a checksum
  mismatch at build time and bump deliberately.

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

| Service  | Image                                            | Port | Notes                                                 |
| -------- | ------------------------------------------------ | ---- | ----------------------------------------------------- |
| `mhxd`   | (built from vendored `mhxd/`)                    | 5500 | Current target — keep as-is.                          |
| `mobius` | `ghcr.io/jhalter/mobius-hotline-server:v0.21.0`  | 5510 | 1.9-style server, pinned tag for stability.           |
| `janus`  | (built from `tests/janus/`, fetches at build)    | 5520 | Closed-source. Chat-history capable. Sha256-pinned.   |

Pinning tags is important — Mobius makes silent wire-format-affecting
changes between minor releases. We bump the pin deliberately, with
a test run, when we want to move forward. Janus is sha256-pinned
rather than version-pinned because the upstream URL doesn't carry
a version suffix (the `get.vespernet.net/janus-linux-amd64.tar.gz`
URL always points at "current"); the checksum in our Dockerfile
both prevents silent updates and gives us a clear "bump required"
signal when VesperNet ships a new build.

Future slots:

- `badmoon` if a binary becomes available.
- A second `mobius` row pinned to a hypothetical chat-history-
  capable release, once Mobius merges issue #105 and the extension
  on top of it.

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

| Phase | Scope                                                                                                                              |
| ----- | ---------------------------------------------------------------------------------------------------------------------------------- |
| A     | Add `hx_test_server` matrix struct + helper + env-var filter; current mhxd-only target acts as the only entry. No behavior change. |
| B     | Add Mobius to the matrix. Audit failures — expect a handful of Mobius-quirk surprises. Fix or skip-flag.                           |
| C     | Build Janus test container (`tests/janus/`), add to matrix with `CAP_CHAT_HISTORY` set.                                            |
| D     | Wire chat-history tests against Janus (Phase 2/3 of the chat-history plan).                                                        |
| E     | When Mobius merges chat-history, add a second Mobius row to the matrix with that cap bit; the existing tests light up against it.  |

Phases A–B are independent of the chat-history work and pay off
immediately (catch Mobius regressions in CI). Phase C is gated on
chat-history Phase 2 starting — no point packaging Janus before
we're ready to write tests against it.

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
5. ~~**Cost of the mock.**~~ Mock cancelled in favour of packaging
   Janus (the closed-source server vespernet runs). See Part 1's
   "What this means for our chat-history work" section for the
   build approach.
6. **Janus binary stability.** The Linux amd64 binary at
   `get.vespernet.net/janus-linux-amd64.tar.gz` is unversioned —
   the URL always points at "current". A new VesperNet release
   could change behavior overnight. Sha256-pinning the tarball in
   our Dockerfile gives us a deliberate-bump signal at image-build
   time when the upstream churns. Acceptable.

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
