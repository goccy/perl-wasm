#!/bin/bash
# build-xsdists.sh — compile the vendored CPAN XS distributions under xsdist/
# into static archives linked into perl.wasm (the same static-ext mechanism
# the core extensions use).
#
# Runs as the tail of wasmify.json's build command, i.e. INSIDE the wasmify
# capture: the compiler is invoked by BARE name ('clang', 'ar') so the PATH
# wrapper records every step and wasm-build replays it with the wasm flags.
# wasm-build links every captured archive, and wasm-ld only extracts members
# something references — so declaring the dist's boot_* in perl.cc's xs_init
# is what actually pulls the code in. No link-set surgery needed.
#
# Per-dist layout: xsdist/<Name>/src/*.xs (+ headers). xsubpp runs under the
# HOST miniperl with the in-tree ExtUtils::ParseXS, so the generated glue
# matches the pinned perl exactly. The boot_* symbols each dist exports must
# be registered in perl.cc's xs_init as "<Module>::bootstrap" so the stock
# XSLoader/DynaLoader static fallback finds them at `use` time.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
PERL="$HERE/perl5"

# The host miniperl (built by wasi-configure.sh) drives xsubpp.
host_out="$(bash "$HERE/scripts/host-miniperl.sh")"
eval "$(printf '%s\n' "$host_out" | grep -E '^HOSTPERL=')"
[ -x "$HOSTPERL" ] || { echo "!! host miniperl missing" >&2; exit 1; }

XSUBPP="$PERL/dist/ExtUtils-ParseXS/lib/ExtUtils/xsubpp"
TYPEMAP="$PERL/lib/ExtUtils/typemap"

# Compile flags: the extension half of config.sh (ccflags WITHOUT PERL_CORE),
# mirroring what MakeMaker would feed a static extension build.
cfg() { sed -n "s/^$1='\(.*\)'$/\1/p" "$PERL/config.sh"; }
CCFLAGS="$(cfg ccflags)"
OPTIMIZE="$(cfg optimize)"

# Generated build inputs (ppport.h, xshelper.h, and any tool-derived
# headers) are vendored alongside each dist's sources: they are
# deterministic artifacts, and vendoring keeps the pipeline free of
# host-perl module dependencies.

build_dist() {
  local name="$1"
  local dist="$HERE/xsdist/$name"
  local out="$dist/build"
  mkdir -p "$out"
  local objs=()
  for xs in "$dist"/src/*.xs; do
    local base c o
    base="$(basename "$xs" .xs)"
    c="$out/$base.c"
    o="$out/$base.o"
    echo "== xsdist $name: xsubpp $base.xs"
    "$HOSTPERL" -I"$PERL/lib" -I"$PERL/dist/ExtUtils-ParseXS/lib" "$XSUBPP" \
        -noprototypes -typemap "$TYPEMAP" "$xs" > "$c"
    echo "== xsdist $name: cc $base.c"
    # Bare 'clang' so the wasmify wrapper captures the step. -I src for the
    # dist's own headers, CORE headers from the tree root.
    # Version macros come from the dist's vendored src/xs_version.h.
    clang $CCFLAGS $OPTIMIZE \
        -I"$PERL" -I"$dist/src" \
        -c "$c" -o "$o"
    objs+=("$o")
  done
  local ar_out="$out/lib${name}.a"
  echo "== xsdist $name: ar $(basename "$ar_out")"
  rm -f "$ar_out"
  ar rc "$ar_out" "${objs[@]}"
  ranlib "$ar_out"
}

build_dist "Text-Xslate"

echo "== xsdist: done"
