#!/bin/bash
# wasi-configure.sh — configure Perl for wasm32-wasi (idempotent).
#
# This is the FIRST step of the wasmify pipeline for perl-wasm. wasmify owns the
# actual wasm compilation (compiler / flag transforms); this script owns the one
# thing wasmify does NOT: producing a Perl `config.h` + `Makefile` whose answers
# are correct for the wasm32-wasi TARGET rather than the build host. It is the
# Perl analogue of python-wasm's scripts/wasi-configure.sh.
#
# Approach (Route B — a baked target config.sh):
#   Perl's Configure normally probes the target by compiling AND RUNNING little
#   test programs. Under wasm32-wasi we cannot run them (no runner), so instead
#   we hand Configure a ready-made config.sh and let it do variable substitution
#   only (`Configure -S`), which emits config.h and Makefile without probing.
#   The base is perl5/Cross/config.sh-arm-linux: it is the SAME Perl (5.42.2)
#   and is 32-bit little-endian (longsize=ptrsize=4, byteorder=1234) — i.e. the
#   integer/pointer model already matches wasm32. We overwrite only what differs
#   for wasi: the toolchain (-> wasi-sdk), a few host capabilities Perl must not
#   assume (fork/dynamic-loading/threads), and the extension set (our static-ext
#   policy; see docs/xs-modules.md).
#
# Idempotent: every run regenerates config.sh from the pinned base + overrides
# and re-runs `Configure -S`, so repeated runs converge to the same output and
# it is safe to re-run after editing the override block below.
#
# Env in:
#   WASI_SDK_PATH   path to wasi-sdk (default: the dir wasmify installs into).
# Runs from anywhere; locates perl5/ relative to this script.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
PERL="$HERE/perl5"
: "${WASI_SDK_PATH:=$HOME/.config/wasmify/bin/wasi-sdk}"
SYSROOT="$WASI_SDK_PATH/share/wasi-sysroot"
# wasi-sdk emulation opt-in defines (paired with -lwasi-emulated-* at link).
WASI_EMU='-D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_GETPID'
# Force-include prototypes for POSIX functions wasi-libc guards out but Perl's
# core references (dup/exec/kill/... — see the header). Absolute path, baked
# into config.sh (regenerated per host, so CI-portable).
POSIX_SHIM="-include $HERE/scripts/wasi-posix-shim.h"
CLANG="$WASI_SDK_PATH/bin/clang"

[ -d "$PERL/.git" ] || [ -f "$PERL/Configure" ] || {
  echo "!! perl5 submodule not checked out. Run: git submodule update --init perl5" >&2
  exit 1
}
[ -x "$CLANG" ] || {
  echo "!! wasi-sdk clang not found at: $CLANG" >&2
  echo "   Install it (wasmify install-sdk) or set WASI_SDK_PATH." >&2
  exit 1
}

echo "== perl5:    $PERL ($(git -C "$PERL" describe --tags 2>/dev/null || echo unknown))"
echo "== wasi-sdk: $WASI_SDK_PATH"
echo "== clang:    $("$CLANG" --version | head -1)"

# --- Host miniperl (build-time codegen) -------------------------------------
# Perl runs miniperl DURING the build to generate sources; the wasm miniperl
# can't run on the host, so cross-compile uses a HOST-native miniperl +
# generate_uudmap. Ensure they exist (idempotent) and capture their paths.
echo "== ensuring host miniperl (scripts/host-miniperl.sh)"
host_out="$(bash "$HERE/scripts/host-miniperl.sh")" || { printf '%s\n' "$host_out" >&2; exit 1; }
eval "$(printf '%s\n' "$host_out" | grep -E '^(HOSTPERL|HOSTGENERATE)=')"
[ -x "$HOSTPERL" ] && [ -x "$HOSTGENERATE" ] || { echo "!! host miniperl/generate_uudmap missing after bootstrap" >&2; exit 1; }
echo "== hostperl:     $HOSTPERL"
echo "== hostgenerate: $HOSTGENERATE"

BASE="$PERL/Cross/config.sh-arm-linux"
[ -f "$BASE" ] || { echo "!! base config not found: $BASE" >&2; exit 1; }

