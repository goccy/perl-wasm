# XS (C) extension selection for perl.wasm

Perl's standard library has two halves that are handled differently in the
wasm build:

- **Pure-Perl `.pm`** (including the `unicore/` Unicode tables and the
  generated `Config.pm`) is bundled into `perl_stdlib.zip`, embedded in
  go-perl, unpacked at runtime, and prepended to `@INC` via
  `perl_new(lib_dir)`. Build the zip from the *assembled* `lib/` after `make`
  (dual-life modules are copied into `lib/` during the build; the source
  `lib/` alone is incomplete).
- **XS (C) extensions** cannot be a runtime artifact: **wasi has no `dlopen`**,
  so `DynaLoader` cannot load `.so` at runtime. Every XS module we want must be
  chosen at build time and **statically linked** into `perl.wasm` via Perl's
  `static_ext` mechanism (`config.sh` `static_ext=`, plus the DynaLoader static
  bootstrap that registers each `boot_<Module>`). This is the direct parallel
  to CPython's C extensions being compiled *into* `python.wasm`.

The policy is **"link everything EXCEPT what is meaningless under wasm32-wasi"**
(OS-specific, external-library/syscall dependent, threads, or test/author-only).
Selection is against upstream Perl **v5.42.2**
(`b62845c7186b0b6a8e4e83419e6b5ef64ceef3ed`).

## Included (statically linked into perl.wasm)

### ext/
`attributes`, `B`, `Devel-Peek`, `DynaLoader`, `Fcntl`, `File-DosGlob`,
`File-Glob`, `Hash-Util`, `Hash-Util-FieldHash`, `I18N-Langinfo`, `mro`,
`Opcode`, `PerlIO-encoding`, `PerlIO-via`, `POSIX`, `re`, `SDBM_File`,
`Sys-Hostname`

### cpan/
`Compress-Raw-Zlib`, `Compress-Raw-Bzip2` (both bundle their own
zlib/bzip2 sources, so no external library is required), `Digest-MD5`,
`Digest-SHA`, `Encode`, `Filter-Util-Call`, `Math-BigInt-FastCalc`,
`MIME-Base64`, `Scalar-List-Utils`, `Socket`, `Sys-Syslog`, `Time-Piece`,
`Unicode-Collate`

### dist/
`Data-Dumper`, `IO`, `PathTools` (`Cwd` / `File::Spec`), `Storable`,
`Time-HiRes`, `Unicode-Normalize`

Notes:
- `SDBM_File` bundles its own `sdbm` implementation, so it needs no external
  DBM library and is kept.
- `Sys-Syslog`: wasi-libc has neither `<syslog.h>` nor `syslog(3)`, so the
  project supplies both (`scripts/include/syslog.h`, `perl_syslog.cc`).
  `openlog`/`syslog`/`closelog` hand each message to the host as typed
  values (the `proc_syslog*` imports); the host delivers it to a system
  logger when the embedder enabled one, and otherwise `openlog` fails. A
  configure-time patch makes `connect_native` report that failure as "this
  mechanism is unavailable", so the module falls through to its other
  mechanisms (`tcp`, `udp`, `unix`, ...) instead of dying or losing the
  message.
- `I18N-Langinfo` (`nl_langinfo`) and `Sys-Hostname` (`gethostname`) may need a
  small host-capability stub under wasi; they are kept as porting targets.
- `Socket` is included because the wasm layer enables host sockets
  (`HostSockets: true`); connection *policy* is enforced on the go-perl side.
