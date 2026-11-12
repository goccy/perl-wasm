#!/usr/bin/env python3
"""Keep the external symbols of the linked-in extensions apart.

Every XS extension of this build is linked statically into one perl.wasm, so
all of them share one C namespace. An extension written to be loaded as a
shared object does not expect that: its non-static helper functions were
private to its own library there. Two extensions with a helper of the same
name (PadWalker and Want both have `upcontext`) then collide — the linker
keeps one definition, and where the signatures differ it replaces the call
with a trap.

For the CPAN distributions of scripts/bundled-cpan.txt the build therefore
renames every external symbol except the boot function to
<Extension>__<symbol> (Want__upcontext). The renaming is a header of
#defines, perlwasm_ext_names.h, in each directory the distribution compiles
in; ccflags force-includes that file name, and a directory without one gets
the empty default from scripts/include. The headers are kept in
scripts/bundled-cpan/<distribution>/, from where fetch-bundled-cpan.py copies
them over the unpacked distribution.

Usage: ext-symbols.py verify <built perl5 dir>
       ext-symbols.py linked <directory of archives>
       ext-symbols.py write  <built perl5 dir>

verify, a step of the perl5 build, fails when
  * two archives (libperl.a or an extension's) define the same external
    symbol, or
  * a bundled distribution has an external symbol without its prefix
    (the headers are out of date: run `write` and build again).

linked, run on the archives the perl.wasm link consumed, fails when two of
them define the same external symbol: the link itself accepts that.

write regenerates the headers under scripts/bundled-cpan from the objects of
a built tree.
"""
import os
import pathlib
import shutil
import subprocess
import sys

HEADER = "perlwasm_ext_names.h"
HERE = pathlib.Path(__file__).resolve().parent


def nm_tool():
    sdk = os.environ.get("WASI_SDK_PATH") or os.path.join(
        os.path.expanduser("~"), ".config", "wasmify", "bin", "wasi-sdk")
    tool = os.path.join(sdk, "bin", "llvm-nm")
    if os.path.exists(tool):
        return tool
    found = shutil.which("llvm-nm")
    if not found:
        sys.exit("!! llvm-nm not found (set WASI_SDK_PATH)")
    return found


def defined(nm, path):
    """The external symbols the object or archive defines."""
    out = subprocess.run([nm, "--defined-only", "--extern-only", "--format=posix", str(path)],
                         capture_output=True, text=True, check=True).stdout
    names = set()
    for line in out.splitlines():
        fields = line.split()
        # "<name> <type> ..."; the member headers of an archive have one field
        if len(fields) >= 2 and len(fields[1]) == 1:
            names.add(fields[0])
    return names


def bundled():
    """(extension name, distribution directory name) per manifest line."""
    out = []
    for line in (HERE / "bundled-cpan.txt").read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            name = line.split()[0]
            out.append((name, name.replace("/", "-")))
    return out


def prefix(name):
    return name.replace("/", "__") + "__"


def object_dirs(dist):
    """The directories of a built distribution that hold objects."""
    return sorted({p.parent for p in dist.rglob("*.o")})


def duplicates(nm, root, paths):
    """Reports the external symbols more than one of the archives defines;
    returns (number of definitions, whether any was a duplicate)."""
    failed = False
    owner = {}
    for archive in paths:
        for sym in sorted(defined(nm, archive)):
            if sym in owner:
                print(f"!! {sym} is defined by both {owner[sym].relative_to(root)} and "
                      f"{archive.relative_to(root)}", file=sys.stderr)
                failed = True
            else:
                owner[sym] = archive
    return len(owner), failed


def linked(root):
    count, failed = duplicates(nm_tool(), root, sorted(root.rglob("*.a")))
    if failed:
        sys.exit(1)
    print(f"== linked archives: {count} external definitions, no duplicates")


def verify(perl):
    nm = nm_tool()
    archives = [perl / "libperl.a"] + sorted((perl / "lib" / "auto").rglob("*.a"))
    count, failed = duplicates(nm, perl, archives)
    for name, dirname in bundled():
        pre = prefix(name)
        for directory in object_dirs(perl / "cpan" / dirname):
            for obj in sorted(directory.glob("*.o")):
                for sym in sorted(defined(nm, obj)):
                    if sym.startswith("boot_") or sym.startswith(pre):
                        continue
                    print(f"!! {obj.relative_to(perl)}: external symbol {sym} is not named "
                          f"{pre}{sym}; run scripts/ext-symbols.py write", file=sys.stderr)
                    failed = True
    if failed:
        sys.exit(1)
    print(f"== extension symbols: {count} external definitions, no duplicates")


def write(perl):
    nm = nm_tool()
    for name, dirname in bundled():
        pre = prefix(name)
        dist = perl / "cpan" / dirname
        for directory in object_dirs(dist):
            names = set()
            for obj in directory.glob("*.o"):
                for sym in defined(nm, obj):
                    if sym.startswith("boot_"):
                        continue
                    names.add(sym[len(pre):] if sym.startswith(pre) else sym)
            target = HERE / "bundled-cpan" / dirname / directory.relative_to(dist) / HEADER
            if not names:
                if target.exists():
                    target.unlink()
                continue
            lines = [
                f"/* {HEADER} -- generated by scripts/ext-symbols.py write; do not edit.",
                f" * The external symbols of {name.replace('/', '::')} other than its boot function,",
                " * renamed so that they cannot collide with another linked-in extension's. */",
            ]
            lines += [f"#define {sym} {pre}{sym}" for sym in sorted(names)]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("\n".join(lines) + "\n")
            print(f"== wrote {target.relative_to(HERE.parent)} ({len(names)} names)")


def main():
    commands = {"verify": verify, "linked": linked, "write": write}
    if len(sys.argv) != 3 or sys.argv[1] not in commands:
        sys.exit("usage: ext-symbols.py verify|linked|write <directory>")
    commands[sys.argv[1]](pathlib.Path(sys.argv[2]).resolve())


if __name__ == "__main__":
    main()