# --- XS extension policy (docs/xs-modules.md), in config.sh directory form ----
# usedl=undef makes every XS extension static, so `extensions` == what we link
# into perl.wasm. Everything meaningful under wasi is IN; OS-specific /
# external-lib / SysV-IPC / mmap / syslog / threads / test-only modules are OUT.
# NOTE on PerlIO::scalar: it is deliberately NOT in this XS list. In 5.42 it has
# no .xs — the in-memory scalar-filehandle layer lives in the core perlio.c —
# so it is a PURE-PERL module (just scalar.pm) carried by nonxs_ext, and the
# native nonxs list already includes it. perl.cc can still capture STDOUT via
# `open $fh,'>',\$buf`, which the built-in scalar layer handles without the .pm.
STATIC_EXT="attributes B Compress/Raw/Bzip2 Compress/Raw/Zlib Cwd Data/Dumper \
Devel/Peek Digest/MD5 Digest/SHA Encode Fcntl File/DosGlob File/Glob \
Filter/Util/Call Hash/Util Hash/Util/FieldHash I18N/Langinfo IO List/Util \
Math/BigInt/FastCalc MIME/Base64 Opcode POSIX PerlIO/encoding \
PerlIO/via SDBM_File Socket Storable Sys/Hostname Time/Piece \
Unicode/Collate Unicode/Normalize mro re"
# Time/HiRes is EXCLUDED: wasi's clockid_t is `const struct __clockid *` (a
# struct pointer, with CLOCK_REALTIME = &_CLOCK_REALTIME), but Time::HiRes moves
# clock ids through Perl as integers (IV), so clock_gettime/clock_getres are
# fundamentally type-incompatible (-Wint-conversion), and setitimer/getitimer /
# interval timers do not exist under wasi at all. Core time()/sleep and POSIX's
# clock interfaces cover the common cases. See docs/xs-modules.md.
# Collapse the line continuations / extra whitespace into single spaces.
STATIC_EXT="$(echo $STATIC_EXT)"

# --- Pure-Perl (nonxs) extensions: take the COMPLETE native list --------------
# nonxs_ext is every pure-Perl dual-life module that must land in lib/ (base,
# Carp, Exporter, constant, ExtUtils::MakeMaker + deps, lib, XSLoader, ...).
# Building the static XS extensions runs each one's Makefile.PL under the host
# miniperl, which `use`s these — so they must be assembled into lib/ FIRST, via
# Perl's own nonxs_ext / pm_to_blib machinery (it also generates the *_pm.PL
# modules like lib.pm/XSLoader.pm and resolves the non-obvious dir->module name
# mapping, e.g. PathTools->Cwd). We cannot compute this list here (Configure
# can't probe the unrunnable wasm target, and dir names are not the module
# names), so reuse the COMPLETE nonxs_ext the native host Configure already
# computed from MANIFEST (saved by host-miniperl.sh as native-config.sh). Drop
# any entry that we build as a static XS extension instead (only PerlIO/scalar
# overlaps: native lists its .pm under nonxs, but we link its XS statically).
NATIVE_CFG="$HERE/build/host-perl/native-config.sh"
[ -f "$NATIVE_CFG" ] || { echo "!! native-config.sh missing: $NATIVE_CFG" >&2; exit 1; }
NATIVE_NONXS="$(grep -E "^nonxs_ext=" "$NATIVE_CFG" | sed "s/nonxs_ext=//; s/'//g")"
NONXS_EXT=""
for m in $NATIVE_NONXS; do
  case " $STATIC_EXT " in
    *" $m "*) continue ;;   # built as a static XS extension — skip here
  esac
  NONXS_EXT="$NONXS_EXT $m"
done
NONXS_EXT="$(echo $NONXS_EXT)"
EXTENSIONS="$STATIC_EXT $NONXS_EXT"

# --- Generate the wasm32-wasi config.sh -------------------------------------
# Start from the pinned 32-bit base, then append overrides. config.sh is sourced
# top-to-bottom by Configure, so a later assignment wins over the base value.
CONFIG="$PERL/config.sh"
cp "$BASE" "$CONFIG"

cat >> "$CONFIG" <<EOF

