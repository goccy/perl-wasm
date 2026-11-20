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
#include <sys/wait.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

#include "wasmify_posix.h"

/* ---- fork-free subprocess (PERL_WASI_SPAWN) ------------------------------
 * WASI has no fork; the runtime's subprocess support is posix_spawn-shaped
 * (wasmify's process.spawn). These back perl's piped opens and system():
 * util.c's Perl_my_popen/_list route here (configure-time patch), and
 * pp_system's non-fork branch calls system(3), defined below over
 * posix_spawn + waitpid. my_pclose stays stock perl — it just waitpids the
 * pid these record in PL_fdpid. */
/* The bridge is always the wasi build: no external gate. (The perl-core
 * patches key on PERL_WASI_SPAWN from the configured ccflags; this TU is
 * compiled by wasmify with its own flag set.) */

extern char **environ;

/* Where the failure to start a command is told, as perl tells it (doio.c,
 * S_exec_failed: "Can't exec", under the exec warnings). */
enum wasi_exec_failure {
    /* in this process: exec, whose failure leaves the program running */
    WASI_EXEC_IN_PLACE,
    /* in the child perl forks for the command: system, a piped open and
     * backticks (wasi_exec_failed_in_child) */
    WASI_EXEC_IN_CHILD
};
static void wasi_exec_failed_in_child(const char *file, int err, int child_stdout,
                                      int child_stderr);

/* Starts the program file with the arguments argv. child_stdin /
 * child_stdout / child_stderr name the fds of this process that become the
 * command's fds 0 / 1 / 2; a negative one stands for this process's own fd
 * of that number. how says where a failure is told. Returns the pid, or -1
 * with errno set. */
static pid_t wasi_spawn_file(const char *file, char *const argv[], int child_stdin,
                             int child_stdout, int child_stderr, int how) {
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
        /* What perl says when its exec fails (doio.c, S_exec_failed); a
         * file that is no program (ENOEXEC) goes to the shell instead, as
         * execvp does. */
        if (rc != ENOEXEC) {
            dTHX;
            if (ckWARN(WARN_EXEC)) {
                if (how == WASI_EXEC_IN_PLACE)
                    Perl_warner(aTHX_ packWARN(WARN_EXEC), "Can't exec \"%s\": %s", file, Strerror(rc));
                else
                    wasi_exec_failed_in_child(file, rc, child_stdout, child_stderr);
            }
        }
        errno = rc;
        return -1;
    }
    return pid;
}

/* The usual case: the program is named by the first argument. */
static pid_t wasi_spawn_argv(char *const argv[], int child_stdin,
                             int child_stdout, int child_stderr, int how) {
    return wasi_spawn_file(argv[0], argv, child_stdin, child_stdout, child_stderr, how);
}

