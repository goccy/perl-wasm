/* wasi-posix-shim.h — prototypes for the handful of POSIX functions that
 * neither wasi-libc nor wasmify's host-capability stub headers declare, but
 * which Perl's doio.c / pp_sys.c reference.
 *
 * Most guarded-out POSIX (sockets, netdb, exec, kill, dup, alarm, getgroups,
 * mkstemp, sigaction, wait/waitpid, ...) is supplied by wasmify's POSIX-compat /
 * host-subprocess stub headers, which `wasmify build`/`wasm-build` inject via
 * -isystem when wasmify.json opts into HostSockets/HostSubprocess. This file
 * only fills the small remaining gap. Force-included via -include in cflags.
 * libperl.a is an archive, so the symbols stay unresolved until the perl.wasm
 * link supplies host shims or ENOSYS stubs.
 */
#ifndef PERLWASM_WASI_POSIX_SHIM_H
#define PERLWASM_WASI_POSIX_SHIM_H

#include <sys/types.h> /* pid_t, uid_t, gid_t, id_t */
#include <sys/time.h>  /* struct timeval (futimes) */
#include <stdint.h>    /* uint32_t (netent) */
#include <fcntl.h>     /* open + O_EXCL/O_APPEND/... (util.c uses them directly) */

/* fenv rounding modes: wasm has no runtime rounding-mode control, so wasi's
 * <fenv.h> defines only FE_TONEAREST (0) and omits the other three. POSIX.xs
 * references FE_TOWARDZERO/FE_DOWNWARD/FE_UPWARD in a switch on fegetround(),
 * so they must at least be declared to compile. Give them the standard musl
 * values (distinct from FE_TONEAREST); they are never actually selected because
 * wasi's fegetround() always returns FE_TONEAREST, so POSIX::rint et al. round
 * to nearest, which is the only mode wasm implements. */
/* wasi's clock ids are STRUCT POINTERS (clockid_t = const struct __clockid *,
 * CLOCK_REALTIME = &_CLOCK_REALTIME), while perl-world code moves clock ids
 * through IVs — generated constant tables (ExtUtils::Constant's const-c.inc
 * in Time::HiRes) assign the macros to IV, and feature probes assign them to
 * clockid_t. Replace the pointer macros with the conventional integer ids
 * so both idioms compile. An integer id never reaches wasi-libc: the one
 * caller of the clock functions, Time::HiRes, finds the wasi clock of an id
 * first (the HiRes.xs patch in wasi-configure.sh). CLOCK_REALTIME must be 0:
 * feature probes assign it to clockid_t, and 0 is the one integer that is
 * also a valid null pointer constant. */
#include <time.h>
#undef CLOCK_REALTIME
#undef CLOCK_MONOTONIC
#undef CLOCK_PROCESS_CPUTIME_ID
#undef CLOCK_THREAD_CPUTIME_ID
#undef CLOCK_MONOTONIC_RAW
#undef CLOCK_REALTIME_COARSE
#undef CLOCK_MONOTONIC_COARSE
#undef CLOCK_BOOTTIME
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
#define CLOCK_MONOTONIC_RAW 4

#include <fenv.h>
#ifndef FE_DOWNWARD
#define FE_DOWNWARD   0x400
#endif
#ifndef FE_UPWARD
#define FE_UPWARD     0x800
#endif
#ifndef FE_TOWARDZERO
#define FE_TOWARDZERO 0xc00
#endif

/* UNIX-domain sockets: wasi-libc ships a MINIMAL `struct sockaddr_un` with only
 * sun_family (its <sys/un.h> comment: "WASI has no UNIX-domain sockets"), but
 * Socket.xs references sun_path. Pull in sa_family_t via <sys/socket.h>, then
 * provide the full BSD struct and suppress wasi's two definers (the sys/un.h
 * include guard and the __struct_sockaddr_un.h guard) so there is no conflicting
 * redefinition. AF_UNIX then COMPILES; whether it works is up to the host socket
 * layer (go-perl) — otherwise the call fails at runtime, which is correct. */
