/* perl_program.cc — runs a whole program on an interpreter that is
 * already up.
 *
 * perl_run_program runs a program the way the perl command does after it has
 * read its command line: perl.c's S_parse_body from open_script on, then
 * perl_run. perl_parse itself cannot be called again — it starts by
 * replacing the main stash, which would drop everything the instance has
 * loaded — and the command line is the host's to interpret, so the host
 * hands over what the switches MEAN (pl.h documents the layout) and this
 * code puts the interpreter in the state perl's switch loop would have.
 *
 * This file is compiled as perl core code (PERL_CORE): it is a rendering
 * of perl.c's own start-up sequence and uses the same private entry
 * points — the grammar, the pad allocator, the lexer flags. */
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#define PERL_CORE
extern "C" {
#include <EXTERN.h>
#include <perl.h>
#include <perliol.h>
}

#include "perl_program.h"

/* perl.c's read_e_script: the source filter that feeds inline program text
 * to the lexer a line at a time. */
static I32 program_read_text(pTHX_ int idx, SV *buf_sv, int maxlen) {
    PERL_UNUSED_ARG(idx);
    PERL_UNUSED_ARG(maxlen);
    const char *const p = SvPVX_const(PL_e_script);
    const char *const e = SvEND(PL_e_script);
    const char *nl = static_cast<const char *>(memchr(p, '\n', e - p));
    nl = nl ? nl + 1 : e;
    if (nl - p == 0) {
        filter_del(program_read_text);
        return 0;
    }
    sv_catpvn(buf_sv, p, nl - p);
    sv_chop(PL_e_script, nl);
    return 1;
}

/* perl.c's S_find_beginning (-x): skip input up to the #! line that names
 * perl, and apply the switches on it. */
static void program_find_beginning(pTHX_ SV *linestr_sv, PerlIO *rsfp) {
    const char *s;
    const char *s2;
    do {
        if ((s = sv_gets(linestr_sv, rsfp, 0)) == NULL)
            croak("No Perl script found in input\n");
        s2 = s;
    } while (!(*s == '#' && s[1] == '!' &&
               ((s = instr(s, "perl")) || (s = instr(s2, "PERL")))));
    PerlIO_ungetc(rsfp, '\n'); /* to keep line count right */
    while (*s && !(isSPACE(*s) || *s == '#')) s++;
    s2 = s;
    while (*s == ' ' || *s == '\t') s++;
    if (*s++ == '-') {
        while (isDIGIT(s2[-1]) || s2[-1] == '-' || s2[-1] == '.' || s2[-1] == '_') s2--;
        if (strBEGINs(s2 - 4, "perl"))
            while ((s = moreswitches(s)))
                ;
    }
}

/* perl.c's file-local SET_CURSTASH(PL_defstash). */
static void program_set_curstash_main(pTHX) {
    if (PL_curstash != PL_defstash) {
        SvREFCNT_dec(PL_curstash);
        PL_curstash = (HV *)SvREFCNT_inc(PL_defstash);
    }
}

/* perl.c's S_incpush as -I uses it: the version- and architecture-specific
 * subdirectories of dir that exist go in ahead of dir itself. */
static void program_incpush(pTHX_ AV *av, const std::string &dir) {
    static const char *const subdirs[] = {
        "/" PERL_FS_VERSION "/" ARCHNAME,
        "/" PERL_FS_VERSION,
        "/" ARCHNAME,
    };
    for (const char *sub : subdirs) {
        SV *path = newSVpvn(dir.data(), dir.size());
        sv_catpv(path, sub);
        Stat_t st;
        if (PerlLIO_stat(SvPVX_const(path), &st) >= 0 && S_ISDIR(st.st_mode))
            av_push(av, path);
        else
            SvREFCNT_dec(path);
    }
    av_push(av, newSVpvn(dir.data(), dir.size()));
}

/* Whether a standard handle is a terminal is decided when its buffer is
 * made (PerlIOBuf_pushed): output to a terminal goes out line by line, and
 * what is waiting for a terminal is written before input is read. The
 * handles of this interpreter were made when the interpreter was, which
 * may be a copy of one that was started with other streams; the program
 * runs with the descriptors it has now, so the two flags are set from
 * those. */
