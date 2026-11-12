/* perl_process.cc — who the process is: its ids, its user and groups, and
 * the user and group databases.
 *
 * WASI has none of this, and wasi-libc declares none of it. The host
 * answers through imports: a guest that runs as the host process (the perl
 * command) gets the host's ids and databases, and an isolated one gets a
 * fixed identity — pid 42, parent 1, user and group 0 — and empty
 * databases. A process made by fork has the pid the host forked it under,
 * and for a parent the process that forked it, or 1 once that one is gone.
 *
 * Nothing is cached here: an instance can be a copy (a memory snapshot) of
 * one that asked a different host.
 *
 * Records cross from the host in binary form: little-endian u32 fields, and
 * strings as a u32 length followed by the bytes.
 */
#include <sys/types.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

/* struct passwd, struct group and the prototypes perl is compiled with */
#include "scripts/wasi-posix-shim.h"

extern "C" {

__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_ids")))
int wasi_host_proc_ids(uint32_t ids[6]);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_groups")))
int wasi_host_proc_groups(uint32_t *gids, int capacity, uint32_t *count);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_user_by_id")))
int wasi_host_proc_user_by_id(uint32_t uid, void *buf, int buf_len, uint32_t *needed);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_user_by_name")))
int wasi_host_proc_user_by_name(const char *name, int name_len, void *buf, int buf_len,
                                uint32_t *needed);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_group_by_id")))
int wasi_host_proc_group_by_id(uint32_t gid, void *buf, int buf_len, uint32_t *needed);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_group_by_name")))
int wasi_host_proc_group_by_name(const char *name, int name_len, void *buf, int buf_len,
                                 uint32_t *needed);

/* The setuid and setgid families, for a process that is the host process:
 * request names the call (the SET_ constants below), a, b and c are its
 * arguments. -ENOSYS when the process has no ids of the host's to change. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_setids")))
int wasi_host_proc_setids(int request, int a, int b, int c);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_setgroups")))
int wasi_host_proc_setgroups(const uint32_t *gids, int count);

} /* extern "C" */

namespace {

enum { ID_PID, ID_PPID, ID_UID, ID_EUID, ID_GID, ID_EGID };

uint32_t process_id(int which) {
    uint32_t ids[6] = {0, 0, 0, 0, 0, 0};
    wasi_host_proc_ids(ids);
    return ids[which];
}

/* A record fetched from the host. The lookup functions return pointers
 * into it, which stay valid until the next lookup of the same kind — the
 * contract of getpwuid and friends. */
struct Record {
    char *data;
    uint32_t cap;
    uint32_t len;
    uint32_t pos;
};

/* Runs one lookup, growing the buffer to the size the host asks for.
 * Returns true when a record was found. */
template <typename Lookup>
bool fetch(Record &rec, Lookup lookup) {
    for (;;) {
        uint32_t needed = 0;
        const int rc = lookup(rec.data, (int)rec.cap, &needed);
        if (rc == 0) {
            rec.len = needed;
            rec.pos = 0;
            return true;
        }
        if (rc != -ERANGE || needed <= rec.cap) return false;
        char *grown = (char *)realloc(rec.data, needed);
        if (!grown) {
            errno = ENOMEM;
            return false;
        }
        rec.data = grown;
        rec.cap = needed;
    }
}

bool next_u32(Record &rec, uint32_t *out) {
    if (rec.len - rec.pos < 4) return false;
    memcpy(out, rec.data + rec.pos, 4);
    rec.pos += 4;
    return true;
}

/* Returns the next string of the record as a C string. The terminator is
 * written over the first byte of the length field that preceded the string
 * (already consumed), after the string has been moved down onto it. */
char *next_string(Record &rec) {
    uint32_t n = 0;
    if (!next_u32(rec, &n) || rec.len - rec.pos < n) return NULL;
    char *s = rec.data + rec.pos - 4;
    memmove(s, rec.data + rec.pos, n);
    s[n] = '\0';
    rec.pos += n;
    return s;
}

Record g_user_rec;
struct passwd g_user;
char g_empty[1] = "";

struct passwd *decode_user(bool found) {
    if (!found) return NULL;
    uint32_t uid = 0, gid = 0;
    if (!next_u32(g_user_rec, &uid) || !next_u32(g_user_rec, &gid)) return NULL;
    g_user.pw_uid = (uid_t)uid;
    g_user.pw_gid = (gid_t)gid;
    g_user.pw_name = next_string(g_user_rec);
    g_user.pw_gecos = next_string(g_user_rec);
    g_user.pw_dir = next_string(g_user_rec);
    g_user.pw_shell = next_string(g_user_rec);
    /* The host does not hand out password fields. */
    g_user.pw_passwd = g_empty;
    if (!g_user.pw_name || !g_user.pw_gecos || !g_user.pw_dir || !g_user.pw_shell)
        return NULL;
    return &g_user;
}

Record g_group_rec;
struct group g_group;
char *g_no_members[1] = {NULL};

struct group *decode_group(bool found) {
    if (!found) return NULL;
    uint32_t gid = 0;
    if (!next_u32(g_group_rec, &gid)) return NULL;
    g_group.gr_gid = (gid_t)gid;
    g_group.gr_name = next_string(g_group_rec);
    g_group.gr_passwd = g_empty;
    /* The host reports a group's id and name, not its members. */
    g_group.gr_mem = g_no_members;
    return g_group.gr_name ? &g_group : NULL;
}

/* The requests of the proc_setids import. */
enum {
    SET_UID, SET_EUID, SET_REUID, SET_RESUID, SET_GID, SET_EGID, SET_REGID, SET_RESGID
};

/* Changes ids of the process. A process that is the host process asks the
 * host, which decides as it does for any process. An isolated one has no
 * privileges: changing an id to the value it already has is all it may do.
 * (uid_t)-1 leaves an id alone; the saved id starts out as the effective
 * one. */
int set_ids(int request, uid_t a, uid_t b, uid_t c, uid_t want_real, uid_t want_effective,
            uid_t want_saved, int real_which, int effective_which) {
    const int rc = wasi_host_proc_setids(request, (int)a, (int)b, (int)c);
    if (rc == 0) return 0;
    if (rc != -ENOSYS) {
        errno = -rc;
        return -1;
    }
    if ((want_real != (uid_t)-1 && want_real != process_id(real_which)) ||
        (want_effective != (uid_t)-1 && want_effective != process_id(effective_which)) ||
        (want_saved != (uid_t)-1 && want_saved != process_id(effective_which))) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

const uid_t kKeep = (uid_t)-1;

} /* namespace */

extern "C" {

pid_t getpid(void) { return (pid_t)process_id(ID_PID); }
pid_t getppid(void) { return (pid_t)process_id(ID_PPID); }

uid_t getuid(void) { return (uid_t)process_id(ID_UID); }
uid_t geteuid(void) { return (uid_t)process_id(ID_EUID); }
gid_t getgid(void) { return (gid_t)process_id(ID_GID); }
gid_t getegid(void) { return (gid_t)process_id(ID_EGID); }

int setuid(uid_t uid) {
    return set_ids(SET_UID, uid, kKeep, kKeep, uid, uid, kKeep, ID_UID, ID_EUID);
}
int seteuid(uid_t uid) {
    return set_ids(SET_EUID, uid, kKeep, kKeep, kKeep, uid, kKeep, ID_UID, ID_EUID);
}
int setreuid(uid_t ruid, uid_t euid) {
    return set_ids(SET_REUID, ruid, euid, kKeep, ruid, euid, kKeep, ID_UID, ID_EUID);
}
int setresuid(uid_t ruid, uid_t euid, uid_t suid) {
    return set_ids(SET_RESUID, ruid, euid, suid, ruid, euid, suid, ID_UID, ID_EUID);
}
int setgid(gid_t gid) {
    return set_ids(SET_GID, gid, kKeep, kKeep, gid, gid, kKeep, ID_GID, ID_EGID);
}
int setegid(gid_t gid) {
    return set_ids(SET_EGID, gid, kKeep, kKeep, kKeep, gid, kKeep, ID_GID, ID_EGID);
}
int setregid(gid_t rgid, gid_t egid) {
    return set_ids(SET_REGID, rgid, egid, kKeep, rgid, egid, kKeep, ID_GID, ID_EGID);
}
int setresgid(gid_t rgid, gid_t egid, gid_t sgid) {
    return set_ids(SET_RESGID, rgid, egid, sgid, rgid, egid, sgid, ID_GID, ID_EGID);
}

int getgroups(int size, gid_t list[]) {
    uint32_t count = 0;
    if (size < 0) {
        errno = EINVAL;
        return -1;
    }
    /* gid_t is the u32 the host writes */
    const int rc = wasi_host_proc_groups((uint32_t *)list, size, &count);
    if (rc == -ERANGE) {
        errno = EINVAL; /* what getgroups reports for a list that is too short */
        return -1;
    }
    if (rc != 0) {
        errno = -rc;
        return -1;
    }
    return (int)count;
}

int setgroups(size_t size, const gid_t *list) {
    /* gid_t is the u32 the host reads; a process that is not the host
     * process has no privilege to change its groups */
    const int rc = wasi_host_proc_setgroups((const uint32_t *)list, (int)size);
    if (rc != 0) {
        errno = rc == -ENOSYS ? EPERM : -rc;
        return -1;
    }
    return 0;
}

struct passwd *getpwuid(uid_t uid) {
    return decode_user(fetch(g_user_rec, [uid](void *buf, int len, uint32_t *needed) {
        return wasi_host_proc_user_by_id((uint32_t)uid, buf, len, needed);
    }));
}

struct passwd *getpwnam(const char *name) {
    if (!name) return NULL;
    return decode_user(fetch(g_user_rec, [name](void *buf, int len, uint32_t *needed) {
        return wasi_host_proc_user_by_name(name, (int)strlen(name), buf, len, needed);
    }));
}

struct group *getgrgid(gid_t gid) {
    return decode_group(fetch(g_group_rec, [gid](void *buf, int len, uint32_t *needed) {
        return wasi_host_proc_group_by_id((uint32_t)gid, buf, len, needed);
    }));
}

struct group *getgrnam(const char *name) {
    if (!name) return NULL;
    return decode_group(fetch(g_group_rec, [name](void *buf, int len, uint32_t *needed) {
        return wasi_host_proc_group_by_name(name, (int)strlen(name), buf, len, needed);
    }));
}

/* Walking the databases: the host looks entries up by id and by name but
 * has no way to enumerate them, so the walk is empty — getpwent and
 * getgrent report the end at once. */
void setpwent(void) {}
void endpwent(void) {}
struct passwd *getpwent(void) { return NULL; }
void setgrent(void) {}
void endgrent(void) {}
struct group *getgrent(void) { return NULL; }

/* The login name is that of the user the process runs as. */
char *getlogin(void) {
    struct passwd *pw = getpwuid(getuid());
    if (!pw) {
        errno = ENOENT;
        return NULL;
    }
    return pw->pw_name;
}

} /* extern "C" */