# ==== wasm32-wasi overrides (generated by scripts/wasi-configure.sh) =========
# Toolchain: BARE tool names, not absolute paths. wasmify captures the build by
# interposing PATH wrappers named 'clang'/'ar'/... ; an absolute path would
# bypass the wrapper and nothing would be logged (build.json empty). So we use
# bare names and put \$WASI_SDK_PATH/bin on PATH (below / at build time) so the
# wrapper resolves the real tool to the wasi-sdk one. Same approach as
# python-wasm's wasi-configure. The --sysroot in ccflags pins the wasi sysroot.
cc='clang'
# cpp/cpprun/cppstdin (the C preprocessor vars) are used ONLY for host-side
# helper steps — dependency scanning (makedepend) and header probing such as
# Errno_pm.PL scanning <errno.h> for E* defs. They never produce object files,
# so they do NOT need to go through wasmify's capture wrapper. Pin them to the
# ABSOLUTE wasi-sdk clang ($CLANG, resolved per-machine at configure time so it
# stays host-OS-independent) rather than a bare name. Rationale:
#   * A bare 'cc' resolves (via the wrapper's WASMIFY_REAL_cc / PATH) to the
#     HOST macOS cc — wasi-sdk ships no 'cc' — dragging in macOS SDK headers
#     (_symbol_aliasing.h / _posix_availability.h), so Errno finds no wasi E*
#     defs and dies.
#   * Even a bare 'clang' is fragile here: these commands run in SUBSHELLS
#     spawned by the HOST miniperl, and must not depend on the wrapper PATH
#     being in effect. The absolute wasi clang always preprocesses the wasi
#     sysroot's <errno.h> (78 E* defs) correctly, including -mllvm
#     -wasm-enable-sjlj, which the host's Apple clang rejects outright.
cpp='$CLANG -E'
cpprun='$CLANG -E'
cppstdin='$CLANG -E'
cpplast='-'
ld='clang'
ar='ar'
full_ar='ar'
nm='nm'
ranlib='ranlib'
obj_ext='.o'
lib_ext='.a'
# Static interpreter library. The arm base names it 'libperl.so' even though a
# usedl=undef build archives it with \`ar rc\`; rename to 'libperl.a' so the
# artifact matches wasmify.json's build_target and the .a convention.
libperl='libperl.a'
# -mllvm -wasm-enable-sjlj: Perl's die/croak/eval are built on setjmp/longjmp,
# and perl.h always includes <setjmp.h>, which #errors under wasm unless this is
# set. It lowers setjmp/longjmp onto the wasm exception-handling proposal (and
# defines __wasm_exception_handling__ so the header compiles) — supported by
# wasm2go's EH backend. Needed at both compile and preprocess (dependency scan).
# The link step later adds -lsetjmp for the __wasm_setjmp/_test/__wasm_longjmp
# runtime.
#
# CRUCIAL: LLVM's -wasm-enable-sjlj lowers ONLY the plain C setjmp/longjmp; it
# does NOT touch sigsetjmp/siglongjmp. Perl's JMPENV (the die/eval unwind core)
# uses Sigsetjmp, which config.h maps to sigsetjmp() when d_sigsetjmp is defined
# and to plain setjmp() otherwise. The arm base has d_sigsetjmp=define, so
# libperl.a ended up with UNRESOLVED sigsetjmp/siglongjmp refs that -lsetjmp
# cannot satisfy (wasi-libc has no sigsetjmp). Force d_sigsetjmp=undef so Perl
# emits plain setjmp/longjmp, which the SjLj pass lowers to __wasm_setjmp/
# __wasm_longjmp. (Signal-mask save/restore across the jump is a no-op under the
# single-threaded, %SIG-emulated wasi build anyway.)
d_sigsetjmp='undef'
# wasi-sdk emulation opt-ins for POSIX features Perl needs but wasi lacks
# natively: process clocks (times), signals (%SIG), mman (mmap), getpid ($$).
# Each pairs a -D_WASI_EMULATED_* compile define with a -lwasi-emulated-* lib
# at link time (see libs). The headers #error without the define. $WASI_EMU and
# $SYSROOT expand here (heredoc), baking literal flags into config.sh.
#
# Do NOT put -DPERL_CORE in ccflags/cppflags. The core .c files need PERL_CORE,
# but the generated \`cflags\` script adds it itself, per file, as
# \`\$cc -c -DPERL_CORE \$ccflags ...\` — so ccflags must stay PERL_CORE-free (the
# arm base is). ccflags is ALSO what MakeMaker feeds to every XS extension via
# %Config; a stray -DPERL_CORE there leaks into e.g. B.xs and hides the
# back-compat symbols (Nullsv, SVt_RV, perl_call_method, PL_amagic_generation)
# that PERL_CORE #ifdefs out, so the extension fails to compile. DynaLoader only
# escaped because it uses none of those.
# -fno-strict-aliasing is MANDATORY for Perl and must not be dropped. Perl's
# core pervasively type-puns its SV/AV/HV/CV/GV pointer types (distinct C struct
# types treated as one "SV-like" family, e.g. \`svp = (SV**)&av; *svp\` in
# Perl_sv_kill_backrefs's single-backref path). With strict aliasing on (clang's
# default at -O2+), TBAA marks the write through the AV* lvalue and the read back
# through the SV** lvalue as non-aliasing, so DSE deletes the "dead" store that
# initialises the slot — a wrong-code miscompile (observed as
# \`panic: del_backref, svp=0\` when a stash with a single CV backref is torn
# down). Upstream Perl's Configure always adds this for gcc/clang (the arm base
# config.sh we derive from carries it); the wasi override dropped it. Restore it.
# Applies to core .c AND every XS extension (MakeMaker reads ccflags via %Config).
ccflags='--sysroot=$SYSROOT -mllvm -wasm-enable-sjlj $WASI_EMU $POSIX_SHIM -fno-strict-aliasing'
cppflags='--sysroot=$SYSROOT -mllvm -wasm-enable-sjlj $WASI_EMU $POSIX_SHIM -fno-strict-aliasing'
optimize='-O2'
cccdlflags=' '
ccdlflags=' '
ldflags='--sysroot=$SYSROOT'
lddlflags=' '
libs='-lm -lsetjmp -lwasi-emulated-process-clocks -lwasi-emulated-signal -lwasi-emulated-mman -lwasi-emulated-getpid'
libc='$SYSROOT/lib/wasm32-wasi/libc.a'
perllibs='-lm'
libpth='$SYSROOT/lib/wasm32-wasi'

