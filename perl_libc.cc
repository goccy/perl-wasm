/* perl_libc.cc — the POSIX functions perl refers to that neither WASI nor
 * its libc has, and that no host shim provides.
 *
 * Every one of them is defined here, because a function the link leaves
 * undefined becomes a host import that the generated binding answers with
 * 0: "success", for something that never happened — chown that changes no
 * owner, setsid that starts no session, mkstemp that hands back descriptor
 * 0. Here each either does its job with what the platform has, or fails
 * with the errno of the thing that is missing.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h> /* struct hostent */
#include <time.h>
#include <unistd.h>
#include <wasi/api.h>

/* the prototypes perl is compiled with (struct netent and friends too) */
#include "scripts/wasi-posix-shim.h"

extern "C" {

/* The path a directory descriptor was opened under, absolute and not
 * NUL-terminated: *length receives its size, also when buf (of buf_len
 * bytes) is too small for it, which is -ERANGE. 0 or a negative errno. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_path")))
int wasi_host_fd_path(int fd, char *buf, int buf_len, int *length);

} /* extern "C" */

/* What holds in a time zone at one moment, as the host reports it: the
 * offset from UTC in seconds east, whether daylight saving time is in
 * effect, the offset under the opposite daylight-saving state, the offset
 * of standard time, and the abbreviations in use, of standard time and of
 * daylight saving time (empty when the zone has none). */
struct wasi_zone {
    int32_t offset;
    int32_t is_dst;
    int32_t other_offset;
    int32_t std_offset;
    char name[16];
    char std_name[16];
    char dst_name[16];
};

extern "C" {

/* tz is the zone the program names (the value of TZ) when has_tz is set;
 * t is seconds since the epoch, or with local set a reading of the zone's
 * own clock. 0 or a negative errno. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("clock_zone")))
int wasi_host_clock_zone(const char *tz, int tz_len, int has_tz, int64_t t, int local,
                         struct wasi_zone *out);

} /* extern "C" */

namespace {

/* A struct tm points at its zone's abbreviation (tm_zone), so the
 * abbreviations a program has met stay where they are for its lifetime.
 * There are few of them: a zone has one or two, and a program names few
 * zones. When the table is full the abbreviation is not kept, and the
 * struct tm gets none. */
const size_t zone_name_limit = 64;
char *zone_names[zone_name_limit];
size_t zone_name_count = 0;

const char *zone_name(const char *name) {
    for (size_t i = 0; i < zone_name_count; i++)
        if (strcmp(zone_names[i], name) == 0) return zone_names[i];
    if (zone_name_count == zone_name_limit) return "";
    char *kept = strdup(name);
    if (!kept) return "";
    zone_names[zone_name_count++] = kept;
    return kept;
}

/* Asks the host about the zone TZ names right now. */
bool zone_at(long long t, int local, struct wasi_zone *zone) {
    const char *tz = getenv("TZ");
    return wasi_host_clock_zone(tz, tz ? (int)strlen(tz) : 0, tz != NULL, (int64_t)t, local,
                                zone) == 0;
}

int fail(int error) {
    errno = error;
    return -1;
}

/* A request to change a file's owner is granted when it changes nothing —
 * each id is -1 ("leave it") or the one the file already has — and refused
 * otherwise: the process has no privilege to give files away. */
int change_owner(const struct stat &st, uid_t owner, gid_t group) {
    if ((owner != (uid_t)-1 && owner != st.st_uid) || (group != (gid_t)-1 && group != st.st_gid))
        return fail(EPERM);
    return 0;
}

/* The protocols database: the entries every /etc/protocols starts with. */
char *no_aliases[] = {NULL};
char proto_ip[] = "ip", proto_icmp[] = "icmp", proto_tcp[] = "tcp", proto_udp[] = "udp",
     proto_ipv6[] = "ipv6", proto_icmpv6[] = "ipv6-icmp";
struct protoent protocols[] = {
    {proto_ip, no_aliases, 0},      {proto_icmp, no_aliases, 1},
    {proto_tcp, no_aliases, 6},     {proto_udp, no_aliases, 17},
    {proto_ipv6, no_aliases, 41},   {proto_icmpv6, no_aliases, 58},
};
const size_t protocol_count = sizeof(protocols) / sizeof(protocols[0]);
size_t protocol_next = 0; /* getprotoent's position */

} /* namespace */

extern "C" {

/* ---- file ownership ------------------------------------------------------ */
int chown(const char *path, uid_t owner, gid_t group) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return change_owner(st, owner, group);
}

int lchown(const char *path, uid_t owner, gid_t group) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    return change_owner(st, owner, group);
}

int fchown(int fd, uid_t owner, gid_t group) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return change_owner(st, owner, group);
}

