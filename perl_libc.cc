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
#include <cstddef>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <locale.h>
#include <string>
#include <strings.h>
#include <arpa/inet.h>
#include <netdb.h> /* struct hostent */
#include <sys/resource.h>
#include <sys/utsname.h>
#include "scripts/include/termios.h"
#include <time.h>
#include <unistd.h>
#include <vector>
#include <poll.h>
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

/* Terminals, which are the host's: the settings of the terminal fd, read
 * into a struct termios and set from one (actions: the host's TCSANOW,
 * TCSADRAIN or TCSAFLUSH); the input (output 0) or output speed of
 * settings, written at was and, with set non-zero, then made speed;
 * tcdrain, tcflow, tcflush and tcsendbreak (what 0, 1, 2, 3, arg being the
 * action, the queue or the duration); the foreground process group of the
 * terminal, written at was and, with set non-zero, then made pgrp; and the
 * path of the terminal (len bytes of it at buf, which holds cap; -ERANGE
 * when it does not fit). All in the host's numbers; 0 or a negative errno,
 * -ENOTTY for a descriptor that is no terminal of the host's. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("tty_getattr")))
int wasi_host_tty_getattr(int fd, struct termios *settings);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("tty_setattr")))
int wasi_host_tty_setattr(int fd, int actions, const struct termios *settings);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("tty_speed")))
int wasi_host_tty_speed(struct termios *settings, int output, int set, unsigned int speed,
                        unsigned int *was);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("tty_control")))
int wasi_host_tty_control(int fd, int what, int arg);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("tty_pgrp")))
int wasi_host_tty_pgrp(int fd, int set, int pgrp, int *was);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("tty_name")))
int wasi_host_tty_name(int fd, char *buf, int cap, int *len);

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
char *__real_setlocale(int category, const char *name);

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

/* ---- locales ----------------------------------------------------------------
 * The C library has the data of two locales: "C" (also named "POSIX") and
 * "C.UTF-8", which is "C" with UTF-8 as the character encoding. Its
 * setlocale nevertheless accepts every name, keeps it, and behaves as
 * "C.UTF-8": a program that asked for "de_DE" was told it had it, and then
 * found the decimal point and the currency of "C". Here a locale that is
 * asked for by name is one of the two, or the call fails, as it does on a
 * system where the locale is not installed.
 *
 * The empty name asks for the locale of the environment (LC_ALL, the
 * category's own variable, LANG, in that order; an environment that names
 * no locale means "C"). A program that asks for it is answered like one
 * that asks by name: when the environment names a locale there is no data
 * for, the call fails and the locale stays as it was. (A program finds out
 * that way whether the user's locale is there: `setlocale(LC_ALL, "") or
 * skip`.)
 *
 * The interpreter asks the same question when it starts, and there a
 * failure would be a complaint on standard error at every start for a user
 * whose environment names, say, "ja_JP.UTF-8". What the user's environment
 * names is not a request the program made for the conventions of that
 * locale: at start-up the nearest locale there is data for is taken, which
 * is the one with the character encoding the environment names: "C.UTF-8"
 * for a name whose codeset is UTF-8, "C" for any other. */

/* Set while the interpreter sets its locale up when it is constructed. The
 * C library cannot tell that call of setlocale(LC_ALL, "") from a
 * program's. */
static bool locale_start_up;

void perl_wasm_locale_start_up(int on) { locale_start_up = on != 0; }

/* Whether the codeset of a locale name (language[_territory][.codeset]
 * [@modifier], the form of X/Open) is UTF-8, in either of its spellings. */
static bool locale_codeset_is_utf8(const char *name) {
    const char *dot = strchr(name, '.');
    if (!dot) return false;
    const char *codeset = dot + 1;
    const size_t len = strcspn(codeset, "@");
    return (len == 5 && strncasecmp(codeset, "UTF-8", 5) == 0) ||
           (len == 4 && strncasecmp(codeset, "UTF8", 4) == 0);
}

/* The name the C library knows a locale it has data for by, given one name
 * (of len characters) a program may use for it; NULL for every other
 * locale. */
