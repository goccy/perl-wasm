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
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h> /* struct hostent */
#include <sys/resource.h>
#include <sys/utsname.h>
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

/* The owner of a file, by a path relative to the root (follow 0: of a
 * symbolic link itself) and by descriptor; an id of -1 stays as it is.
 * 0 or a negative errno, -ENOSYS where files have no owners to change. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("path_chown")))
int wasi_host_path_chown(const char *path, int len, int uid, int gid, int follow);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_chown")))
int wasi_host_fd_chown(int fd, int uid, int gid);

/* Creates a FIFO at a path relative to the root. 0 or a negative errno,
 * -ENOSYS where the filesystem has none. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("path_mkfifo")))
int wasi_host_path_mkfifo(const char *path, int len, unsigned int mode);

/* Process groups and sessions, which the host keeps: the group of process
 * pid (0: the caller); moving pid (0: the caller, or a child of it) into
 * group pgid (0: one of its own); starting a session. 0 or a negative
 * errno. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_getpgid")))
int wasi_host_proc_getpgid(int pid, int *pgid_out);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_setpgid")))
int wasi_host_proc_setpgid(int pid, int pgid);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_setsid")))
int wasi_host_proc_setsid(int *sid_out);

/* The nice value of processes: which and who as getpriority takes them.
 * With set non-zero the value is changed first; the value then in effect is
 * written at nice_out. 0 or a negative errno, -ENOSYS for a process that is
 * not a process of the host. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_priority")))
int wasi_host_proc_priority(int which, int who, int set, int value, int *nice_out);

/* What uname reports, for a process that is the host process: five strings
 * (a u32 length, then the bytes), the system's name, the node's name, the
 * release, the version and the machine type. *needed receives the size of
 * the record, also when buf is too small (-ERANGE). -ENOSYS otherwise. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_uname")))
int wasi_host_proc_uname(void *buf, int buf_len, uint32_t *needed);

/* Reports a change of the environment: name is set to value, or removed
 * when has_value is 0. The host process follows when the program is it. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_setenv")))
int wasi_host_proc_setenv(const char *name, int name_len, const char *value, int value_len,
                          int has_value);

/* An entry of the host's services database, by name (by_port 0) or by
 * port, for the protocol proto when it is not empty: the port (u32), the
 * name, the protocol, the number of aliases (u32) and the aliases — strings
 * as a u32 length and the bytes. -ENOENT when there is no such service. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("sock_service")))
int wasi_host_sock_service(const char *name, int name_len, int port, int by_port,
                           const char *proto, int proto_len, void *buf, int buf_len,
                           uint32_t *needed);

int __real_setenv(const char *name, const char *value, int overwrite);
int __real_unsetenv(const char *name);
int __real_putenv(char *string);
int __real_clearenv(void);

extern char **environ;

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

/* Where files have no owners to change (the host says ENOSYS), a request
 * to change a file's owner is granted when it changes nothing — each id is
 * -1 ("leave it") or the one the file already has — and refused otherwise:
 * the process has no privilege to give files away. */
int change_owner(const struct stat &st, uid_t owner, gid_t group) {
    if ((owner != (uid_t)-1 && owner != st.st_uid) || (group != (gid_t)-1 && group != st.st_gid))
        return fail(EPERM);
    return 0;
}

/* A path as the host imports take it: absolute, without the leading
 * slash. Returns NULL with errno set when the path cannot be named. */
const char *root_relative(const char *path, char *buf, size_t size) {
    if (!path) {
        errno = EFAULT;
        return NULL;
    }
    if (!*path) {
        errno = ENOENT;
        return NULL;
    }
    const char *p = path;
    if (*p != '/') {
        char cwd[3584];
        if (!getcwd(cwd, sizeof(cwd))) return NULL;
        if (snprintf(buf, size, "%s/%s", cwd, path) >= (int)size) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        p = buf;
    }
    while (*p == '/') p++;
    return p;
}

int change_path_owner(const char *path, uid_t owner, gid_t group, int follow) {
    char buf[4096];
    const char *p = root_relative(path, buf, sizeof(buf));
    if (!p) return -1;
    const int rc = wasi_host_path_chown(p, (int)strlen(p), (int)owner, (int)group, follow);
    if (rc == 0) return 0;
    if (rc != -ENOSYS) return fail(-rc);
    struct stat st;
    if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) return -1;
    return change_owner(st, owner, group);
}

