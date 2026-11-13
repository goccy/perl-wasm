/* perl_numbers.cc — the numbers of the operating system, as the program
 * sees them.
 *
 * The C library of the interpreter has numbers of its own for what an
 * operating system numbers: errors, signals, the flags of open(2) and
 * poll(2), address families, socket options. A program that is the host
 * process (the host says so: host_numbering) is shown the host's numbers
 * instead, in every place a number passes between the program and the
 * interpreter's C code:
 *
 *   - $! and the status of a die (three calls in mg.c and perl.c, see the
 *     patch in scripts/wasi-configure.sh): host_errno_of, guest_errno_of;
 *   - the constants of the linked-in extensions (Fcntl, POSIX, Socket,
 *     IO::Poll, ...): when an extension has booted, every constant in its
 *     package that names a number of a family gets the host's number
 *     (numbers_show_constants); Errno.pm asks for the host's whole table
 *     (Internals::host_numbers);
 *   - the built-in functions and the XSUBs that take or return such
 *     numbers: each is entered through a routine that turns the program's
 *     numbers into the C library's and the results back
 *     (numbers_wrap_builtins, numbers_extension_booted);
 *   - the status of a child ($?), whose low bits are a signal number: where
 *     waitpid hands it to the interpreter (the link wraps waitpid).
 *
 * One table drives all of it. perl_numbers.inc lists the families and, per
 * family, the names this C library defines, with its values; the host gives
 * its value for a name (host_number_at). The two meet by the name the
 * number has in C. A family the host has no table of keeps the C library's
 * numbers. A number that has no counterpart is carried, not lost: see
 * enum_to_host.
 *
 * Without host numbering nothing here changes a number.
 */
#include "scripts/wasi-posix-shim.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <langinfo.h>
#include <locale.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unordered_map>
#include <vector>

extern "C" {
#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>

/* Whose numbers the program sees: 0 the C library's own, otherwise the
 * host's. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("host_numbering")))
int wasi_host_numbering(void);

/* The index-th number of a family of the host's: its name in C (name_len
 * bytes at name, which holds name_cap) and its value. 0, or a negative
 * errno: ENOENT past the last one. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("host_number_at")))
int wasi_host_number_at(int family, int index, char *name, int name_cap, int *name_len,
                        long long *value);

/* the C library's waitpid and strerror: the link gives the names to
 * __wrap_waitpid and __wrap_strerror, below */
pid_t __real_waitpid(pid_t pid, int *status, int options);
char *__real_strerror(int number);
}

