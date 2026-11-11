#!/usr/bin/env python3
"""Unpack the CPAN distributions listed in bundled-cpan.txt into perl5/cpan.

Usage: fetch-bundled-cpan.py <manifest> <perl5 dir>

Each archive is downloaded once into a per-user cache, verified against the
sha256 recorded in the manifest, and unpacked into perl5/cpan/<dir>. A
distribution already unpacked from the same archive is left alone, so the
script is safe to re-run. Prints the extension names, space separated, on
stdout; progress goes to stderr.

A manifest line may end with the option "core-makefile": the distribution's
Makefile.PL is dropped and the core's extension machinery writes one. It is
for a Makefile.PL that cannot run under the build's miniperl (one that loads
XS modules to probe the compiler). Whatever such a Makefile.PL would have
generated is kept in scripts/bundled-cpan/<dir>/ and copied over the unpacked
distribution.
"""
import hashlib
import os
import pathlib
import shutil
import sys
import tarfile
import urllib.request

MIRROR = os.environ.get("CPAN_MIRROR", "https://cpan.metacpan.org")
STAMP = ".bundled-archive-sha256"


def cache_dir():
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    return pathlib.Path(base) / "perl-wasm" / "cpan"


def archive(path, digest):
    cached = cache_dir() / digest
    if not cached.exists():
        cached.parent.mkdir(parents=True, exist_ok=True)
        tmp = cached.with_suffix(".part")
        with urllib.request.urlopen(f"{MIRROR}/authors/id/{path}") as resp, open(tmp, "wb") as out:
            shutil.copyfileobj(resp, out)
        tmp.rename(cached)
    got = hashlib.sha256(cached.read_bytes()).hexdigest()
    if got != digest:
        cached.unlink()
        sys.exit(f"!! {path}: sha256 is {got}, the manifest says {digest}")
    return cached


def unpack(src, dest):
    """Unpacks the archive's single top-level directory as dest."""
    tmp = dest.with_name(dest.name + ".unpack")
    shutil.rmtree(tmp, ignore_errors=True)
    with tarfile.open(src) as tar:
        if hasattr(tarfile, "data_filter"):
            tar.extractall(tmp, filter="data")
        else:
            root = os.path.realpath(tmp)
            for member in tar.getmembers():
                target = os.path.realpath(os.path.join(root, member.name))
                if os.path.commonpath([root, target]) != root or member.islnk() or member.issym():
                    sys.exit(f"!! {src}: refusing archive member {member.name}")
            tar.extractall(tmp)
    tops = list(tmp.iterdir())
    if len(tops) != 1 or not tops[0].is_dir():
        sys.exit(f"!! {src}: expected one top-level directory, found {[t.name for t in tops]}")
    shutil.rmtree(dest, ignore_errors=True)
    tops[0].rename(dest)
    tmp.rmdir()


def prepare(dest, extra, options):
    """Applies what the manifest and scripts/bundled-cpan/<dir> say about a
    freshly unpacked distribution."""
    for option in options:
        if option == "core-makefile":
            # make_ext.pl writes a plain Makefile.PL for an extension that
            # has none.
            (dest / "Makefile.PL").unlink()
        else:
            sys.exit(f"!! {dest.name}: unknown manifest option {option}")
    if extra.is_dir():
        shutil.copytree(extra, dest, dirs_exist_ok=True)


def main():
    manifest, perl = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    names = []
    for line in manifest.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        name, path, digest, *options = line.split()
        dest = perl / "cpan" / name.replace("/", "-")
        stamp = dest / STAMP
        if not (stamp.exists() and stamp.read_text().strip() == digest):
            unpack(archive(path, digest), dest)
            prepare(dest, manifest.parent / "bundled-cpan" / dest.name, options)
            stamp.write_text(digest + "\n")
            print(f"== unpacked {path} into {dest}", file=sys.stderr)
        names.append(name)
    print(" ".join(names))


if __name__ == "__main__":
    main()
