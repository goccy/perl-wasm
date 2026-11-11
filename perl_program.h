/* perl_program.h — whole-program execution, shared between the bridge
 * (perl.cc decodes the host's description) and perl_program.cc (which
 * drives the interpreter).
 *
 * Include it AFTER <perl.h>, in a file that already included <string> and
 * <vector>: the declarations take perl's interpreter-context parameter
 * (pTHX), and no C++ standard header survives being first included after
 * perl.h. */
#ifndef PERL_PROGRAM_H
#define PERL_PROGRAM_H

enum : uint32_t {
    PROG_LOOP          = 1u << 0,
    PROG_LOOP_PRINT    = 1u << 1,
    PROG_AUTOSPLIT     = 1u << 2,
    PROG_SPLIT_PATTERN = 1u << 3,
    PROG_LINE_ENDINGS  = 1u << 4,
    PROG_RS_SET        = 1u << 5,
    PROG_RS_UNDEF      = 1u << 6,
    PROG_RS_UTF8       = 1u << 7,
    PROG_INPLACE       = 1u << 8,
    PROG_SYNTAX_ONLY   = 1u << 9,
    PROG_FEATURES      = 1u << 10,
    PROG_EXTRACT       = 1u << 11,
    PROG_UNSAFE        = 1u << 12,
    PROG_DEBUGGER      = 1u << 13,
    PROG_UNICODE       = 1u << 14,
};
enum : uint8_t { PROG_WARN_DEFAULT = 0, PROG_WARN_ON = 1, PROG_WARN_ALL = 2, PROG_WARN_NONE = 3 };
enum : uint8_t { PROG_TAINT_OFF = 0, PROG_TAINT_WARN = 1, PROG_TAINT_ON = 2 };
enum : uint8_t { PROG_SOURCE_FILE = 0, PROG_SOURCE_TEXT = 1, PROG_SOURCE_STDIN = 2 };

struct ProgramDefine {
    std::string name;
    bool has_value;
    std::string value;
};

struct ProgramSpec {
    uint32_t flags;
    uint8_t warnings;
    uint8_t taint;
    uint32_t unicode;
    uint8_t source;
    std::string path;
    std::string code;
    std::string extract_dir;
    std::string rs;
    std::string ors;
    std::string split;
    std::string inplace;
    std::vector<std::string> preamble;
    std::vector<std::string> inc;
    std::vector<ProgramDefine> defines;
    std::vector<std::string> args;
};

/* Compiles and runs the program to its end; an exit or an uncaught die
 * ends it like the perl command. The exit status is in the interpreter
 * (STATUS_EXIT) once program_run_end_blocks has run. */
void program_execute(pTHX_ const ProgramSpec &ps);
void program_run_end_blocks(pTHX);

/* Removes from @INC the directories the interpreter took from the
 * environment at start-up (PERL5LIB, "."): what perl -T leaves out.
 * Defined next to the code that builds @INC (perl.cc). */
void program_inc_drop_environment(pTHX);

#endif /* PERL_PROGRAM_H */
