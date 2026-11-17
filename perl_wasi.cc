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

/* Starts the program file with the arguments argv. child_stdin /
 * child_stdout / child_stderr name the fds of this process that become the
 * command's fds 0 / 1 / 2; a negative one stands for this process's own fd
 * of that number. Returns the pid, or -1 with errno set. */
static pid_t wasi_spawn_file(const char *file, char *const argv[], int child_stdin,
                             int child_stdout, int child_stderr = -1) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    /* Default the child's stdio to the guest's CURRENT fds 0/1/2, not the
     * interpreter's original streams: the program may have re-opened
     * STDOUT/STDERR onto files (cpanm's build.log), and children must
     * follow those redirections. The host resolves an unredirected fd to
     * the interpreter stream. */
    posix_spawn_file_actions_adddup2(&fa, child_stdin >= 0 ? child_stdin : 0, 0);
    posix_spawn_file_actions_adddup2(&fa, child_stdout >= 0 ? child_stdout : 1, 1);
    posix_spawn_file_actions_adddup2(&fa, child_stderr >= 0 ? child_stderr : 2, 2);
    pid_t pid = -1;
    /* perl runs commands with execvp: a bare name is looked up on PATH. */
    int rc = posix_spawnp(&pid, file, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return pid;
}

/* The usual case: the program is named by the first argument. */
static pid_t wasi_spawn_argv(char *const argv[], int child_stdin,
                             int child_stdout, int child_stderr = -1) {
    return wasi_spawn_file(argv[0], argv, child_stdin, child_stdout, child_stderr);
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

/* Starts a command given as ONE string, by the rule of perl's own exec of a
 * string (doio.c, Perl_do_exec3): a command without shell metacharacters is
 * split into words and run directly — so that a command that does not
 * exist is an error of the caller's (system returns -1 with $! set, a
 * piped open fails) and not a shell's exit status 127 — and only a command
 * that needs the shell goes to sh -c. As there:
 *
 *   - leading white space is skipped;
 *   - ". file", "exec command" and a leading VAR=value go to the shell;
 *   - a single trailing newline is dropped;
 *   - a trailing " 2>&1" is honoured without a shell: the command's
 *     stderr becomes its stdout;
 *   - a file that cannot be executed as a program (ENOEXEC) is given to
 *     the shell after all.
 *
 * Returns the pid, or -1 with errno set. A command with no word in it
 * starts nothing: -1 with errno 0, and *blank set. */
static pid_t wasi_spawn_command(const char *incmd, int child_stdin,
                                int child_stdout, bool *blank) {
    *blank = false;
    while (*incmd && isSPACE(*incmd)) incmd++;
    const size_t len = strlen(incmd);
    char *cmd = (char *)malloc(len + 1);
    char **argv = (char **)malloc(sizeof(char *) * (len / 2 + 2));
    if (!cmd || !argv) {
        free(cmd);
        free(argv);
        errno = ENOMEM;
        return -1;
    }
    memcpy(cmd, incmd, len + 1);

    bool shell = false;
    int child_stderr = -1;
    char *end = cmd + len; /* where the words end */
    if ((*cmd == '.' && isSPACE(cmd[1])) || (strncmp(cmd, "exec", 4) == 0 && isSPACE(cmd[4]))) {
        shell = true;
    } else {
        char *s = cmd;
        while (isWORDCHAR(*s)) s++; /* catch VAR=val gizmo */
        if (*s == '=') shell = true;
    }
    for (char *s = cmd; !shell && *s; s++) {
        if (*s == ' ' || isALPHA(*s) || !memCHRs("$&*(){}[]'\";\\|?<>~`\n", *s)) continue;
        if (*s == '\n' && !s[1]) {
            end = s;
            break;
        }
        /* the 2>&1 construct at the end */
        if (*s == '>' && s[1] == '&' && s[2] == '1' && s > cmd + 1 && s[-1] == '2' &&
            isSPACE(s[-2]) && (!s[3] || isSPACE(s[3]))) {
            const char *t = s + 3;
            while (*t && isSPACE(*t)) ++t;
            if (!*t) {
                child_stderr = child_stdout >= 0 ? child_stdout : 1;
                end = s - 2;
                break;
            }
        }
        shell = true;
    }

    pid_t pid = -1;
    if (!shell) {
        *end = '\0';
        char **a = argv;
        for (char *s = cmd; *s;) {
            while (isSPACE(*s)) s++;
            if (*s) *(a++) = s;
            while (*s && !isSPACE(*s)) s++;
            if (*s) *s++ = '\0';
        }
        *a = NULL;
        if (!argv[0]) {
            *blank = true;
            errno = 0;
        } else {
            pid = wasi_spawn_argv(argv, child_stdin, child_stdout, child_stderr);
            if (pid < 0 && errno == ENOEXEC) shell = true; /* a script without #! */
        }
    }
    if (shell) pid = wasi_spawn_shell(incmd, child_stdin, child_stdout);
    const int e = errno;
    free(cmd);
    free(argv);
    errno = e;
    return pid;
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

/* Waits for the command pid in the command's name: from here on every
 * signal sent to this process goes to the command. Returns 0 with the
 * command's wait status, or a negative errno. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_exec")))
extern "C" int wasi_host_proc_exec(int pid, int *status_out);

/* Says (on 1) that the next command this process starts is the one it
 * becomes: the host then starts it with what the process made of its
 * session (a process that leads a session of its own is replaced by a
 * command that leads one, with its controlling terminal). on 0 takes it
 * back, for a command that could not be started. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_exec_next")))
extern "C" int wasi_host_proc_exec_next(int on);

/* Ends the process as one killed by sig (perl_signal.cc uses it for the
 * default action of a signal). */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_terminate"), noreturn))
extern "C" void wasi_host_proc_terminate(int sig);

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

extern "C" void wasi_signal_fork_child(pTHX);

extern "C" int kill(pid_t pid, int sig) {
    if (sig < 0 || sig >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    /* A signal to the process itself is the host's to deliver when the
     * process is one the host knows by that pid: the host process, whose
     * action for the signal may be native code's handler and not the
     * interpreter's, or a fork child. It is delivered by the time kill
     * returns either way. Otherwise (no host process has the pid: the
     * program is a process of its own) it is delivered here and now
     * (perl_signal.cc). Any other process, and a process group (a pid of 0
     * or less), is the host's to find. */
    if (pid > 0 && pid == getpid()) {
        if (sig == 0) return 0;
        const int rc = wasi_host_proc_kill((int)pid, sig);
        if (rc == 0) return 0;
        if (rc != -ESRCH && rc != ESRCH) {
            errno = rc < 0 ? -rc : rc;
            return -1;
        }
        return raise(sig);
    }
    int rc = wasi_host_proc_kill((int)pid, sig);
    if (rc != 0) {
        errno = rc < 0 ? -rc : rc;
        return -1;
    }
    return 0;
}

/* The CPU time of the process, in nanoseconds: user and system time of the
 * process itself, then of the children it has waited for. 0 or a negative
 * errno. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_times")))
extern "C" int wasi_host_proc_times(uint64_t times[4]);

/* times(2). wasi-libc's emulation reports process time in clock()'s unit,
 * nanoseconds, while perl divides what times returns by the clock tick it
 * established at start-up (PL_clocktick: sysconf(_SC_CLK_TCK), or its
 * fallback). This one reports in that unit, and what the host accounts to
 * the process: user and system time, its own and its children's. Where the
 * host keeps no such account the process time is the emulated clock's:
 * time elapsed since the instance started, all of it user time. */
extern "C" clock_t times(struct tms *buf) {
    const long ticks_per_sec = PL_clocktick;
    if (ticks_per_sec <= 0) {
        errno = ENOSYS; /* no interpreter has set the unit yet */
        return (clock_t)-1;
    }
    const clock_t elapsed = (clock_t)((long long)clock() * ticks_per_sec / CLOCKS_PER_SEC);
    uint64_t nanos[4] = {0, 0, 0, 0};
    const bool accounted = wasi_host_proc_times(nanos) == 0;
    if (buf) {
        if (accounted) {
            const uint64_t per_tick = 1000000000ull / (uint64_t)ticks_per_sec;
            buf->tms_utime = (clock_t)(nanos[0] / per_tick);
            buf->tms_stime = (clock_t)(nanos[1] / per_tick);
            buf->tms_cutime = (clock_t)(nanos[2] / per_tick);
            buf->tms_cstime = (clock_t)(nanos[3] / per_tick);
        } else {
            buf->tms_utime = elapsed;
            buf->tms_stime = 0;
            buf->tms_cutime = 0;
            buf->tms_cstime = 0;
        }
    }
    return elapsed;
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
    /* The host knows this instance as the process pid: getpid and getppid
     * ask it (perl_process.cc). */
    (void)pid;

    /* The jump environments the copied interpreter points at lived in
     * native frames this instance does not have. */
    PL_top_env = &PL_start_env;
    /* What stock pp_fork does in the child. */
    PL_sig_pending = 0;
    if (PL_psig_pend)
        for (int sig = 1; sig < SIG_SIZE; sig++) PL_psig_pend[sig] = 0;
    /* This instance is new to the host: no signal state of the parent's
     * came along. Attaching comes after the reset above — the host may
     * already hold a signal for this child, sent the moment it was forked,
     * and raises PL_sig_pending for it right here. */
    wasi_signal_fork_child(aTHX);
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

/* The second half of exec. A process image cannot be replaced, so exec
 * starts the command with this process's current fds and environment
 * (wasi_spawn_argv and friends) and then this process stands in for it
 * until it ends:
 *
 *   - It gives up every descriptor it holds, as exec does for the
 *     close-on-exec ones (all but stdio, in perl) — the command has its own
 *     stdio. A peer waiting for end-of-file on one of them (the status pipe
 *     IPC::Open3 reads to learn that exec succeeded) must not wait until the
 *     command ends.
 *   - It waits in the host, which passes every signal this process is sent
 *     on to the command: the pid others know is this one's, and the program
 *     that had the handlers is gone. SIGKILL included.
 *   - It ends the way the command ended: with its exit status, or killed by
 *     the signal that killed it, so that whoever waits for this process
 *     sees what happened to the command.
 *
 * All of it without running anything of the interpreter (_exit, not exit):
 * a replaced program runs neither END blocks nor destructors. */
__attribute__((noreturn)) static void wasi_become(pid_t pid) {
    wasi_host_fd_closefrom(0);
    int status = 0;
    if (wasi_host_proc_exec((int)pid, &status) != 0) _exit(255);
    if (WIFSIGNALED(status)) wasi_host_proc_terminate(WTERMSIG(status));
    _exit(WEXITSTATUS(status));
}

/* execv and execvp, for the callers that exec without going through
 * pp_exec: the lexer, when the #! line of the program names another
 * interpreter (toke.c hands the program to it), and POSIX-level code. They
 * return only when the command could not be started, with errno set. */
extern "C" int execv(const char *path, char *const argv[]) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, 0, 0);
    posix_spawn_file_actions_adddup2(&fa, 1, 1);
    posix_spawn_file_actions_adddup2(&fa, 2, 2);
    pid_t pid = -1;
    wasi_host_proc_exec_next(1);
    const int rc = posix_spawn(&pid, path, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        wasi_host_proc_exec_next(0);
        errno = rc;
        return -1;
    }
    wasi_become(pid);
}

extern "C" int execvp(const char *file, char *const argv[]) {
    wasi_host_proc_exec_next(1);
    const pid_t pid = wasi_spawn_file(file, argv, -1, -1);
    if (pid < 0) {
        const int e = errno;
        wasi_host_proc_exec_next(0);
        errno = e;
        return -1;
    }
    wasi_become(pid);
}

/* pp_exec (configure patch): starts the command and becomes it
 * (wasi_become). Returns only when the command could not be started, with
 * errno set. */
extern "C" void wasi_exec(pTHX_ SV **mark, SV **sp, int stacked) {
    pid_t pid;
    const char *what;
    if (!stacked && sp - mark == 1) {
        /* exec STRING */
        bool blank = false;
        what = SvPV_nolen(*sp);
        wasi_host_proc_exec_next(1);
        pid = wasi_spawn_command(what, -1, -1, &blank);
        if (blank) {
            wasi_host_proc_exec_next(0);
            return; /* nothing to run: exec fails, as perl's does */
        }
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
        wasi_host_proc_exec_next(1);
        pid = wasi_spawn_argv(argv, -1, -1);
        free(argv);
    }
    if (pid < 0) {
        const int e = errno;
        wasi_host_proc_exec_next(0);
        Perl_ck_warner(aTHX_ packWARN(WARN_EXEC), "Can't exec \"%s\": %s",
                       what, Strerror(e));
        errno = e;
        return;
    }
    /* The program is gone from here on: only the command remains. */
    wasi_become(pid);
}

extern "C" PerlIO *wasi_spawn_popen(pTHX_ const char *cmd, const char *mode) {
    if (*cmd == '-' && cmd[1] == '\0') {
        /* open(FH, "-|") / open(FH, "|-"): fork with a pipe between the
         * parent's handle and the child's stdout / stdin. */
        PERL_FLUSHALL_FOR_CHILD;
        int p[2];
        if (Perl_PerlProc_pipe_cloexec(aTHX_ p) < 0) return NULL;
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
    /* Close-on-exec, as perl's own piped open makes its pipe: the command
     * gets one end as its stdin or stdout, and must not inherit the pair
     * under their own numbers as well — a writer that holds the read end of
     * its own pipe never learns that the reader has gone. */
    int p[2];
    if (Perl_PerlProc_pipe_cloexec(aTHX_ p) < 0) return NULL;
    const bool reading = (*mode == 'r');
    bool blank = false;
    pid_t pid = reading ? wasi_spawn_command(cmd, -1, p[1], &blank)
                        : wasi_spawn_command(cmd, p[0], -1, &blank);
    if (blank) {
        /* perl's forked child finds nothing to exec and exits with status
         * 1; the open itself succeeds. A shell that does just that stands
         * in for the child. */
        pid = reading ? wasi_spawn_shell("exit 1", -1, p[1])
                      : wasi_spawn_shell("exit 1", p[0], -1);
    }
    if (pid < 0) {
        const int e = errno;
        close(p[0]);
        close(p[1]);
        errno = e;
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
    if (Perl_PerlProc_pipe_cloexec(aTHX_ p) < 0) {
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
    bool blank = false;
    pid_t pid = wasi_spawn_command(cmd, -1, -1, &blank);
    /* perl's forked child finds nothing to exec and exits with status 255 */
    if (blank) return 255 << 8;
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