#!/bin/bash
# wasi-configure.sh — configure Perl for wasm64-wasi (idempotent).
#
# This is the FIRST step of the wasmify pipeline for perl-wasm. wasmify owns the
# actual wasm compilation (compiler / flag transforms); this script owns the one
# thing wasmify does NOT: producing a Perl `config.h` + `Makefile` whose answers
# are correct for the wasm64-wasi TARGET rather than the build host. It is the
# Perl analogue of python-wasm's scripts/wasi-configure.sh.
#
# Approach (Route B — a baked target config.sh):
#   Perl's Configure normally probes the target by compiling AND RUNNING little
#   test programs. Under wasm64-wasi we cannot run them (no runner), so instead
#   we hand Configure a ready-made config.sh and let it do variable substitution
#   only (`Configure -S`), which emits config.h and Makefile without probing.
#   The base is perl5/Cross/config.sh-arm-linux: it is the SAME Perl (5.42.2)
#   and little-endian, but 32-bit (longsize=ptrsize=4). The target is LP64:
#   wasm64 (memory64) with wasi-libc, where a pointer, a long and a size_t
#   are 8 bytes and an int is 4 — the C data model of the 64-bit machines the
#   native modules a program loads are compiled for. The block "the C data
#   model" below overwrites every answer of the base that follows from its
#   sizes. We also overwrite what differs for wasi: the toolchain (-> wasi-sdk),
#   a few host capabilities Perl must not assume (fork/dynamic-loading/
#   threads), and the extension set (our static-ext policy; see
#   docs/xs-modules.md).
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
# The target: wasm64 (memory64) under WASI preview 1, with the wasm64
# sysroot wasmify provisions into the SDK (`wasmify install-sdk --wasm64`;
# a wasm64 wasm-build provisions it too). The compiler is told so in every
# flag set below, so that the build wasmify captures compiles for the same
# target as the wasm-build that replays it (which sets --target itself).
TARGET=wasm64-wasip1
# The name of the architecture: the directories of installed modules that
# are specific to it are named after it (archlib, sitearch).
ARCHNAME=wasm64-wasi
# wasi-sdk emulation opt-in defines (paired with -lwasi-emulated-* at link).
WASI_EMU='-D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_GETPID -DPERL_WASI_SPAWN -DPERL_WASI_SELECT'
# Force-include prototypes for POSIX functions wasi-libc guards out but Perl's
# core references (dup/exec/kill/... — see the header). Absolute path, baked
# into config.sh (regenerated per host, so CI-portable).
POSIX_SHIM="-include $HERE/scripts/wasi-posix-shim.h"
# Headers of system interfaces wasi-libc has no header for and this project
# implements (<syslog.h>, see perl_syslog.cc).
SYSTEM_HEADERS="-I$HERE/scripts/include"
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
# The compiler's version as perl records it for a gcc-compatible compiler
# (Configure reads __VERSION__ the same way).
GCCVERSION="$(echo __VERSION__ | "$CLANG" -E -P - 2>/dev/null | tr -d '"')"

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
# external-lib / SysV-IPC / mmap / threads / test-only modules are OUT.
# NOTE on PerlIO::scalar: it is deliberately NOT in this XS list. In 5.42 it has
# no .xs — the in-memory scalar-filehandle layer lives in the core perlio.c —
# so it is a PURE-PERL module (just scalar.pm) carried by nonxs_ext, and the
# native nonxs list already includes it. perl.cc can still capture STDOUT via
# `open $fh,'>',\$buf`, which the built-in scalar layer handles without the .pm.
STATIC_EXT="attributes B Compress/Raw/Bzip2 Compress/Raw/Zlib Cwd Data/Dumper \
Devel/Peek Digest/MD5 Digest/SHA Encode Fcntl File/DosGlob File/Glob \
Filter/Util/Call Hash/Util Hash/Util/FieldHash I18N/Langinfo IO List/Util \
Math/BigInt/FastCalc MIME/Base64 Opcode POSIX PerlIO/encoding \
PerlIO/via SDBM_File Socket Storable Sys/Hostname Sys/Syslog Time/HiRes \
Time/Piece Unicode/Collate Unicode/Normalize mro re"
# Time/HiRes builds as the REAL core XS module: gettimeofday, usleep,
# nanosleep, sleep, time, the interval timer (alarm/ualarm/setitimer/
# getitimer), clock_gettime and clock_getres. wasi's clockid_t is
# `const struct __clockid *` (a struct pointer, CLOCK_REALTIME =
# &_CLOCK_REALTIME) while Time::HiRes moves clock ids through Perl as
# integers, so its XS is given integer clock ids over the wasi clocks (the
# HiRes.xs patch below). clock_nanosleep stays configured off. See
# docs/xs-modules.md.

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

# --- Generate the wasm64-wasi config.sh -------------------------------------
# Start from the pinned 32-bit base, then append overrides. config.sh is sourced
# top-to-bottom by Configure, so a later assignment wins over the base value.
CONFIG="$PERL/config.sh"
cp "$BASE" "$CONFIG"

cat >> "$CONFIG" <<EOF

# ==== wasm64-wasi overrides (generated by scripts/wasi-configure.sh) =========
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
# Time::HiRes features (dist/Time-HiRes/Makefile.PL consults these before
# probing, exactly for cross-compilation). clock_gettime and clock_getres
# are wasi-libc's, reached through integer clock ids (the HiRes.xs patch
# further down): a program that finds no clock_gettime falls back to worse
# time sources (AnyEvent's loop to POSIX::times). clock_nanosleep stays
# off: the sleeps of this build are perl_signal.cc's, which a signal
# interrupts, and wasi-libc's clock_nanosleep is not one of them. nanosleep
# and usleep DO exist in wasi-libc; the base disables nanosleep, so
# re-enable it. setitimer, getitimer and ualarm have no wasi-libc symbols:
# perl_signal.cc defines them over the host's real-time interval timer
# (ITIMER_REAL, the only timer there is), and scripts/wasi-posix-shim.h
# declares them.
d_clock_gettime='define'
d_clock_getres='define'
d_clock_nanosleep='undef'
d_setitimer='define'
d_getitimer='define'
d_ualarm='define'
d_nanosleep='define'
# wasi-sdk emulation opt-ins for POSIX features Perl needs but wasi lacks
# natively: process clocks (times), signals (%SIG), mman (mmap), getpid (\$\$).
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
ccflags='--target=$TARGET --sysroot=$SYSROOT -mllvm -wasm-enable-sjlj -D_GNU_SOURCE $WASI_EMU $POSIX_SHIM $SYSTEM_HEADERS -fno-strict-aliasing'
cppflags='--target=$TARGET --sysroot=$SYSROOT -mllvm -wasm-enable-sjlj -D_GNU_SOURCE $WASI_EMU $POSIX_SHIM $SYSTEM_HEADERS -fno-strict-aliasing'
optimize='-O2'
cccdlflags=' '
ccdlflags=' '
ldflags='--target=$TARGET --sysroot=$SYSROOT'
lddlflags=' '
libs='-lm -lsetjmp -lwasi-emulated-process-clocks -lwasi-emulated-signal -lwasi-emulated-mman -lwasi-emulated-getpid'
libc='$SYSROOT/lib/$TARGET/libc.a'
perllibs='-lm'
# libpth is not the wasi sysroot: %Config keeps this value for the life of
# the installed perl, where it tells ExtUtils::Liblist, DynaLoader
# (@dl_library_path) and library finders such as FFI::CheckLib where
# libraries are — and the sysroot is a directory of the machine that built
# perl. The libraries a program can load at run time are the host's, so
# these are the host's conventional library directories. Nothing in this
# build looks a library up through it: the interpreter is linked by wasmify
# with the libraries of `libs`, found through --sysroot.
libpth='/usr/local/lib /usr/lib /lib'

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
archname='$ARCHNAME'
myarchname='$ARCHNAME'
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