namespace {

enum Kind { ENUM, FLAGS };

/* The ids of the families. */
enum {
#define NUMBER_FAMILY(id, family, kind) FAMILY_##family = id,
#define NUMBER(family, name)
#include "perl_numbers.inc"
#undef NUMBER_FAMILY
#undef NUMBER
    FAMILY_END
};

struct FamilyInfo {
    int id;
    const char *name;
    Kind kind;
};
const FamilyInfo family_infos[] = {
#define NUMBER_FAMILY(id, family, kind) {id, #family, kind},
#define NUMBER(family, name)
#include "perl_numbers.inc"
#undef NUMBER_FAMILY
#undef NUMBER
};

/* A number of this C library under its name. */
struct GuestNumber {
    int family;
    const char *name;
    long long value;
};
const GuestNumber guest_numbers[] = {
#define NUMBER_FAMILY(id, family, kind)
#define NUMBER(family, name) {FAMILY_##family, #name, (long long)(name)},
#include "perl_numbers.inc"
#undef NUMBER_FAMILY
#undef NUMBER
};

/* A number both sides have a name for. */
struct Pair {
    const char *name;
    long long guest;
    long long host;
};

/* What the host said about a family: every name it has, and the pairs
 * with this C library's names, in the order of perl_numbers.inc. It is
 * kept because asking again costs a call per name; the host's numbers do
 * not change while the process lives. */
struct Family {
    bool loaded = false;
    std::vector<std::pair<std::string, long long>> host;
    std::vector<Pair> pairs;
};
Family g_families[FAMILY_END];

bool active() { return wasi_host_numbering() != 0; }

const FamilyInfo *family_info(int id) {
    for (const FamilyInfo &info : family_infos)
        if (info.id == id) return &info;
    return NULL;
}

Family &family(int id) {
    Family &f = g_families[id];
    if (f.loaded) return f;
    char name[64];
    int len = 0;
    long long value = 0;
    for (int i = 0; wasi_host_number_at(id, i, name, (int)sizeof(name), &len, &value) == 0; i++)
        f.host.emplace_back(std::string(name, (size_t)len), value);
    for (const GuestNumber &g : guest_numbers) {
        if (g.family != id) continue;
        for (const auto &h : f.host) {
            if (h.first == g.name) {
                f.pairs.push_back({g.name, g.value, h.second});
                break;
            }
        }
    }
    f.loaded = true;
    return f;
}

bool is_guest_number(int id, long long value) {
    for (const GuestNumber &g : guest_numbers)
        if (g.family == id && g.value == value) return true;
    return false;
}

/* ---- one number for one thing ---------------------------------------------
 * A number with a name on both sides becomes the other side's number for
 * that name (the first name, where a side has two names for one number).
 * Any other number means nothing to the other side, and must not be taken
 * there for the number of something else:
 *
 *   - a number of the host's that this C library has no name for stays as
 *     it is here, unless it is the number of something else here; then it
 *     is marked (FOREIGN);
 *   - a number this C library has a name for and the host has none is
 *     shown to the program marked.
 *
 * A marked number is refused as unknown where it is used, and turns back
 * into the number it was when it returns to the side it came from. The
 * mark is a bit no number of these families uses. */
const long long FOREIGN = 0x40000000LL;

bool is_marked(long long value) { return value >= FOREIGN && value < 2 * FOREIGN; }

bool can_be_marked(long long value) { return value >= 0 && value < FOREIGN; }

long long enum_to_host(int id, long long guest) {
    const Family &f = family(id);
    if (f.host.empty()) return guest;
    for (const Pair &p : f.pairs)
        if (p.guest == guest) return p.host;
    if (is_marked(guest)) return guest - FOREIGN;
    if (can_be_marked(guest) && is_guest_number(id, guest)) return guest + FOREIGN;
    return guest;
}

long long enum_to_guest(int id, long long host) {
    const Family &f = family(id);
    if (f.host.empty()) return host;
    for (const Pair &p : f.pairs)
        if (p.host == host) return p.guest;
    if (is_marked(host)) return host - FOREIGN;
    if (can_be_marked(host) && is_guest_number(id, host)) return host + FOREIGN;
    return host;
}

/* ---- bits that are or-ed ---------------------------------------------------
 * Each bit (or group of bits: O_SYNC is two on linux) with a name on both
 * sides becomes the other side's; the groups with the most bits are looked
 * for first. A name whose value is 0 on one side (O_NOCTTY here) stands
 * for nothing there. Bits without a counterpart are reported in *unknown.
 *
 * The flags of open(2) hold a field that is not a set of bits, the access
 * mode: O_RDONLY, O_WRONLY and O_RDWR are values of the field O_ACCMODE
 * masks, and are translated as such. */
int bit_count(long long v) {
    int n = 0;
    for (unsigned long long u = (unsigned long long)v; u; u &= u - 1) n++;
    return n;
}

bool is_access_name(const char *name) {
    return strcmp(name, "O_RDONLY") == 0 || strcmp(name, "O_WRONLY") == 0 ||
           strcmp(name, "O_RDWR") == 0 || strcmp(name, "O_ACCMODE") == 0;
}

long long flags_translate(int id, long long value, bool to_host, long long *unknown) {
    const Family &f = family(id);
    *unknown = 0;
    if (f.host.empty()) return value;
    long long out = 0;
    long long rest = value;
    if (id == FAMILY_open) {
        long long from_mask = 0;
        bool has_mask = false;
        for (const Pair &p : f.pairs) {
            if (strcmp(p.name, "O_ACCMODE") == 0) {
                from_mask = to_host ? p.guest : p.host;
                has_mask = true;
            }
        }
        if (has_mask) {
            const long long mode = rest & from_mask;
            bool found = false;
            for (const Pair &p : f.pairs) {
                if (!is_access_name(p.name) || strcmp(p.name, "O_ACCMODE") == 0) continue;
                if ((to_host ? p.guest : p.host) == mode) {
                    out |= to_host ? p.host : p.guest;
                    found = true;
                    break;
                }
            }
            if (!found) *unknown |= mode;
            rest &= ~from_mask;
        }
    }
    for (int bits = 64; bits >= 1; bits--) {
        for (const Pair &p : f.pairs) {
            if (id == FAMILY_open && is_access_name(p.name)) continue;
            const long long from = to_host ? p.guest : p.host;
            if (from == 0 || bit_count(from) != bits || (rest & from) != from) continue;
            out |= to_host ? p.host : p.guest;
            rest &= ~from;
        }
    }
    *unknown |= rest;
    return out;
}

Kind kind_of(int id) {
    const FamilyInfo *info = family_info(id);
    return info ? info->kind : ENUM;
}

/* The host's number for one of this C library's, and the reverse. Bits
 * without a counterpart are dropped; a caller that must not drop them
 * uses flags_translate. */
long long to_host(int id, long long guest) {
    if (kind_of(id) == ENUM) return enum_to_host(id, guest);
    long long unknown;
    return flags_translate(id, guest, true, &unknown);
}

long long to_guest(int id, long long host) {
    if (kind_of(id) == ENUM) return enum_to_guest(id, host);
    long long unknown;
    return flags_translate(id, host, false, &unknown);
}

/* The type of a socket is a number that may have flags or-ed into it
 * (SOCK_NONBLOCK, SOCK_CLOEXEC, where the system has them). */
long long socktype_flags_mask(bool host_side) {
    long long mask = 0;
    for (const Pair &p : family(FAMILY_sockflag).pairs) mask |= host_side ? p.host : p.guest;
    return mask;
}

long long socktype_to_guest(long long host) {
    const long long flags = host & socktype_flags_mask(true);
    return enum_to_guest(FAMILY_socktype, host & ~flags) | to_guest(FAMILY_sockflag, flags);
}

long long socktype_to_host(long long guest) {
    const long long flags = guest & socktype_flags_mask(false);
    return enum_to_host(FAMILY_socktype, guest & ~flags) | to_host(FAMILY_sockflag, flags);
}

/* The family of the option names of a socket level (this C library's
 * number of the level); -1 for a level whose options are not numbered
 * here. */
int option_family(long long guest_level) {
    if (guest_level == SOL_SOCKET) return FAMILY_so;
    if (guest_level == IPPROTO_IP) return FAMILY_ip;
    if (guest_level == IPPROTO_IPV6) return FAMILY_ipv6;
    if (guest_level == IPPROTO_TCP) return FAMILY_tcp;
    return -1;
}

/* ---- the program's arguments ------------------------------------------------ */

/* Puts the C library's number for the program's in a stack slot: a new
 * value, the program's own is not touched. */
void slot_to_guest(pTHX_ SV **slot, int id) {
    SV *const sv = *slot;
    SvGETMAGIC(sv);
    if (!SvOK(sv)) return;
    *slot = sv_2mortal(newSViv((IV)to_guest(id, (long long)SvIV_nomg(sv))));
}

/* The XSUB (or built-in) that a routine of this file stands in front of.
 * Nothing else records it: the routines of an extension are static, and
 * the op table has only the one slot. */
std::unordered_map<CV *, XSUBADDR_t> g_xsub_behind;
Perl_ppaddr_t g_builtin_behind[MAXO];

XSUBADDR_t xsub_behind(CV *cv) {
    const auto it = g_xsub_behind.find(cv);
    return it == g_xsub_behind.end() ? NULL : it->second;
}

/* ---- built-in functions ------------------------------------------------------
 * Each routine below runs in place of the built-in of its op, translates
 * the arguments that are numbers of a family on the stack, runs the
 * built-in, and translates what it returns. */

OP *behind(pTHX) { return g_builtin_behind[PL_op->op_type](aTHX); }

/* sysopen FILEHANDLE, FILENAME, MODE [, PERMS]. A mode with bits this
 * system has no equivalent of is refused (EINVAL), not opened without
 * them. */
OP *builtin_sysopen(pTHX) {
    if (!active()) return behind(aTHX);
    dSP;
    const int items = (MAXARG > 3) ? 4 : 3;
    SV **const mode = (MAXARG > 3) ? SP - 1 : SP;
    long long unknown;
    const long long guest = flags_translate(FAMILY_open, (long long)SvIV(*mode), false, &unknown);
    if (unknown) {
        SP -= items;
        errno = EINVAL;
        PUSHs(&PL_sv_undef);
        RETURN;
    }
    *mode = sv_2mortal(newSViv((IV)guest));
    return behind(aTHX);
}

/* fcntl FILEHANDLE, FUNCTION, SCALAR: the function's number; the flags of
 * F_SETFL and of what F_GETFL returns are the flags of open. */
OP *builtin_fcntl(pTHX) {
    if (!active()) return behind(aTHX);
    dSP;
    SV **const function = SP - 1;
    SV **const arg = SP;
    const long long guest_function = to_guest(FAMILY_fcntl, (long long)SvIV(*function));
    *function = sv_2mortal(newSViv((IV)guest_function));
    if (guest_function == F_SETFL && !SvPOK(*arg)) slot_to_guest(aTHX_ arg, FAMILY_open);
    OP *const next = behind(aTHX);
    if (guest_function == F_GETFL) {
        SPAGAIN;
        SV *const result = TOPs;
        if (SvOK(result)) {
            /* "0 but true" is the number 0 */
            const long long host = to_host(FAMILY_open, (long long)SvIV(result));
            SETs(host ? sv_2mortal(newSViv((IV)host)) : newSVpvs_flags("0 but true", SVs_TEMP));
        }
    }
    return next;
}

/* ioctl FILEHANDLE, FUNCTION, SCALAR: the number of the request, for the
 * requests this C library answers. */
OP *builtin_ioctl(pTHX) {
    if (!active()) return behind(aTHX);
    dSP;
    slot_to_guest(aTHX_ SP - 1, FAMILY_ioctl);
    return behind(aTHX);
}

/* socket SOCKET, DOMAIN, TYPE, PROTOCOL and socketpair SOCKET1, SOCKET2,
 * DOMAIN, TYPE, PROTOCOL. */
OP *builtin_socket(pTHX) {
    if (!active()) return behind(aTHX);
    dSP;
    slot_to_guest(aTHX_ SP - 2, FAMILY_af);
    SV *const type = *(SP - 1);
    *(SP - 1) = sv_2mortal(newSViv((IV)socktype_to_guest((long long)SvIV(type))));
    return behind(aTHX);
}

/* getsockopt SOCKET, LEVEL, OPTNAME and setsockopt SOCKET, LEVEL, OPTNAME,
 * OPTVAL. The value of an option that is itself a number of a family (the
 * pending error, the type, the address family of the socket) is translated
 * where getsockopt returns it. */
OP *builtin_sockopt(pTHX) {
    if (!active()) return behind(aTHX);
    dSP;
    const bool get = PL_op->op_type == OP_GSOCKOPT;
    SV **const level = get ? SP - 1 : SP - 2;
    SV **const name = get ? SP : SP - 1;
    const long long guest_level = to_guest(FAMILY_sol, (long long)SvIV(*level));
    *level = sv_2mortal(newSViv((IV)guest_level));
    const int names = option_family(guest_level);
    long long guest_name = (long long)SvIV(*name);
    if (names >= 0) {
        guest_name = to_guest(names, guest_name);
        *name = sv_2mortal(newSViv((IV)guest_name));
    }
    OP *const next = behind(aTHX);
    if (get && guest_level == SOL_SOCKET) {
        int value_family = -1;
        if (guest_name == SO_ERROR) value_family = FAMILY_errno;
        if (guest_name == SO_TYPE) value_family = FAMILY_socktype;
#ifdef SO_DOMAIN
        if (guest_name == SO_DOMAIN) value_family = FAMILY_af;
#endif
        SPAGAIN;
        SV *const result = TOPs;
        if (value_family >= 0 && SvPOK(result) && SvCUR(result) >= sizeof(int)) {
            int value;
            memcpy(&value, SvPVX(result), sizeof(int));
            value = (int)to_host(value_family, value);
            memcpy(SvPVX(result), &value, sizeof(int));
        }
    }
    return next;
}

/* send SOCKET, MSG, FLAGS [, TO] and recv SOCKET, SCALAR, LENGTH, FLAGS:
 * the flags are the third and the fourth argument. */
OP *builtin_send(pTHX) {
    if (!active()) return behind(aTHX);
    dSP;
    SV **const mark = PL_stack_base + TOPMARK;
    if (SP - mark >= 3) slot_to_guest(aTHX_ mark + 3, FAMILY_msg);
    return behind(aTHX);
}

OP *builtin_recv(pTHX) {
    if (!active()) return behind(aTHX);
    dSP;
    SV **const mark = PL_stack_base + TOPMARK;
    if (SP - mark >= 4) slot_to_guest(aTHX_ mark + 4, FAMILY_msg);
    return behind(aTHX);
}

/* gethostbyname NAME, gethostbyaddr ADDR, ADDRTYPE and gethostent: the
 * address type is an address family, as an argument and as the third
 * value of the list they return. */
OP *builtin_hostent(pTHX) {
    if (!active()) return behind(aTHX);
    dSP;
    const int type = PL_op->op_type;
    const SSize_t args = type == OP_GHBYADDR ? 2 : type == OP_GHBYNAME ? 1 : 0;
    if (type == OP_GHBYADDR) slot_to_guest(aTHX_ SP, FAMILY_af);
    const SSize_t first = (SSize_t)(SP - PL_stack_base) - args + 1;
    const bool list = GIMME_V == G_LIST;
    OP *const next = behind(aTHX);
    if (list && PL_stack_sp - (PL_stack_base + first) + 1 >= 3) {
        SV **const addrtype = PL_stack_base + first + 2;
        if (SvOK(*addrtype))
            *addrtype = sv_2mortal(newSViv((IV)to_host(FAMILY_af, (long long)SvIV(*addrtype))));
    }
    return next;
}

/* kill SIGNAL, LIST: a signal given by number (negative: to process
 * groups). A name is the same name everywhere. */
OP *builtin_kill(pTHX) {
    if (!active()) return behind(aTHX);
    dSP;
    SV **const mark = PL_stack_base + TOPMARK;
    if (SP - mark >= 1) {
        SV *const signal = mark[1];
        STRLEN len;
        const char *const s = SvPV(signal, len);
        const bool named = len > 0 && (isALPHA(s[0]) || (s[0] == '-' && len > 1 && isALPHA(s[1])));
        if (!named) {
            const IV number = SvIV_nomg(signal);
            const long long guest = to_guest(FAMILY_signal, number < 0 ? -(long long)number : number);
            mark[1] = sv_2mortal(newSViv((IV)(number < 0 ? -guest : guest)));
        }
    }
    return behind(aTHX);
}

void wrap_builtin(int op, Perl_ppaddr_t routine) {
    if (PL_ppaddr[op] == routine) return;
    g_builtin_behind[op] = PL_ppaddr[op];
    PL_ppaddr[op] = routine;
}

/* ---- XSUBs --------------------------------------------------------------------
 * The same for the routines of the extensions. A routine here finds its
 * arguments where the XSUB behind it will: above the mark, which it leaves
 * for that XSUB to take. */

struct XsubCall {
    SSize_t ax; /* the index of the first argument */
    SSize_t items;
};

XsubCall xsub_call(pTHX) {
    SV **const mark = PL_stack_base + TOPMARK;
    return {(SSize_t)(mark - PL_stack_base) + 1, (SSize_t)(PL_stack_sp - mark)};
}

SV **xsub_slot(pTHX_ const XsubCall &call, SSize_t i) { return PL_stack_base + call.ax + i; }

SSize_t xsub_returned(pTHX_ const XsubCall &call) {
    return (SSize_t)(PL_stack_sp - (PL_stack_base + call.ax)) + 1;
}

/* IO::Poll::_poll(timeout, fd, mask, fd, mask, ...): the masks are bits of
 * poll, and the XSUB writes the returned events over them. */
XS_INTERNAL(xsub_poll) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (!active()) {
        xsub(aTHX_ cv);
        return;
    }
    const XsubCall call = xsub_call(aTHX);
    std::vector<SV *> masks;
    for (SSize_t i = 2; i < call.items; i += 2) {
        SV *const mask = *xsub_slot(aTHX_ call, i);
        sv_setiv(mask, (IV)to_guest(FAMILY_poll, (long long)SvIV(mask)));
        masks.push_back(mask);
    }
    xsub(aTHX_ cv);
    for (SV *const mask : masks) sv_setiv(mask, (IV)to_host(FAMILY_poll, (long long)SvIV(mask)));
}

