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
#include <unistd.h>

/* the prototypes perl is compiled with (struct netent and friends too) */
#include "scripts/wasi-posix-shim.h"

namespace {

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

/* ---- what a WASI process cannot do ---------------------------------------- */

/* The root of the filesystem is the host's to set. */
int chroot(const char *path) {
    (void)path;
    return fail(EPERM);
}

/* The working directory is a path kept by libc; a directory that is only
 * known by its descriptor cannot become it. */
int fchdir(int fd) {
    (void)fd;
    return fail(ENOSYS);
}

/* FIFOs cannot be created through WASI. */
int mkfifo(const char *path, mode_t mode) {
    (void)path;
    (void)mode;
    return fail(ENOSYS);
}

/* A process image cannot be replaced; perl's own exec is built on spawning
 * (perl_wasi.cc). */
int execv(const char *path, char *const argv[]) {
    (void)path;
    (void)argv;
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

/* wasi-libc has one time zone, UTC, and no zone database to read another
 * from: there is nothing for tzset to set up. */
void tzset(void) {}

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