# The base config predates getaddrinfo, but wasmify's host-sockets shim
# implements getaddrinfo/freeaddrinfo/gai_strerror/getnameinfo (names
# resolved by the host). Socket.xs only calls the real resolver under
# HAS_GETADDRINFO — its fallback emulates it over gethostbyname, which has
# no wasm implementation — so DNS (and with it HTTP::Tiny, cpanm's index
# lookups, ...) needs these on.
d_getaddrinfo='define'
d_getnameinfo='define'
d_gai_strerror='define'
# The base config predates these as well, and wasi-libc has all of them:
# inet_pton and inet_ntop, and in <netinet/in.h> and <sys/socket.h> the
# structures sockaddr_in6 (with sin6_scope_id), sockaddr_storage, ip_mreq,
# ip_mreq_source and ipv6_mreq. Socket.xs compiles inet_pton/inet_ntop, the
# sockaddr_in6 packers and the multicast-request packers only when the
# configuration says they exist; without them they croak "not implemented on
# this architecture", which takes down everything that formats an address
# with them (IO::Socket::IP, and through it most networking code).
d_inetpton='define'
d_inetntop='define'
d_sockaddr_in6='define'
d_sin6_scope_id='define'
d_sockaddr_storage='define'
d_ip_mreq='define'
d_ip_mreq_source='define'
d_ipv6_mreq='define'

# vfork / wait4: no wasmify stub. Perl falls back (fork; waitpid instead
# of wait4). syscall() is the host's raw system call (pp_syscall calls
# goperl_syscall, perl_libc.cc, below).
d_vfork='undef'
d_syscall='define'
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
# wasi-libc has no <syslog.h> and no syslog(3); this project has both:
# scripts/include/syslog.h, and openlog/syslog/closelog/setlogmask in
# perl_syslog.cc, which hand each message to the host. Sys::Syslog's "native"
# mechanism is built on them like on any libc's.
i_syslog='define'
# wasi-libc has no <termios.h> and no terminal control; this project has
# both: scripts/include/termios.h, and tcgetattr and its companions in
# perl_libc.cc, which ask the host. POSIX is built with POSIX::Termios and
# the tc* functions like on any libc.
i_termios='define'
d_tcgetpgrp='define'
d_tcsetpgrp='define'
d_ctermid='define'
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
# futimes: the arm base answers 'undef', and wasi-libc has none. wasmify's
# host-fs shim (bridge.HostFS in wasmify.json) defines it over futimens and
# scripts/wasi-posix-shim.h declares it, so `utime` works on a filehandle.
d_futimes='define'
# wasi-libc's FILE is opaque; Perl must not poke its glibc _IO_FILE internals.
d_stdstdio='undef'
d_stdiobase='undef'
d_stdio_ptr='undef'
d_stdio_cnt='undef'
d_stdio_stream_array='undef'

# ---- what the base says about its own target and is not true of this one ----
# The base describes a 2003 arm-linux system with glibc 2.2. The answers
# below are the ones that differ on wasm64 with wasi-libc (a musl); each was
# read off the compiler and the C library, not carried over.
#
# long double is IEEE 754 binary128, little-endian: 16 bytes, 113 bits of
# precision. The base says 8 bytes next to the 16-byte images of the x86
# extended format, and perl.h then builds PL_inf and PL_nan (the NV is a
# double, the same size as that "long double") from the first 8 of those
# bytes, which are zero: "inf"+0 and "nan"+0 gave 0.
longdblsize='16'
longdblkind='1'
longdblmantbits='112'
longdblinfbytes='0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x7f'
longdblnanbytes='0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xff, 0x7f'
d_long_double_style_ieee='define'
d_long_double_style_ieee_std='define'
# The printf and scanf length modifier of a long double is L; the base has
# none (its long double was a double). sprintf("%Lf") passes one to the C
# library with this format.
sPRIfldbl='"Lf"'
sPRIgldbl='"Lg"'
sPRIeldbl='"Le"'
sPRIFUldbl='"LF"'
sPRIGUldbl='"LG"'
sPRIEUldbl='"LE"'
sSCNfldbl='"Lf"'
# A double is aligned on 8 bytes.
alignbytes='8'

# ---- the C data model: LP64 -------------------------------------------------
# A pointer, a long and a size_t are 8 bytes, an int is 4, a long long is 8:
# the sizes of wasm64 with wasi-libc, read off the compiler and the C
# library's headers. The base has the sizes of its 32-bit target; these are
# the answers Configure derives on an LP64 system, each from the rule
# Configure has for it:
#   - use64bitall, and with it use64bitint: IV and UV are 64 bits wide, and
#     on LP64 the type of that width is long, which is also int64_t
#     (wasi-libc's PRId64 is "ld"); a quad is a long (QUAD_IS_LONG);
#   - the 32-bit integer type is int, a long being 64 bits wide;
#   - the printf formats of each type use its own length modifier (l for
#     long, none for int); a uid_t and a gid_t are unsigned ints;
#   - an NV (a double, 53 bits of mantissa) no longer holds every UV;
#   - byteorder is the order of the bytes of a UV, so it has eight digits.
# Configure also appends "-64all" to archname on a platform where 64-bit
# pointers are a choice; on wasm64 they are the platform's own, so the
# name has no suffix (as on x86_64-linux).
ptrsize='8'
longsize='8'
sizesize='8'
intsize='4'
longlongsize='8'
use64bitall='define'
use64bitint='define'
ivtype='long'
uvtype='unsigned long'
ivsize='8'
uvsize='8'
ivdformat='"ld"'
uvuformat='"lu"'
uvoformat='"lo"'
uvxformat='"lx"'
uvXUformat='"lX"'
i64type='long'
u64type='unsigned long'
i64size='8'
u64size='8'
quadtype='long'
uquadtype='unsigned long'
quadkind='2'
d_quad='define'
sPRId64='"ld"'
sPRIi64='"li"'
sPRIu64='"lu"'
sPRIo64='"lo"'
sPRIx64='"lx"'
sPRIXU64='"lX"'
i32type='int'
u32type='unsigned int'
i32size='4'
u32size='4'
i32dformat='"d"'
u32uformat='"u"'
u32oformat='"o"'
u32xformat='"x"'
u32XUformat='"X"'
uidformat='"u"'
gidformat='"u"'
uidsize='4'
gidsize='4'
d_nv_preserves_uv='undef'
nv_preserves_uv_bits='53'
byteorder='12345678'
# st_dev and st_ino are unsigned 64-bit; the base says 4 bytes, and stat
# then returned only the low half of a device number. The wide path of
# pp_stat formats the number with snprintf.
st_dev_size='8'
st_ino_size='8'
d_snprintf='define'
# setlocale(LC_ALL, NULL) names the categories by position, separated by
# ";" ("C.UTF-8;C;C;C;C;C"), in the order LC_CTYPE, LC_NUMERIC, LC_TIME,
# LC_COLLATE, LC_MONETARY, LC_MESSAGES: the values 0 to 5 of the C library's
# <locale.h>. The base says name=value pairs (glibc), with the word "define"
# where the separator and the positions belong; locale.c then could not
# read the string back, and a program started with LC_CTYPE differing from
# the other categories died in a panic. A locale has to be one the C library
# has data for (perl_libc.cc), so not every name is accepted.
d_perl_lc_all_uses_name_value_pairs='undef'
d_perl_lc_all_separator='define'
perl_lc_all_separator='";"'
d_perl_lc_all_category_positions_init='define'
perl_lc_all_category_positions_init='{ 0, 1, 2, 3, 4, 5 }'
d_setlocale_accepts_any_locale_name='undef'
# The C library is not glibc, and the compiler is not gcc 2.95. perl.h
# defines _GNU_SOURCE for glibc; for a musl, perl's own hints (hints/linux.sh)
# put -D_GNU_SOURCE into ccflags instead, and so does ccflags above: perl
# uses functions the C library declares only then (memrchr, gcvt).
d_gnulibc='undef'
gnulibc_version=''
gccversion='$GCCVERSION'
# The C99 classification macros and math functions, all in <math.h> and
# libm. The base predates them, and POSIX.xs falls back to approximations
# of its own (or to "not implemented") for each one it is told is missing.
d_fpclassify='define'
d_isfinite='define'
d_isnormal='define'
d_asinh='define'
d_atanh='define'
d_cbrt='define'
d_copysign='define'
d_erf='define'
d_erfc='define'
d_exp2='define'
d_expm1='define'
d_fdim='define'
d_fma='define'
d_fmax='define'
d_fmin='define'
d_hypot='define'
d_ilogb='define'
d_lgamma='define'
d_lgamma_r='define'
d_llrint='define'
d_llrintl='define'
d_llround='define'
d_llroundl='define'
d_log1p='define'
d_log2='define'
d_logb='define'
d_lrint='define'
d_lrintl='define'
d_lround='define'
d_lroundl='define'
d_nan='define'
d_nearbyint='define'
d_nextafter='define'
d_nexttoward='define'
d_remainder='define'
d_remquo='define'
d_rint='define'
d_round='define'
d_scalbn='define'
d_tgamma='define'
d_trunc='define'
# String functions the C library has and the base predates.
d_memmem='define'
d_memrchr='define'
d_strnlen='define'
d_strlcat='define'
d_strlcpy='define'
# Who configured this perl: not the author of the base, on his machine.
cf_by='perl-wasm'
cf_email='root@localhost'
perladmin='root@localhost'
myhostname='localhost'
mydomain=''
myuname='wasi wasm64'

