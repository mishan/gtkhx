# Update notices

GtkHx can tell its user that a newer version is out. It never downloads or
installs anything itself: the notice is a banner across the top of the main
window, and acting on it is up to the user. There is no desktop notification.

## Which builds check

Whether a build checks at all is decided when it is configured, with
`-Dupdate_check`:

| Value | Meaning |
|---|---|
| `auto` (default) | On when the host is Windows or macOS, off everywhere else. |
| `enabled` | On. The Flatpak manifest passes this. |
| `disabled` | Off: nothing checks, and no switch appears in Settings. The banner's slot is still packed, hidden. |

It is a combo rather than a meson feature so that `--auto-features=enabled`,
which Fedora's `%meson` and Arch's `arch-meson` pass, leaves it alone.

`auto` stands for the Windows and macOS builds GtkHx ships itself. A package
that a package manager updates already has something telling its user what is
newer. **Packagers: on Linux, leave it at `auto` or pass `disabled`; for
Homebrew, MacPorts or MSYS2, pass `disabled`, since `auto` turns it on there.**

Where the build allows it, the user still decides: Settings → General →
Updates → "Check for updates" (`updates.check`, on by default). The group is
absent from a build that can't check, and stays hidden in every build until a
check is wired up, so there is never a switch that does nothing.

## How a build checks

- **Flatpak** asks Flatpak, through the portal, whether its own installation
  has an update. The repository behind it is `https://dl.gtkhx.org`. Nothing
  else is contacted.
- **Everything else** fetches `https://dl.gtkhx.org/updates.json` at most once
  a day. The request carries no information about the user or the servers
  they visit, and the comparison happens locally; the address it comes from
  and the User-Agent are visible to the site, as with any request.

## The feed

```json
{
  "schema": 1,
  "channels": {
    "stable": {
      "version": "1.4.1",
      "date": "2026-10-01",
      "notes_url": "https://…",
      "downloads": { "windows": "https://…", "macos-arm64": "https://…" }
    },
    "beta": { "version": "1.4.2b1", "…": "…" }
  }
}
```

Fields this version doesn't know are ignored, so the feed can grow without
breaking builds already out there. A channel that doesn't parse reads as
absent, so a mistake in one leaves the other working. A different `schema` is
refused rather than guessed at, and so is a feed over a size cap. The parser and the decision live
in the pure `hxupdate` crate.

## Versions

A version is `MAJOR.MINOR.PATCH` with an optional leading `v` and one optional
stage, ordered

`1.4.1-dev < 1.4.1b1 < 1.4.1b2 < 1.4.1rc1 < 1.4.1 < 1.4.2-dev`

Anything else (a snapshot named after its commit, say) can't be read, and an
unreadable version never produces a notice, on either side of the comparison.

A release hears only about the stable channel. A pre-release (a `-dev`, beta or
release-candidate build) also hears about betas, so nobody is moved onto a beta
who didn't already choose one. The notice is the newest entry newer than the
build that isn't the skipped version, and the release when both channels name
the same version. "Skip this version" records the version in
`updates.skip_version`; a later one is announced again, and skipping a beta
doesn't hide a newer release. Whether the next step is to fetch or to wait,
the last feed's notice stays up, so a failed fetch doesn't hide a known
update.

The version a build reports is `-Dbuild_version`, falling back to the project
version in `meson.build`. The package workflow sets it from the version it was
given, so a beta reports `1.4.1b1` rather than the tree's `1.4.1-dev`. About,
`--version` and the Windows executable's version strings show the same string.
Release tags are checked against this shape when the release workflow runs, so
a tag the check couldn't read fails the release instead of shipping a build
that never hears of an update.

Two places still carry the project version: the Windows executable's numeric
version fields, which windres can't fill from `1.4.1b1`, and the macOS bundle's
`Info.plist`, which `packaging/macos/bundle.sh` reads from `meson.build`.

## Open

- The Flatpak portal query and the feed fetch are not wired up yet; the
  banner is in place and stays hidden, and so does the Settings switch.
- Where the last check time is stored, and backing off after a failed fetch,
  arrive with the fetch.
- `https://dl.gtkhx.org` is not live yet.