# Build-host-portable fixups. These drive the BUILD machine (not the wasm
# target), so they must be correct on EVERY host that may run configure — the
# Linux CI runner and a macOS dev box alike. No host-OS conditionals: each value
# is chosen to be right everywhere.
#   issymlink: the arm base hardcodes absolute '/usr/bin/test -h' (exists on
#   Linux, absent on macOS). Bare 'test' is a POSIX shell builtin on every host.
issymlink='test -h'
#   firstmakefile: Makefile.SH writes the 'Makefile' template then runs
#   \`rm -f \$firstmakefile\`. The arm default 'makefile' (lowercase) deletes the
#   just-written 'Makefile' on a case-INSENSITIVE FS (macOS). 'GNUmakefile' never
#   collides case-insensitively AND is honored by GNU make on Linux, so it is
#   safe on every host (same choice as Perl's darwin hints).
firstmakefile='GNUmakefile'

# Target identity.
archname='wasm32-wasi'
myarchname='wasm32-wasi'
osname='wasi'
osvers='1.0'

# Cross-compile: run build-time codegen with the host-native miniperl /
# generate_uudmap instead of the (unrunnable) wasm ones. The generated Makefile
# symlinks these to ./miniperl and ./generate_uudmap
# (Makefile.SH: \$(LNS) \$(HOST_PERL) \$(MINIPERL_EXE)).
usecrosscompile='define'
hostperl='$HOSTPERL'
hostgenerate='$HOSTGENERATE'

# Static, single-threaded, no dynamic loading (wasi has no dlopen).
usedl='undef'
# dlsrc names the DynaLoader backend .xs. With usedl=undef the interpreter has
# no runtime loader, so it must be the no-op stub 'dl_none.xs' (Perl's own
# Configure picks this when usedl=undef). DynaLoader/Makefile.PL sets its DLSRC
# make-var from the config dlsrc and MakeMaker emits a rule making DynaLoader.xs
# depend on DLSRC — a wrong value (e.g. the malformed 'dlsrc.none') yields a
# "No rule to make target" failure.
dlsrc='dl_none.xs'
d_dlopen='undef'
useithreads='undef'
usemultiplicity='undef'
usethreads='undef'
useshrplib='false'
# No threads => do not pull in <pthread.h> (arm base defines it).
i_pthread='undef'
d_pthread_yield='undef'
d_pthreads_created_joinable='undef'