# Extension set (our static-ext policy).
usedl='undef'
static_ext='$STATIC_EXT'
dynamic_ext=' '
nonxs_ext='$NONXS_EXT'
extensions='$EXTENSIONS'
# ============================================================================
EOF

# --- Installation layout ----------------------------------------------------
# The base describes an arm build staged under ./install_me_here. A program that
# asks %Config where perl is installed (ExtUtils::MakeMaker, Module::Build,
# Pod::Html, ...) must get absolute directories named after this architecture,
# the same ones perl searches, as on any installed perl. "." is left out of
# @INC, perl's default since 5.26, which the base predates.
PERL_VERSION="$(sed -n "s/^version='\(.*\)'$/\1/p" "$BASE")"
[ -n "$PERL_VERSION" ] || { echo "!! no version in $BASE" >&2; exit 1; }
PRIVLIB="/usr/lib/perl5/$PERL_VERSION"
SITELIB="/usr/lib/perl5/site_perl/$PERL_VERSION"
cat >> "$CONFIG" <<EOF
default_inc_excludes_dot='define'
archlib='$PRIVLIB/$ARCHNAME'
archlibexp='$PRIVLIB/$ARCHNAME'
sitearch='$SITELIB/$ARCHNAME'
sitearchexp='$SITELIB/$ARCHNAME'
installstyle='lib/perl5'
installprefix='/usr'
installprefixexp='/usr'
installbin='/usr/bin'
installscript='/usr/bin'
installsitebin='/usr/bin'
installsitescript='/usr/bin'
installprivlib='$PRIVLIB'
installarchlib='$PRIVLIB/$ARCHNAME'
installsitelib='$SITELIB'
installsitearch='$SITELIB/$ARCHNAME'
installman1dir='/usr/share/man/man1'
installman3dir='/usr/share/man/man3'
installsiteman1dir='/usr/share/man/man1'
installsiteman3dir='/usr/share/man/man3'
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

# --- The name of the architecture is the host's -----------------------------
# One interpreter runs on every host the embedder is built for, and the
# native modules a program loads are built for that host: the name of the
# architecture is the host's, which the embedder gives at start-up
# (perl_new's archname; perl.cc keeps it). perl.c names the architecture
# subdirectories it adds to @INC with a directory (PERL5LIB, -I, ...) and
# says what perl was built for (-v) with the compile-time ARCHNAME; it asks
# perl.cc instead. config.sh's archname is the build's own name, which
# %Config does not show (scripts/make-stdlib-zip.py).
python3 - "$PERL/perl.c" <<'PYPATCH'
import sys
path = sys.argv[1]
t = open(path).read()
marker = "perl_wasi_archname"
if marker in t:
    print("run-time archname already in perl.c")
    sys.exit(0)
edits = [
    ('#include "perl.h"\n',
     '#include "perl.h"\n'
     '/* the name of the architecture: the host\'s (perl.cc) */\n'
     'extern const char *perl_wasi_archname(void);\n'),
    ('                " (%" SVf ") built for "\tARCHNAME, SVfARG(level)\n',
     '                " (%" SVf ") built for %s", SVfARG(level), perl_wasi_archname()\n'),
    ('                sv_catpvs(subdir, "/" PERL_FS_VERSION "/" ARCHNAME);\n',
     '                sv_catpvf(subdir, "/" PERL_FS_VERSION "/%s", perl_wasi_archname());\n'),
    ('                sv_catpvs(subdir, "/" ARCHNAME);\n',
     '                sv_catpvf(subdir, "/%s", perl_wasi_archname());\n'),
]
for old, new in edits:
    n = t.count(old)
    assert n == 1, (path, old[:60], n)
    t = t.replace(old, new)
assert "ARCHNAME" not in t.replace("PERL_WASI", ""), "perl.c still uses ARCHNAME"
open(path, "w").write(t)
print("run-time archname patched into perl.c")
PYPATCH

# --- Emit config.h + Makefile from config.sh (no probing) -------------------
# `Configure -S` performs variable substitution on the *.SH templates using the
# existing config.sh: it writes config.h, Makefile, and the other generated
# build files, and does NOT re-probe the (unrunnable) target.
cd "$PERL"

# ---- fork-free subprocess routing (PERL_WASI_SPAWN) -------------------------
# WASI cannot fork. The runtime provides posix_spawn/waitpid/pipe, so:
#   - Perl_my_popen / Perl_my_popen_list return the spawn-based versions
#     implemented in the bridge (wasi_spawn_popen*); my_pclose stays stock
#     (it only waitpids the pid recorded in PL_fdpid).
#   - pp_system takes its non-fork branch, whose system(3) the bridge
#     implements over posix_spawn.
# Applied at configure time; idempotent (a marker guards each file), so a
# re-run over an already-patched tree is a no-op.
python3 - <<'PYPATCH'
def patch(path, edits):
    t = open(path).read()
    if "PERL_WASI_SPAWN" in t or "PERL_WASI_SELECT" in t:
        print(f"wasi-spawn routing already present in {path}")
        return
    for anchor, replacement, expect in edits:
        n = t.count(anchor)
        assert n >= expect, (path, anchor[:60], n)
        t = t.replace(anchor, replacement)
    open(path, "w").write(t)

