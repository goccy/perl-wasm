#!/bin/bash
# xs-wasm-build.sh — compile one XS module into a SHARED wasm side module
# (PIC + dylink.0) that links against a running perl.wasm instance at load
# time, sharing its linear memory.
#
# Usage:
#   scripts/xs-wasm-build.sh Module::Name path/to/Module.xs [extra.c ...] [-o out.wasm]
#
# Output shape (verify with wasm-objdump -x):
#   - custom section "dylink.0"      memory/table reservation the loader honours
#   - import env.memory              THE MAIN MODULE'S LINEAR MEMORY (shared)
#   - import env.__indirect_function_table, env.__memory_base, env.__table_base,
#     env.__stack_pointer            relocation bases + shared C stack pointer
#   - import env.Perl_*              the perl API the module actually uses,
#                                    resolved against the main module's exports
#   - import GOT.mem.PL_*            addresses of interpreter data symbols
#   - export boot_Module__Name       the XS bootstrap (DynaLoader entry point)
#
# The loader side (a Go runtime linker in perlwasm2go/go-perl) is what turns
# this artifact into a loadable module: it must allocate dylink.mem_size bytes
# in the shared heap for __memory_base, reserve dylink.table_size function
# table slots for __table_base, bind the env.Perl_*/GOT.mem.PL_* imports from
# the main module's (to-be-added) dynamic symbol table, run
# __wasm_apply_data_relocs when present, and hand boot_* to DynaLoader. Until
# that lands this script is the build half of the experiment; keep its output
# byte-shape pinned by docs/xs-modules.md discussion.
#
# Prerequisites: a CONFIGURED AND BUILT tree (perl5/config.h and the
# .wasmify/wasm-build include overlays exist — run `make wasm-host` first).
#
# NOTE: xsubpp is taken from the HOST perl for now. Its output is
# version-tolerant glue, but the Phase-1 static-XS pipeline should switch to
# the in-tree ExtUtils::ParseXS under the host miniperl so the XS macros match
# the pinned perl exactly.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
: "${WASI_SDK_PATH:=$HOME/.config/wasmify/bin/wasi-sdk}"
CLANG="$WASI_SDK_PATH/bin/clang"
SYSROOT="$WASI_SDK_PATH/share/wasi-sysroot"

[ $# -ge 2 ] || { echo "usage: $0 Module::Name file.xs [extra.c ...] [-o out.wasm]" >&2; exit 2; }
MODULE="$1"; shift
XS="$1"; shift
OUT=""
EXTRA_SRCS=()
while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUT="$2"; shift 2 ;;
    *)  EXTRA_SRCS+=("$1"); shift ;;
  esac
done
BOOT="boot_$(echo "$MODULE" | sed 's/::/__/g')"
BASE="$(basename "$XS" .xs)"
[ -n "$OUT" ] || OUT="$(dirname "$XS")/$BASE.wasm"

[ -f "$HERE/perl5/config.h" ] || { echo "!! perl5/config.h missing — run 'make wasm-host' first" >&2; exit 1; }
[ -d "$HERE/.wasmify/wasm-build/host-include" ] || { echo "!! .wasmify include overlays missing — run 'make wasm-host' first" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "== xsubpp: $XS -> $WORK/$BASE.c"
xsubpp -noprototypes -typemap "$HERE/perl5/lib/ExtUtils/typemap" "$XS" > "$WORK/$BASE.c"

# Same flags as the interpreter build (wasi emulation defines, posix shim,
# strict-aliasing off, SjLj lowering) plus -fPIC for relocatable code.
CFLAGS=(
  --target=wasm32-wasip1 --sysroot="$SYSROOT"
  -fPIC -O2 -fno-strict-aliasing
  -mllvm -wasm-enable-sjlj
  -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_SIGNAL
  -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_GETPID
  -include "$HERE/scripts/wasi-posix-shim.h"
  -isystem "$HERE/.wasmify/wasm-build/host-include"
  -isystem "$HERE/.wasmify/wasm-build/posix-compat/include"
  -I"$HERE/perl5"
)

OBJS=()
echo "== cc: $BASE.c"
"$CLANG" "${CFLAGS[@]}" -c "$WORK/$BASE.c" -o "$WORK/$BASE.o"
OBJS+=("$WORK/$BASE.o")
for src in ${EXTRA_SRCS[@]+"${EXTRA_SRCS[@]}"}; do
  obj="$WORK/$(basename "${src%.*}").o"
  echo "== cc: $src"
  "$CLANG" "${CFLAGS[@]}" -c "$src" -o "$obj"
  OBJS+=("$obj")
done

# -shared + import-dynamic: every unresolved Perl_*/PL_* symbol becomes an
# env./GOT. import for the loader to bind; --export=$BOOT keeps the bootstrap
# (and transitively every XSUB it registers) out of linker GC.
echo "== ld: $OUT"
"$CLANG" --target=wasm32-wasip1 -fPIC -shared -nostdlib \
  -Wl,--no-entry -Wl,--unresolved-symbols=import-dynamic \
  -Wl,--export="$BOOT" \
  "${OBJS[@]}" -o "$OUT" 2> >(grep -v "not yet stable" >&2 || true)

echo "== done: $OUT ($BOOT)"