/* POSIX::open(filename, flags = O_RDONLY, mode = 0666). */
XS_INTERNAL(xsub_posix_open) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (!active()) {
        xsub(aTHX_ cv);
        return;
    }
    const XsubCall call = xsub_call(aTHX);
    if (call.items >= 2) {
        long long unknown;
        const long long guest = flags_translate(
            FAMILY_open, (long long)SvIV(*xsub_slot(aTHX_ call, 1)), false, &unknown);
        if (unknown) {
            (void)POPMARK;
            PL_stack_sp = PL_stack_base + call.ax;
            *PL_stack_sp = &PL_sv_undef;
            errno = EINVAL;
            return;
        }
        *xsub_slot(aTHX_ call, 1) = sv_2mortal(newSViv((IV)guest));
    }
    xsub(aTHX_ cv);
}

/* SDBM_File::TIEHASH(dbtype, filename, flags, mode, pagname): the flags are
 * those of open. */
XS_INTERNAL(xsub_third_is_open_flags) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (active()) {
        const XsubCall call = xsub_call(aTHX);
        if (call.items >= 3) slot_to_guest(aTHX_ xsub_slot(aTHX_ call, 2), FAMILY_open);
    }
    xsub(aTHX_ cv);
}

/* An XSUB whose first argument is an address family (Socket::inet_pton,
 * Socket::inet_ntop). */