a = "PerlIO *\nPerl_my_popen_list(pTHX_ const char *mode, int n, SV **args)\n{\n"
b = "PerlIO *\nPerl_my_popen(pTHX_ const char *cmd, const char *mode)\n{\n"
patch("util.c", [
    (a, a + "#ifdef PERL_WASI_SPAWN\n    extern PerlIO *wasi_spawn_popen_list(pTHX_ const char *mode, int n, SV **args);\n    return wasi_spawn_popen_list(aTHX_ mode, n, args);\n#endif\n", 1),
    # the fork version plus platform-alternative bodies
    (b, b + "#ifdef PERL_WASI_SPAWN\n    extern PerlIO *wasi_spawn_popen(pTHX_ const char *cmd, const char *mode);\n    return wasi_spawn_popen(aTHX_ cmd, mode);\n#endif\n", 1),
])
c = "#if (defined(HAS_FORK) || defined(__amigaos4__)) && !defined(VMS) && !defined(OS2)"
# pp_sselect passes Perl's select bit vectors directly as fd_set, which is
# only correct where fd_set is that bitmask; wasi-libc's fd_set is a
# {count, fd array} struct. Route the call through the bridge's converter.
d = "#ifdef PERL_IRIX5_SELECT_TIMEVAL_VOID_CAST"
# fork is a pseudo-process (see "fork as a pseudo-process" in perl_wasi.cc):
# pp_fork forks through the host, exec runs the command and exits with its
# status, and pp_open records the stack height it returns at so a pipe-open
# child can finish the op without the C frame that held it.
e = ("    PERL_FLUSHALL_FOR_CHILD;\n"
     "    if (PL_op->op_flags & OPf_STACKED) {\n"
     "        SV * const really = *++MARK;\n"
     "        value = (I32)do_aexec(really, MARK, SP);\n")
g = "PP_wrapped(pp_fork, 0, 0)\n{\n#ifdef HAS_FORK\n    dSP; dTARGET;\n    Pid_t childpid;\n"
o = "    tmps = SvPV_const(sv, len);\n    ok = do_open6(gv, tmps, len, NULL, MARK+1, (SP-MARK));\n"
# dbmopen hands the open flags it ties with to TIEHASH, which takes the
# numbers the program sees (a program that shows the host's numbers passes
# the host's flags, and SDBM_File's TIEHASH turns them into the C
# library's): the flags dbmopen makes are the program's numbers too.
dbm = "PP_wrapped(pp_dbmopen, 3, 0)\n{\n"
patch("pp_sys.c", [
    (g, g + "#ifdef PERL_WASI_SPAWN\n"
        "    {\n"
        "        extern OP *wasi_fork(pTHX);\n"
        "        PERL_UNUSED_VAR(sp);\n"
        "        PERL_UNUSED_VAR(targ);\n"
        "        PERL_UNUSED_VAR(childpid);\n"
        "        return wasi_fork(aTHX);\n"
        "    }\n"
        "#endif\n", 1),
    (c, "#if (defined(HAS_FORK) || defined(__amigaos4__)) && !defined(VMS) && !defined(OS2) && !defined(PERL_WASI_SPAWN)", 1),
    (d, "#ifdef PERL_WASI_SELECT\n"
        "    {\n"
        "        extern int wasi_bitvec_select(int nbits, char *rbits, char *wbits, char *ebits, struct timeval *tbuf);\n"
        "        nfound = wasi_bitvec_select(maxlen * 8, fd_sets[1], fd_sets[2], fd_sets[3], tbuf);\n"
        "    }\n"
        "#elif defined(PERL_IRIX5_SELECT_TIMEVAL_VOID_CAST)", 1),
    (e, "    PERL_FLUSHALL_FOR_CHILD;\n"
        "#ifdef PERL_WASI_SPAWN\n"
        "    {\n"
        "        /* returns only when the command could not be started */\n"
        "        extern void wasi_exec(pTHX_ SV **mark, SV **sp, int stacked);\n"
        "        wasi_exec(aTHX_ MARK, SP, (PL_op->op_flags & OPf_STACKED) != 0);\n"
        "        SP = ORIGMARK;\n"
        "        XPUSHi(0);\n"
        "        RETURN;\n"
        "    }\n"
        "#endif\n"
        "    if (PL_op->op_flags & OPf_STACKED) {\n"
        "        SV * const really = *++MARK;\n"
        "        value = (I32)do_aexec(really, MARK, SP);\n", 1),
    (dbm, "#ifdef PERL_WASI_SPAWN\n"
        "extern UV wasi_program_open_flags(UV flags);\n"
        "#define DBM_OPEN_FLAGS(f) wasi_program_open_flags(f)\n"
        "#else\n"
        "#define DBM_OPEN_FLAGS(f) (f)\n"
        "#endif\n" + dbm, 1),
    ("        mPUSHu(O_RDWR|O_CREAT);\n", "        mPUSHu(DBM_OPEN_FLAGS(O_RDWR|O_CREAT));\n", 1),
    ("        mPUSHu(O_RDWR);\n", "        mPUSHu(DBM_OPEN_FLAGS(O_RDWR));\n", 1),
    ("        mPUSHu(O_RDONLY);\n", "        mPUSHu(DBM_OPEN_FLAGS(O_RDONLY));\n", 1),
    (o, "    tmps = SvPV_const(sv, len);\n"
        "#ifdef PERL_WASI_SPAWN\n"
        "    {\n"
        "        extern SSize_t wasi_open_return_sp;\n"
        "        wasi_open_return_sp = ORIGMARK - PL_stack_base;\n"
        "    }\n"
        "#endif\n"
        "    ok = do_open6(gv, tmps, len, NULL, MARK+1, (SP-MARK));\n", 1),
])
print("wasi-spawn source routing applied")
PYPATCH

# ---- Hash::Util: the object keeps the name the compiler gives it ---------------
# ext/Hash-Util/Makefile.PL has a rule of its own for a static build: it
# compiles the extension as HashUtil.c and renames HashUtil.o to Util.o
# afterwards, for platforms whose static libraries hold one object per name
# and compare the names without regard to case (the top-level util.o). The
# capture of this build records what the compiler and the archiver are
# called with, not a rename between the two: the archiver's Util.o was an
# object no recorded compilation had written, the archive of the extension
# was never made again, and the link went on using whatever archive an
# earlier build had left — code compiled for another configuration. Nothing
# here needs the rule (every extension has an archive of its own), so the
# extension is built by the standard rules like the others. Idempotent.
python3 - <<'PYPATCH'
path = "ext/Hash-Util/Makefile.PL"
t = open(path).read()
marker = "perl-wasm: standard rules"
if marker in t:
    print(f"standard rules already selected in {path}")
else:
    old = "    unless (defined $Config{usedl}) {\n"
    assert t.count(old) == 1, path
    t = t.replace(old, "    # " + marker + " (see scripts/wasi-configure.sh)\n"
                       "    unless (defined $Config{usedl} || $Config{osname} eq 'wasi') {\n")
    open(path, "w").write(t)
    print(f"standard rules selected in {path}")
