#!/usr/bin/env python3
"""Compares the imports of the linked perl.wasm with the ones it is meant
to have (scripts/wasm-imports.txt), and fails naming every import that is
not listed.

The linker leaves a symbol the program uses but nothing defines (a typo, a
function whose definition is not visible to it: static, or in an anonymous
namespace) as an import of the module "env" instead of failing, since the
host provides some functions by import. The transpiled program then binds
such an import to a function that does nothing and returns zero, and the
code that calls it goes on with that answer. This check makes the build
fail there instead: an import that is not in the list is either a mistake
or a new function of the host, which goes into the list in the commit that
starts using it. A listed import perl.wasm does not have is only reported:
the host shims of the wasmify that builds it bring some imports with them
(fd_dupfd and fd_lock came with its fcntl shim), and one it no longer uses
does no harm.

usage: wasm-imports.py <perl.wasm> [<expected list>]
       wasm-imports.py --list <perl.wasm>   (prints the imports as the list
                                             has them)
"""
import os
import sys

KINDS = {0: 'func', 1: 'table', 2: 'memory', 3: 'global', 4: 'tag'}


def leb(b, i):
    r = shift = 0
    while True:
        x = b[i]
        i += 1
        r |= (x & 0x7f) << shift
        shift += 7
        if not x & 0x80:
            return r, i


def name(b, i):
    n, i = leb(b, i)
    return b[i:i + n].decode('utf-8'), i + n


def limits(b, i):
    flags, i = leb(b, i)
    _, i = leb(b, i)
    if flags & 1:
        _, i = leb(b, i)
    return i


def imports(path):
    """The (module, name, kind) of every import of the module at path."""
    b = open(path, 'rb').read()
    if b[:4] != b'\0asm':
        raise SystemExit('%s: not a wasm module' % path)
    out = []
    i = 8
    while i < len(b):
        section = b[i]
        size, i = leb(b, i + 1)
        end = i + size
        if section == 2:
            count, j = leb(b, i)
            for _ in range(count):
                mod, j = name(b, j)
                field, j = name(b, j)
                kind = b[j]
                j += 1
                if kind == 0:  # a function: its type index
                    _, j = leb(b, j)
                elif kind == 1:  # a table: element type, limits
                    j = limits(b, j + 1)
                elif kind == 2:  # a memory: limits
                    j = limits(b, j)
                elif kind == 3:  # a global: value type, mutability
                    j += 2
                elif kind == 4:  # a tag: attribute, type index
                    _, j = leb(b, j + 1)
                else:
                    raise SystemExit('%s: import %s.%s of unknown kind %d' % (path, mod, field, kind))
                out.append((mod, field, KINDS[kind]))
            break
        i = end
    return out


def expected(path):
    """The (module, name, kind) lines of the list: three words a line, # starts
    a comment."""
    out = []
    for n, line in enumerate(open(path), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        words = line.split()
        if len(words) != 3 or words[2] not in KINDS.values():
            raise SystemExit('%s:%d: not "module name kind": %r' % (path, n, line))
        out.append(tuple(words))
    return out


def main(argv):
    if len(argv) == 3 and argv[1] == '--list':
        for mod, field, kind in sorted(imports(argv[2])):
            print(mod, field, kind)
        return 0
    if len(argv) not in (2, 3):
        print(__doc__, file=sys.stderr)
        return 2
    wasm = argv[1]
    listed = argv[2] if len(argv) == 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                         'wasm-imports.txt')
    have, want = set(imports(wasm)), set(expected(listed))
    extra, missing = sorted(have - want), sorted(want - have)
    for mod, field, kind in extra:
        print('%s: unexpected import %s.%s (%s): an undefined symbol, or a host function '
              'missing from %s' % (wasm, mod, field, kind, listed), file=sys.stderr)
    for mod, field, kind in missing:
        print('%s: note: import %s.%s (%s) listed in %s is not there' % (wasm, mod, field, kind, listed),
              file=sys.stderr)
    if extra:
        return 1
    print('%s: each of its %d imports is listed' % (wasm, len(have)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