- `Time-HiRes`: `time`/`gettimeofday`/`tv_interval`/`sleep`/`usleep`/
  `nanosleep` are the real implementations, and so are `clock_gettime` and
  `clock_getres` (`CLOCK_REALTIME`, `CLOCK_MONOTONIC`, and the process and
  thread CPU-time clocks). wasi's `clockid_t` is a struct pointer while
  Time::HiRes moves clock ids through Perl as integers, so
  `wasi-configure.sh` gives its XS integer clock ids (Linux's numbers) over
  the wasi clocks. `clock_nanosleep` is configured off and fails cleanly, so
  feature probes fall back. The interval timer (`alarm`/`ualarm`/`setitimer`/`getitimer`
  with `ITIMER_REAL`) is implemented in `perl_signal.cc` over a timer the
  host keeps; the CPU-time timers (`ITIMER_VIRTUAL`, `ITIMER_PROF`) do not
  exist.

## CPAN distributions linked in

The interpreter also links a set of XS distributions that are not part of
perl: the ones a large share of CPAN depends on, directly or through a chain
of dependencies. A distribution whose XS code only calls the documented API
can be built natively against go-perl's XS SDK and loaded at run time, but
these reach into interpreter structures (they assign through `SvCUR`-style
macros, walk pads, ops and stashes), which only code compiled against the
real perl headers can do. Linking them in is what makes everything that
depends on them usable.

They are listed in `scripts/bundled-cpan.txt`, one pinned archive with its
sha256 per line: `Clone`, `Test-LeakTrace`, `Package-Stash-XS`,
`List-MoreUtils-XS`, `Params-Util`, `HTML-Parser`, `PadWalker`, `Sub-Name`,
`Class-XSAccessor`, `Digest-SHA1`, `Text-CSV_XS`, `YAML-LibYAML`,
`Devel-Size`, `Want`, `Class-Load-XS`, `Sub-Identify`, `Set-Object`,
`Cpanel-JSON-XS`.

`scripts/wasi-configure.sh` downloads each archive (cached per user),
verifies it, unpacks it into `perl5/cpan/` and adds it to `static_ext`, so
the core's own extension machinery builds it: `make_ext.pl` runs the
distribution's unmodified `Makefile.PL`, the objects land in
`lib/auto/<Module>/<Module>.a`, and the modules land in `lib/` and from
there in `perl_stdlib.zip`. `perl.cc`'s `xs_init` registers one `boot_*` per
XS module, and the stock `XSLoader` finds it there.

Two details of that build:

- A `Makefile.PL` runs under the build's host miniperl, which has no XS
  modules. One that loads XS modules to probe the compiler
  (`List-MoreUtils-XS`, `Params-Util`: Config::AutoConf through IO::File)
  cannot run there. Such a line carries `core-makefile` in the manifest: the
  `Makefile.PL` is dropped and `make_ext.pl` writes its plain one, and what
  the original would have generated is kept in
  `scripts/bundled-cpan/<Distribution>/` (the `LMUconfig.h` of
  `List-MoreUtils-XS` with the answers for wasi-sdk).
- Some of these `Makefile.PL`s need the Unicode tables (`use charnames`), so
  the build command in `wasmify.json` makes `uni.data` before the static
  extensions.

To add a distribution: add its line to the manifest, its `boot_*`
declaration and registration to `perl.cc`, and rebuild from a clean `perl5`
tree. Only the distribution's own modules are shipped; a distribution that
needs other, pure-Perl distributions at run time (`HTML-Parser` needs
`HTML-Tagset`, `Class-Load-XS` needs `Class-Load`) gets them installed the
usual way.

`Encode` is built with all of its encoding families (`Encode::Byte`, `CN`,
`EBCDIC`, `JP`, `KR`, `Symbol`, `TW`, `Unicode`): each is an XS module of its
own inside the `Encode` distribution and has its own `boot_*` registration.

## Excluded (NOT linked) and why

| Module(s) | Reason for exclusion |
| --- | --- |
| `Amiga-ARexx`, `Amiga-Exec`, `VMS-DCLsym`, `VMS-Stdio`, `Win32`, `Win32API-File` | OS-specific (AmigaOS / VMS / Windows) — meaningless on wasm32-wasi. |
| `GDBM_File`, `NDBM_File`, `ODBM_File`, `DB_File` | Require external DBM libraries (gdbm / ndbm / Berkeley DB) that are not available in the wasi sysroot. (`SDBM_File`, which bundles its own implementation, is kept instead.) |
| `IPC-SysV` | System V IPC (`shmget` / `semget` / `msgget`) does not exist under wasi. |
| `PerlIO-mmap` | Relies on `mmap`, which wasi implements only partially. |
| `threads`, `threads-shared` | The interpreter is built without ithreads (`useithreads=undef`); the wasm target is single-threaded. |
| `XS-APItest`, `XS-Typemap` | Test-only / module-author tooling; not needed at runtime. |
| `Devel-PPPort` (the XS half) | The XS functions exist for the distribution's own tests. The module itself, `Devel/PPPort.pm`, is pure Perl and ships in the standard library: build tools require it to write `ppport.h`. The perl build step generates it from `PPPort_pm.PL` (see the `build` command in `wasmify.json`). |

If a use case later needs one of the excluded modules, the fix is either to
port the missing capability into the wasi host layer or
to build the required third-party library for wasm32-wasi and add the module to
`static_ext`.
