#!/usr/bin/env python3
"""tools/update-feed.py: how publishing moves the channels, and when it refuses.

hxupdate's own tests parse update-feed/feed-after.json, so the generator and
the reader agree on the shape.
"""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "update-feed.py"
FIXTURES = ROOT / "tests" / "update-feed"


def run(channel, release, feed=None):
    with tempfile.TemporaryDirectory() as tmp:
        rel = pathlib.Path(tmp) / "release.json"
        rel.write_text(json.dumps(release))
        args = [sys.executable, TOOL, "--channel", channel, "--release", rel]
        if feed is not None:
            old = pathlib.Path(tmp) / "feed.json"
            old.write_text(json.dumps(feed))
            args += ["--feed", old]
        return subprocess.run(args, capture_output=True, text=True)


def release(tag):
    base = json.loads((FIXTURES / "release.json").read_text())
    return dict(base, tagName=tag)


def feed(stable, beta):
    channels = {}
    for name, version in (("stable", stable), ("beta", beta)):
        if version:
            channels[name] = {"version": version}
    return {"schema": 1, "channels": channels}


class UpdateFeed(unittest.TestCase):
    def test_fixture(self):
        out = run(
            "stable",
            json.loads((FIXTURES / "release.json").read_text()),
            json.loads((FIXTURES / "feed-before.json").read_text()),
        )
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(json.loads(out.stdout), json.loads((FIXTURES / "feed-after.json").read_text()))

    def test_channels(self):
        # (channel, tag, stable before, beta before, stable after, beta after);
        # None after means the publish is refused.
        cases = [
            ("beta", "v1.4.1b1", None, None, None, "1.4.1b1"),
            ("stable", "v1.4.1", None, None, "1.4.1", "1.4.1"),
            ("beta", "v1.4.2b1", "1.4.1", "1.4.1", "1.4.1", "1.4.2b1"),
            ("stable", "v1.4.1", "1.4.0", "1.4.2b1", "1.4.1", "1.4.2b1"),
            ("stable", "v1.4.2", "1.4.1", "1.4.2rc1", "1.4.2", "1.4.2"),
            ("stable", "v1.4.1", "1.4.1", "1.4.1", "1.4.1", "1.4.1"),
            ("stable", "v1.4.0", "1.4.1", None, None, None),
            ("beta", "v1.4.1b1", None, "1.4.1b2", None, None),
            ("beta", "v1.4.1b2", "1.4.1", None, None, None),
        ]
        for channel, tag, stable, beta, want_stable, want_beta in cases:
            with self.subTest(channel=channel, tag=tag, stable=stable, beta=beta):
                out = run(channel, release(tag), feed(stable, beta))
                if want_beta is None:
                    self.assertNotEqual(out.returncode, 0)
                    continue
                self.assertEqual(out.returncode, 0, out.stderr)
                got = json.loads(out.stdout)["channels"]
                self.assertEqual(got.get("stable", {}).get("version"), want_stable)
                self.assertEqual(got["beta"]["version"], want_beta)


if __name__ == "__main__":
    unittest.main()
