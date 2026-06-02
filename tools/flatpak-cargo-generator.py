#!/usr/bin/env python3
# Generate rust/cargo-sources.json from rust/Cargo.lock for the Flatpak
# build.  Run this whenever Cargo.lock changes so that flatpak-builder
# can pre-download all crate dependencies (the build sandbox has no
# network access).
#
# Usage:
#   python3 tools/flatpak-cargo-generator.py rust/Cargo.lock
#
# Output: rust/cargo-sources.json (referenced by com.nasledov.gtkhx.yml)

import json
import os
import sys


def parse_cargo_lock(path):
    """Parse Cargo.lock and yield (name, version, checksum) for registry crates."""
    with open(path) as f:
        content = f.read()

    current = {}
    for line in content.split("\n"):
        if line == "[[package]]":
            if (
                current
                and "source" in current
                and "registry+" in current.get("source", "")
                and "checksum" in current
            ):
                yield current["name"], current["version"], current["checksum"]
            current = {}
        elif "=" in line and not line.startswith("#"):
            key, _, val = line.partition("=")
            key = key.strip()
            val = val.strip().strip('"')
            if key in ("name", "version", "source", "checksum"):
                current[key] = val

    # Last package
    if (
        current
        and "source" in current
        and "registry+" in current.get("source", "")
        and "checksum" in current
    ):
        yield current["name"], current["version"], current["checksum"]


def generate_sources(lock_path):
    sources = []

    for name, version, checksum in parse_cargo_lock(lock_path):
        url = f"https://static.crates.io/crates/{name}/{name}-{version}.crate"

        # The .crate file is a gzipped tarball; extract into vendor dir.
        sources.append(
            {
                "type": "archive",
                "archive-type": "tar-gzip",
                "url": url,
                "sha256": checksum,
                "dest": f"cargo/vendor/{name}-{version}",
            }
        )

        # Cargo requires a .cargo-checksum.json alongside each vendored crate.
        sources.append(
            {
                "type": "inline",
                "contents": json.dumps({"package": checksum, "files": {}}),
                "dest": f"cargo/vendor/{name}-{version}",
                "dest-filename": ".cargo-checksum.json",
            }
        )

    # Cargo config that redirects crates-io to the vendored directory.
    # Use an absolute path because Cargo resolves relative paths from
    # the working directory (the Meson build dir), not from CARGO_HOME.
    cargo_config = (
        "[source.crates-io]\n"
        'replace-with = "vendored-sources"\n'
        "\n"
        "[source.vendored-sources]\n"
        'directory = "/run/build/gtkhx/cargo/vendor"\n'
    )
    sources.append(
        {
            "type": "inline",
            "contents": cargo_config,
            "dest": "cargo",
            "dest-filename": "config.toml",
        }
    )

    return sources


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <path/to/Cargo.lock>", file=sys.stderr)
        sys.exit(1)

    lock_path = sys.argv[1]
    if not os.path.isfile(lock_path):
        print(f"error: {lock_path}: not found", file=sys.stderr)
        sys.exit(1)

    sources = generate_sources(lock_path)
    n_crates = sum(1 for s in sources if s["type"] == "archive")

    # Output next to the Cargo.lock
    out_dir = os.path.dirname(lock_path)
    out_path = os.path.join(out_dir, "cargo-sources.json")

    with open(out_path, "w") as f:
        json.dump(sources, f, indent=2)
        f.write("\n")

    print(f"wrote {out_path} ({n_crates} crates)")


if __name__ == "__main__":
    main()