#include <sys/socket.h>
#ifndef _SYS_UN_H
#define _SYS_UN_H
#define __wasilibc___struct_sockaddr_un_h
struct sockaddr_un {
	sa_family_t sun_family;
	char        sun_path[108];
};
#endif

/* sigprocmask how-arg constants: wasi-libc guards them out; wasmify's signal
 * stub omits them. Standard musl/Linux values. */
#ifndef SIG_BLOCK
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif

/* Socket constants wasi-libc omits but Socket.xs bakes into its constant
 * table when defined — and pure-Perl networking code imports at compile time:
 * IO::Socket::INET does `use Socket qw(... SOCK_RAW)` (its icmp socket type),
 * so a missing macro kills the import of IO::Socket, IO::Socket::IP, and
 * everything above them (Mojolicious, Plack middlewares, LWP). Standard
 * musl/Linux values; actually OPENING a raw/seqpacket socket still fails at
 * socket() time under the host socket layer, which is the correct place. */
#ifndef SOCK_RAW
#define SOCK_RAW 3
#endif
#ifndef SOCK_SEQPACKET
#define SOCK_SEQPACKET 5
#endif
#ifndef SOMAXCONN
#define SOMAXCONN 128
#endif

/* shutdown(2)'s "how": 0 stops reads, 1 writes, 2 both, on every system
 * perl runs on, and programs pass the numbers themselves
 * (`shutdown($sock, 1)`, documented that way in perlfunc). wasi-libc gives
 * the three names the bits of the WASI call instead (1, 2 and 3), so that
 * a literal 1 stopped reads and a 0 was refused. The names get the usual
 * numbers here, and shutdown() itself (perl_libc.cc) translates them. */
#undef SHUT_RD
#undef SHUT_WR
#undef SHUT_RDWR
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

/* poll(2) event bits wasi-libc leaves out. Programs name them when they
 * build an event mask (POE's IO::Poll loop asks for POLLRDBAND with every
 * read), so the names have to exist; the bits are free ones of this C
 * library's numbering. No descriptor here has out-of-band or priority-band
 * data, so poll never reports them. POLLNORM is the historical name of
 * POLLRDNORM. */
#include <poll.h>
#ifndef POLLRDBAND
#define POLLRDBAND 0x0080
#endif
#ifndef POLLWRBAND
#define POLLWRBAND 0x0100
#endif
#ifndef POLLNORM
#define POLLNORM POLLRDNORM
#endif

/* SO_ACCEPTCONN ("is this socket listening?", read only). wasi-libc has no
 * number for it; the host socket layer answers it (wasmify's socket shim
 * uses this number when the C library has none). */
#ifndef SO_ACCEPTCONN
#define SO_ACCEPTCONN 30
#endif