static void program_standard_terminals(pTHX) {
    PerlIO *const handles[] = {PerlIO_stdin(), PerlIO_stdout(), PerlIO_stderr()};
    for (PerlIO *handle : handles) {
        if (!handle || !PerlIOValid(handle)) continue;
        const int fd = PerlIO_fileno(handle);
        const bool terminal = fd >= 0 && PerlLIO_isatty(fd);
        for (PerlIO *layer = handle; PerlIOValid(layer); layer = PerlIONext(layer)) {
            if (!(PerlIOBase(layer)->tab->kind & PERLIO_K_BUFFERED)) continue;
            if (terminal)
                PerlIOBase(layer)->flags |= PERLIO_F_LINEBUF | PERLIO_F_TTY;
            else
                PerlIOBase(layer)->flags &= ~(PERLIO_F_LINEBUF | PERLIO_F_TTY);
        }
    }
}

/* The part of S_parse_body that follows the switch loop, with the switch
 * loop's effects taken from ps. Croaks like perl_parse does. */
static void program_compile(pTHX_ const ProgramSpec &ps) {
    /* What perl_parse does before parse_body. */
    if (PL_main_root) {
        op_free(PL_main_root);
        PL_main_root = NULL;
    }
    PL_main_start = NULL;
    SvREFCNT_dec(PL_main_cv);
    PL_main_cv = NULL;
    /* $^T is when the program started. The interpreter may be a copy of one
     * that was started long before (an instance made from a snapshot of
     * the process's first), and would otherwise keep that one's. */
    time(&PL_basetime);
    program_standard_terminals(aTHX);
    PL_dowarn = G_WARN_OFF;
    PERL_SET_PHASE(PERL_PHASE_START);

    /* The compile-time state a fresh interpreter starts a program with. */
    PL_curcop = &PL_compiling;
    CopLINE_set(&PL_compiling, 0);
    CopSTASH_set(&PL_compiling, PL_defstash);
    program_set_curstash_main(aTHX);
    PL_hints = 0;

    /* The switches. */
    switch (ps.warnings) {
    case PROG_WARN_ON:
        PL_dowarn |= G_WARN_ON;
        break;
    case PROG_WARN_ALL:
        PL_dowarn = G_WARN_ALL_ON | G_WARN_ON;
        free_and_set_cop_warnings(&PL_compiling, pWARN_ALL);
        break;
    case PROG_WARN_NONE:
        PL_dowarn = G_WARN_ALL_OFF;
        free_and_set_cop_warnings(&PL_compiling, pWARN_NONE);
        break;
    }
#ifndef NO_TAINT_SUPPORT
    if (ps.taint != PROG_TAINT_OFF) {
        TAINTING_set(TRUE);
        TAINT_WARN_set(ps.taint == PROG_TAINT_WARN);
        /* $^X and %ENV exist since perl_parse, from before tainting was
         * on; under -T perl creates both tainted (perl.c: set_caret_X and
         * init_postdump_symbols run inside TAINT). */
        GV *caret_x = gv_fetchpvs("\030", GV_ADD | GV_NOTQUAL, SVt_PV);
        if (caret_x) sv_taint(GvSV(caret_x));
        if (PL_envgv) {
            HV *env = GvHVn(PL_envgv);
            HE *he;
            hv_iterinit(env);
            while ((he = hv_iternext(env))) sv_taint(HeVAL(he));
        }
        /* and @INC without what the environment contributed */
        program_inc_drop_environment(aTHX);
    }
#endif
    PL_minus_n = cBOOL(ps.flags & PROG_LOOP);
    PL_minus_p = cBOOL(ps.flags & PROG_LOOP_PRINT);
    PL_minus_a = cBOOL(ps.flags & PROG_AUTOSPLIT);
    PL_minus_F = cBOOL(ps.flags & PROG_SPLIT_PATTERN);
    if (ps.flags & PROG_SPLIT_PATTERN) {
        Safefree(PL_splitstr);
        PL_splitstr = savepvn(ps.split.data(), ps.split.size());
    }
    PL_minus_c = cBOOL(ps.flags & PROG_SYNTAX_ONLY);
    PL_minus_E = cBOOL(ps.flags & PROG_FEATURES);
    PL_unsafe = cBOOL(ps.flags & PROG_UNSAFE);
    if (ps.flags & PROG_RS_SET) {
        SvREFCNT_dec(PL_rs);
        if (ps.flags & PROG_RS_UNDEF) {
            PL_rs = &PL_sv_undef;
        } else {
            PL_rs = newSVpvn(ps.rs.data(), ps.rs.size());
            if (ps.flags & PROG_RS_UTF8) SvUTF8_on(PL_rs);
        }
        sv_setsv(get_sv("/", GV_ADD), PL_rs);
    }
    if (ps.flags & PROG_LINE_ENDINGS) {
        PL_minus_l = TRUE;
        SvREFCNT_dec(PL_ors_sv);
        PL_ors_sv = ps.ors.empty() ? NULL : newSVpvn(ps.ors.data(), ps.ors.size());
    }
    if (ps.flags & PROG_INPLACE) {
        Safefree(PL_inplace);
        PL_inplace = savepvn(ps.inplace.data(), ps.inplace.size());
    }
    if (ps.flags & PROG_UNICODE) {
        PL_unicode = ps.unicode;
        if (PL_unicode & PERL_UNICODE_UTF8CACHEASSERT_FLAG) PL_utf8cache = -1;
    }
    if ((ps.flags & PROG_DEBUGGER) && !PL_perldb) {
        PL_perldb = PERLDB_ALL;
        init_debugger();
    }
    for (const std::string &stmt : ps.preamble)
        Perl_av_create_and_push(aTHX_ &PL_preambleav,
                                newSVpvn(stmt.data(), stmt.size()));
    if (!ps.inc.empty()) {
        AV *front = newAV();
        for (const std::string &dir : ps.inc) program_incpush(aTHX_ front, dir);
        AV *inc = GvAVn(PL_incgv);
        const SSize_t n = static_cast<SSize_t>(av_count(front));
        av_unshift(inc, n);
        for (SSize_t i = 0; i < n; i++)
            av_store(inc, i, SvREFCNT_inc(*av_fetch(front, i, FALSE)));
        SvREFCNT_dec(front);
    }

    /* The command line as perl remembers it (PL_origargv). The lexer reads
     * it when the #! line of the program names another interpreter: the
     * program is handed to that interpreter, with these arguments — perl's
     * own switches among them, as they were typed. The interpreter was
     * started without a program, so the vector is put together here from
     * what the host said: perl, then the words of the command line when the
     * host has one, and otherwise the program and its arguments. As when
     * perl_parse itself runs a second time, the vector is not the process's
     * real argument area, and assigning to $0 must not write into it:
     * PL_origalen is 1. The storage is this function's own: perl keeps the
     * pointers, not copies. */
    {
        static std::vector<std::string> words;
        static std::vector<char *> argv;
        /* the name perl was started under; on a later run it is in words */
        const std::string name(PL_origargv && PL_origargv[0] ? PL_origargv[0] : "perl");
        words.clear();
        words.push_back(name);
        if (ps.has_command_line) {
            for (const std::string &w : ps.command_line) words.push_back(w);
        } else {
            switch (ps.source) {
            case PROG_SOURCE_TEXT:
                words.emplace_back("-e");
                words.push_back(ps.code);
                break;
            case PROG_SOURCE_STDIN:
                words.emplace_back("-");
                break;
            default:
                words.push_back(ps.path);
                break;
            }
            for (const std::string &a : ps.args) words.push_back(a);
        }
        argv.clear();
        for (std::string &w : words) argv.push_back(&w[0]);
        argv.push_back(NULL);
        PL_origargc = static_cast<int>(words.size());
        PL_origargv = argv.data();
        PL_origalen = 1;
    }

    /* open_script */
    PerlIO *rsfp = NULL;
    SV *linestr_sv = NULL;
    U32 lex_start_flags = 0;
    Safefree(PL_origfilename);
    SvREFCNT_dec(PL_e_script);
    PL_e_script = NULL;
    switch (ps.source) {
    case PROG_SOURCE_TEXT:
        PL_e_script = newSVpvn(ps.code.data(), ps.code.size());
        PL_origfilename = savepvs("-e");
        break;
    case PROG_SOURCE_STDIN:
        PL_origfilename = savepvs("-");
        break;
    default:
        PL_origfilename = savepvn(ps.path.data(), ps.path.size());
        break;
    }
    CopFILE_free(PL_curcop);
    CopFILE_set(PL_curcop, PL_origfilename);
    if (ps.source == PROG_SOURCE_STDIN) {
        rsfp = PerlIO_stdin();
        lex_start_flags = LEX_DONT_CLOSE_RSFP;
    } else {
        /* Inline text is fed by a source filter over the bit bucket. */
        rsfp = PerlIO_open(PL_e_script ? BIT_BUCKET : PL_origfilename, PERL_SCRIPT_MODE);
        if (!rsfp) {
            if (PL_e_script)
                croak("Can't open " BIT_BUCKET ": %s\n", Strerror(errno));
            croak("Can't open perl script \"%s\": %s\n", CopFILE(PL_curcop),
                  Strerror(errno));
        }
        Stat_t st;
        int fd = PerlIO_fileno(rsfp);
        if (fd < 0 || (PerlLIO_fstat(fd, &st) >= 0 && S_ISDIR(st.st_mode)))
            croak("Can't open perl script \"%s\": %s\n", CopFILE(PL_curcop),
                  Strerror(EISDIR));
    }
    if (ps.flags & PROG_EXTRACT) {
        linestr_sv = newSV_type(SVt_PV);
        lex_start_flags |= LEX_START_COPIED;
        program_find_beginning(aTHX_ linestr_sv, rsfp);
        if (!ps.extract_dir.empty() && PerlDir_chdir(ps.extract_dir.c_str()) < 0)
            croak("Can't chdir to %s", ps.extract_dir.c_str());
    }

    PL_main_cv = PL_compcv = MUTABLE_CV(newSV_type(SVt_PVCV));
    CvUNIQUE_on(PL_compcv);
    CvPADLIST_set(PL_compcv, pad_new(0));

    /* init_postdump_symbols: $0, the -s variables, @ARGV */
    {
        GV *zero = gv_fetchpvs("0", GV_ADD | GV_NOTQUAL, SVt_PV);
        TAINT;
        if (zero) sv_setpv(GvSV(zero), PL_origfilename);
        TAINT_NOT;
    }
    for (const ProgramDefine &d : ps.defines) {
        GV *gv = gv_fetchpvn_flags(d.name.data(), d.name.size(), GV_ADD, SVt_PV);
        if (!gv) continue;
        if (d.has_value)
            sv_setpvn(GvSV(gv), d.value.data(), d.value.size());
        else
            sv_setiv(GvSV(gv), 1);
    }
    {
        const bool argv_utf8 =
            (!(PL_unicode & PERL_UNICODE_LOCALE_FLAG) || PL_utf8locale) &&
            (PL_unicode & PERL_UNICODE_ARGV_FLAG);
        AV *argv = GvAVn(PL_argvgv);
        av_clear(argv);
        TAINT;
        for (const std::string &a : ps.args) {
            SV *sv = newSVpvn(a.data(), a.size());
            SvTAINT(sv);
            av_push(argv, sv);
            if (argv_utf8) SvUTF8_on(sv);
            if (PL_unicode & PERL_UNICODE_WIDESYSCALLS_FLAG) (void)sv_utf8_decode(sv);
        }
        TAINT_NOT;
    }

    /* -C: the standard handles and the default open layers */
    if ((ps.flags & PROG_UNICODE) && PL_unicode &&
        (!(PL_unicode & PERL_UNICODE_LOCALE_FLAG) || PL_utf8locale)) {
        IO *io;
        PerlIO *fp;
        SV *sv;
        if ((PL_unicode & PERL_UNICODE_STDIN_FLAG) && PL_stdingv &&
            (io = GvIO(PL_stdingv)) && (fp = IoIFP(io)))
            PerlIO_binmode(aTHX_ fp, IoTYPE(io), 0, ":utf8");
        if ((PL_unicode & PERL_UNICODE_STDOUT_FLAG) && PL_defoutgv &&
            (io = GvIO(PL_defoutgv)) && (fp = IoOFP(io)))
            PerlIO_binmode(aTHX_ fp, IoTYPE(io), 0, ":utf8");
        if ((PL_unicode & PERL_UNICODE_STDERR_FLAG) && PL_stderrgv &&
            (io = GvIO(PL_stderrgv)) && (fp = IoOFP(io)))
            PerlIO_binmode(aTHX_ fp, IoTYPE(io), 0, ":utf8");
        if ((PL_unicode & PERL_UNICODE_INOUT_FLAG) &&
            (sv = GvSV(gv_fetchpvs("\017PEN", GV_ADD | GV_NOTQUAL, SVt_PV)))) {
            U32 in = PL_unicode & PERL_UNICODE_IN_FLAG;
            U32 out = PL_unicode & PERL_UNICODE_OUT_FLAG;
            if (in) {
                if (out)
                    sv_setpvs(sv, ":utf8\0:utf8");
                else
                    sv_setpvs(sv, ":utf8\0");
            } else if (out)
                sv_setpvs(sv, "\0:utf8");
            SvSETMAGIC(sv);
        }
    }

    ENTER;
    lex_start(linestr_sv, rsfp, lex_start_flags);
    SvREFCNT_dec(linestr_sv);

    SvREFCNT_dec(PL_subname);
    PL_subname = newSVpvs("main");

    if (PL_e_script)
        filter_add(program_read_text, NULL);
    else
        PL_hints |= HINTS_DEFAULT;

    SETERRNO(0, SS_NORMAL);
    if (yyparse(GRAMPROG) || PL_parser->error_count)
        abort_execution(NULL, PL_origfilename);
    CopLINE_set(PL_curcop, 0);
    program_set_curstash_main(aTHX);
    if (PL_e_script) {
        SvREFCNT_dec(PL_e_script);
        PL_e_script = NULL;
    }
    if (isWARN_ONCE) {
        SAVECOPFILE(PL_curcop);
        SAVECOPLINE(PL_curcop);
        gv_check(PL_defstash);
    }
    LEAVE;
    FREETMPS;
    PL_restartjmpenv = NULL;
    PL_restartop = 0;
}

