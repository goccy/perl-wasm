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
#include <signal.h>
#include <spawn.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
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
    /* perl runs commands with execvp: a bare name is looked up on PATH. */
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
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
    /* what a piped open returns: the pid of the command (my_popen's job) */
    PL_forkprocess = pid;
    SV **svp = av_fetch(PL_fdpid, PerlIO_fileno(fp), TRUE);
    if (svp) {
        SvUPGRADE(*svp, SVt_IV);
        SvIV_set(*svp, (IV)pid);
        SvIOK_on(*svp);
    }
    return fp;
}

/* ---- fork as a pseudo-process ---------------------------------------------
 * WASI has no fork, by design: process creation is spawn-shaped (see
 * above). fork therefore cannot be a system call here; it is emulated one
 * layer up, the way perl itself emulates it on Windows (perlfork): the
 * interpreter is duplicated and the duplicate carries on as the child.
 *
 * The duplication is done by the host. proc_fork snapshots this
 * instance's whole linear memory into a NEW instance (with a copy of the
 * fd table whose slots share the parent's open descriptors, as fork's
 * do) and returns the child's pid here, in the parent. The new instance
 * holds an exact copy of the interpreter — heap, perl stacks, the very op
 * that was forking — but no native call stack: nothing is "inside"
 * pp_fork there. So the host starts the child through
 * wasi_fork_child_run, which finishes the forking op by hand (pushes the
 * child's result, 0) and runs a fresh run loop from the next op, exactly
 * what win32's fork emulation does after perl_clone. The child ends when
 * that run loop ends or the program exits.
 *
 * Two consequences of the missing native stack, both shared with perl's
 * Windows emulation: a child forked while a nested run loop was active (a
 * sort block, a BEGIN block, a tie or overload handler) exits when that
 * nested level finishes instead of returning into C code; and exec in a
 * child cannot replace a process image — it runs the command and exits
 * with its status.
 *
 * wasi_fork_point is how the forking parent tells the child, through the
 * copied memory, where and how to resume. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_fork")))
extern "C" int wasi_host_proc_fork(int *pid_out);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_kill")))
extern "C" int wasi_host_proc_kill(int pid, int sig);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_closefrom")))
extern "C" int wasi_host_fd_closefrom(int low_fd);

enum { WASI_FORK_NONE = 0, WASI_FORK_OP = 1, WASI_FORK_PIPE_OPEN = 2 };
static struct {
    int kind;
    OP *resume_op;
    /* pipe-open only: the pipe end that becomes the child's stdio fd, the
     * end the parent keeps, and the stack height pp_open returns at */
    int child_end;
    int parent_end;
    int stdio_fd;
    SSize_t return_sp;
} wasi_fork_point;

/* Set by pp_open just before do_open6 (configure patch): the stack height
 * the op returns its result at. A pipe-open child needs it to finish
 * pp_open without the C frame that held it. */
extern "C" { SSize_t wasi_open_return_sp = 0; }

/* A pseudo-process has the pid the host registered it under, and the
 * process that forked it for a parent; the first process of an instance has
 * the ids the host gives it (perl_process.cc). */
static pid_t wasi_self_pid = 0;
static pid_t wasi_parent_pid = 0;

extern "C" pid_t wasi_host_pid(void);
extern "C" pid_t wasi_host_ppid(void);
extern "C" void wasi_signal_fork_child(pTHX);

extern "C" pid_t getpid(void) {
    return wasi_self_pid ? wasi_self_pid : wasi_host_pid();
}

extern "C" pid_t getppid(void) {
    return wasi_parent_pid ? wasi_parent_pid : wasi_host_ppid();
}

