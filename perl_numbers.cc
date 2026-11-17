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
 *     IO::Poll, ...): when an extension has booted, the value of every
 *     constant in its packages that names a number of a family becomes the
 *     host's number (show_constants_of); Errno.pm asks for the host's whole
 *     table (Internals::host_numbers);
 *   - the built-in functions and the XSUBs that take or return such
 *     numbers: each is entered through a routine that turns the program's
 *     numbers into the C library's and the results back
 *     (numbers_wrap_builtins, numbers_extension_booted);
 *   - the status of a child ($?), whose low bits are a signal number: where
 *     waitpid hands it to the interpreter (the link wraps waitpid);
 *   - the packed socket addresses, which are structures with a family number
 *     in them: see "socket addresses" below.
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
#include "scripts/include/termios.h"
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

/* A request of ioctl that this C library does not answer, made of the host:
 * request is the host's number, and the argument is a number (buffer 0:
 * arg is it) or memory the request reads and writes (arg_len bytes at arg,
 * in the host's layout). What the call returned is written at result. 0,
 * or a negative errno: EINVAL for a request that is not one of a
 * terminal's. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_ioctl")))
int wasi_host_fd_ioctl(int fd, unsigned int request, int buffer, uintptr_t arg,
                       unsigned int arg_len, int *result);

/* the C library's waitpid and strerror: the link gives the names to
 * __wrap_waitpid and __wrap_strerror, below */
pid_t __real_waitpid(pid_t pid, int *status, int options);
char *__real_strerror(int number);

/* A socket address of this C library as the host's C library has it, and
 * the reverse (wasmify's host sockets). 0 or an errno. */
int __wasmify_sockaddr_to_host(const struct sockaddr *addr, socklen_t addrlen, void *out,
                               size_t cap, size_t *out_len);
int __wasmify_sockaddr_from_host(const void *raw, size_t len, struct sockaddr *out,
                                 socklen_t *out_len);
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
 * sides becomes the other side's. Where one side has one bit for two names
 * of the other (this C library's POLLIN is its POLLRDNORM; linux has a bit
 * for each), the one bit becomes both: the caller that knows which were
 * asked for keeps those (see xsub_poll). A name whose value is 0 on one
 * side (O_NOCTTY here) stands for nothing there. Bits without a
 * counterpart are reported in *unknown.
 *
 * The flags of open(2) hold a field that is not a set of bits, the access
 * mode: O_RDONLY, O_WRONLY and O_RDWR are values of the field O_ACCMODE
 * masks, and are translated as such. */
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
    long long known = 0;
    for (const Pair &p : f.pairs) {
        if (id == FAMILY_open && is_access_name(p.name)) continue;
        const long long from = to_host ? p.guest : p.host;
        if (from == 0 || (rest & from) != from) continue;
        out |= to_host ? p.host : p.guest;
        known |= from;
    }
    *unknown |= rest & ~known;
    return out;
}

