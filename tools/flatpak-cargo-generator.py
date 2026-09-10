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
#
# Two kinds of dependency are vendored:
#
#   crates.io   each .crate archive is unpacked into cargo/vendor/, and
#               crates-io is replaced with that directory.
#   git         each repository is checked out at the locked commit under
#               cargo/git/, and that git source is replaced with the
#               checkout's directory of crates. The crates are read in
#               place, inside their own workspace, so `workspace = true`
#               inheritance in their manifests still resolves.
#
# Finding which directory of a repository holds its crates needs the
# repository itself, so a lockfile with git dependencies makes this script
# fetch each locked commit (shallow). Any other kind of source is an error:
# a dependency left out of the vendored set only fails at release time,
# inside the build sandbox.

import json
import os
import posixpath
import subprocess
import sys
import tempfile
import tomllib
from urllib.parse import parse_qs, urlsplit

CRATES_IO = "registry+https://github.com/rust-lang/crates.io-index"

# The gtkhx module's build directory inside the sandbox. Cargo resolves a
# relative source directory from the working directory (the Meson build
# dir), not from CARGO_HOME, so every directory below is absolute.
BUILD_ROOT = "/run/build/gtkhx"


def die(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def read_lock(path):
    with open(path, "rb") as f:
        return tomllib.load(f).get("package", [])


def registry_sources(packages):
    sources = []

    for pkg in packages:
        name, version = pkg["name"], pkg["version"]
        checksum = pkg.get("checksum")
        if not checksum:
            die(f"{name} {version}: crates.io package with no checksum")

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

    return sources


def parse_git_source(source):
    """Split a lockfile git source into (spec, url, query, commit).

    `git+https://host/owner/repo.git?rev=abc#<commit>` gives spec
    `https://host/owner/repo.git?rev=abc`, the bare url, {"rev": "abc"} and
    the locked commit.
    """
    spec, _, commit = source.removeprefix("git+").partition("#")
    split = urlsplit(spec)
    url = split._replace(query="", fragment="").geturl()
    query = {key: values[0] for key, values in parse_qs(split.query).items()}
    return spec, url, query, commit


def crate_dirs_at(url, commit):
    """Map each package name in `url` at `commit` to its directory."""
    with tempfile.TemporaryDirectory() as tmp:

        def git(*args):
            try:
                return subprocess.run(
                    ["git", "-C", tmp, *args],
                    check=True,
                    capture_output=True,
                    text=True,
                ).stdout
            except subprocess.CalledProcessError as e:
                die(f"git {' '.join(args)}: {e.stderr.strip()}")

        git("init", "-q")
        git("fetch", "-q", "--depth=1", url, commit)

        dirs = {}
        for path in git("ls-tree", "-r", "--name-only", commit).splitlines():
            if posixpath.basename(path) != "Cargo.toml":
                continue
            package = tomllib.loads(git("show", f"{commit}:{path}")).get("package")
            if package:
                dirs[package["name"]] = posixpath.dirname(path)
        return dirs


def git_sources(packages):
    """Flatpak sources and cargo config for the lockfile's git packages."""
    by_source = {}
    for pkg in packages:
        by_source.setdefault(pkg["source"], []).append(pkg)

    sources = []
    config = ""

    for index, (source, pkgs) in enumerate(sorted(by_source.items())):
        spec, url, query, commit = parse_git_source(source)
        if not commit:
            die(f"{source}: git source with no locked commit")

        repo = posixpath.basename(urlsplit(url).path).removesuffix(".git")
        checkout = f"cargo/git/{repo}-{commit[:12]}/{repo}"
        dirs = crate_dirs_at(url, commit)

        # A directory source is a directory of crate directories, so every
        # locked crate from this repository has to share a parent. A crate
        # at the repository root works too: the checkout sits one level down
        # for exactly that case.
        roots = set()
        for pkg in pkgs:
            if pkg["name"] not in dirs:
                die(f"{pkg['name']}: no crate of that name in {url} at {commit}")
            crate = posixpath.normpath(posixpath.join(checkout, dirs[pkg["name"]]))
            roots.add(posixpath.dirname(crate))
        if len(roots) != 1:
            die(f"{url}: locked crates live in more than one directory")
        root = roots.pop()

        sources.append(
            {
                "type": "git",
                "url": url,
                "commit": commit,
                "dest": checkout,
            }
        )

        # Cargo reads every crate in a directory source, locked or not, and
        # each one needs a checksum file. Git sources carry no package
        # checksum in Cargo.lock, hence null.
        for crate in sorted(
            posixpath.normpath(posixpath.join(checkout, d)) for d in dirs.values()
        ):
            if posixpath.dirname(crate) == root:
                sources.append(
                    {
                        "type": "inline",
                        "contents": json.dumps({"package": None, "files": {}}),
                        "dest": crate,
                        "dest-filename": ".cargo-checksum.json",
                    }
                )

        config += f'\n[source."git+{spec}"]\ngit = "{url}"\n'
        for key in ("branch", "tag", "rev"):
            if key in query:
                config += f'{key} = "{query[key]}"\n'
        config += f'replace-with = "vendored-git-{index}"\n'
        config += (
            f"\n[source.vendored-git-{index}]\n"
            f'directory = "{BUILD_ROOT}/{root}"\n'
        )

    return sources, config


def generate_sources(lock_path):
    registry = []
    git = []

    for pkg in read_lock(lock_path):
        source = pkg.get("source")
        if source is None:
            continue  # a workspace member, built from the tree itself
        if source == CRATES_IO:
            registry.append(pkg)
        elif source.startswith("git+"):
            git.append(pkg)
        else:
            die(f"{pkg['name']} {pkg['version']}: unsupported source {source}")

    sources = registry_sources(registry)
    git_list, git_config = git_sources(git)
    sources += git_list

    # Cargo config that redirects crates-io and each git source to the
    # vendored directories.
    cargo_config = (
        "[source.crates-io]\n"
        'replace-with = "vendored-sources"\n'
        "\n"
        "[source.vendored-sources]\n"
        f'directory = "{BUILD_ROOT}/cargo/vendor"\n'
    ) + git_config
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
    n_repos = sum(1 for s in sources if s["type"] == "git")

    # Output next to the Cargo.lock
    out_dir = os.path.dirname(lock_path)
    out_path = os.path.join(out_dir, "cargo-sources.json")

    with open(out_path, "w") as f:
        json.dump(sources, f, indent=2)
        f.write("\n")

    print(f"wrote {out_path} ({n_crates} crates, {n_repos} git repositories)")


if __name__ == "__main__":
    main()
