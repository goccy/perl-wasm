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
`MIME-Base64`, `Scalar-List-Utils`, `Socket`, `Time-Piece`, `Unicode-Collate`

### dist/
`Data-Dumper`, `IO`, `PathTools` (`Cwd` / `File::Spec`), `Storable`,
`Unicode-Normalize`

Notes:
- `SDBM_File` bundles its own `sdbm` implementation, so it needs no external
  DBM library and is kept.
- `I18N-Langinfo` (`nl_langinfo`) and `Sys-Hostname` (`gethostname`) may need a
  small host-capability stub under wasi; they are kept as porting targets.
- `Socket` is included because the wasm layer enables host sockets
  (`HostSockets: true`); connection *policy* is enforced on the go-perl side.

## Excluded (NOT linked) and why

| Module(s) | Reason for exclusion |
| --- | --- |
| `Amiga-ARexx`, `Amiga-Exec`, `VMS-DCLsym`, `VMS-Stdio`, `Win32`, `Win32API-File` | OS-specific (AmigaOS / VMS / Windows) — meaningless on wasm32-wasi. |
| `GDBM_File`, `NDBM_File`, `ODBM_File`, `DB_File` | Require external DBM libraries (gdbm / ndbm / Berkeley DB) that are not available in the wasi sysroot. (`SDBM_File`, which bundles its own implementation, is kept instead.) |
| `IPC-SysV` | System V IPC (`shmget` / `semget` / `msgget`) does not exist under wasi. |
| `PerlIO-mmap` | Relies on `mmap`, which wasi implements only partially. |
| `Sys-Syslog` | There is no `syslog` facility under wasi. |
| `Time-HiRes` | wasi's `clockid_t` is `const struct __clockid *` (a struct pointer; `CLOCK_REALTIME` = `&_CLOCK_REALTIME`), but Time::HiRes passes clock ids through Perl as integers, so `clock_gettime`/`clock_getres` are type-incompatible; `setitimer`/`getitimer` / interval timers also do not exist under wasi. Core `time()`/`sleep` and `POSIX`'s clock interfaces cover the common cases. |
| `threads`, `threads-shared` | The interpreter is built without ithreads (`useithreads=undef`); the wasm target is single-threaded. |
| `XS-APItest`, `XS-Typemap`, `Devel-PPPort` | Test-only / module-author tooling; not needed at runtime. |

If a use case later needs one of the excluded modules, the fix is either to
port the missing capability into the wasi host layer (e.g. a `syslog` shim) or
to build the required third-party library for wasm32-wasi and add the module to
`static_ext`.
