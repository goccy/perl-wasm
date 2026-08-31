/* perl_wasi.cc — perl-runtime adaptations for the WASI build.
 *
 * Everything here exists because perl's own process/IO model assumes
 * primitives WASI does not have, and the adaptation needs PERL internals
 * (PerlIO, PL_fdpid, the context/savestack, pp semantics) — it is not the
 * go-perl API bridge (that is perl.cc) and not a generic libc gap (those
 * live in wasmify's host shims). The configure-time source patches in
 * scripts/wasi-configure.sh route the interpreter here.
 */
#include <EXTERN.h>
#include <perl.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

/* ---- fork-free subprocess (PERL_WASI_SPAWN) ------------------------------
 * WASI has no fork; the runtime's subprocess support is posix_spawn-shaped
 * (wasmify HostSubprocess). These back perl's piped opens and system():
 * util.c's Perl_my_popen/_list route here (configure-time patch), and
 * pp_system's non-fork branch calls system(3), defined below over
 * posix_spawn + waitpid. my_pclose stays stock perl — it just waitpids the
 * pid these record in PL_fdpid. */
/* The bridge is always the wasi build: no external gate. (The perl-core
 * patches key on PERL_WASI_SPAWN from the configured ccflags; this TU is
 * compiled by wasmify with its own flag set.) */

extern char **environ;

static pid_t wasi_spawn_argv(char *const argv[], int child_stdin,
                             int child_stdout) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    /* Default the child's stdio to the guest's CURRENT fds 0/1/2, not the
     * interpreter's original streams: the program may have re-opened
     * STDOUT/STDERR onto files (cpanm's build.log), and children must
     * follow those redirections. The host resolves an unredirected fd to
     * the interpreter stream. */
    posix_spawn_file_actions_adddup2(&fa, child_stdin >= 0 ? child_stdin : 0, 0);
    posix_spawn_file_actions_adddup2(&fa, child_stdout >= 0 ? child_stdout : 1, 1);
    posix_spawn_file_actions_adddup2(&fa, 2, 2);
    pid_t pid = -1;
    int rc = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return pid;
}

static pid_t wasi_spawn_shell(const char *cmd, int child_stdin,
                              int child_stdout) {
    char *argv[4];
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)cmd;
    argv[3] = NULL;
    return wasi_spawn_argv(argv, child_stdin, child_stdout);
}

/* Record the child pid under the stream's fd, the shape stock my_pclose
 * expects (SVt_IV in PL_fdpid). */
static PerlIO *wasi_spawn_finish(pTHX_ int parent_fd, const char *mode,
                                 pid_t pid) {
    PerlIO *fp = PerlIO_fdopen(parent_fd, mode);
    if (!fp) return NULL;
    SV **svp = av_fetch(PL_fdpid, PerlIO_fileno(fp), TRUE);
    if (svp) {
        SvUPGRADE(*svp, SVt_IV);
        SvIV_set(*svp, (IV)pid);
        SvIOK_on(*svp);
    }
    return fp;
}

/* ---- fork emulation by statement replay ----------------------------------
 * The fork-then-exec idioms — my $pid = open FH, "-|" (child execs, parent
 * reads its output) and my $pid = fork (child redirects stdio and execs,
 * parent waitpids) — are how the CPAN toolchain (Module::Build's
 * _backticks, cpanm's run) drives subprocesses. WASI has no fork, but
 * both idioms carry a vfork-shaped contract: the child only adjusts fds
 * and then execs (or exits). That contract can be met without fork by a
 * statement replay:
 *
 *   1. Arming (my_popen("-"), or the pp_fork hook): record the COP of
 *      the forking statement plus the context/savestack depths, save the
 *      stdio fds (a fork child's redirections must not survive into the
 *      parent), and return the CHILD result (pid 0). The program
 *      continues into its child branch in-process. The pipe-open form
 *      also creates the pipe and points the guest's stdout (or stdin)
 *      at it, so the child sees the fds a fork child would.
 *   2. When the child branch execs, the hook pp_exec calls (the
 *      configure patch inserts it) spawns the command against the
 *      guest's current fds, restores the saved stdio, unwinds any scopes
 *      the child branch entered (the same machinery die uses), and
 *      resumes execution AT THE FORKING STATEMENT's COP.
 *   3. The statement re-runs; the arming site now returns the PARENT
 *      result — the spawned pid (fork), or the other pipe end with the
 *      pid in PL_fdpid (pipe-open) — and the parent branch runs.
 *
 * A child branch that exits instead of execing is emulated with a shell
 * child carrying the exit status. As with real vfork, a child branch
 * that neither execs nor exits (or that expects copied state) is outside
 * the contract: it keeps running with $pid == 0. */