PYPATCH

# ---- Time::HiRes: clocks named by integers ------------------------------------
# Time::HiRes passes clock ids between Perl and C as integers (its typemap
# says clockid_t is an IV, its constants are CLOCK_REALTIME and friends).
# wasi-libc names a clock by the address of an object. The CLOCK_ constants
# are integers already (scripts/wasi-posix-shim.h); here the XS gets an
# integer clockid_t, and its clock_gettime/clock_getres calls go through
# functions that find the wasi clock of an id. The module does the same
# kind of thing itself where a system lacks the functions (its darwin
# emulation). Idempotent.
python3 - <<'PYPATCH'
path = "dist/Time-HiRes/HiRes.xs"
t = open(path).read()
marker = "th_wasi_clockid_t"
if marker in t:
    print(f"integer clock ids already present in {path}")
else:
    old = ("#ifndef TIME_HIRES_CLOCKID_T\n"
           "typedef int clockid_t;\n"
           "#endif\n")
    new = old + r"""
#ifdef __wasi__
/* wasi names a clock by the address of an object; Perl code names it by a
 * small integer, as on every other system. The integers are the ones
 * scripts/wasi-posix-shim.h gives the CLOCK_ constants (Linux's). */
typedef int th_wasi_clockid_t;
static clockid_t th_wasi_clock(th_wasi_clockid_t id) {
    switch (id) {
    case CLOCK_REALTIME: return &_CLOCK_REALTIME;
    case CLOCK_MONOTONIC: return &_CLOCK_MONOTONIC;
    }
    return NULL; /* a clock this system does not have */
}
static int th_wasi_clock_gettime(th_wasi_clockid_t id, struct timespec *ts) {
    clockid_t clock = th_wasi_clock(id);
    if (!clock) {
        errno = EINVAL;
        return -1;
    }
    return clock_gettime(clock, ts);
}
static int th_wasi_clock_getres(th_wasi_clockid_t id, struct timespec *ts) {
    clockid_t clock = th_wasi_clock(id);
    if (!clock) {
        errno = EINVAL;
        return -1;
    }
    return clock_getres(clock, ts);
}
#  define clockid_t th_wasi_clockid_t
#  define clock_gettime(id, ts) th_wasi_clock_gettime((id), (ts))
#  define clock_getres(id, ts) th_wasi_clock_getres((id), (ts))
#endif /* __wasi__ */
"""
    assert t.count(old) == 1, path
    open(path, "w").write(t.replace(old, new))
    print(f"integer clock ids applied to {path}")
PYPATCH

# ---- Sys::Syslog: a native mechanism that can be unavailable ------------------
# Sys::Syslog assumes that openlog(3) cannot fail: connect_native calls it
# and reports the mechanism connected. Here openlog raises an exception when
# the program has no system logger (perl_syslog.cc; the embedder has to
# enable one). connect_native then has to answer like every other
# connect_* function that cannot connect: record why and return false, so
# that the module goes on to its next mechanism. Idempotent.
python3 - <<'PYPATCH'
path = "cpan/Sys-Syslog/Syslog.pm"
t = open(path).read()
marker = "no system logger"
if marker in t:
    print(f"native-mechanism patch already present in {path}")
else:
    old = ("    openlog_xs($ident, $logopt, xlate($facility));\n"
           "    $syslog_send = \\&_syslog_send_native;\n")
    new = ("    # openlog fails where the program has no system logger: the\n"
           "    # mechanism is then unavailable, like one that cannot connect.\n"
           "    if (!eval { openlog_xs($ident, $logopt, xlate($facility)); 1 }) {\n"
           "        chomp(my $why = $@);\n"
           "        push @$errs, \"native: $why\";\n"
           "        return 0;\n"
           "    }\n"
           "    $syslog_send = \\&_syslog_send_native;\n")
    assert t.count(old) == 1, path
    open(path, "w").write(t.replace(old, new))
    print(f"native-mechanism patch applied to {path}")
PYPATCH
# ---- B: the number of an op carries bit 62 ------------------------------------
# A native module of the host process makes an op a number (PTR2IV) and a
# number an op (INT2PTR), and so does B: the number of a B::OP object is the
# op's address. The module's own ops are in the host process, the
# interpreter's are in this memory, and an address of either can be any
# number below the end of the memory: a number alone would not say which.
# So the number of an interpreter op has bit 62 set, which no address of a
# host process has (user space ends below 2^47 on macOS and 2^48, or 2^57,
# on Linux; arm64 memory tags use bits 56-59), and the native SDK's PTR2IV
# and INT2PTR of an op do the same (go-perl's xs/sdk/include/perl.h,
# GOPERL_OP_NUMBER_TAG). B gives tagged numbers (make_op_object, the keys of
# B::overlay, walkoptree's objects) and takes tagged and untagged ones
# (T_OP_OBJ). No op is still the number 0. Idempotent.
python3 - <<'PYPATCH'
def patch(path, marker, edits):
    t = open(path).read()
    if marker in t:
        print(f"op numbers with bit 62 already in {path}")
        return
    for old, new in edits:
        assert t.count(old) == 1, (path, old[:60], t.count(old))
        t = t.replace(old, new)
    open(path, "w").write(t)
    print(f"op numbers with bit 62 applied to {path}")

patch("ext/B/B.xs", "GOPERL_OP_NUMBER_TAG", [
    ('#include "XSUB.h"\n',
     '#include "XSUB.h"\n'
     '\n'
     '/* The number of an op is its address with bit 62 set, which no address\n'
     ' * of the host process has: a native module tells an op of this\n'
     ' * interpreter from one of its own by the number (perl-wasm\n'
     ' * scripts/wasi-configure.sh). No op is 0. */\n'
     '#define GOPERL_OP_NUMBER_TAG ((IV)1 << 62)\n'
     '#define GOPERL_OP_NUMBER(o) ((o) ? (PTR2IV(o) | GOPERL_OP_NUMBER_TAG) : (IV)0)\n'
     '#define GOPERL_NUMBER_OP(type, iv) INT2PTR(type, (iv) & ~GOPERL_OP_NUMBER_TAG)\n'),
    ('    sv_setiv(newSVrv(opsv, opclassnames[op_class(o)]), PTR2IV(o));\n',
     '    sv_setiv(newSVrv(opsv, opclassnames[op_class(o)]), GOPERL_OP_NUMBER(o));\n'),
    ('    key = newSViv(PTR2IV(o));\n',
     '    key = newSViv(GOPERL_OP_NUMBER(o));\n'),
    ('    sv_setiv(object, PTR2IV(o));\n',
     '    sv_setiv(object, GOPERL_OP_NUMBER(o));\n'),
])
patch("ext/B/typemap", "GOPERL_NUMBER_OP", [
    ('T_OP_OBJ\n'
     '\tif (SvROK($arg)) {\n'
     '\t    IV tmp = SvIV((SV*)SvRV($arg));\n'
     '\t    $var = INT2PTR($type,tmp);\n',
     'T_OP_OBJ\n'
     '\tif (SvROK($arg)) {\n'
     '\t    IV tmp = SvIV((SV*)SvRV($arg));\n'
     '\t    $var = GOPERL_NUMBER_OP($type,tmp);\n'),
])
PYPATCH
# ---- B: an op number that is no address of this memory ------------------------
# A number without bit 62 that B takes as an op is an address of this
# memory, or one of the host process: a native module that makes a B::OP
# object of an op of its own (B::CallChecker builds one on its C stack and
# asks B for its next op) hands B an address outside this memory. B is given
# an op of this memory that stands for it (perl.cc goperl_native_op).
# Idempotent.
python3 - <<'PYPATCH'
path = "ext/B/B.xs"
t = open(path).read()
if "goperl_number_op" in t:
    print(f"op numbers outside the memory already taken in {path}")
