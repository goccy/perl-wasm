#!/usr/bin/env python3
"""Build the embeddable Perl standard-library zip from the assembled lib/ tree.

perl.wasm is a bare interpreter: the pure-Perl half of the standard library
(strict.pm, warnings.pm, Config.pm, the unicore/ Unicode tables, ...) lives on
disk, not in the wasm. This produces `perl_stdlib.zip`, a trimmed copy of the
assembled `perl5/lib/` tree that an embedding application (go-perl) unpacks at
runtime and passes to perl_new(lib_dir) as the @INC search path. It is published
as a release asset alongside perl.wasm and the wasm2go bundle and attested, so
consumers verify it the same way — and it is locked to the exact Perl version
perl.wasm was built from (the submodule is pinned at v5.44.0).

IMPORTANT: run this AGAINST THE ASSEMBLED lib/, i.e. AFTER `make` has run the
nonxs pm_to_blib step. A pristine checkout's lib/ holds only the ~73 core .pm;
the dual-life modules from cpan/ dist/ ext/ are copied in during the build, and
the generated Config.pm / Config_heavy.pl are written there too. Zipping a
pre-build lib/ silently ships an incomplete stdlib.

Trim (everything that is meaningless in the embedded, statically-linked build):
  - auto/**/*.a  — the XS static archives are already linked into perl.wasm;
    XSLoader::load short-circuits for static_ext, so the .a is never read.
  - auto/**/*.ld / auto/**/extralibs.ld — MakeMaker linker hints (build-only).
  - **/.exists — MakeMaker directory-stamp markers (build-only).
  - *.pod and any pod/ tree — documentation (perldoc does not run under wasi).
  - *.t and any t/ or test/ directory — module test suites.
  - *.bs — bootstrap stubs (unused for static ext).
The unicore/ Unicode tables (*.pl / *.txt) ARE kept — required for \w, \p{...},
case folding, and Encode.

The archive is written deterministically (sorted entries, fixed 1980 timestamps,
fixed 0644 perms) so the same lib/ always yields byte-identical bytes.

Usage: make-stdlib-zip.py <perl5/lib dir> <output zip>
"""

import os
import sys
import zipfile

# Directory names excluded at ANY depth (with everything beneath them).
ANYWHERE_DIR_EXCLUDES = {"t", "test", "tests", "pod"}

# Fixed DOS timestamp (1980-01-01 00:00:00) for reproducible archives.
FIXED_DATE_TIME = (1980, 1, 1, 0, 0, 0)


def included(rel_path: str) -> bool:
    parts = rel_path.split("/")
    base = parts[-1]
    if any(p in ANYWHERE_DIR_EXCLUDES for p in parts[:-1]):
        return False
    if base == ".exists":
        return False
    # Build log with an embedded generation timestamp; not needed at runtime
    # and the only nondeterministic byte source in the tree.
    if rel_path == "unicore/mktables.lst":
        return False
    for ext in (".a", ".ld", ".bs", ".pod", ".t"):
        if rel_path.endswith(ext):
            return False
    return True


def collect(lib_dir: str) -> list[str]:
    out = []
    for root, _dirs, files in os.walk(lib_dir):
        for name in files:
            rel = os.path.relpath(os.path.join(root, name), lib_dir)
            rel = rel.replace(os.sep, "/")
            if included(rel):
                out.append(rel)
    out.sort()
    return out


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: make-stdlib-zip.py <perl5/lib dir> <output zip>\n")
        return 2
    lib_dir, out_path = sys.argv[1], sys.argv[2]
    if not os.path.isdir(lib_dir):
        sys.stderr.write(f"not a directory: {lib_dir}\n")
        return 1

    sources = {rel: lib_dir for rel in collect(lib_dir)}
    rels = sorted(sources)
    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for rel in rels:
            info = zipfile.ZipInfo(filename=rel, date_time=FIXED_DATE_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16  # regular file, rw-r--r--
            with open(os.path.join(sources[rel], rel), "rb") as fh:
                zf.writestr(info, fh.read())
    sys.stderr.write(f"wrote {out_path}: {len(rels)} files\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