/* ---- sockets ----------------------------------------------------------------
 * shutdown: "how" is 0, 1 or 2 (SHUT_RD, SHUT_WR, SHUT_RDWR as
 * scripts/wasi-posix-shim.h defines them, the numbers programs pass
 * literally); the WASI call takes one bit per direction. This definition
 * takes the place of wasi-libc's, which hands its argument over as the
 * bits. */
int shutdown(int fd, int how) {
    __wasi_sdflags_t flags;
    switch (how) {
    case SHUT_RD:
        flags = __WASI_SDFLAGS_RD;
        break;
    case SHUT_WR:
        flags = __WASI_SDFLAGS_WR;
        break;
    case SHUT_RDWR:
        flags = __WASI_SDFLAGS_RD | __WASI_SDFLAGS_WR;
        break;
    default:
        return fail(EINVAL);
    }
    const __wasi_errno_t error = __wasi_sock_shutdown((__wasi_fd_t)fd, flags);
    if (error != 0) return fail(error);
    return 0;
}

/* ---- temporary files ------------------------------------------------------
 * mkstemp: replaces the trailing XXXXXX of the template with characters
 * that make a name no file has, and creates that file, for this process
 * only. */
int mkstemp(char *tmpl) {
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const size_t len = tmpl ? strlen(tmpl) : 0;
    if (len < 6 || memcmp(tmpl + len - 6, "XXXXXX", 6) != 0) return fail(EINVAL);
    for (int attempt = 0; attempt < 100; attempt++) {
        unsigned char random[6];
        if (getentropy(random, sizeof(random)) != 0) return -1;
        for (int i = 0; i < 6; i++)
            tmpl[len - 6 + i] = alphabet[random[i] % (sizeof(alphabet) - 1)];
        const int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) break;
    }
    memcpy(tmpl + len - 6, "XXXXXX", 6);
    return -1;
}

/* ---- the working directory by descriptor ----------------------------------- */

/* The working directory is a path kept by libc, so a directory that is
 * known by its descriptor has to be named first. The host knows the path the
 * descriptor was opened under (fd_path). That name may lead somewhere else
 * by now — the directory was renamed, or replaced — and fchdir is about the
 * directory, not the name: when the two no longer agree the directory
 * cannot be named, and the call fails rather than go elsewhere. */
int fchdir(int fd) {
    struct stat by_descriptor;
    if (fstat(fd, &by_descriptor) != 0) return -1;
    if (!S_ISDIR(by_descriptor.st_mode)) return fail(ENOTDIR);
    char path[4096];
    int length = 0;
    const int rc = wasi_host_fd_path(fd, path, (int)sizeof(path) - 1, &length);
    if (rc != 0) return fail(rc == -ERANGE ? ENAMETOOLONG : -rc);
    path[length] = '\0';
    struct stat by_name;
    if (stat(path, &by_name) != 0) return -1;
    if (by_name.st_dev != by_descriptor.st_dev || by_name.st_ino != by_descriptor.st_ino)
        return fail(ENOENT);
    return chdir(path);
}

/* ---- what a WASI process cannot do ---------------------------------------- */

/* The root of the filesystem is the host's to set. */
int chroot(const char *path) {
    (void)path;
    return fail(EPERM);
}

/* FIFOs cannot be created through WASI. */
int mkfifo(const char *path, mode_t mode) {
    (void)path;
    (void)mode;
    return fail(ENOSYS);
}

/* There are no process groups, sessions or scheduling priorities. */
pid_t getpgid(pid_t pid) {
    (void)pid;
    return (pid_t)fail(ENOSYS);
}

int setpgid(pid_t pid, pid_t pgid) {
    (void)pid;
    (void)pgid;
    return fail(ENOSYS);
}

pid_t setsid(void) { return (pid_t)fail(ENOSYS); }

int killpg(pid_t pgrp, int sig) {
    (void)pgrp;
    (void)sig;
    return fail(ENOSYS);
}

int getpriority(int which, id_t who) {
    (void)which;
    (void)who;
    return fail(ENOSYS);
}

int setpriority(int which, id_t who, int value) {
    (void)which;
    (void)who;
    (void)value;
    return fail(ENOSYS);
}

int nice(int increment) {
    (void)increment;
    return fail(ENOSYS);
}

/* A terminal has no name here: the host hands over streams, not devices. */
char *ttyname(int fd) {
    errno = isatty(fd) ? ENODEV : ENOTTY;
    return NULL;
}