XS_INTERNAL(xsub_first_is_family) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (active()) {
        const XsubCall call = xsub_call(aTHX);
        if (call.items >= 1) slot_to_guest(aTHX_ xsub_slot(aTHX_ call, 0), FAMILY_af);
    }
    xsub(aTHX_ cv);
}

/* Socket::sockaddr_family(sockaddr) returns an address family. */
XS_INTERNAL(xsub_returns_family) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (!active()) {
        xsub(aTHX_ cv);
        return;
    }
    const XsubCall call = xsub_call(aTHX);
    xsub(aTHX_ cv);
    if (xsub_returned(aTHX_ call) == 1) {
        SV **const result = xsub_slot(aTHX_ call, 0);
        if (SvOK(*result)) *result = sv_2mortal(newSViv((IV)to_host(FAMILY_af, (long long)SvIV(*result))));
    }
}

/* The error of getaddrinfo and getnameinfo is a number and a text in one
 * value: the number is the host's. */
void error_to_host(pTHX_ SV *err) {
    if (SvIOK(err) && SvIVX(err) != 0) SvIV_set(err, (IV)to_host(FAMILY_eai, (long long)SvIVX(err)));
}

void member_translate(pTHX_ HV *hv, const char *key, long long (*translate)(long long)) {
    SV **const member = hv_fetch(hv, key, (I32)strlen(key), 0);
    if (member && SvOK(*member)) sv_setiv(*member, (IV)translate((long long)SvIV(*member)));
}