enum { WASI_VFORK_FORK = 0, WASI_VFORK_POPEN = 1 };
static struct {
    int phase; /* 0 idle; 1 child branch running; 2 replay pending */
    int kind;
    OP *replay_op;
    I32 cx_ix;
    I32 ss_ix;
    int parent_fd;
    int child_fd;
    char mode;
    pid_t pid;
    int saved_fd[3];
} wasi_vfork;

static void wasi_vfork_arm(pTHX_ int kind) {
    wasi_vfork.kind = kind;
    wasi_vfork.replay_op = (OP *)PL_curcop;
    wasi_vfork.cx_ix = cxstack_ix;
    wasi_vfork.ss_ix = PL_savestack_ix;
    for (int i = 0; i < 3; i++) wasi_vfork.saved_fd[i] = dup(i);
    wasi_vfork.phase = 1;
    PL_forkprocess = 0;
}

static void wasi_vfork_restore_stdio(void) {
    for (int i = 0; i < 3; i++) {
        if (wasi_vfork.saved_fd[i] >= 0) {
            dup2(wasi_vfork.saved_fd[i], i);
            close(wasi_vfork.saved_fd[i]);
            wasi_vfork.saved_fd[i] = -1;
        }
    }
}

/* Ends the child phase: records the spawned pid, restores stdio, unwinds
 * to the forking statement's depths, and returns its COP for the runloop
 * to re-enter. */
static OP *wasi_vfork_resume(pTHX_ pid_t pid) {
    wasi_vfork.pid = pid;
    wasi_vfork.phase = 2;
    /* anything the child branch printed must reach ITS fds (the pipe)
     * before the parent's are restored */
    PerlIO_flush(NULL);
    wasi_vfork_restore_stdio();
    if (cxstack_ix > wasi_vfork.cx_ix)
        dounwind(wasi_vfork.cx_ix);
    LEAVE_SCOPE(wasi_vfork.ss_ix);
    return wasi_vfork.replay_op;
}

/* Called at the top of pp_fork (configure patch): bare fork() arms a
 * child phase with unchanged fds; the replay of the same statement then
 * delivers the spawned pid to the parent branch. */
extern "C" OP *wasi_vfork_fork(pTHX) {
    if (wasi_vfork.phase == 1)
        return NULL; /* fork inside a child branch: outside the contract */
    dSP;
    EXTEND(SP, 1);
    if (wasi_vfork.phase == 2 && wasi_vfork.kind == WASI_VFORK_FORK) {
        wasi_vfork.phase = 0;
        PL_forkprocess = wasi_vfork.pid;
        mPUSHi((IV)wasi_vfork.pid);
        PUTBACK;
        return NORMAL;
    }
    PERL_FLUSHALL_FOR_CHILD;
    wasi_vfork_arm(aTHX_ WASI_VFORK_FORK);
    wasi_vfork.parent_fd = wasi_vfork.child_fd = -1;
    wasi_vfork.mode = 0;
    mPUSHi(0);
    PUTBACK;
    return NORMAL;
}

/* Called at the top of pp_exec (configure patch). Outside a child phase
 * it returns NULL and pp_exec proceeds normally; inside one it spawns
 * the command wired to the fork-pipe and resumes the open statement. */