/* The login name, as getlogin has it (perl_process.cc). */
char *cuserid(char *buf) {
    static char name[64];
    const char *login = getlogin();
    if (!buf) buf = name;
    if (!login || strlen(login) >= sizeof(name)) {
        buf[0] = '\0';
        return NULL;
    }
    strcpy(buf, login);
    return buf;
}

/* ---- time zones ------------------------------------------------------------
 * wasi-libc has one time zone, UTC: its localtime, mktime and strftime ask
 * two internal functions what holds in "the" zone, and its own answers are
 * constant. The definitions below take their place (the object that holds
 * libc's is then not linked), so that the same libc functions follow TZ:
 * the zone's rules are the host's to know (the clock_zone import), named by
 * the value TZ has at the moment of the call — a program may change
 * $ENV{TZ} at any time. With TZ unset the host decides where the program
 * is. tzset, tzname, timezone and daylight, which wasi-libc leaves out, are
 * defined on the same answer. */

/* libc's name for UTC, which gmtime gives tm_zone. (Declared first: a
 * const object has to be declared extern to be visible to libc.) */
extern const char __utc[];
const char __utc[] = "UTC";

char *tzname[2] = {(char *)__utc, (char *)__utc};
long timezone = 0;
int daylight = 0;

/* What holds in the zone at t; libc's contract. With local set, t is a
 * reading of the zone's clock (mktime's starting point) and *oppoff
 * receives the offset under the opposite daylight-saving state. */
void __secs_to_zone(long long t, int local, int *isdst, long *offset, long *oppoff,
                    const char **zonename) {
    struct wasi_zone zone;
    if (!zone_at(t, local, &zone)) {
        *isdst = 0;
        *offset = 0;
        if (oppoff) *oppoff = 0;
        *zonename = __utc;
        return;
    }
    *isdst = zone.is_dst;
    *offset = zone.offset;
    if (oppoff) *oppoff = zone.other_offset;
    *zonename = zone_name(zone.name);
}

/* The abbreviation a struct tm carries, for strftime's %Z: tm_zone when it
 * is one this file (or gmtime) handed out, and nothing for a struct tm that
 * was filled in by hand. */
const char *__tm_to_tzname(const struct tm *tm) {
    const char *name = tm->tm_zone;
    if (name == __utc) return name;
    for (size_t i = 0; i < zone_name_count; i++)
        if (name == zone_names[i]) return name;
    return "";
}

void tzset(void) {
    struct wasi_zone zone;
    if (!zone_at((long long)time(NULL), 0, &zone)) return;
    const char *standard = zone_name(zone.std_name);
    tzname[0] = (char *)standard;
    tzname[1] = (char *)(zone.dst_name[0] ? zone_name(zone.dst_name) : standard);
    timezone = -(long)zone.std_offset;
    daylight = zone.dst_name[0] != 0;
}

/* ---- network databases -----------------------------------------------------
 * Hosts: lookups by name go to the host (gethostbyname, in the socket
 * shim); there is no table to walk. */
void sethostent(int stayopen) { (void)stayopen; }
void endhostent(void) {}
struct hostent *gethostent(void) { return NULL; }

/* Networks: no database. */
void setnetent(int stayopen) { (void)stayopen; }
void endnetent(void) {}
struct netent *getnetent(void) { return NULL; }
struct netent *getnetbyname(const char *name) {
    (void)name;
    return NULL;
}
struct netent *getnetbyaddr(uint32_t net, int type) {
    (void)net;
    (void)type;
    return NULL;
}

/* Services: no database. Callers name a port by number. */
void setservent(int stayopen) { (void)stayopen; }
void endservent(void) {}
struct servent *getservent(void) { return NULL; }
struct servent *getservbyname(const char *name, const char *proto) {
    (void)name;
    (void)proto;
    return NULL;
}
struct servent *getservbyport(int port, const char *proto) {
    (void)port;
    (void)proto;
    return NULL;
}

/* Protocols: the table above. */
void setprotoent(int stayopen) {
    (void)stayopen;
    protocol_next = 0;
}
void endprotoent(void) { protocol_next = 0; }
struct protoent *getprotoent(void) {
    return protocol_next < protocol_count ? &protocols[protocol_next++] : NULL;
}
struct protoent *getprotobyname(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < protocol_count; i++)
        if (strcmp(protocols[i].p_name, name) == 0) return &protocols[i];
    return NULL;
}
struct protoent *getprotobynumber(int number) {
    for (size_t i = 0; i < protocol_count; i++)
        if (protocols[i].p_proto == number) return &protocols[i];
    return NULL;
}

} /* extern "C" */