long long ai_flags_to_guest(long long v) { return to_guest(FAMILY_ai, v); }
long long af_to_guest(long long v) { return to_guest(FAMILY_af, v); }
long long af_to_host(long long v) { return to_host(FAMILY_af, v); }

/* Socket::getaddrinfo(host, service, hints): the hints name flags, a
 * family and a socket type; each result names a family and a socket type. */
XS_INTERNAL(xsub_getaddrinfo) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (!active()) {
        xsub(aTHX_ cv);
        return;
    }
    const XsubCall call = xsub_call(aTHX);
    if (call.items >= 3) {
        SV *const hints = *xsub_slot(aTHX_ call, 2);
        SvGETMAGIC(hints);
        if (SvROK(hints) && SvTYPE(SvRV(hints)) == SVt_PVHV) {
            /* a copy: the program's hash keeps the program's numbers */
            HV *const copy = newHVhv(MUTABLE_HV(SvRV(hints)));
            member_translate(aTHX_ copy, "flags", ai_flags_to_guest);
            member_translate(aTHX_ copy, "family", af_to_guest);
            member_translate(aTHX_ copy, "socktype", socktype_to_guest);
            *xsub_slot(aTHX_ call, 2) = sv_2mortal(newRV_noinc(MUTABLE_SV(copy)));
        }
    }
    xsub(aTHX_ cv);
    const SSize_t returned = xsub_returned(aTHX_ call);
    if (returned >= 1) error_to_host(aTHX_ *xsub_slot(aTHX_ call, 0));
    for (SSize_t i = 1; i < returned; i++) {
        SV *const result = *xsub_slot(aTHX_ call, i);
        if (!SvROK(result) || SvTYPE(SvRV(result)) != SVt_PVHV) continue;
        member_translate(aTHX_ MUTABLE_HV(SvRV(result)), "family", af_to_host);
        member_translate(aTHX_ MUTABLE_HV(SvRV(result)), "socktype", socktype_to_host);
    }
}