extern "C" OP *wasi_vfork_exec(pTHX_ SV **mark, SV **sp, int stacked) {
    if (wasi_vfork.phase != 1)
        return NULL;
    /* The child branch may have redirected fds 0/1/2 (the arming site
     * pointed them at the pipe for pipe-opens; cpanm's fork children
     * re-open them onto its log): the spawn's stdio defaults follow the
     * guest's CURRENT fds, which is exactly the fork-child picture. */
    pid_t pid;
    if (!stacked && sp - mark == 1) {
        /* exec STRING: shell semantics */
        pid = wasi_spawn_shell(SvPV_nolen(*sp), -1, -1);
    } else {
        SV *really = NULL;
        if (stacked)
            really = *++mark;
        char **argv = (char **)malloc(sizeof(char *) * (size_t)(sp - mark + 1));
        if (!argv)
            return wasi_vfork_resume(aTHX_ -1);
        int out = 0;
        for (SV **p = mark + 1; p <= sp; p++) argv[out++] = SvPV_nolen(*p);
        argv[out] = NULL;
        if (really && SvPOK(really) && SvCUR(really))
            argv[0] = SvPV_nolen(really);
        pid = wasi_spawn_argv(argv, -1, -1);
        free(argv);
    }
    if (pid < 0) {
        /* exec failure in a real fork child leaves the parent an empty
         * pipe and a child that exited nonzero. */
        pid = wasi_spawn_shell("exit 1", -1, -1);
    }
    return wasi_vfork_resume(aTHX_ pid);
}

/* Called by pp_exit just before my_exit (configure patch): an exit in
 * the child branch becomes a shell child carrying the status. */
extern "C" OP *wasi_vfork_exit(pTHX_ int status) {
    if (wasi_vfork.phase != 1)
        return NULL;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "exit %d", status & 0xff);
    return wasi_vfork_resume(aTHX_ wasi_spawn_shell(cmd, -1, -1));
}

extern "C" PerlIO *wasi_spawn_popen(pTHX_ const char *cmd, const char *mode) {
    if (*cmd == '-' && cmd[1] == '\0') {
        if (wasi_vfork.phase == 2) {
            /* the open statement replaying after the child phase */
            wasi_vfork.phase = 0;
            PL_forkprocess = wasi_vfork.pid;
            return wasi_spawn_finish(aTHX_ wasi_vfork.parent_fd, mode,
                                     wasi_vfork.pid);
        }
        if (wasi_vfork.phase != 0) {
            errno = EAGAIN;
            return NULL;
        }
        PERL_FLUSHALL_FOR_CHILD;
        int p[2];
        if (pipe(p) < 0) return NULL;
        const bool rd = (*mode == 'r');
        wasi_vfork_arm(aTHX_ WASI_VFORK_POPEN);
        wasi_vfork.mode = *mode;
        wasi_vfork.parent_fd = rd ? p[0] : p[1];
        /* Point the guest's stdout (or stdin) at the pipe, like the fork
         * child's fds would be: both a child branch that prints and the
         * command it execs (whose spawn follows the current fds) write
         * into the pipe. The saved fds restore the real stdio at resume. */
        const int stdio_fd = rd ? 1 : 0;
        dup2(rd ? p[1] : p[0], stdio_fd);
        close(rd ? p[1] : p[0]);
        wasi_vfork.child_fd = -1;
        /* the child's handle, like a fork child's: a wrapper over its
         * (now pipe-backed) stdio fd */
        return PerlIO_fdopen(dup(stdio_fd), rd ? "w" : "r");
    }
    PERL_FLUSHALL_FOR_CHILD;
    int p[2];
    if (pipe(p) < 0) return NULL;
    const bool reading = (*mode == 'r');
    pid_t pid = reading ? wasi_spawn_shell(cmd, -1, p[1])
                        : wasi_spawn_shell(cmd, p[0], -1);
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return NULL;
    }
    if (reading) {
        close(p[1]);
        return wasi_spawn_finish(aTHX_ p[0], mode, pid);
    }
    close(p[0]);
    return wasi_spawn_finish(aTHX_ p[1], mode, pid);
}

extern "C" PerlIO *wasi_spawn_popen_list(pTHX_ const char *mode, int n,
                                         SV **args) {
    PERL_FLUSHALL_FOR_CHILD;
    if (n < 1) {
        errno = EINVAL;
        return NULL;
    }
    char **argv = (char **)malloc(sizeof(char *) * (size_t)(n + 1));
    if (!argv) return NULL;
    for (int i = 0; i < n; i++) argv[i] = SvPV_nolen(args[i]);
    argv[n] = NULL;
    int p[2];
    if (pipe(p) < 0) {
        free(argv);
        return NULL;
    }
    const bool reading = (*mode == 'r');
    pid_t pid = reading ? wasi_spawn_argv(argv, -1, p[1])
                        : wasi_spawn_argv(argv, p[0], -1);
    free(argv);
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return NULL;
    }
    if (reading) {
        close(p[1]);
        return wasi_spawn_finish(aTHX_ p[0], mode, pid);
    }
    close(p[0]);
    return wasi_spawn_finish(aTHX_ p[1], mode, pid);
}