/* The JMPENV half of perl_run_program: perl_parse's and perl_run's own
 * shape, joined. ps is owned by the caller, so no C++ object lives in a
 * frame the jumps cross. */
void program_execute(pTHX_ const ProgramSpec &ps) {
    const I32 oldscope = PL_scopestack_ix;
    volatile bool compiled = false;
    dJMPENV;
    int ret;
    JMPENV_PUSH(ret);
    switch (ret) {
    case 0:
        program_compile(aTHX_ ps);
        if (PL_unitcheckav) call_list(oldscope, PL_unitcheckav);
        if (PL_checkav) {
            PERL_SET_PHASE(PERL_PHASE_CHECK);
            call_list(oldscope, PL_checkav);
        }
        compiled = true;
        /* S_run_body */
        if (PL_minus_c) {
            PerlIO_printf(Perl_error_log, "%s syntax OK\n", PL_origfilename);
            my_exit(0);
        }
        if (PERLDB_SINGLE && PL_DBsingle) PL_DBsingle_iv = 1;
        if (PL_initav) {
            PERL_SET_PHASE(PERL_PHASE_INIT);
            call_list(oldscope, PL_initav);
        }
        PERL_SET_PHASE(PERL_PHASE_RUN);
        if (PL_main_start) {
            CvDEPTH(PL_main_cv) = 1;
            PL_op = PL_main_start;
            CALLRUNOPS(aTHX);
        }
        my_exit(0);
        NOT_REACHED; /* NOTREACHED */
    case 1:
        STATUS_ALL_FAILURE;
        /* FALLTHROUGH */
    case 2: /* my_exit() */
        while (PL_scopestack_ix > oldscope) LEAVE;
        FREETMPS;
        program_set_curstash_main(aTHX);
        break;
    case 3: /* a die caught by an eval of the main program */
        if (compiled && PL_restartop) {
            POPSTACK_TO(PL_mainstack);
            PL_restartjmpenv = NULL;
            PL_op = PL_restartop;
            PL_restartop = 0;
            CALLRUNOPS(aTHX);
            my_exit(0);
            NOT_REACHED; /* NOTREACHED */
        }
        PerlIO_printf(Perl_error_log, "panic: restartop in perl_run_program\n");
        FREETMPS;
        STATUS_ALL_FAILURE;
        break;
    }
    JMPENV_POP;
}