/* Socket::getnameinfo(addr, flags, xflags). */
XS_INTERNAL(xsub_getnameinfo) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (!active()) {
        xsub(aTHX_ cv);
        return;
    }
    const XsubCall call = xsub_call(aTHX);
    if (call.items >= 2) slot_to_guest(aTHX_ xsub_slot(aTHX_ call, 1), FAMILY_ni);
    xsub(aTHX_ cv);
    if (xsub_returned(aTHX_ call) >= 1) error_to_host(aTHX_ *xsub_slot(aTHX_ call, 0));
}

/* An XSUB whose arguments from a position on are signal numbers:
 * POSIX::SigSet->new(signals...) from the second, addset, delset and
 * ismember (sigset, signal) the second, POSIX::sigaction's first (below). */
void signals_to_guest(pTHX_ const XsubCall &call, SSize_t from) {
    for (SSize_t i = from; i < call.items; i++) slot_to_guest(aTHX_ xsub_slot(aTHX_ call, i), FAMILY_signal);
}

XS_INTERNAL(xsub_signals_follow_first) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (active()) signals_to_guest(aTHX_ xsub_call(aTHX), 1);
    xsub(aTHX_ cv);
}

/* POSIX::sigaction(signal, action, oldaction): the signal, and the flags
 * the two actions hold (a POSIX::SigAction is a hash with FLAGS in it). */
XS_INTERNAL(xsub_sigaction) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (!active()) {
        xsub(aTHX_ cv);
        return;
    }
    const XsubCall call = xsub_call(aTHX);
    if (call.items >= 1) slot_to_guest(aTHX_ xsub_slot(aTHX_ call, 0), FAMILY_signal);
    if (call.items >= 2) {
        SV *const action = *xsub_slot(aTHX_ call, 1);
        SvGETMAGIC(action);
        if (SvROK(action) && SvTYPE(SvRV(action)) == SVt_PVHV) {
            /* a copy of the same class: the program's object keeps the
             * program's numbers */
            HV *const copy = newHVhv(MUTABLE_HV(SvRV(action)));
            SV **const flags = hv_fetchs(copy, "FLAGS", 0);
            if (flags && SvOK(*flags)) sv_setiv(*flags, (IV)to_guest(FAMILY_sa, (long long)SvIV(*flags)));
            SV *const ref = sv_2mortal(newRV_noinc(MUTABLE_SV(copy)));
            if (SvOBJECT(SvRV(action))) sv_bless(ref, SvSTASH(SvRV(action)));
            *xsub_slot(aTHX_ call, 1) = ref;
        }
    }
    SV *const old = call.items >= 3 ? *xsub_slot(aTHX_ call, 2) : NULL;
    xsub(aTHX_ cv);
    if (old && SvROK(old) && SvTYPE(SvRV(old)) == SVt_PVHV) {
        SV **const flags = hv_fetchs(MUTABLE_HV(SvRV(old)), "FLAGS", 0);
        if (flags && SvOK(*flags)) sv_setiv(*flags, (IV)to_host(FAMILY_sa, (long long)SvIV(*flags)));
    }
}

/* POSIX::sigprocmask(how, sigset, oldsigset). */
XS_INTERNAL(xsub_sigprocmask) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (active()) {
        const XsubCall call = xsub_call(aTHX);
        if (call.items >= 1) slot_to_guest(aTHX_ xsub_slot(aTHX_ call, 0), FAMILY_sighow);
    }
    xsub(aTHX_ cv);
}

/* I18N::Langinfo::langinfo(item). */
XS_INTERNAL(xsub_langinfo) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (active()) {
        const XsubCall call = xsub_call(aTHX);
        if (call.items >= 1) slot_to_guest(aTHX_ xsub_slot(aTHX_ call, 0), FAMILY_langinfo);
    }
    xsub(aTHX_ cv);
}

/* POSIX::setlocale(category, locale). */
XS_INTERNAL(xsub_setlocale) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (active()) {
        const XsubCall call = xsub_call(aTHX);
        if (call.items >= 1) slot_to_guest(aTHX_ xsub_slot(aTHX_ call, 0), FAMILY_lc);
    }
    xsub(aTHX_ cv);
}