/* The fields of uname as the host reports them; false when the process is
 * a system of its own. */
bool host_uname(struct utsname *out) {
    char rec[2048];
    uint32_t needed = 0;
    if (wasi_host_proc_uname(rec, (int)sizeof(rec), &needed) != 0) return false;
    char *fields[5] = {out->sysname, out->nodename, out->release, out->version, out->machine};
    const size_t sizes[5] = {sizeof(out->sysname), sizeof(out->nodename), sizeof(out->release),
                             sizeof(out->version), sizeof(out->machine)};
    uint32_t pos = 0;
    for (int i = 0; i < 5; i++) {
        uint32_t n = 0;
        if (needed - pos < 4) return false;
        memcpy(&n, rec + pos, 4);
        pos += 4;
        if (needed - pos < n) return false;
        const size_t kept = n < sizes[i] - 1 ? n : sizes[i] - 1;
        memcpy(fields[i], rec + pos, kept);
        fields[i][kept] = '\0';
        pos += n;
    }
    return true;
}

/* A lookup in the host's services database. */
struct servent *find_service(const char *name, int port, int by_port, const char *proto) {
    static struct servent ent;
    static char rec[2048];
    static char *aliases[32];
    if (!proto) proto = "";
    uint32_t needed = 0;
    if (wasi_host_sock_service(name, (int)strlen(name), port, by_port, proto, (int)strlen(proto),
                               rec, (int)sizeof(rec), &needed) != 0)
        return NULL;
    /* The record: u32 port, then strings as a u32 length and the bytes —
     * the name, the protocol — a u32 count and the aliases. Each string is
     * moved down onto its length, which makes room for its terminator. */
    uint32_t pos = 0, value = 0;
    auto number = [&](uint32_t *out) {
        if (needed - pos < 4) return false;
        memcpy(out, rec + pos, 4);
        pos += 4;
        return true;
    };
    auto string = [&]() -> char * {
        uint32_t n = 0;
        if (!number(&n) || needed - pos < n) return NULL;
        char *s = rec + pos - 4;
        memmove(s, rec + pos, n);
        s[n] = '\0';
        pos += n;
        return s;
    };
    if (!number(&value)) return NULL;
    ent.s_port = (int)htons((uint16_t)value);
    ent.s_name = string();
    ent.s_proto = string();
    uint32_t count = 0;
    if (!ent.s_name || !ent.s_proto || !number(&count)) return NULL;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; i++) {
        char *alias = string();
        if (!alias) return NULL;
        if (kept + 1 < sizeof(aliases) / sizeof(aliases[0])) aliases[kept++] = alias;
    }
    aliases[kept] = NULL;
    ent.s_aliases = aliases;
    return &ent;
}

void report_env(const char *name, size_t name_len, const char *value) {
    wasi_host_proc_setenv(name, (int)name_len, value ? value : "",
                          value ? (int)strlen(value) : 0, value != NULL);
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
    return change_path_owner(path, owner, group, 1);
}

int lchown(const char *path, uid_t owner, gid_t group) {
    return change_path_owner(path, owner, group, 0);
}

int fchown(int fd, uid_t owner, gid_t group) {
    const int rc = wasi_host_fd_chown(fd, (int)owner, (int)group);
    if (rc == 0) return 0;
    if (rc != -ENOSYS) return fail(-rc);
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return change_owner(st, owner, group);
}

/* ---- the environment --------------------------------------------------------
 * The environment is libc's own table. Every change to it goes through
 * these (the link wraps the libc functions), which pass it on to the host
 * once libc has made it. */
int __wrap_setenv(const char *name, const char *value, int overwrite) {
    const int rc = __real_setenv(name, value, overwrite);
    if (rc == 0) report_env(name, strlen(name), getenv(name));
    return rc;
}

int __wrap_unsetenv(const char *name) {
    const int rc = __real_unsetenv(name);
    if (rc == 0) report_env(name, strlen(name), NULL);
    return rc;
}

int __wrap_putenv(char *string) {
    const int rc = __real_putenv(string);
    if (rc == 0) {
        const char *eq = strchr(string, '=');
        if (eq)
            report_env(string, (size_t)(eq - string), eq + 1);
        else
            report_env(string, strlen(string), NULL);
    }
    return rc;
}