static const char *locale_with_data(const char *name, size_t len) {
    static const struct {
        const char *name;
        const char *known_as;
    } locales[] = {
        {"C", "C"},           {"POSIX", "POSIX"},   {"C.UTF-8", "C.UTF-8"},
        {"C.utf8", "C.UTF-8"}, {"C.UTF8", "C.UTF-8"}, {"C.utf-8", "C.UTF-8"},
    };
    for (const auto &l : locales)
        if (strlen(l.name) == len && memcmp(l.name, name, len) == 0) return l.known_as;
    return NULL;
}

/* The six categories setlocale(LC_ALL) stands for, with the environment
 * variable of each. */
static const struct {
    int category;
    const char *variable;
} locale_categories[] = {
    {LC_CTYPE, "LC_CTYPE"},       {LC_NUMERIC, "LC_NUMERIC"},   {LC_TIME, "LC_TIME"},
    {LC_COLLATE, "LC_COLLATE"},   {LC_MONETARY, "LC_MONETARY"}, {LC_MESSAGES, "LC_MESSAGES"},
};

/* The locale the environment names for one category, as the C library
 * knows it; NULL when there is no data for it. */
static const char *locale_of_environment(const char *variable) {
    const char *name = getenv("LC_ALL");
    if (!name || !*name) name = getenv(variable);
    if (!name || !*name) name = getenv("LANG");
    if (!name || !*name) name = "C";
    const char *known = locale_with_data(name, strlen(name));
    if (!known && locale_start_up) known = locale_codeset_is_utf8(name) ? "C.UTF-8" : "C";
    return known;
}

char *__wrap_setlocale(int category, const char *name) {
    if (!name) return __real_setlocale(category, NULL);
    if (!*name) {
        /* every category asked for has to be there before one is set */
        for (const auto &c : locale_categories)
            if ((category == LC_ALL || c.category == category) && !locale_of_environment(c.variable)) {
                errno = ENOENT;
                return NULL;
            }
        bool one_of_the_six = false;
        for (const auto &c : locale_categories) {
            if (category != LC_ALL && c.category != category) continue;
            one_of_the_six = true;
            if (!__real_setlocale(c.category, locale_of_environment(c.variable))) return NULL;
        }
        if (!one_of_the_six) return __real_setlocale(category, name);
        return __real_setlocale(category, NULL);
    }
    /* A name, or for LC_ALL also one name per category separated by ";"
     * (the form setlocale(LC_ALL, NULL) reports them in): every one has to
     * be a locale there is data for. */
    std::string known;
    for (const char *part = name;;) {
        const size_t len = category == LC_ALL ? strcspn(part, ";") : strlen(part);
        const char *locale = locale_with_data(part, len);
        if (!locale) {
            errno = ENOENT;
            return NULL;
        }
        if (!known.empty()) known += ';';
        known += locale;
        if (!part[len]) break;
        part += len + 1;
    }
    return __real_setlocale(category, known.c_str());
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
#ifdef __wasm64__
    strcpy(out->machine, "wasm64");
#else
    strcpy(out->machine, "wasm32");
#endif
    return 0;
}

/* ---- terminals ----------------------------------------------------------------
 * A terminal is a device of the host: its name, its settings and what is
 * done to it are the host's. The structure the settings are kept in is this
 * C library's (scripts/include/termios.h); what is in it, and the selectors
 * the functions take, are in the host's numbers and are passed on as they
 * are. A descriptor that is not one of the host's is no terminal here. */

/* The layout the host reads and writes. */
static_assert(sizeof(struct termios) == 60, "struct termios is what the host fills");
static_assert(offsetof(struct termios, c_cc) == 17, "struct termios is what the host fills");
static_assert(offsetof(struct termios, __c_ispeed) == 52, "struct termios is what the host fills");

char *ttyname(int fd) {
    static char name[256];
    int len = 0;
    const int rc = wasi_host_tty_name(fd, name, (int)sizeof(name) - 1, &len);
    if (rc != 0) {
        /* a stream the host handed over that is a terminal has no name */
        errno = rc == -ENOTTY && isatty(fd) ? ENODEV : -rc;
        return NULL;
    }
    name[len] = '\0';
    return name;
}

/* The controlling terminal has one name for every process. */
char *ctermid(char *buf) {
    static char name[L_ctermid];
    if (!buf) buf = name;
    strcpy(buf, "/dev/tty");
    return buf;
}