const struct {
    const char *name;
    XSUBADDR_t routine;
} xsub_routines[] = {
    {"POSIX::SigSet::new", xsub_signals_follow_first},
    {"POSIX::SigSet::addset", xsub_signals_follow_first},
    {"POSIX::SigSet::delset", xsub_signals_follow_first},
    {"POSIX::SigSet::ismember", xsub_signals_follow_first},
    {"POSIX::sigaction", xsub_sigaction},
    {"POSIX::sigprocmask", xsub_sigprocmask},
    {"POSIX::setlocale", xsub_setlocale},
    {"I18N::Langinfo::langinfo", xsub_langinfo},
    {"SDBM_File::TIEHASH", xsub_third_is_open_flags},
    {"IO::Poll::_poll", xsub_poll},
    {"POSIX::open", xsub_posix_open},
    {"Socket::inet_pton", xsub_first_is_family},
    {"Socket::inet_ntop", xsub_first_is_family},
    {"Socket::sockaddr_family", xsub_returns_family},
    {"Socket::getaddrinfo", xsub_getaddrinfo},
    {"Socket::getnameinfo", xsub_getnameinfo},
};

/* ---- constants ------------------------------------------------------------------ */

/* The value of the constant a package has under a name, if it has one:
 * a constant sub, or the reference to a value that stands for one until
 * it is called. */
bool constant_value(pTHX_ HV *stash, const char *name, STRLEN len, long long *value) {
    SV **const entry = hv_fetch(stash, name, (I32)len, 0);
    if (!entry) return false;
    SV *constant = NULL;
    if (SvROK(*entry) && SvTYPE(SvRV(*entry)) < SVt_PVAV) {
        constant = SvRV(*entry);
    } else if (isGV(*entry) && GvCV((GV *)*entry) && CvCONST(GvCV((GV *)*entry))) {
        constant = cv_const_sv(GvCV((GV *)*entry));
    }
    if (!constant || !(SvIOK(constant) || SvNOK(constant))) return false;
    *value = SvIsUV(constant) ? (long long)SvUV(constant) : (long long)SvIV(constant);
    return true;
}

/* The number the program is to see for one of this C library's names. */
long long shown_number(const GuestNumber &g) {
    const Family &f = family(g.family);
    if (f.host.empty()) return g.value;
    for (const Pair &p : f.pairs)
        if (strcmp(p.name, g.name) == 0) return p.host;
    /* the host has no such name */
    return kind_of(g.family) == ENUM ? enum_to_host(g.family, g.value) : 0;
}

/* Names the extensions have for a number that the table knows under
 * another: the protocol families are the address families, and IO::Poll
 * has a name of its own for POLLRDNORM. */
const struct {
    const char *name;
    const char *known_as;
} constant_aliases[] = {
    {"PF_UNSPEC", "AF_UNSPEC"}, {"PF_INET", "AF_INET"},   {"PF_INET6", "AF_INET6"},
    {"PF_UNIX", "AF_UNIX"},     {"PF_LOCAL", "AF_LOCAL"}, {"POLLNORM", "POLLRDNORM"},
};

/* Gives the constant a package has under a name the number the program is
 * to see, when the constant is the number g. */
bool show_constant(pTHX_ HV *stash, const char *name, const GuestNumber &g) {
    const STRLEN len = strlen(name);
    long long now;
    if (!constant_value(aTHX_ stash, name, len, &now) || now != g.value) return false;
    const long long shown = shown_number(g);
    if (shown == now) return false;
    (void)hv_delete(stash, name, (I32)len, G_DISCARD);
    newCONSTSUB(stash, name, newSViv((IV)shown));
    return true;
}

void show_constants_of(pTHX_ HV *stash) {
    bool changed = false;
    for (const GuestNumber &g : guest_numbers) {
        if (show_constant(aTHX_ stash, g.name, g)) changed = true;
        for (const auto &alias : constant_aliases)
            if (strcmp(alias.known_as, g.name) == 0 && show_constant(aTHX_ stash, alias.name, g))
                changed = true;
    }
    if (changed) mro_method_changed_in(stash);
}

/* The packages an extension puts constants into, other than its own. */
const struct {
    const char *module;
    const char *package;
} constant_packages[] = {
    {"IO", "IO::Poll"},
    {"IO", "IO::Handle"},
    {"IO", "IO::Seekable"},
};

} /* namespace */

