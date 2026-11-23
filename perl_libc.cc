/* perl_libc.cc — what of the C library is perl's own.
 *
 * The POSIX functions WASI lacks are wasmify's host shims
 * (wasmify_posix.h). Two things stay here, because they are about perl:
 * the locale the interpreter takes at start-up, and perl's syscall(),
 * whose interface (pp_sys.c, patched by scripts/wasi-configure.sh) says
 * which arguments are addresses of strings, which the host import needs
 * and syscall(2) does not say.
 */
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <locale.h>
#include <string>
#include <strings.h>

#include "wasmify_posix.h"

extern "C" {

char *__real_setlocale(int category, const char *name);

} /* extern "C" */

namespace {

int fail(int error) {
    errno = error;
    return -1;
}

} /* namespace */

extern "C" {

/* ---- locales ----------------------------------------------------------------
 * The C library has the data of two locales: "C" (also named "POSIX") and
 * "C.UTF-8", which is "C" with UTF-8 as the character encoding. Its
 * setlocale nevertheless accepts every name, keeps it, and behaves as
 * "C.UTF-8": a program that asked for "de_DE" was told it had it, and then
 * found the decimal point and the currency of "C". Here a locale that is
 * asked for by name is one of the two, or the call fails, as it does on a
 * system where the locale is not installed.
 *
 * The empty name asks for the locale of the environment (LC_ALL, the
 * category's own variable, LANG, in that order; an environment that names
 * no locale means "C"). A program that asks for it is answered like one
 * that asks by name: when the environment names a locale there is no data
 * for, the call fails and the locale stays as it was. (A program finds out
 * that way whether the user's locale is there: `setlocale(LC_ALL, "") or
 * skip`.)
 *
 * The interpreter asks the same question when it starts, and there a
 * failure would be a complaint on standard error at every start for a user
 * whose environment names, say, "ja_JP.UTF-8". What the user's environment
 * names is not a request the program made for the conventions of that
 * locale: at start-up the nearest locale there is data for is taken, which
 * is the one with the character encoding the environment names: "C.UTF-8"
 * for a name whose codeset is UTF-8, "C" for any other. */

/* Set while the interpreter sets its locale up when it is constructed. The
 * C library cannot tell that call of setlocale(LC_ALL, "") from a
 * program's. */
static bool locale_start_up;

void perl_wasm_locale_start_up(int on) { locale_start_up = on != 0; }

/* Whether the codeset of a locale name (language[_territory][.codeset]
 * [@modifier], the form of X/Open) is UTF-8, in either of its spellings. */
static bool locale_codeset_is_utf8(const char *name) {
    const char *dot = strchr(name, '.');
    if (!dot) return false;
    const char *codeset = dot + 1;
    const size_t len = strcspn(codeset, "@");
    return (len == 5 && strncasecmp(codeset, "UTF-8", 5) == 0) ||
           (len == 4 && strncasecmp(codeset, "UTF8", 4) == 0);
}

/* The name the C library knows a locale it has data for by, given one name
 * (of len characters) a program may use for it; NULL for every other
 * locale. */
static const char *locale_with_data(const char *name, size_t len) {
    static const struct {
        const char *name;
        const char *known_as;
    } locales[] = {
        {"C", "C"},           {"POSIX", "C"},       {"C.UTF-8", "C.UTF-8"},
        {"C.utf8", "C.UTF-8"}, {"C.UTF8", "C.UTF-8"}, {"C.utf-8", "C.UTF-8"},
    };
    for (const auto &l : locales)
        if (strlen(l.name) == len && memcmp(l.name, name, len) == 0) return l.known_as;
    return NULL;
}

/* The six categories setlocale(LC_ALL) stands for, with the environment
 * variable of each. */
static const struct {
    int category;
    const char *variable;
} locale_categories[] = {
    {LC_CTYPE, "LC_CTYPE"},       {LC_NUMERIC, "LC_NUMERIC"},   {LC_TIME, "LC_TIME"},
    {LC_COLLATE, "LC_COLLATE"},   {LC_MONETARY, "LC_MONETARY"}, {LC_MESSAGES, "LC_MESSAGES"},
};

/* The locale the environment names for one category, as the C library
 * knows it; NULL when there is no data for it. */
static const char *locale_of_environment(const char *variable) {
    const char *name = getenv("LC_ALL");
    if (!name || !*name) name = getenv(variable);
    if (!name || !*name) name = getenv("LANG");
    if (!name || !*name) name = "C";
    const char *known = locale_with_data(name, strlen(name));
    if (!known && locale_start_up) known = locale_codeset_is_utf8(name) ? "C.UTF-8" : "C";
    return known;
}

/* The name each of the six categories was set to, in the order of
 * locale_categories. The C library keeps what a category's data is, but
 * not the name it was given: its data of C.UTF-8 differs from C's in
 * LC_CTYPE alone, and it reports every other category set to C.UTF-8 as
 * "C" (glibc reports "C.UTF-8"; libintl-perl takes a category reported as
 * "C" for one without translations). The names are the ones
 * locale_with_data gives, which live as long as the program. */
static const char *locale_names[6] = {"C", "C", "C", "C", "C", "C"};

/* The name setlocale reports for a category: that of the category, or for
 * LC_ALL the one name of all six, or else theirs separated by ";" in the
 * order of the categories (the C library's form, which setlocale takes
 * back). */
static char *locale_name_of(int category) {
    for (size_t i = 0; i < 6; i++)
        if (locale_categories[i].category == category) return (char *)locale_names[i];
    if (category != LC_ALL) return __real_setlocale(category, NULL);
    static std::string all;
    all = locale_names[0];
    bool same = true;
    for (size_t i = 1; i < 6; i++) same = same && strcmp(locale_names[i], locale_names[0]) == 0;
    if (same) return (char *)all.c_str();
    for (size_t i = 1; i < 6; i++) {
        all += ';';
        all += locale_names[i];
    }
    return (char *)all.c_str();
}

/* Records the name a category (LC_ALL: every one) was set to. */
static void locale_name_set(int category, const char *known) {
    for (size_t i = 0; i < 6; i++)
        if (category == LC_ALL || locale_categories[i].category == category) locale_names[i] = known;
}

char *__wrap_setlocale(int category, const char *name) {
    if (!name) return locale_name_of(category);
    if (!*name) {
        /* every category asked for has to be there before one is set */
        for (const auto &c : locale_categories)
            if ((category == LC_ALL || c.category == category) && !locale_of_environment(c.variable)) {
                errno = ENOENT;
                return NULL;
            }
        bool one_of_the_six = false;
        for (const auto &c : locale_categories) {
            if (category != LC_ALL && c.category != category) continue;
            one_of_the_six = true;
            const char *known = locale_of_environment(c.variable);
            if (!__real_setlocale(c.category, known)) return NULL;
            locale_name_set(c.category, known);
        }
        if (!one_of_the_six) return __real_setlocale(category, name);
        return locale_name_of(category);
    }
    /* A name, or for LC_ALL also one name per category separated by ";"
     * (the form setlocale(LC_ALL, NULL) reports them in): every one has to
     * be a locale there is data for. */
    std::string known;
    const char *parts[6];
    size_t count = 0;
    for (const char *part = name;;) {
        const size_t len = category == LC_ALL ? strcspn(part, ";") : strlen(part);
        const char *locale = locale_with_data(part, len);
        if (!locale) {
            errno = ENOENT;
            return NULL;
        }
        if (!known.empty()) known += ';';
        known += locale;
        if (count < 6) parts[count] = locale;
        count++;
        if (!part[len]) break;
        part += len + 1;
    }
    if (!__real_setlocale(category, known.c_str())) return NULL;
    if (count == 1) {
        locale_name_set(category, parts[0]);
    } else if (count == 6) {
        for (size_t i = 0; i < 6; i++) locale_names[i] = parts[i];
    }
    return locale_name_of(category);
}

/* perl's syscall() (pp_sys.c, scripts/wasi-configure.sh): a[0] is the
 * number and a[1..items-1] its arguments, those whose bit (by their index
 * in a) is set in addresses the addresses of strings of this memory. More
 * than six arguments no system call takes. -1 with errno set on failure,
 * as syscall(2). */
extern "C" void perl_wasm_croak_syscall_forked(long long number) __attribute__((noreturn));
extern "C" long goperl_syscall(const unsigned long *a, int items, unsigned long addresses) {
    unsigned long long args[6] = {0, 0, 0, 0, 0, 0};
    const int nargs = items - 1;
    if (nargs < 0 || nargs > 6) {
        errno = EINVAL;
        return -1;
    }
    for (int i = 0; i < nargs; i++) args[i] = (unsigned long long)a[i + 1];
    long long result = 0;
    const int rc = __wasmify_host_sys_syscall((long long)a[0], args, nargs,
                                         (unsigned int)(addresses >> 1), &result);
    /* the host does not make the call for a forked process (1): the call
     * depends on the process of the operating system, which a forked
     * process of gperl shares with the process it was forked from */
    if (rc == 1) perl_wasm_croak_syscall_forked((long long)a[0]);
    if (rc != 0) return fail(-rc);
    return (long)result;
}

} /* extern "C" */