/* The host's number under a name of a family; 0 when it has none. */
long long host_number(int id, const char *name) {
    for (const auto &h : family(id).host)
        if (h.first == name) return h.second;
    return 0;
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

/* ---- socket addresses ----------------------------------------------------------
 * A program holds a socket address as a string, the bytes of a struct
 * sockaddr, and hands it to the system as it is. The structures of this C
 * library are not the host's: the family is another number, an IPv6 or a
 * Unix-domain address has another size, some systems put a length byte in
 * front. A program that is shown the host's numbers holds the host's
 * structures, so that an address it gets from native code (a resolver's
 * result), packs itself (pack 'S n a4 x8', AF_INET, ...) or hands to native
 * code is one the system and the native code read alike. Each place where
 * a packed address passes between the program and the interpreter's C code
 * turns it into the other side's:
 *
 *   - the built-ins bind, connect and send (its TO) take one; accept,
 *     getpeername, getsockname and recv return one;
 *   - Socket's pack_sockaddr_in, pack_sockaddr_in6 and pack_sockaddr_un
 *     return one, unpack_sockaddr_in, unpack_sockaddr_in6,
 *     unpack_sockaddr_un, sockaddr_family and getnameinfo take one, and
 *     getaddrinfo returns them.
 *
 * A string that is no address of a family both have is passed on as it
 * is: the system refuses it as it refuses any string that is no address. */

/* The bytes of a packed address; at most this many are an address. */
union AddressBytes {
    struct sockaddr sa;
    char bytes[256];
};

/* The C library's address for a packed address of the host's: a new value,
 * or the program's own where it is no address of the host's. */
SV *guest_address(pTHX_ SV *host) {
    SvGETMAGIC(host);
    if (!SvOK(host) || !SvPOK(host)) return host;
    SV *bytes = host;
    if (SvUTF8(host)) {
        /* the built-ins take the bytes of a string */
        bytes = sv_2mortal(newSVsv_nomg(host));
        if (!sv_utf8_downgrade(bytes, TRUE)) return host;
    }
    STRLEN len;
    const char *const raw = SvPV_nomg(bytes, len);
    AddressBytes guest;
    socklen_t guest_len = sizeof(guest);
    if (__wasmify_sockaddr_from_host(raw, len, &guest.sa, &guest_len) != 0) return host;
    if (guest_len > sizeof(guest)) return host;
    return sv_2mortal(newSVpvn(guest.bytes, guest_len));
}

/* Puts the host's address in a stack slot that holds a packed address of
 * the C library's; a slot that holds no address is left as it is. */
void address_slot_to_host(pTHX_ SV **slot) {
    SV *const sv = *slot;
    if (!SvPOK(sv) || SvCUR(sv) < sizeof(sa_family_t) || SvCUR(sv) > sizeof(AddressBytes)) return;
    AddressBytes guest;
    memcpy(guest.bytes, SvPVX(sv), SvCUR(sv));
    char host[sizeof(AddressBytes)];
    size_t host_len = 0;
    if (__wasmify_sockaddr_to_host(&guest.sa, (socklen_t)SvCUR(sv), host, sizeof(host), &host_len) != 0) return;
    *slot = sv_2mortal(newSVpvn(host, host_len));
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

/* ioctl FILEHANDLE, FUNCTION, SCALAR. A request this C library answers
 * (perl_numbers.inc) goes to it, under its number. Every other request is
 * the host's: one of a terminal's, say, whose number the program has from
 * a native module or a header of the host. It goes to the host as it is,
 * and what pp_ioctl does around the call is done here: the third argument
 * is a number, or a buffer the request reads and writes, which is at least
 * as long as a request takes (IOCPARM_LEN, 256 where the system does not
 * say) and guarded against a request that writes more. */
OP *builtin_ioctl(pTHX) {
    if (!active()) return behind(aTHX);
    dSP;
    SV *const function = SP[-1];
    SvGETMAGIC(function);
    const long long request = SvOK(function) ? (long long)SvUV_nomg(function) : 0;
    const Family &requests = family(FAMILY_ioctl);
    if (requests.host.empty()) return behind(aTHX);
    for (const Pair &p : requests.pairs) {
        if (p.host == request) {
            SP[-1] = sv_2mortal(newSViv((IV)p.guest));
            return behind(aTHX);
        }
    }

    dTARGET;
    SV *const argsv = POPs;
    (void)POPs;
    GV *const gv = MUTABLE_GV(POPs);
    IO *const io = GvIOn(gv);
    if (!IoIFP(io)) {
        Perl_report_evil_fh(aTHX_ gv);
        SETERRNO(EBADF, RMS_IFI);
        RETPUSHUNDEF;
    }
    const bool buffer = SvPOK(argsv) || !SvNIOK(argsv);
    char *s = NULL;
    IV value = 0;
    if (buffer) {
        const STRLEN need = 256;
        STRLEN len;
        s = SvPV_force(argsv, len);
        if (len < need) {
            s = Sv_Grow(argsv, need + 1);
            SvCUR_set(argsv, need);
        }
        s[SvCUR(argsv)] = 17; /* the guard pp_ioctl puts behind the buffer */
    } else {
        value = SvIV(argsv);
    }
    TAINT_PROPER(PL_op_desc[OP_IOCTL]);
    int result = 0;
    const int rc = wasi_host_fd_ioctl(PerlIO_fileno(IoIFP(io)), (unsigned int)request, buffer,
                                      buffer ? (uintptr_t)s : (uintptr_t)value,
                                      buffer ? (unsigned int)SvCUR(argsv) : 0, &result);
    IV retval = result;
    if (rc != 0) {
        errno = -rc;
        retval = -1;
    }
    if (SvPOK(argsv)) {
        if (s[SvCUR(argsv)] != 17)
            DIE(aTHX_ "Possible memory corruption: %s overflowed 3rd argument", OP_NAME(PL_op));
        s[SvCUR(argsv)] = 0;
        SvSETMAGIC(argsv);
    }
    if (retval == -1) RETPUSHUNDEF;
    if (retval != 0) {
        PUSHi(retval);
    } else {
        PUSHp("0 but true", 10);
    }
    RETURN;
}

/* socket SOCKET, DOMAIN, TYPE, PROTOCOL and socketpair SOCKET1, SOCKET2,
 * DOMAIN, TYPE, PROTOCOL. */
OP *builtin_socket(pTHX) {
    if (!active()) return behind(aTHX);
    dSP;
    slot_to_guest(aTHX_ SP - 2, FAMILY_af);
    SV *const type = *(SP - 1);
    *(SP - 1) = sv_2mortal(newSViv((IV)socktype_to_guest((long long)SvIV(type))));
    /* The protocol of a Unix-domain socket is 0 or the number of its
     * protocol family (PF_UNIX), which is a number of that family. */
    if (SvIV(*(SP - 2)) == AF_UNIX) slot_to_guest(aTHX_ SP, FAMILY_af);
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

/* The flags a program gives send and recv. MSG_TRUNC and MSG_CTRUNC are
 * left out: this C library has them only as what a receive reports, under
 * a bit that is MSG_PEEK when given, and a receive asked to tell the real
 * length of a datagram must not become one that leaves the datagram
 * unread. */
void message_flags_to_guest(pTHX_ SV **slot) {
    SV *const sv = *slot;
    SvGETMAGIC(sv);
    if (!SvOK(sv)) return;
    const long long reported = host_number(FAMILY_msg, "MSG_TRUNC") | host_number(FAMILY_msg, "MSG_CTRUNC");
    const long long flags = (long long)SvIV_nomg(sv);
    *slot = sv_2mortal(newSViv((IV)to_guest(FAMILY_msg, flags & ~reported)));
}

/* send SOCKET, MSG, FLAGS [, TO] and recv SOCKET, SCALAR, LENGTH, FLAGS:
 * the flags are the third and the fourth argument; send's TO is a packed
 * address, and so is what recv returns (the sender's). */
OP *builtin_send(pTHX) {
    if (!active()) return behind(aTHX);
    dSP;
    SV **const mark = PL_stack_base + TOPMARK;
    if (SP - mark >= 3) message_flags_to_guest(aTHX_ mark + 3);
    if (SP - mark >= 4) mark[4] = guest_address(aTHX_ mark[4]);
    return behind(aTHX);
}

OP *builtin_recv(pTHX) {
    if (!active()) return behind(aTHX);
    dSP;
    SV **const mark = PL_stack_base + TOPMARK;
    if (SP - mark >= 4) message_flags_to_guest(aTHX_ mark + 4);
    OP *const next = behind(aTHX);
    address_slot_to_host(aTHX_ PL_stack_sp);
    return next;
}

/* bind SOCKET, NAME and connect SOCKET, NAME: the name is a packed
 * address. */
OP *builtin_address_in(pTHX) {
    if (!active()) return behind(aTHX);
    dSP;
    *SP = guest_address(aTHX_ *SP);
    return behind(aTHX);
}

/* accept NEWSOCKET, GENERICSOCKET, getpeername SOCKET and getsockname
 * SOCKET return a packed address. */
OP *builtin_address_out(pTHX) {
    if (!active()) return behind(aTHX);
    OP *const next = behind(aTHX);
    address_slot_to_host(aTHX_ PL_stack_sp);
    return next;
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
 * poll, and the XSUB writes the returned events over them. The events of a
 * descriptor are among those its mask asked for, and the three that are
 * reported unasked: an event this C library has one bit for and the host
 * two (readable: POLLIN and POLLRDNORM) comes back as the bits that were
 * asked for. */
XS_INTERNAL(xsub_poll) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (!active()) {
        xsub(aTHX_ cv);
        return;
    }
    const XsubCall call = xsub_call(aTHX);
    const long long unasked = host_number(FAMILY_poll, "POLLERR") |
                              host_number(FAMILY_poll, "POLLHUP") |
                              host_number(FAMILY_poll, "POLLNVAL");
    std::vector<std::pair<SV *, long long>> masks;
    for (SSize_t i = 2; i < call.items; i += 2) {
        SV *const mask = *xsub_slot(aTHX_ call, i);
        const long long asked = (long long)SvIV(mask);
        sv_setiv(mask, (IV)to_guest(FAMILY_poll, asked));
        masks.emplace_back(mask, asked);
    }
    xsub(aTHX_ cv);
    for (const auto &m : masks)
        sv_setiv(m.first, (IV)(to_host(FAMILY_poll, (long long)SvIV(m.first)) & (m.second | unasked)));
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

/* Socket::sockaddr_family(sockaddr) returns the address family of a packed
 * address. */
XS_INTERNAL(xsub_returns_family) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (!active()) {
        xsub(aTHX_ cv);
        return;
    }
    const XsubCall call = xsub_call(aTHX);
    if (call.items >= 1) *xsub_slot(aTHX_ call, 0) = guest_address(aTHX_ *xsub_slot(aTHX_ call, 0));
    xsub(aTHX_ cv);
    if (xsub_returned(aTHX_ call) == 1) {
        SV **const result = xsub_slot(aTHX_ call, 0);
        if (SvOK(*result)) *result = sv_2mortal(newSViv((IV)to_host(FAMILY_af, (long long)SvIV(*result))));
    }
}

/* Socket::pack_sockaddr_in, pack_sockaddr_in6 and pack_sockaddr_un return
 * a packed address. */
XS_INTERNAL(xsub_returns_address) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (!active()) {
        xsub(aTHX_ cv);
        return;
    }
    const XsubCall call = xsub_call(aTHX);
    xsub(aTHX_ cv);
    if (xsub_returned(aTHX_ call) >= 1) address_slot_to_host(aTHX_ xsub_slot(aTHX_ call, 0));
}

/* Socket::unpack_sockaddr_in, unpack_sockaddr_in6 and unpack_sockaddr_un
 * take a packed address. */
XS_INTERNAL(xsub_takes_address) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (active()) {
        const XsubCall call = xsub_call(aTHX);
        if (call.items >= 1) *xsub_slot(aTHX_ call, 0) = guest_address(aTHX_ *xsub_slot(aTHX_ call, 0));
    }
    xsub(aTHX_ cv);
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
 * family and a socket type; each result names a family and a socket type,
 * and holds a packed address. */
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
        SV **const addr = hv_fetchs(MUTABLE_HV(SvRV(result)), "addr", 0);
        if (addr) {
            SV *host = *addr;
            address_slot_to_host(aTHX_ &host);
            if (host != *addr) sv_setsv(*addr, host);
        }
    }
}