else:
    old = ('#define GOPERL_NUMBER_OP(type, iv) INT2PTR(type, (iv) & ~GOPERL_OP_NUMBER_TAG)\n')
    new = ('#define GOPERL_NUMBER_OP(type, iv) ((type)goperl_number_op(aTHX_ (iv)))\n'
           'extern OP *goperl_native_op(pTHX_ UV addr);\n'
           'static void *goperl_number_op(pTHX_ IV iv) {\n'
           '    const UV addr = (UV)iv & ~(UV)GOPERL_OP_NUMBER_TAG;\n'
           '    if (!(iv & GOPERL_OP_NUMBER_TAG) &&\n'
           '        addr >= ((UV)__builtin_wasm_memory_size(0) << 16))\n'
           '        return goperl_native_op(aTHX_ addr);\n'
           '    return INT2PTR(void *, addr);\n'
           '}\n')
    assert t.count(old) == 1, (path, t.count(old))
    open(path, "w").write(t.replace(old, new))
    print(f"op numbers outside the memory taken in {path}")
PYPATCH
# ---- B: the numbers of pad structures ----------------------------------------
# The number of a B::PADLIST, B::PADNAMELIST or B::PADNAME object is the
# structure's address with bit 61 set and the kind of structure in bits 59
# and 60 (0 a pad name, 1 a list of names, 2 a padlist), which no address of
# the host process has: a native module that takes the structure of such an
# object (B::Flags reads PadnameFLAGS of a B::PADNAME) gets the SDK's view
# of it, which an address of this memory cannot tell from one of its own
# (go-perl's xs/sdk/include/goperl_compiler.h, GOPERL_PAD_NUMBER_TAG). B's
# typemaps take tagged and untagged numbers. No structure is still 0.
# Idempotent.
python3 - <<'PYPATCH'
def patch(path, marker, edits):
    t = open(path).read()
    if marker in t:
        print(f"pad structure numbers already in {path}")
        return
    for old, new in edits:
        assert t.count(old) == 1, (path, old[:60], t.count(old))
        t = t.replace(old, new)
    open(path, "w").write(t)
    print(f"pad structure numbers applied to {path}")

patch("ext/B/B.xs", "GOPERL_PAD_NUMBER", [
    ('#define GOPERL_OP_NUMBER_TAG ((IV)1 << 62)\n',
     '#define GOPERL_OP_NUMBER_TAG ((IV)1 << 62)\n'
     '/* The number of a pad structure: its address with bit 61 set and its\n'
     ' * kind in bits 59 and 60 (0 a pad name, 1 a list of names, 2 a\n'
     ' * padlist). No structure is 0. */\n'
     '#define GOPERL_PAD_NUMBER_TAG ((IV)1 << 61)\n'
     '#define GOPERL_PAD_NUMBER(p, kind) \\\n'
     '    ((p) ? (PTR2IV(p) | GOPERL_PAD_NUMBER_TAG | ((IV)(kind) << 59)) : (IV)0)\n'
     '#define GOPERL_NUMBER_PAD(type, iv) INT2PTR(type, (iv) & (((IV)1 << 59) - 1))\n'),
    ('\t    sv_setiv(newSVrv(rv, padlist ? "B::PADLIST" : "B::NULL"),\n'
     '\t\t     PTR2IV(padlist));\n',
     '\t    sv_setiv(newSVrv(rv, padlist ? "B::PADLIST" : "B::NULL"),\n'
     '\t\t     GOPERL_PAD_NUMBER(padlist, 2));\n'),
    ('\t\t     PTR2IV(PadlistNAMES(padlist)));\n',
     '\t\t     GOPERL_PAD_NUMBER(PadlistNAMES(padlist), 1));\n'),
    ('\t\t\t PTR2IV(padp[i]));\n',
     '\t\t\t GOPERL_PAD_NUMBER(padp[i], 0));\n'),
])
inp = ('\tif (SvROK($arg)) {\n'
       '\t    IV tmp = SvIV((SV*)SvRV($arg));\n'
       '\t    $var = INT2PTR($type,tmp);\n')
inp_new = ('\tif (SvROK($arg)) {\n'
           '\t    IV tmp = SvIV((SV*)SvRV($arg));\n'
           '\t    $var = GOPERL_NUMBER_PAD($type,tmp);\n')
edits = []
for name in ("T_PL_OBJ", "T_PNL_OBJ", "T_PN_OBJ"):
    edits.append((name + '\n' + inp, name + '\n' + inp_new))
for name, cls, kind in (("T_PL_OBJ", "B::PADLIST", 2), ("T_PNL_OBJ", "B::PADNAMELIST", 1),
                        ("T_PN_OBJ", "B::PADNAME", 0)):
    null = "B::SPECIAL" if kind == 0 else "B::NULL"
    edits.append((name + '\n'
                  '\tsv_setiv(newSVrv($arg, $var ? "' + cls + '" : "' + null + '"),\n'
                  '\t\t PTR2IV($var));\n',
                  name + '\n'
                  '\tsv_setiv(newSVrv($arg, $var ? "' + cls + '" : "' + null + '"),\n'
                  '\t\t GOPERL_PAD_NUMBER($var, ' + str(kind) + '));\n'))
patch("ext/B/typemap", "GOPERL_NUMBER_PAD", edits)
PYPATCH
# ---- POSIX: CHAR_MIN and CHAR_MAX are the host's ------------------------------
# char is signed where the interpreter is built (wasm) and may be unsigned
# where native code runs (aarch64 Linux), and a module that passes a char
# between the two compares with POSIX::CHAR_MIN and CHAR_MAX (FFI::Raw's
# tests). Where the host's char is unsigned (perl.cc
# goperl_host_char_unsigned) they are 0 and UCHAR_MAX, as perl has them
# there. Idempotent.
python3 - <<'PYPATCH'
path = "ext/POSIX/POSIX.xs"
t = open(path).read()
if "goperl_host_char_unsigned" in t:
    print(f"the host's CHAR_MIN and CHAR_MAX already present in {path}")
