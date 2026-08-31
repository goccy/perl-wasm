#!/bin/bash
# host-miniperl.sh — build a HOST miniperl + generate_uudmap (idempotent).
#
# Perl's build RUNS miniperl during the build to generate sources (Config.pm,
# the Unicode tables, perlmain.c via ExtUtils::Miniperl::writemain, the ext
# build driven by MakeMaker, ...). When we cross-compile the interpreter to
# wasm32-wasi that miniperl is a wasm binary and cannot run on the build host,
# so Perl's cross-compile support expects a HOST-native miniperl to be supplied
# via config.sh's `hostperl` (and generate_uudmap via `hostgenerate`); the
# generated Makefile then symlinks HOST_PERL -> ./miniperl and uses it for all
# build-time codegen (Makefile.SH: `$(LNS) $(HOST_PERL) $(MINIPERL_EXE)`).
# This is the Perl analogue of python-wasm building a native build-python first.
#
# The host tools MUST be the SAME Perl version as the target (5.42.2); the macOS
# system perl (5.34) will not do. We build them from the pinned perl5 submodule
# with the HOST compiler, in a throwaway git worktree so the target perl5 tree
# is never polluted with host-arch objects, then copy just the two binaries into
# build/host-perl/.
#
# Idempotent: if build/host-perl/{miniperl,generate_uudmap,native-config.sh}
# already exist and miniperl reports the pinned version, it does nothing. Safe
# to re-run.
#
# Also stashes the native build's config.sh as build/host-perl/native-config.sh.
# The full native Configure is the only thing that walks MANIFEST + every
# ext/dist/cpan module and computes the COMPLETE nonxs_ext (pure-Perl dual-life
# modules) and dynamic_ext (XS) lists — data the wasm cross-config needs to
# populate lib/ correctly but cannot compute itself (Configure can't probe the
# unrunnable wasm target). wasi-configure.sh reads nonxs_ext from here.
#
# Prints (on the last lines) the absolute paths, so callers can capture them:
#   HOSTPERL=<...>/build/host-perl/miniperl
#   HOSTGENERATE=<...>/build/host-perl/generate_uudmap
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$HERE/perl5"
OUT="$HERE/build/host-perl"
WT="$HERE/build/host-build"
HOSTPERL="$OUT/miniperl"
HOSTGEN="$OUT/generate_uudmap"
NATIVECFG="$OUT/native-config.sh"
WANT_VER="5.044000"   # $] for Perl 5.44.0

[ -f "$SRC/Configure" ] || { echo "!! perl5 not checked out: git submodule update --init perl5" >&2; exit 1; }

emit_paths() { echo "HOSTPERL=$HOSTPERL"; echo "HOSTGENERATE=$HOSTGEN"; }

# --- Idempotent short-circuit ------------------------------------------------
if [ -x "$HOSTPERL" ] && [ -x "$HOSTGEN" ] && [ -f "$NATIVECFG" ]; then
  got="$("$HOSTPERL" -e 'print $]' 2>/dev/null || true)"
  if [ "$got" = "$WANT_VER" ]; then
    echo "== host miniperl already present ($HOSTPERL, \$]=$got)"
    emit_paths
    exit 0
  fi
  echo "== rebuilding host miniperl (found \$]=$got, want $WANT_VER)"
fi

# --- Fresh throwaway build tree ---------------------------------------------
# Prefer a git worktree (cheap, shares objects); fall back to a plain copy if
# the submodule layout refuses a worktree.
rm -rf "$WT"
mkdir -p "$HERE/build"
if ! git -C "$SRC" worktree add --force --detach "$WT" HEAD >/dev/null 2>&1; then
  echo "== worktree unavailable; copying source tree"
  git -C "$SRC" worktree prune >/dev/null 2>&1 || true
  cp -a "$SRC" "$WT"
  rm -rf "$WT/.git"
fi

# --- Native configure + build of just miniperl and generate_uudmap ----------
# Plain host toolchain (NO wasi-sdk here): this perl runs on the build machine.
# -des = accept all defaults non-interactively; -Dusedevel avoids the
# development-version prompt on a git checkout. On macOS the darwin hints set
# firstmakefile=GNUmakefile automatically, so the case-insensitive-FS Makefile
# clash does not bite the host build.
cd "$WT"
echo "== configuring host perl (native) in $WT"
sh ./Configure -des -Dusedevel -Dprefix="$WT/inst" >/tmp/host-perl-configure.log 2>&1 || {
  echo "!! host Configure failed; tail of /tmp/host-perl-configure.log:" >&2
  tail -30 /tmp/host-perl-configure.log >&2
  exit 1
}
echo "== building miniperl + generate_uudmap"
make miniperl generate_uudmap >/tmp/host-perl-make.log 2>&1 || {
  echo "!! host make failed; tail of /tmp/host-perl-make.log:" >&2
  tail -40 /tmp/host-perl-make.log >&2
  exit 1
}

# --- Stash the two standalone binaries; drop the build tree -----------------
mkdir -p "$OUT"
cp "$WT/miniperl" "$HOSTPERL"
cp "$WT/generate_uudmap" "$HOSTGEN"
# Stash the native config.sh before the worktree is dropped: it carries the
# complete nonxs_ext/dynamic_ext lists the wasm cross-config reuses.
cp "$WT/config.sh" "$NATIVECFG"
git -C "$SRC" worktree remove --force "$WT" >/dev/null 2>&1 || rm -rf "$WT"

got="$("$HOSTPERL" -e 'print $]' 2>/dev/null || true)"
[ "$got" = "$WANT_VER" ] || { echo "!! built host miniperl reports \$]=$got, expected $WANT_VER" >&2; exit 1; }
echo "== host miniperl ready ($HOSTPERL, \$]=$got)"
emit_paths
