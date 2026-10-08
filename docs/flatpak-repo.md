# The Flatpak repository

GtkHx's Flatpak is published to its own repository at `https://dl.gtkhx.org`, an
OSTree repository signed with GtkHx's key and served by GitHub Pages. Users install
from it with

```sh
flatpak install --user https://dl.gtkhx.org/gtkhx.flatpakref        # stable
flatpak install --user https://dl.gtkhx.org/gtkhx-beta.flatpakref   # beta
```

and from then on it updates like any other Flatpak app: `flatpak update`, a
software center's automatic updates, or the banner in docs/updates.md. The GNOME
runtime still comes from Flathub; the `.flatpakref` names Flathub as the runtime
repository and adds it if it is missing.

## What is where

The site is built and deployed by `publish.yml` in
[mishan/gtkhx-flatpak](https://github.com/mishan/gtkhx-flatpak). That repository
holds only the workflow; nothing it serves is ever committed, so its history
stays small however many releases go through it. Every run deploys the whole
site at once. Pages still caches each file on its own for a few minutes, so a
client can briefly see a summary and its signature or deltas out of step after
a publish; that fails one update, and the next one works.

What the site carries:

| Path | What |
|---|---|
| `repo/` | The OSTree repository: `app/com.nasledov.gtkhx/<arch>/{stable,beta}`, the AppStream branches, static deltas. |
| `gtkhx.flatpakref`, `gtkhx-beta.flatpakref` | One-step installs of each channel. |
| `gtkhx.flatpakrepo` | The repository alone, for `flatpak remote-add`. |
| `GtkHx-<arch>.flatpak`, `GtkHx-beta-<arch>.flatpak` | Single-file bundles of the signed commits. Installed, they set up the repository as their remote and update from it. |
| `gtkhx.gpg` | The public key. |
| `updates.json` | The feed the update check reads (docs/updates.md). |
| `index.html`, `gtkhx.png` | The landing page. |

The templates for all but the repository, the feed and the bundles live here, in
`packaging/flatpak/`; the workflow checks out `main` for them, and for
`tools/update-feed.py`. The `@GPGKEY@` in the templates is replaced with the
public key when the site is written.

The `.flatpak` attached to a GitHub release is not the same file as the one on
dl.gtkhx.org. Flatpak refuses to install a bundle that names a key unless its
commit is signed with it, and the release workflow never has the key, so the
release's bundle is unsigned and has no remote; it installs but never updates.
It is the input to publishing, and stays for anyone who wants that.

## How a release gets there

1. `release.yml` builds a tag and attaches the packages to a draft release; the
   Flatpak is built per architecture (x86_64 and aarch64), as
   `GtkHx-<tag>-<arch>.flatpak`.
2. Publishing the release starts `flatpak-dispatch.yml`, which asks
   gtkhx-flatpak's `publish.yml` to publish the tag. The tag picks the channel,
   not the pre-release box: `b` and `rc` tags go to beta, the rest to stable.
   `snapshot.yml` does the same after publishing a pre-release, since a release
   made with `GITHUB_TOKEN` fires no event; it publishes only from `main`, the
   one branch the dispatch environment allows.
3. `publish.yml` mirrors the live repository, imports the bundles, commits them
   signed onto the channel's branch, regenerates the summary, AppStream data and
   static deltas, writes the site and deploys it. It then waits for the live site
   to serve the new commits.

The dispatch token (`GTKHX_FLATPAK_DISPATCH_TOKEN`, in the `flatpak-dispatch`
environment, which only `main` and `v*` tags can use) can only start workflows in
gtkhx-flatpak. `publish.yml` downloads the release's assets itself, or rebuilds
the released tag when asked, so the token can't put anything into the
repository that isn't already a public release. The signing key lives only in
gtkhx-flatpak's `github-pages` environment, in a job that builds nothing and
runs no code from this repository; `update-feed.py` runs in a job of its own.

Run `publish.yml` by hand when the dispatch didn't happen or came too early: a
tag made before `flatpak-dispatch.yml` existed, a release published before its
packages were attached (the run fails without both architectures' bundles), or
a run GitHub cancelled while it waited, since it keeps only one run pending
behind the one in progress. The feed moves only with the Flatpak, so a
pre-release published without one doesn't reach `updates.json` either.

## Channels

`stable` and `beta` are branches of the same app, so both can be installed side
by side and `flatpak make-current` picks which one `flatpak run` starts. The beta
channel follows stable: publishing a stable release newer than the current beta
commits it to beta too, so a beta user is never behind. `tools/update-feed.py`
decides that, and the workflow commits to beta when the feed's beta entry is the
release it just published. A channel never moves to an older version, and a beta
never goes behind stable; the publish fails instead.

Each commit gets the current time, not the build's. Flatpak refuses an update
whose commit is older than the installed one, and a ref can move to an older
build when beta catches up with stable.

## Signing

The key is RSA-4096 with no expiry: a primary that only certifies
(`52EA6BE17DDA3820F9D27052B2F3E8C4817CDACD`) and a signing subkey
(`E9A3999C20D8D5423DE8E442A4DD6C217BA284EB`). CI holds only the subkey. The
primary, and its revocation certificate, are kept offline. No expiry because
OSTree treats a signature from an expired key as invalid, and an installed remote
never picks up a new key on its own.

`publish.yml` imports the subkey into a temporary `GNUPGHOME` for the one step
that signs, and removes it at the end of that step. Before deploying, it checks
the new summary and commits against `packaging/flatpak/gtkhx.gpg`, so a secret
that doesn't match the published key fails the run instead of publishing a
repository no client can verify.

Moving to a new key would mean signing with both for a while (`--gpg-sign`
twice) and asking every user to run `flatpak remote-modify --gpg-import=gtkhx.gpg`
on the remote GtkHx came from (`flatpak list --app --columns=application,origin`;
`gtkhx` from a `.flatpakref`, `gtkhx-origin` from a bundle). Avoid needing to.

## Size

`build-update-repo --prune-depth=2` keeps each branch's head and two commits
behind it, and the mirror pulls the same depth. Two are kept, not one, because
Pages caches for up to ten minutes and a client with the previous summary must
still find what it names. Each run regenerates the static deltas, from scratch
and from the parent, for every branch. That comes to a few tens of MB per
branch and architecture, far under Pages' 1 GB.

## Recovery

- **A run failed before deploying.** Nothing changed; run it again.
- **The live site is gone or unreadable.** A normal run stops before it builds
  or signs anything. When the site is really gone (a 404 for `repo/config`),
  run `publish.yml` with `bootstrap` for the newest stable release, then again without it for the
  newest beta. Installed clients see the new commits as updates, since every
  commit is newer than what they have.
- **The dispatch token expired.** Releases still publish; the dispatch job fails.
  Make a new fine-grained token with Actions read and write on gtkhx-flatpak
  only, store it as `GTKHX_FLATPAK_DISPATCH_TOKEN`, and run `publish.yml` by
  hand for anything missed.
- **A bundle installed before this repository existed** has no remote. Its user
  runs `flatpak uninstall com.nasledov.gtkhx` and installs from the
  `.flatpakref`; their data in `~/.var/app` stays.
- **A release's bundles are missing or unusable.** `source: rebuild` builds the
  tag from source, in a job of its own without the key, using the tag's own
  manifest with `separate-locales` forced off. That also serves a tag cut before
  the manifest kept translations in the app, whose bundles lack them.

## The first publish

`bootstrap` is never set by the dispatch; only a run started by hand can start a
new repository. The first release published after this setup goes:

1. `publish.yml` must already be on gtkhx-flatpak's `main`, or the dispatch
   has no workflow to start.
2. The release is published, and the dispatch starts `publish.yml`. With no site
   at dl.gtkhx.org yet, the run fails at its first check, saying to run again
   with `bootstrap`. Nothing is built, signed or deployed.
3. Run `publish.yml` by hand with the same tag and channel, `source: release`
   and `bootstrap` ticked. It starts an empty repository and feed, publishes the
   release into them and deploys.

From then on the dispatch works on its own. The stable `.flatpakref` and
bundles are missing until the first stable release is published.