static pid_t wasi_spawn_shell(const char *cmd, int child_stdin,
                              int child_stdout, int how) {
    char *argv[4];
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)cmd;
    argv[3] = NULL;
    return wasi_spawn_argv(argv, child_stdin, child_stdout, -1, how);
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
                                int child_stdout, bool *blank, int how) {
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
            pid = wasi_spawn_argv(argv, child_stdin, child_stdout, child_stderr, how);
            if (pid < 0 && errno == ENOEXEC) shell = true; /* a script without #! */
        }
    }
    if (shell) pid = wasi_spawn_shell(incmd, child_stdin, child_stdout, how);
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
 * pp_fork there. So the host starts the child through the function
 * given to __wasmify_fork, perl_fork_child_entry, which runs
 * wasi_fork_child_run: it finishes the forking op by hand (pushes the
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

enum { WASI_FORK_NONE = 0, WASI_FORK_OP = 1, WASI_FORK_PIPE_OPEN = 2, WASI_FORK_EXEC_FAILED = 3 };
static struct {
    int kind;
    OP *resume_op;
    /* pipe-open only: the pipe end that becomes the child's stdio fd, the
     * end the parent keeps, and the stack height pp_open returns at */
    int child_end;
    int parent_end;
    int stdio_fd;
    SSize_t return_sp;
    /* exec-failed only: the program that could not be started, why, and
     * the fds of the parent that are the command's stdout and stderr (-1
     * for the parent's own) */
    const char *exec_file;
    int exec_errno;
    int exec_stdout;
    int exec_stderr;
} wasi_fork_point;

/* Set by pp_open just before do_open6 (configure patch): the stack height
 * the op returns its result at. A pipe-open child needs it to finish
 * pp_open without the C frame that held it. */
extern "C" { SSize_t wasi_open_return_sp = 0; }

extern "C" void wasi_signal_attach(pTHX);
extern "C" int perl_fork_child_entry(void *arg);

/* Forks at the current op. Returns the child's pid, or -1 with errno set.
 * Only the parent ever returns from this. */
static pid_t wasi_fork_here(pTHX_ int kind) {
    wasi_fork_point.kind = kind;
    wasi_fork_point.resume_op = PL_op ? PL_op->op_next : NULL;
    const pid_t pid = __wasmify_fork(perl_fork_child_entry, &wasi_fork_point);
    wasi_fork_point.kind = WASI_FORK_NONE;
    return pid;
}

/* A command that system, a piped open or backticks could not start. perl
 * forks first, and the child's exec fails: the child says "Can't exec"
 * (doio.c, S_exec_failed), in its copy of the program — its
 * $SIG{__WARN__}, the warnings in scope — with the command's standard
 * handles (a piped open's pipe, the stderr of a trailing 2>&1), and ends;
 * nothing it does reaches the parent, whose handler records nothing. A
 * pseudo-process is that child here (wasi_fork_child_run says it), and
 * this process waits for it as perl's parent waits for its child. Called
 * only when the warning is on: the child would find it on as this process
 * does, and say nothing otherwise. Where no pseudo-process can be made,
 * nothing is said. */
static void wasi_exec_failed_in_child(const char *file, int err, int child_stdout,
                                      int child_stderr) {
    dTHX;
    PERL_FLUSHALL_FOR_CHILD;
    wasi_fork_point.exec_file = file;
    wasi_fork_point.exec_errno = err;
    wasi_fork_point.exec_stdout = child_stdout;
    wasi_fork_point.exec_stderr = child_stderr;
    const pid_t pid = wasi_fork_here(aTHX_ WASI_FORK_EXEC_FAILED);
    if (pid > 0) {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
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

/* Runs a forked child to its end. Called on the NEW instance, once,
 * before anything else runs there. Returns 0 with the child's exit
 * status in *status_out, or -1 when this instance is not a fork child. */
extern "C" int wasi_fork_child_run(pTHX_ int *status_out) {
    const int kind = wasi_fork_point.kind;
    if (kind == WASI_FORK_NONE) return -1;
    wasi_fork_point.kind = WASI_FORK_NONE;

    /* The jump environments the copied interpreter points at lived in
     * native frames this instance does not have. */
    PL_top_env = &PL_start_env;
    /* What stock pp_fork does in the child. */
    PL_sig_pending = 0;
    if (PL_psig_pend)
        for (int sig = 1; sig < SIG_SIZE; sig++) PL_psig_pend[sig] = 0;
    /* This instance is new to the host: no signal state of the parent's
     * came along (the signals the shim held for the parent were dropped
     * before this was entered). Attaching comes after the reset above —
     * the host may already hold a signal for this child, sent the moment
     * it was forked, and raises PL_sig_pending for it right here. */
    wasi_signal_attach(aTHX);
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
            /* A standard handle that is opened anew keeps its file while
             * it is: do_open6 takes the file out of the handle, and gives
             * it back when the open fails, as it does in the child (open
             * returns 0 there). The child does not return through
             * do_open6, so it gives the handle its file back here: the
             * child of open(STDERR, "|-") writes to its STDERR. The handle
             * is pp_open's first argument. */
            SV *const target = PL_stack_base[wasi_fork_point.return_sp + 1];
            if (target && isGV_with_GP(target) && GvIOp((GV *)target) && !IoIFP(GvIOp((GV *)target))) {
                IO *const io = GvIOp((GV *)target);
                if ((GV *)target == PL_stdingv) {
                    IoIFP(io) = PerlIO_stdin();
                    IoTYPE(io) = IoTYPE_RDONLY;
                } else if ((GV *)target == PL_stderrgv) {
                    IoIFP(io) = IoOFP(io) = PerlIO_stderr();
                    IoTYPE(io) = IoTYPE_WRONLY;
                } else if ((GV *)target == gv_fetchpvs("STDOUT", GV_NOTQUAL, SVt_PVIO)) {
                    IoIFP(io) = IoOFP(io) = PerlIO_stdout();
                    IoTYPE(io) = IoTYPE_WRONLY;
                }
            }
            SP = PL_stack_base + wasi_fork_point.return_sp;
            XPUSHs(&PL_sv_zero);
        } else if (kind == WASI_FORK_EXEC_FAILED) {
            /* the child of a command that could not be started: its
             * standard handles are the command's */
            if (wasi_fork_point.exec_stdout >= 0) dup2(wasi_fork_point.exec_stdout, 1);
            if (wasi_fork_point.exec_stderr >= 0) dup2(wasi_fork_point.exec_stderr, 2);
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
        if (kind == WASI_FORK_EXEC_FAILED) {
            /* what perl's child does when its exec fails: it says so, and
             * ends without anything of the program running (a handler
             * that dies inside an eval the child inherited carries on
             * from there, below, as perl's child does) */
            Perl_warner(aTHX_ packWARN(WARN_EXEC), "Can't exec \"%s\": %s", wasi_fork_point.exec_file,
                        Strerror(wasi_fork_point.exec_errno));
            PerlProc__exit(1);
        }
    redo_body:
        if (PL_op) CALLRUNOPS(aTHX);
        /* A process forked while a file was being compiled (in a BEGIN
         * block, or in the code of a module a use loads) comes here when
         * that BEGIN block returns. It cannot go on compiling the file: the
         * compiler ran in frames of the parent that this copy of the
         * process does not have (perlfork, "BEGIN blocks"). It ends as a
         * die does, saying so; one that exits or execs inside the block
         * does not come here. */
        if (PL_parser)
            Perl_croak(aTHX_ "fork inside BEGIN: the child process cannot compile the rest of the file");
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

/* pp_exec (configure patch): starts the command and becomes it
 * (wasmify's __wasmify_exec_become). Returns only when the command could not be started, with
 * errno set. */
extern "C" void wasi_exec(pTHX_ SV **mark, SV **sp, int stacked) {
    pid_t pid;
    if (!stacked && sp - mark == 1) {
        /* exec STRING */
        bool blank = false;
        __wasmify_host_proc_exec_next(1);
        pid = wasi_spawn_command(SvPV_nolen(*sp), -1, -1, &blank, WASI_EXEC_IN_PLACE);
        if (blank) {
            __wasmify_host_proc_exec_next(0);
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
        __wasmify_host_proc_exec_next(1);
        pid = wasi_spawn_argv(argv, -1, -1, -1, WASI_EXEC_IN_PLACE);
        free(argv);
    }
    if (pid < 0) {
        /* (the spawn has said "Can't exec") */
        const int e = errno;
        __wasmify_host_proc_exec_next(0);
        errno = e;
        return;
    }
    /* The program is gone from here on: only the command remains. */
    __wasmify_exec_become(pid);
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
    pid_t pid = reading ? wasi_spawn_command(cmd, -1, p[1], &blank, WASI_EXEC_IN_CHILD)
                        : wasi_spawn_command(cmd, p[0], -1, &blank, WASI_EXEC_IN_CHILD);
    if (blank) {
        /* perl's forked child finds nothing to exec and exits with status
         * 1; the open itself succeeds. A shell that does just that stands
         * in for the child. */
        pid = reading ? wasi_spawn_shell("exit 1", -1, p[1], WASI_EXEC_IN_CHILD)
                      : wasi_spawn_shell("exit 1", p[0], -1, WASI_EXEC_IN_CHILD);
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
    pid_t pid = reading ? wasi_spawn_argv(argv, -1, p[1], -1, WASI_EXEC_IN_CHILD)
                        : wasi_spawn_argv(argv, p[0], -1, -1, WASI_EXEC_IN_CHILD);
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
 * names): run and wait, returning the native wait status.
 *
 * The wait is perl's (wait4pid): a signal that interrupts it has its
 * handler run there, as pp_system's own wait does, and a handler that dies
 * (an alarm that times the command out) leaves system with the command
 * still running. */
static int wasi_spawn_wait(pTHX_ pid_t pid) {
    int status = 0;
    I32 r;
    do {
        r = Perl_wait4pid(aTHX_ pid, &status, 0);
    } while (r == -1 && errno == EINTR);
    return r == -1 ? -1 : status;
}

extern "C" int do_spawn(char *cmd) {
    dTHX;
    bool blank = false;
    pid_t pid = wasi_spawn_command(cmd, -1, -1, &blank, WASI_EXEC_IN_CHILD);
    /* perl's forked child finds nothing to exec and exits with status 255 */
    if (blank) return 255 << 8;
    if (pid < 0) return -1;
    return wasi_spawn_wait(aTHX_ pid);
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
    pid_t pid = wasi_spawn_argv(argv, -1, -1, -1, WASI_EXEC_IN_CHILD);
    free(argv);
    if (pid < 0) return -1;
    return wasi_spawn_wait(aTHX_ pid);
}

/* ---- 4-arg select for pp_sselect (PERL_WASI_SELECT) ----------------------
 * Perl's pp_sselect hands its bit vectors (fd n = bit n%8 of byte n/8)
 * straight to select() as fd_set, which is only correct where fd_set IS
 * that bitmask. wasi-libc's fd_set is {size_t __nfds; int __fds[FD_SETSIZE]}
 * — interpreting bitmask bytes as that struct reads a garbage count and
 * walks off the stack. Convert the bits into real fd_sets, select, and
 * write the ready sets back as bits. The configure patch routes pp_sselect
 * here.
 *
 * The timeout is left holding the time that was not waited, as select(2)
 * does on Linux (perl returns it as the time left): 0 once the whole
 * timeout has passed. wasi-libc's select leaves it as it was. */
static long long select_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

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
    const long long started = tbuf ? select_now_ns() : 0;
    int nfound = select(maxfd + 1, use[0], use[1], use[2], tbuf);
    if (tbuf) {
        const long long timeout = (long long)tbuf->tv_sec * 1000000000LL + (long long)tbuf->tv_usec * 1000LL;
        long long left = timeout - (select_now_ns() - started);
        if (nfound == 0 || left < 0) left = 0;
        tbuf->tv_sec = (time_t)(left / 1000000000LL);
        tbuf->tv_usec = (suseconds_t)((left % 1000000000LL) / 1000LL);
    }
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