#ifdef __cplusplus
extern "C" {
#endif

mode_t umask(mode_t);
/* The real-time interval timer. wasi-libc guards setitimer, getitimer and
 * their types out of <sys/time.h> ("WASI has no getitimer") and shows ualarm
 * only to _GNU_SOURCE code; perl_signal.cc implements all three on the timer
 * the host keeps, the one alarm() sets. ITIMER_REAL is the only timer there
 * is: the process CPU-time timers (ITIMER_VIRTUAL, ITIMER_PROF) have nothing
 * to count, and stay undefined so that nothing offers them. */
#ifndef ITIMER_REAL
#define ITIMER_REAL 0
struct itimerval {
	struct timeval it_interval;
	struct timeval it_value;
};
#endif
int getitimer(int, struct itimerval *);
int setitimer(int, const struct itimerval *, struct itimerval *);
unsigned ualarm(unsigned, unsigned);
/* wasi-libc keeps futimes out of <sys/time.h> and out of the library as a
 * legacy function; wasmify's host-fs shim defines it over futimens. */
int futimes(int, const struct timeval [2]);
pid_t getppid(void);
int execl(const char *, const char *, ...);
int killpg(pid_t, int);
int setpriority(int, id_t, int);
int getpriority(int, id_t);
int pause(void);
int setreuid(uid_t, uid_t);
int setregid(gid_t, gid_t);
int setpgid(pid_t, pid_t);
pid_t getpgid(pid_t);
pid_t setsid(void);
int chown(const char *, uid_t, gid_t);
int fchown(int, uid_t, gid_t);
int fchdir(int);
int chroot(const char *);

/* POSIX functions POSIX.xs references but wasi-libc does not declare (no tty,
 * no timezone DB, no FIFOs, no process priority, no lchown). Declared so POSIX
 * compiles; they stay unresolved in libperl.a until the perl.wasm link supplies
 * host shims or ENOSYS stubs. ttyname returns char* (matches POSIX.c usage). */
void tzset(void);
int nice(int);
int mkfifo(const char *, mode_t);
char *ttyname(int);
int lchown(const char *, uid_t, gid_t);

/* The netdb ITERATION / by-net / by-serv / by-proto lookups: wasmify's netdb.h
 * stub supplies struct hostent + gethostbyname/gethostbyaddr, but not these.
 * Perl uses the system prototypes (d_*protos=define in config) so it does not
 * redeclare them — supply the structs + prototypes here. struct hostent /
 * addrinfo come from wasmify's netdb.h; do not redefine them. */
struct netent {
	char         *n_name;
	char        **n_aliases;
	int           n_addrtype;
	uint32_t      n_net;
};
struct servent {
	char  *s_name;
	char **s_aliases;
	int    s_port;
	char  *s_proto;
};
struct protoent {
	char  *p_name;
	char **p_aliases;
	int    p_proto;
};
struct hostent  *gethostent(void);
struct netent   *getnetent(void);
struct netent   *getnetbyname(const char *);
struct netent   *getnetbyaddr(uint32_t, int);
struct servent  *getservent(void);
struct servent  *getservbyname(const char *, const char *);
struct servent  *getservbyport(int, const char *);
struct protoent *getprotoent(void);
struct protoent *getprotobyname(const char *);
struct protoent *getprotobynumber(int);

/* Iteration control for the network databases (not in wasmify's netdb.h). */
void sethostent(int);
void endhostent(void);
void setnetent(int);
void endnetent(void);
void setservent(int);
void endservent(void);
void setprotoent(int);
void endprotoent(void);

/* User / group databases: wasi has no pwd.h / grp.h. Perl's pp_gpwent /
 * pp_ggrent use these unconditionally, so supply the structs + prototypes
 * (d_*_r_proto=undef, d_getpwent/getgrent enabled in config). */
struct passwd {
	char  *pw_name;
	char  *pw_passwd;
	uid_t  pw_uid;
	gid_t  pw_gid;
	char  *pw_gecos;
	char  *pw_dir;
	char  *pw_shell;
};
struct group {
	char  *gr_name;
	char  *gr_passwd;
	gid_t  gr_gid;
	char **gr_mem;
};
/* getpwnam / getgrnam are declared by Perl itself (pp_sys.c), so omit them
 * here to avoid a conflicting redeclaration. */
struct passwd *getpwuid(uid_t);
struct passwd *getpwent(void);
void setpwent(void);
void endpwent(void);
struct group *getgrgid(gid_t);
struct group *getgrent(void);
void setgrent(void);
void endgrent(void);

#ifdef __cplusplus
}
#endif

#endif /* PERLWASM_WASI_POSIX_SHIM_H */

/* ---- fork-free piped opens (PERL_WASI_SPAWN) -----------------------------
 * WASI has no fork: the runtime's subprocess support is posix_spawn-shaped
 * (wasmify HostSubprocess: posix_spawn + waitpid + pipe over host imports).
 * The perl build defines PERL_WASI_SPAWN and routes my_popen/my_popen_list
 * into these spawn-based implementations (perl.cc); my_pclose keeps perl's
 * own implementation, which only needs waitpid. */
#ifdef PERL_WASI_SPAWN
/* pp_system's non-fork branch calls do_spawn/do_aspawn, which embed.h maps
 * to the Perl_ names below; the bridge (perl.cc) implements them over
 * posix_spawn. Opaque pointer types here (callers pass SV pointers and
 * stack slots). */
int do_spawn(char *cmd);
int do_aspawn(void *really, void **mark, void **sp);
#endif