else:
    old = "INCLUDE: const-xs.inc\n"
    assert t.count(old) == 1, path
    t = t.replace(old, old +
        "\n"
        "BOOT:\n"
        "{\n"
        "    /* CHAR_MIN and CHAR_MAX are the host's (perl-wasm\n"
        "     * scripts/wasi-configure.sh) */\n"
        "    extern int goperl_host_char_unsigned(void);\n"
        "    if (goperl_host_char_unsigned()) {\n"
        "        HV *const stash = gv_stashpvs(\"POSIX\", 0);\n"
        "        static const char *const names[2] = {\"CHAR_MIN\", \"CHAR_MAX\"};\n"
        "        const IV values[2] = {0, UCHAR_MAX};\n"
        "        int i;\n"
        "        for (i = 0; stash && i < 2; i++) {\n"
        "            SV **const svp = hv_fetch(stash, names[i], (I32)strlen(names[i]), 0);\n"
        "            SV *const value = newSViv(values[i]);\n"
        "            SvREADONLY_on(value);\n"
        "            if (svp && SvROK(*svp) && !SvOBJECT(SvRV(*svp))) {\n"
        "                /* the value behind a reference (a proxy constant) */\n"
        "                SV *const old = SvRV(*svp);\n"
        "                SvRV_set(*svp, value);\n"
        "                SvREADONLY_off(old);\n"
        "                SvREFCNT_dec(old);\n"
        "            } else if (svp && isGV(*svp) && GvCV((GV *)*svp) &&\n"
        "                       CvCONST(GvCV((GV *)*svp))) {\n"
        "                /* a constant sub, made where the name was used before\n"
        "                 * the module was loaded: its value replaced */\n"
        "                CV *const cv = GvCV((GV *)*svp);\n"
        "                SV *const old = (SV *)CvXSUBANY(cv).any_ptr;\n"
        "                CvXSUBANY(cv).any_ptr = value;\n"
        "                if (old) {\n"
        "                    SvREADONLY_off(old);\n"
        "                    SvREFCNT_dec(old);\n"
        "                }\n"
        "            } else {\n"
        "                SvREFCNT_dec(value);\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n")
    open(path, "w").write(t)
    print(f"the host's CHAR_MIN and CHAR_MAX applied to {path}")
PYPATCH
# ---- Encode: a table that is no address of this memory ------------------------
# An Encode::XS object holds the address of its encode_t table. A native
# module made by enc2xs (Encode::EUCJPASCII, Encode::HanExtra) defines its
# encodings with tables of its own, in the host process: an address outside
# this memory. Encode is given a copy of the table in this memory (perl.cc
# goperl_native_encode_table). Idempotent.
python3 - <<'PYPATCH'
path = "cpan/Encode/Encode.xs"
t = open(path).read()
if "goperl_encode_table" in t:
    print(f"tables outside the memory already taken in {path}")
else:
    old = 'INT2PTR(encode_t *, SvIV(SvRV(obj)))'
    n = t.count(old)
    assert n >= 5, (path, n)
    t = t.replace(old, 'goperl_encode_table(aTHX_ obj)')
    anchor = '\nstatic void\nEncode_XSEncoding(pTHX_ encode_t * enc)\n'
    assert t.count(anchor) == 1, path
    t = t.replace(anchor,
        '\n/* The table of an Encode::XS object: one of this memory, or a copy\n'
        ' * of one native code made (perl-wasm scripts/wasi-configure.sh). */\n'
        'extern void *goperl_native_encode_table(pTHX_ UV addr);\n'
        'static encode_t *\n'
        'goperl_encode_table(pTHX_ SV *obj)\n'
        '{\n'
        '    const UV addr = (UV)SvIV(SvRV(obj));\n'
        '    if (addr >= ((UV)__builtin_wasm_memory_size(0) << 16))\n'
        '        return (encode_t *)goperl_native_encode_table(aTHX_ addr);\n'
        '    return INT2PTR(encode_t *, addr);\n'
        '}\n' + anchor)
    open(path, "w").write(t)
    print(f"tables outside the memory taken in {path} ({n} places)")
PYPATCH
# ---- $! and the status of a die are numbers the program sees ------------------
# errno holds the error numbers of this C library. A program that is the
# host process is shown the host's numbers (perl_numbers.cc), so the three
# places where errno becomes a value of the program or the reverse go
# through host_errno_of and guest_errno_of (declared in wasi-posix-shim.h,
# which every file is compiled with): reading $!, assigning to $! and to
# $^E, and the exit status of a die, which is the error number. Without host
# numbering the two return what they are given. Idempotent.
python3 - <<'PYPATCH'
def patch(path, edits):
    t = open(path).read()
    if "host_errno_of" in t or "guest_errno_of" in t:
        print(f"error numbers of the program already present in {path}")
        return
    for old, new in edits:
        assert t.count(old) == 1, (path, old[:60], t.count(old))
        t = t.replace(old, new)
    open(path, "w").write(t)
    print(f"error numbers of the program applied to {path}")

patch("mg.c", [
    ("            sv_setnv(sv, (NV)errno);\n#endif\n#ifdef OS2\n",
     "            sv_setnv(sv, (NV)host_errno_of(errno));\n#endif\n#ifdef OS2\n"),
    ("        SETERRNO(SvIOK(sv) ? SvIVX(sv) : SvOK(sv) ? sv_2iv(sv) : 0,\n"
     "                 (SvIV(sv) == EVMSERR) ? 4 : PERL_VMS_BANG);\n#endif\n        }\n",
     "        SETERRNO(guest_errno_of(SvIOK(sv) ? SvIVX(sv) : SvOK(sv) ? sv_2iv(sv) : 0),\n"
     "                 (SvIV(sv) == EVMSERR) ? 4 : PERL_VMS_BANG);\n#endif\n        }\n"),
    ("            SETERRNO(SvIV(sv), 4);\n",
     "            SETERRNO(guest_errno_of(SvIV(sv)), 4);\n"),
])
patch("perl.c", [
    ("    int eno = errno;\n    if (eno & 255)\n",
     "    int eno = host_errno_of(errno);\n    if (eno & 255)\n"),
])
PYPATCH

# ---- XSLoader, DynaLoader: linked-in extensions are found when loaded ---------
# A statically linked perl registers <Module>::bootstrap for every linked-in
# extension at start-up, and the two loaders look for that sub. This build
# registers nothing up front (a module that was never loaded must not look
# loaded; see "linked-in extensions" in perl.cc): the loaders ask
# DynaLoader::dl_static_boot, which installs the boot function of a
# linked-in extension as <Module>::bootstrap at that moment and returns it.
# The host miniperl that runs the build has no such function and loads as
# before. Idempotent.
python3 - <<'PYPATCH'
def patch(path, old, new):
    t = open(path).read()
    if "dl_static_boot" in t:
        print(f"linked-in extension lookup already present in {path}")
        return
    assert t.count(old) == 1, path
    open(path, "w").write(t.replace(old, new))
    print(f"linked-in extension lookup applied to {path}")

old = ("    my $boots = \"$module\\::bootstrap\";\n"
       "    goto &$boots if defined &$boots;\n")
patch("dist/XSLoader/XSLoader_pm.PL", old, old +
      "\n"
      "    # An extension linked into this perl: its boot function is installed\n"
      "    # under that name now, the first time the module is loaded.\n"
      "    if (defined &dl_static_boot and my $xs = dl_static_boot($module)) {\n"
      "        goto &$xs;\n"
      "    }\n")

old = ("    # A common error on platforms which don't support dynamic loading.\n"
       "    # Since it's fatal and potentially confusing we give a detailed message.\n")
patch("ext/DynaLoader/DynaLoader_pm.PL", old,
      "    # An extension linked into this perl: its boot function is installed\n"
      "    # as ${module}::bootstrap now, the first time the module is loaded.\n"
      "    if (defined &dl_static_boot and my $xs = dl_static_boot($module)) {\n"
      "        return &$xs(@args);\n"
      "    }\n"
      "\n" + old)
PYPATCH
# ---- Socket: Unix-domain addresses have Linux's lengths -----------------------
# unpack_sockaddr_un takes an address of exactly sizeof(struct sockaddr_un)
# bytes and reads its path up to the first NUL, except on the systems it
# knows to return shorter addresses. The host sockets return Linux's: an
# abstract name (one that starts with a NUL, and may hold more) ends where
# the address does, and a socket without a name has an address that ends
# before the path. The two places that single out Linux take this system
# with it. Idempotent.
python3 - <<'PYPATCH'
path = "cpan/Socket/Socket.xs"
t = open(path).read()
if "defined(__wasi__)" in t:
    print(f"Unix-domain address lengths already present in {path}")