/* Socket::getnameinfo(addr, flags, xflags): the address is a packed one. */
XS_INTERNAL(xsub_getnameinfo) {
    const XSUBADDR_t xsub = xsub_behind(cv);
    if (!active()) {
        xsub(aTHX_ cv);
        return;
    }
    const XsubCall call = xsub_call(aTHX);
    if (call.items >= 1) *xsub_slot(aTHX_ call, 0) = guest_address(aTHX_ *xsub_slot(aTHX_ call, 0));
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
    {"Socket::pack_sockaddr_in", xsub_returns_address},
    {"Socket::pack_sockaddr_in6", xsub_returns_address},
    {"Socket::pack_sockaddr_un", xsub_returns_address},
    {"Socket::unpack_sockaddr_in", xsub_takes_address},
    {"Socket::unpack_sockaddr_in6", xsub_takes_address},
    {"Socket::unpack_sockaddr_un", xsub_takes_address},
};

/* ---- constants ------------------------------------------------------------------
 * An extension makes each of its constants once, in its boot function: a
 * value, held either by a constant sub or by the reference that stands in
 * the package for one until the name is first looked up. Everything a
 * program can reach the constant by leads to that one value: the sub under
 * its name (which may be older than the extension's boot: a call compiled
 * before a `require`, a \&Fcntl::O_CREAT taken early), the same sub
 * imported into another package, a reference to it, ->can, and the copy
 * the compiler puts in place of a call.
 *
 * The invariant: a program that is shown the host's numbers never sees the
 * C library's number in such a value. It holds because the value itself is
 * given the host's number, in place, in the step that follows the
 * extension's boot function and before any code of the program or of the
 * module can run (XS_static_extension_bootstrap is the only way a boot
 * function is entered). Nothing is replaced, so nothing that already holds
 * the sub or the value is left with the other number. */