int tcgetattr(int fd, struct termios *settings) {
    const int rc = wasi_host_tty_getattr(fd, settings);
    return rc != 0 ? fail(-rc) : 0;
}

int tcsetattr(int fd, int actions, const struct termios *settings) {
    const int rc = wasi_host_tty_setattr(fd, actions, settings);
    return rc != 0 ? fail(-rc) : 0;
}

/* Where the speed is kept in the settings is the host's own. */
static speed_t terminal_speed(const struct termios *settings, int output) {
    unsigned int speed = 0;
    struct termios copy = *settings;
    wasi_host_tty_speed(&copy, output, 0, 0, &speed);
    return (speed_t)speed;
}

static int set_terminal_speed(struct termios *settings, int output, speed_t speed) {
    unsigned int was = 0;
    const int rc = wasi_host_tty_speed(settings, output, 1, (unsigned int)speed, &was);
    return rc != 0 ? fail(-rc) : 0;
}

speed_t cfgetispeed(const struct termios *settings) { return terminal_speed(settings, 0); }
speed_t cfgetospeed(const struct termios *settings) { return terminal_speed(settings, 1); }
int cfsetispeed(struct termios *settings, speed_t speed) { return set_terminal_speed(settings, 0, speed); }
int cfsetospeed(struct termios *settings, speed_t speed) { return set_terminal_speed(settings, 1, speed); }

static int terminal_control(int fd, int what, int arg) {
    const int rc = wasi_host_tty_control(fd, what, arg);
    return rc != 0 ? fail(-rc) : 0;
}

int tcdrain(int fd) { return terminal_control(fd, 0, 0); }
int tcflow(int fd, int action) { return terminal_control(fd, 1, action); }
int tcflush(int fd, int queue) { return terminal_control(fd, 2, queue); }
int tcsendbreak(int fd, int duration) { return terminal_control(fd, 3, duration); }

pid_t tcgetpgrp(int fd) {
    int pgrp = 0;
    const int rc = wasi_host_tty_pgrp(fd, 0, 0, &pgrp);
    return rc != 0 ? (pid_t)fail(-rc) : (pid_t)pgrp;
}