/* The platform spawn helpers pp_system's non-fork branch calls (the role
 * win32's do_spawn/do_aspawn play — embed.h maps the calls to the Perl_
 * names): run and wait, returning the native wait status. */
extern "C" int do_spawn(char *cmd) {
    pid_t pid = wasi_spawn_shell(cmd, -1, -1);
    if (pid < 0) return -1;
    int status = 0;
    pid_t r;
    do {
        r = waitpid(pid, &status, 0);
    } while (r == -1 && errno == EINTR);
    return r == -1 ? -1 : status;
}

extern "C" int do_aspawn(void *vreally, void **mark, void **sp) {
    dTHX;
    SV *really = (SV *)vreally;
    SV **m = (SV **)mark;
    SV **s = (SV **)sp;
    int n = (int)(s - m);
    if (n < 1) return -1;
    char **argv = (char **)malloc(sizeof(char *) * (size_t)(n + 1));
    if (!argv) return -1;
    int out = 0;
    for (SV **p = m + 1; p <= s; p++) argv[out++] = SvPV_nolen(*p);
    argv[out] = NULL;
    if (really && SvPOK(really) && SvCUR(really))
        argv[0] = SvPV_nolen(really);
    pid_t pid = wasi_spawn_argv(argv, -1, -1);
    free(argv);
    if (pid < 0) return -1;
    int status = 0;
    pid_t r;
    do {
        r = waitpid(pid, &status, 0);
    } while (r == -1 && errno == EINTR);
    return r == -1 ? -1 : status;
}

/* system(3) over spawn: any C caller lands here. NULL asks "is a shell
 * available" — yes. */
extern "C" int system(const char *cmd) {
    if (!cmd) return 1;
    pid_t pid = wasi_spawn_shell(cmd, -1, -1);
    if (pid < 0) return -1;
    int status = 0;
    pid_t r;
    do {
        r = waitpid(pid, &status, 0);
    } while (r == -1 && errno == EINTR);
    return r == -1 ? -1 : status;
}

/* ---- 4-arg select for pp_sselect (PERL_WASI_SELECT) ----------------------
 * Perl's pp_sselect hands its bit vectors (fd n = bit n%8 of byte n/8)
 * straight to select() as fd_set, which is only correct where fd_set IS
 * that bitmask. wasi-libc's fd_set is {size_t __nfds; int __fds[FD_SETSIZE]}
 * — interpreting bitmask bytes as that struct reads a garbage count and
 * walks off the stack. Convert the bits into real fd_sets, select, and
 * write the ready sets back as bits. The configure patch routes pp_sselect
 * here. */
extern "C" int wasi_bitvec_select(int nbits, char *rbits, char *wbits,
                                  char *ebits, struct timeval *tbuf) {
    fd_set sets[3];
    char *bits[3] = {rbits, wbits, ebits};
    fd_set *use[3] = {NULL, NULL, NULL};
    int maxfd = -1;
    for (int i = 0; i < 3; i++) {
        if (!bits[i]) continue;
        FD_ZERO(&sets[i]);
        use[i] = &sets[i];
        for (int fd = 0; fd < nbits && fd < FD_SETSIZE; fd++) {
            if (bits[i][fd >> 3] & (1 << (fd & 7))) {
                FD_SET(fd, &sets[i]);
                if (fd > maxfd) maxfd = fd;
            }
        }
    }
    int nfound = select(maxfd + 1, use[0], use[1], use[2], tbuf);
    if (nfound < 0) return nfound;
    for (int i = 0; i < 3; i++) {
        if (!bits[i]) continue;
        memset(bits[i], 0, (size_t)((nbits + 7) / 8));
        for (int fd = 0; fd < nbits && fd < FD_SETSIZE; fd++) {
            if (FD_ISSET(fd, use[i]))
                bits[i][fd >> 3] |= (char)(1 << (fd & 7));
        }
    }
    return nfound;
}