# Capabilities. wasi-libc lacks these natively, but wasmify's POSIX-compat /
# host-subprocess stub headers (injected at BOTH the capture build and wasm-build
# when wasmify.json opts into HostSockets/HostSubprocess) DECLARE them and the
# host-import shims IMPLEMENT them at link. So sockets, netdb, exec, fork, pipe,
# wait/waitpid, sigaction, dup/dup2, kill, alarm, getgroups, mkstemp are left
# ENABLED (arm base values) and resolved through wasmify — NOT disabled here.
# scripts/wasi-posix-shim.h fills only the small gap wasmify's stubs miss
# (execl/killpg/setpriority/pause/setre[ug]id/setpgid/chown/fchown/...).
#
# Only what NEITHER wasi NOR wasmify provides is disabled below.

# vfork / raw syscall / wait4: no wasmify stub. Perl falls back (fork; no
# syscall; waitpid instead of wait4).
d_vfork='undef'
d_syscall='undef'
d_wait4='undef'
# No System V IPC under wasi (sys/ipc.h, sys/msg.h, sys/sem.h, sys/shm.h).
# perl.h gates these headers on HAS_SEM/HAS_MSG/HAS_SHM (d_sem/d_msg/d_shm), not
# the i_sys* header vars.
i_sysipc='undef'
i_sysmsg='undef'
i_syssem='undef'
i_sysshm='undef'
d_sem='undef'
d_msg='undef'
d_shm='undef'
d_semget='undef'
d_semctl='undef'
d_semop='undef'
d_msgget='undef'
d_msgctl='undef'
d_msgsnd='undef'
d_msgrcv='undef'
d_shmget='undef'
d_shmctl='undef'
d_shmat='undef'
d_shmdt='undef'
# No shadow passwords (shadow.h / getspnam) under wasi.
i_shadow='undef'
d_getspnam='undef'
# glibc/BSD math-error-mode switch (_LIB_VERSION=_IEEE_); wasi-libc has no
# _LIB_VERSION. Gated by LIBM_LIB_VERSION (d_libm_lib_version).
d_libm_lib_version='undef'
# No terminal control under wasi: there is no <termios.h>. POSIX.xs guards its
# \`#include <termios.h>\` (and the tcgetattr/tc* + Termios class) on I_TERMIOS,
# so turn i_termios off; POSIX still builds, just without the termios interface.
i_termios='undef'
d_tcgetpgrp='undef'
d_tcsetpgrp='undef'
# (sockets, gethostbyname/addr, sigaction, dup/dup2 come from wasmify's stubs +
# host shims — left enabled.) The netdb functions have system prototypes now
# (wasmify's netdb.h for gethostby*, scripts/wasi-posix-shim.h for the *ent /
# by-net / by-serv / by-proto structs + prototypes), so tell Perl NOT to emit
# its own conflicting fallback declarations.
d_gethostprotos='define'
d_getnetprotos='define'
d_getservprotos='define'
d_getprotoprotos='define'
# Linux-only prctl (sys/prctl.h, gated on HAS_PRCTL / d_prctl); absent on wasi.
d_prctl='undef'
d_prctl_set_name='undef'
# User/group databases: wasi ships no pwd.h / grp.h, so do NOT include them.
# The struct passwd/group + get/set/end prototypes come from wasi-posix-shim.h;
# the functions stay ENABLED (Perl's pp_gpwent/pp_ggrent use them) and resolve
# to host shims / ENOSYS stubs at link. d_*_r flags off (no reentrant variants);
# d_getpwent_protos/d_getgrent_protos on so Perl uses the shim prototypes.
i_pwd='undef'
i_grp='undef'
d_getpwent_protos='define'
d_getgrent_protos='define'
d_getpwnam_r='undef'
d_getpwuid_r='undef'
d_getpwent_r='undef'
d_getgrnam_r='undef'
d_getgrgid_r='undef'
d_getgrent_r='undef'
# wasi-libc's FILE is opaque; Perl must not poke its glibc _IO_FILE internals.
d_stdstdio='undef'
d_stdiobase='undef'
d_stdio_ptr='undef'
d_stdio_cnt='undef'
d_stdio_stream_array='undef'