/* The value of the constant a package has under a name, if it has one:
 * that of a constant sub, or the value a reference stands for until the
 * name is looked up. */
SV *constant_of(pTHX_ HV *stash, const char *name, STRLEN len) {
    SV **const entry = hv_fetch(stash, name, (I32)len, 0);
    if (!entry) return NULL;
    if (SvROK(*entry) && SvTYPE(SvRV(*entry)) < SVt_PVAV) return SvRV(*entry);
    if (isGV(*entry) && GvCV((GV *)*entry) && CvCONST(GvCV((GV *)*entry)))
        return cv_const_sv(GvCV((GV *)*entry));
    return NULL;
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
 * another: the protocol families are the address families, IO::Poll has a
 * name of its own for POLLRDNORM, and Fcntl has old names of flags of open
 * (wasi-posix-shim.h). */
const struct {
    const char *name;
    const char *known_as;
} constant_aliases[] = {
    {"PF_UNSPEC", "AF_UNSPEC"}, {"PF_INET", "AF_INET"},   {"PF_INET6", "AF_INET6"},
    {"PF_UNIX", "AF_UNIX"},     {"PF_LOCAL", "AF_LOCAL"}, {"POLLNORM", "POLLRDNORM"},
    {"O_NDELAY", "O_NONBLOCK"}, {"FNDELAY", "O_NONBLOCK"}, {"FNONBLOCK", "O_NONBLOCK"},
    {"FAPPEND", "O_APPEND"},
};

/* Gives the constant a package has under a name the number the program is
 * to see, when the constant is the number g: the value is changed where it
 * is (see the invariant above). The truth values are no numbers an
 * extension made. */
void show_constant(pTHX_ HV *stash, const char *name, const GuestNumber &g) {
    SV *const constant = constant_of(aTHX_ stash, name, strlen(name));
    if (!constant || SvIMMORTAL(constant) || !(SvIOK(constant) || SvNOK(constant))) return;
    const long long now = SvIsUV(constant) ? (long long)SvUV(constant) : (long long)SvIV(constant);
    if (now != g.value) return;
    const long long shown = shown_number(g);
    if (shown == now) return;
    const bool readonly = SvREADONLY(constant);
    SvREADONLY_off(constant);
    sv_setiv(constant, (IV)shown);
    if (readonly) SvREADONLY_on(constant);
}

void show_constants_of(pTHX_ HV *stash) {
    for (const GuestNumber &g : guest_numbers) {
        show_constant(aTHX_ stash, g.name, g);
        for (const auto &alias : constant_aliases)
            if (strcmp(alias.known_as, g.name) == 0) show_constant(aTHX_ stash, alias.name, g);
    }
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

/* The text of each error, by its name in C, as perl on Linux shows it in $!
 * (glibc's), and, for the names glibc does not have, as perl on macOS does
 * (darwin's). The C library of the interpreter words most of them
 * otherwise ("I/O error" for EIO, "Message too large" for EMSGSIZE), and a
 * program or its tests compare the text of $! with perl's. */
const struct {
    const char *name;
    const char *text;
} error_texts[] = {
    {"EPERM", "Operation not permitted"},
    {"ENOENT", "No such file or directory"},
    {"ESRCH", "No such process"},
    {"EINTR", "Interrupted system call"},
    {"EIO", "Input/output error"},
    {"ENXIO", "No such device or address"},
    {"E2BIG", "Argument list too long"},
    {"ENOEXEC", "Exec format error"},
    {"EBADF", "Bad file descriptor"},
    {"ECHILD", "No child processes"},
    {"EAGAIN", "Resource temporarily unavailable"},
    {"EWOULDBLOCK", "Resource temporarily unavailable"},
    {"ENOMEM", "Cannot allocate memory"},
    {"EACCES", "Permission denied"},
    {"EFAULT", "Bad address"},
    {"ENOTBLK", "Block device required"},
    {"EBUSY", "Device or resource busy"},
    {"EEXIST", "File exists"},
    {"EXDEV", "Invalid cross-device link"},
    {"ENODEV", "No such device"},
    {"ENOTDIR", "Not a directory"},
    {"EISDIR", "Is a directory"},
    {"EINVAL", "Invalid argument"},
    {"ENFILE", "Too many open files in system"},
    {"EMFILE", "Too many open files"},
    {"ENOTTY", "Inappropriate ioctl for device"},
    {"ETXTBSY", "Text file busy"},
    {"EFBIG", "File too large"},
    {"ENOSPC", "No space left on device"},
    {"ESPIPE", "Illegal seek"},
    {"EROFS", "Read-only file system"},
    {"EMLINK", "Too many links"},
    {"EPIPE", "Broken pipe"},
    {"EDOM", "Numerical argument out of domain"},
    {"ERANGE", "Numerical result out of range"},
    {"EDEADLK", "Resource deadlock avoided"},
    {"EDEADLOCK", "Resource deadlock avoided"},
    {"ENAMETOOLONG", "File name too long"},
    {"ENOLCK", "No locks available"},
    {"ENOSYS", "Function not implemented"},
    {"ENOTEMPTY", "Directory not empty"},
    {"ELOOP", "Too many levels of symbolic links"},
    {"ENOMSG", "No message of desired type"},
    {"EIDRM", "Identifier removed"},
    {"ECHRNG", "Channel number out of range"},
    {"EL2NSYNC", "Level 2 not synchronized"},
    {"EL3HLT", "Level 3 halted"},
    {"EL3RST", "Level 3 reset"},
    {"ELNRNG", "Link number out of range"},
    {"EUNATCH", "Protocol driver not attached"},
    {"ENOCSI", "No CSI structure available"},
    {"EL2HLT", "Level 2 halted"},
    {"EBADE", "Invalid exchange"},
    {"EBADR", "Invalid request descriptor"},
    {"EXFULL", "Exchange full"},
    {"ENOANO", "No anode"},
    {"EBADRQC", "Invalid request code"},
    {"EBADSLT", "Invalid slot"},
    {"EBFONT", "Bad font file format"},
    {"ENOSTR", "Device not a stream"},
    {"ENODATA", "No data available"},
    {"ETIME", "Timer expired"},
    {"ENOSR", "Out of streams resources"},
    {"ENONET", "Machine is not on the network"},
    {"ENOPKG", "Package not installed"},
    {"EREMOTE", "Object is remote"},
    {"ENOLINK", "Link has been severed"},
    {"EADV", "Advertise error"},
    {"ESRMNT", "Srmount error"},
    {"ECOMM", "Communication error on send"},
    {"EPROTO", "Protocol error"},
    {"EMULTIHOP", "Multihop attempted"},
    {"EDOTDOT", "RFS specific error"},
    {"EBADMSG", "Bad message"},
    {"EOVERFLOW", "Value too large for defined data type"},
    {"ENOTUNIQ", "Name not unique on network"},
    {"EBADFD", "File descriptor in bad state"},
    {"EREMCHG", "Remote address changed"},
    {"ELIBACC", "Can not access a needed shared library"},
    {"ELIBBAD", "Accessing a corrupted shared library"},
    {"ELIBSCN", ".lib section in a.out corrupted"},
    {"ELIBMAX", "Attempting to link in too many shared libraries"},
    {"ELIBEXEC", "Cannot exec a shared library directly"},
    {"EILSEQ", "Invalid or incomplete multibyte or wide character"},
    {"ERESTART", "Interrupted system call should be restarted"},
    {"ESTRPIPE", "Streams pipe error"},
    {"EUSERS", "Too many users"},
    {"ENOTSOCK", "Socket operation on non-socket"},
    {"EDESTADDRREQ", "Destination address required"},
    {"EMSGSIZE", "Message too long"},
    {"EPROTOTYPE", "Protocol wrong type for socket"},
    {"ENOPROTOOPT", "Protocol not available"},
    {"EPROTONOSUPPORT", "Protocol not supported"},
    {"ESOCKTNOSUPPORT", "Socket type not supported"},
    {"ENOTSUP", "Operation not supported"},
    {"EOPNOTSUPP", "Operation not supported"},
    {"EPFNOSUPPORT", "Protocol family not supported"},
    {"EAFNOSUPPORT", "Address family not supported by protocol"},
    {"EADDRINUSE", "Address already in use"},
    {"EADDRNOTAVAIL", "Cannot assign requested address"},
    {"ENETDOWN", "Network is down"},
    {"ENETUNREACH", "Network is unreachable"},
    {"ENETRESET", "Network dropped connection on reset"},
    {"ECONNABORTED", "Software caused connection abort"},
    {"ECONNRESET", "Connection reset by peer"},
    {"ENOBUFS", "No buffer space available"},
    {"EISCONN", "Transport endpoint is already connected"},
    {"ENOTCONN", "Transport endpoint is not connected"},
    {"ESHUTDOWN", "Cannot send after transport endpoint shutdown"},
    {"ETOOMANYREFS", "Too many references: cannot splice"},
    {"ETIMEDOUT", "Connection timed out"},
    {"ECONNREFUSED", "Connection refused"},
    {"EHOSTDOWN", "Host is down"},
    {"EHOSTUNREACH", "No route to host"},
    {"EALREADY", "Operation already in progress"},
    {"EINPROGRESS", "Operation now in progress"},
    {"ESTALE", "Stale file handle"},
    {"EUCLEAN", "Structure needs cleaning"},
    {"ENOTNAM", "Not a XENIX named type file"},
    {"ENAVAIL", "No XENIX semaphores available"},
    {"EISNAM", "Is a named type file"},
    {"EREMOTEIO", "Remote I/O error"},
    {"EDQUOT", "Disk quota exceeded"},
    {"ENOMEDIUM", "No medium found"},
    {"EMEDIUMTYPE", "Wrong medium type"},
    {"ECANCELED", "Operation canceled"},
    {"ENOKEY", "Required key not available"},
    {"EKEYEXPIRED", "Key has expired"},
    {"EKEYREVOKED", "Key has been revoked"},
    {"EKEYREJECTED", "Key was rejected by service"},
    {"EOWNERDEAD", "Owner died"},
    {"ENOTRECOVERABLE", "State not recoverable"},
    {"ERFKILL", "Operation not possible due to RF-kill"},
    {"EHWPOISON", "Memory page has hardware error"},
    /* names glibc does not have: darwin's texts */
    {"EAUTH", "Authentication error"},
    {"EBADARCH", "Bad CPU type in executable"},
    {"EBADEXEC", "Bad executable (or shared library)"},
    {"EBADMACHO", "Malformed Mach-o file"},
    {"EBADRPC", "RPC struct is bad"},
    {"EDEVERR", "Device error"},
    {"EFTYPE", "Inappropriate file type or format"},
    {"ELAST", "Capabilities insufficient"},
    {"ENEEDAUTH", "Need authenticator"},
    {"ENOATTR", "Attribute not found"},
    {"ENOPOLICY", "Policy not found"},
    {"ENOTCAPABLE", "Capabilities insufficient"},
    {"EPROCLIM", "Too many processes"},
    {"EPROCUNAVAIL", "Bad procedure for program"},
    {"EPROGMISMATCH", "Program version wrong"},
    {"EPROGUNAVAIL", "RPC prog. not avail"},
    {"EPWROFF", "Device power is off"},
    {"EQFULL", "Interface output queue is full"},
    {"ERPCMISMATCH", "RPC version wrong"},
    {"ESHLIBVERS", "Shared library version mismatch"},
};

const char *error_text(const char *name) {
    for (const auto &e : error_texts)
        if (strcmp(e.name, name) == 0) return e.text;
    return NULL;
}

/* The name in C of an error number of this C library, or NULL. */
const char *guest_error_name(int number) {
    for (const GuestNumber &g : guest_numbers)
        if (g.family == FAMILY_errno && g.value == number) return g.name;
    return NULL;
}

/* The name in C of an error number of the host's, or NULL. */
const char *host_error_name(long long number) {
    for (const auto &h : family(FAMILY_errno).host)
        if (h.second == number) return h.first.c_str();
    return NULL;
}

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

/* The flags of open(2) as the program sees them, for flags this C library
 * makes up for the program (dbmopen's, which TIEHASH takes in the
 * program's numbers). */
UV wasi_program_open_flags(UV flags) {
    if (!active()) return flags;
    return (UV)to_host(FAMILY_open, (long long)flags);
}

/* strerror as the interpreter calls it: the text of an error is glibc's
 * for its name (error_texts). An error of the host's that this C library
 * has no number for has the text of the host's name for it: one a system
 * call failed with (marked HOST_ONLY_ERRNO), or one the program assigned to
 * $! by the host's number; a program that is not shown the host's numbers
 * sees the former as EIO. A number that is no error at all is one the
 * program assigned ($! = 9999); the C library's own gives the text of "no
 * error" for it, and it is "Unknown error" here, with the number the
 * program sees, as glibc says. (--wrap=strerror in wasmify.json.) */
char *__wrap_strerror(int number) {
    if (number == 0) return __real_strerror(number);
    const char *name = NULL;
    if (is_host_only_errno(number)) {
        if (active()) name = host_error_name(number - HOST_ONLY_ERRNO);
        else number = EIO;
    }
    if (!name) name = guest_error_name(number);
    if (!name && active()) name = host_error_name(host_errno_of(number));
    if (name) {
        if (const char *text = error_text(name)) return (char *)text;
        if (is_guest_number(FAMILY_errno, number)) return __real_strerror(number);
    }
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
    wrap_builtin(OP_BIND, builtin_address_in);
    wrap_builtin(OP_CONNECT, builtin_address_in);
    wrap_builtin(OP_ACCEPT, builtin_address_out);
    wrap_builtin(OP_GETPEERNAME, builtin_address_out);
    wrap_builtin(OP_GETSOCKNAME, builtin_address_out);
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
