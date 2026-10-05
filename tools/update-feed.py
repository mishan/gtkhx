#!/usr/bin/env python3
"""Write updates.json with one channel moved to a new release.

    gh release view v1.4.1 --json tagName,publishedAt,url,assets > release.json
    tools/update-feed.py --channel stable --release release.json \\
        [--feed live-updates.json] > updates.json

The publish workflow in mishan/gtkhx-flatpak runs this; the shape it writes
is the one hxupdate reads, described in docs/updates.md. The other channel's
entry is kept as it was, except that a stable release newer than the beta
moves the beta along with it, as it does in the Flatpak repository.

A channel never moves backward, and a beta never falls behind stable: the
publish stops instead.
"""

import argparse
import json
import re
import sys

SITE = "https://dl.gtkhx.org"

# Release asset suffix to the package name the feed lists it under.
ASSETS = {
    "-win64.zip": "windows",
    "-macos-arm64.zip": "macos-arm64",
    "-macos-x86_64.zip": "macos-x86_64",
}

FLATPAKREF = {"stable": "gtkhx.flatpakref", "beta": "gtkhx-beta.flatpakref"}

VERSION = re.compile(r"v?(\d+)\.(\d+)\.(\d+)(?:(-dev)|b(\d+)|rc(\d+))?")


def order(version):
    """The ordering hxupdate::Version gives, as a sortable tuple."""
    m = VERSION.fullmatch(version)
    if not m:
        sys.exit(f"update-feed: can't read version {version!r}")
    major, minor, patch, dev, beta, rc = m.groups()
    if dev:
        stage = (0, 0)
    elif beta:
        stage = (1, int(beta))
    elif rc:
        stage = (2, int(rc))
    else:
        stage = (3, 0)
    return (int(major), int(minor), int(patch)) + stage


def entry(release, channel):
    downloads = {}
    for asset in release["assets"]:
        for suffix, name in ASSETS.items():
            if asset["name"].endswith(suffix):
                downloads[name] = asset["url"]
    downloads["flatpak"] = f"{SITE}/{FLATPAKREF[channel]}"
    return {
        "version": release["tagName"].removeprefix("v"),
        "date": release["publishedAt"][:10],
        "notes_url": release["url"],
        "downloads": downloads,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--channel", required=True, choices=FLATPAKREF)
    ap.add_argument("--release", required=True, help="gh release view --json output")
    ap.add_argument("--feed", help="the feed as it is now; omit to start one")
    args = ap.parse_args()

    with open(args.release, encoding="utf-8") as f:
        release = json.load(f)
    feed = {"schema": 1, "channels": {}}
    if args.feed:
        with open(args.feed, encoding="utf-8") as f:
            feed = json.load(f)
        if feed.get("schema") != 1:
            sys.exit(f"update-feed: unknown schema {feed.get('schema')!r}")
    channels = feed["channels"]

    new = entry(release, args.channel)
    current = channels.get(args.channel)
    if current and order(new["version"]) < order(current["version"]):
        sys.exit(
            f"update-feed: {args.channel} is at {current['version']};"
            f" {new['version']} would move it backward"
        )
    stable = channels.get("stable")
    if args.channel == "beta" and stable and order(new["version"]) < order(stable["version"]):
        sys.exit(
            f"update-feed: stable is at {stable['version']};"
            f" beta {new['version']} would fall behind it"
        )

    channels[args.channel] = new
    beta = channels.get("beta")
    if args.channel == "stable" and (not beta or order(beta["version"]) < order(new["version"])):
        channels["beta"] = entry(release, "beta")

    json.dump(feed, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