int tcsetpgrp(int fd, pid_t pgrp) {
    int was = 0;
    const int rc = wasi_host_tty_pgrp(fd, 1, (int)pgrp, &was);
    return rc != 0 ? fail(-rc) : 0;
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

/* ---- poll(2) ---------------------------------------------------------------
 * poll over poll_oneoff, as wasi-libc's ppoll, except that a descriptor not
 * asked for reading is still watched for the events poll reports whether
 * asked or not: POLLNVAL (it is not open), POLLERR (nobody reads the pipe
 * it writes to any more) and POLLHUP (its peer has hung up). wasi-libc
 * refuses a descriptor asked for neither reading nor writing (ENOSYS: it
 * builds one subscription per POLLRDNORM and POLLWRNORM), and a program
 * that turns reading off for a while passes one: Danga::Socket's poll loop
 * asks POLLERR|POLLHUP|POLLNVAL of a client it has stopped reading, got -1
 * for every wait and spun (Perlbal). An event loop that watches the write
 * end of a pipe for a hang-up (IO::Async) asks POLLHUP alone.
 *
 * Such a descriptor gets a subscription for reading whose readiness is not
 * reported, as it was not asked for: only its error (POLLNVAL, POLLERR) and
 * the event's hang-up flag (POLLHUP). When a wait ends with nothing to
 * report but such readiness — the descriptor has input nobody asked about —
 * the wait goes on without that subscription for what is left of the
 * timeout, as poll(2) would not have returned. */
/* What a subscription of poll's is for, in the low bits of its userdata
 * (the descriptor's index is above them). */
static const uint64_t POLL_READ = 0, POLL_WRITE = 1, POLL_UNASKED = 2;

static uint64_t poll_userdata(size_t index, uint64_t kind) { return (uint64_t)index << 2 | kind; }

/* The monotonic clock, in nanoseconds. */
static uint64_t poll_now(void) {
    __wasi_timestamp_t now = 0;
    (void)__wasi_clock_time_get(__WASI_CLOCKID_MONOTONIC, 1, &now);
    return now;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    for (nfds_t i = 0; i < nfds; i++) fds[i].revents = 0;
    /* A descriptor whose unasked readiness ended a wait: left out of the
     * waits that follow. */
    std::vector<bool> quiet(nfds, false);
    const bool bounded = timeout >= 0;
    const uint64_t deadline = bounded ? poll_now() + (uint64_t)timeout * 1000000 : 0;
    std::vector<__wasi_subscription_t> subs;
    std::vector<__wasi_event_t> events;
    for (;;) {
        subs.clear();
        for (nfds_t i = 0; i < nfds; i++) {
            if (fds[i].fd < 0) continue;
            /* POLLPRI is never reported: nothing here has priority data */
            const short asked = fds[i].events & ~POLLPRI;
            __wasi_subscription_t sub = {};
            if (asked & POLLRDNORM) {
                sub.userdata = poll_userdata(i, POLL_READ);
                sub.u.tag = __WASI_EVENTTYPE_FD_READ;
                sub.u.u.fd_read.file_descriptor = fds[i].fd;
                subs.push_back(sub);
            }
            if (asked & POLLWRNORM) {
                sub.userdata = poll_userdata(i, POLL_WRITE);
                sub.u.tag = __WASI_EVENTTYPE_FD_WRITE;
                sub.u.u.fd_write.file_descriptor = fds[i].fd;
                subs.push_back(sub);
            }
            if (!(asked & POLLRDNORM) && !quiet[i]) {
                sub.userdata = poll_userdata(i, POLL_UNASKED);
                sub.u.tag = __WASI_EVENTTYPE_FD_READ;
                sub.u.u.fd_read.file_descriptor = fds[i].fd;
                subs.push_back(sub);
            }
        }
        const bool watched = !subs.empty();
        if (bounded || (!watched && nfds > 0)) {
            /* The timeout; with nothing left to watch and none given, a
             * wait a signal ends, as poll(2) waits on descriptors that
             * report nothing. */
            const uint64_t now = poll_now();
            __wasi_subscription_t sub = {};
            sub.userdata = UINT64_MAX;
            sub.u.tag = __WASI_EVENTTYPE_CLOCK;
            sub.u.u.clock.id = __WASI_CLOCKID_MONOTONIC;
            sub.u.u.clock.timeout = bounded ? (deadline > now ? deadline - now : 0) : (uint64_t)1 << 62;
            subs.push_back(sub);
        }
        if (subs.empty()) {
            /* nothing at all to wait for: as wasi-libc */
            errno = ENOTSUP;
            return -1;
        }
        events.resize(subs.size());
        size_t nevents = 0;
        const __wasi_errno_t error = __wasi_poll_oneoff(subs.data(), events.data(), subs.size(), &nevents);
        if (error != 0) {
            errno = error;
            return -1;
        }
        bool expired = false;
        for (size_t e = 0; e < nevents; e++) {
            const __wasi_event_t &ev = events[e];
            if (ev.type == __WASI_EVENTTYPE_CLOCK) {
                expired = expired || bounded;
                continue;
            }
            const size_t i = (size_t)(ev.userdata >> 2);
            const uint64_t kind = ev.userdata & 3;
            if (i >= nfds) continue;
            struct pollfd &p = fds[i];
            if (ev.error == __WASI_ERRNO_BADF) {
                p.revents |= POLLNVAL;
            } else if (ev.error != 0) {
                /* in error: the reader of the pipe it writes to has closed
                 * it, and a write fails at once */
                p.revents |= POLLERR;
            } else {
                const bool hangup = (ev.fd_readwrite.flags & __WASI_EVENTRWFLAGS_FD_READWRITE_HANGUP) != 0;
                if (kind == POLL_READ) p.revents |= POLLRDNORM;
                if (kind == POLL_WRITE) p.revents |= POLLWRNORM;
                if (hangup) p.revents |= POLLHUP;
                /* readable, and nobody asked: not an event of poll's */
                if (kind == POLL_UNASKED && !hangup) quiet[i] = true;
            }
        }
        /* A socket whose peer has hung up is reported hung up and, when
         * that was asked, writable (the write finds out), as on Linux. */
        int ready = 0;
        for (nfds_t i = 0; i < nfds; i++) {
            if (fds[i].revents) ready++;
        }
        if (ready > 0 || expired) return ready;
        if (bounded && poll_now() >= deadline) return 0;
    }
}

} /* extern "C" */