# Extension set (our static-ext policy).
usedl='undef'
static_ext='$STATIC_EXT'
dynamic_ext=' '
nonxs_ext='$NONXS_EXT'
extensions='$EXTENSIONS'
# ============================================================================
EOF

echo "== wrote $CONFIG (static_ext: $(echo $STATIC_EXT | wc -w | tr -d ' ') modules)"

# --- Canonicalise config.sh: keep only the LAST assignment of each key ------
# We layer overrides by APPENDING `key='...'` lines after the arm base, so each
# overridden key now appears twice. The two config.sh consumers disagree on
# which duplicate wins:
#   * Shell sourcing (cflags.SH → the CORE $(CCCMD)) keeps the LAST — our wasi
#     override. The core .c files therefore compile with clang + --sysroot.
#   * `configpm`, which bakes lib/Config_heavy.pl (→ %Config, read by MakeMaker
#     for every XS extension such as DynaLoader), keeps the FIRST — the arm
#     base. So the extension Makefiles came out with `CC=cc` and base ccflags
#     (no --sysroot), and DynaLoader.xs pulled the host macOS SDK headers.
# Collapse each key to a single line holding its LAST value so BOTH consumers
# agree on the wasi toolchain. Comments/blank lines are preserved in place;
# every config.sh line is a single-line `key='value'` assignment (verified: no
# multi-line values), so a per-line pass is safe.
awk '
  /^[A-Za-z_][A-Za-z0-9_]*=/ {
    key = substr($0, 1, index($0, "=") - 1)
    lastnr[key] = NR
  }
  { line[NR] = $0 }
  END {
    for (i = 1; i <= NR; i++) {
      s = line[i]
      if (s ~ /^[A-Za-z_][A-Za-z0-9_]*=/) {
        k = substr(s, 1, index(s, "=") - 1)
        if (lastnr[k] != i) continue   # drop earlier duplicate assignment
      }
      print s
    }
  }
' "$CONFIG" > "$CONFIG.dedup" && mv "$CONFIG.dedup" "$CONFIG"
echo "== canonicalised $CONFIG (deduped to last-wins per key)"

# --- Emit config.h + Makefile from config.sh (no probing) -------------------
# `Configure -S` performs variable substitution on the *.SH templates using the
# existing config.sh: it writes config.h, Makefile, and the other generated
# build files, and does NOT re-probe the (unrunnable) target.
cd "$PERL"
# Snapshot the old config.h so we can tell whether this reconfigure changed the
# compile inputs. Perl's Makefile does NOT list config.h as a prerequisite of
# the core *.o files, so a flag change alone would leave stale objects that
# `make` happily reuses (a real trap: e.g. flipping d_sigsetjmp had no effect
# until the objects were removed). If config.h changes, invalidate the compiled
# products so the next build recompiles against the new config. A no-op
# reconfigure (byte-identical config.h) keeps the objects and stays fast.
OLD_CONFIG_H_SUM=""
[ -f config.h ] && OLD_CONFIG_H_SUM=$(cksum < config.h)
echo "== running ./Configure -S (substitution only, no target probes)"
./Configure -S
NEW_CONFIG_H_SUM=$(cksum < config.h)
if [ "$OLD_CONFIG_H_SUM" != "$NEW_CONFIG_H_SUM" ]; then
  echo "== config.h changed — removing stale objects so the next build recompiles"
  rm -f "$PERL"/*.o "$PERL"/ext/DynaLoader/*.o "$PERL"/DynaLoader.o "$PERL"/libperl.a
fi

# NOTE: lib/ is populated with the pure-Perl dual-life modules by the BUILD, not
# here. `make n_dummy` (the nonxs_ext target set above) runs each module's
# pm_to_blib under the host miniperl, which copies its .pm into the right lib/
# path AND generates the *_pm.PL modules (lib.pm, XSLoader.pm, ...). A previous
# revision hand-mirrored dist/*/lib + cpan/*/lib into lib/, but that missed the
# flat (no lib/ subdir) modules and every generated *_pm.PL, and could not know
# the dir->module name mapping — so the build must own it. See the build_commands
# in wasmify.json: `make n_dummy` precedes `make libperl.a ext.libs`.

echo "== done: $(ls -1 config.h Makefile 2>/dev/null | tr '\n' ' ')"