extern "C" int kill(pid_t pid, int sig) {
    if (sig < 0 || sig >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    /* A signal to the process itself is delivered here and now
     * (perl_signal.cc); any other process is the host's to find. */
    if (pid == getpid()) return raise(sig);
    int rc = wasi_host_proc_kill((int)pid, sig);
    if (rc != 0) {
        errno = rc < 0 ? -rc : rc;
        return -1;
    }
    return 0;
}

/* times(2). wasi-libc's emulation reports process time in clock()'s unit,
 * nanoseconds, while perl divides what times returns by the clock tick it
 * established at start-up (PL_clocktick: sysconf(_SC_CLK_TCK), or its
 * fallback). This one reports in that unit. The process time is the
 * emulated clock's: time elapsed since the instance started, all of it
 * accounted as user time. */
extern "C" clock_t times(struct tms *buf) {
    const long ticks_per_sec = PL_clocktick;
    if (ticks_per_sec <= 0) {
        errno = ENOSYS; /* no interpreter has set the unit yet */
        return (clock_t)-1;
    }
    const clock_t user = (clock_t)((long long)clock() * ticks_per_sec / CLOCKS_PER_SEC);
    if (buf) {
        buf->tms_utime = user;
        buf->tms_stime = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return user;
}

/* Forks at the current op. Returns the child's pid, or -1 with errno set.
 * Only the parent ever returns from this. */
static pid_t wasi_fork_here(pTHX_ int kind) {
    wasi_fork_point.kind = kind;
    wasi_fork_point.resume_op = PL_op->op_next;
    int pid = -1;
    int rc = wasi_host_proc_fork(&pid);
    wasi_fork_point.kind = WASI_FORK_NONE;
    if (rc != 0) {
        errno = rc < 0 ? -rc : rc;
        return -1;
    }
    return (pid_t)pid;
}

/* pp_fork (configure patch routes the whole op here). */
extern "C" OP *wasi_fork(pTHX) {
    dSP;
    EXTEND(SP, 1);
    PERL_FLUSHALL_FOR_CHILD;
    PUTBACK; /* the snapshot must see the stack as the op found it */
    pid_t pid = wasi_fork_here(aTHX_ WASI_FORK_OP);
    if (pid < 0) {
        PUSHs(&PL_sv_undef);
    } else {
        PERL_SRAND_OVERRIDE_NEXT_PARENT();
        mPUSHi((IV)pid);
    }
    PUTBACK;
    return NORMAL;
}

/* Runs a forked child to its end. Called by the host on the NEW instance,
 * once, before anything else runs there. Returns 0 with the child's exit
 * status in *status_out, or -1 when this instance is not a fork child. */
extern "C" int wasi_fork_child_run(pTHX_ int pid, int *status_out) {
    const int kind = wasi_fork_point.kind;
    if (kind == WASI_FORK_NONE) return -1;
    wasi_fork_point.kind = WASI_FORK_NONE;

    wasi_parent_pid = getpid();
    wasi_self_pid = (pid_t)pid;
    /* This instance is new to the host: no signal state of the parent's
     * came along. */
    wasi_signal_fork_child(aTHX);

    /* The jump environments the copied interpreter points at lived in
     * native frames this instance does not have. */
    PL_top_env = &PL_start_env;
    /* What stock pp_fork does in the child. */
    PL_sig_pending = 0;
    if (PL_psig_pend)
        for (int sig = 1; sig < SIG_SIZE; sig++) PL_psig_pend[sig] = 0;
#ifdef PERL_USES_PL_PIDSTATUS
    hv_clear(PL_pidstatus); /* no kids, so don't wait for 'em */
#endif
    PERL_SRAND_OVERRIDE_NEXT_CHILD();
    PL_forkprocess = 0;

    /* Finish the forking op as the child. */
    {
        dSP;
        if (kind == WASI_FORK_PIPE_OPEN) {
            /* my_popen's child half: the pipe end becomes stdin/stdout,
             * and pp_open returns 0 ("we are a new child"). */
            dup2(wasi_fork_point.child_end, wasi_fork_point.stdio_fd);
            close(wasi_fork_point.child_end);
            close(wasi_fork_point.parent_end);
            SP = PL_stack_base + wasi_fork_point.return_sp;
            XPUSHs(&PL_sv_zero);
        } else {
            mPUSHi(0);
        }
        PUTBACK;
    }
    PL_op = wasi_fork_point.resume_op;

    /* From here this is perl_run's own shape (and win32_start_child's):
     * run under a jump environment that fields exit and die. */
    dJMPENV;
    int ret;
    JMPENV_PUSH(ret);
    switch (ret) {
    case 0:
    redo_body:
        if (PL_op) CALLRUNOPS(aTHX);
        /* The program (or the nested level the fork happened in) ran to
         * its end. */
        my_exit(0);
        NOT_REACHED; /* NOTREACHED */
    case 2: /* my_exit() */
        /* This frame is the bottom of the child: it owns every scope. */
        while (PL_scopestack_ix > 1) LEAVE;
        FREETMPS;
        if (PL_curstash != PL_defstash) {
            SvREFCNT_dec(PL_curstash);
            PL_curstash = (HV *)SvREFCNT_inc(PL_defstash);
        }
        *status_out = (int)STATUS_EXIT;
        break;
    case 3: /* die */
        if (PL_restartop) {
            /* caught by an eval the child inherited: resume after it */
            PL_restartjmpenv = NULL;
            PL_op = PL_restartop;
            PL_restartop = NULL;
            goto redo_body;
        }
        /* The die unwound to the eval of the call the PARENT was started
         * with, which has no continuation here: an uncaught die. */
        PerlIO_printf(Perl_error_log, "%" SVf, SVfARG(ERRSV));
        my_failure_exit();
        NOT_REACHED; /* NOTREACHED */
    default:
        PerlIO_printf(Perl_error_log, "panic: fork child: unexpected jump %d\n", ret);
        *status_out = 255;
        break;
    }
    JMPENV_POP;
    return 0;
}

/* pp_exec (configure patch). A process image cannot be replaced, so exec
 * runs the command with this process's current fds and environment, waits
 * for it, and ends the process with the command's status — through _exit,
 * because a replaced program runs neither END blocks nor destructors, and
 * nothing of the interpreter may run once its descriptors are gone.
 * Returns only when the command could not be started, with errno set. */
extern "C" void wasi_exec(pTHX_ SV **mark, SV **sp, int stacked) {
    pid_t pid;
    const char *what;
    if (!stacked && sp - mark == 1) {
        /* exec STRING: shell semantics */
        what = SvPV_nolen(*sp);
        pid = wasi_spawn_shell(what, -1, -1);
    } else {
        SV *really = NULL;
        if (stacked) really = *++mark;
        char **argv = (char **)malloc(sizeof(char *) * (size_t)(sp - mark + 1));
        if (!argv) {
            errno = ENOMEM;
            return;
        }
        int out = 0;
        for (SV **p = mark + 1; p <= sp; p++) argv[out++] = SvPV_nolen(*p);
        argv[out] = NULL;
        if (out == 0) {
            free(argv);
            errno = ENOENT;
            return;
        }
        if (really && SvPOK(really) && SvCUR(really))
            argv[0] = SvPV_nolen(really);
        what = argv[0];
        pid = wasi_spawn_argv(argv, -1, -1);
        free(argv);
    }
    if (pid < 0) {
        const int e = errno;
        Perl_ck_warner(aTHX_ packWARN(WARN_EXEC), "Can't exec \"%s\": %s",
                       what, Strerror(e));
        errno = e;
        return;
    }
    /* The program is gone from here on: only the command remains. Give up
     * every descriptor this process holds, as exec does for the
     * close-on-exec ones (all but stdio, in perl) — the command has its
     * own stdio. A peer waiting for end-of-file on one of them (the status
     * pipe IPC::Open3 reads to learn that exec succeeded) must not wait
     * until the command ends. */
    wasi_host_fd_closefrom(0);
    int status = 0;
    pid_t r;
    do {
        r = waitpid(pid, &status, 0);
    } while (r == -1 && errno == EINTR);
    if (r == -1) _exit(255);
    /* a command killed by a signal reports the shell's 128+signal */
    _exit(WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status));
}

extern "C" PerlIO *wasi_spawn_popen(pTHX_ const char *cmd, const char *mode) {
    if (*cmd == '-' && cmd[1] == '\0') {
        /* open(FH, "-|") / open(FH, "|-"): fork with a pipe between the
         * parent's handle and the child's stdout / stdin. */
        PERL_FLUSHALL_FOR_CHILD;
        int p[2];
        if (pipe(p) < 0) return NULL;
        const bool rd = (*mode == 'r');
        wasi_fork_point.child_end = rd ? p[1] : p[0];
        wasi_fork_point.parent_end = rd ? p[0] : p[1];
        wasi_fork_point.stdio_fd = rd ? 1 : 0;
        wasi_fork_point.return_sp = wasi_open_return_sp;
        pid_t pid = wasi_fork_here(aTHX_ WASI_FORK_PIPE_OPEN);
        if (pid < 0) {
            const int e = errno;
            close(p[0]);
            close(p[1]);
            errno = e;
            return NULL;
        }
        close(wasi_fork_point.child_end);
        PL_forkprocess = pid;
        return wasi_spawn_finish(aTHX_ wasi_fork_point.parent_end, mode, pid);
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