else:
    for old, new in [
        ("#   if defined(__linux__) || defined(__CYGWIN__) || defined(sun) || defined(HAS_SOCKADDR_SA_LEN)\n",
         "#   if defined(__linux__) || defined(__wasi__) || defined(__CYGWIN__) || defined(sun) || defined(HAS_SOCKADDR_SA_LEN)\n"),
        ("#   ifdef __linux__\n"
         "        if (addr.sun_path[0] == '\\0') {\n",
         "#   if defined(__linux__) || defined(__wasi__)\n"
         "        if (addr.sun_path[0] == '\\0') {\n"),
    ]:
        assert t.count(old) == 1, (path, old)
        t = t.replace(old, new)
    open(path, "w").write(t)
    print(f"Unix-domain address lengths applied to {path}")
PYPATCH
# ---- pp_pack.c: 'p' and 'P' are addresses of the process ----------------------
# A pointer pack 'p' or 'P' makes is for native code, which reads the
# process's memory; unpack 'p' and 'P' read at such an address
# (goperl_pack_address / goperl_unpack_address in perl.cc). Idempotent.
python3 - <<'PYPATCH'
path = "pp_pack.c"
t = open(path).read()
if "goperl_pack_address" in t:
    print(f"process addresses of 'p' and 'P' already present in {path}")
else:
    for old, new in [
        ("#define PERL_IN_PP_PACK_C\n#include \"perl.h\"\n",
         "#define PERL_IN_PP_PACK_C\n#include \"perl.h\"\n\n"
         "extern const char *goperl_pack_address(const char *p);\n"
         "extern SV *goperl_unpack_address(pTHX_ const char *p, SSize_t len);\n"),
        ("                SHIFT_VAR(utf8, s, strend, aptr, datumtype, needs_swap);\n"
         "                /* newSVpv generates undef if aptr is NULL */\n"
         "                mPUSHs(newSVpv(aptr, 0));\n",
         "                SHIFT_VAR(utf8, s, strend, aptr, datumtype, needs_swap);\n"
         "                /* newSVpv generates undef if aptr is NULL */\n"
         "                mPUSHs(goperl_unpack_address(aTHX_ aptr, -1));\n"),
        ("                /* newSVpvn generates undef if aptr is NULL */\n"
         "                PUSHs(newSVpvn_flags(aptr, len, SVs_TEMP));\n",
         "                /* newSVpvn generates undef if aptr is NULL */\n"
         "                PUSHs(sv_2mortal(goperl_unpack_address(aTHX_ aptr, len)));\n"),
        ("                        aptr = SvPV_force_flags_nolen(fromstr, 0);\n"
         "                }\n"
         "                PUSH_VAR(utf8, cur, aptr, needs_swap);\n",
         "                        aptr = SvPV_force_flags_nolen(fromstr, 0);\n"
         "                }\n"
         "                aptr = goperl_pack_address(aptr);\n"
         "                PUSH_VAR(utf8, cur, aptr, needs_swap);\n"),
    ]:
        assert t.count(old) == 1, (path, old[:60], t.count(old))
        t = t.replace(old, new)
    open(path, "w").write(t)
    print(f"process addresses of 'p' and 'P' applied to {path}")
PYPATCH
# ---- pp_sys.c: syscall() is the host's ----------------------------------------
# perl's syscall() hands the host's system call its arguments: numbers, or
# for a string the address of its buffer, which is an address of this
# memory and has to become the process's. pp_syscall tells goperl_syscall
# (perl_libc.cc) which arguments are such addresses; the C library has no
# syscall() to call. Idempotent.
python3 - <<'PYPATCH'
path = "pp_sys.c"
t = open(path).read()
if "goperl_syscall" in t:
    print(f"the host's syscall already present in {path}")
else:
    for old, new in [
        ("    unsigned long a[20];\n    I32 i = 0;\n    IV retval = -1;\n",
         "    unsigned long a[20];\n    I32 i = 0;\n    IV retval = -1;\n"
         "    unsigned long goperl_addresses = 0;\n"
         "    extern long goperl_syscall(const unsigned long *a, int items, unsigned long addresses);\n"),
        ("        else\n            a[i++] = (unsigned long)SvPV_force_nolen(*MARK);\n",
         "        else {\n"
         "            goperl_addresses |= 1UL << i;\n"
         "            a[i++] = (unsigned long)SvPV_force_nolen(*MARK);\n"
         "        }\n"),
    ]:
        assert t.count(old) == 1, (path, old[:60], t.count(old))
        t = t.replace(old, new)
    start = t.index("    switch (items) {\n    default:\n        DIE(aTHX_ \"Too many args to syscall\");\n")
    end = t.index("    SP = ORIGMARK;\n    PUSHi(retval);\n", start)
    t = (t[:start] +
         "    if (items > 8)\n"
         "        DIE(aTHX_ \"Too many args to syscall\");\n"
         "    if (items == 0)\n"
         "        DIE(aTHX_ \"Too few args to syscall\");\n"
         "    retval = goperl_syscall(a, (int)items, goperl_addresses);\n" +
         t[end:])
    open(path, "w").write(t)
    print(f"the host's syscall applied to {path}")
PYPATCH
# ---- op.c: the ops whose memory goes back are reported -------------------------
# The XS bridge tells the host which of the ops it described were freed
# (perl.cc, goperl_op_released): only then can an address name another op,
# and the host's copy of the op must go. PL_opfreehook does not say it of
# every op: op_free calls it, op_destroy does not (the list op_append_list
# moved into another, the loop op newFOROP makes anew). Slab_Free, which
# both free an op through, reports the op; so does newFOROP when realloc
# moves a loop op made outside a slab. Idempotent.
python3 - <<'PYPATCH'
path = "op.c"
t = open(path).read()
if "goperl_op_released" in t:
    print(f"op release reports already present in {path}")
else:
    for old, new in [
        ("void\nPerl_Slab_Free(pTHX_ void *op)\n{\n    PERL_ARGS_ASSERT_SLAB_FREE;\n\n"
         "    OP * const o = (OP *)op;\n    OPSLAB *slab;\n",
         "extern void goperl_op_released(OP *o);\n\n"
         "void\nPerl_Slab_Free(pTHX_ void *op)\n{\n    PERL_ARGS_ASSERT_SLAB_FREE;\n\n"
         "    OP * const o = (OP *)op;\n    OPSLAB *slab;\n\n    goperl_op_released(o);\n"),
        ("        /* loop was malloc()ed */\n"
         "        loop = (LOOP*)PerlMemShared_realloc(loop, sizeof(LOOP));\n",
         "        /* loop was malloc()ed */\n"
         "        const uintptr_t was = (uintptr_t)loop;\n"
         "        loop = (LOOP*)PerlMemShared_realloc(loop, sizeof(LOOP));\n"
         "        if ((uintptr_t)loop != was) goperl_op_released((OP *)was);\n"),
    ]:
        assert t.count(old) == 1, (path, old[:60], t.count(old))
        t = t.replace(old, new)
    open(path, "w").write(t)
    print(f"op release reports applied to {path}")
PYPATCH
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