extern "C" {

/* An errno the host marked as one of its own error numbers that this C
 * library has no number for (the host does so where a system call failed
 * with EHOSTDOWN, say): the number is in the bits below the mark. The
 * errors of this C library end far below it. Native code marks the errors
 * it leaves in errno the same way (the XS SDK). */
const int HOST_ONLY_ERRNO = 0x4000;

static bool is_host_only_errno(int e) { return e >= HOST_ONLY_ERRNO && e < 2 * HOST_ONLY_ERRNO; }

/* The number the program sees for an error number of this C library, and
 * the reverse: for $! and the status of a die. A program that is not shown
 * the host's numbers sees an error only the host has as EIO. */
int host_errno_of(int guest_errno) {
    if (is_host_only_errno(guest_errno)) return active() ? guest_errno - HOST_ONLY_ERRNO : EIO;
    if (!active()) return guest_errno;
    return (int)enum_to_host(FAMILY_errno, guest_errno);
}

int guest_errno_of(long long host_errno) {
    if (!active()) return (int)host_errno;
    return (int)enum_to_guest(FAMILY_errno, host_errno);
}

/* strerror as the interpreter calls it. The C library's own gives the text
 * of "no error" for a number that is none of its errors; such a number is
 * one the program assigned ($! = 9999), or an error of the host's that
 * this C library has no name for, and says so here, with the number the
 * program sees. (--wrap=strerror in wasmify.json.) */
char *__wrap_strerror(int number) {
    if (number == 0 || is_guest_number(FAMILY_errno, number)) return __real_strerror(number);
    if (is_host_only_errno(number) && !active()) return __real_strerror(EIO);
    static char text[40];
    snprintf(text, sizeof(text), "Unknown error %d", host_errno_of(number));
    return text;
}

/* waitpid as the interpreter calls it. The status of a child that a signal
 * ended or stopped holds the signal's number, which the program reads out
 * of $?: it is the host's number then. (The link gives every call of
 * waitpid to this function; --wrap=waitpid in wasmify.json.) */
pid_t __wrap_waitpid(pid_t pid, int *status, int options) {
    const pid_t child = __real_waitpid(pid, status, options);
    if (child <= 0 || !status || !active()) return child;
    const int st = *status;
    if (WIFSIGNALED(st)) {
        *status = (st & ~0x7f) | (int)(to_host(FAMILY_signal, WTERMSIG(st)) & 0x7f);
    } else if (WIFSTOPPED(st)) {
        *status = (st & ~0xff00) | (int)((to_host(FAMILY_signal, WSTOPSIG(st)) & 0xff) << 8);
    }
    return child;
}

/* The built-in functions that take or return numbers of a family are
 * entered through the routines above from now on. Called once, before any
 * program is compiled. */
void numbers_wrap_builtins(void) {
    wrap_builtin(OP_SYSOPEN, builtin_sysopen);
    wrap_builtin(OP_FCNTL, builtin_fcntl);
    wrap_builtin(OP_IOCTL, builtin_ioctl);
    wrap_builtin(OP_SOCKET, builtin_socket);
    wrap_builtin(OP_SOCKPAIR, builtin_socket);
    wrap_builtin(OP_SSOCKOPT, builtin_sockopt);
    wrap_builtin(OP_GSOCKOPT, builtin_sockopt);
    wrap_builtin(OP_SEND, builtin_send);
    wrap_builtin(OP_RECV, builtin_recv);
    wrap_builtin(OP_KILL, builtin_kill);
    wrap_builtin(OP_GHBYNAME, builtin_hostent);
    wrap_builtin(OP_GHBYADDR, builtin_hostent);
    wrap_builtin(OP_GHOSTENT, builtin_hostent);
}

/* An extension has booted: its XSUBs that take or return numbers of a
 * family are entered through the routines above, and, when the program
 * sees the host's numbers, the constants of its packages show them. */
void numbers_extension_booted(pTHX_ const char *module) {
    const size_t module_len = strlen(module);
    for (const auto &r : xsub_routines) {
        /* the routines of this extension: those in its packages */
        if (strncmp(r.name, module, module_len) != 0 || r.name[module_len] != ':') continue;
        CV *const cv = get_cv(r.name, 0);
        if (!cv || !CvISXSUB(cv) || CvXSUB(cv) == r.routine) continue;
        g_xsub_behind[cv] = CvXSUB(cv);
        CvXSUB(cv) = r.routine;
    }
    if (!active()) return;
    if (HV *const stash = gv_stashpv(module, 0)) show_constants_of(aTHX_ stash);
    for (const auto &p : constant_packages) {
        if (strcmp(p.module, module) != 0) continue;
        if (HV *const stash = gv_stashpv(p.package, 0)) show_constants_of(aTHX_ stash);
    }
}

/* Internals::host_numbers(family): the names and numbers of a family of
 * the host's numbers, each name followed by its number; an empty list when
 * the program sees the numbers of this C library, and for a family the
 * host has no table of. */
XS(XS_Internals_host_numbers) {
    dXSARGS;
    if (items != 1) croak_xs_usage(cv, "family");
    STRLEN len;
    const char *const name = SvPV(ST(0), len);
    SP -= items;
    if (active()) {
        for (const FamilyInfo &info : family_infos) {
            if (strlen(info.name) != len || memcmp(info.name, name, len) != 0) continue;
            const Family &f = family(info.id);
            EXTEND(SP, (SSize_t)(2 * f.host.size()));
            for (const auto &h : f.host) {
                mPUSHs(newSVpvn(h.first.data(), h.first.size()));
                mPUSHs(newSViv((IV)h.second));
            }
        }
    }
    PUTBACK;
}

} /* extern "C" */