int __wrap_clearenv(void) {
    /* the names first: they are gone once libc has cleared the table */
    if (environ) {
        for (char **e = environ; *e; e++) {
            const char *eq = strchr(*e, '=');
            report_env(*e, eq ? (size_t)(eq - *e) : strlen(*e), NULL);
        }
    }
    return __real_clearenv();
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

/* FIFOs are the host filesystem's to create; WASI has no call for it. */
int mkfifo(const char *path, mode_t mode) {
    char buf[4096];
    const char *p = root_relative(path, buf, sizeof(buf));
    if (!p) return -1;
    const int rc = wasi_host_path_mkfifo(p, (int)strlen(p), (unsigned int)mode);
    if (rc != 0) return fail(-rc);
    return 0;
}

/* ---- process groups and sessions ---------------------------------------------
 * The host keeps them: the processes of a program are its to know. */
pid_t getpgid(pid_t pid) {
    int pgid = 0;
    const int rc = wasi_host_proc_getpgid((int)pid, &pgid);
    if (rc != 0) return (pid_t)fail(-rc);
    return (pid_t)pgid;
}

pid_t getpgrp(void) { return getpgid(0); }

int setpgid(pid_t pid, pid_t pgid) {
    const int rc = wasi_host_proc_setpgid((int)pid, (int)pgid);
    if (rc != 0) return fail(-rc);
    return 0;
}

pid_t setsid(void) {
    int sid = 0;
    const int rc = wasi_host_proc_setsid(&sid);
    if (rc != 0) return (pid_t)fail(-rc);
    return (pid_t)sid;
}

int killpg(pid_t pgrp, int sig) {
    if (pgrp < 0) return fail(EINVAL);
    /* group 0 is the caller's own, as kill has it */
    return kill(pgrp > 1 ? -pgrp : 0, sig);
}

/* ---- scheduling priority ---------------------------------------------------
 * A process of the host has the host process's; an isolated one has none
 * (ENOSYS). */
int getpriority(int which, id_t who) {
    int nice_value = 0;
    const int rc = wasi_host_proc_priority(which, (int)who, 0, 0, &nice_value);
    if (rc != 0) return fail(-rc);
    errno = 0; /* -1 is a legitimate value */
    return nice_value;
}

int setpriority(int which, id_t who, int value) {
    int nice_value = 0;
    const int rc = wasi_host_proc_priority(which, (int)who, 1, value, &nice_value);
    if (rc != 0) return fail(-rc);
    return 0;
}

int nice(int increment) {
    const int process = 0; /* PRIO_PROCESS, which wasi-libc does not define */
    int nice_value = 0;
    int rc = wasi_host_proc_priority(process, 0, 0, 0, &nice_value);
    if (rc == 0)
        rc = wasi_host_proc_priority(process, 0, 1, nice_value + increment, &nice_value);
    if (rc != 0) return fail(rc == -EACCES ? EPERM : -rc);
    errno = 0; /* -1 is a legitimate value */
    return nice_value;
}

/* ---- the system's name --------------------------------------------------------
 * A process that is the host process is on the host's system: uname and
 * gethostname (which libc answers from uname) report the host's. An
 * isolated one is a system of its own, with the names wasi-libc gives. */
int uname(struct utsname *out) {
    if (!out) return fail(EFAULT);
    if (host_uname(out)) return 0;
    strcpy(out->sysname, "wasi");
    strcpy(out->nodename, "(none)");
    strcpy(out->release, "0.0.0");
    strcpy(out->version, "0.0.0");
    strcpy(out->machine, "wasm32");
    return 0;
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

/* Services: lookups by name and by port go to the host, which has the
 * database when the process is the host process; there is no table to
 * walk. The result lives in static storage, as the interface has it. */
void setservent(int stayopen) { (void)stayopen; }
void endservent(void) {}
struct servent *getservent(void) { return NULL; }
struct servent *getservbyname(const char *name, const char *proto) {
    if (!name) return NULL;
    return find_service(name, 0, 0, proto);
}
struct servent *getservbyport(int port, const char *proto) {
    /* the port is in network byte order, as in a sockaddr */
    return find_service("", (int)ntohs((uint16_t)port), 1, proto);
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
