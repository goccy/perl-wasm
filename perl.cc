/* perl.cc — Perl embedding bridge (implements pl.h) for wasm64-wasi + Go.
 *
 * This is the wasmify CustomBridgeSource: it wraps libperl.a (+ the statically
 * linked XS extensions) behind the tiny pl.h surface (perl_new / perl_eval /
 * perl_close / perl_interrupt_addr) that the generator exports to Go.
 *
 * Layering note on headers: our public API lives in "pl.h" (NOT perl.h) so it
 * does not collide with Perl's own <perl.h>, which we pull in below. The bridge
 * source itself is still perl.cc.
 *
 * Threading: one wasm instance == one PerlInterpreter. Perl is built
 * -Dusemultiplicity (implicit pTHX) but WITHOUT ithreads, so a single global
 * interpreter with PERL_SET_CONTEXT before each entry point is sufficient; a
 * second interpreter means a second wasm2go module instance with its own
 * linear memory and its own copy of these globals.
 *
 * Addresses: the interpreter is LP64 (wasm64, memory64): a pointer, a long
 * and a size_t are 8 bytes. Its linear memory has no maximum of its own:
 * the embedder sets how far it may grow, past 4 GiB as well. An address
 * crosses to the host whole, in 64 bits (goperl_gaddr_t), wherever it
 * crosses: an operand or a result of perl_xs_helper of its own, a 64-bit
 * word of a callback request or response, a 64-bit word of a record laid
 * out in this memory (goperl_gword_t). Nothing packs an address beside
 * another value. What the host reads in linear memory itself is laid out at
 * the interpreter's widths: a pointer stored there is 8 bytes.
 */
#include "pl.h"

/* The C++ standard headers MUST come before Perl's: perl.h defines
 * short-name macros (do_open, do_close, ...) that break any libc++ header
 * textually included after it. */
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" {
#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>
#include <perliol.h>
}

#include "perl_program.h"

/* ---- linked-in extensions -------------------------------------------------
 * The XS extensions of this build are part of perl.wasm: one boot function
 * per extension, each in its lib/auto/<Mod>/<Mod>.a archive and resolved at
 * the perl.wasm link. KEEP IN SYNC with STATIC_EXT in
 * scripts/wasi-configure.sh.
 *
 * A statically linked perl usually registers every one of them at start-up
 * as <Module>::bootstrap (the xs_init ExtUtils::Miniperl writes into
 * perlmain.c). A program can see that: the package of a module it never
 * loaded has a sub, so the module looks loaded (Class::Load's
 * is_class_loaded and similar probes). Here nothing is registered until the
 * module is loaded: XSLoader::load and DynaLoader::bootstrap ask
 * DynaLoader::dl_static_boot for the module's boot function, which installs
 * it as <Module>::bootstrap then -- the name a dynamically loading perl
 * installs a boot function under, at the same moment. (The two loaders are
 * taught to ask in scripts/wasi-configure.sh.) */
extern "C" {
void boot_DynaLoader(pTHX_ CV *cv);
void boot_attributes(pTHX_ CV *cv);
void boot_B(pTHX_ CV *cv);
void boot_Compress__Raw__Bzip2(pTHX_ CV *cv);
void boot_Compress__Raw__Zlib(pTHX_ CV *cv);
void boot_Cwd(pTHX_ CV *cv);
void boot_Data__Dumper(pTHX_ CV *cv);
void boot_Devel__Peek(pTHX_ CV *cv);
void boot_Digest__MD5(pTHX_ CV *cv);
void boot_Digest__SHA(pTHX_ CV *cv);
void boot_Encode(pTHX_ CV *cv);
void boot_Encode__Byte(pTHX_ CV *cv);
void boot_Encode__CN(pTHX_ CV *cv);
void boot_Encode__EBCDIC(pTHX_ CV *cv);
void boot_Encode__JP(pTHX_ CV *cv);
void boot_Encode__KR(pTHX_ CV *cv);
void boot_Encode__Symbol(pTHX_ CV *cv);
void boot_Encode__TW(pTHX_ CV *cv);
void boot_Encode__Unicode(pTHX_ CV *cv);
void boot_Fcntl(pTHX_ CV *cv);
void boot_File__DosGlob(pTHX_ CV *cv);
void boot_File__Glob(pTHX_ CV *cv);
void boot_Filter__Util__Call(pTHX_ CV *cv);
void boot_Hash__Util(pTHX_ CV *cv);
void boot_Hash__Util__FieldHash(pTHX_ CV *cv);
void boot_I18N__Langinfo(pTHX_ CV *cv);
void boot_IO(pTHX_ CV *cv);
void boot_List__Util(pTHX_ CV *cv);
void boot_Math__BigInt__FastCalc(pTHX_ CV *cv);
void boot_MIME__Base64(pTHX_ CV *cv);
void boot_Opcode(pTHX_ CV *cv);
void boot_POSIX(pTHX_ CV *cv);
void boot_PerlIO__encoding(pTHX_ CV *cv);
void boot_PerlIO__via(pTHX_ CV *cv);
void boot_SDBM_File(pTHX_ CV *cv);
void boot_Socket(pTHX_ CV *cv);
void boot_Storable(pTHX_ CV *cv);
void boot_Sys__Hostname(pTHX_ CV *cv);
void boot_Sys__Syslog(pTHX_ CV *cv);
void boot_Time__HiRes(pTHX_ CV *cv);
void boot_Time__Piece(pTHX_ CV *cv);
void boot_Unicode__Collate(pTHX_ CV *cv);
void boot_Unicode__Normalize(pTHX_ CV *cv);
void boot_mro(pTHX_ CV *cv);
void boot_re(pTHX_ CV *cv);
}

/* The numbers of the operating system as the program sees them
 * (perl_numbers.cc). */
extern "C" {
void numbers_wrap_builtins(void);
void numbers_extension_booted(pTHX_ const char *module);
void XS_Internals_host_numbers(pTHX_ CV *cv);
}

namespace {

struct StaticExtension {
    const char *module;
    XSUBADDR_t boot;
};

const StaticExtension static_extensions[] = {
    {"attributes", boot_attributes},
    {"B", boot_B},
    {"Compress::Raw::Bzip2", boot_Compress__Raw__Bzip2},
    {"Compress::Raw::Zlib", boot_Compress__Raw__Zlib},
    {"Cwd", boot_Cwd},
    {"Data::Dumper", boot_Data__Dumper},
    {"Devel::Peek", boot_Devel__Peek},
    {"Digest::MD5", boot_Digest__MD5},
    {"Digest::SHA", boot_Digest__SHA},
    {"Encode", boot_Encode},
    {"Encode::Byte", boot_Encode__Byte},
    {"Encode::CN", boot_Encode__CN},
    {"Encode::EBCDIC", boot_Encode__EBCDIC},
    {"Encode::JP", boot_Encode__JP},
    {"Encode::KR", boot_Encode__KR},
    {"Encode::Symbol", boot_Encode__Symbol},
    {"Encode::TW", boot_Encode__TW},
    {"Encode::Unicode", boot_Encode__Unicode},
    {"Fcntl", boot_Fcntl},
    {"File::DosGlob", boot_File__DosGlob},
    {"File::Glob", boot_File__Glob},
    {"Filter::Util::Call", boot_Filter__Util__Call},
    {"Hash::Util", boot_Hash__Util},
    {"Hash::Util::FieldHash", boot_Hash__Util__FieldHash},
    {"I18N::Langinfo", boot_I18N__Langinfo},
    {"IO", boot_IO},
    {"List::Util", boot_List__Util},
    {"Math::BigInt::FastCalc", boot_Math__BigInt__FastCalc},
    {"MIME::Base64", boot_MIME__Base64},
    {"Opcode", boot_Opcode},
    {"POSIX", boot_POSIX},
    {"PerlIO::encoding", boot_PerlIO__encoding},
    {"PerlIO::via", boot_PerlIO__via},
    {"SDBM_File", boot_SDBM_File},
    {"Socket", boot_Socket},
    {"Storable", boot_Storable},
    {"Sys::Hostname", boot_Sys__Hostname},
    {"Sys::Syslog", boot_Sys__Syslog},
    {"Time::HiRes", boot_Time__HiRes},
    {"Time::Piece", boot_Time__Piece},
    {"Unicode::Collate", boot_Unicode__Collate},
    {"Unicode::Normalize", boot_Unicode__Normalize},
    {"mro", boot_mro},
    {"re", boot_re},
};

} /* namespace */

/* <module>::bootstrap of a linked-in extension: its boot function, and
 * then what follows a boot here (the numbers its constants show, see
 * perl_numbers.cc). The sub holds the extension it boots. */
XS(XS_static_extension_bootstrap) {
    const StaticExtension *const ext =
        static_cast<const StaticExtension *>(CvXSUBANY(cv).any_ptr);
    ext->boot(aTHX_ cv);
    numbers_extension_booted(aTHX_ ext->module);
}

/* The directory the interpreter's own library is in (perl_new's lib_dir),
 * empty when it was started without one. It is kept because @INC, where it
 * was put, is the program's to change. */
static std::string g_lib_dir;

/* Whether a shared object for the module takes the place of the linked-in
 * extension. A linked-in extension belongs to the interpreter's own
 * library: it is found where a shared object in that directory would be.
 * On a perl that loads dynamically, a module that was upgraded from CPAN
 * has its files in a directory ahead of the library in @INC, and both the
 * module file and auto/<Module>/<Name>.<dlext> are taken from there. So
 * when the interpreter can load shared objects (the host defined
 * DynaLoader::dl_load_file), an @INC directory ahead of the library that
 * holds the module's shared object wins: the loaders are told that the
 * module is not linked in, and find that file the way they find any. The
 * path is the one XSLoader and DynaLoader build (dlext is "so" in this
 * configuration, and there is no mod2fname). */
static bool static_extension_shadowed(pTHX_ const char *module) {
    if (!get_cv("DynaLoader::dl_load_file", 0)) return false;
    std::string rel = "/auto/";
    const char *last = module;
    for (const char *p = module; *p;) {
        if (p[0] == ':' && p[1] == ':') {
            rel.push_back('/');
            p += 2;
            last = p;
        } else {
            rel.push_back(*p++);
        }
    }
    rel.push_back('/');
    rel += last;
    rel += ".so";
    AV *const inc = GvAVn(PL_incgv);
    const SSize_t n = static_cast<SSize_t>(av_count(inc));
    for (SSize_t i = 0; i < n; i++) {
        SV **const svp = av_fetch(inc, i, FALSE);
        /* a hook in @INC holds no files */
        if (!svp || !SvOK(*svp) || SvROK(*svp)) continue;
        STRLEN len = 0;
        const char *const dir = SvPV(*svp, len);
        if (!g_lib_dir.empty() && len == g_lib_dir.size() && memcmp(dir, g_lib_dir.data(), len) == 0)
            return false;
        const std::string path = std::string(dir, len) + rel;
        Stat_t st;
        if (PerlLIO_stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return true;
    }
    return false;
}

/* DynaLoader::dl_static_boot(module): the boot function of a linked-in
 * extension, installed as <module>::bootstrap and returned as a code
 * reference; undef for a module that is not linked in, or whose place a
 * shared object has taken (static_extension_shadowed).
 *
 * A module the host has registered natively (go-perl's XS loader) is found
 * the same way and at the same moment: the host keeps its boot function as
 * DynaLoader::native::<module>::bootstrap, outside the module's package,
 * and it becomes <module>::bootstrap here. */
XS(XS_DynaLoader_dl_static_boot) {
    dXSARGS;
    if (items != 1) croak_xs_usage(cv, "module");
    STRLEN len;
    const char *const module = SvPV(ST(0), len);
    for (const StaticExtension &ext : static_extensions) {
        if (strlen(ext.module) != len || memcmp(ext.module, module, len) != 0) continue;
        if (static_extension_shadowed(aTHX_ ext.module)) break;
        SV *const name = sv_2mortal(newSVpvf("%s::bootstrap", ext.module));
        CV *boot = get_cv(SvPV_nolen(name), 0);
        if (!boot) {
            boot = newXS(SvPV_nolen(name), XS_static_extension_bootstrap, __FILE__);
            CvXSUBANY(boot).any_ptr = const_cast<StaticExtension *>(&ext);
        }
        ST(0) = sv_2mortal(newRV_inc((SV *)boot));
        XSRETURN(1);
    }
    SV *const held = sv_2mortal(newSVpvf("DynaLoader::native::%" SVf "::bootstrap", SVfARG(ST(0))));
    if (CV *const native = get_cv(SvPV_nolen(held), SvUTF8(ST(0)) ? SVf_UTF8 : 0)) {
        SV *const name = sv_2mortal(newSVpvf("%" SVf "::bootstrap", SVfARG(ST(0))));
        GV *const gv = gv_fetchsv(name, GV_ADD, SVt_PVCV);
        SV *const ref = sv_2mortal(newRV_inc((SV *)native));
        /* assigning a code reference to the glob installs the sub */
        if (!GvCV(gv)) sv_setsv((SV *)gv, ref);
        ST(0) = ref;
        XSRETURN(1);
    }
    XSRETURN_UNDEF;
}

/* ---- site directories ------------------------------------------------------
 * The build configures the site directories under /usr/lib/perl5, where
 * nothing is. The embedder names the directory they are under at run time
 * (perl_new's site_dir); they are laid out below it here, by the one who
 * knows the interpreter's version. g_site_config holds every %Config key
 * that names one of them with its value — empty when the interpreter was
 * started without a site_dir — and is what the shipped Config.pm and
 * Config_heavy.pl ask for (Internals::site_directories, next to the
 * Internals::V they ask for the build options). It has to be kept: nothing
 * else in the interpreter records what it was started with. */
static std::vector<std::pair<const char *, std::string>> g_site_config;
/* sitearch and sitelib, the two that are in @INC; empty without a site_dir. */
static std::string g_site_arch_dir, g_site_lib_dir;

/* The name of the architecture (perl_new's archname): the host's, or the
 * build's ARCHNAME when the embedder names none. perl.c asks for it where
 * it would use ARCHNAME (the patch in scripts/wasi-configure.sh), Config.pm
 * and Config_heavy.pl through Internals::archname. */
static std::string g_archname = ARCHNAME;

extern "C" const char *perl_wasi_archname(void) { return g_archname.c_str(); }

/* Internals::archname(): the name of the architecture. */
XS(XS_Internals_archname) {
    dXSARGS;
    if (items != 0) croak_xs_usage(cv, "");
    XSRETURN_PV(g_archname.c_str());
}

static void site_configure(const char *site_dir) {
    g_site_config.clear();
    g_site_arch_dir.clear();
    g_site_lib_dir.clear();
    if (!site_dir || !*site_dir) return;
    std::string root(site_dir);
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    const std::string under = root == "/" ? "" : root;
    const std::string stem = under + "/site_perl";
    g_site_lib_dir = stem + "/" PERL_VERSION_STRING;
    g_site_arch_dir = g_site_lib_dir + "/" + g_archname;
    const std::string bin = under + "/bin";
    const std::string man1 = under + "/man/man1";
    const std::string man3 = under + "/man/man3";
    static const struct {
        const char *key;
        int dir;
    } keys[] = {
        {"siteprefix", 0},     {"siteprefixexp", 0},
        {"sitelib_stem", 1},
        {"sitelib", 2},        {"sitelibexp", 2},        {"installsitelib", 2},
        {"sitearch", 3},       {"sitearchexp", 3},       {"installsitearch", 3},
        {"sitebin", 4},        {"sitebinexp", 4},        {"installsitebin", 4},
        {"sitescript", 4},     {"sitescriptexp", 4},     {"installsitescript", 4},
        {"siteman1dir", 5},    {"siteman1direxp", 5},    {"installsiteman1dir", 5},
        {"siteman3dir", 6},    {"siteman3direxp", 6},    {"installsiteman3dir", 6},
    };
    const std::string *const dirs[] = {&root, &stem, &g_site_lib_dir, &g_site_arch_dir, &bin, &man1, &man3};
    for (const auto &k : keys) g_site_config.emplace_back(k.key, *dirs[k.dir]);
}

/* Internals::site_directories(): the %Config keys of the site directories
 * this interpreter was started with, each followed by its value; an empty
 * list when it was started with none. */
XS(XS_Internals_site_directories) {
    dXSARGS;
    if (items != 0) croak_xs_usage(cv, "");
    SP -= items;
    EXTEND(SP, static_cast<SSize_t>(2 * g_site_config.size()));
    for (const auto &entry : g_site_config) {
        mPUSHs(newSVpv(entry.first, 0));
        mPUSHs(newSVpvn(entry.second.data(), entry.second.size()));
    }
    PUTBACK;
}

/* ---- the directory the program starts in ---------------------------------
 * Internals::start_directory(DIR) makes DIR the working directory, as the
 * directory the process was started in: the C library keeps it as chdir
 * does, but without the search permission chdir(2) needs, which a process
 * started in a directory it may not search does not have (perl runs there,
 * and its relative paths fail as they do there). The host is told, as
 * after a chdir; the host process is there already. True, or false with $!
 * set (DIR is no directory); DIR is an absolute path. The embedder sets the
 * program up with it. */
extern "C" int __real_chdir(const char *path);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_chdir")))
extern "C" int wasi_host_proc_chdir(const char *path, int len);

XS(XS_Internals_start_directory) {
    dXSARGS;
    if (items != 1) croak_xs_usage(cv, "dir");
    STRLEN len;
    const char *dir = SvPV_const(ST(0), len);
    if (len == 0 || dir[0] != '/' || strlen(dir) != len)
        croak("Internals::start_directory: %s is not an absolute path", dir);
    if (__real_chdir(dir) != 0) XSRETURN_NO;
    (void)wasi_host_proc_chdir(dir, (int)len);
    XSRETURN_YES;
}

static void xs_init(pTHX) {
    static const char file[] = __FILE__;
    dXSUB_SYS;
    PERL_UNUSED_CONTEXT;

    /* The loader itself, and its way to the table above. */
    newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, file);
    newXS("DynaLoader::dl_static_boot", XS_DynaLoader_dl_static_boot, file);
    newXS("Internals::site_directories", XS_Internals_site_directories, file);
    newXS("Internals::archname", XS_Internals_archname, file);
    newXS("Internals::host_numbers", XS_Internals_host_numbers, file);
    newXS("Internals::start_directory", XS_Internals_start_directory, file);
}

/* ---- interpreter state --------------------------------------------------- */
static PerlInterpreter *g_my_perl = nullptr;

/* A guest address as it crosses to the host: whole, in 64 bits. */
typedef uint64_t goperl_gaddr_t;
#define GOPERL_ADDR(p) ((goperl_gaddr_t)(uintptr_t)(p))

/* A word of the records that cross as words: the requests of the callbacks
 * below and the responses to them, the strings of words an operation of
 * perl_xs_helper takes, and the answers it lays out as words in this
 * memory. A word is as wide as an address, which it may hold. */
typedef uint64_t goperl_gword_t;
static_assert(sizeof(goperl_gword_t) == sizeof(goperl_gaddr_t) &&
                  sizeof(goperl_gaddr_t) >= sizeof(uintptr_t),
              "a word holds an address of this memory");

/* The words of a response that follow its status byte: the i-th of them,
 * or 0 when the response is shorter. */
static inline goperl_gword_t goperl_resp_word(const unsigned char *resp, uint32_t len,
                                              uint32_t i) {
    goperl_gword_t w = 0;
    if ((size_t)len >= 1 + 8 * ((size_t)i + 1)) memcpy(&w, resp + 1 + 8 * (size_t)i, 8);
    return w;
}

/* ---- Perl -> Go dispatch -------------------------------------------------
 * The wasmify callback import: the ONE host entry point every C++->Go (here:
 * Perl->Go) call goes through. Declared here rather than via the generated
 * bridge header so perl.cc stays self-contained; the signature must match
 * bridge/api_bridge.h's WASM_IMPORT(wasmify, callback_invoke). The i64 result
 * names the response (goperl_callback_response); the response buffer
 * transfers to us (we free it), while the request buffer only lends: the host
 * reads it during the call and we keep ownership (it lives in a Perl SV). */
__attribute__((import_module("wasmify"), import_name("callback_invoke")))
extern "C" int64_t wasmify_callback_invoke(int32_t callback_id, int32_t method_id,
                                           void *req, size_t req_len);

/* What the import returns when the host ended the callback by a non-local
 * exit of this interpreter: native code asked for an operation, the
 * interpreter died (or exited) inside it, and the jump to the place that
 * catches it was held on the host while the native frames were left (see
 * go-perl's guestJump). The host has put the exception back when the import
 * returns, and the transpiled code sees it only where a function that may
 * raise one returns to its caller: everything up to there runs as if
 * nothing had happened. So the code after the import does nothing but
 * return, at once — no response to read (there is none), nothing that
 * dies (a die would run halfway and leave what it had made by then). Every
 * function that makes the call is one that may raise (it croaks on a bad
 * response, or is called through a pointer of a type that croaking
 * functions have), so its caller goes on unwinding right after it returns.
 * The host answers this value for nothing else. */
#define GOPERL_CALLBACK_JUMPED ((int64_t)-1)

/* The response of a call of the import: its buffer (which transfers to the
 * caller, who frees it) and its length, from the i64 the import returns. On
 * wasm32 the i64 packs (buffer << 32) | length. On wasm64 an address does
 * not fit beside a length, and the i64 is the address of a {buffer, length}
 * pair of u64 the host keeps for the next answer (the convention of the
 * wasmify bridge's decode_result): it is read here, before anything else
 * can call the host. 0 is no response on both. Never called with
 * GOPERL_CALLBACK_JUMPED, which is no address. */
static inline unsigned char *goperl_callback_response(int64_t rc, uint32_t *len) {
    if (rc == 0) {
        *len = 0;
        return nullptr;
    }
#ifdef __wasm64__
    const uint64_t *d = (const uint64_t *)(uintptr_t)rc;
    *len = (uint32_t)d[1];
    return (unsigned char *)(uintptr_t)d[0];
#else
    *len = (uint32_t)((uint64_t)rc & 0xFFFFFFFFu);
    return (unsigned char *)(uintptr_t)((uint64_t)rc >> 32);
#endif
}

/* The Go-side callback id every Perl->Go call from this instance dispatches
 * to. 0 = no dispatcher registered (perl_set_go_dispatcher not called). */
static int32_t g_go_cb = 0;

/* The requests and responses of the reserved callback method ids below are
 * made of 64-bit words (goperl_gword_t, little-endian): a request is a
 * string of words (a few end in bytes of text, as said for each), a
 * response is a status byte (GOPERL_RESP_*) and then words, or the message
 * of a failure. Every number is a word of its own, an address as well as a
 * count or a host id. */

/* Reserved callback method id for native-XS dispatch (see pl.h). Ordinary
 * bound Go functions use positive ids. */
#define GOPERL_NATIVE_METHOD_ID (-1)

/* Reserved callback method id for host-side magic teardown: when an SV
 * carrying goperl anchor magic (perl_xs_helper op SV_MAGIC_ATTACH) is freed,
 * the guest notifies the host with the host id ([id]) so it can run the
 * native module's svt_free and drop its mirror MAGIC chain. */
#define GOPERL_MG_FREE_METHOD_ID (-2)

/* Reserved callback method id for pp hooks: a native module that patched a
 * PL_ppaddr slot (host-side proxy table) gets called for every execution of
 * that op type. Request [op][op_type] and the perl stack (see
 * goperl_pp_stack_words); response [1][next_op] and what the hook made of
 * the stack on success, [0]+croak-message on failure. The hook runs the
 * ORIGINAL pp itself (perl_xs_helper op RUN_ORIGINAL) and decides the next
 * op, exactly like a real PL_ppaddr replacement. */
#define GOPERL_PP_HOOK_METHOD_ID (-3)

/* Reserved callback method id for keyword and infix plugins: request
 * [kind] and the bytes of the word (kind 0 a keyword, 1 an infix
 * operator). A keyword's response is [1][handled][the plugin's result][the
 * op]; an infix operator's [1][claimed][its length][the module's
 * struct][its precedence][has parse]. */
#define GOPERL_KEYWORD_METHOD_ID (-7)

/* Reserved callback method id for save-stack destructors registered via
 * perl_xs_helper op SAVE_DESTRUCTOR: fires with the host id ([id]) when the
 * enclosing guest scope pops (normally or during die unwinding). */
#define GOPERL_DTOR_METHOD_ID (-4)

/* `local` on an SV that carries the anchor: the request is [the anchor's
 * host id][the SV that takes the variable's place]. */
#define GOPERL_MG_LOCAL_METHOD_ID (-8)

/* Reserved callback method id for PerlIO layer hooks: a host-registered
 * layer (op PERLIO_DEF_LAYER) gets its CUSTOM slots (Pushed/Fill/Popped)
 * run on the host; the stock buffered behavior stays guest-native. The
 * request starts [hook][funcs_id][f][layer]; responses are hook-specific,
 * [0]+message croaks in the guest. */
#define GOPERL_PERLIO_METHOD_ID (-6)

/* Reserved callback method id for calls of native code that go through one
 * channel: the request starts with a GOPERL_HOST_ kind, the words that
 * follow are the kind's own. The response is [1] and the kind's result, or
 * [0] and the message to croak with. */
#define GOPERL_HOST_METHOD_ID (-9)

/* Reserved callback method id for reading memory of the process that is not
 * this memory (unpack 'p' and 'P' of such an address, goperl_unpack_address). */
#define GOPERL_PROCMEM_METHOD_ID (-10)
enum {
    GOPERL_HOST_NEWOP = 0, /* an op checker: [type][op] -> [op] */
    GOPERL_HOST_PEEP = 1,  /* a peephole hook: [which][op] */
    GOPERL_HOST_ATEXIT = 2, /* a function given to call_atexit: [id] */
    GOPERL_HOST_CALLCK = 3, /* a call checker: [id][entersubop][namegv][ckobj] -> [op] */
    GOPERL_HOST_MAGIC = 4,  /* a hook of the magic native code attached:
                             * [hook][sv][nsv][name][namlen] -> [result] */
    GOPERL_HOST_UVAR = 5,   /* a function of uvar magic: [is_set][arg][sv][key] -> [key][result] */
    GOPERL_HOST_FILTER = 6, /* a source filter: [id][idx][buf][maxlen] -> [result] */
    GOPERL_HOST_RUNOPS = 7, /* a run loop: [id] */
    GOPERL_HOST_BHK = 8,    /* a block hook: [id][which][arg] -> [op] */
    GOPERL_HOST_INFIX = 9,  /* an infix operator of native code: [0][def][opdata]
                             * -> [opdata] (its parse), or [1][def][opdata][lhs]
                             * [rhs] -> [opdata][op] (its build_op) */
    GOPERL_HOST_SIGHANDLER = 10 /* the signal handler of native code that is
                                 * PL_sighandlerp: [id][signal] */
};
/* The hooks of GOPERL_HOST_MAGIC, and the bits of an anchor's mask. */
enum {
    GOPERL_MGHOOK_GET = 0,
    GOPERL_MGHOOK_SET = 1,
    GOPERL_MGHOOK_LEN = 2,
    GOPERL_MGHOOK_CLEAR = 3,
    GOPERL_MGHOOK_COPY = 4
};

/* Host->guest response protocol (native XS / pp hook): byte 0 is the status,
 * GOPERL_RESP_OK followed by the result payload, GOPERL_RESP_FAIL followed by
 * the croak message bytes. */
#define GOPERL_RESP_FAIL 0
#define GOPERL_RESP_OK 1
#define GOPERL_RESP_THROW 2 /* die with the SV the native code threw */

/* The exception value a native XSUB ended with (croak_sv, or croak(NULL)
 * on $@): the host hands it over with the SV_CALL THROW operation while the
 * native code unwinds, and the thunk dies with it once the host call has
 * returned. NULL whenever no native XSUB is ending that way. */
static SV *goperl_thrown_sv = NULL;

/* The host answered that the native code it ran ended by throwing a value
 * (GOPERL_RESP_THROW): the interpreter dies with that value — the object
 * itself, or the whole text with its encoding. Whatever ran the native
 * code calls this on the response first: an XSUB, a keyword plugin, a pp
 * function, a hook. The response is freed when it does not return. */
static void goperl_die_if_thrown(pTHX_ unsigned char *resp) {
    if (resp[0] != GOPERL_RESP_THROW) return;
    SV *thrown = goperl_thrown_sv;
    goperl_thrown_sv = NULL;
    free(resp);
    if (!thrown) Perl_croak(aTHX_ "native code: the thrown value is missing");
    /* $SIG{__DIE__} has seen it where the native code croaked (see
     * goperl_die_hook): it is not called again on the way out */
    ENTER;
    SAVESPTR(PL_diehook);
    PL_diehook = NULL;
    croak_sv(sv_2mortal(thrown));
}

/* The count results of a call on top of the stack, taken off it, as words
 * native code reads (CALL_SV, CALL_METHOD, EVAL_SV): -> their address, 0
 * for none. One buffer serves every call: native code reads the words as
 * soon as the operation returns, and a call nested in the callee is done
 * with its own by then. No reference is taken on the results, as perl's
 * call_sv takes none (see CALL_SV). */
static uint64_t goperl_call_results(pTHX_ I32 count) {
    static std::vector<goperl_gword_t> results;
    dSP;
    if (count <= 0) return 0;
    results.resize((size_t)count);
    for (I32 i = count - 1; i >= 0; i--) results[(size_t)i] = GOPERL_ADDR(POPs);
    PUTBACK;
    return GOPERL_ADDR(results.data());
}

/* The op native code has as PL_op (the SDK's goperl_plop_want: 4 | its
 * want bits, or 0 while native code has not looked at PL_op). What the
 * interpreter does that reads the context of PL_op reads that op's want
 * bits: this is PL_op meanwhile with them, as native code has it
 * (Algorithm::Heapify::XS makes a copy of PL_op with OPf_WANT_SCALAR PL_op
 * around the overloading it calls). */
struct goperl_native_plop {
    OP op;
    bool set;
};
static void goperl_native_plop_enter(pTHX_ goperl_native_plop *p, uint64_t want) {
    p->set = want != 0 && PL_op;
    if (!p->set) return;
    Copy(PL_op, &p->op, 1, OP);
    p->op.op_flags = (U8)((p->op.op_flags & ~OPf_WANT) | (want & OPf_WANT));
    ENTER;
    SAVEOP();
    PL_op = &p->op;
}
static void goperl_native_plop_leave(pTHX_ goperl_native_plop *p) {
    if (p->set) LEAVE;
}

/* What perl's croak does before it unwinds: the exception, with the place
 * of the statement added to a message, goes to $SIG{__DIE__} (PL_diehook)
 * while the native code that croaks is still running, inside the scopes it
 * opened (Text::Xslate sets PL_diehook for a render, and its hook names
 * Text::Xslate in what dies there). It is perl's invoke_exception_hook that
 * runs it; a hook that dies makes its exception the one thrown, leaving this
 * operation by the jump a die inside any operation makes (the SDK's
 * GOPERL_FAILED_JUMP). Returns the exception to throw, which the caller
 * takes a reference on. */
/* perl's own, which its headers declare for pp_ctl.c and util.c only */
extern "C" bool Perl_invoke_exception_hook(pTHX_ SV *ex, bool warn);
static SV *goperl_die_hook(pTHX_ SV *msg) {
    SV *const ex = mess_sv(msg, 0);
    Perl_invoke_exception_hook(aTHX_ ex, FALSE);
    return ex;
}

/* Native-XSUB request: GOPERL_NATIVE_HDR_WORDS header words (fn_id, the
 * CV, the item count, the stack index of the first item (ax) and the
 * address of the stack) followed by the frame's SVs, a word each. The
 * native function's stack is this one by index: it reads what lies below
 * its items here (an index it has from PL_markstack or a context). */
#define GOPERL_NATIVE_HDR_WORDS 5

/* Connects this module instance to the host's signal delivery
 * (perl_signal.cc). Every entry point that runs Perl code calls it: the
 * instance may be a snapshot copy the host has not seen yet. */
extern "C" void wasi_signal_attach(pTHX);
extern "C" void perl_wasm_locale_start_up(int on);
/* Ends the process by a signal, as the signal's default action does
 * (perl_signal.cc). Does not return. */
extern "C" __attribute__((noreturn)) void wasi_signal_die(int sig);

/* Words the host writes with plain linear-memory writes, no wasm code
 * running on the instance (perl_interrupt_addr returns their address):
 *   +0  the interrupt flag. The host trips a runaway eval by storing 1 here;
 *       the interruptible run loop clears it and croaks. `volatile` so the
 *       compiler always reloads it in the op loop.
 *   +8  where the host process has this memory: an address of this memory
 *       is that much higher in the process (see goperl_pack_address). The
 *       host writes it before any code runs in an instance that runs native
 *       code, and never changes it; 0 (no base) otherwise. */
static volatile struct {
    uint32_t interrupt;
    uint32_t unused;
    uint64_t membase;
} g_hostwords = {0, 0, 0};
#define g_interrupt (g_hostwords.interrupt)

/* pp hooks: a host-native module (via the go-perl XS SDK) can claim op
 * types, as a module replaces PL_ppaddr[type] on perl: the interpreter's
 * PL_ppaddr slot of the type becomes goperl_pp_type_hook, so the ops made
 * from then on hand their execution to the host
 * (GOPERL_PP_HOOK_METHOD_ID), and the ops made before keep the pp function
 * they were made with. The interpreter's own functions are kept in
 * g_pp_orig (saved before the first claim), which RUN_ORIGINAL runs. */
static uint8_t g_pp_hooked[MAXO];
static Perl_ppaddr_t g_pp_orig[MAXO];
static bool g_pp_orig_saved = false;
static OP *goperl_pp_type_hook(pTHX);
/* The interpreter's own pp function of a type. */
static inline Perl_ppaddr_t goperl_pp_original(unsigned t) {
    return g_pp_orig_saved ? g_pp_orig[t] : PL_ppaddr[t];
}

/* Per-op host dispatch: a native module wrote one op's op_ppaddr; that op
 * (and only that op) executes through the pp-hook callback. */
static OP *goperl_pp_hooked_op(pTHX);

static size_t goperl_native_enter(pTHX);
static void goperl_native_leave(size_t below);

/* Host keyword/infix plugin forwarding (go-perl XS SDK, method -7). */
static Perl_keyword_plugin_t g_next_keyword_plugin = NULL;
static Perl_infix_plugin_t g_next_infix_plugin = NULL;

static int goperl_keyword_plugin(pTHX_ char *keyword_ptr, STRLEN keyword_len,
                                 OP **op_ptr) {
    if (g_go_cb && keyword_len > 0 && keyword_len < 256) {
        unsigned char buf[8 + 256];
        const goperl_gword_t kind = 0;
        memcpy(buf, &kind, 8);
        memcpy(buf + 8, keyword_ptr, keyword_len);
        const size_t native_below = goperl_native_enter(aTHX);
        int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_KEYWORD_METHOD_ID,
                                             buf, 8 + (size_t)keyword_len);
        goperl_native_leave(native_below);
        if (rc == GOPERL_CALLBACK_JUMPED) return KEYWORD_PLUGIN_DECLINE;
        uint32_t resp_len;
        unsigned char *resp = goperl_callback_response(rc, &resp_len);
        if (resp != nullptr && resp_len >= 1) {
            goperl_die_if_thrown(aTHX_ resp);
            if (resp[0] == GOPERL_RESP_FAIL) {
                char msg[512];
                size_t ml = resp_len - 1;
                if (ml > sizeof(msg) - 1) ml = sizeof(msg) - 1;
                memcpy(msg, resp + 1, ml);
                msg[ml] = '\0';
                free(resp);
                Perl_croak(aTHX_ "%s", msg);
            }
            /* [1][handled][the plugin's result][the op] */
            if (goperl_resp_word(resp, resp_len, 0)) {
                const int ret = (int)(int64_t)goperl_resp_word(resp, resp_len, 1);
                OP *const o = (OP *)(uintptr_t)goperl_resp_word(resp, resp_len, 2);
                free(resp);
                *op_ptr = o;
                return ret;
            }
            free(resp);
        }
    }
    return (*g_next_keyword_plugin)(aTHX_ keyword_ptr, keyword_len, op_ptr);
}

/* An infix operator of native code: what this interpreter is given is a
 * struct of its own for each of the module's, whose functions ask the host
 * to run the module's (GOPERL_HOST_INFIX). The structs last as long as the
 * interpreter, as the modules' static ones do. */
struct goperl_infix_def {
    struct Perl_custom_infix def; /* first: perl hands this pointer back */
    uint64_t host;                /* the module's struct */
};
static std::unordered_map<uint64_t, goperl_infix_def *> g_infix_defs;
static unsigned char *goperl_host_call(pTHX_ goperl_gword_t kind, const goperl_gword_t *words,
                                       uint32_t nwords, uint32_t *rlen);
static void goperl_infix_parse(pTHX_ SV **opdata, struct Perl_custom_infix *def) {
    const goperl_gword_t words[3] = {0, ((goperl_infix_def *)def)->host, GOPERL_ADDR(*opdata)};
    uint32_t rlen = 0;
    unsigned char *resp = goperl_host_call(aTHX_ GOPERL_HOST_INFIX, words, 3, &rlen);
    if (rlen >= 1 + 8) *opdata = (SV *)(uintptr_t)goperl_resp_word(resp, rlen, 0);
    free(resp);
}
static OP *goperl_infix_build(pTHX_ SV **opdata, OP *lhs, OP *rhs,
                              struct Perl_custom_infix *def) {
    const goperl_gword_t words[5] = {1, ((goperl_infix_def *)def)->host, GOPERL_ADDR(*opdata),
                                     GOPERL_ADDR(lhs), GOPERL_ADDR(rhs)};
    uint32_t rlen = 0;
    unsigned char *resp = goperl_host_call(aTHX_ GOPERL_HOST_INFIX, words, 5, &rlen);
    OP *o = NULL;
    if (rlen >= 1 + 16) {
        *opdata = (SV *)(uintptr_t)goperl_resp_word(resp, rlen, 0);
        o = (OP *)(uintptr_t)goperl_resp_word(resp, rlen, 1);
    }
    free(resp);
    return o;
}

static STRLEN goperl_infix_plugin(pTHX_ char *opname, STRLEN oplen,
                                  struct Perl_custom_infix **def) {
    if (g_go_cb && oplen > 0 && oplen < 256) {
        unsigned char buf[8 + 256];
        const goperl_gword_t kind = 1;
        memcpy(buf, &kind, 8);
        memcpy(buf + 8, opname, oplen);
        const size_t native_below = goperl_native_enter(aTHX);
        int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_KEYWORD_METHOD_ID,
                                             buf, 8 + (size_t)oplen);
        goperl_native_leave(native_below);
        if (rc == GOPERL_CALLBACK_JUMPED) return 0;
        uint32_t resp_len;
        unsigned char *resp = goperl_callback_response(rc, &resp_len);
        if (resp != nullptr && resp_len >= 1) {
            goperl_die_if_thrown(aTHX_ resp);
            if (resp[0] == GOPERL_RESP_FAIL) {
                char msg[512];
                size_t ml = resp_len - 1;
                if (ml > sizeof(msg) - 1) ml = sizeof(msg) - 1;
                memcpy(msg, resp + 1, ml);
                msg[ml] = '\0';
                free(resp);
                Perl_croak(aTHX_ "%s", msg);
            }
            /* claimed: [1][1][its length][the module's struct][its
             * precedence][has parse] */
            if (goperl_resp_word(resp, resp_len, 0)) {
                const goperl_gword_t len = goperl_resp_word(resp, resp_len, 1);
                const uint64_t host = goperl_resp_word(resp, resp_len, 2);
                const goperl_gword_t prec = goperl_resp_word(resp, resp_len, 3);
                const bool has_parse = goperl_resp_word(resp, resp_len, 4) != 0;
                free(resp);
                goperl_infix_def *&d = g_infix_defs[host];
                if (!d) {
                    d = (goperl_infix_def *)calloc(1, sizeof *d);
                    if (!d) Perl_croak(aTHX_ "Out of memory!");
                    d->host = host;
                }
                d->def.prec = (enum Perl_custom_infix_precedence)prec;
                d->def.parse = has_parse ? goperl_infix_parse : NULL;
                d->def.build_op = goperl_infix_build;
                *def = &d->def;
                return (STRLEN)len;
            }
            free(resp);
        }
    }
    if (g_next_infix_plugin)
        return (*g_next_infix_plugin)(aTHX_ opname, oplen, def);
    return 0;
}

/* Where the save stack and the scope stack stood when each native call
 * that is running was made (innermost last).
 *
 * When native code croaks, perl unwinds the scopes between the croak and
 * the eval that catches it before it jumps, so a destructor a function
 * registered for one of its own variables (SAVEDESTRUCTOR_X(cleanup,
 * &local)) runs while the function's frame still exists. Here the jump out
 * of the native frames comes first and this interpreter's die after it, so
 * the host asks for the scopes of the call to be left right before it
 * jumps (GOPERL_HOOK_UNWIND): the running call is the innermost, the last
 * entry. When the interpreter itself is what jumps (it died inside an
 * operation of the call, and the eval that catches that is further out),
 * it has left every scope down to that eval already, and the stacks are
 * below the call's marks.
 *
 * Every call takes its entry off when it returns, the entries of deeper
 * calls with it: the code after the call of the host runs also when the
 * call ended by a jump of the interpreter, which goes on afterwards. */
struct goperl_native_mark {
    I32 save_ix;  /* PL_savestack_ix */
    I32 scope_ix; /* PL_scopestack_ix */
};
static std::vector<goperl_native_mark> g_native_marks;
/* -> the number of the calls running below this one, which
 * goperl_native_leave takes back */
static size_t goperl_native_enter(pTHX) {
    const size_t below = g_native_marks.size();
    g_native_marks.push_back({PL_savestack_ix, PL_scopestack_ix});
    return below;
}
static void goperl_native_leave(size_t below) {
    if (g_native_marks.size() > below) g_native_marks.resize(below);
}

/* One call of native code through the host-call channel: kind and its
 * words go out, the response comes back (the caller frees it; *rlen is its
 * length, the result starts at byte 1). A failure reported by the host is
 * this interpreter's die. */
static unsigned char *goperl_host_call(pTHX_ goperl_gword_t kind, const goperl_gword_t *words,
                                       uint32_t nwords, uint32_t *rlen) {
    goperl_gword_t buf[1 + 8];
    if (nwords > 8) Perl_croak(aTHX_ "goperl: host call with too many words");
    buf[0] = kind;
    for (uint32_t i = 0; i < nwords; i++) buf[1 + i] = words[i];
    const size_t native_below = goperl_native_enter(aTHX);
    int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_HOST_METHOD_ID, buf,
                                         sizeof(goperl_gword_t) * (1 + nwords));
    goperl_native_leave(native_below);
    if (rc == GOPERL_CALLBACK_JUMPED) {
        *rlen = 0;
        return NULL;
    }
    uint32_t resp_len;
    unsigned char *resp = goperl_callback_response(rc, &resp_len);
    if (resp == nullptr || resp_len < 1)
        Perl_croak(aTHX_ "goperl: host call %u: empty response", (unsigned)kind);
    goperl_die_if_thrown(aTHX_ resp);
    if (resp[0] == GOPERL_RESP_FAIL) {
        char msg[512];
        size_t ml = resp_len - 1;
        if (ml > sizeof(msg) - 1) ml = sizeof(msg) - 1;
        memcpy(msg, resp + 1, ml);
        msg[ml] = '\0';
        free(resp);
        Perl_croak(aTHX_ "%s", msg);
    }
    *rlen = resp_len;
    return resp;
}

/* Op checkers of native code. A module that assigns PL_check[type] (or
 * calls wrap_op_checker) gets every op of that type the compiler builds:
 * the entry of this interpreter's table becomes a function that knows its
 * type and hands the op to the host, and the entry that was there is kept,
 * for the module to call as the checker it replaced. */
static Perl_check_t g_newop_orig[MAXO];

static OP *goperl_newop_dispatch(pTHX_ int type, OP *o) {
    if (g_go_cb == 0) {
        Perl_check_t orig = g_newop_orig[type];
        return orig ? orig(aTHX_ o) : o;
    }
    const goperl_gword_t words[2] = {(goperl_gword_t)type, GOPERL_ADDR(o)};
    uint32_t rlen = 0;
    unsigned char *resp = goperl_host_call(aTHX_ GOPERL_HOST_NEWOP, words, 2, &rlen);
    OP *const r = (OP *)(uintptr_t)goperl_resp_word(resp, rlen, 0);
    free(resp);
    return r;
}
template <int N> static OP *goperl_newop_tramp(pTHX_ OP *o) {
    return goperl_newop_dispatch(aTHX_ N, o);
}
template <std::size_t... I>
static constexpr std::array<Perl_check_t, sizeof...(I)> goperl_newop_tramps_make(
    std::index_sequence<I...>) {
    return {{&goperl_newop_tramp<(int)I>...}};
}
static const std::array<Perl_check_t, MAXO> goperl_newop_tramps =
    goperl_newop_tramps_make(std::make_index_sequence<MAXO>{});

/* Call checkers of native code (cv_set_call_checker). The checker the
 * interpreter is given is this function, and the object it keeps for the
 * sub is an array of the host's id of the module's function and the
 * module's own object: a call checker is not told which sub it is called
 * for, only handed that object. */
static OP *goperl_callck_tramp(pTHX_ OP *entersubop, GV *namegv, SV *ckobj) {
    AV *pair = (AV *)ckobj;
    SV **idp = av_fetch(pair, 0, 0);
    SV **objp = av_fetch(pair, 1, 0);
    if (g_go_cb == 0 || !idp)
        return ck_entersub_args_proto_or_list(entersubop, namegv, (SV *)namegv);
    const goperl_gword_t words[4] = {(goperl_gword_t)SvUV(*idp), GOPERL_ADDR(entersubop),
                                     GOPERL_ADDR(namegv), GOPERL_ADDR(objp ? *objp : NULL)};
    uint32_t rlen = 0;
    unsigned char *resp = goperl_host_call(aTHX_ GOPERL_HOST_CALLCK, words, 4, &rlen);
    OP *const r = (OP *)(uintptr_t)goperl_resp_word(resp, rlen, 0);
    free(resp);
    return r;
}

/* The peephole optimizer hooks of native code (PL_peepp and PL_rpeepp),
 * the same way: [0] is PL_peepp's previous value, [1] PL_rpeepp's. */
static peep_t g_peep_orig[2];

static void goperl_peep_dispatch(pTHX_ int which, OP *o) {
    if (g_go_cb == 0) {
        if (g_peep_orig[which]) g_peep_orig[which](aTHX_ o);
        return;
    }
    const goperl_gword_t words[2] = {(goperl_gword_t)which, GOPERL_ADDR(o)};
    uint32_t rlen = 0;
    free(goperl_host_call(aTHX_ GOPERL_HOST_PEEP, words, 2, &rlen));
}
static void goperl_peep_tramp(pTHX_ OP *o) { goperl_peep_dispatch(aTHX_ 0, o); }
static void goperl_rpeep_tramp(pTHX_ OP *o) { goperl_peep_dispatch(aTHX_ 1, o); }

/* A function native code gave to call_atexit: it runs when the interpreter
 * is destroyed. */
static void goperl_atexit_thunk(pTHX_ void *p) {
    if (g_go_cb == 0) return;
    const goperl_gword_t words[1] = {GOPERL_ADDR(p)};
    uint32_t rlen = 0;
    free(goperl_host_call(aTHX_ GOPERL_HOST_ATEXIT, words, 1, &rlen));
}

/* Source filters of native code (filter_add with a function of its own).
 * The filter the interpreter runs is one of these functions, one per
 * function of native code (by the host's id of it): the filter's data
 * stays the module's (FILTER_DATA), and the function a filter has, which
 * filter_del looks for and a module compares with its own, tells the id. */
#define GOPERL_FILTERS_MAX 64
static I32 goperl_filter_dispatch(pTHX_ int id, int idx, SV *buf, int maxlen) {
    if (g_go_cb == 0) return -1;
    const goperl_gword_t words[4] = {(goperl_gword_t)id, (goperl_gword_t)(int64_t)idx,
                                     GOPERL_ADDR(buf), (goperl_gword_t)(int64_t)maxlen};
    uint32_t rlen = 0;
    unsigned char *resp = goperl_host_call(aTHX_ GOPERL_HOST_FILTER, words, 4, &rlen);
    const I32 r = (I32)(int64_t)goperl_resp_word(resp, rlen, 0);
    free(resp);
    return r;
}
template <int N> static I32 goperl_filter_tramp(pTHX_ int idx, SV *buf, int maxlen) {
    return goperl_filter_dispatch(aTHX_ N + 1, idx, buf, maxlen);
}
template <std::size_t... I>
static constexpr std::array<filter_t, sizeof...(I)> goperl_filter_tramps_make(
    std::index_sequence<I...>) {
    return {{&goperl_filter_tramp<(int)I>...}};
}
static const std::array<filter_t, GOPERL_FILTERS_MAX> goperl_filter_tramps =
    goperl_filter_tramps_make(std::make_index_sequence<GOPERL_FILTERS_MAX>{});
/* The id (1-based) of the function of native code a filter function runs,
 * or 0 for a function of the interpreter's. */
static uint32_t goperl_filter_id(filter_t fn) {
    for (std::size_t i = 0; i < goperl_filter_tramps.size(); i++)
        if (goperl_filter_tramps[i] == fn) return (uint32_t)i + 1;
    return 0;
}

/* Block hooks of native code (blockhook_register). Each is a BHK of the
 * interpreter's whose entries call the module's through the host-call
 * channel, by the hook's id (its index here, plus one): [id][which][arg],
 * which 0 start (arg: full), 1 pre_end and 2 post_end (arg: the op, the
 * answer the op that takes its place), 3 eval (arg: the op). The flags
 * are the module's: an entry it leaves unset is not called. */
#define GOPERL_BHK_MAX 16
static BHK g_native_bhk[GOPERL_BHK_MAX];
static uint32_t g_native_bhk_n = 0;
static goperl_gword_t goperl_bhk_dispatch(pTHX_ int id, goperl_gword_t which,
                                          goperl_gword_t arg) {
    if (g_go_cb == 0) return arg;
    const goperl_gword_t words[3] = {(goperl_gword_t)id, which, arg};
    uint32_t rlen = 0;
    unsigned char *resp = goperl_host_call(aTHX_ GOPERL_HOST_BHK, words, 3, &rlen);
    const goperl_gword_t r = rlen >= 1 + 8 ? goperl_resp_word(resp, rlen, 0) : arg;
    free(resp);
    return r;
}
template <int K> static void goperl_bhk_start_tramp(pTHX_ int full) {
    goperl_bhk_dispatch(aTHX_ K + 1, 0, (goperl_gword_t)(int64_t)full);
}
template <int K> static void goperl_bhk_pre_end_tramp(pTHX_ OP **o) {
    *o = (OP *)(uintptr_t)goperl_bhk_dispatch(aTHX_ K + 1, 1, GOPERL_ADDR(*o));
}
template <int K> static void goperl_bhk_post_end_tramp(pTHX_ OP **o) {
    *o = (OP *)(uintptr_t)goperl_bhk_dispatch(aTHX_ K + 1, 2, GOPERL_ADDR(*o));
}
template <int K> static void goperl_bhk_eval_tramp(pTHX_ OP *const o) {
    goperl_bhk_dispatch(aTHX_ K + 1, 3, GOPERL_ADDR(o));
}
struct goperl_bhk_entries {
    void (*start)(pTHX_ int);
    void (*pre_end)(pTHX_ OP **);
    void (*post_end)(pTHX_ OP **);
    void (*eval)(pTHX_ OP *const);
};
template <std::size_t... I>
static constexpr std::array<goperl_bhk_entries, sizeof...(I)> goperl_bhk_tramps_make(
    std::index_sequence<I...>) {
    return {{{&goperl_bhk_start_tramp<(int)I>, &goperl_bhk_pre_end_tramp<(int)I>,
              &goperl_bhk_post_end_tramp<(int)I>, &goperl_bhk_eval_tramp<(int)I>}...}};
}
static const std::array<goperl_bhk_entries, GOPERL_BHK_MAX> goperl_bhk_tramps =
    goperl_bhk_tramps_make(std::make_index_sequence<GOPERL_BHK_MAX>{});

/* The perl stack as a pp function on the host sees it.
 *
 * A pp function reads and writes the argument stack and the mark stack
 * directly, and a module's own (the function of a custom op, or one it put
 * in place of an op's) runs on the host, where it has a copy of both. The
 * copy is made when the function is called; what the function left in it
 * comes back with the next op, and around every operation of the function
 * that runs interpreter code on the stack (the pp function it stands in
 * for) the two are brought in line again.
 *
 * The state, as words: the depth of the stack, the index of the top mark,
 * how many marks follow (the top GOPERL_PP_MARKS_MAX at most), those marks
 * from the lowest, then the values of the stack from index 1. */
#define GOPERL_PP_MARKS_MAX 32
static void goperl_pp_stack_words(pTHX_ std::vector<goperl_gword_t> &out) {
    SSize_t depth = PL_stack_sp - PL_stack_base;
    if (depth < 0) depth = 0;
    const SSize_t top = PL_markstack_ptr - PL_markstack;
    const SSize_t nmarks = top > GOPERL_PP_MARKS_MAX ? GOPERL_PP_MARKS_MAX : (top > 0 ? top : 0);
    out.push_back((goperl_gword_t)depth);
    out.push_back((goperl_gword_t)(int64_t)top);
    out.push_back((goperl_gword_t)nmarks);
    for (SSize_t i = top - nmarks + 1; i <= top; i++)
        out.push_back((goperl_gword_t)(int64_t)PL_markstack[i]);
    for (SSize_t i = 1; i <= depth; i++)
        out.push_back(GOPERL_ADDR(PL_stack_base[i]));
}

/* What the host's copy became: [depth][lo] and the values of the indices
 * above lo up to depth, then [top mark][mlo] and the marks above mlo up to
 * the top, as words. Returns false when the words are not that. */
static bool goperl_pp_stack_apply(pTHX_ const unsigned char *p, size_t len) {
    const size_t nw = len / 8;
    if (nw < 2) return false;
    goperl_gword_t w;
    memcpy(&w, p, 8);
    const int64_t depth = (int64_t)w;
    memcpy(&w, p + 8, 8);
    const int64_t lo = (int64_t)w;
    if (depth < 0 || lo < 0 || lo > depth) return false;
    const size_t nvals = (size_t)(depth - lo);
    if (nw < 2 + nvals + 2) return false;
    const SSize_t have = PL_stack_max - PL_stack_base;
    if (depth >= have) {
        SV **sp = PL_stack_sp;
        (void)stack_grow(sp, sp, depth - (PL_stack_sp - PL_stack_base) + 1);
    }
    for (size_t i = 0; i < nvals; i++) {
        memcpy(&w, p + 8 * (2 + i), 8);
        PL_stack_base[lo + 1 + (SSize_t)i] = (SV *)(uintptr_t)w;
    }
    PL_stack_sp = PL_stack_base + depth;
    const unsigned char *q = p + 8 * (2 + nvals);
    memcpy(&w, q, 8);
    const int64_t top = (int64_t)w;
    memcpy(&w, q + 8, 8);
    const int64_t mlo = (int64_t)w;
    if (top < 0 || mlo < 0 || mlo > top) return false;
    const size_t nmarks = (size_t)(top - mlo);
    if (nw < 2 + nvals + 2 + nmarks) return false;
    while (PL_markstack + top >= PL_markstack_max) (void)markstack_grow();
    for (size_t i = 0; i < nmarks; i++) {
        memcpy(&w, q + 8 * (2 + i), 8);
        PL_markstack[mlo + 1 + (SSize_t)i] = (Stack_off_t)(int64_t)w;
    }
    PL_markstack_ptr = PL_markstack + top;
    return true;
}

static OP *goperl_call_pp_hook(pTHX_ OP *op) {
    if (g_go_cb == 0)
        return op->op_ppaddr == goperl_pp_type_hook && op->op_type < MAXO
                   ? goperl_pp_original(op->op_type)(aTHX)
                   : op->op_ppaddr(aTHX);
    /* request: the op, its type, and the stack as it is */
    std::vector<goperl_gword_t> buf;
    buf.push_back(GOPERL_ADDR(op));
    buf.push_back((goperl_gword_t)op->op_type);
    goperl_pp_stack_words(aTHX_ buf);
    const size_t native_below = goperl_native_enter(aTHX);
    int64_t rc = wasmify_callback_invoke(
        g_go_cb, GOPERL_PP_HOOK_METHOD_ID, buf.data(),
        sizeof(goperl_gword_t) * buf.size());
    goperl_native_leave(native_below);
    /* nothing with a destructor may be alive across a croak */
    std::vector<goperl_gword_t>().swap(buf);
    if (rc == GOPERL_CALLBACK_JUMPED) return NULL;
    uint32_t resp_len;
    unsigned char *resp = goperl_callback_response(rc, &resp_len);
    if (resp == nullptr || resp_len < 1)
        Perl_croak(aTHX_ "pp hook: empty response");
    goperl_die_if_thrown(aTHX_ resp);
    if (resp[0] == GOPERL_RESP_FAIL) {
        char msg[512];
        size_t ml = resp_len - 1;
        if (ml > sizeof(msg) - 1) ml = sizeof(msg) - 1;
        memcpy(msg, resp + 1, ml);
        msg[ml] = '\0';
        free(resp);
        Perl_croak(aTHX_ "%s", msg);
    }
    /* [1][next op][stack state, when the function changed its copy] */
    OP *const next = (OP *)(uintptr_t)goperl_resp_word(resp, resp_len, 0);
    const bool applied = resp_len <= 1 + 8 ||
                         goperl_pp_stack_apply(aTHX_ resp + 1 + 8, resp_len - (1 + 8));
    free(resp);
    if (!applied) Perl_croak(aTHX_ "pp hook: malformed stack state");
    return next;
}
static OP *goperl_pp_hooked_op(pTHX) {
    return goperl_call_pp_hook(aTHX_ PL_op);
}
/* The pp function of the ops made while their type was claimed: the host's
 * while the claim lasts, the interpreter's own after it was given up. */
static OP *goperl_pp_type_hook(pTHX) {
    OP *op = PL_op;
    const unsigned t = op->op_type;
    if (t < MAXO && g_pp_hooked[t]) return goperl_call_pp_hook(aTHX_ op);
    return goperl_pp_original(t < MAXO ? t : OP_NULL)(aTHX);
}

/* A custom op native code registered (custom_op_register) has a function
 * of its own here too, which does what goperl_pp_hooked_op does: the
 * interpreter knows a custom op by its pp function, so the name, the
 * description and the class the module registered are this interpreter's
 * for the ops that run it (B, the op dumper, op_class). */
#define GOPERL_PP_TRAMPS 256
template <int N> static OP *goperl_pp_tramp(pTHX) {
    return goperl_call_pp_hook(aTHX_ PL_op);
}
template <std::size_t... I>
static constexpr std::array<Perl_ppaddr_t, sizeof...(I)> goperl_pp_tramps_make(
    std::index_sequence<I...>) {
    return {{&goperl_pp_tramp<(int)I>...}};
}
static const std::array<Perl_ppaddr_t, GOPERL_PP_TRAMPS> goperl_pp_tramps =
    goperl_pp_tramps_make(std::make_index_sequence<GOPERL_PP_TRAMPS>{});
static XOP g_xops[GOPERL_PP_TRAMPS];
static uint32_t g_xop_n = 0;
/* Whether the pp function of an op is one that hands the op to the host. */
static bool goperl_pp_is_hook(Perl_ppaddr_t fn) {
    if (fn == goperl_pp_hooked_op || fn == goperl_pp_type_hook) return true;
    for (uint32_t i = 0; i < g_xop_n; i++)
        if (goperl_pp_tramps[i] == fn) return true;
    return false;
}

static inline OP *goperl_exec_op(pTHX_ OP *op) {
    return op->op_ppaddr(aTHX);
}

/* Save-stack destructor bridging save_destructor_x for host modules: fires
 * the host id over GOPERL_DTOR_METHOD_ID when the guest scope pops. */
static void goperl_dtor_thunk(pTHX_ void *p) {
    goperl_gword_t id = GOPERL_ADDR(p);
    if (g_go_cb == 0) return;
    int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_DTOR_METHOD_ID, &id,
                                         sizeof(id));
    if (rc == GOPERL_CALLBACK_JUMPED) return;
    uint32_t resp_len;
    unsigned char *resp = goperl_callback_response(rc, &resp_len);
    if (resp == nullptr) return;
    /* The destructor died: so does the scope's end, as with a destructor
     * of perl's own (the die goes on to the nearest eval, also when the
     * scope ends because of an earlier one). */
    goperl_die_if_thrown(aTHX_ resp);
    if (resp_len >= 1 && resp[0] == GOPERL_RESP_FAIL) {
        SV *msg = newSVpvn_flags((const char *)resp + 1, resp_len - 1, SVs_TEMP);
        free(resp);
        croak_sv(msg);
    }
    free(resp);
}

/* PL_runops replacement: checks the interrupt flag on every opcode. When set,
 * Perl_croak longjmps out to the nearest eval trap — for perl_eval that is the
 * `eval $src` inside the wrapper, so the eval returns ok=false with the message
 * in $@, exactly like a Perl-level die. Otherwise identical to
 * Perl_runops_standard (see run.c), plus the pp-hook dispatch above.
 *
 * A flag value with GOPERL_INTERRUPT_SIGNAL set is a terminating signal
 * delivered to this (pseudo-)process: its low bits are the signal number.
 * Unlike an interruption it cannot be caught by eval, and the process ends
 * where it stands, as one killed by a signal does: no END block and no
 * destructor runs. */
#define GOPERL_INTERRUPT_SIGNAL 0x100u
static void goperl_interrupt_now(pTHX) {
    const uint32_t flag = g_interrupt;
    g_interrupt = 0;
    if (flag & GOPERL_INTERRUPT_SIGNAL)
        wasi_signal_die((int)(flag & 0x7f));
    Perl_croak(aTHX_ "Perl execution interrupted");
}
/* What PERL_ASYNC_CHECK delivers, as a sub: native code asks for it in the
 * middle of a call of its own, and a handler that dies must not unwind
 * past that call, so the delivery is called as code an eval catches. The
 * sub is the bridge's, no code of the program's: the debugger's DB::sub is
 * not entered for it (G_NODEBUG), as it is not on perl, which has no such
 * sub. */
XS(goperl_xs_deliver_pending) {
    dXSARGS;
    PERL_UNUSED_VAR(items);
    if (g_interrupt) goperl_interrupt_now(aTHX);
    PERL_ASYNC_CHECK();
    XSRETURN_EMPTY;
}
static bool goperl_deliver_pending(pTHX) {
    if (!g_interrupt && !PL_sig_pending) return false;
    static CV *deliver = NULL;
    if (!deliver) deliver = newXS(NULL, goperl_xs_deliver_pending, __FILE__);
    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);
    PUTBACK;
    call_sv((SV *)deliver, G_VOID | G_DISCARD | G_NOARGS | G_EVAL | G_NODEBUG);
    const bool died = SvTRUE(ERRSV);
    FREETMPS;
    LEAVE;
    return died;
}

static int runops_interruptible(pTHX) {
    OP *op = PL_op;
    while ((PL_op = op = goperl_exec_op(aTHX_ op))) {
        if (g_interrupt) goperl_interrupt_now(aTHX);
    }
    PERL_ASYNC_CHECK();

    /* The run loop ends with no taint pending, as Perl_runops_standard's
     * does: a die that unwinds to an eval leaves PL_tainted as the failing
     * op set it, and the code that called the loop (the parser compiling the
     * next `use`, for a BEGIN block) must not inherit it. */
    TAINT_NOT;
    return 0;
}

/* A run loop of native code: PL_runops given a function of a module's own
 * (Devel::Cover runs every op through one). The host's id of the function,
 * while it is the loop; the loop runs on the host, from PL_op, until an op
 * returns no next op. */
static uint32_t g_runops_native = 0;
static int goperl_runops_native(pTHX) {
    if (g_go_cb == 0 || g_runops_native == 0) return runops_interruptible(aTHX);
    const goperl_gword_t words[1] = {g_runops_native};
    uint32_t rlen = 0;
    free(goperl_host_call(aTHX_ GOPERL_HOST_RUNOPS, words, 1, &rlen));
    TAINT_NOT;
    return 0;
}

/* The signal handler of native code that is PL_sighandlerp (Tk's Event
 * marks a handler of its own with it): the host's id of the function. The
 * interpreter calls it for a signal that has arrived, where it would run
 * the %SIG handler. */
static uint32_t g_sighandler_native = 0;
static Signal_t goperl_sighandler_native(int sig) {
    if (g_go_cb == 0 || g_sighandler_native == 0) {
        Perl_sighandler(sig);
        return;
    }
    const goperl_gword_t words[2] = {g_sighandler_native, (goperl_gword_t)sig};
    uint32_t rlen = 0;
    free(goperl_host_call(aTHX_ GOPERL_HOST_SIGHANDLER, words, 2, &rlen));
}

/* ---- helpers ------------------------------------------------------------- */
static std::string sv_to_std(pTHX_ SV *sv) {
    if (!sv || !SvOK(sv)) return std::string();
    STRLEN len = 0;
    const char *p = SvPV(sv, len);
    return std::string(p, static_cast<size_t>(len));
}

/* ---- typed value codec ---------------------------------------------------
 * The node/envelope wire format documented in pl.h. Little-endian, packed;
 * every value crosses with its kind, references cross by handle. */

enum : uint8_t {
    NODE_UNDEF    = 0,
    NODE_BOOL     = 1,
    NODE_INT      = 2,
    NODE_FLOAT    = 3,
    NODE_STRING   = 4,
    NODE_REF      = 5,
    NODE_HOSTFUNC = 6,
    NODE_FLATTEN  = 7,
    NODE_UINT     = 8,
};

enum : uint8_t {
    REFK_SCALAR = 0,
    REFK_ARRAY  = 1,
    REFK_HASH   = 2,
    REFK_CODE   = 3,
    REFK_GLOB   = 4,
    REFK_IO     = 5,
    REFK_FORMAT = 6,
    REFK_REGEXP = 7,
    REFK_OTHER  = 8,
};

enum : uint8_t { ENV_OK = 0, ENV_DIE = 1, ENV_EXIT = 2 };

static void put_u8(std::string &b, uint8_t v) { b.push_back(static_cast<char>(v)); }
static void put_u16(std::string &b, uint16_t v) {
    for (int i = 0; i < 2; i++) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
static void put_u32(std::string &b, uint32_t v) {
    for (int i = 0; i < 4; i++) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
static void put_u64(std::string &b, uint64_t v) {
    for (int i = 0; i < 8; i++) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
static void put_i64(std::string &b, int64_t v) { put_u64(b, static_cast<uint64_t>(v)); }
static void put_f64(std::string &b, double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    put_u64(b, bits);
}
static void put_bytes(std::string &b, const char *p, uint32_t len) {
    put_u32(b, len);
    if (len) b.append(p, len);
}

/* NodeReader walks a node buffer with bounds checking; any overrun flips
 * fail and every later read yields zero, so decoding is total. */
struct NodeReader {
    const unsigned char *p;
    const unsigned char *end;
    bool fail;
    NodeReader(const char *data, uint32_t len)
        : p(reinterpret_cast<const unsigned char *>(data)), end(p + len), fail(false) {}
    bool need(size_t n) {
        if (fail || static_cast<size_t>(end - p) < n) { fail = true; return false; }
        return true;
    }
    uint8_t get_u8() { if (!need(1)) return 0; return *p++; }
    uint16_t get_u16() {
        if (!need(2)) return 0;
        uint16_t v = static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1]) << 8;
        p += 2;
        return v;
    }
    uint32_t get_u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(p[i]) << (8 * i);
        p += 4;
        return v;
    }
    uint64_t get_u64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i);
        p += 8;
        return v;
    }
    int64_t get_i64() { return static_cast<int64_t>(get_u64()); }
    double get_f64() {
        uint64_t bits = get_u64();
        double v;
        memcpy(&v, &bits, 8);
        return v;
    }
    const char *get_bytes(uint32_t len) {
        if (!need(len)) return nullptr;
        const char *q = reinterpret_cast<const char *>(p);
        p += len;
        return q;
    }
};

static std::string env_die(const std::string &msg) {
    std::string out;
    put_u8(out, ENV_DIE);
    put_bytes(out, msg.data(), static_cast<uint32_t>(msg.size()));
    return out;
}

/* ---- reference-handle registry (identity-preserving pins) ----------------
 * Pins the actual referent: the registry owns one reference (an RV it holds
 * a refcount through), ids are deduplicated by referent address so the same
 * Perl reference always crosses under the same id, and pin counts align the
 * guest refcount with host-side wrapper liveness (the host releases pins via
 * perl_release). Lives in plain C++ maps — copy-on-write instance snapshots
 * copy them along with the rest of linear memory. */
static std::unordered_map<uint64_t, SV *> g_reg_by_id;
static std::unordered_map<UV, uint64_t> g_reg_by_addr;
static std::unordered_map<uint64_t, uint64_t> g_reg_pins;
static uint64_t g_reg_next = 0;

static uint64_t reg_pin(pTHX_ SV *rv) {
    SV *ref = SvRV(rv);
    UV addr = PTR2UV(ref);
    uint64_t id;
    auto it = g_reg_by_addr.find(addr);
    if (it == g_reg_by_addr.end()) {
        id = ++g_reg_next;
        g_reg_by_id[id] = newRV_inc(ref);
        g_reg_by_addr[addr] = id;
    } else {
        id = it->second;
    }
    g_reg_pins[id]++;
    return id;
}

static SV *reg_lookup(uint64_t id) {
    auto it = g_reg_by_id.find(id);
    return it == g_reg_by_id.end() ? nullptr : it->second;
}

static void reg_release(pTHX_ uint64_t id) {
    auto it = g_reg_by_id.find(id);
    if (it == g_reg_by_id.end()) return;
    if (--g_reg_pins[id] > 0) return;
    SV *rv = it->second;
    g_reg_by_addr.erase(PTR2UV(SvRV(rv)));
    g_reg_by_id.erase(it);
    g_reg_pins.erase(id);
    SvREFCNT_dec(rv);
}

static void reg_clear(pTHX) {
    for (auto &e : g_reg_by_id) SvREFCNT_dec(e.second);
    g_reg_by_id.clear();
    g_reg_by_addr.clear();
    g_reg_pins.clear();
    g_reg_next = 0;
}

static uint8_t refkind_of(pTHX_ SV *ref) {
    switch (SvTYPE(ref)) {
    case SVt_PVAV: return REFK_ARRAY;
    case SVt_PVHV: return REFK_HASH;
    case SVt_PVCV: return REFK_CODE;
    case SVt_PVFM: return REFK_FORMAT;
    case SVt_PVIO: return REFK_IO;
    case SVt_REGEXP: return REFK_REGEXP;
    case SVt_PVGV: return isGV_with_GP(ref) ? REFK_GLOB : REFK_SCALAR;
    default: return REFK_SCALAR;
    }
}

/* encode_sv appends one node for sv. References are pinned (the host owns
 * the new pin); everything else crosses by value with its kind. Get-magic
 * runs here, so a tied FETCH executes like ordinary Perl code — a die from
 * it outside an eval frame follows Perl's own fatal-die semantics. */
static void encode_sv(pTHX_ SV *sv, std::string &out) {
    if (sv) SvGETMAGIC(sv);
    if (!sv || !SvOK(sv)) {
        put_u8(out, NODE_UNDEF);
        return;
    }
    if (SvROK(sv)) {
        SV *ref = SvRV(sv);
        uint64_t id = reg_pin(aTHX_ sv);
        put_u8(out, NODE_REF);
        put_u64(out, id);
        put_u8(out, refkind_of(aTHX_ ref));
        const char *cls = nullptr;
        if (SvOBJECT(ref)) {
            HV *stash = SvSTASH(ref);
            if (stash) cls = HvNAME(stash);
        }
        size_t clen = cls ? strlen(cls) : 0;
        if (clen > 0xFFFF) clen = 0xFFFF;
        put_u16(out, static_cast<uint16_t>(clen));
        if (clen) out.append(cls, clen);
        return;
    }
    if (SvIsBOOL(sv)) {
        put_u8(out, NODE_BOOL);
        put_u8(out, SvTRUE(sv) ? 1 : 0);
        return;
    }
    if (SvIOK(sv)) {
        /* An integer scalar holds an IV or a UV; reading a UV above IV_MAX
         * as an IV would wrap it negative. The int node carries a signed
         * 64-bit value; a UV beyond that has a node of its own. */
        if (SvIsUV(sv)) {
            const uint64_t u = static_cast<uint64_t>(SvUV(sv));
            if (u > static_cast<uint64_t>(INT64_MAX)) {
                put_u8(out, NODE_UINT);
                put_u64(out, u);
                return;
            }
            put_u8(out, NODE_INT);
            put_i64(out, static_cast<int64_t>(u));
            return;
        }
        put_u8(out, NODE_INT);
        put_i64(out, static_cast<int64_t>(SvIV(sv)));
        return;
    }
    if (SvNOK(sv)) {
        put_u8(out, NODE_FLOAT);
        put_f64(out, static_cast<double>(SvNV(sv)));
        return;
    }
    STRLEN len = 0;
    const char *pv = SvPV(sv, len); /* POK, or stringify anything else */
    put_u8(out, NODE_STRING);
    put_u8(out, SvUTF8(sv) ? 1 : 0);
    put_bytes(out, pv, static_cast<uint32_t>(len));
}

static void encode_list(pTHX_ SV **items, I32 count, std::string &out) {
    put_u32(out, static_cast<uint32_t>(count));
    for (I32 i = 0; i < count; i++) encode_sv(aTHX_ items[i], out);
}

/* XS_goperl_go_thunk (defined with the Perl->Go dispatch below) backs every
 * host function value: an ANONYMOUS XS CV whose XSANY carries the host
 * function id — an ordinary code ref Perl can store, pass around, and call
 * later, with no Perl-source closure involved. */
XS(XS_goperl_go_thunk);

static SV *make_go_closure(pTHX_ uint32_t id) {
    CV *cv = newXS(NULL, XS_goperl_go_thunk, __FILE__);
    CvXSUBANY(cv).any_i32 = static_cast<int32_t>(id);
    return sv_2mortal(newRV_noinc((SV *)cv));
}

/* decode_node turns one wire node into a mortal SV. NODE_FLATTEN is handled
 * by the argument-push loops (it expands to many stack entries), never here.
 * On malformed input or a stale handle, sets err and returns NULL. */
static SV *decode_node(pTHX_ NodeReader &r, std::string &err) {
    uint8_t tag = r.get_u8();
    if (r.fail) { err = "malformed value node"; return nullptr; }
    switch (tag) {
    case NODE_UNDEF:
        return &PL_sv_undef;
    case NODE_BOOL:
        return boolSV(r.get_u8() != 0);
    case NODE_INT:
        return sv_2mortal(newSViv(static_cast<IV>(r.get_i64())));
    case NODE_UINT:
        return sv_2mortal(newSVuv(static_cast<UV>(r.get_u64())));
    case NODE_FLOAT:
        return sv_2mortal(newSVnv(static_cast<NV>(r.get_f64())));
    case NODE_STRING: {
        uint8_t utf8 = r.get_u8();
        uint32_t len = r.get_u32();
        const char *p = r.get_bytes(len);
        if (r.fail) { err = "malformed string node"; return nullptr; }
        SV *sv = newSVpvn(p ? p : "", len);
        if (utf8) SvUTF8_on(sv);
        return sv_2mortal(sv);
    }
    case NODE_REF: {
        uint64_t id = r.get_u64();
        (void)r.get_u8();                 /* refkind: advisory on decode */
        uint16_t clen = r.get_u16();
        (void)r.get_bytes(clen);          /* class: advisory on decode */
        if (r.fail) { err = "malformed ref node"; return nullptr; }
        SV *rv = reg_lookup(id);
        if (!rv) {
            char buf[64];
            snprintf(buf, sizeof(buf), "stale Perl reference handle %llu",
                     static_cast<unsigned long long>(id));
            err = buf;
            return nullptr;
        }
        return sv_2mortal(newRV_inc(SvRV(rv)));
    }
    case NODE_HOSTFUNC: {
        uint32_t id = r.get_u32();
        if (r.fail) { err = "malformed hostfunc node"; return nullptr; }
        return make_go_closure(aTHX_ id);
    }
    default:
        err = "unknown value node tag";
        return nullptr;
    }
}

/* goperl_go_dispatch is the Perl->Go half of the function bridge: encodes
 * args as a node list, dispatches to the host function bound under func_id
 * over the wasmify callback import, and decodes the response envelope into
 * mortal result SVs — croaking with the reported message on failure (the
 * caller is an XSUB, so the croak is an ordinary Perl die). Reference
 * arguments are pinned by encode_sv and OWNED by the host side (its wrapper
 * releases each pin), so a handler may keep a reference beyond the call. */
static void goperl_go_dispatch(pTHX_ int32_t func_id, SV **args, I32 nargs,
                               std::vector<SV *> &out) {
    if (g_go_cb == 0)
        Perl_croak(aTHX_ "no Go dispatcher registered for this instance");

    std::string req;
    put_u32(req, static_cast<uint32_t>(nargs));
    for (I32 i = 0; i < nargs; i++) encode_sv(aTHX_ args[i], req);

    int64_t rc = wasmify_callback_invoke(g_go_cb, func_id,
                                         const_cast<char *>(req.data()), req.size());
    if (rc == GOPERL_CALLBACK_JUMPED) return;
    uint32_t resp_len;
    char *resp = reinterpret_cast<char *>(goperl_callback_response(rc, &resp_len));
    if (resp == nullptr || resp_len < 1)
        Perl_croak(aTHX_ "Go function dispatch: empty response");

    NodeReader r(resp, resp_len);
    uint8_t status = r.get_u8();
    if (status != ENV_OK) {
        uint32_t elen = r.get_u32();
        const char *ep = r.get_bytes(elen);
        char msg[512];
        size_t n = (ep && !r.fail) ? elen : 0;
        if (n > sizeof(msg) - 1) n = sizeof(msg) - 1;
        memcpy(msg, ep ? ep : "", n);
        msg[n] = '\0';
        free(resp);
        Perl_croak(aTHX_ "%s", n ? msg : "Go function dispatch failed");
    }
    uint32_t count = r.get_u32();
    out.reserve(count);
    std::string derr;
    for (uint32_t i = 0; i < count && !r.fail; i++) {
        SV *sv = decode_node(aTHX_ r, derr);
        if (!sv) break;
        out.push_back(sv);
    }
    free(resp);
    if (out.size() != count) {
        Perl_croak(aTHX_ "Go function dispatch: %s",
                   derr.empty() ? "truncated response" : derr.c_str());
    }
}

/* Push the dispatch results as an XSUB's return list; scalar context
 * historically receives the FIRST result. Returns the item count. */
#define GOPERL_GO_RETURN(out)                                   \
    do {                                                        \
        if (GIMME_V == G_SCALAR) {                              \
            XSprePUSH;                                          \
            ST(0) = (out).empty() ? &PL_sv_undef : (out)[0];    \
            XSRETURN(1);                                        \
        }                                                       \
        XSprePUSH;                                              \
        EXTEND(SP, (SSize_t)(out).size());                      \
        for (size_t k_ = 0; k_ < (out).size(); k_++)            \
            ST(k_) = (out)[k_];                                 \
        XSRETURN((out).size());                                 \
    } while (0)

/* main::__plwasm_go_call($func_id, @args): the named entry the host's Bind
 * points its generated subs at. */
XS(XS___plwasm_go_call);
XS(XS___plwasm_go_call) {
    dXSARGS;
    if (items < 1) Perl_croak(aTHX_ "usage: __plwasm_go_call(func_id, args...)");
    IV func_id = SvIV(ST(0));
    std::vector<SV *> out;
    goperl_go_dispatch(aTHX_ static_cast<int32_t>(func_id), &ST(1), items - 1, out);
    GOPERL_GO_RETURN(out);
}

/* The anonymous host-function thunk: the id lives in XSANY (see
 * make_go_closure), the whole argument list is the host call's argument
 * list. */
XS(XS_goperl_go_thunk) {
    dXSARGS;
    int32_t func_id = XSANY.any_i32;
    std::vector<SV *> out;
    goperl_go_dispatch(aTHX_ func_id, items > 0 ? &ST(0) : nullptr, items, out);
    GOPERL_GO_RETURN(out);
}

/* Generic thunk backing every host-native XSUB (see pl.h "Native XS
 * support"). Marshals the call frame's SV tokens to the host over the
 * wasmify callback import and pushes the returned (mortal) SVs. */
static inline SV *goperl_static_immortal(pTHX_ uint64_t v);
XS(XS_goperl_native_thunk);
XS(XS_goperl_native_thunk) {
    dXSARGS;
    int32_t fn_id = XSANY.any_i32;
    if (g_go_cb == 0)
        Perl_croak(aTHX_ "no Go dispatcher registered for this instance");

    size_t payload_len =
        sizeof(goperl_gword_t) * (GOPERL_NATIVE_HDR_WORDS + (size_t)items);
    goperl_gword_t *payload = (goperl_gword_t *)malloc(payload_len);
    if (!payload) Perl_croak(aTHX_ "native XS dispatch: out of memory");
    payload[0] = (goperl_gword_t)(int64_t)fn_id;
    payload[1] = GOPERL_ADDR(cv); /* real XSUBs receive their CV */
    payload[2] = (goperl_gword_t)items;
    payload[3] = (goperl_gword_t)ax;
    payload[4] = GOPERL_ADDR(PL_stack_base);
    for (I32 i = 0; i < items; i++)
        payload[GOPERL_NATIVE_HDR_WORDS + i] = GOPERL_ADDR(ST(i));

    const size_t native_below = goperl_native_enter(aTHX);
    int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_NATIVE_METHOD_ID,
                                         payload, payload_len);
    goperl_native_leave(native_below);
    free(payload);
    if (rc == GOPERL_CALLBACK_JUMPED) return;

    uint32_t resp_len;
    unsigned char *resp = goperl_callback_response(rc, &resp_len);
    if (resp == nullptr || resp_len < 1)
        Perl_croak(aTHX_ "native XS dispatch: empty response");

    goperl_die_if_thrown(aTHX_ resp);
    if (resp[0] == GOPERL_RESP_FAIL) { /* the rest is the croak message */
        char msg[512];
        size_t n = resp_len - 1;
        if (n > sizeof(msg) - 1) n = sizeof(msg) - 1;
        memcpy(msg, resp + 1, n);
        msg[n] = '\0';
        free(resp);
        Perl_croak(aTHX_ "%s", msg);
    }
    /* [1][nret][the returned SVs] */
    if (resp_len < 1 + 8) { free(resp); Perl_croak(aTHX_ "native XS dispatch: short response"); }
    const goperl_gword_t nret = goperl_resp_word(resp, resp_len, 0);
    if (nret > ((size_t)resp_len - (1 + 8)) / 8) {
        free(resp);
        Perl_croak(aTHX_ "native XS dispatch: truncated response");
    }
    XSprePUSH;
    EXTEND(SP, (SSize_t)nret);
    for (goperl_gword_t k = 0; k < nret; k++) {
        const uint64_t w = goperl_resp_word(resp, resp_len, (uint32_t)(1 + k));
        SV *const imm = w < 0x40 ? goperl_static_immortal(aTHX_ w) : NULL;
        ST(k) = imm ? imm : (SV *)(uintptr_t)w;
    }
    free(resp);
    XSRETURN((IV)nret);
}

/* ---- value-operation XSUBs -----------------------------------------------
 * The bridge operations are ANONYMOUS C XSUBS — no Perl-source glue: each
 * receives already-decoded SVs on the stack and drives the perl API
 * directly (the lvalue-fetch + sv_setsv_mg / iterator patterns the pp_*
 * functions themselves use), so ties and overloads behave exactly as in
 * plain Perl. run_glue calls them through call_sv under G_EVAL, so a die —
 * a tied FETCH, a wrong reference type — is caught like Perl's own. The
 * CVs live in g_op_cv (created once at perl_new, invisible from Perl
 * space); clones inherit them with the copied memory. */

static AV *op_want_av(pTHX_ SV *sv) {
    if (!SvROK(sv) || SvTYPE(SvRV(sv)) != SVt_PVAV)
        Perl_croak(aTHX_ "Not an ARRAY reference");
    return (AV *)SvRV(sv);
}

static HV *op_want_hv(pTHX_ SV *sv) {
    if (!SvROK(sv) || SvTYPE(SvRV(sv)) != SVt_PVHV)
        Perl_croak(aTHX_ "Not a HASH reference");
    return (HV *)SvRV(sv);
}

/* $$ref (scalar dereference; a $$ on an aggregate ref dies like Perl's). */
XS(XS_goperl_op_deref);
XS(XS_goperl_op_deref) {
    dXSARGS;
    if (items < 1 || !SvROK(ST(0)))
        Perl_croak(aTHX_ "Not a SCALAR reference");
    SV *t = SvRV(ST(0));
    switch (SvTYPE(t)) {
    case SVt_PVAV: case SVt_PVHV: case SVt_PVCV: case SVt_PVFM:
        Perl_croak(aTHX_ "Not a SCALAR reference");
    default:
        break;
    }
    ST(0) = sv_mortalcopy(t); /* runs get-magic inside the eval frame */
    XSRETURN(1);
}

/* scalar @$av */
XS(XS_goperl_op_alen);
XS(XS_goperl_op_alen) {
    dXSARGS;
    if (items < 1) Perl_croak(aTHX_ "Not an ARRAY reference");
    AV *av = op_want_av(aTHX_ ST(0));
    SSize_t n = av_top_index(av) + 1; /* FETCHSIZE runs for tied arrays */
    XSprePUSH;
    mXPUSHs(newSViv((IV)n));
    XSRETURN(1);
}

/* $av->[$idx] */
XS(XS_goperl_op_aget);
XS(XS_goperl_op_aget) {
    dXSARGS;
    if (items < 2) Perl_croak(aTHX_ "Not an ARRAY reference");
    AV *av = op_want_av(aTHX_ ST(0));
    IV idx = SvIV(ST(1));
    SV **svp = av_fetch(av, idx, 0);
    SV *out = sv_mortalcopy(svp ? *svp : &PL_sv_undef);
    XSprePUSH;
    PUSHs(out);
    XSRETURN(1);
}

/* $av->[$idx] = $val */
XS(XS_goperl_op_aset);
XS(XS_goperl_op_aset) {
    dXSARGS;
    if (items < 3) Perl_croak(aTHX_ "Not an ARRAY reference");
    AV *av = op_want_av(aTHX_ ST(0));
    IV idx = SvIV(ST(1));
    SV **slot = av_fetch(av, idx, TRUE); /* lvalue fetch, like pp_aelem */
    if (!slot) Perl_croak(aTHX_ "Modification of non-creatable array value attempted");
    sv_setsv_mg(*slot, ST(2)); /* set-magic drives a tied STORE */
    XSprePUSH;
    XSRETURN(0);
}

/* push @$av, LIST */
XS(XS_goperl_op_apush);
XS(XS_goperl_op_apush) {
    dXSARGS;
    if (items < 1) Perl_croak(aTHX_ "Not an ARRAY reference");
    AV *av = op_want_av(aTHX_ ST(0));
    for (I32 i = 1; i < items; i++)
        av_push(av, newSVsv(ST(i))); /* av_push runs PUSH magic for ties */
    XSprePUSH;
    XSRETURN(0);
}

/* @$av */
XS(XS_goperl_op_avals);
XS(XS_goperl_op_avals) {
    dXSARGS;
    if (items < 1) Perl_croak(aTHX_ "Not an ARRAY reference");
    AV *av = op_want_av(aTHX_ ST(0));
    SSize_t n = av_top_index(av) + 1;
    XSprePUSH;
    EXTEND(SP, n);
    for (SSize_t i = 0; i < n; i++) {
        SV **svp = av_fetch(av, i, 0);
        PUSHs(sv_mortalcopy(svp ? *svp : &PL_sv_undef));
    }
    XSRETURN(n);
}

/* exists $hv->{$k} ? (1, $hv->{$k}) : (0) */
XS(XS_goperl_op_hget);
XS(XS_goperl_op_hget) {
    dXSARGS;
    if (items < 2) Perl_croak(aTHX_ "Not a HASH reference");
    HV *hv = op_want_hv(aTHX_ ST(0));
    SV *key = ST(1);
    bool exists = hv_exists_ent(hv, key, 0);
    XSprePUSH;
    EXTEND(SP, 2);
    PUSHs(boolSV(exists));
    if (exists) {
        HE *he = hv_fetch_ent(hv, key, 0, 0);
        PUSHs(sv_mortalcopy(he ? HeVAL(he) : &PL_sv_undef));
        XSRETURN(2);
    }
    XSRETURN(1);
}

/* $hv->{$k} = $val */
XS(XS_goperl_op_hset);
XS(XS_goperl_op_hset) {
    dXSARGS;
    if (items < 3) Perl_croak(aTHX_ "Not a HASH reference");
    HV *hv = op_want_hv(aTHX_ ST(0));
    HE *he = hv_fetch_ent(hv, ST(1), 1, 0); /* lvalue fetch, like pp_helem */
    if (!he) Perl_croak(aTHX_ "Modification of non-creatable hash value attempted");
    sv_setsv_mg(HeVAL(he), ST(2));
    XSprePUSH;
    XSRETURN(0);
}

/* delete $hv->{$k} */
XS(XS_goperl_op_hdel);
XS(XS_goperl_op_hdel) {
    dXSARGS;
    if (items < 2) Perl_croak(aTHX_ "Not a HASH reference");
    HV *hv = op_want_hv(aTHX_ ST(0));
    (void)hv_delete_ent(hv, ST(1), G_DISCARD, 0);
    XSprePUSH;
    XSRETURN(0);
}

/* keys %$hv (FIRSTKEY/NEXTKEY run for tied hashes) */
XS(XS_goperl_op_hkeys);
XS(XS_goperl_op_hkeys) {
    dXSARGS;
    if (items < 1) Perl_croak(aTHX_ "Not a HASH reference");
    HV *hv = op_want_hv(aTHX_ ST(0));
    hv_iterinit(hv);
    XSprePUSH;
    HE *he;
    SSize_t n = 0;
    while ((he = hv_iternext(hv)) != nullptr) {
        XPUSHs(hv_iterkeysv(he)); /* already mortal */
        n++;
    }
    XSRETURN(n);
}

/* [@_] */
XS(XS_goperl_op_newa);
XS(XS_goperl_op_newa) {
    dXSARGS;
    AV *av = newAV();
    for (I32 i = 0; i < items; i++) av_push(av, newSVsv(ST(i)));
    XSprePUSH;
    mXPUSHs(newRV_noinc((SV *)av));
    XSRETURN(1);
}

/* +{@_} (an odd trailing key gets undef, like list assignment to a hash) */
XS(XS_goperl_op_newh);
XS(XS_goperl_op_newh) {
    dXSARGS;
    HV *hv = newHV();
    for (I32 i = 0; i < items; i += 2) {
        SV *val = (i + 1 < items) ? newSVsv(ST(i + 1)) : newSV(0);
        if (!hv_store_ent(hv, ST(i), val, 0)) SvREFCNT_dec(val);
    }
    XSprePUSH;
    mXPUSHs(newRV_noinc((SV *)hv));
    XSRETURN(1);
}

/* The op CV table run_glue dispatches through. */
enum {
    GOP_DEREF = 0, GOP_ALEN, GOP_AGET, GOP_ASET, GOP_APUSH, GOP_AVALS,
    GOP_HGET, GOP_HSET, GOP_HDEL, GOP_HKEYS, GOP_NEWA, GOP_NEWH,
    GOP_COUNT,
};
static CV *g_op_cv[GOP_COUNT];

static void install_op_cvs(pTHX) {
    static const struct { int idx; XSUBADDR_t body; } ops[] = {
        {GOP_DEREF, XS_goperl_op_deref}, {GOP_ALEN, XS_goperl_op_alen},
        {GOP_AGET, XS_goperl_op_aget},   {GOP_ASET, XS_goperl_op_aset},
        {GOP_APUSH, XS_goperl_op_apush}, {GOP_AVALS, XS_goperl_op_avals},
        {GOP_HGET, XS_goperl_op_hget},   {GOP_HSET, XS_goperl_op_hset},
        {GOP_HDEL, XS_goperl_op_hdel},   {GOP_HKEYS, XS_goperl_op_hkeys},
        {GOP_NEWA, XS_goperl_op_newa},   {GOP_NEWH, XS_goperl_op_newh},
    };
    for (const auto &op : ops)
        g_op_cv[op.idx] = newXS(NULL, op.body, __FILE__);
}

/* The eval wrapper (run via eval_pv): redirect STDOUT/STDERR onto in-memory
 * scalars (the built-in PerlIO scalar layer), string-eval the user source, and
 * stash the result / captured streams / $@ into package globals we read back in
 * C. `local` restores the real STDOUT/STDERR when the block exits. The user
 * source is passed via $main::__plwasm_src (an SV, so no C-string escaping). */
static const char *EVAL_WRAPPER =
    "$main::__plwasm_out = ''; $main::__plwasm_err = '';"
    "{ local(*STDOUT, *STDERR);"
    "  open(STDOUT, '>', \\$main::__plwasm_out);"
    "  open(STDERR, '>', \\$main::__plwasm_err);"
    "  $main::__plwasm_result = eval $main::__plwasm_src;"
    "  $main::__plwasm_errsv = $@;"
    "}";

/* ---- @INC: the standard library takes the place of the built-in dirs ------
 * perl_parse fills @INC the way perl.c's S_init_perllib does: the PERL5LIB
 * directories first, then the library directories this perl was configured
 * with (sitearch, sitelib, archlib, privlib, ...), then "." where perl
 * still adds it. The configured directories are those of the cross-build
 * configuration: they do not exist here, and on a host filesystem they
 * could name an unrelated perl's files. The library this interpreter runs
 * with lives in lib_dir, so lib_dir goes where they were: after PERL5LIB —
 * a module found there shadows the standard library's copy, as on any
 * perl — and before ".". The site directories the interpreter was started
 * with (site_configure) go there too, in perl's order: sitearch, sitelib,
 * then the library.
 *
 * The built-in entries are identified by what they are, not by how they
 * look: the list below is S_init_perllib's (perl_inc_macro.h), in the order
 * it pushes them, built from the same configuration constants under the
 * same conditions. perl pushes each of them once, after the PERL5LIB
 * directories, so each is the LAST entry with that value that precedes the
 * one matched before it — walking the list backwards from the end of @INC.
 * A PERL5LIB directory that happens to have the same name is therefore
 * left alone. */
#if defined(APPLLIB_EXP) || defined(PERL_OTHERLIBDIRS)
#error "this configuration has built-in @INC entries inc_use_stdlib does not know"
#endif
/* The separator of a directory list, as perl.c defines it for itself. */
#ifndef PERLLIB_SEP
#define PERLLIB_SEP ':'
#endif
static const char *const BUILTIN_INC[] = {
#ifdef SITEARCH_EXP
    SITEARCH_EXP,
#endif
#ifdef SITELIB_EXP
    SITELIB_EXP,
#endif
#ifdef PERL_VENDORARCH_EXP
    PERL_VENDORARCH_EXP,
#endif
#ifdef PERL_VENDORLIB_EXP
    PERL_VENDORLIB_EXP,
#endif
#ifdef ARCHLIB_EXP
    ARCHLIB_EXP,
#endif
    PRIVLIB_EXP,
#if defined(SITELIB_STEM) && defined(PERL_INC_VERSION_LIST)
    SITELIB_STEM,
#endif
#if defined(PERL_VENDORLIB_STEM) && defined(PERL_INC_VERSION_LIST)
    PERL_VENDORLIB_STEM,
#endif
};

/* The @INC entries perl took from the environment when the interpreter
 * started: the PERL5LIB (or PERLLIB) directories and ".". A program run
 * under taint checks must not have them (perl -T adds neither), and by
 * then they cannot be told from directories added since by their values —
 * hence this record of the entries themselves. */
static AV *g_inc_from_environment = nullptr;

void program_inc_drop_environment(pTHX) {
    if (!g_inc_from_environment) return;
    AV *inc = GvAVn(PL_incgv);
    AV *kept = newAV();
    const SSize_t n = static_cast<SSize_t>(av_count(inc));
    const SSize_t m = static_cast<SSize_t>(av_count(g_inc_from_environment));
    for (SSize_t i = 0; i < n; i++) {
        SV **svp = av_fetch(inc, i, FALSE);
        if (!svp) continue;
        bool from_environment = false;
        for (SSize_t j = 0; j < m && !from_environment; j++) {
            SV **envp = av_fetch(g_inc_from_environment, j, FALSE);
            from_environment = envp && *envp == *svp;
        }
        if (!from_environment) av_push(kept, SvREFCNT_inc(*svp));
    }
    av_clear(inc);
    const SSize_t k = static_cast<SSize_t>(av_count(kept));
    for (SSize_t i = 0; i < k; i++) av_push(inc, SvREFCNT_inc(*av_fetch(kept, i, FALSE)));
    SvREFCNT_dec(kept);
}

static void inc_use_stdlib(pTHX_ const char *lib_dir) {
    /* What takes the place of the built-in directories. */
    std::vector<std::string> own;
    if (!g_site_lib_dir.empty()) {
        own.push_back(g_site_arch_dir);
        own.push_back(g_site_lib_dir);
    }
    if (lib_dir && *lib_dir) own.emplace_back(lib_dir);

    AV *inc = GvAVn(PL_incgv);
    const SSize_t n = static_cast<SSize_t>(av_count(inc));
    std::vector<bool> builtin(static_cast<size_t>(n), false);

    /* Each constant is a list in the form perl splits with PERLLIB_SEP; the
     * entries are matched last to first. */
    std::vector<std::string> expected;
    for (const char *list : BUILTIN_INC) {
        for (const char *p = list;;) {
            const char *end = strchr(p, PERLLIB_SEP);
            const size_t len = end ? static_cast<size_t>(end - p) : strlen(p);
            if (len) expected.emplace_back(p, len);
            if (!end) break;
            p = end + 1;
        }
    }
    SSize_t limit = n;        /* entries at or after this are already matched */
    SSize_t first_builtin = -1;
    for (size_t e = expected.size(); e-- > 0;) {
        for (SSize_t i = limit; i-- > 0;) {
            SV **svp = av_fetch(inc, i, FALSE);
            if (!svp || !SvPOK(*svp)) continue;
            STRLEN len = 0;
            const char *dir = SvPV(*svp, len);
            if (len == expected[e].size() && memcmp(dir, expected[e].data(), len) == 0) {
                builtin[static_cast<size_t>(i)] = true;
                limit = first_builtin = i;
                break;
            }
        }
    }

    /* Where the library goes: where the first built-in directory was; in a
     * list that had none, ahead of the "." perl puts last. */
    SSize_t at = first_builtin;
    if (at < 0) {
        at = n;
        if (n > 0) {
            SV **last = av_fetch(inc, n - 1, FALSE);
            if (last && SvPOK(*last) && SvCUR(*last) == 1 && *SvPVX_const(*last) == '.') at = n - 1;
        }
    }
    AV *fresh = newAV();
    g_inc_from_environment = newAV();
    for (SSize_t i = 0; i <= n; i++) {
        if (i == at)
            for (const std::string &dir : own) av_push(fresh, newSVpvn(dir.data(), dir.size()));
        if (i == n || builtin[static_cast<size_t>(i)]) continue;
        SV **svp = av_fetch(inc, i, FALSE);
        if (!svp) continue;
        /* everything perl_parse pushed besides the built-in directories
         * came from the environment */
        av_push(fresh, SvREFCNT_inc(*svp));
        av_push(g_inc_from_environment, SvREFCNT_inc(*svp));
    }
    av_clear(inc);
    const SSize_t m = static_cast<SSize_t>(av_count(fresh));
    for (SSize_t i = 0; i < m; i++) av_push(inc, SvREFCNT_inc(*av_fetch(fresh, i, FALSE)));
    SvREFCNT_dec(fresh);
}

/* ---- public API (pl.h) --------------------------------------------------- */
uint64_t perl_new(const char *lib_dir, const char *site_dir, const char *archname) {
    if (g_my_perl) return 0; /* one interpreter per instance */
    g_archname = archname && *archname ? archname : ARCHNAME;
    site_configure(site_dir);
    g_lib_dir = lib_dir ? lib_dir : "";

    static bool sys_inited = false;
    if (!sys_inited) {
        int sargc = 0;
        char **sargv = nullptr;
        char **senv = nullptr;
        PERL_SYS_INIT3(&sargc, &sargv, &senv);
        sys_inited = true;
    }

    g_my_perl = perl_alloc();
    if (!g_my_perl) return 0;
    PERL_SET_CONTEXT(g_my_perl);
    /* the locale of the environment, as the interpreter's own start-up
     * takes it (perl_libc.cc) */
    perl_wasm_locale_start_up(1);
    perl_construct(g_my_perl);
    perl_wasm_locale_start_up(0);
    PL_exit_flags |= PERL_EXIT_DESTRUCT_END;

    /* Bootstrap args: perl -e 0 — parse an empty program so the interpreter
     * is ready to eval. lib_dir is not a -I directory: those come first in
     * @INC, ahead of PERL5LIB, and the standard library belongs behind it
     * (inc_use_stdlib, below). */
    /* perl keeps the argument vector it is started with for good
     * (PL_origargv) and takes the strings for an area it may write to: an
     * assignment to $0 copies the new name over them, as far as they are
     * contiguous, and clears the other entries of the vector. So both are
     * storage of the interpreter's lifetime, laid out the way a system
     * lays out a process's arguments, and writable. */
    static const char arg_template[] = "perl\0-e\0000";
    static char arg_area[sizeof arg_template];
    static char *argv[4];
    memcpy(arg_area, arg_template, sizeof arg_template);
    int argc = 0;
    argv[argc++] = arg_area;
    argv[argc++] = arg_area + 5;
    argv[argc++] = arg_area + 8;
    argv[argc] = nullptr;

    /* PERL5OPT holds switches of the perl COMMAND LINE, and the command line
     * is the host's to interpret: it hands a program over with what the
     * switches mean, PERL5OPT's included (perl_run_program). This parse is
     * the interpreter's start-up, not a program's: were it to apply the
     * variable too, a -M module in it would be loaded here — before @INC
     * is in order, from the wrong directory — and again by the program.
     * So the parse does not see the variable; %ENV and the environment of
     * child processes keep it. */
    std::string perl5opt;
    const char *const perl5opt_env = getenv("PERL5OPT");
    const bool had_perl5opt = perl5opt_env != nullptr;
    if (had_perl5opt) {
        perl5opt = perl5opt_env;
        unsetenv("PERL5OPT");
    }
    /* before anything is compiled: an op is given its routine then */
    numbers_wrap_builtins();
    const int parse_failed =
        perl_parse(g_my_perl, xs_init, argc, argv, nullptr);
    if (had_perl5opt) setenv("PERL5OPT", perl5opt.c_str(), 1);
    if (parse_failed) {
        perl_destruct(g_my_perl);
        perl_free(g_my_perl);
        g_my_perl = nullptr;
        return 0;
    }
    {
        PERL_SET_CONTEXT(g_my_perl);
        dTHX;
        if (had_perl5opt) {
            /* %ENV was filled while the variable was hidden */
            SV *val = newSVpvn(perl5opt.data(), perl5opt.size());
            if (hv_store(GvHVn(PL_envgv), "PERL5OPT", 8, val, 0))
                SvSETMAGIC(val);
            else
                SvREFCNT_dec(val);
        }
        if ((lib_dir && *lib_dir) || !g_site_lib_dir.empty()) inc_use_stdlib(aTHX_ lib_dir);
    }
    perl_run(g_my_perl); /* runs the empty program; leaves the interp live */

    /* Install the interruptible run loop AFTER parse/run so the bootstrap
     * itself is never interrupted. */
    PL_runops = runops_interruptible;
    g_interrupt = 0;
    {
        PERL_SET_CONTEXT(g_my_perl);
        dTHX;
        wasi_signal_attach(aTHX);
    }

    /* Install the Go bridge: the named Perl->Go dispatcher (Bind's generated
     * subs call it) and the anonymous value-operation XSUBs. All C — no
     * Perl-source glue and no module loads. */
    {
        PERL_SET_CONTEXT(g_my_perl);
        dTHX;
        newXS("main::__plwasm_go_call", XS___plwasm_go_call, __FILE__);
        install_op_cvs(aTHX);
    }
    g_go_cb = 0;

    return 1; /* opaque handle */
}

std::string perl_eval(uint64_t h, const char *src) {
    if (!g_my_perl || h == 0) {
        std::string out = env_die("no interpreter");
        put_u32(out, 0); /* empty stdout */
        put_u32(out, 0); /* empty stderr */
        return out;
    }
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    wasi_signal_attach(aTHX);

    /* Feed the source in as an SV (no C-string escaping needed) and clear the
     * previous result/error slots. */
    sv_setpv(get_sv("main::__plwasm_src", GV_ADD), src ? src : "");
    sv_setsv(get_sv("main::__plwasm_result", GV_ADD), &PL_sv_undef);
    sv_setpvs(get_sv("main::__plwasm_errsv", GV_ADD), "");

    /* Run the wrapper without letting it croak (croak_on_error = FALSE); any
     * die from the user code is captured into __plwasm_errsv by the wrapper. */
    eval_pv(EVAL_WRAPPER, FALSE);

    SV *res_sv   = get_sv("main::__plwasm_result", 0);
    SV *out_sv   = get_sv("main::__plwasm_out", 0);
    SV *err_sv   = get_sv("main::__plwasm_err", 0);
    SV *errsv_sv = get_sv("main::__plwasm_errsv", 0);

    bool ok = !(errsv_sv && SvTRUE(errsv_sv));
    /* If the wrapper itself failed (should not happen), ERRSV carries it. */
    if (SvTRUE(ERRSV)) {
        ok = false;
        errsv_sv = ERRSV;
    }

    std::string out;
    if (ok) {
        put_u8(out, ENV_OK);
        encode_sv(aTHX_ res_sv, out); /* the result node; refs are pinned */
    } else {
        std::string msg = sv_to_std(aTHX_ errsv_sv);
        put_u8(out, ENV_DIE);
        put_bytes(out, msg.data(), static_cast<uint32_t>(msg.size()));
    }
    std::string out_s = sv_to_std(aTHX_ out_sv);
    std::string err_s = sv_to_std(aTHX_ err_sv);
    put_bytes(out, out_s.data(), static_cast<uint32_t>(out_s.size()));
    put_bytes(out, err_s.data(), static_cast<uint32_t>(err_s.size()));
    return out;
}

extern "C" int wasi_fork_child_run(pTHX_ int pid, int *status_out);

/* The end of the interpreter, as perl's main() performs it: perl_destruct
 * (the END blocks, the flush of PerlIO, global destruction) and perl_free.
 * Returns perl_destruct's result, the status a perl process exits with: a
 * destructor that runs during global destruction may still assign $?. A
 * destructor that calls exit there ends the process on the spot, as it does
 * in perl (there is no jump environment left to return to).
 *
 * The destruct level is perl's main()'s, 0: the objects are destroyed and
 * the exit-list functions run, and the rest of the memory goes with the
 * instance, which is never entered again, as a process's memory goes with
 * it. Freeing every value besides (a higher level, a leak-hunting setting)
 * frees values code is entitled to leave behind at exit: Object::Pad keeps
 * pointers to C structures of its own in arrays that live to the end, which
 * perl's main() never frees.
 *
 * PERL_SYS_TERM is intentionally NOT called: it is process teardown. */
static int destroy_interpreter() {
    PERL_SET_CONTEXT(g_my_perl);
    {
        dTHX;
        reg_clear(aTHX); /* drop registry pins so destruct sees true counts */
        SvREFCNT_dec(g_inc_from_environment);
        g_inc_from_environment = nullptr;
    }
    PL_perl_destruct_level = 0;
    const int status = perl_destruct(g_my_perl);
    perl_free(g_my_perl);
    g_my_perl = nullptr;
    return status;
}

void perl_close(uint64_t h) {
    if (!g_my_perl || h == 0) return;
    destroy_interpreter();
}

uint32_t perl_fork_child(uint64_t h, int32_t pid) {
    if (!g_my_perl || h == 0) return UINT32_MAX;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    int status = 0;
    if (wasi_fork_child_run(aTHX_ static_cast<int>(pid), &status) != 0)
        return UINT32_MAX;
    return static_cast<uint32_t>(destroy_interpreter()) & 0xff;
}

/* ---- whole-program execution (perl_program.cc) -------------------------- */
static void read_str(NodeReader &r, std::string &out) {
    uint32_t n = r.get_u32();
    const char *p = r.get_bytes(n);
    if (p) out.assign(p, n);
}

static void read_strlist(NodeReader &r, std::vector<std::string> &out) {
    uint32_t n = r.get_u32();
    for (uint32_t i = 0; i < n && !r.fail; i++) {
        out.emplace_back();
        read_str(r, out.back());
    }
}

static bool decode_program(const char *buf, uint32_t len, ProgramSpec &ps) {
    NodeReader r(buf, len);
    ps.flags = r.get_u32();
    ps.warnings = r.get_u8();
    ps.taint = r.get_u8();
    ps.unicode = r.get_u32();
    ps.source = r.get_u8();
    read_str(r, ps.path);
    read_str(r, ps.code);
    read_str(r, ps.extract_dir);
    read_str(r, ps.rs);
    read_str(r, ps.ors);
    read_str(r, ps.split);
    read_str(r, ps.inplace);
    read_strlist(r, ps.preamble);
    read_strlist(r, ps.inc);
    uint32_t ndef = r.get_u32();
    for (uint32_t i = 0; i < ndef && !r.fail; i++) {
        ps.defines.emplace_back();
        ProgramDefine &d = ps.defines.back();
        read_str(r, d.name);
        d.has_value = r.get_u8() != 0;
        read_str(r, d.value);
    }
    read_strlist(r, ps.args);
    ps.has_command_line = r.get_u8() != 0;
    read_strlist(r, ps.command_line);
    return !r.fail && r.p == r.end && ps.warnings <= PROG_WARN_NONE &&
           ps.taint <= PROG_TAINT_ON && ps.source <= PROG_SOURCE_STDIN;
}

uint32_t perl_run_program(uint64_t h, const char *program, uint32_t program_len) {
    if (!g_my_perl || h == 0) return UINT32_MAX;
    ProgramSpec ps{};
    if (!decode_program(program, program_len, ps)) return UINT32_MAX;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    wasi_signal_attach(aTHX);
    program_execute(aTHX_ ps);
    return static_cast<uint32_t>(destroy_interpreter()) & 0xff;
}

uint64_t perl_interrupt_addr(uint64_t h) {
    (void)h;
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&g_hostwords));
}

/* ---- addresses of the process: pack and unpack 'p' and 'P' -----------------
 * A pointer pack 'p' or 'P' makes is for native code (FFI::Platypus passes
 * the string buffer of scalar_to_buffer to a C function), which reads
 * memory where the process has it: the address of a string of this memory
 * is its address here plus the host's base of the memory. unpack 'p' and
 * 'P' read at such an address: in this memory when it falls there, and
 * otherwise in the memory of the process, which the host reads (a buffer a
 * C function returned). Without a base from the host the addresses are
 * this memory's own. pp_pack.c calls these (scripts/wasi-configure.sh). */
extern "C" const char *goperl_pack_address(const char *p) {
    const uint64_t base = g_hostwords.membase;
    if (!p || !base) return p;
    return reinterpret_cast<const char *>(static_cast<uintptr_t>(
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)) + base));
}

extern "C" SV *goperl_unpack_address(pTHX_ const char *p, SSize_t len) {
    const uint64_t base = g_hostwords.membase;
    const uint64_t a = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
    if (p && base) {
        const uint64_t size = static_cast<uint64_t>(__builtin_wasm_memory_size(0)) << 16;
        if (a >= base && a - base < size) {
            p = reinterpret_cast<const char *>(static_cast<uintptr_t>(a - base));
        } else {
            /* [address][length, or -1 for the string up to its NUL] ->
             * [1] and the bytes, or [0] and a message */
            if (!g_go_cb)
                Perl_croak(aTHX_ "goperl: no host to read the memory of the process at 0x%" UVxf,
                           (UV)a);
            goperl_gword_t req[2] = {a, static_cast<goperl_gword_t>(static_cast<int64_t>(len))};
            const int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_PROCMEM_METHOD_ID, req, sizeof req);
            if (rc == GOPERL_CALLBACK_JUMPED) return &PL_sv_undef;
            uint32_t resp_len;
            unsigned char *resp = goperl_callback_response(rc, &resp_len);
            if (resp == nullptr || resp_len < 1)
                Perl_croak(aTHX_ "goperl: reading the memory of the process: empty response");
            if (resp[0] != GOPERL_RESP_OK) {
                SV *const msg = newSVpvn_flags(reinterpret_cast<const char *>(resp + 1), resp_len - 1,
                                               SVs_TEMP);
                free(resp);
                Perl_croak(aTHX_ "%" SVf, SVfARG(msg));
            }
            SV *const sv = newSVpvn(reinterpret_cast<const char *>(resp + 1), resp_len - 1);
            free(resp);
            return sv;
        }
    }
    /* newSVpv and newSVpvn make undef of a null pointer */
    return len < 0 ? newSVpv(p, 0) : newSVpvn(p, static_cast<STRLEN>(len));
}

/* ---- protected operation driver ------------------------------------------
 * Every value operation runs the same way: resolve handles to their SVs,
 * push the arguments on the Perl stack, run the target under G_EVAL (a die
 * lands in ERRSV, not a longjmp past our locals), shape the returned list
 * into the operation's ok payload, all under a JMPENV that catches a guest
 * exit() the way perl_run itself does. Targets are a named sub (call_pv —
 * stub + AUTOLOAD semantics), the CODE reference behind a handle (call_sv),
 * a method dispatched by Perl's own resolution (call_method), or one of the
 * anonymous operation XSUBs above. */

/* How run_glue shapes the glue sub's return list into the ok payload. */
enum GlueShape {
    SHAPE_LIST,        /* node list of every returned value */
    SHAPE_NODE,        /* one node (undef node when the list is empty) */
    SHAPE_I64,         /* i64 of the first returned value */
    SHAPE_EMPTY,       /* nothing */
    SHAPE_EXISTS_NODE, /* u8 flag (first value) + node (second, or undef) */
};

/* What run_glue calls. */
enum GlueTarget {
    TARGET_NAMED,  /* call_pv(name): the handle (if any) is an argument */
    TARGET_CODE,   /* call_sv(handle's RV): the handle is the CALLEE */
    TARGET_METHOD, /* call_method(name): the handle is the invocant */
    TARGET_OP,     /* call_sv(g_op_cv[op]): the handle is the first argument */
};

/* GlueCall describes one operation. Lead arguments are DESCRIPTORS, not
 * SVs: run_glue_body materialises them inside its own SAVETMPS scope so
 * every mortal it creates is reclaimed by its own FREETMPS (an SV made
 * outside that scope would sit on the tmps stack forever — the bridge entry
 * points have no surrounding FREETMPS). Push order: handle, integer, key
 * node, then the trailing node list. */
struct GlueCall {
    GlueTarget target;
    const char *name;      /* TARGET_NAMED: sub name; TARGET_METHOD: method */
    int op;                /* TARGET_OP: g_op_cv index */
    I32 gimme;             /* G_LIST or G_SCALAR for the target call */
    uint64_t harg;         /* registry handle (role depends on target) */
    bool has_harg;
    bool has_iarg;         /* integer argument (array index) */
    int64_t iarg;
    const char *karg;      /* one key node (may be NULL) */
    uint32_t karg_len;
    const char *nodes;     /* trailing node-list buffer ([u32 count] nodes) */
    uint32_t nodes_len;    /* 0 = no trailing arguments */
    GlueShape shape;
};

/* run_glue_body: the half that runs under the JMPENV (its C++ locals leak
 * only on the exit longjmp, which tears the call down anyway). */
static std::string run_glue_body(pTHX_ const GlueCall &gc) {
    SV *hsv = nullptr;
    if (gc.has_harg) {
        hsv = reg_lookup(gc.harg);
        if (!hsv) {
            char buf[64];
            snprintf(buf, sizeof(buf), "stale Perl reference handle %llu",
                     static_cast<unsigned long long>(gc.harg));
            return env_die(buf);
        }
    }
    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);
    /* TARGET_CODE's handle is the callee, not an argument. */
    if (hsv && gc.target != TARGET_CODE) XPUSHs(hsv);
    if (gc.has_iarg) mXPUSHs(newSViv(static_cast<IV>(gc.iarg)));
    if (gc.karg) {
        NodeReader kr(gc.karg, gc.karg_len);
        std::string kerr;
        SV *ksv = decode_node(aTHX_ kr, kerr);
        if (!ksv) {
            PUTBACK;
            FREETMPS;
            LEAVE;
            return env_die(kerr.empty() ? "malformed key node" : kerr);
        }
        XPUSHs(ksv);
    }
    if (gc.nodes_len) {
        NodeReader r(gc.nodes, gc.nodes_len);
        uint32_t count = r.get_u32();
        std::string derr;
        for (uint32_t i = 0; i < count; i++) {
            if (r.fail) break;
            /* NODE_FLATTEN expands to the aggregate's contents — Perl's own
             * list-flattening calling convention for @array / %hash args. */
            if (r.p < r.end && *r.p == NODE_FLATTEN) {
                r.get_u8();
                uint64_t id = r.get_u64();
                SV *rv = r.fail ? nullptr : reg_lookup(id);
                if (!rv) { r.fail = true; derr = "stale Perl reference handle"; break; }
                SV *ref = SvRV(rv);
                if (SvTYPE(ref) == SVt_PVAV) {
                    AV *av = (AV *)ref;
                    SSize_t n = av_top_index(av) + 1;
                    for (SSize_t k = 0; k < n; k++) {
                        SV **elt = av_fetch(av, k, 0);
                        XPUSHs(elt ? *elt : &PL_sv_undef);
                    }
                } else if (SvTYPE(ref) == SVt_PVHV) {
                    HV *hv = (HV *)ref;
                    hv_iterinit(hv);
                    HE *he;
                    while ((he = hv_iternext(hv)) != nullptr) {
                        XPUSHs(hv_iterkeysv(he));
                        XPUSHs(HeVAL(he));
                    }
                } else {
                    r.fail = true;
                    derr = "flatten argument is not an ARRAY or HASH reference";
                    break;
                }
                continue;
            }
            SV *sv = decode_node(aTHX_ r, derr);
            if (!sv) { r.fail = true; break; }
            XPUSHs(sv);
        }
        if (r.fail) {
            PUTBACK;
            FREETMPS;
            LEAVE;
            return env_die(derr.empty() ? "malformed argument buffer" : derr);
        }
    }
    PUTBACK;

    int count;
    switch (gc.target) {
    case TARGET_NAMED:
        count = call_pv(gc.name, gc.gimme | G_EVAL);
        break;
    case TARGET_CODE:
        count = call_sv(hsv, gc.gimme | G_EVAL);
        break;
    case TARGET_METHOD:
        count = call_method(gc.name, gc.gimme | G_EVAL);
        break;
    default: /* TARGET_OP: a sub of the bridge's, which the debugger's
              * DB::sub is not entered for */
        count = call_sv((SV *)g_op_cv[gc.op], gc.gimme | G_EVAL | G_NODEBUG);
        break;
    }
    SPAGAIN;

    std::string out;
    if (SvTRUE(ERRSV)) {
        out = env_die(sv_to_std(aTHX_ ERRSV));
        SP -= count; /* discard whatever the failed call left */
    } else {
        SV **items = SP - count + 1;
        put_u8(out, ENV_OK);
        switch (gc.shape) {
        case SHAPE_LIST:
            encode_list(aTHX_ items, count, out);
            break;
        case SHAPE_NODE:
            if (count > 0) encode_sv(aTHX_ items[0], out);
            else put_u8(out, NODE_UNDEF);
            break;
        case SHAPE_I64: {
            int64_t v = count > 0 ? static_cast<int64_t>(SvIV(items[0])) : 0;
            put_i64(out, v);
            break;
        }
        case SHAPE_EMPTY:
            break;
        case SHAPE_EXISTS_NODE: {
            bool exists = count > 0 && SvTRUE(items[0]);
            put_u8(out, exists ? 1 : 0);
            if (exists && count > 1) encode_sv(aTHX_ items[1], out);
            else put_u8(out, NODE_UNDEF);
            break;
        }
        }
        SP -= count;
    }
    PUTBACK;
    FREETMPS;
    LEAVE;
    return out;
}

static std::string run_glue(pTHX_ const GlueCall &gc) {
    wasi_signal_attach(aTHX);
    /* Catch a guest exit(): my_exit unwinds with JMPENV_JUMP(2), and without
     * a live JMPENV here it would fall through to the C exit() — a wasi
     * proc_exit that aborts the wasm mid-call, before PerlIO ever flushes.
     * Mirroring perl_run's own catch (perl.c), the exit unwinds cleanly back
     * to this frame, the interpreter stays destructible (flush + END blocks
     * run at perl_close), and the status is reported in the exit envelope
     * for the host to turn into a process exit. */
    dJMPENV;
    int jmp;
    I32 oldscope = PL_scopestack_ix;
    std::string out;
    JMPENV_PUSH(jmp);
    switch (jmp) {
    case 0:
        out = run_glue_body(aTHX_ gc);
        break;
    case 2: { /* my_exit() */
        while (PL_scopestack_ix > oldscope)
            LEAVE;
        FREETMPS;
        /* perl.c's SET_CURSTASH is file-local; inline its body. */
        if (PL_curstash != PL_defstash) {
            SvREFCNT_dec(PL_curstash);
            PL_curstash = (HV *)SvREFCNT_inc(PL_defstash);
        }
        out.clear();
        put_u8(out, ENV_EXIT);
        put_u32(out, static_cast<uint32_t>(static_cast<int32_t>(STATUS_EXIT)));
        break;
    }
    default:
        out = env_die("unexpected longjmp");
        break;
    }
    JMPENV_POP;
    return out;
}

/* ready() guards every entry point below. */
static bool ready(uint64_t h, std::string *err_out) {
    if (g_my_perl && h != 0) return true;
    *err_out = env_die("no interpreter");
    return false;
}

std::string perl_call(uint64_t h, const char *sub_name,
                      const char *args, uint32_t args_len) {
    std::string err;
    if (!ready(h, &err)) return err;
    if (!sub_name || !*sub_name) return env_die("empty sub name");
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    GlueCall gc{};
    gc.target = TARGET_NAMED;
    gc.name = sub_name;
    gc.gimme = G_LIST;
    gc.nodes = args;
    gc.nodes_len = args_len;
    gc.shape = SHAPE_LIST;
    return run_glue(aTHX_ gc);
}

std::string perl_invoke(uint64_t h, uint64_t code, uint32_t want_scalar,
                        const char *args, uint32_t args_len) {
    std::string err;
    if (!ready(h, &err)) return err;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    GlueCall gc{};
    gc.target = TARGET_CODE;
    gc.gimme = want_scalar ? G_SCALAR : G_LIST;
    gc.harg = code;
    gc.has_harg = true;
    gc.nodes = args;
    gc.nodes_len = args_len;
    gc.shape = SHAPE_LIST;
    return run_glue(aTHX_ gc);
}

std::string perl_method_call(uint64_t h, uint64_t obj, const char *method,
                             const char *args, uint32_t args_len) {
    std::string err;
    if (!ready(h, &err)) return err;
    if (!method || !*method) return env_die("empty method name");
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    GlueCall gc{};
    gc.target = TARGET_METHOD;
    gc.name = method;
    gc.gimme = G_LIST;
    gc.harg = obj;
    gc.has_harg = true;
    gc.nodes = args;
    gc.nodes_len = args_len;
    gc.shape = SHAPE_LIST;
    return run_glue(aTHX_ gc);
}

std::string perl_deref(uint64_t h, uint64_t ref) {
    std::string err;
    if (!ready(h, &err)) return err;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    GlueCall gc{};
    gc.target = TARGET_OP;
    gc.op = GOP_DEREF;
    gc.gimme = G_SCALAR;
    gc.harg = ref;
    gc.has_harg = true;
    gc.shape = SHAPE_NODE;
    return run_glue(aTHX_ gc);
}

std::string perl_array_len(uint64_t h, uint64_t av) {
    std::string err;
    if (!ready(h, &err)) return err;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    GlueCall gc{};
    gc.target = TARGET_OP;
    gc.op = GOP_ALEN;
    gc.gimme = G_SCALAR;
    gc.harg = av;
    gc.has_harg = true;
    gc.shape = SHAPE_I64;
    return run_glue(aTHX_ gc);
}

std::string perl_array_get(uint64_t h, uint64_t av, int64_t idx) {
    std::string err;
    if (!ready(h, &err)) return err;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    GlueCall gc{};
    gc.target = TARGET_OP;
    gc.op = GOP_AGET;
    gc.gimme = G_SCALAR;
    gc.harg = av;
    gc.has_harg = true;
    gc.has_iarg = true;
    gc.iarg = idx;
    gc.shape = SHAPE_NODE;
    return run_glue(aTHX_ gc);
}

std::string perl_array_set(uint64_t h, uint64_t av, int64_t idx,
                           const char *val, uint32_t val_len) {
    std::string err;
    if (!ready(h, &err)) return err;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    /* Wrap the single value node as a one-element list for the driver. */
    std::string one;
    put_u32(one, 1);
    one.append(val ? val : "", val_len);
    GlueCall gc{};
    gc.target = TARGET_OP;
    gc.op = GOP_ASET;
    gc.gimme = G_LIST;
    gc.harg = av;
    gc.has_harg = true;
    gc.has_iarg = true;
    gc.iarg = idx;
    gc.nodes = one.data();
    gc.nodes_len = static_cast<uint32_t>(one.size());
    gc.shape = SHAPE_EMPTY;
    return run_glue(aTHX_ gc);
}

std::string perl_array_push(uint64_t h, uint64_t av,
                            const char *vals, uint32_t vals_len) {
    std::string err;
    if (!ready(h, &err)) return err;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    GlueCall gc{};
    gc.target = TARGET_OP;
    gc.op = GOP_APUSH;
    gc.gimme = G_LIST;
    gc.harg = av;
    gc.has_harg = true;
    gc.nodes = vals;
    gc.nodes_len = vals_len;
    gc.shape = SHAPE_EMPTY;
    return run_glue(aTHX_ gc);
}

std::string perl_array_values(uint64_t h, uint64_t av) {
    std::string err;
    if (!ready(h, &err)) return err;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    GlueCall gc{};
    gc.target = TARGET_OP;
    gc.op = GOP_AVALS;
    gc.gimme = G_LIST;
    gc.harg = av;
    gc.has_harg = true;
    gc.shape = SHAPE_LIST;
    return run_glue(aTHX_ gc);
}

std::string perl_hash_get(uint64_t h, uint64_t hv,
                          const char *key, uint32_t key_len) {
    std::string err;
    if (!ready(h, &err)) return err;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    GlueCall gc{};
    gc.target = TARGET_OP;
    gc.op = GOP_HGET;
    gc.gimme = G_LIST;
    gc.harg = hv;
    gc.has_harg = true;
    gc.karg = key;
    gc.karg_len = key_len;
    gc.shape = SHAPE_EXISTS_NODE;
    return run_glue(aTHX_ gc);
}

std::string perl_hash_set(uint64_t h, uint64_t hv,
                          const char *key, uint32_t key_len,
                          const char *val, uint32_t val_len) {
    std::string err;
    if (!ready(h, &err)) return err;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    std::string one;
    put_u32(one, 1);
    one.append(val ? val : "", val_len);
    GlueCall gc{};
    gc.target = TARGET_OP;
    gc.op = GOP_HSET;
    gc.gimme = G_LIST;
    gc.harg = hv;
    gc.has_harg = true;
    gc.karg = key;
    gc.karg_len = key_len;
    gc.nodes = one.data();
    gc.nodes_len = static_cast<uint32_t>(one.size());
    gc.shape = SHAPE_EMPTY;
    return run_glue(aTHX_ gc);
}

std::string perl_hash_delete(uint64_t h, uint64_t hv,
                             const char *key, uint32_t key_len) {
    std::string err;
    if (!ready(h, &err)) return err;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    GlueCall gc{};
    gc.target = TARGET_OP;
    gc.op = GOP_HDEL;
    gc.gimme = G_LIST;
    gc.harg = hv;
    gc.has_harg = true;
    gc.karg = key;
    gc.karg_len = key_len;
    gc.shape = SHAPE_EMPTY;
    return run_glue(aTHX_ gc);
}

std::string perl_hash_keys(uint64_t h, uint64_t hv) {
    std::string err;
    if (!ready(h, &err)) return err;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    GlueCall gc{};
    gc.target = TARGET_OP;
    gc.op = GOP_HKEYS;
    gc.gimme = G_LIST;
    gc.harg = hv;
    gc.has_harg = true;
    gc.shape = SHAPE_LIST;
    return run_glue(aTHX_ gc);
}

std::string perl_new_array(uint64_t h, const char *vals, uint32_t vals_len) {
    std::string err;
    if (!ready(h, &err)) return err;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    GlueCall gc{};
    gc.target = TARGET_OP;
    gc.op = GOP_NEWA;
    gc.gimme = G_SCALAR;
    gc.nodes = vals;
    gc.nodes_len = vals_len;
    gc.shape = SHAPE_NODE;
    return run_glue(aTHX_ gc);
}

std::string perl_new_hash(uint64_t h, const char *pairs, uint32_t pairs_len) {
    std::string err;
    if (!ready(h, &err)) return err;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    GlueCall gc{};
    gc.target = TARGET_OP;
    gc.op = GOP_NEWH;
    gc.gimme = G_SCALAR;
    gc.nodes = pairs;
    gc.nodes_len = pairs_len;
    gc.shape = SHAPE_NODE;
    return run_glue(aTHX_ gc);
}

std::string perl_release(uint64_t h, const char *ids, uint32_t ids_len) {
    std::string err;
    if (!ready(h, &err)) return err;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    /* Dropping the last pin can run DESTROY; die semantics there follow
     * plain Perl (fatal outside eval), like refcount drops anywhere else. */
    NodeReader r(ids, ids_len);
    while (r.p + 8 <= r.end) reg_release(aTHX_ r.get_u64());
    std::string out;
    put_u8(out, ENV_OK);
    return out;
}

void perl_set_go_dispatcher(uint64_t h, int32_t callback_id) {
    if (h == 0) return;
    g_go_cb = callback_id;
}

void perl_register_native_xs(uint64_t h, const char *name, int32_t fn_id) {
    if (!g_my_perl || h == 0 || !name || !*name) return;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    CV *cv = newXS(name, XS_goperl_native_thunk, __FILE__);
    CvXSUBANY(cv).any_i32 = fn_id;
}

/* Anchor magic for host-side MAGIC mirrors (perl_xs_helper ops
 * MAGIC_ATTACH/MAGIC_ID/MAGIC_UNATTACH). The
 * real MAGIC chain a native module builds (vtbl identity, mg_ptr payloads,
 * mg_obj) lives entirely on the host; the guest SV carries one PERL_MAGIC_ext
 * entry whose mg_ptr holds the host id, so lifetime is aligned: freeing the
 * SV fires svt_free here, which forwards the id to the host (callback method
 * -2) to run the module's own svt_free chain and drop the mirror. */
static int goperl_host_mg_free(pTHX_ SV *sv, MAGIC *mg) {
    PERL_UNUSED_ARG(sv);
    if (g_go_cb != 0) {
        goperl_gword_t id = GOPERL_ADDR(mg->mg_ptr);
        int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_MG_FREE_METHOD_ID,
                                             &id, sizeof(id));
        if (rc != GOPERL_CALLBACK_JUMPED) {
            uint32_t resp_len;
            unsigned char *resp = goperl_callback_response(rc, &resp_len);
            /* a svt_free of native code died ([0]+message): the free dies
             * with it, as perl's mg_free lets a die out. The MAGIC is not
             * touched: the hook may have had the value freed once more
             * before it died (Variable::Magic's free does, for the rest of
             * the chain), and that inner free has freed this MAGIC too. */
            if (resp != nullptr && resp_len >= 1 && resp[0] == GOPERL_RESP_FAIL) {
                SV *msg = newSVpvn_flags((const char *)resp + 1, resp_len - 1, SVs_TEMP);
                free(resp);
                croak_sv(msg);
            }
            free(resp);
        }
    }
    mg->mg_ptr = NULL; /* not a real pointer; never let core touch it */
    return 0;
}

/* `local` gives the new value of a variable a copy of the variable's
 * container magic (mg_localize). A copy of the anchor would give two SVs
 * the same host id: writes to the new value would be reported for the old
 * one, and the end of the scope, freeing the new value, would tear down
 * the magic of the variable itself. So the anchor is not copied (it has
 * svt_local, which mg_localize calls instead); the host is told, and gives
 * the new SV what mg_localize would give it for each MAGIC of its mirror,
 * under an anchor of its own. When the host cannot (the magic is native
 * code's, which a fork child cannot run), `local` dies with its message. */
static int goperl_host_mg_local(pTHX_ SV *nsv, MAGIC *mg) {
    if (g_go_cb != 0) {
        goperl_gword_t req[2] = {GOPERL_ADDR(mg->mg_ptr), GOPERL_ADDR(nsv)};
        int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_MG_LOCAL_METHOD_ID,
                                             req, sizeof(req));
        if (rc == GOPERL_CALLBACK_JUMPED) return 0;
        uint32_t resp_len;
        unsigned char *resp = goperl_callback_response(rc, &resp_len);
        if (resp == nullptr) return 0;
        goperl_die_if_thrown(aTHX_ resp);
        if (resp_len >= 1 && resp[0] == GOPERL_RESP_FAIL) {
            SV *msg = newSVpvn_flags((const char *)resp + 1, resp_len - 1, SVs_TEMP);
            free(resp);
            croak_sv(msg);
        }
        free(resp);
    }
    return 0;
}

static unsigned char *goperl_host_call(pTHX_ goperl_gword_t kind, const goperl_gword_t *words,
                                       uint32_t nwords, uint32_t *rlen);

/* The other hooks of a module's vtable: the host runs the hook of every
 * MAGIC of the SV's mirror chain that has it, and answers with what the
 * last one returned. */
static goperl_gword_t goperl_host_mg_hook(pTHX_ goperl_gword_t hook, SV *sv, SV *nsv,
                                          const char *name, I32 namlen) {
    if (g_go_cb == 0) return 0;
    const goperl_gword_t words[5] = {hook, GOPERL_ADDR(sv), GOPERL_ADDR(nsv), GOPERL_ADDR(name),
                                     (goperl_gword_t)(int64_t)namlen};
    uint32_t rlen = 0;
    unsigned char *resp = goperl_host_call(aTHX_ GOPERL_HOST_MAGIC, words, 5, &rlen);
    const goperl_gword_t result = goperl_resp_word(resp, rlen, 0);
    free(resp);
    return result;
}
static int goperl_host_mg_get(pTHX_ SV *sv, MAGIC *mg) {
    PERL_UNUSED_ARG(mg);
    return (int)goperl_host_mg_hook(aTHX_ GOPERL_MGHOOK_GET, sv, NULL, NULL, 0);
}
/* Set magic of the anchor: the svt_set hooks of the SV's mirror chain run
 * (Moose's re-export flag magic clears itself when the glob is
 * overwritten); a hook that croaks dies here, as in perl's mg_set. */
static int goperl_host_mg_set(pTHX_ SV *sv, MAGIC *mg) {
    PERL_UNUSED_ARG(mg);
    return (int)goperl_host_mg_hook(aTHX_ GOPERL_MGHOOK_SET, sv, NULL, NULL, 0);
}
static U32 goperl_host_mg_len(pTHX_ SV *sv, MAGIC *mg) {
    PERL_UNUSED_ARG(mg);
    return (U32)goperl_host_mg_hook(aTHX_ GOPERL_MGHOOK_LEN, sv, NULL, NULL, 0);
}
static int goperl_host_mg_clear(pTHX_ SV *sv, MAGIC *mg) {
    PERL_UNUSED_ARG(mg);
    return (int)goperl_host_mg_hook(aTHX_ GOPERL_MGHOOK_CLEAR, sv, NULL, NULL, 0);
}
static int goperl_host_mg_copy(pTHX_ SV *sv, MAGIC *mg, SV *nsv, const char *name,
                               I32 namlen) {
    PERL_UNUSED_ARG(mg);
    return (int)goperl_host_mg_hook(aTHX_ GOPERL_MGHOOK_COPY, sv, nsv, name, namlen);
}

/* The vtable of an anchor has the hooks the mirror chain of its SV needs
 * and no others: a hook the interpreter finds in a vtable costs a round
 * trip to the host every time the SV is read, written, measured or
 * cleared. An anchor starts with none of the five below (bit 0 get, 1 set,
 * 2 len, 3 clear, 4 copy) and is moved to the vtable with more of them
 * when an entry of the chain has such a hook (SV_MAGIC_SET_HOOK). */
#define GOPERL_ANCHOR_VTBLS 32
static MGVTBL goperl_anchor_vtbls[GOPERL_ANCHOR_VTBLS];
static const MGVTBL *goperl_anchor_vtbl(unsigned mask) {
    static bool made = false;
    if (!made) {
        made = true;
        for (unsigned m = 0; m < GOPERL_ANCHOR_VTBLS; m++) {
            MGVTBL &v = goperl_anchor_vtbls[m];
            v.svt_get = (m & 1) ? goperl_host_mg_get : NULL;
            v.svt_set = (m & 2) ? goperl_host_mg_set : NULL;
            v.svt_len = (m & 4) ? goperl_host_mg_len : NULL;
            v.svt_clear = (m & 8) ? goperl_host_mg_clear : NULL;
            v.svt_free = goperl_host_mg_free;
            v.svt_copy = (m & 16) ? goperl_host_mg_copy : NULL;
            v.svt_dup = NULL;
            v.svt_local = goperl_host_mg_local;
        }
    }
    return &goperl_anchor_vtbls[mask & (GOPERL_ANCHOR_VTBLS - 1)];
}
static bool goperl_is_anchor(const MAGIC *mg) {
    return mg->mg_virtual >= &goperl_anchor_vtbls[0] &&
           mg->mg_virtual < &goperl_anchor_vtbls[GOPERL_ANCHOR_VTBLS];
}
static MAGIC *goperl_anchor_of(pTHX_ SV *sv) {
    if (SvTYPE(sv) < SVt_PVMG) return NULL;
    for (MAGIC *mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic)
        if (goperl_is_anchor(mg)) return mg;
    return NULL;
}

/* The calls of uvar magic of native code's that are running: perl's hash
 * access holds the uvar MAGIC across the call and writes to it after it
 * (hv_magic_uvar_xkey), so the MAGIC must outlive the call whatever native
 * code does in it (see goperl_uvar_drop). The scope of the call puts the
 * count back, also when the call dies. */
static I32 g_uvar_calls = 0;

/* uvar magic of native code (sv_magic(sv, NULL, PERL_MAGIC_uvar, &uf, ...)):
 * the functions of the ufuncs this interpreter is given are these, and the
 * module's own are the host's to call. A hash calls uf_val with the kind
 * of access and has the key in the MAGIC's mg_obj, where the function may
 * put another; a scalar calls uf_val and uf_set with the index of the
 * ufuncs, which the host knows. */
static I32 goperl_uvar_call(pTHX_ goperl_gword_t is_set, IV arg, SV *sv) {
    if (g_go_cb == 0) return 0;
    ENTER;
    SAVEI32(g_uvar_calls);
    g_uvar_calls++;
    MAGIC *mg = mg_find(sv, PERL_MAGIC_uvar);
    const goperl_gword_t words[4] = {is_set, (goperl_gword_t)(int64_t)arg, GOPERL_ADDR(sv),
                                     GOPERL_ADDR(mg ? mg->mg_obj : NULL)};
    uint32_t rlen = 0;
    unsigned char *resp = goperl_host_call(aTHX_ GOPERL_HOST_UVAR, words, 4, &rlen);
    goperl_gword_t key = words[3], result = 0;
    if (rlen >= 1 + 16) {
        key = goperl_resp_word(resp, rlen, 0);
        result = goperl_resp_word(resp, rlen, 1);
    }
    free(resp);
    /* the MAGIC may be another one by now */
    mg = mg_find(sv, PERL_MAGIC_uvar);
    if (mg) mg->mg_obj = (SV *)(uintptr_t)key;
    LEAVE;
    return (I32)(int64_t)result;
}
static I32 goperl_uvar_val(pTHX_ IV arg, SV *sv) { return goperl_uvar_call(aTHX_ 0, arg, sv); }
static I32 goperl_uvar_set(pTHX_ IV arg, SV *sv) { return goperl_uvar_call(aTHX_ 1, arg, sv); }

/* The uvar magic of the value that stands for native code's (made by
 * GOPERL_CALL_UVAR: its functions are the two above) goes once native code
 * has taken its own off by relinking its chain (Variable::Magic's dispell
 * unlinks its uvar MAGIC and calls mg_magical): on perl the value has no
 * uvar magic left then. Freed as mg_free frees a uvar MAGIC that has the
 * copy of its ufuncs that sv_magic made. Not while a call of uvar magic
 * runs, which holds the MAGIC (g_uvar_calls): native code that dispells
 * from its own uvar callback (Variable::Magic t/15-self.t) leaves the
 * MAGIC, which calls nothing of native code's any more, to a later
 * mg_magical. */
static void goperl_uvar_drop(pTHX_ SV *sv) {
    if (g_uvar_calls || SvTYPE(sv) < SVt_PVMG) return;
    for (MAGIC *prev = NULL, *mg = SvMAGIC(sv); mg; prev = mg, mg = mg->mg_moremagic) {
        if (mg->mg_type != PERL_MAGIC_uvar || !mg->mg_ptr) continue;
        const struct ufuncs *uf = (const struct ufuncs *)mg->mg_ptr;
        if (uf->uf_val != goperl_uvar_val && uf->uf_set != goperl_uvar_set) continue;
        if (prev)
            prev->mg_moremagic = mg->mg_moremagic;
        else
            SvMAGIC_set(sv, mg->mg_moremagic);
        if (mg->mg_len > 0) Safefree(mg->mg_ptr);
        Safefree(mg);
        return;
    }
}

/* ---- host-backed PerlIO layers (v7) --------------------------------------
 * A native module defines a layer with custom Pushed/Fill (the
 * PerlIO::utf8_strict shape); the guest registers a PerlIOBuf-derived
 * proxy whose stock slots are the real :perlio behaviors and whose custom
 * slots round-trip to the host over GOPERL_PERLIO_METHOD_ID. The buffer
 * lives in the GUEST (fast_gets stays native); one host call per refill. */

#define GOPERLIO_HOOK_PUSHED 1
#define GOPERLIO_HOOK_FILL 2
#define GOPERLIO_HOOK_POPPED 3

#define GOPERLIO_MASK_PUSHED 0x1
#define GOPERLIO_MASK_FILL 0x2

typedef struct {
    PerlIOBuf buf;
    U32 funcs_id;
    U32 mask;
} GoperlIOL;

#define GOPERLIO_MAX_LAYERS 8
typedef struct {
    PerlIO_funcs funcs;
    char name[32];
    U32 funcs_id;
    U32 mask;
} goperlio_class_t;
static goperlio_class_t goperlio_classes[GOPERLIO_MAX_LAYERS];
static int goperlio_nclasses = 0;

static goperlio_class_t *goperlio_class_of(PerlIO_funcs *tab) {
    for (int i = 0; i < goperlio_nclasses; i++)
        if (&goperlio_classes[i].funcs == tab) return &goperlio_classes[i];
    return NULL;
}

/* Send one hook to the host. head is the fixed prefix; extra/extralen is
 * appended. Returns the malloc'd response (caller frees) or croaks. */
static unsigned char *goperlio_call(pTHX_ U32 hook, GoperlIOL *l, PerlIO *f,
                                    const void *extra, size_t extralen,
                                    uint32_t *resplen) {
    if (g_go_cb == 0)
        Perl_croak(aTHX_ "PerlIO layer hook: no Go dispatcher");
    goperl_gword_t head[4];
    head[0] = hook;
    head[1] = l->funcs_id;
    head[2] = GOPERL_ADDR(f);
    head[3] = GOPERL_ADDR(l);
    size_t n = sizeof head + extralen;
    unsigned char *buf = (unsigned char *)malloc(n);
    if (!buf) Perl_croak(aTHX_ "PerlIO layer hook: out of memory");
    memcpy(buf, head, sizeof head);
    if (extralen) memcpy(buf + sizeof head, extra, extralen);
    int64_t rc =
        wasmify_callback_invoke(g_go_cb, GOPERL_PERLIO_METHOD_ID, buf, n);
    free(buf);
    if (rc == GOPERL_CALLBACK_JUMPED) {
        *resplen = 0;
        return NULL;
    }
    uint32_t rlen;
    unsigned char *resp = goperl_callback_response(rc, &rlen);
    if (resp == nullptr || rlen < 1)
        Perl_croak(aTHX_ "PerlIO layer hook: empty response");
    if (resp[0] == 0) {
        char msg[512];
        size_t ml = rlen - 1;
        if (ml > sizeof(msg) - 1) ml = sizeof(msg) - 1;
        memcpy(msg, resp + 1, ml);
        msg[ml] = '\0';
        free(resp);
        Perl_croak(aTHX_ "%s", msg);
    }
    *resplen = rlen;
    return resp;
}

static IV goperlio_pushed(pTHX_ PerlIO *f, const char *mode, SV *arg,
                          PerlIO_funcs *tab) {
    goperlio_class_t *cls = goperlio_class_of(tab);
    IV code = PerlIOBuf_pushed(aTHX_ f, mode, arg, tab);
    if (!cls) return code;
    GoperlIOL *l = PerlIOSelf(f, GoperlIOL);
    l->funcs_id = cls->funcs_id;
    l->mask = cls->mask;
    if (code != 0 || !(cls->mask & GOPERLIO_MASK_PUSHED)) return code;
    /* extra: [the arg][the bytes of the mode, NUL] */
    char extra[64];
    const goperl_gword_t argtok = GOPERL_ADDR(arg);
    memcpy(extra, &argtok, 8);
    size_t ml = mode ? strlen(mode) : 0;
    if (ml > 55) ml = 55;
    memcpy(extra + 8, mode ? mode : "", ml);
    extra[8 + ml] = '\0';
    uint32_t rlen = 0;
    unsigned char *resp =
        goperlio_call(aTHX_ GOPERLIO_HOOK_PUSHED, l, f, extra, 9 + ml, &rlen);
    /* response: [1][flags to set][flags to clear] */
    if (rlen >= 1 + 16) {
        const U32 setf = (U32)goperl_resp_word(resp, rlen, 0);
        const U32 clrf = (U32)goperl_resp_word(resp, rlen, 1);
        PerlIOBase(f)->flags = (PerlIOBase(f)->flags & ~clrf) | setf;
    }
    free(resp);
    return 0;
}

static IV goperlio_fill(pTHX_ PerlIO *f) {
    GoperlIOL *l = PerlIOSelf(f, GoperlIOL);
    PerlIOBuf *b = &l->buf;
    if (PerlIO_flush(f) != 0) return -1;
    if (!b->buf) PerlIO_get_base(f);
    b->ptr = b->end = b->buf;
    /* extra: [buf][bufsiz][flags] */
    goperl_gword_t extra[3];
    extra[0] = GOPERL_ADDR(b->buf);
    extra[1] = (goperl_gword_t)b->bufsiz;
    extra[2] = PerlIOBase(f)->flags;
    uint32_t rlen = 0;
    unsigned char *resp = goperlio_call(aTHX_ GOPERLIO_HOOK_FILL, l, f, extra,
                                        sizeof extra, &rlen);
    /* response: [1][status][ptrOff][endOff][setflags] */
    IV ret = -1;
    if (rlen >= 1 + 32) {
        const goperl_gword_t status = goperl_resp_word(resp, rlen, 0);
        const goperl_gword_t ptrOff = goperl_resp_word(resp, rlen, 1);
        const goperl_gword_t endOff = goperl_resp_word(resp, rlen, 2);
        const U32 setf = (U32)goperl_resp_word(resp, rlen, 3);
        b->ptr = b->buf + ptrOff;
        b->end = b->buf + endOff;
        PerlIOBase(f)->flags |= setf;
        ret = status == 0 ? 0 : -1;
    }
    free(resp);
    return ret;
}

static IV goperlio_popped(pTHX_ PerlIO *f) {
    GoperlIOL *l = PerlIOSelf(f, GoperlIOL);
    if (l->funcs_id) {
        uint32_t rlen = 0;
        unsigned char *resp =
            goperlio_call(aTHX_ GOPERLIO_HOOK_POPPED, l, f, "", 0, &rlen);
        free(resp);
    }
    return PerlIOBuf_popped(aTHX_ f);
}

/* ---- perl_xs_helper protocol constants ----------------------------------
 * KEEP IN SYNC with go-perl's xsnative/sdk/include/perl.h, which mirrors
 * every value here (the GOPERL_OP_* enum, the GOPERL_PL_* ids, and the
 * SV_INFO bitset). The numbers are the wire protocol between the host SDK
 * and this guest; renumbering is an ABI break. */

/* Guest op numbers (the `op` argument of perl_xs_helper). Grouped by the
 * SDK generation that introduced them: 1-22 the initial scalar/agg surface,
 * 23-97 the v3 Text::Xslate-class surface, 98-127 the v4 interpreter-hook
 * (Devel::NYTProf-class) surface. 52 is retired and stays unused. */
enum goperl_xs_op {
    GOPERL_OP_SV_IV = 1,
    GOPERL_OP_SV_PV = 2,
    GOPERL_OP_NEW_IV = 3,
    GOPERL_OP_NEW_PVN = 4,
    GOPERL_OP_SV_MORTAL = 5,
    GOPERL_OP_NEW_UV = 6,
    GOPERL_OP_NEW_AV = 7,
    GOPERL_OP_NEW_HV = 8,
    GOPERL_OP_NEW_RV_INC = 9,
    GOPERL_OP_AV_PUSH = 10,
    GOPERL_OP_AV_LEN = 11,
    GOPERL_OP_AV_FETCH = 12,
    GOPERL_OP_HV_STORE = 13,
    GOPERL_OP_HV_FETCH = 14,
    GOPERL_OP_REFCNT_INC = 15,
    GOPERL_OP_GV_STASHPV = 16,
    GOPERL_OP_SV_BLESS = 17,
    GOPERL_OP_SV_RV = 18,
    GOPERL_OP_SV_TYPE = 19,
    GOPERL_OP_SV_ISA = 20,
    GOPERL_OP_SV_DERIVED_FROM = 21,
    GOPERL_OP_SETREF_IV = 22,
    GOPERL_OP_SV_SETSV = 23,
    GOPERL_OP_SV_SETSV_NOMG = 24,
    GOPERL_OP_SV_SETSV_MG = 25,
    GOPERL_OP_SV_SETIV = 26,
    GOPERL_OP_SV_SETUV = 27,
    GOPERL_OP_SV_SETNV = 28,
    GOPERL_OP_SV_SETPVN = 29,
    GOPERL_OP_SV_NV = 30,
    GOPERL_OP_NEW_NV = 31,
    GOPERL_OP_NEW_SVSV = 32,
    GOPERL_OP_SV_MORTALCOPY = 33,
    GOPERL_OP_SV_CATSV = 34,
    GOPERL_OP_SV_CATSV_NOMG = 35,
    GOPERL_OP_SV_CATPVN = 36,
    GOPERL_OP_SV_TRUE = 37,
    GOPERL_OP_SV_INFO = 38,
    GOPERL_OP_SV_CUR_SET = 39,
    GOPERL_OP_SV_GROW = 40,
    GOPERL_OP_SV_EQ = 41,
    GOPERL_OP_SV_CMP = 42,
    GOPERL_OP_SV_RVWEAKEN = 43,
    GOPERL_OP_SV_2CV = 44,
    GOPERL_OP_GET_SV = 45,
    GOPERL_OP_GET_CV = 46,
    GOPERL_OP_GV_FETCH = 47,
    GOPERL_OP_AV_STORE = 48,
    GOPERL_OP_AV_EXTEND = 49,
    GOPERL_OP_AV_FILL = 50,
    GOPERL_OP_AV_READ = 51,
    GOPERL_OP_HV_FETCH_ENT = 53,
    GOPERL_OP_HV_STORE_ENT = 54,
    GOPERL_OP_HV_EXISTS_ENT = 55,
    GOPERL_OP_HE_VAL = 56,
    GOPERL_OP_HV_ITERINIT = 57,
    GOPERL_OP_HV_ITERNEXT = 58,
    GOPERL_OP_HV_ITERKEYSV = 59,
    GOPERL_OP_HV_ITERVAL = 60,
    GOPERL_OP_ENTER = 61,
    GOPERL_OP_LEAVE = 62,
    GOPERL_OP_SAVETMPS = 63,
    GOPERL_OP_FREETMPS = 64,
    GOPERL_OP_CALL_SV = 65,
    GOPERL_OP_CALL_METHOD = 66,
    GOPERL_OP_ERRSV = 67,
    GOPERL_OP_SAVE_OP = 68,
    GOPERL_OP_RUN_PP = 69,
    GOPERL_OP_MAGIC_ATTACH = 70,
    GOPERL_OP_MAGIC_ID = 71,
    GOPERL_OP_MAGIC_UNATTACH = 72,
    GOPERL_OP_PLVAR_GET = 73,
    GOPERL_OP_PLVAR_SET = 74,
    GOPERL_OP_SV_UTF8_ON = 75,
    GOPERL_OP_SV_POK_ON = 76,
    GOPERL_OP_SV_DUMP = 77,
    GOPERL_OP_SV_REFCNT_DEC = 78,
    GOPERL_OP_SV_UNMAGIC = 79,
    GOPERL_OP_SAVE_HOOK = 80,
    GOPERL_OP_NEW_XS = 81,
    GOPERL_OP_SV_STASH = 82,
    GOPERL_OP_NEW_SV = 83,
    GOPERL_OP_SV_UPGRADE = 84,
    GOPERL_OP_SV_UTF8_UPGRADE = 85,
    GOPERL_OP_SAVE_DELETE = 86,
    GOPERL_OP_SAVE_HELEM = 87,
    GOPERL_OP_NEW_PVN_SHARE = 88,
    GOPERL_OP_SV_LEN = 89,
    GOPERL_OP_NEW_HVHV = 90,
    GOPERL_OP_AV_MAKE = 91,
    GOPERL_OP_LOOKS_LIKE_NUMBER = 92,
    GOPERL_OP_SV_AMAGIC = 93,
    GOPERL_OP_AMAGIC_CALL = 94,
    GOPERL_OP_WARN = 95,
    GOPERL_OP_GET_HV = 96,
    GOPERL_OP_AV_STORE_RAW = 97,
    GOPERL_OP_PP_HOOK_SET = 98,
    GOPERL_OP_RUN_ORIGINAL = 99,
    GOPERL_OP_SAVE_DESTRUCTOR = 100,
    GOPERL_OP_OP_FIELDS = 101,
    GOPERL_OP_OP_PTR = 102,
    GOPERL_OP_COP_LINE = 103,
    GOPERL_OP_COP_FILE = 104,
    GOPERL_OP_COP_STASHPV = 105,
    GOPERL_OP_CV_INFO = 106,
    GOPERL_OP_CV_PTR = 107,
    GOPERL_OP_GV_PTR = 108,
    GOPERL_OP_GV_NAME = 109,
    GOPERL_OP_SV_ISGV_GP = 110,
    GOPERL_OP_HV_NAME = 111,
    GOPERL_OP_SI_GET = 112,
    GOPERL_OP_CX_FIELDS = 113,
    GOPERL_OP_CX_PTR = 114,
    GOPERL_OP_OP_NAME_STR = 115,
    GOPERL_OP_CV_FILE = 116,
    GOPERL_OP_GV_FETCHFILE = 117,
    GOPERL_OP_HV_CLEAR = 118,
    GOPERL_OP_HV_DELETE = 119,
    GOPERL_OP_AV_EXISTS = 120,
    GOPERL_OP_SV_UTF8_OFF = 121,
    GOPERL_OP_SAVE_SCALAR = 122,
    GOPERL_OP_SV_READONLY_ON = 123,
    GOPERL_OP_EVAL_PV = 124,
    GOPERL_OP_AV_UNSHIFT = 125,
    GOPERL_OP_NEW_CONSTSUB = 126,
    GOPERL_OP_SV_PVX_RAW = 127,
    /* v5: the Class::MOP/Moose surface (stash mro generation, glob
     * initialisation, overload flags, set-magic upgrades). */
    GOPERL_OP_HV_PKG_GEN = 128,
    GOPERL_OP_SV_REFCNT = 129,
    GOPERL_OP_GV_INIT = 130,
    GOPERL_OP_GV_AMG = 131,
    GOPERL_OP_SV_AMAGIC_SET = 132,
    GOPERL_OP_SV_MAGIC_SET_HOOK = 133,
    GOPERL_OP_GV_FETCHMETHOD = 134,
    GOPERL_OP_SV_READONLY_OFF = 135,
    GOPERL_OP_SV_PV_FORCE = 136,
    GOPERL_OP_SV_CHOP = 137,
    GOPERL_OP_SV_INSERT = 138,
    GOPERL_OP_SV_UTF8_DECODE = 139,
    GOPERL_OP_SV_UTF8_DOWNGRADE = 140,
    GOPERL_OP_MG_SET = 141,
    GOPERL_OP_GIMME_V = 142,
    GOPERL_OP_CKWARN = 143,
    GOPERL_OP_SV_LEN_BUF = 144,
    GOPERL_OP_HV_STORE_KLEN = 145,
    GOPERL_OP_HV_FETCH_KLEN = 146,
    GOPERL_OP_HV_DELETE_ENT = 147,
    GOPERL_OP_AV_POP = 148,
    GOPERL_OP_COP_HINTS = 149,
    GOPERL_OP_SV_IS_BOOL = 150,
    GOPERL_OP_SV_POK_ONLY = 151,
    GOPERL_OP_EVAL_SV = 152,
    /* the DBI surface */
    GOPERL_OP_AV_SHIFT = 153,
    GOPERL_OP_SV_2IO = 154,
    GOPERL_OP_SV_FORCE_NORMAL = 155,
    GOPERL_OP_PERLIO_OPEN = 156,
    GOPERL_OP_PERLIO_CLOSE = 157,
    GOPERL_OP_PERLIO_PUTS = 158,
    GOPERL_OP_PERLIO_FLUSH = 159,
    GOPERL_OP_PERLIO_WRITE = 160,
    GOPERL_OP_IO_OFP = 161,
    GOPERL_OP_SV_CUR = 162,
    GOPERL_OP_SV_MAGIC_STD = 163,
    /* v7: PerlIO layer bridging */
    GOPERL_OP_PERLIO_DEF_LAYER = 164,
    GOPERL_OP_PERLIO_NEXT_READ = 165,
    GOPERL_OP_PERLIO_NEXT_FASTGETS = 166,
    GOPERL_OP_PERLIO_NEXT_GETCNT = 167,
    GOPERL_OP_PERLIO_NEXT_GETPTR = 168,
    GOPERL_OP_PERLIO_NEXT_SETPTRCNT = 169,
    GOPERL_OP_PERLIO_NEXT_FILL = 170,
    GOPERL_OP_PERLIO_STATE = 171,
    /* v8: keyword plugins, lexer/parser bridging, optree construction */
    GOPERL_OP_OPTREE_NEW = 172,
    GOPERL_OP_OPTREE_MISC = 173,
    GOPERL_OP_OP_SET = 174,
    GOPERL_OP_LEX = 175,
    GOPERL_OP_PARSE = 176,
    GOPERL_OP_PARSER_GET = 177,
    GOPERL_OP_PARSER_SET = 178,
    GOPERL_OP_BLOCK = 179,
    GOPERL_OP_PAD = 180,
    GOPERL_OP_KEYWORD_ENABLE = 181,
    GOPERL_OP_CHARCLASS = 182,
    GOPERL_OP_SAVE_MISC = 183,
    GOPERL_OP_SV_CLASSIFY = 184,
    GOPERL_OP_SV_FLAGS = 185,
    GOPERL_OP_SV_SLOT_SET = 186,
    GOPERL_OP_SV_CALL = 187,
    GOPERL_OP_GV_CALL = 188,
    GOPERL_OP_SV_STR = 189,
    GOPERL_OP_SV_SLOT_GET = 190,
    GOPERL_OP_CALLER_CX = 191,
    GOPERL_OP_PERLIO = 192,
    GOPERL_OP_MEM_FREE = 193,
    GOPERL_OP_MEM_REALLOC = 194,
    GOPERL_OP_DO_OPENN = 195,
    /* Fields of interpreter structures native code reads and assigns. */
    GOPERL_OP_FIELD = 196,
    /* The regular expression API. */
    GOPERL_OP_REGEXP = 197,
    /* Hooks into compilation and teardown that run native code. */
    GOPERL_OP_HOOK = 198,
    /* The perl stack as a pp function on the host sees it. */
    GOPERL_OP_PP_STACK = 199,
    /* The context stack. */
    GOPERL_OP_CX = 200,
    /* The fields of an op and of the ops around it, in one answer. */
    GOPERL_OP_OP_DUMP = 201,
    /* Functions of the perl API that take or return more than two values. */
    GOPERL_OP_CALL = 202,
    /* Functions that build op trees and subs: a = a GOPERL_TREE_ selector,
     * s = words (b bytes of them). */
    GOPERL_OP_TREE = 203,
    /* The compiler's structures native code reaches through their fields
     * (pads, the body of a sub, the hints of a COP), and saves the
     * interpreter undoes: c = a GOPERL_INT_ selector, a = a token or an
     * address of this memory. */
    GOPERL_OP_INTERNAL = 204
};

/* the C library's waitpid, which the link otherwise gives to the program's
 * (perl_numbers.cc) */
extern "C" pid_t __real_waitpid(pid_t pid, int *status, int options);

/* INTERNAL selectors. */
enum {
    GOPERL_INT_LAYOUT = 0,          /* -> the address of the offsets of the
                                     * fields of the pad structures, and of
                                     * the entries of hashes (below) */
    GOPERL_INT_SAVE_PLVAR = 1,      /* a: an interpreter variable (GOPERL_PL_);
                                     * b: a GOPERL_SAVEK_ kind */
    GOPERL_INT_SAVE_MEM = 2,        /* a: an address of this memory; b: the
                                     * width in bytes<<8 | a GOPERL_SAVEK_ kind */
    GOPERL_INT_SAVE_AVELEM = 3,     /* a: an array; b: index<<8 | kind: the
                                     * slot of its body */
    GOPERL_INT_LEAVE_SCOPE = 4,     /* b: the save stack index to leave to */
    GOPERL_INT_CV_GET = 5,          /* a: a sub; b: a GOPERL_CVF_ field */
    GOPERL_INT_CV_SET = 6,          /* a: a sub; b: the value; c: field<<32 */
    GOPERL_INT_PAD_NEW = 7,         /* b: the flags -> a padlist */
    GOPERL_INT_PADNAME_NEW = 8,     /* s: the name, b: its length -> a pad name */
    GOPERL_INT_PADNAME_FREE = 9,    /* a: a pad name: PadnameREFCNT_dec */
    GOPERL_INT_SAVE_FREEOP = 10,    /* a: an op */
    GOPERL_INT_RUNOPS = 11,         /* a: the op to run from (see below) */
    GOPERL_INT_COP_LABEL = 12,      /* a: a COP -> the address of the label, or
                                     * 0; second: utf8<<31 | length */
    GOPERL_INT_HINTHASH = 13,       /* a: a COP -> its hints hash (a chain) */
    GOPERL_INT_HINTHASH_FETCH = 14, /* a: a chain; b: flags<<32 | length;
                                     * s: the key -> the value */
    GOPERL_INT_HINTHASH_2HV = 15,   /* a: a chain; b: the flags -> a new hash */
    GOPERL_INT_OBJ_FIELDS = 16,     /* a: an object -> the address of its fields;
                                     * second: their number (ObjectMAXFIELD + 1) */
    GOPERL_INT_OBJ_MAXFIELD_SET = 17, /* a: an object; b: ObjectMAXFIELD */
    GOPERL_INT_OBJ_FIELDS_NEW = 18, /* a: an object; b: a number of fields -> the
                                     * address of the new array of its fields,
                                     * all NULL (ObjectMAXFIELD is one less) */
    GOPERL_INT_PARSER_NEW = 19,     /* s = [copline][preambling] words: a parser
                                     * of the interpreter's in place of none */
    GOPERL_INT_HV_ARRAY = 20,       /* a: a hash -> the address of its buckets, or
                                     * 0; second: their number (HvMAX + 1) */
    GOPERL_INT_PERLIO_DEBUG = 21,   /* s: a message for PerlIO_debug */
    GOPERL_INT_SV_PV_FREE = 22,     /* a: an SV: SvPV_free */
    GOPERL_INT_STATCACHE = 23,      /* b = 0: -> the address of the fields of
                                     * PL_statcache as 13 64-bit words (below);
                                     * b = 1: s = those words, stored there */
    GOPERL_INT_FILTER_ADD = 24,     /* a: the data (or 0); b: the host's id of
                                     * the function -> the data filter_add
                                     * returned, or 0 */
    GOPERL_INT_FILTER_DEL = 25,     /* b: the host's id of the function */
    GOPERL_INT_FILTER_READ = 26,    /* a: the buffer; b: idx<<32 | maxlen ->
                                     * filter_read's result */
    GOPERL_INT_FILTER_ID = 27,      /* a: a filter's data -> the host's id of the
                                     * function of native code it runs, or 0 */
    GOPERL_INT_SV_GETS = 28,        /* a: the SV; b: the handle; c: append<<32
                                     * -> 1 when a line was read, 0 at the end */
    GOPERL_INT_BHK_REGISTER = 29,   /* b: the flags -> the id of a new block
                                     * hook of native code */
    GOPERL_INT_BHK_FLAGS = 30,      /* a: the id of a block hook; b: its flags */
    GOPERL_INT_SAVE_FIELD = 31,     /* a: an SV; b: field<<8 | kind (GOPERL_SAVEK_):
                                     * a save of the field (GOPERL_SVF_) */
    GOPERL_INT_HASH = 32,           /* s+b: a key's bytes -> its hash (PERL_HASH),
                                     * as the keys of this interpreter's hashes
                                     * have it */
    GOPERL_INT_PUSHSTACK = 33,      /* b: the stackinfo type: PUSHSTACKi */
    GOPERL_INT_POPSTACK = 34,       /* POPSTACK */
    GOPERL_INT_WAITPID = 35,        /* a: a pid; b: 1 for WNOHANG -> the pid
                                     * waitpid returned (0: none has ended),
                                     * second: the status word; or -1, second:
                                     * errno */
    GOPERL_INT_MY_EXIT = 36,        /* a: 0, b: a status: my_exit; a: 1:
                                     * my_failure_exit */
    GOPERL_INT_SIGHANDLER = 37,     /* b: a signal: the interpreter's own
                                     * handler of it (native code calls the
                                     * handler PL_sighandlerp had) */
    GOPERL_INT_SCAN_VSTRING = 38,   /* a: an SV; s+b: a v-string's spelling ->
                                     * the bytes scan_vstring read */
    GOPERL_INT_SAVE_SVREF = 39,     /* a: the address of a slot of this
                                     * memory (an element's, HeVAL) ->
                                     * save_svref's new SV */
    GOPERL_INT_SIGACTION = 40,      /* b: a signal -> the program's action
                                     * of it: 0 the default, 1 ignored, 2 a
                                     * handler */
    GOPERL_INT_WAIT_MASK = 41       /* a: 1 a wait of native code with a
                                     * signal mask begins, 0 it has ended;
                                     * b: the mask (the one to put back at
                                     * the end), or with c's high half 1 the
                                     * address of a sigset_t of this memory
                                     * -> the mask before; second: 1 when a
                                     * handler ran */
};
/* The fields of PL_statcache (INTERNAL STATCACHE), in this order: dev, ino,
 * mode, nlink, uid, gid, rdev, size, atime, mtime, ctime, blksize, blocks. */
#define GOPERL_STATCACHE_WORDS 13
/* What a save is of (INTERNAL SAVE_*): a pointer that holds a reference
 * (SAVESPTR), a pointer that holds none (SAVEVPTR, SAVEPPTR, save_hptr), a
 * pointer whose reference the save owns (SAVEGENERICSV), an integer. */
enum {
    GOPERL_SAVEK_SPTR = 0,
    GOPERL_SAVEK_VPTR = 1,
    GOPERL_SAVEK_GENERIC = 2,
    GOPERL_SAVEK_INT = 3
};
/* The fields of a sub (INTERNAL CV_GET / CV_SET), stored as they are. */
enum {
    GOPERL_CVF_FLAGS = 0,
    GOPERL_CVF_ROOT = 1,
    GOPERL_CVF_START = 2,
    GOPERL_CVF_OUTSIDE_SEQ = 3,
    GOPERL_CVF_PADLIST = 4,
    GOPERL_CVF_DEPTH = 5,
    GOPERL_CVF_FILE = 6,
    GOPERL_CVF_STASH = 7,
    GOPERL_CVF_GV_U = 8,
    GOPERL_CVF_OUTSIDE = 9
};

/* TREE selectors. */
enum {
    GOPERL_TREE_NEWREF = 0,         /* [kind][flags or type][op] */
    GOPERL_TREE_NEWFOROP = 1,       /* [flags][sv][expr][block][cont] */
    GOPERL_TREE_NEWNULLLIST = 2,
    GOPERL_TREE_NEWDEFSVOP = 3,
    GOPERL_TREE_NEWRANGE = 4,       /* [flags][left][right] */
    GOPERL_TREE_NEWWHILEOP = 5,     /* [flags][debuggable][loop][expr][block][cont][has_my] */
    GOPERL_TREE_NEWANON = 6,        /* [0 list, 1 hash][op] */
    GOPERL_TREE_OP_LVALUE = 7,      /* [op][type] */
    GOPERL_TREE_START_SUBPARSE = 8, /* [is_format][flags] -> floor */
    GOPERL_TREE_NEWATTRSUB = 9,     /* [floor][o][proto][attrs][block] -> cv */
    GOPERL_TREE_NEWANONATTRSUB = 10, /* [floor][proto][attrs][block] -> op */
    GOPERL_TREE_PAD_ADD_NAME_SV = 11, /* [name sv][flags][typestash][ourstash] */
    GOPERL_TREE_FIND_RUNCV = 12,
    GOPERL_TREE_SAVE_CLEARSV = 13,  /* [pad index] */
    GOPERL_TREE_LEX_STUFF_SV = 14,  /* [sv][flags] */
    GOPERL_TREE_RV2CV_OP_CV = 15,   /* [cvop][flags] -> cv or gv */
    GOPERL_TREE_CK_ARGS = 16,       /* [which][entersubop][namegv][protosv] -> op */
    GOPERL_TREE_CALLCK_SET = 17,    /* [cv][host id, 0: the interpreter's own][ckobj][flags] */
    GOPERL_TREE_CALLCK_GET = 18,    /* [cv][gflags] -> the address of [which][id][ckobj][flags] */
    GOPERL_TREE_CXINC = 19,
    GOPERL_TREE_NEWPVOP = 20,       /* [type][flags], then the string */
    GOPERL_TREE_NEWLOOPEX = 21,     /* [type][label op] */
    GOPERL_TREE_NEWLOOPOP = 22,     /* [flags][debuggable][expr][block] */
    GOPERL_TREE_NEWUNOP_AUX = 23,   /* [type][flags][first][kind] and the aux:
                                     * kind 0 none, 1 a number in place of
                                     * the pointer, 2 the three fields of an
                                     * op_argcheck_aux */
    GOPERL_TREE_PARSE_SUBSIGNATURE = 24, /* [flags] -> op */
    GOPERL_TREE_NEWMYSUB = 25,      /* [floor][o][proto][attrs][block] -> cv */
    GOPERL_TREE_CV_NAME = 26        /* [cv][0 get, 1 set][utf8][length], then
                                     * the name for a set. get -> the address
                                     * of the name the sub has by itself, or
                                     * 0; second: length<<1 | utf8 */
};


/* OP_DUMP modes (b = cap<<8 | mode; bit 7 of the mode: the op is a COP
 * whatever its type says, as PL_compiling is). */
enum {
    GOPERL_OPDUMP_ONE = 0,     /* the op */
    GOPERL_OPDUMP_SUBTREE = 1, /* and the ops below it */
    GOPERL_OPDUMP_REACH = 2,   /* and every op that can be reached from it */
    GOPERL_OPDUMP_FREED = 3    /* no op: the described ops freed since the
                                * last time, as tokens (the address of them;
                                * second: their number) */
};
/* One op in the answer of OP_DUMP, as words. */
enum {
    GOPERL_OPREC_TOK = 0,
    GOPERL_OPREC_NEXT = 1,
    GOPERL_OPREC_SIBPARENT = 2,
    GOPERL_OPREC_FIRST = 3,
    GOPERL_OPREC_LAST = 4,
    GOPERL_OPREC_OTHER = 5,
    GOPERL_OPREC_REDOOP = 6,
    GOPERL_OPREC_NEXTOP = 7,
    GOPERL_OPREC_LASTOP = 8,
    GOPERL_OPREC_TARG = 9,
    GOPERL_OPREC_TYPE = 10, /* type | flags<<16 | private<<24 */
    GOPERL_OPREC_BITS = 11, /* GOPERL_OPBIT_ flags | class<<8 */
    GOPERL_OPREC_LINE = 12,
    GOPERL_OPREC_HINTS = 13,
    GOPERL_OPREC_FILE = 14,
    GOPERL_OPREC_FILE_LEN = 15,
    GOPERL_OPREC_STASH = 16,
    GOPERL_OPREC_STASH_LEN = 17,
    GOPERL_OPREC_SV = 18,
    GOPERL_OPREC_PV = 19,
    GOPERL_OPREC_WARNINGS = 20,
    GOPERL_OPREC_SEQ = 21,
    GOPERL_OPREC_AUX = 22,  /* address of the items of a multideref */
    GOPERL_OPREC_AUX_N = 23,
    GOPERL_OPREC_FEATURES = 24, /* cop_features.bits[0] */
    GOPERL_OPREC_WORDS = 25
};
static_assert(COP_FEATURE_SIZE == 1, "a COP's feature bits are one word in the op record");
#define GOPERL_OPBIT_MORESIB 1
#define GOPERL_OPBIT_COP 2
#define GOPERL_OPBIT_HOOKED 4
#define GOPERL_OPBIT_EDGE 8 /* outside what was asked for: no links */
#define GOPERL_OPBIT_RAW 16 /* backs a module's own op struct */
#define GOPERL_OPBIT_OPT 0x10000
#define GOPERL_OPBIT_SLABBED 0x20000
#define GOPERL_OPBIT_SAVEFREE 0x40000
#define GOPERL_OPBIT_STATIC 0x80000
#define GOPERL_OPBIT_FOLDED 0x100000
#define GOPERL_OPBIT_TYPEHOOK 0x200000 /* made while native code claimed its
                                        * type: PL_ppaddr's function then */

/* CALL selectors (c = selector, a = token). */
enum {
    GOPERL_CALL_PV_DISPLAY = 0,
    GOPERL_CALL_PV_ESCAPE = 1,
    GOPERL_CALL_PV_PRETTY = 2,
    GOPERL_CALL_CV_CLONE = 3,
    GOPERL_CALL_NEW_WARNINGS = 4, /* Perl_new_warnings_bitfield */
    GOPERL_CALL_GETCWD_SV = 5,
    GOPERL_CALL_COP_SET = 6,      /* b = value, c = field<<32 */
    GOPERL_CALL_MESS_SV = 7,
    GOPERL_CALL_MG_FIND = 8, /* b = type -> the address of the words, or 0 */
    GOPERL_CALL_ATOF = 9,    /* s = the text -> the bits of the NV */
    GOPERL_CALL_AMT = 10,    /* a = stash -> the address of the overload table's
                              * words: [flags][fallback][was_ok_sub][n], n CVs */
    GOPERL_CALL_UVAR = 11,   /* uvar magic whose functions are the host's */
    GOPERL_CALL_XOP = 12,    /* custom_op_register: b = class, s = name NUL
                              * description NUL -> the op's number */
    GOPERL_CALL_COP_SAVE = 13, /* b: 0 SAVECOPLINE, 1 SAVECOPFILE_FREE,
                                * 2 SAVECOPSTASH_FREE */
    GOPERL_CALL_LEX_START = 14, /* a = the line (or 0), b = flags */
    GOPERL_CALL_PARSER = 15, /* -> the address of the parser's fields, or 0 */
    GOPERL_CALL_SCAN_VERSION = 17, /* a = the SV to fill; b = the address of
                              * the text in linear memory, or 0 for the text
                              * in s; c = qv<<32 -> how many bytes were taken */
    GOPERL_CALL_NEWGVGEN = 18, /* s = the package, b = flags -> a new glob */
    GOPERL_CALL_RANDOM = 19, /* b low 2 bits: 0 Drand01() -> the bits of the
                              * double; 1 seedDrand01(b >> 2); 2 seed() */
    GOPERL_CALL_QERROR = 20, /* a = the message: a compile error is queued */
    GOPERL_CALL_SV_CAT_DECODE = 21, /* sv_cat_decode (see the call) */
    GOPERL_CALL_SAVE_RE_CONTEXT = 22, /* save_re_context() */
    GOPERL_CALL_SV_PEEK = 23,   /* a = the SV -> the address of sv_peek's text */
    GOPERL_CALL_SV_RECODE = 24, /* a = the SV, b = the encoding -> the address of
                                 * the string after, or 0 */
    GOPERL_CALL_POS_GET = 28,   /* a = the SV, hi = flags -> [found][pos] */
    GOPERL_CALL_POS_SET = 29,   /* a = the SV, b = pos, hi = flags */
    GOPERL_CALL_POS_CLEAR = 30, /* a = the SV */
    GOPERL_CALL_HASH_SEED = 31, /* -> the address of [count][the bytes] */
    GOPERL_CALL_LVALUE_SUB = 32, /* b: 0 is_lvalue_sub(), 1 LVRET (of the
                                  * entersub that called the XSUB) */
    GOPERL_CALL_CV_NAME = 27,   /* a = the CV (or glob), b = the SV to set or
                                 * 0, hi = flags -> cv_name's SV */
    GOPERL_CALL_MG_LIST = 26,   /* a = the SV -> the address of [number] and,
                                 * per MAGIC of the interpreter's own, in the
                                 * chain's order, the six words of MG_FIND */
    GOPERL_CALL_CASEMAP = 25,   /* a = which (0 upper, 1 lower, 2 title, 3 fold),
                                 * b = a code point -> the address of [the first
                                 * code point of the result][its length in
                                 * bytes], two words, and its UTF-8 bytes */
    GOPERL_CALL_ASYNC = 16   /* what is pending is delivered, as between ops
                              * -> 1 when a handler died (the error is $@) */
};
/* The words of CALL_PARSER. */
enum {
    GOPERL_PARSERW_LINESTR = 0,
    GOPERL_PARSERW_PV = 1,
    GOPERL_PARSERW_BUFPTR = 2,
    GOPERL_PARSERW_BUFEND = 3,
    GOPERL_PARSERW_OLDBUFPTR = 4,
    GOPERL_PARSERW_OLDOLDBUFPTR = 5,
    GOPERL_PARSERW_LINESTART = 6,
    GOPERL_PARSERW_ERROR_COUNT = 7,
    GOPERL_PARSERW_IN_MY = 8,
    GOPERL_PARSERW_LEX_FLAGS = 9,
    GOPERL_PARSERW_PREAMBLING = 10,
    GOPERL_PARSERW_EXPECT = 11,
    GOPERL_PARSERW_LEX_STATE = 12,
    GOPERL_PARSERW_LEX_BRACKETS = 13,
    GOPERL_PARSERW_LEX_INWHAT = 14,
    GOPERL_PARSERW_MULTI_START = 15,
    GOPERL_PARSERW_MULTI_END = 16,
    GOPERL_PARSERW_MULTI_OPEN = 17,
    GOPERL_PARSERW_MULTI_CLOSE = 18,
    GOPERL_PARSERW_COPLINE = 19,
    GOPERL_PARSERW_IN_MY_STASH = 20,
    GOPERL_PARSERW_LAST_UNI = 21,
    GOPERL_PARSERW_LAST_LOP = 22,
    GOPERL_PARSERW_RSFP = 23,
    GOPERL_PARSERW_RSFP_FILTERS = 24,
    GOPERL_PARSERW_LEX_FORMBRACK = 25,
    GOPERL_PARSERW_LEX_STUFF = 26,
    GOPERL_PARSERW_LEX_REPL = 27,
    GOPERL_PARSERW_LEX_BRACKSTACK = 28, /* an address of this memory */
    GOPERL_PARSERW_TOKENBUF = 29,       /* the address of the parser's
                                         * tokenbuf (a char[256]) */
    GOPERL_PARSERW_FILTERED = 30,
    GOPERL_PARSERW_LAST_LOP_OP = 31,
    GOPERL_PARSERW_WORDS = 32
};
/* COP_SET fields. */
enum {
    GOPERL_COPF_LINE = 0,
    GOPERL_COPF_WARNINGS = 1,
    GOPERL_COPF_HINTS = 2,
    GOPERL_COPF_STASH = 3,
    GOPERL_COPF_FILE = 4, /* s = the name */
    GOPERL_COPF_FEATURES = 5,
    GOPERL_COPF_FILE_FREE = 6 /* the file is let go of (CopFILE_free) */
};

/* FIELD selectors (c = selector, a = token, b = the value to store). */
enum {
    GOPERL_FIELD_REFCNT_SET = 0,
    GOPERL_FIELD_HVAUX_BACKREFS = 1,
    GOPERL_FIELD_HVAUX_RITER = 2,
    GOPERL_FIELD_HVAUX_RITER_SET = 3,
    GOPERL_FIELD_HVAUX_EITER = 4,
    GOPERL_FIELD_HVAUX_EITER_SET = 5,
    GOPERL_FIELD_AV_FILLP = 6,
    GOPERL_FIELD_AV_FILLP_SET = 7,
    GOPERL_FIELD_CV_DEPTH_SET = 8,
    GOPERL_FIELD_LV_TYPE = 9,
    GOPERL_FIELD_LV_TARG = 10,
    GOPERL_FIELD_SHARED_HASH = 11,
    GOPERL_FIELD_OP_SV = 12,
    GOPERL_FIELD_OP_GV = 13,
    GOPERL_FIELD_IS_FREED = 14,
    GOPERL_FIELD_GV_REFCNT = 15,
    GOPERL_FIELD_CV_OUTSIDE_SET = 16,
    GOPERL_FIELD_HVAUX_FLAGS = 17,
    GOPERL_FIELD_LV_TYPE_SET = 18,
    GOPERL_FIELD_LV_TARG_SET = 19,
    /* the fields of an IO, the plain stores of sv.h: b = the handle (an
     * address of this memory), the type character, the flags */
    GOPERL_FIELD_IO_IFP_SET = 20,
    GOPERL_FIELD_IO_OFP_SET = 21,
    GOPERL_FIELD_IO_TYPE_SET = 22,
    GOPERL_FIELD_IO_FLAGS_SET = 23,
    /* the other words of an IO's body, b = which (GOPERL_IOWORD_): the
     * load gives the word (an address for a glob or a name), the store
     * (which in the high half of c) takes b, or a copy of the text s for
     * a name */
    GOPERL_FIELD_IO_WORD = 24,
    GOPERL_FIELD_IO_WORD_SET = 25
};
enum {
    GOPERL_IOWORD_LINES = 0,
    GOPERL_IOWORD_PAGE = 1,
    GOPERL_IOWORD_PAGE_LEN = 2,
    GOPERL_IOWORD_LINES_LEFT = 3,
    GOPERL_IOWORD_TOP_GV = 4,
    GOPERL_IOWORD_FMT_GV = 5,
    GOPERL_IOWORD_BOTTOM_GV = 6,
    GOPERL_IOWORD_TOP_NAME = 7,
    GOPERL_IOWORD_FMT_NAME = 8,
    GOPERL_IOWORD_BOTTOM_NAME = 9
};
static_assert(HvAUXf_IS_CLASS == 0x4, "the SDK's HvAUXf_IS_CLASS is this build's");

/* REGEXP selectors (c = selector, a = token). */
enum {
    GOPERL_RX_OF_SV = 0,   /* SvRX */
    GOPERL_RX_COMP = 1,    /* pregcomp(pattern sv, b = flags) */
    GOPERL_RX_EXEC = 2,    /* pregexec; s = six words */
    GOPERL_RX_BODY = 3,    /* the fields of the regexp, as words */
    GOPERL_RX_OFFS = 4,    /* b = paren -> start; second: end */
    GOPERL_RX_COMP_PV = 5, /* re_compile of bytes: s = pattern, b = flags<<32 | len */
    GOPERL_RX_EXEC_FLAGS = 6 /* regexec_flags (CALLREGEXEC); s = seven words: the six of
                              * GOPERL_RX_EXEC with the flags in place of nosave,
                              * and the data (0) */
};

/* HOOK selectors (a = selector). */
enum {
    GOPERL_HOOK_NEWOP_SET = 0,  /* b = type<<1 | on */
    GOPERL_HOOK_NEWOP_ORIG = 1, /* b = op, c = type -> op */
    GOPERL_HOOK_PEEP_SET = 2,   /* b = which<<1 | on (0 PL_peepp, 1 PL_rpeepp) */
    GOPERL_HOOK_PEEP_ORIG = 3,  /* b = op, c = which */
    GOPERL_HOOK_ATEXIT = 4,     /* b = host id */
    GOPERL_HOOK_UNWIND = 5      /* leave what the running native call opened */
};

/* PP_STACK selectors (a = selector). */
enum {
    GOPERL_PPSTACK_PULL = 0, /* -> the address of the state's words; second:
                              * their number */
    GOPERL_PPSTACK_PUSH = 1  /* s = the state, b = its length in bytes */
};

/* CX selectors (c = selector, a = stackinfo token or 0). */
enum {
    GOPERL_CX_DUMP = 0,     /* b = index -> address of the fields, or 0 */
    GOPERL_CX_DOUNWIND = 1, /* b = index */
    GOPERL_CX_MAX = 2,      /* cxstack_max */
    GOPERL_CX_SET = 3       /* s = words [index][field][value]: a field of
                             * the context, numbered as CX_DUMP's words */
};

/* PERLIO selectors: the handle functions of perlio.h. */
enum {
    GOPERL_PIO_STDIN = 0,
    GOPERL_PIO_STDOUT = 1,
    GOPERL_PIO_STDERR = 2,
    GOPERL_PIO_FILENO = 3,
    GOPERL_PIO_READ = 4,
    GOPERL_PIO_EOF = 5,
    GOPERL_PIO_ERROR = 6,
    GOPERL_PIO_CLEARERR = 7,
    GOPERL_PIO_SEEK_SET = 8,
    GOPERL_PIO_SEEK_CUR = 9,
    GOPERL_PIO_SEEK_END = 10,
    GOPERL_PIO_TELL = 11,
    GOPERL_PIO_GETC = 12,
    GOPERL_PIO_UNGETC = 13,
    GOPERL_PIO_UNREAD = 14,
    GOPERL_PIO_FDOPEN = 15,
    GOPERL_PIO_BINMODE = 16,
    GOPERL_PIO_SETLINEBUF = 17,
    GOPERL_PIO_IFP = 18,
    GOPERL_PIO_OFP = 19,
    GOPERL_PIO_VALID = 20,
    GOPERL_PIO_FAST_GETS = 21,
    GOPERL_PIO_HAS_CNTPTR = 22,
    GOPERL_PIO_CANSET_CNT = 23,
    GOPERL_PIO_HAS_BASE = 24,
    GOPERL_PIO_GET_CNT = 25,
    GOPERL_PIO_GET_PTR = 26,
    GOPERL_PIO_GET_BASE = 27,
    GOPERL_PIO_GET_BUFSIZ = 28,
    GOPERL_PIO_SET_PTRCNT = 29,
    GOPERL_PIO_FILL = 30,
    GOPERL_PIO_APPLY_LAYERS = 31,
    GOPERL_PIO_LAYERS = 32, /* -> the address of [n] and, for each layer
                            * from the top, [the address of its next
                            * field][the address of its name][its kind][its
                            * flags]; with b = 1 five more words a layer:
                            * [buf][end][ptr][bufsiz][posn] of a buffered one
                            * (PerlIOBuf), zeros for another */
    /* The stdio stream of the handle, made as perl makes it: FIND_FILE is
     * PerlIO_findFILE (the stream of the handle's :stdio layer, or a new
     * one), EXPORT_FILE is PerlIO_exportFILE (s = the mode, or empty): the
     * handle is flushed, a stream is opened on its descriptor and a :stdio
     * layer with it is pushed. Both -> the address of [descriptor][the
     * stream][st_dev][st_ino][offset], the descriptor being -1 when there is
     * no stream; the device, the inode and the offset (-1 where it has none)
     * are the descriptor's, for the host to know whether its descriptor of
     * that number is the same.
     * RELEASE_FILE (b = the stream) is
     * PerlIO_releaseFILE: the :stdio layer of that stream is popped. */
    GOPERL_PIO_FIND_FILE = 33,
    GOPERL_PIO_EXPORT_FILE = 34,
    GOPERL_PIO_RELEASE_FILE = 35,
    GOPERL_PIO_OPENN = 36,  /* a = the old handle or 0; b = the length of s:
                             * four words (fd, imode, perm, narg), narg
                             * words of SVs, the layers NUL the mode NUL */
    GOPERL_PIO_ISUTF8 = 37
};

/* SAVE_MISC selectors. */
enum {
    GOPERL_SAVE_FREESV = 0,
    GOPERL_SAVE_MORTALIZESV = 1,
    GOPERL_SAVE_ITEM = 2,
    GOPERL_SAVE_ARY = 3,
    GOPERL_SAVE_HASH = 4,
    GOPERL_SAVE_GP = 5,
    GOPERL_SAVE_HINTS = 6,
    GOPERL_SAVE_COMPILE_WARNINGS = 7,
    GOPERL_SAVE_DEFSV = 8
};

/* SV_SLOT_SET selectors. */
enum {
    GOPERL_SVSLOT_RV = 0,
    GOPERL_SVSLOT_STASH = 1,
    GOPERL_SVSLOT_IV = 2,
    GOPERL_SVSLOT_UV = 3,
    GOPERL_SVSLOT_NV = 4,
    GOPERL_SVSLOT_PV = 5,
    GOPERL_SVSLOT_LEN = 6,
    GOPERL_SVSLOT_GV_STASH = 7,
    GOPERL_SVSLOT_GV_CVGEN = 8
};

/* What the SDK stores in the buffer slot of an SV whose buffer is a
 * pointer of native code's process (the SDK's perl.h): no buffer has the
 * address. Native code keeps a structure of its own behind an SV that way,
 * an SV that is no string. */
#define GOPERL_PV_HOST ((char *)(uintptr_t)1)

/* A scalar whose buffer slot holds GOPERL_PV_HOST cannot be a string of
 * any length: its bytes are in the memory of native code's process, which
 * the interpreter has no access to, and it would read and write its own
 * memory at that address instead (File::Map maps a file and makes the
 * mapping the string of a scalar). The flags, the buffer or the length
 * that would make it one are refused, before they change, with an error
 * that says so. An empty string is no such string: no byte of it is read
 * (Object::Pad keeps a structure behind an SV that is an empty string, and
 * so does File::Map with the mapping of an empty file). */
static bool goperl_pv_host_string(pTHX_ SV *sv, U32 flags, char *pv, STRLEN cur) {
    if (!(flags & (SVf_POK | SVp_POK)) || !cur) return false;
    if (SvTYPE(sv) < SVt_PV || SvTYPE(sv) == SVt_PVAV || SvTYPE(sv) == SVt_PVHV ||
        SvTYPE(sv) == SVt_PVCV || SvTYPE(sv) == SVt_PVFM || SvTYPE(sv) == SVt_PVIO)
        return false;
    if (isGV_with_GP(sv) || SvROK(sv)) return false;
    return pv == GOPERL_PV_HOST;
}
static void goperl_pv_host_refuse(pTHX) __attribute__((noreturn));
static void goperl_pv_host_refuse(pTHX) {
    Perl_croak(aTHX_ "the string of a scalar cannot be in native memory on go-perl: native "
                     "code set the scalar's buffer (SvPVX, SvPV_set) to an address outside "
                     "the interpreter's memory and made it a string");
}

/* SV_CALL selectors. */
enum {
    GOPERL_SVCALL_MG_GET = 0,
    GOPERL_SVCALL_MG_CLEAR = 1,
    GOPERL_SVCALL_MG_FREE = 2,
    GOPERL_SVCALL_MG_MAGICAL = 3,
    GOPERL_SVCALL_MG_SIZE = 4,
    GOPERL_SVCALL_BACKOFF = 5,
    GOPERL_SVCALL_UNREF = 6,
    GOPERL_SVCALL_RVUNWEAKEN = 7,
    GOPERL_SVCALL_UTF8_ENCODE = 8,
    GOPERL_SVCALL_LEN_UTF8 = 9,
    GOPERL_SVCALL_DEC = 10,
    GOPERL_SVCALL_TAINTED = 11,
    GOPERL_SVCALL_TAINT = 12,
    GOPERL_SVCALL_UNTAINT = 13,
    GOPERL_SVCALL_REF = 14,
    GOPERL_SVCALL_CV_CONST_SV = 15,
    GOPERL_SVCALL_CV_UNDEF = 16,
    GOPERL_SVCALL_POS_U2B = 17,
    GOPERL_SVCALL_POS_B2U = 18,
    GOPERL_SVCALL_SET_UNDEF = 19,
    GOPERL_SVCALL_SHRINK_TO_CUR = 20,
    GOPERL_SVCALL_AV_DELETE = 21,
    GOPERL_SVCALL_VOK = 22,
    GOPERL_SVCALL_HV_USEDKEYS = 23,
    GOPERL_SVCALL_HV_TOTALKEYS = 24,
    GOPERL_SVCALL_HV_FILL = 25,
    GOPERL_SVCALL_HV_MAX = 26,
    GOPERL_SVCALL_AV_MAX = 27,
    GOPERL_SVCALL_UTF8_UPGRADE = 28,
    GOPERL_SVCALL_NEWSVSV = 29,
    GOPERL_SVCALL_NEWAVAV = 30,
    GOPERL_SVCALL_NEWAVHV = 31,
    GOPERL_SVCALL_SETRV_INC = 32,
    GOPERL_SVCALL_SETRV_NOINC = 33,
    GOPERL_SVCALL_SETSV = 34,
    GOPERL_SVCALL_CATSV = 35,
    GOPERL_SVCALL_CMP = 36,
    GOPERL_SVCALL_EQ = 37,
    GOPERL_SVCALL_MRO_LINEAR_ISA = 38,
    GOPERL_SVCALL_MRO_METHOD_CHANGED = 39,
    GOPERL_SVCALL_2BOOL = 40,
    GOPERL_SVCALL_2IV = 41,
    GOPERL_SVCALL_2UV = 42,
    GOPERL_SVCALL_2NV = 43,
    GOPERL_SVCALL_2PV = 44,
    GOPERL_SVCALL_CLEAR_UNDEF = 45,
    GOPERL_SVCALL_IO_TYPE = 46,
    GOPERL_SVCALL_IO_FLAGS = 47,
    GOPERL_SVCALL_IO_LINES = 48,
    GOPERL_SVCALL_SETPV_BUFSIZE = 49,
    GOPERL_SVCALL_THROW = 50,
    GOPERL_SVCALL_2PVBYTE = 51,
    GOPERL_SVCALL_HV_KSPLIT = 52,
    GOPERL_SVCALL_UNI_DISPLAY = 53,
    GOPERL_SVCALL_TAINT_ENV = 54, /* no SV: taint_env() */
    GOPERL_SVCALL_MAGIC_IS_VALUE = 55, /* the SV slot is a magic type: whether
                 * it is value magic, which `local` does not copy */
    GOPERL_SVCALL_TIED = 56, /* b = the kind of tie magic -> the object the SV
                 * is tied to (SvTIED_obj), or 0 when it is not tied */
    GOPERL_SVCALL_PV_AS = 57, /* the string of the SV in an encoding:
                 * b = SV_GMAGIC or 0, hi = how: 1 UTF-8, 2 bytes, plus
                 * 0x100 for "or null" (an undefined value has no string)
                 * -> the string, 0 for none; second: its length */
    GOPERL_SVCALL_PV_FORCE_AS = 58, /* the SV made a string of its own, to be
                 * written to: b = SV_GMAGIC or 0, hi = how: 0 as it is,
                 * 1 UTF-8, 2 bytes -> the string; second: its length */
    GOPERL_SVCALL_CMP_LOCALE = 60, /* as CMP, by the rules of the locale */
    GOPERL_SVCALL_AMAGIC = 61, /* amagic_call: the SV is the left operand, b
                                * the right one, hi the method's index |
                                * the AMGf_ flags << 8; the result, or 0
                                * when the operator is not overloaded */
    GOPERL_SVCALL_NUMCMP = 62, /* as CMP, numerically (sv_numcmp_flags) */
    GOPERL_SVCALL_INC = 63,    /* sv_inc; b: 1 without get magic */
    GOPERL_SVCALL_AV_BODY = 59 /* the body of an array as it is in memory:
                 * AvARRAY; second: the number of elements (AvFILLp + 1).
                 * Native code reads the slots there in one go. */
};

/* GV_CALL selectors: the ones below FIRST_CV take a glob, the rest a CV. */
enum {
    GOPERL_GVCALL_SVN = 0,
    GOPERL_GVCALL_AVN = 1,
    GOPERL_GVCALL_HVN = 2,
    GOPERL_GVCALL_ION = 3,
    GOPERL_GVCALL_CV_SET = 4,
    GOPERL_GVCALL_FLAGS = 5,
    GOPERL_GVCALL_CVGEN = 6,
    GOPERL_GVCALL_FORM = 7,
    GOPERL_GVCALL_FULLNAME = 8,
    GOPERL_GVCALL_GP = 9,
    GOPERL_GVCALL_FILE = 10,
    GOPERL_GVCALL_LINE = 11,
    GOPERL_GVCALL_NAME_UTF8 = 12,
    GOPERL_GVCALL_SV_SET = 13,
    GOPERL_GVCALL_AV_SET = 14,
    GOPERL_GVCALL_HV_SET = 15,
    GOPERL_GVCALL_CVGEN_SET = 16, /* GvCVGEN(gv) = b */
    GOPERL_GVCALL_STASH_SET = 17, /* GvSTASH(gv) = b, the plain assignment */
    GOPERL_GVCALL_IO_SET = 18,    /* GvIOp(gv) = b, the plain assignment */
    GOPERL_GVCALL_FIRST_CV = 32,
    GOPERL_GVCALL_CV_GV_SET = 32,
    GOPERL_GVCALL_CV_STASH_SET = 33,
    GOPERL_GVCALL_CV_FLAGS = 34,
    GOPERL_GVCALL_CV_OUTSIDE = 35,
    GOPERL_GVCALL_CV_PROTO = 36,
    GOPERL_GVCALL_CV_NATIVE = 37 /* CvXSUB(cv) = the native function the
                 * host knows by the id b */
};

/* SV_STR selectors. */
enum {
    GOPERL_SVSTR_CATPVN = 0,
    GOPERL_SVSTR_NEWSVRV = 1,
    GOPERL_SVSTR_DOES = 2,
    GOPERL_SVSTR_FETCHMETH = 3,
    GOPERL_SVSTR_FETCHMETH_AUTOLOAD = 4,
    GOPERL_SVSTR_FETCHMETHOD = 5,
    GOPERL_SVSTR_GV_FETCHPVN = 6,
    GOPERL_SVSTR_GET_CVN = 7,
    GOPERL_SVSTR_WHICHSIG = 8,
    GOPERL_SVSTR_COP_HINT = 9,
    GOPERL_SVSTR_SETENV = 10,
    /* Text with its encoding. Flags bit 0: the bytes are UTF-8. */
    GOPERL_SVSTR_NEWPVN = 11, /* bit 1: the new SV is mortal */
    GOPERL_SVSTR_WARN = 12,
    GOPERL_SVSTR_THROW = 13,
    GOPERL_SVSTR_SETPVN = 14,
    GOPERL_SVSTR_TAINT_PROPER = 15, /* taint_proper(NULL, s) */
    GOPERL_SVSTR_GV_NAME_SET = 17, /* gv_name_set(gv, s, len, flags) */
    GOPERL_SVSTR_HV_NAME_SET = 18, /* hv_name_set(stash, s, len, flags) */
    GOPERL_SVSTR_WARNER = 16 /* a warning of the category in the SV slot.
                 * Flags bit 0: the text is UTF-8; bits 1-2: 0 warner (always
                 * given), 1 ck_warner (when the category is enabled),
                 * 2 ck_warner_d (when it is not disabled). Fatal when the
                 * caller's `use warnings FATAL` makes the category so. */
};

/* v8 sub-selectors (shared with the SDK headers). */
enum {
    GOPERL_OPC_BASEOP = 0,
    GOPERL_OPC_UNOP = 1,
    GOPERL_OPC_BINOP = 2,
    GOPERL_OPC_LISTOP = 3,
    GOPERL_OPC_LOGOP = 4,
    GOPERL_OPC_SVOP = 5,
    GOPERL_OPC_PVOP = 6,
    GOPERL_OPC_GVOP = 7,
    GOPERL_OPC_METHOP_NAMED = 8,
    GOPERL_OPC_STATEOP = 9,
    GOPERL_OPC_CONDOP = 10,
    GOPERL_OPC_SLICEOP = 11,
    GOPERL_OPC_RAW = 12,
    GOPERL_OPC_ASSIGNOP = 13
};
enum {
    GOPERL_OPM_APPEND_ELEM = 0,
    GOPERL_OPM_APPEND_LIST = 1,
    GOPERL_OPM_PREPEND_ELEM = 2,
    GOPERL_OPM_CONVERT_LIST = 3,
    GOPERL_OPM_CONTEXTUALIZE = 4,
    GOPERL_OPM_SCOPE = 5,
    GOPERL_OPM_LINKLIST = 6,
    GOPERL_OPM_FREE = 7,
    GOPERL_OPM_NULL = 8,
    GOPERL_OPM_FORCE_LIST = 9,
    GOPERL_OPM_SIBLING_SPLICE = 10
};
enum {
    GOPERL_OPF_NEXT = 0,
    GOPERL_OPF_SIBPARENT = 1,
    GOPERL_OPF_FIRST = 2,
    GOPERL_OPF_LAST = 3,
    GOPERL_OPF_OTHER = 4,
    GOPERL_OPF_TARG = 5,
    GOPERL_OPF_TYPE = 6,
    GOPERL_OPF_FLAGS = 7,
    GOPERL_OPF_PRIVATE = 8,
    GOPERL_OPF_PPADDR_HOOK = 9,
    GOPERL_OPF_MORESIB = 10,
    GOPERL_OPF_PPADDR_OWN = 11 /* the pp function of the op's type again */
};
enum {
    GOPERL_LEX_READ_SPACE = 0,
    GOPERL_LEX_PEEK_UNICHAR = 1,
    GOPERL_LEX_READ_UNICHAR = 2,
    GOPERL_LEX_READ_TO = 3,
    GOPERL_LEX_BUFUTF8 = 4,
    GOPERL_LEX_STUFF_PVN = 5,
    GOPERL_LEX_UNSTUFF = 6,
    GOPERL_LEX_GROW_LINESTR = 7,
    GOPERL_LEX_NEXT_CHUNK = 8,
    GOPERL_LEX_DISCARD_TO = 9,
    /* the scanning functions perl gives B::Hooks::Parser (PERL_EXT); b is
     * the address in the buffer to scan from */
    GOPERL_LEX_SCAN_WORD = 10, /* s = [destlen][allow_package] -> the address
                                * of [the address after the word][its length]
                                * [the word] */
    GOPERL_LEX_SCAN_STR = 11,  /* s = [keep_quoted][keep_delims][re_reparse] ->
                                * the address of [the address after the
                                * string, or 0][the address of the closing
                                * delimiter]; the string is the parser's
                                * lex_stuff */
    GOPERL_LEX_SKIPSPACE = 12  /* s = [flags] -> the address after the space */
};
enum {
    GOPERL_PARSE_BLOCK = 0,
    GOPERL_PARSE_TERMEXPR = 1,
    GOPERL_PARSE_LISTEXPR = 2,
    GOPERL_PARSE_ARITHEXPR = 3,
    GOPERL_PARSE_FULLEXPR = 4,
    GOPERL_PARSE_FULLSTMT = 5,
    GOPERL_PARSE_STMTSEQ = 6,
    GOPERL_PARSE_BARESTMT = 7
};
enum {
    GOPERL_PARSER_PRESENT = 0,
    GOPERL_PARSER_LINESTR = 1,
    GOPERL_PARSER_BUFPTR = 2,
    GOPERL_PARSER_BUFEND = 3,
    GOPERL_PARSER_OLDBUFPTR = 4,
    GOPERL_PARSER_LINESTART = 5,
    GOPERL_PARSER_ERROR_COUNT = 6,
    GOPERL_PARSER_IN_MY = 7,
    GOPERL_PARSER_LEX_FLAGS = 8,
    GOPERL_PARSER_PREAMBLING = 9,
    GOPERL_PARSER_PV = 10
};
enum {
    GOPERL_PAD_ALLOC = 0,
    GOPERL_PAD_ADD_NAME_PVN = 1,
    GOPERL_PAD_FINDMY_PVN = 2,
    GOPERL_PAD_INTRO_MY = 3,
    GOPERL_PAD_SETSV = 4,
    GOPERL_PAD_SV_FETCH = 5,
    GOPERL_PAD_CV_PADLIST = 6,   /* b = cv -> the padlist */
    GOPERL_PAD_PADLIST = 7,      /* b = padlist -> the address of [max][names][pads...] */
    GOPERL_PAD_NAMELIST = 8,     /* b = padnamelist -> the address of [max][maxnamed][names...] */
    GOPERL_PAD_NAME = 9,         /* b = padname -> the address of [pv][len][flags]
                                  * [refcnt][low][high][ourstash][type] */
    GOPERL_PAD_SAVE_CLEARSV = 10, /* b = pad index */
    GOPERL_PAD_NAMELIST_STORE = 11 /* b = padnamelist, c = key, s = a word: the padname */
};
enum {
    GOPERL_CC_IDFIRST = 0,
    GOPERL_CC_IDCONT = 1,
    GOPERL_CC_WORDCHAR = 2,
    GOPERL_CC_SPACE = 3,
    GOPERL_CC_DIGIT = 4,
    GOPERL_CC_ALPHA = 5,
    GOPERL_CC_UPPER = 6
};

/* PLVAR_GET / PLVAR_SET interpreter-variable ids (the `a` argument). */
#define GOPERL_PL_DIEHOOK 1
#define GOPERL_PL_WARNHOOK 2
#define GOPERL_PL_SV_UNDEF 3
#define GOPERL_PL_SV_YES 4
#define GOPERL_PL_SV_NO 5
#define GOPERL_PL_CURCOP 6
#define GOPERL_PL_OP 7
#define GOPERL_PL_PERLDB 8
#define GOPERL_PL_DBSUB 9
#define GOPERL_PL_DBSINGLE 10
#define GOPERL_PL_ENDAV 11
#define GOPERL_PL_CHECKAV 12
#define GOPERL_PL_INITAV 13
#define GOPERL_PL_MAIN_CV 14
#define GOPERL_PL_DEBSTASH 15
#define GOPERL_PL_SAWAMPERSAND 16
#define GOPERL_PL_SCOPESTACK_IX 17
#define GOPERL_PL_EXIT_FLAGS 18
#define GOPERL_PL_BASETIME 19
#define GOPERL_PL_MODGLOBAL 20
#define GOPERL_PL_MINUS_C 21
#define GOPERL_PL_CURSTASH 22
#define GOPERL_PL_DOWARN 23
#define GOPERL_PL_HINTS 24
#define GOPERL_PL_SUB_GENERATION 25
#define GOPERL_PL_DEFGV 26
#define GOPERL_PL_HINTGV 27
#define GOPERL_PL_COMPCV 28
#define GOPERL_PL_ERRGV 29
#define GOPERL_PL_DEFSTASH 30
#define GOPERL_PL_INCGV 31
#define GOPERL_PL_STDERRGV 32
#define GOPERL_PL_DEFOUTGV 33
#define GOPERL_PL_RS 34
#define GOPERL_PL_IN_EVAL 35
#define GOPERL_PL_PHASE 36
#define GOPERL_PL_TAINTING 37
#define GOPERL_PL_TAINTED 38
#define GOPERL_PL_DIRTY 39
#define GOPERL_PL_SAVESTACK_IX 40
#define GOPERL_PL_TMPS_IX 41
#define GOPERL_PL_STDINGV 42
#define GOPERL_PL_ARGVGV 43
#define GOPERL_PL_ENVGV 44
#define GOPERL_PL_LOCALIZING 45
#define GOPERL_PL_BEGINAV 46
#define GOPERL_PL_UNITCHECKAV 47
#define GOPERL_PL_SV_ZERO 48
#define GOPERL_PL_SV_PLACEHOLDER 49
#define GOPERL_PL_STRTAB 50
#define GOPERL_PL_STATGV 51
#define GOPERL_PL_TMPS_FLOOR 52
#define GOPERL_PL_ORIGFILENAME 53
#define GOPERL_PL_UNSAFE 54
#define GOPERL_PL_DESTRUCT_LEVEL 55
#define GOPERL_PL_LASTGV 56
#define GOPERL_PL_COP_SEQMAX 57
#define GOPERL_PL_ERRSV 58
#define GOPERL_PL_DEFSV 59
#define GOPERL_PL_ERRNO 60
#define GOPERL_PL_PID 61
#define GOPERL_PL_STATUSVALUE 62
#define GOPERL_PL_COMPILING 63      /* &PL_compiling */
#define GOPERL_PL_WARN_ALL 64       /* pWARN_ALL */
#define GOPERL_PL_WARN_NONE 65      /* pWARN_NONE */
#define GOPERL_PL_ERRORS 66
#define GOPERL_PL_SCOPESTACK 67     /* -> the address of the array */
#define GOPERL_PL_SAVEBEGIN 68
#define GOPERL_PL_CURPAD 69         /* -> the address of the array */
#define GOPERL_PL_COMPPAD 70
#define GOPERL_PL_COMPPAD_NAME 71
#define GOPERL_PL_OP_FREED 72       /* -> the address of the count of described
                                     * ops that were freed and not yet reported */
#define GOPERL_PL_CURPM 73
#define GOPERL_PL_RESTARTOP 74
#define GOPERL_PL_MARKSTACK_IX 75
#define GOPERL_PL_STACK_IX 76
#define GOPERL_PL_MARKSTACK 77      /* -> the address of the array */
#define GOPERL_PL_SRAND_CALLED 78
#define GOPERL_PL_SV_COUNT 79       /* read only */
#define GOPERL_PL_SIG_PENDING 80    /* read only */
#define GOPERL_PL_FIRSTGV 81        /* the globs of $a and $b of a sort block */
#define GOPERL_PL_SECONDGV 82
/* The state of the compilation of a sub (pad.c, toke.c). */
#define GOPERL_PL_PADIX 83
#define GOPERL_PL_CONSTPADIX 84
#define GOPERL_PL_COMPPAD_NAME_FILL 85
#define GOPERL_PL_MIN_INTRO_PENDING 86
#define GOPERL_PL_MAX_INTRO_PENDING 87
#define GOPERL_PL_CV_HAS_EVAL 88
#define GOPERL_PL_PAD_RESET_PENDING 89
#define GOPERL_PL_SUBLINE 90
#define GOPERL_PL_SUBNAME 91
#define GOPERL_PL_CURSTNAME 92
#define GOPERL_PL_PADIX_FLOOR 93
#define GOPERL_PL_PARSER 94         /* the parser is CALL 15's; a store
                                     * only clears it (b = 0), as code does
                                     * that hides the parser from the hooks
                                     * an op it builds runs */
#define GOPERL_PL_MULTIDEREF_PC 95  /* the item of a multideref's aux being
                                     * run (an address of this memory) */
#define GOPERL_PL_NOMEMOK 96
#define GOPERL_PL_MINUS_N 97
#define GOPERL_PL_MINUS_P 98
#define GOPERL_PL_RUNOPS 99         /* the host's id of the run loop of native
                                     * code that is PL_runops, 0 for the
                                     * interpreter's own */
#define GOPERL_PL_ARGVOUTGV 100
#define GOPERL_PL_GLOBALSTASH 101
#define GOPERL_PL_SIGNALS 102
#define GOPERL_PL_OP_MASK 103       /* the address of the mask, or 0 */
#define GOPERL_PL_LASTSTATVAL 104
#define GOPERL_PL_LASTSTYPE 105
#define GOPERL_PL_STATNAME 106
#define GOPERL_PL_CURSTACK 107      /* read only */
#define GOPERL_PL_MAINSTACK 108     /* read only */
#define GOPERL_PL_IN_CLEAN_OBJS 109 /* read only */
#define GOPERL_PL_IN_CLEAN_ALL 110  /* read only */
#define GOPERL_PL_ORIGARGC 111      /* read only */
#define GOPERL_PL_ORIGARGV 112      /* read only: the address of the vector */
#define GOPERL_PL_ORS 113          /* PL_ors_sv, the output record separator */
#define GOPERL_PL_SORTCOP 114      /* PL_sortcop, as the word it is: native code
                                    * keeps what it likes there while it sorts
                                    * with a comparator of its own */
#define GOPERL_PL_SIGHANDLERP 115  /* the host's id of the signal handler of
                                    * native code that is PL_sighandlerp, 0
                                    * for the interpreter's own */
#define GOPERL_PL_SV_ARENAROOT 116 /* the first SV arena (read only): native
                                    * code walks the arenas from it */

/* SV_INFO result bitset. SYNTHETIC flags - deliberately NOT perl's real
 * SvFLAGS bits; both sides pin these values so host-side SvOK/SvPOK/...
 * macros agree with the guest. */
#define GOPERL_INFO_OK 1
#define GOPERL_INFO_ROK 2
#define GOPERL_INFO_POK 4
#define GOPERL_INFO_NIOK 8
#define GOPERL_INFO_UTF8 16
#define GOPERL_INFO_ISOBJ 32
#define GOPERL_INFO_READONLY 64
#define GOPERL_INFO_ISCV 128
#define GOPERL_INFO_ISGV 256
#define GOPERL_INFO_IOK 512
#define GOPERL_INFO_NOK 1024
#define GOPERL_INFO_POKp 2048
#define GOPERL_INFO_NIOKp 4096
#define GOPERL_INFO_ISUV 8192
#define GOPERL_INFO_RMAGICAL 16384
#define GOPERL_INFO_OBJECT 32768

/* Pointer-field selectors (the `b` argument of the *_PTR / SI_GET ops). */
#define GOPERL_OPPTR_NEXT 0    /* OP_PTR: op_next */
#define GOPERL_OPPTR_SIBLING 1 /* OP_PTR: OpSIBLING */
#define GOPERL_OPPTR_FIRST 2   /* OP_PTR: op_first (OPf_KIDS only) */
#define GOPERL_OPPTR_REDOOP 3  /* OP_PTR: op_redoop (LOOP class only) */
#define GOPERL_OPPTR_LAST 4    /* OP_PTR: op_last (BINOP/LISTOP-shaped) */
#define GOPERL_OPPTR_OTHER 5   /* OP_PTR: op_other (LOGOP-shaped) */
#define GOPERL_OPPTR_MORESIB 6 /* OP_PTR: OpHAS_SIBLING */
#define GOPERL_OPPTR_IS_HOOKED 7 /* OP_PTR: op_ppaddr is the host hook */
#define GOPERL_CVPTR_START 0   /* CV_PTR: CvSTART */
#define GOPERL_CVPTR_GV 1      /* CV_PTR: CvGV */
#define GOPERL_CVPTR_STASH 2   /* CV_PTR: CvSTASH */
#define GOPERL_CVPTR_ROOT 3    /* CV_PTR: CvROOT */
#define GOPERL_GVPTR_STASH 0   /* GV_PTR: GvSTASH */
#define GOPERL_GVPTR_CVU 1     /* GV_PTR: GvCVu */
#define GOPERL_GVPTR_HV 2      /* GV_PTR: GvHV */
#define GOPERL_GVPTR_AV 3      /* GV_PTR: GvAV */
#define GOPERL_GVPTR_EGV 4     /* GV_PTR: GvEGV */
#define GOPERL_GVPTR_SV 5      /* GV_PTR: GvSV */
#define GOPERL_GVPTR_IO 6      /* GV_PTR: GvIO */
#define GOPERL_GVPTR_CV 7      /* GV_PTR: GvCV */
#define GOPERL_SI_SELF 0       /* SI_GET: the stackinfo token itself */
#define GOPERL_SI_PREV 1       /* SI_GET: si_prev */
#define GOPERL_SI_TYPE 2       /* SI_GET: si_type */
#define GOPERL_SI_CXIX 3       /* SI_GET: si_cxix */
#define GOPERL_CXPTR_OLDCOP 0  /* CX_PTR: blk_oldcop */
#define GOPERL_CXPTR_CV 1      /* CX_PTR: blk_sub.cv (SUB/FORMAT only) */
#define GOPERL_CXPTR_MYOP 2    /* CX_PTR: blk_loop.my_op (LOOP only) */

/* CX_FIELDS result when the context index is out of range. */
#define GOPERL_CX_TYPE_NONE 0xFFFFFFFFu

/* The operation box (pl.h perl_xs_opbox): the operands of an operation the
 * host writes into this memory, and its result, and the second value of an
 * operation that answers two (goperl_two). */
#define GOPERL_OPBOX_STR 4096
struct goperl_opbox {
    uint64_t a;
    uint64_t b;
    uint64_t c;
    uint64_t result;
    uint64_t result2;
    int32_t op;
    uint32_t slen;
    char s[GOPERL_OPBOX_STR];
};
static_assert(offsetof(goperl_opbox, c) == 16 && offsetof(goperl_opbox, result) == 24 &&
                  offsetof(goperl_opbox, result2) == 32 && offsetof(goperl_opbox, op) == 40 &&
                  offsetof(goperl_opbox, slen) == 44 && offsetof(goperl_opbox, s) == 48,
              "the box's layout is the host's");
static goperl_opbox g_opbox;

/* The answer of an operation that answers two values: the first is its
 * result, the second is left in the box (result2), where the host reads it
 * right after the operation, whichever way it asked for it (perl_xs_op or
 * perl_xs_helper). Both are evaluated before the second is stored, so
 * nothing the operation does in between can overwrite it. */
static inline uint64_t goperl_two(uint64_t first, uint64_t second) {
    g_opbox.result2 = second;
    return first;
}

/* What an op is now, as a word: targ<<32 | private<<24 | flags<<16 | type
 * (0 for no op). The shadow native code has of an op is described again
 * when it says otherwise: the op may have been changed without native code
 * being entered (the peephole optimizer rewrites ops a checker of native
 * code was handed). */
static inline uint64_t goperl_op_fields(const OP *o) {
    if (!o) return 0;
    return ((uint64_t)(uint32_t)o->op_targ << 32) | ((uint64_t)o->op_private << 24) |
           ((uint64_t)o->op_flags << 16) | (uint64_t)o->op_type;
}

/* The IV and UV of native code and this interpreter's are the same 64 bits:
 * an integer crosses as it is, in both directions, and the flags native code
 * sees are the interpreter's own. */
static_assert(sizeof(IV) == sizeof(int64_t) && sizeof(UV) == sizeof(uint64_t),
              "the XS helper carries IVs and UVs as 64-bit operands");

/* SvIV (or SvUV, for want_uv) of sv; get magic runs unless nomg. */
static uint64_t goperl_sv_integer(pTHX_ SV *sv, bool want_uv, bool nomg) {
    if (want_uv) return (uint64_t)(nomg ? SvUV_nomg(sv) : SvUV(sv));
    return (uint64_t)(int64_t)(nomg ? SvIV_nomg(sv) : SvIV(sv));
}

/* The ops that back a module's own op struct (OPTREE_NEW's RAW class). The
 * module's struct lives on the host; this interpreter holds the links, in
 * an op as large as a LOGOP whose class it does not know. */
static std::unordered_set<OP *> g_raw_ops;

/* The class a custom op was built with by a constructor called for native
 * code (newSVOP(OP_CUSTOM, ...)): until the op has its pp function, which
 * the module assigns afterwards, that is all that says what the op is. */
static std::unordered_map<OP *, OPclass> g_custom_built;

/* The classes of ops, as the op-class word of OP_DUMP names them: the
 * interpreter's own OPclass. */
static void goperl_op_record(pTHX_ OP *o, bool is_cop, bool edge,
                             std::vector<goperl_gword_t> &out) {
    goperl_gword_t w[GOPERL_OPREC_WORDS];
    memset(w, 0, sizeof w);
    if (o->op_type == OP_FREED) {
        /* an op whose slot went back to the free list of its slab, which a
         * link an op still holds can lead to: its type says so, and nothing
         * else of it means anything */
        w[GOPERL_OPREC_TOK] = GOPERL_ADDR(o);
        w[GOPERL_OPREC_TYPE] = (uint32_t)OP_FREED;
        w[GOPERL_OPREC_BITS] = edge ? GOPERL_OPBIT_EDGE : 0;
        out.insert(out.end(), w, w + GOPERL_OPREC_WORDS);
        return;
    }
    OPclass cls = is_cop ? OPclass_COP : op_class(o);
    if (o->op_type == OP_CUSTOM && !g_custom_built.empty()) {
        auto built = g_custom_built.find(o);
        if (built != g_custom_built.end()) cls = built->second;
    }
    w[GOPERL_OPREC_TOK] = GOPERL_ADDR(o);
    /* as wide as the field: a module keeps a pointer of its own there
     * (op_targ = PTR2IV(data)) */
    w[GOPERL_OPREC_TARG] = (goperl_gword_t)o->op_targ;
    w[GOPERL_OPREC_TYPE] = (uint32_t)o->op_type | ((uint32_t)o->op_flags << 16) |
                           ((uint32_t)o->op_private << 24);
    uint32_t bits = (uint32_t)cls << 8;
    if (o->op_ppaddr == goperl_pp_type_hook) bits |= GOPERL_OPBIT_TYPEHOOK;
    else if (goperl_pp_is_hook(o->op_ppaddr)) bits |= GOPERL_OPBIT_HOOKED;
    if (o->op_opt) bits |= GOPERL_OPBIT_OPT;
    if (o->op_slabbed) bits |= GOPERL_OPBIT_SLABBED;
    if (o->op_savefree) bits |= GOPERL_OPBIT_SAVEFREE;
    if (o->op_static) bits |= GOPERL_OPBIT_STATIC;
    if (o->op_folded) bits |= GOPERL_OPBIT_FOLDED;
    if (cls == OPclass_COP) bits |= GOPERL_OPBIT_COP;
    if (edge) {
        w[GOPERL_OPREC_BITS] = bits | GOPERL_OPBIT_EDGE;
        out.insert(out.end(), w, w + GOPERL_OPREC_WORDS);
        return;
    }
    if (OpHAS_SIBLING(o)) bits |= GOPERL_OPBIT_MORESIB;
    const bool raw = g_raw_ops.count(o) != 0;
    if (raw) bits |= GOPERL_OPBIT_RAW;
    w[GOPERL_OPREC_BITS] = bits;
    w[GOPERL_OPREC_NEXT] = GOPERL_ADDR(o->op_next);
    w[GOPERL_OPREC_SIBPARENT] = GOPERL_ADDR(o->op_sibparent);
    if (o->op_flags & OPf_KIDS) w[GOPERL_OPREC_FIRST] = GOPERL_ADDR(cUNOPx(o)->op_first);
    if (raw) {
        /* the one link it has room for beside op_first: op_other or
         * op_last, whichever the module's struct makes of it */
        w[GOPERL_OPREC_OTHER] = GOPERL_ADDR(cLOGOPx(o)->op_other);
        out.insert(out.end(), w, w + GOPERL_OPREC_WORDS);
        return;
    }
    /* An op that was nulled keeps the memory of the op it was, and op_targ
     * says which: the list of arguments of a call is an ex-list that still
     * has its last child, and code that walks a tree reads it
     * (`((LISTOP *)o)->op_last`). */
    if (o->op_type == OP_NULL && (o->op_flags & OPf_KIDS) && o->op_targ > 0 &&
        o->op_targ < OP_max) {
        const U32 was = PL_opargs[o->op_targ] & OA_CLASS_MASK;
        if (was == OA_BINOP || was == OA_LISTOP)
            w[GOPERL_OPREC_LAST] = GOPERL_ADDR(cBINOPx(o)->op_last);
    }
    /* A call whose children are its arguments themselves was made out of
     * the list of them (op_convert_list) and is a list op in memory, though
     * its class is that of a unary op; ck_subr tells that shape from the
     * other, a call above an ex-list, the same way. */
    if (o->op_type == OP_ENTERSUB && (o->op_flags & OPf_KIDS) && cUNOPx(o)->op_first &&
        OpHAS_SIBLING(cUNOPx(o)->op_first))
        w[GOPERL_OPREC_LAST] = GOPERL_ADDR(cLISTOPx(o)->op_last);
    switch (cls) {
    case OPclass_BINOP:
    case OPclass_LISTOP:
    case OPclass_PMOP:
        w[GOPERL_OPREC_LAST] = GOPERL_ADDR(cBINOPx(o)->op_last);
        break;
    case OPclass_LOGOP:
        w[GOPERL_OPREC_OTHER] = GOPERL_ADDR(cLOGOPx(o)->op_other);
        break;
    case OPclass_LOOP:
        w[GOPERL_OPREC_LAST] = GOPERL_ADDR(cLOOPx(o)->op_last);
        w[GOPERL_OPREC_REDOOP] = GOPERL_ADDR(cLOOPx(o)->op_redoop);
        w[GOPERL_OPREC_NEXTOP] = GOPERL_ADDR(cLOOPx(o)->op_nextop);
        w[GOPERL_OPREC_LASTOP] = GOPERL_ADDR(cLOOPx(o)->op_lastop);
        break;
    case OPclass_SVOP:
        w[GOPERL_OPREC_SV] = GOPERL_ADDR(cSVOPx_sv(o));
        break;
    case OPclass_PADOP:
        w[GOPERL_OPREC_SV] = GOPERL_ADDR(PAD_SVl(cPADOPx(o)->op_padix));
        break;
    case OPclass_METHOP:
        if (!(o->op_flags & OPf_KIDS))
            w[GOPERL_OPREC_SV] = GOPERL_ADDR(cMETHOPx_meth(o));
        break;
    case OPclass_PVOP:
        w[GOPERL_OPREC_PV] = GOPERL_ADDR(cPVOPx(o)->op_pv);
        break;
    case OPclass_UNOP_AUX:
        if (o->op_type == OP_ARGELEM)
            /* the index is kept where the pointer would be */
            w[GOPERL_OPREC_AUX_N] = (uint32_t)PTR2IV(cUNOP_AUXx(o)->op_aux);
        if (o->op_type == OP_MULTIPARAM) {
            /* the op_multiparam_aux, which the host reads field by field at
             * these offsets (go-perl's SDK, goperl_multiparam_aux) */
            static_assert(offsetof(struct op_multiparam_aux, min_args) == 0 &&
                              offsetof(struct op_multiparam_aux, n_positional) == 8 &&
                              offsetof(struct op_multiparam_aux, n_named) == 16 &&
                              offsetof(struct op_multiparam_aux, slurpy) == 24 &&
                              offsetof(struct op_multiparam_aux, slurpy_padix) == 32 &&
                              offsetof(struct op_multiparam_aux, param_padix) == 40 &&
                              offsetof(struct op_multiparam_aux, named) == 48 &&
                              sizeof(PADOFFSET) == 8,
                          "the host reads op_multiparam_aux at these offsets");
            static_assert(offsetof(struct op_multiparam_named_aux, padix) == 0 &&
                              offsetof(struct op_multiparam_named_aux, namepv) == 8 &&
                              offsetof(struct op_multiparam_named_aux, namelen) == 16 &&
                              offsetof(struct op_multiparam_named_aux, namehash) == 24 &&
                              sizeof(struct op_multiparam_named_aux) == 32,
                          "the host reads op_multiparam_named_aux at these offsets");
            w[GOPERL_OPREC_AUX] = GOPERL_ADDR(cUNOP_AUXx(o)->op_aux);
        }
        if (o->op_type == OP_MULTIDEREF) {
            UNOP_AUX_item *items = cUNOP_AUXx(o)->op_aux;
            w[GOPERL_OPREC_AUX] = GOPERL_ADDR(items);
            w[GOPERL_OPREC_AUX_N] = (uint32_t)items[-1].uv;
        }
        break;
    case OPclass_COP: {
        COP *cop = (COP *)o;
        const char *file = CopFILE(cop);
        const char *stash = CopSTASHPV(cop);
        w[GOPERL_OPREC_LINE] = (uint32_t)CopLINE(cop);
        w[GOPERL_OPREC_HINTS] = (uint32_t)cop->cop_hints;
        w[GOPERL_OPREC_FILE] = GOPERL_ADDR(file);
        w[GOPERL_OPREC_FILE_LEN] = file ? (uint32_t)strlen(file) : 0;
        w[GOPERL_OPREC_STASH] = GOPERL_ADDR(stash);
        w[GOPERL_OPREC_STASH_LEN] = stash ? (uint32_t)strlen(stash) : 0;
        w[GOPERL_OPREC_WARNINGS] = GOPERL_ADDR(cop->cop_warnings);
        w[GOPERL_OPREC_SEQ] = (uint32_t)cop->cop_seq;
        w[GOPERL_OPREC_FEATURES] = cop->cop_features.bits[0];
        break;
    }
    default: break;
    }
    out.insert(out.end(), w, w + GOPERL_OPREC_WORDS);
}

/* The op and the ops around it (see the GOPERL_OPDUMP_ modes), at most cap
 * of them with their links; every op one of those links to and that is not
 * among them follows as an edge record, so that no link of the answer
 * leads to an op the answer does not describe. */
static void goperl_op_dump(pTHX_ OP *root, int mode, bool root_is_cop,
                           size_t cap, std::vector<goperl_gword_t> &out) {
    std::vector<OP *> todo;
    std::vector<OP *> full;
    std::unordered_map<OP *, bool> seen; /* true: with its links */
    todo.push_back(root);
    while (!todo.empty() && full.size() < cap) {
        OP *o = todo.back();
        todo.pop_back();
        if (!o || seen.count(o)) continue;
        seen[o] = true;
        full.push_back(o);
        if (mode == GOPERL_OPDUMP_ONE) break;
        if (o->op_type == OP_FREED) continue; /* its links are the slab's */
        if (o->op_flags & OPf_KIDS)
            for (OP *kid = cUNOPx(o)->op_first; kid; kid = OpSIBLING(kid))
                todo.push_back(kid);
        if (mode != GOPERL_OPDUMP_REACH) continue;
        todo.push_back(o->op_next);
        todo.push_back(o->op_sibparent);
        if (g_raw_ops.count(o)) {
            todo.push_back(cLOGOPx(o)->op_other);
            continue;
        }
        switch (o == root && root_is_cop ? OPclass_COP : op_class(o)) {
        case OPclass_LOGOP: todo.push_back(cLOGOPx(o)->op_other); break;
        case OPclass_LOOP:
            todo.push_back(cLOOPx(o)->op_redoop);
            todo.push_back(cLOOPx(o)->op_nextop);
            todo.push_back(cLOOPx(o)->op_lastop);
            break;
        default: break;
        }
    }
    const size_t first = out.size();
    for (OP *o : full) goperl_op_record(aTHX_ o, o == root && root_is_cop, false, out);
    const size_t nfull = full.size();
    static const int links[] = {GOPERL_OPREC_NEXT, GOPERL_OPREC_SIBPARENT, GOPERL_OPREC_FIRST,
                                GOPERL_OPREC_LAST, GOPERL_OPREC_OTHER, GOPERL_OPREC_REDOOP,
                                GOPERL_OPREC_NEXTOP, GOPERL_OPREC_LASTOP};
    for (size_t i = 0; i < nfull; i++)
        for (int l : links) {
            OP *t = (OP *)(uintptr_t)out[first + i * GOPERL_OPREC_WORDS + l];
            if (!t || seen.count(t)) continue;
            seen[t] = false;
            goperl_op_record(aTHX_ t, false, true, out);
        }
}

/* The ops the host holds a description of (every op OP_DUMP answered
 * with), and the ones of them the interpreter has freed since the host last
 * asked: a description is good until its op is freed, since only then can
 * the address name another op. The host reads the count straight from this
 * memory, and asks for the list when it is not zero.
 *
 * Every op perl frees goes back through Slab_Free, by op_free, which tells
 * PL_opfreehook, or by op_destroy, which does not (the list op_append_list
 * moved into another, the loop op newFOROP makes anew); and newFOROP moves
 * a loop op made outside a slab with realloc. The patch to op.c (see
 * scripts/wasi-configure.sh) reports each of them here. */
static std::unordered_set<OP *> g_ops_described;
static std::vector<goperl_gword_t> g_ops_freed;
static uint32_t g_ops_freed_count = 0;
extern "C" void goperl_op_released(OP *o) {
    if (!g_raw_ops.empty()) g_raw_ops.erase(o);
    if (!g_custom_built.empty()) g_custom_built.erase(o);
    if (!g_ops_described.empty()) {
        auto it = g_ops_described.find(o);
        if (it != g_ops_described.end()) {
            g_ops_described.erase(it);
            g_ops_freed.push_back(GOPERL_ADDR(o));
            g_ops_freed_count = (uint32_t)g_ops_freed.size();
        }
    }
}

/* ---- INTERNAL: the compiler's structures and saves of interpreter state ---- */

/* A save of an interpreter variable, made as perl's own code makes it for
 * that variable: a pointer by the kind of save asked for (a pointer that
 * holds a reference, one that holds none, one whose reference the save
 * owns), an integer by the variable's own type. */
static void goperl_save_plvar(pTHX_ int id, int kind) {
#define GOPERL_SAVE_PTRVAR(var)                                         \
    do {                                                                \
        if (kind == GOPERL_SAVEK_GENERIC) save_generic_svref((SV **)&(var)); \
        else if (kind == GOPERL_SAVEK_VPTR) save_vptr((void *)&(var));  \
        else save_sptr((SV **)&(var));                                  \
        return;                                                         \
    } while (0)
    switch (id) {
    case GOPERL_PL_CURCOP: GOPERL_SAVE_PTRVAR(PL_curcop);
    case GOPERL_PL_OP: GOPERL_SAVE_PTRVAR(PL_op);
    case GOPERL_PL_DBSUB: GOPERL_SAVE_PTRVAR(PL_DBsub);
    case GOPERL_PL_DBSINGLE: GOPERL_SAVE_PTRVAR(PL_DBsingle);
    case GOPERL_PL_ENDAV: GOPERL_SAVE_PTRVAR(PL_endav);
    case GOPERL_PL_CHECKAV: GOPERL_SAVE_PTRVAR(PL_checkav);
    case GOPERL_PL_INITAV: GOPERL_SAVE_PTRVAR(PL_initav);
    case GOPERL_PL_BEGINAV: GOPERL_SAVE_PTRVAR(PL_beginav);
    case GOPERL_PL_UNITCHECKAV: GOPERL_SAVE_PTRVAR(PL_unitcheckav);
    case GOPERL_PL_CURSTASH: GOPERL_SAVE_PTRVAR(PL_curstash);
    case GOPERL_PL_COMPCV: GOPERL_SAVE_PTRVAR(PL_compcv);
    case GOPERL_PL_DEFOUTGV: GOPERL_SAVE_PTRVAR(PL_defoutgv);
    case GOPERL_PL_RS: GOPERL_SAVE_PTRVAR(PL_rs);
    case GOPERL_PL_ORS: GOPERL_SAVE_PTRVAR(PL_ors_sv);
    case GOPERL_PL_SORTCOP: save_vptr((void *)&PL_sortcop); return;
    case GOPERL_PL_ERRORS: GOPERL_SAVE_PTRVAR(PL_errors);
    case GOPERL_PL_CURPAD: GOPERL_SAVE_PTRVAR(PL_curpad);
    /* Making a pad the one in use makes its values PL_curpad too (see
     * GOPERL_PL_COMPPAD under the variables native code sets), so putting
     * the pad back puts its values back, as perl's SAVECOMPPAD does: a save
     * of PL_curpad made after the pad was changed (Exporter::Lexical saves
     * PL_comppad, sets it, then saves PL_curpad) holds the new pad's
     * values, and the scope ends with the pad and the values of the code
     * that was running. */
    case GOPERL_PL_COMPPAD:
        if (kind != GOPERL_SAVEK_GENERIC) {
            SAVECOMPPAD();
            return;
        }
        GOPERL_SAVE_PTRVAR(PL_comppad);
    case GOPERL_PL_COMPPAD_NAME: GOPERL_SAVE_PTRVAR(PL_comppad_name);
    case GOPERL_PL_CURPM: GOPERL_SAVE_PTRVAR(PL_curpm);
    case GOPERL_PL_FIRSTGV: GOPERL_SAVE_PTRVAR(PL_firstgv);
    case GOPERL_PL_SECONDGV: GOPERL_SAVE_PTRVAR(PL_secondgv);
    case GOPERL_PL_SUBNAME: GOPERL_SAVE_PTRVAR(PL_subname);
    case GOPERL_PL_PARSER: GOPERL_SAVE_PTRVAR(PL_parser);
    case GOPERL_PL_CURSTNAME: GOPERL_SAVE_PTRVAR(PL_curstname);
    case GOPERL_PL_RESTARTOP: GOPERL_SAVE_PTRVAR(PL_restartop);
    /* $@ and $_ are the scalar slots of their globs */
    case GOPERL_PL_ERRSV: GOPERL_SAVE_PTRVAR(GvSVn(PL_errgv));
    case GOPERL_PL_DEFSV: GOPERL_SAVE_PTRVAR(GvSVn(PL_defgv));
    case GOPERL_PL_ERRNO: save_int(&errno); return;
    case GOPERL_PL_TAINTED: save_bool(&PL_tainted); return;
    case GOPERL_PL_TAINTING: save_bool(&PL_tainting); return;
    case GOPERL_PL_PERLDB: save_int((int *)&PL_perldb); return;
    case GOPERL_PL_HINTS: save_I32((I32 *)&PL_hints); return;
    case GOPERL_PL_SUB_GENERATION: save_I32((I32 *)&PL_sub_generation); return;
    case GOPERL_PL_STATUSVALUE: save_I32((I32 *)&PL_statusvalue); return;
    case GOPERL_PL_COP_SEQMAX: save_I32((I32 *)&PL_cop_seqmax); return;
    case GOPERL_PL_SUBLINE: save_I32(&PL_subline); return;
    case GOPERL_PL_EXIT_FLAGS: save_I8((I8 *)&PL_exit_flags); return;
    case GOPERL_PL_LOCALIZING: save_I8((I8 *)&PL_localizing); return;
    case GOPERL_PL_DOWARN: save_I8((I8 *)&PL_dowarn); return;
    case GOPERL_PL_DESTRUCT_LEVEL: save_I8((I8 *)&PL_perl_destruct_level); return;
    case GOPERL_PL_SAVEBEGIN: save_bool(&PL_savebegin); return;
    case GOPERL_PL_SRAND_CALLED: save_bool(&PL_srand_called); return;
    case GOPERL_PL_CV_HAS_EVAL: save_bool(&PL_cv_has_eval); return;
    case GOPERL_PL_PAD_RESET_PENDING: save_bool(&PL_pad_reset_pending); return;
    case GOPERL_PL_PADIX: Perl_save_strlen(aTHX_ (STRLEN *)&PL_padix); return;
    case GOPERL_PL_CONSTPADIX: Perl_save_strlen(aTHX_ (STRLEN *)&PL_constpadix); return;
    case GOPERL_PL_COMPPAD_NAME_FILL: Perl_save_strlen(aTHX_ (STRLEN *)&PL_comppad_name_fill); return;
    case GOPERL_PL_MIN_INTRO_PENDING: Perl_save_strlen(aTHX_ (STRLEN *)&PL_min_intro_pending); return;
    case GOPERL_PL_MAX_INTRO_PENDING: Perl_save_strlen(aTHX_ (STRLEN *)&PL_max_intro_pending); return;
    case GOPERL_PL_PADIX_FLOOR: Perl_save_strlen(aTHX_ (STRLEN *)&PL_padix_floor); return;
    case GOPERL_PL_DEFSTASH: GOPERL_SAVE_PTRVAR(PL_defstash);
    case GOPERL_PL_ARGVOUTGV: GOPERL_SAVE_PTRVAR(PL_argvoutgv);
    case GOPERL_PL_GLOBALSTASH: GOPERL_SAVE_PTRVAR(PL_globalstash);
    case GOPERL_PL_STATNAME: GOPERL_SAVE_PTRVAR(PL_statname);
    case GOPERL_PL_OP_MASK: GOPERL_SAVE_PTRVAR(PL_op_mask);
    case GOPERL_PL_SIGNALS: save_I32((I32 *)&PL_signals); return;
    case GOPERL_PL_LASTSTATVAL: save_I32((I32 *)&PL_laststatval); return;
    case GOPERL_PL_LASTSTYPE: save_I16((I16 *)&PL_laststype); return;
    case GOPERL_PL_TMPS_FLOOR: Perl_save_strlen(aTHX_ (STRLEN *)&PL_tmps_floor); return;
    }
#undef GOPERL_SAVE_PTRVAR
    Perl_croak(aTHX_ "goperl: interpreter variable %d cannot be saved", id);
}

/* A save of a location in this memory: a pointer by the kind, an integer
 * by its width. */
static void goperl_save_mem(pTHX_ void *loc, int width, int kind) {
    switch (kind) {
    case GOPERL_SAVEK_SPTR: save_sptr((SV **)loc); return;
    case GOPERL_SAVEK_VPTR: save_vptr(loc); return;
    case GOPERL_SAVEK_GENERIC: save_generic_svref((SV **)loc); return;
    }
    switch (width) {
    case 1: save_I8((I8 *)loc); return;
    case 2: save_I16((I16 *)loc); return;
    case 4: save_I32((I32 *)loc); return;
    case 8: save_iv((IV *)loc); return;
    }
    Perl_croak(aTHX_ "goperl: a save of %d bytes", width);
}

/* The perl stack native code had, laid out on the interpreter's, and back
 * (INTERNAL RUNOPS): s = [number of marks][number of values], the marks
 * (offsets from the bottom of that stack), the values. The answer is the
 * address of [count][number of marks][array of the values], the marks:
 * what the ops left above where the native stack began. */
static uint64_t goperl_runops_with_stack(pTHX_ OP *start, const char *s, size_t len) {
    static std::vector<goperl_gword_t> answer;
    goperl_gword_t nmarks = 0, n = 0;
    if (s && len >= 16) {
        memcpy(&nmarks, s, 8);
        memcpy(&n, s + 8, 8);
    }
    if (nmarks > len / 8 || n > len / 8 || len < 16 + 8 * (size_t)(nmarks + n))
        Perl_croak(aTHX_ "goperl: short stack for a run of ops");
    const SSize_t base = PL_stack_sp - PL_stack_base;
    const I32 markbase = (I32)(PL_markstack_ptr - PL_markstack);
    dSP;
    EXTEND(SP, (SSize_t)n);
    for (goperl_gword_t i = 0; i < nmarks; i++) {
        goperl_gword_t m;
        memcpy(&m, s + 16 + 8 * i, 8);
        PL_stack_sp = PL_stack_base + base + (SSize_t)m;
        PUSHMARK(PL_stack_sp);
    }
    SP = PL_stack_base + base;
    for (goperl_gword_t i = 0; i < n; i++) {
        goperl_gword_t tok;
        memcpy(&tok, s + 16 + 8 * (size_t)(nmarks + i), 8);
        *++SP = (SV *)(uintptr_t)tok;
    }
    PUTBACK;
    PL_op = start;
    CALLRUNOPS(aTHX);
    const SSize_t top = PL_stack_sp - PL_stack_base;
    AV *values = newAV();
    sv_2mortal((SV *)values);
    for (SSize_t i = base + 1; i <= top; i++)
        av_push(values, SvREFCNT_inc(PL_stack_base[i]));
    answer.clear();
    answer.push_back((goperl_gword_t)(top > base ? top - base : 0));
    const I32 marks_now = (I32)(PL_markstack_ptr - PL_markstack);
    answer.push_back((goperl_gword_t)(marks_now > markbase ? marks_now - markbase : 0));
    answer.push_back(GOPERL_ADDR(values));
    for (I32 i = markbase + 1; i <= marks_now; i++)
        answer.push_back((goperl_gword_t)(int64_t)(PL_markstack[i] - base));
    PL_stack_sp = PL_stack_base + (base < top ? base : top);
    PL_markstack_ptr = PL_markstack + (markbase < marks_now ? markbase : marks_now);
    return (uint64_t)(uintptr_t)answer.data();
}

/* ---- what native code hands the interpreter -------------------------------
 * A number native code hands an operation as a scalar is read as the head of
 * an SV, and as an op, a handle or an entry it is read as one: a number that
 * is not one (an address of the host process, a word read out of some other
 * structure, a scalar freed since) can lead the operation outside the
 * interpreter's memory, a trap that leaves the interpreter where the
 * operation stopped. Such a number makes the operation die instead, with an
 * error that names it.
 *
 * A scalar is one of the immortals or a slot of an SV arena that is not on
 * the free list. perl adds an arena at the head of PL_sv_arenaroot's chain
 * and frees the arenas only when it destructs, so the arenas sorted by
 * address are kept here and brought up to date from the head of the chain
 * whenever it has moved: telling a scalar is a search of them, where perl
 * keeps nothing to find an arena by an address in it. */
extern "C" int perl_signal_wait_mask(int begin, uint64_t mask, uint64_t *old);

static std::vector<const SV *> g_sv_arenas;
static const SV *g_sv_arenas_head;
/* the arena the last scalar was found in: the scalars an operation is
 * handed one after another are mostly of one arena */
static const SV *g_sv_arena_last;

static void goperl_sv_arenas_sync(pTHX) {
    const SV *const head = PL_sv_arenaroot;
    std::vector<const SV *> fresh;
    const SV *a = head;
    for (; a && a != g_sv_arenas_head; a = (const SV *)SvANY(a))
        fresh.push_back(a);
    if (!a && g_sv_arenas_head) { /* a chain of other arenas */
        g_sv_arenas.clear();
        g_sv_arena_last = nullptr;
    }
    for (const SV *n : fresh)
        g_sv_arenas.insert(std::upper_bound(g_sv_arenas.begin(), g_sv_arenas.end(), n), n);
    g_sv_arenas_head = head;
}

/* Whether p is a slot of an SV arena (live or freed) or an immortal. */
static bool goperl_sv_head(pTHX_ const SV *p) {
    if ((const char *)p - (const char *)&PL_sv_immortals[0] <
            (ptrdiff_t)sizeof PL_sv_immortals &&
        p >= &PL_sv_immortals[0])
        return ((const char *)p - (const char *)&PL_sv_immortals[0]) % sizeof(SV) == 0;
    if (p == &PL_sv_placeholder) return true;
    if (PL_sv_arenaroot != g_sv_arenas_head) goperl_sv_arenas_sync(aTHX);
    const SV *arena = g_sv_arena_last;
    size_t off = (size_t)((const char *)p - (const char *)arena);
    if (!arena || off / sizeof(SV) >= SvREFCNT(arena)) {
        auto at = std::upper_bound(g_sv_arenas.begin(), g_sv_arenas.end(), p);
        if (at == g_sv_arenas.begin()) return false;
        arena = *--at;
        off = (size_t)((const char *)p - (const char *)arena);
        if (off / sizeof(SV) >= SvREFCNT(arena)) return false;
        g_sv_arena_last = arena;
    }
    return off != 0 && off % sizeof(SV) == 0;
}

static const char *goperl_op_name(int32_t op) {
    switch (op) {
#define N(name) case GOPERL_OP_##name: return #name;
    N(SV_IV) N(SV_PV) N(NEW_IV) N(NEW_PVN) N(SV_MORTAL) N(NEW_UV) N(NEW_AV)
    N(NEW_HV) N(NEW_RV_INC) N(AV_PUSH) N(AV_LEN) N(AV_FETCH) N(HV_STORE)
    N(HV_FETCH) N(REFCNT_INC) N(GV_STASHPV) N(SV_BLESS) N(SV_RV) N(SV_TYPE)
    N(SV_ISA) N(SV_DERIVED_FROM) N(SETREF_IV) N(SV_SETSV) N(SV_SETSV_NOMG)
    N(SV_SETSV_MG) N(SV_SETIV) N(SV_SETUV) N(SV_SETNV) N(SV_SETPVN) N(SV_NV)
    N(NEW_NV) N(NEW_SVSV) N(SV_MORTALCOPY) N(SV_CATSV) N(SV_CATSV_NOMG)
    N(SV_CATPVN) N(SV_TRUE) N(SV_INFO) N(SV_CUR_SET) N(SV_GROW) N(SV_EQ)
    N(SV_CMP) N(SV_RVWEAKEN) N(SV_2CV) N(GET_SV) N(GET_CV) N(GV_FETCH)
    N(AV_STORE) N(AV_EXTEND) N(AV_FILL) N(AV_READ) N(HV_FETCH_ENT)
    N(HV_STORE_ENT) N(HV_EXISTS_ENT) N(HE_VAL) N(HV_ITERINIT) N(HV_ITERNEXT)
    N(HV_ITERKEYSV) N(HV_ITERVAL) N(ENTER) N(LEAVE) N(SAVETMPS) N(FREETMPS)
    N(CALL_SV) N(CALL_METHOD) N(ERRSV) N(SAVE_OP) N(RUN_PP) N(MAGIC_ATTACH)
    N(MAGIC_ID) N(MAGIC_UNATTACH) N(PLVAR_GET) N(PLVAR_SET) N(SV_UTF8_ON)
    N(SV_POK_ON) N(SV_DUMP) N(SV_REFCNT_DEC) N(SV_UNMAGIC) N(SAVE_HOOK)
    N(NEW_XS) N(SV_STASH) N(NEW_SV) N(SV_UPGRADE) N(SV_UTF8_UPGRADE)
    N(SAVE_DELETE) N(SAVE_HELEM) N(NEW_PVN_SHARE) N(SV_LEN) N(NEW_HVHV)
    N(AV_MAKE) N(LOOKS_LIKE_NUMBER) N(SV_AMAGIC) N(AMAGIC_CALL) N(WARN)
    N(GET_HV) N(AV_STORE_RAW) N(PP_HOOK_SET) N(RUN_ORIGINAL)
    N(SAVE_DESTRUCTOR) N(OP_FIELDS) N(OP_PTR) N(COP_LINE) N(COP_FILE)
    N(COP_STASHPV) N(CV_INFO) N(CV_PTR) N(GV_PTR) N(GV_NAME) N(SV_ISGV_GP)
    N(HV_NAME) N(SI_GET) N(CX_FIELDS) N(CX_PTR) N(OP_NAME_STR) N(CV_FILE)
    N(GV_FETCHFILE) N(HV_CLEAR) N(HV_DELETE) N(AV_EXISTS) N(SV_UTF8_OFF)
    N(SAVE_SCALAR) N(SV_READONLY_ON) N(EVAL_PV) N(AV_UNSHIFT) N(NEW_CONSTSUB)
    N(SV_PVX_RAW) N(HV_PKG_GEN) N(SV_REFCNT) N(GV_INIT) N(GV_AMG)
    N(SV_AMAGIC_SET) N(SV_MAGIC_SET_HOOK) N(GV_FETCHMETHOD) N(SV_READONLY_OFF)
    N(SV_PV_FORCE) N(SV_CHOP) N(SV_INSERT) N(SV_UTF8_DECODE)
    N(SV_UTF8_DOWNGRADE) N(MG_SET) N(GIMME_V) N(CKWARN) N(SV_LEN_BUF)
    N(HV_STORE_KLEN) N(HV_FETCH_KLEN) N(HV_DELETE_ENT) N(AV_POP) N(COP_HINTS)
    N(SV_IS_BOOL) N(SV_POK_ONLY) N(EVAL_SV) N(AV_SHIFT) N(SV_2IO)
    N(SV_FORCE_NORMAL) N(PERLIO_OPEN) N(PERLIO_CLOSE) N(PERLIO_PUTS)
    N(PERLIO_FLUSH) N(PERLIO_WRITE) N(IO_OFP) N(SV_CUR) N(SV_MAGIC_STD)
    N(PERLIO_DEF_LAYER) N(PERLIO_NEXT_READ) N(PERLIO_NEXT_FASTGETS)
    N(PERLIO_NEXT_GETCNT) N(PERLIO_NEXT_GETPTR) N(PERLIO_NEXT_SETPTRCNT)
    N(PERLIO_NEXT_FILL) N(PERLIO_STATE) N(OPTREE_NEW) N(OPTREE_MISC) N(OP_SET)
    N(LEX) N(PARSE) N(PARSER_GET) N(PARSER_SET) N(BLOCK) N(PAD)
    N(KEYWORD_ENABLE) N(CHARCLASS) N(SAVE_MISC) N(SV_CLASSIFY) N(SV_FLAGS)
    N(SV_SLOT_SET) N(SV_CALL) N(GV_CALL) N(SV_STR) N(SV_SLOT_GET) N(CALLER_CX)
    N(PERLIO) N(MEM_FREE) N(MEM_REALLOC) N(DO_OPENN) N(FIELD) N(REGEXP)
    N(HOOK) N(PP_STACK) N(CX) N(OP_DUMP) N(CALL) N(TREE) N(INTERNAL)
#undef N
    }
    return "?";
}

static void goperl_arg_refuse(pTHX_ int32_t op, uint64_t v, const char *what)
    __attribute__((noreturn));
static void goperl_arg_refuse(pTHX_ int32_t op, uint64_t v, const char *what) {
    Perl_croak(aTHX_ "native code handed the interpreter 0x%" PRIx64 " as %s in the operation "
                     "%s (%d), and it is not one of this interpreter's",
               v, what, goperl_op_name(op), (int)op);
}

/* A scalar of the interpreter's that was freed already: native code kept
 * it past its last reference. */
static void goperl_arg_refuse_freed(pTHX_ int32_t op, uint64_t v) __attribute__((noreturn));
static void goperl_arg_refuse_freed(pTHX_ int32_t op, uint64_t v) {
    Perl_croak(aTHX_ "native code handed the interpreter 0x%" PRIx64 " as a scalar in the "
                     "operation %s (%d), and that scalar was freed already",
               v, goperl_op_name(op), (int)op);
}

/* &PL_sv_undef, &PL_sv_yes and &PL_sv_no as native code has them in code
 * that ran before it was booted (a C++ static initializer of the module:
 * GOPERL_STATIC_SV_UNDEF in the SDK), NULL for any other number. */
#define GOPERL_STATIC_SV_UNDEF 0x18u
#define GOPERL_STATIC_SV_YES 0x28u
#define GOPERL_STATIC_SV_NO 0x38u
static inline SV *goperl_static_immortal(pTHX_ uint64_t v) {
    switch (v) {
    case GOPERL_STATIC_SV_UNDEF: return &PL_sv_undef;
    case GOPERL_STATIC_SV_YES: return &PL_sv_yes;
    case GOPERL_STATIC_SV_NO: return &PL_sv_no;
    }
    return NULL;
}

/* The scalar a number native code handed an operation is, NULL for 0. A
 * freed scalar is refused unless the operation reads its head alone
 * (freed): a freed scalar has no flags left, and perl reads it as undef
 * there, as Tk reads one it has freed (its Tcl_SetVar returns the scalar
 * it frees, and asks SvROK and SvPOK of it). */
static inline SV *goperl_sv_arg(pTHX_ uint64_t v, int32_t op, bool freed = false) {
    SV *const sv = (SV *)(uintptr_t)v;
    if (!v) return sv;
    if (!goperl_sv_head(aTHX_ sv)) {
        if (SV *const imm = goperl_static_immortal(aTHX_ v)) return imm;
        goperl_arg_refuse(aTHX_ op, v, "a scalar");
    }
    if (!freed && SvTYPE(sv) == (svtype)SVTYPEMASK) goperl_arg_refuse_freed(aTHX_ op, v);
    return sv;
}

/* The structure of `size` bytes at an address native code handed an
 * operation, NULL for 0: one inside the interpreter's memory, on a word
 * boundary. */
static inline void *goperl_mem_arg(pTHX_ uint64_t v, size_t size, int32_t op, const char *what) {
    const uint64_t end = (uint64_t)__builtin_wasm_memory_size(0) << 16;
    if (v && (v % sizeof(void *) != 0 || v >= end || end - v < size))
        goperl_arg_refuse(aTHX_ op, v, what);
    return (void *)(uintptr_t)v;
}
#define GOPERL_SVARG(v) goperl_sv_arg(aTHX_ (v), op)
#define GOPERL_AVARG(v) ((AV *)goperl_sv_arg(aTHX_ (v), op))
#define GOPERL_HVARG(v) ((HV *)goperl_sv_arg(aTHX_ (v), op))
/* The hash of an operation perl's hv_common does (fetch, store, exists,
 * delete): a freed one too, for which hv_common answers NULL (SvIS_FREED),
 * as perl takes the freed hash of a module that tears its hashes down out
 * of order (Prima's font cache, whose faces' hash went first). */
#define GOPERL_HVARG_COMMON(v) ((HV *)goperl_sv_arg(aTHX_ (v), op, true))
#define GOPERL_CVARG(v) ((CV *)goperl_sv_arg(aTHX_ (v), op))
#define GOPERL_GVARG(v) ((GV *)goperl_sv_arg(aTHX_ (v), op))
#define GOPERL_IOARG(v) ((IO *)goperl_sv_arg(aTHX_ (v), op))
#define GOPERL_MEMARG(type, v) ((type *)goperl_mem_arg(aTHX_ (v), sizeof(type), op, "a " #type))
/* A value an operation only stores, into a slot of a container or of a
 * scalar, is not read there, by perl either: it passes as it is (a module
 * keeps structures of its own in an array, as Object::Pad does). */
#define GOPERL_STORED(v) goperl_stored(aTHX_ (v))
static inline SV *goperl_stored(pTHX_ uint64_t v) {
    SV *const imm = v < 0x40 ? goperl_static_immortal(aTHX_ v) : NULL;
    return imm ? imm : (SV *)(uintptr_t)v;
}

/* A value stored in a container that lies outside the interpreter's memory
 * (a structure the module allocated: Text::KnuthPlass keeps its list of
 * breakpoints in its object's hash) is one perl must not drop. perl drops
 * what a container holds when the container is freed and when a store
 * replaces it, and its SvREFCNT_dec reads the value's head: perl reads the
 * module's memory there and gets by, the interpreter reads outside its own
 * and traps. Such a value is taken out of the container first: the
 * container gets magic whose free takes them out before perl frees what it
 * holds (mg_free runs before the body is cleared), and a store over one
 * takes it out of the slot. Native code reads it back as it stored it. */
static bool goperl_foreign(const SV *v) {
    return v && (uint64_t)(uintptr_t)v >= ((uint64_t)__builtin_wasm_memory_size(0) << 16);
}
static int goperl_foreign_free(pTHX_ SV *sv, MAGIC *mg) {
    PERL_UNUSED_ARG(mg);
    if (SvTYPE(sv) == SVt_PVAV && AvARRAY((AV *)sv)) {
        SV **const a = AvARRAY((AV *)sv);
        for (SSize_t i = 0; i <= AvFILLp((AV *)sv); i++)
            if (goperl_foreign(a[i])) a[i] = NULL;
    } else if (SvTYPE(sv) == SVt_PVHV && HvARRAY((HV *)sv)) {
        for (STRLEN b = 0; b <= HvMAX((HV *)sv); b++)
            for (HE *he = HvARRAY((HV *)sv)[b]; he; he = HeNEXT(he))
                if (goperl_foreign(HeVAL(he))) HeVAL(he) = NULL;
    }
    return 0;
}
static MGVTBL goperl_foreign_vtbl = {NULL, NULL, NULL, NULL, goperl_foreign_free,
                                     NULL, NULL, NULL};
/* v is stored in the container: one outside the memory marks it. */
static SV *goperl_store_into(pTHX_ SV *container, SV *v) {
    /* a freed hash takes nothing (hv_common answers NULL for it) */
    if (goperl_foreign(v) && !SvIS_FREED(container) &&
        !mg_findext(container, PERL_MAGIC_ext, &goperl_foreign_vtbl))
        sv_magicext(container, NULL, PERL_MAGIC_ext, &goperl_foreign_vtbl, NULL, 0);
    return v;
}
/* Whether the container may hold such values: a store into it looks at
 * the slot it replaces first. */
static bool goperl_holds_foreign(SV *container) {
    return SvRMAGICAL(container) && mg_findext(container, PERL_MAGIC_ext, &goperl_foreign_vtbl);
}
/* The slot a store is about to replace: a value outside the memory in it
 * is taken out, for the store not to drop it. */
static void goperl_slot_unforeign(SV **slot) {
    if (slot && goperl_foreign(*slot)) *slot = NULL;
}

static uint64_t goperl_internal_op(pTHX_ int sel, uint64_t a, uint64_t b, uint64_t c,
                                   const char *s) {
    const int32_t op = GOPERL_OP_INTERNAL;
    /* a: the token or the address; c: the selector, and in its high half a
     * value that goes with an address in b */
    const uint64_t lo = a;
    const uint32_t c_hi = (uint32_t)(c >> 32);
    switch (sel) {
    case GOPERL_INT_LAYOUT: {
        static uint32_t w[32];
        if (!w[0]) {
            w[0] = (uint32_t)sizeof(PADNAME);
            w[1] = (uint32_t)STRUCT_OFFSET(PADNAME, xpadn_pv);
            w[2] = (uint32_t)STRUCT_OFFSET(PADNAME, xpadn_ourstash);
            w[3] = (uint32_t)STRUCT_OFFSET(PADNAME, xpadn_type_u);
            w[4] = (uint32_t)STRUCT_OFFSET(PADNAME, xpadn_fieldinfo);
            w[5] = (uint32_t)STRUCT_OFFSET(PADNAME, xpadn_low);
            w[6] = (uint32_t)STRUCT_OFFSET(PADNAME, xpadn_high);
            w[7] = (uint32_t)STRUCT_OFFSET(PADNAME, xpadn_refcnt);
            w[8] = (uint32_t)STRUCT_OFFSET(PADNAME, xpadn_gen);
            w[9] = (uint32_t)STRUCT_OFFSET(PADNAME, xpadn_len);
            w[10] = (uint32_t)STRUCT_OFFSET(PADNAME, xpadn_flags);
            w[11] = (uint32_t)STRUCT_OFFSET(PADNAMELIST, xpadnl_fill);
            w[12] = (uint32_t)STRUCT_OFFSET(PADNAMELIST, xpadnl_alloc);
            w[13] = (uint32_t)STRUCT_OFFSET(PADNAMELIST, xpadnl_max);
            w[14] = (uint32_t)STRUCT_OFFSET(PADNAMELIST, xpadnl_max_named);
            w[15] = (uint32_t)STRUCT_OFFSET(PADNAMELIST, xpadnl_refcnt);
            w[16] = (uint32_t)STRUCT_OFFSET(PADLIST, xpadl_max);
            w[17] = (uint32_t)STRUCT_OFFSET(PADLIST, xpadl_arr);
            w[18] = (uint32_t)STRUCT_OFFSET(PADLIST, xpadl_id);
            w[19] = (uint32_t)STRUCT_OFFSET(PADLIST, xpadl_outid);
            w[20] = (uint32_t)sizeof(SSize_t);
            w[21] = (uint32_t)sizeof(PADOFFSET);
            w[22] = (uint32_t)sizeof(void *);
            /* the entries of hashes and their keys, which native code
             * reads and writes where they lie: the loader compares a
             * module's layout of them with these */
            w[23] = (uint32_t)STRUCT_OFFSET(HE, hent_next);
            w[24] = (uint32_t)STRUCT_OFFSET(HE, hent_hek);
            w[25] = (uint32_t)STRUCT_OFFSET(HE, he_valu);
            w[26] = (uint32_t)sizeof(HE);
            w[27] = (uint32_t)STRUCT_OFFSET(HEK, hek_hash);
            w[28] = (uint32_t)STRUCT_OFFSET(HEK, hek_len);
            w[29] = (uint32_t)STRUCT_OFFSET(HEK, hek_key);
        }
        return (uint64_t)(uintptr_t)w;
    }
    case GOPERL_INT_SAVE_PLVAR:
        goperl_save_plvar(aTHX_ (int)lo, (int)b);
        return 0;
    case GOPERL_INT_HASH: {
        U32 hash;
        PERL_HASH(hash, s ? s : "", (STRLEN)b);
        return hash;
    }
    case GOPERL_INT_PUSHSTACK:
        /* Native code runs a callback on a stack of its own (a stackinfo
         * of the type it says): a loop control or a goto in the callback
         * finds no context of its caller's to leave to. The stack pointer
         * the interpreter has is the one to come back to; the native call
         * may run in the middle of an operation (a sub's return values lie
         * below it while the sub's scope ends). */
        push_stackinfo((I32)(int64_t)b, 0);
        return 0;
    case GOPERL_INT_POPSTACK:
        if (!PL_curstackinfo->si_prev)
            Perl_croak(aTHX_ "goperl: POPSTACK with no stack pushed");
        /* Back on the stack below, where it was left. pop_stackinfo sets
         * the stack pointer itself: no copy of the pointer taken before
         * (one into the stack popped) may be stored after it. */
        pop_stackinfo();
        return 0;
    case GOPERL_INT_WAITPID: {
        /* native code's waitpid, wait and wait4: the program's own, as
         * Perl's waitpid is (the C library of this interpreter knows the
         * processes its fork made and the commands it started). Nothing of
         * the interpreter's changes: not $?, and not errno, which native
         * code gets the failure in. The status word is the C library's,
         * with its numbers of signals, not the program's (the waitpid the
         * interpreter calls gives it the host's numbers): native code maps
         * it to the host's, as it maps errno. */
        const int saved = errno;
        int st = 0;
        const pid_t got = __real_waitpid((pid_t)(int64_t)lo, &st, (b & 1) ? WNOHANG : 0);
        const int failure = errno;
        errno = saved;
        if (got < 0) return goperl_two((uint64_t)(int64_t)-1, (uint64_t)failure);
        return goperl_two((uint64_t)(int64_t)got, (uint64_t)(uint32_t)st);
    }
    case GOPERL_INT_MY_EXIT:
        if (lo) my_failure_exit();
        my_exit((U32)b);
        return 0;
    case GOPERL_INT_SIGHANDLER:
        Perl_sighandler((int)(int64_t)b);
        return 0;
    case GOPERL_INT_WAIT_MASK: {
        /* the sigset_t of POSIX::SigSet, which native code hands its wait
         * as it is: one byte here, naming a set (perl_signal.cc) */
        uint64_t mask = b;
        if (c_hi & 1) {
            const sigset_t *set = (const sigset_t *)goperl_mem_arg(aTHX_ b, 1, GOPERL_OP_INTERNAL,
                                                                   "a sigset_t");
            mask = 0;
            for (int sig = 1; sig <= 64; sig++)
                if (sigismember(set, sig) == 1) mask |= (uint64_t)1 << (sig - 1);
        }
        uint64_t old = 0;
        const int ran = perl_signal_wait_mask(lo != 0, mask, &old);
        return goperl_two(old, (uint64_t)ran);
    }
    case GOPERL_INT_SIGACTION: {
        /* native code shares the process's signals with the program, whose
         * actions are its C library's: the action native code finds where
         * the runtime's handler is the process's */
        struct sigaction act;
        const int saved = errno;
        const int r = sigaction((int)(int64_t)b, NULL, &act);
        errno = saved;
        if (r != 0 || act.sa_handler == SIG_DFL) return 0;
        return act.sa_handler == SIG_IGN ? 1 : 2;
    }
    case GOPERL_INT_SAVE_SVREF:
        if (!lo) Perl_croak(aTHX_ "save_svref of no slot");
        return (uint64_t)(uintptr_t)save_svref((SV **)goperl_mem_arg(aTHX_ lo, sizeof(SV *), op,
                                                                     "a slot"));
    case GOPERL_INT_SCAN_VSTRING: {
        /* the spelling native code has, ending where it ends: scan_vstring
         * may look at the byte at the end */
        SV *sv = GOPERL_SVARG(lo);
        if (!sv) Perl_croak(aTHX_ "goperl: scan_vstring into no SV");
        std::string spelling(s ? s : "", s ? (size_t)b : 0);
        const char *begin = spelling.c_str();
        const char *end = Perl_scan_vstring(aTHX_ begin, begin + spelling.size(), sv);
        return (uint64_t)(end - begin);
    }
    case GOPERL_INT_SAVE_FIELD: {
        /* the fields are numbered as the host's lvalue slots of them */
        enum { GOPERL_SVF_IV = 0, GOPERL_SVF_UV = 1, GOPERL_SVF_NV = 2, GOPERL_SVF_RV = 6,
               GOPERL_SVF_GVSV = 7, GOPERL_SVF_GVAV = 8, GOPERL_SVF_GVHV = 9 };
        SV *sv = GOPERL_SVARG(lo);
        const int field = (int)(b >> 8), kind = (int)(b & 0xFF);
        void *loc = NULL;
        int width = (int)sizeof(void *);
        if (sv) {
            switch (field) {
            case GOPERL_SVF_GVSV: if (isGV_with_GP(sv)) loc = (void *)&GvSV((GV *)sv); break;
            case GOPERL_SVF_GVAV: if (isGV_with_GP(sv)) loc = (void *)&GvAV((GV *)sv); break;
            case GOPERL_SVF_GVHV: if (isGV_with_GP(sv)) loc = (void *)&GvHV((GV *)sv); break;
            case GOPERL_SVF_RV: loc = (void *)&SvRV(sv); break;
            case GOPERL_SVF_IV:
            case GOPERL_SVF_UV:
                if (SvTYPE(sv) == SVt_IV || SvTYPE(sv) >= SVt_PVIV) loc = (void *)&SvIVX(sv);
                width = (int)sizeof(IV);
                break;
            case GOPERL_SVF_NV:
                if (SvTYPE(sv) == SVt_NV || SvTYPE(sv) >= SVt_PVNV) loc = (void *)&SvNVX(sv);
                width = (int)sizeof(NV);
                break;
            }
        }
        if (!loc) Perl_croak(aTHX_ "goperl: a save of a field the scalar does not have");
        goperl_save_mem(aTHX_ loc, width, kind);
        return 0;
    }
    case GOPERL_INT_SAVE_MEM:
        goperl_save_mem(aTHX_ (void *)(uintptr_t)lo, (int)(b >> 8), (int)(b & 0xFF));
        return 0;
    case GOPERL_INT_SAVE_AVELEM: {
        AV *av = GOPERL_AVARG(lo);
        const SSize_t ix = (SSize_t)(b >> 8);
        if (!av || SvTYPE((SV *)av) != SVt_PVAV || ix < 0 || ix > AvMAX(av))
            Perl_croak(aTHX_ "goperl: a save of a slot an array does not have");
        goperl_save_mem(aTHX_ (void *)&AvARRAY(av)[ix], (int)sizeof(SV *), (int)(b & 0xFF));
        return 0;
    }
    case GOPERL_INT_LEAVE_SCOPE:
        if ((I32)(int64_t)b < PL_savestack_ix) leave_scope((I32)(int64_t)b);
        return 0;
    case GOPERL_INT_CV_GET:
    case GOPERL_INT_CV_SET: {
        CV *cv = GOPERL_CVARG(lo);
        if (!cv || (SvTYPE((SV *)cv) != SVt_PVCV && SvTYPE((SV *)cv) != SVt_PVFM))
            return 0;
        XPVCV *body = (XPVCV *)SvANY(cv);
        if (sel == GOPERL_INT_CV_GET) {
            switch ((int)b) {
            case GOPERL_CVF_FLAGS: return (uint64_t)CvFLAGS(cv);
            case GOPERL_CVF_ROOT: return CvISXSUB(cv) ? 0 : (uint64_t)(uintptr_t)CvROOT(cv);
            case GOPERL_CVF_START: return CvISXSUB(cv) ? 0 : (uint64_t)(uintptr_t)CvSTART(cv);
            case GOPERL_CVF_OUTSIDE_SEQ: return (uint64_t)CvOUTSIDE_SEQ(cv);
            case GOPERL_CVF_PADLIST: return CvISXSUB(cv) ? 0 : (uint64_t)(uintptr_t)CvPADLIST(cv);
            case GOPERL_CVF_DEPTH: return (uint64_t)(uint32_t)CvDEPTH(cv);
            case GOPERL_CVF_FILE: return (uint64_t)(uintptr_t)CvFILE(cv);
            case GOPERL_CVF_STASH: return (uint64_t)(uintptr_t)(SV *)body->xcv_stash;
            case GOPERL_CVF_GV_U: return (uint64_t)(uintptr_t)body->xcv_gv_u.xcv_gv;
            case GOPERL_CVF_OUTSIDE: return (uint64_t)(uintptr_t)(SV *)CvOUTSIDE(cv);
            }
            Perl_croak(aTHX_ "goperl: unknown field %d of a sub", (int)b);
        }
        /* b: the value, c_hi: the field */
        const uint64_t v = b;
        switch ((int)c_hi) {
        case GOPERL_CVF_FLAGS: CvFLAGS(cv) = (cv_flags_t)v; return 0;
        case GOPERL_CVF_ROOT:
            if (CvISXSUB(cv)) Perl_croak(aTHX_ "goperl: CvROOT of an XSUB");
            CvROOT(cv) = GOPERL_MEMARG(OP, v);
            return 0;
        case GOPERL_CVF_START:
            if (CvISXSUB(cv)) Perl_croak(aTHX_ "goperl: CvSTART of an XSUB");
            CvSTART(cv) = GOPERL_MEMARG(OP, v);
            return 0;
        case GOPERL_CVF_OUTSIDE_SEQ: CvOUTSIDE_SEQ(cv) = (U32)v; return 0;
        case GOPERL_CVF_PADLIST:
            if (CvISXSUB(cv)) Perl_croak(aTHX_ "goperl: CvPADLIST of an XSUB");
            CvPADLIST_set(cv, GOPERL_MEMARG(PADLIST, v));
            return 0;
        case GOPERL_CVF_DEPTH: CvDEPTH(cv) = (I32)(int64_t)v; return 0;
        case GOPERL_CVF_FILE: CvFILE(cv) = (char *)(uintptr_t)v; return 0;
        case GOPERL_CVF_STASH: body->xcv_stash = GOPERL_HVARG(v); return 0;
        case GOPERL_CVF_GV_U: body->xcv_gv_u.xcv_gv = GOPERL_GVARG(v); return 0;
        case GOPERL_CVF_OUTSIDE: CvOUTSIDE(cv) = GOPERL_CVARG(v); return 0;
        }
        Perl_croak(aTHX_ "goperl: unknown field %d of a sub", (int)c_hi);
    }
    case GOPERL_INT_PAD_NEW:
        return (uint64_t)(uintptr_t)pad_new((int)b);
    case GOPERL_INT_PADNAME_NEW:
        return (uint64_t)(uintptr_t)newPADNAMEpvn(s ? s : "", (STRLEN)b);
    case GOPERL_INT_PADNAME_FREE:
        if (lo) PadnameREFCNT_dec(GOPERL_MEMARG(PADNAME, lo));
        return 0;
    case GOPERL_INT_SAVE_FREEOP:
        if (lo) save_freeop(GOPERL_MEMARG(OP, lo));
        return 0;
    case GOPERL_INT_RUNOPS: {
        OP *start = GOPERL_MEMARG(OP, lo);
        if (b >> 63) { /* the stack is the interpreter's already */
            PL_op = start;
            CALLRUNOPS(aTHX);
            return 0;
        }
        return goperl_runops_with_stack(aTHX_ start, s, (size_t)b);
    }
    case GOPERL_INT_COP_LABEL: {
        STRLEN len = 0;
        U32 flags = 0;
        const char *p = lo ? CopLABEL_len_flags(GOPERL_MEMARG(COP, lo), &len, &flags) : NULL;
        if (!p) return 0;
        return goperl_two(GOPERL_ADDR(p), ((flags & SVf_UTF8) ? 0x80000000u : 0) |
                                              ((uint32_t)len & 0x7FFFFFFFu));
    }
    case GOPERL_INT_HINTHASH:
        return lo ? (uint64_t)(uintptr_t)CopHINTHASH_get(GOPERL_MEMARG(COP, lo)) : 0;
    case GOPERL_INT_HINTHASH_FETCH:
        return (uint64_t)(uintptr_t)Perl_refcounted_he_fetch_pvn(
            aTHX_ (const struct refcounted_he *)(uintptr_t)lo, s ? s : "",
            (STRLEN)(uint32_t)b, 0, (U32)(b >> 32));
    case GOPERL_INT_HINTHASH_2HV:
        return (uint64_t)(uintptr_t)(SV *)Perl_refcounted_he_chain_2hv(
            aTHX_ (const struct refcounted_he *)(uintptr_t)lo, (U32)b);
    case GOPERL_INT_OBJ_FIELDS:
    case GOPERL_INT_OBJ_MAXFIELD_SET:
    case GOPERL_INT_OBJ_FIELDS_NEW: {
        SV *obj = GOPERL_SVARG(lo);
        if (!obj || SvTYPE(obj) != SVt_PVOBJ)
            Perl_croak(aTHX_ "goperl: the fields of something that is not an object");
        if (sel == GOPERL_INT_OBJ_FIELDS) {
            const SSize_t n = ObjectMAXFIELD(obj) + 1;
            return goperl_two(GOPERL_ADDR(ObjectFIELDS(obj)), (uint64_t)(n > 0 ? n : 0));
        }
        if (sel == GOPERL_INT_OBJ_MAXFIELD_SET) {
            ObjectMAXFIELD(obj) = (SSize_t)(int64_t)b;
            return 0;
        }
        /* the array perl's sv_clear frees: its own memory */
        SV **fields = NULL;
        const SSize_t n = (SSize_t)(int64_t)b;
        if (n > 0) Newxz(fields, n, SV *);
        Safefree(ObjectFIELDS(obj));
        ObjectFIELDS(obj) = fields;
        ObjectMAXFIELD(obj) = n - 1;
        return (uint64_t)(uintptr_t)fields;
    }
    case GOPERL_INT_HV_ARRAY: {
        HV *hv = GOPERL_HVARG(lo);
        if (!hv || SvTYPE((SV *)hv) != SVt_PVHV || !HvARRAY(hv)) return 0;
        return goperl_two(GOPERL_ADDR(HvARRAY(hv)), (uint64_t)HvMAX(hv) + 1);
    }
    case GOPERL_INT_PERLIO_DEBUG:
        PerlIO_debug("%s", s ? s : "");
        return 0;
    case GOPERL_INT_SV_PV_FREE: {
        /* SvPV_free: the string buffer the SV owns is freed (the offset of a
         * buffer whose start was chopped off is undone first); the fields
         * stay as they are, for the caller to set */
        SV *sv = GOPERL_SVARG(lo);
        if (!sv || SvTYPE(sv) < SVt_PV) return 0;
        SvPV_free(sv);
        return 0;
    }
    case GOPERL_INT_STATCACHE: {
        /* what the last stat found, which -X _ reads: native code reads it
         * and may store what it found itself */
        static uint64_t w[GOPERL_STATCACHE_WORDS];
        if (b == 0) {
            w[0] = (uint64_t)PL_statcache.st_dev;
            w[1] = (uint64_t)PL_statcache.st_ino;
            w[2] = (uint64_t)PL_statcache.st_mode;
            w[3] = (uint64_t)PL_statcache.st_nlink;
            w[4] = (uint64_t)PL_statcache.st_uid;
            w[5] = (uint64_t)PL_statcache.st_gid;
            w[6] = (uint64_t)PL_statcache.st_rdev;
            w[7] = (uint64_t)(int64_t)PL_statcache.st_size;
            w[8] = (uint64_t)(int64_t)PL_statcache.st_atime;
            w[9] = (uint64_t)(int64_t)PL_statcache.st_mtime;
            w[10] = (uint64_t)(int64_t)PL_statcache.st_ctime;
            w[11] = (uint64_t)(int64_t)PL_statcache.st_blksize;
            w[12] = (uint64_t)(int64_t)PL_statcache.st_blocks;
            return (uint64_t)(uintptr_t)w;
        }
        if (!s) return 0;
        memcpy(w, s, sizeof w);
        PL_statcache.st_dev = (dev_t)w[0];
        PL_statcache.st_ino = (ino_t)w[1];
        PL_statcache.st_mode = (mode_t)w[2];
        PL_statcache.st_nlink = (nlink_t)w[3];
        PL_statcache.st_uid = (uid_t)w[4];
        PL_statcache.st_gid = (gid_t)w[5];
        PL_statcache.st_rdev = (dev_t)w[6];
        PL_statcache.st_size = (off_t)(int64_t)w[7];
        PL_statcache.st_atime = (time_t)(int64_t)w[8];
        PL_statcache.st_mtime = (time_t)(int64_t)w[9];
        PL_statcache.st_ctime = (time_t)(int64_t)w[10];
        PL_statcache.st_blksize = (blksize_t)(int64_t)w[11];
        PL_statcache.st_blocks = (blkcnt_t)(int64_t)w[12];
        return 0;
    }
    case GOPERL_INT_FILTER_ADD: {
        const uint32_t id = (uint32_t)b;
        if (id == 0 || id > goperl_filter_tramps.size())
            Perl_croak(aTHX_ "goperl: no source filter function %u", (unsigned)id);
        return (uint64_t)(uintptr_t)filter_add(goperl_filter_tramps[id - 1], GOPERL_SVARG(lo));
    }
    case GOPERL_INT_FILTER_DEL: {
        const uint32_t id = (uint32_t)b;
        if (id == 0 || id > goperl_filter_tramps.size()) return 0;
        filter_del(goperl_filter_tramps[id - 1]);
        return 0;
    }
    case GOPERL_INT_FILTER_READ:
        return (uint64_t)(uint32_t)filter_read((int)(int32_t)(uint32_t)(b >> 32),
                                               GOPERL_SVARG(lo), (int)(int32_t)(uint32_t)b);
    case GOPERL_INT_FILTER_ID: {
        SV *datasv = GOPERL_SVARG(lo);
        if (!datasv || SvTYPE(datasv) != SVt_PVIO) return 0;
        return goperl_filter_id(DPTR2FPTR(filter_t, IoANY(datasv)));
    }
    case GOPERL_INT_SV_GETS: /* b: the handle, c_hi: append */
        return sv_gets(GOPERL_SVARG(lo), GOPERL_MEMARG(PerlIO, b), (I32)(int32_t)c_hi) ? 1 : 0;
    case GOPERL_INT_BHK_REGISTER: {
        if (g_native_bhk_n >= GOPERL_BHK_MAX)
            Perl_croak(aTHX_ "goperl: too many block hooks of native code");
        const uint32_t k = g_native_bhk_n++;
        BHK *hk = &g_native_bhk[k];
        hk->bhk_flags = (U32)b;
        hk->bhk_start = goperl_bhk_tramps[k].start;
        hk->bhk_pre_end = goperl_bhk_tramps[k].pre_end;
        hk->bhk_post_end = goperl_bhk_tramps[k].post_end;
        hk->bhk_eval = goperl_bhk_tramps[k].eval;
        Perl_blockhook_register(aTHX_ hk);
        return k + 1;
    }
    case GOPERL_INT_BHK_FLAGS:
        if (lo == 0 || lo > g_native_bhk_n) return 0;
        g_native_bhk[lo - 1].bhk_flags = (U32)b;
        return 0;
    case GOPERL_INT_PARSER_NEW: {
        /* just enough of a parser for the compiler's functions, as a module
         * makes one when there is none; freed with the scope */
        goperl_gword_t w[2] = {NOLINE, NOLINE};
        if (s) memcpy(w, s, sizeof w);
        yy_parser *p;
        Newxz(p, 1, yy_parser);
        p->copline = (line_t)w[0];
        p->preambling = (line_t)w[1];
        PL_parser = p;
        SAVEFREEPV(p);
        return 0;
    }
    }
    Perl_croak(aTHX_ "goperl: unknown internal call %d", sel);
}

uint64_t perl_xs_opbox(uint64_t h) {
    return h ? GOPERL_ADDR(&g_opbox) : 0;
}

static uint64_t goperl_xs_run(int32_t op, uint64_t a, uint64_t b, uint64_t c,
                              const char *s, STRLEN slen);

uint32_t perl_xs_op(void) {
    /* the arguments are taken out first: what the operation leads to may
     * use the box for operations of its own */
    const int32_t op = g_opbox.op;
    const uint64_t a = g_opbox.a, b = g_opbox.b, c = g_opbox.c;
    uint32_t slen = g_opbox.slen;
    if (slen > GOPERL_OPBOX_STR) slen = GOPERL_OPBOX_STR;
    char s[GOPERL_OPBOX_STR + 1];
    memcpy(s, g_opbox.s, slen);
    s[slen] = '\0';
    g_opbox.result = g_my_perl ? goperl_xs_run(op, a, b, c, s, slen) : 0;
    return 0;
}

/* The string of an operation that comes this way (one longer than the box)
 * ends at its first NUL. */
uint64_t perl_xs_helper(uint64_t h, int32_t op, uint64_t a, uint64_t b, uint64_t c,
                        const char *s) {
    if (!g_my_perl || h == 0) return 0;
    return goperl_xs_run(op, a, b, c, s, s ? strlen(s) : 0);
}

/* An operation of native code; s is its string, slen bytes (with a NUL
 * after them), which a name may contain (Sub::Name gives subs names with
 * NULs in them). */
static uint64_t goperl_xs_run(int32_t op, uint64_t a, uint64_t b, uint64_t c,
                              const char *s, STRLEN slen) {
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    switch (op) {
    case GOPERL_OP_SV_IV: /* b bit 0: as an unsigned value; bit 1: no get magic */
        return goperl_sv_integer(aTHX_ GOPERL_SVARG(a), cBOOL(b & 1), cBOOL(b & 2));
    case GOPERL_OP_SV_PV: { /* -> the address of the string; second: its length */
        STRLEN len = 0;
        const char *p = SvPV(GOPERL_SVARG(a), len);
        return goperl_two(GOPERL_ADDR(p), (uint64_t)len);
    }
    case GOPERL_OP_NEW_IV:
        return (uint64_t)(uintptr_t)newSViv((IV)(int64_t)a);
    case GOPERL_OP_NEW_PVN:
        return (uint64_t)(uintptr_t)newSVpvn(s ? s : "", (STRLEN)b);
    case GOPERL_OP_SV_MORTAL:
        return (uint64_t)(uintptr_t)sv_2mortal(GOPERL_SVARG(a));
    case GOPERL_OP_NEW_UV:
        return (uint64_t)(uintptr_t)newSVuv((UV)a);
    case GOPERL_OP_NEW_AV:
        return (uint64_t)(uintptr_t)(SV *)newAV();
    case GOPERL_OP_NEW_HV:
        return (uint64_t)(uintptr_t)(SV *)newHV();
    case GOPERL_OP_NEW_RV_INC:
        return (uint64_t)(uintptr_t)newRV_inc(GOPERL_SVARG(a));
    case GOPERL_OP_AV_PUSH: { /* steals a ref, like the C API */
        AV *const av = GOPERL_AVARG(a);
        av_push(av, goperl_store_into(aTHX_ (SV *)av, GOPERL_STORED(b)));
        return 0;
    }
    case GOPERL_OP_AV_LEN:
        return (uint64_t)(int64_t)av_len(GOPERL_AVARG(a));
    case GOPERL_OP_AV_FETCH: { /* -> the element; second: the address of
                 * the slot av_fetch gave (native code's SV** is that slot) */
        SV **slot = av_fetch(GOPERL_AVARG(a), (SSize_t)(int64_t)b, FALSE);
        return goperl_two(slot ? (uint64_t)(uintptr_t)*slot : 0, GOPERL_ADDR(slot));
    }
    case GOPERL_OP_HV_STORE: { /* steals a ref */
        HV *const hv = GOPERL_HVARG_COMMON(a);
        const I32 klen = (I32)strlen(s ? s : "");
        if (goperl_holds_foreign((SV *)hv)) goperl_slot_unforeign(hv_fetch(hv, s ? s : "", klen, 0));
        hv_store(hv, s ? s : "", klen, goperl_store_into(aTHX_ (SV *)hv, GOPERL_STORED(b)), 0);
        return 0;
    }
    case GOPERL_OP_HV_FETCH: {
        SV **slot = hv_fetch(GOPERL_HVARG_COMMON(a), s ? s : "",
                             (I32)strlen(s ? s : ""), FALSE);
        return slot ? (uint64_t)(uintptr_t)*slot : 0;
    }
    case GOPERL_OP_REFCNT_INC: /* of a freed scalar too, as perl counts it (Tk's
                 * Tcl_IncrRefCount of the scalar its Tcl_SetVar freed) */
        return (uint64_t)(uintptr_t)SvREFCNT_inc(goperl_sv_arg(aTHX_ a, op, true));
    case GOPERL_OP_GV_STASHPV: /* a = flags, s = the name */
        return (uint64_t)(uintptr_t)gv_stashpvn(s ? s : "", (U32)slen, (I32)(int64_t)a);
    case GOPERL_OP_SV_BLESS:
        return (uint64_t)(uintptr_t)sv_bless(GOPERL_SVARG(a),
                                             GOPERL_HVARG(b));
    case GOPERL_OP_SV_RV: { /* SvRV token when SvROK, else 0 */
        SV *sv = goperl_sv_arg(aTHX_ a, op, true);
        return SvROK(sv) ? (uint64_t)(uintptr_t)SvRV(sv) : 0;
    }
    case GOPERL_OP_SV_TYPE: { /* the raw svtype enum value (the SDK pins the
              * same enum, so SVt_* comparisons agree across the boundary) */
        /* SvTYPE of something that is no scalar reads the byte where a
         * scalar's type would be, as perl's does: code asks it of what it
         * has not checked is a scalar (XML::Bare: SvTYPE(SvRV(*svp)) of a
         * string, whose SvRV is its buffer; Overload::FileCheck: of
         * `arg + AvMAX(arg)`), and goes on by the answer. Reading it
         * changes nothing; an address outside the memory is refused. */
        const SV *const sv = (const SV *)(uintptr_t)a;
        if (!a) return (uint64_t)SVt_NULL;
        if (goperl_sv_head(aTHX_ sv)) return (uint64_t)SvTYPE(sv);
        const uint64_t end = (uint64_t)__builtin_wasm_memory_size(0) << 16;
        if (a >= end || end - a < sizeof(SV)) goperl_arg_refuse(aTHX_ op, a, "a scalar");
        U32 flags;
        memcpy(&flags, (const char *)sv + offsetof(SV, sv_flags), sizeof flags);
        return (uint64_t)(flags & SVTYPEMASK);
    }
    case GOPERL_OP_SV_ISA:
        return sv_isa(GOPERL_SVARG(a), s ? s : "") ? 1 : 0;
    case GOPERL_OP_SV_DERIVED_FROM:
        return sv_derived_from(GOPERL_SVARG(a), s ? s : "") ? 1 : 0;
    case GOPERL_OP_SETREF_IV: /* bless rv `a` as class s pointing at IV b */
    {
        SV *rv = GOPERL_SVARG(a);
        sv_setiv(newSVrv(rv, s ? s : ""), (IV)(int64_t)b);
        return (uint64_t)(uintptr_t)rv;
    }

    /* ---- v3: the surface interpreter-heavy XS (Text::Xslate class)
     * compiles against. Conventions as above: a, b and c are u64 operands,
     * a token (an address) always one of its own; s+b carry strings or
     * strings of words. */
    case GOPERL_OP_SV_SETSV:
        sv_setsv(GOPERL_SVARG(a), GOPERL_SVARG(b));
        return 0;
    case GOPERL_OP_SV_SETSV_NOMG:
        sv_setsv_nomg(GOPERL_SVARG(a), GOPERL_SVARG(b));
        return 0;
    case GOPERL_OP_SV_SETSV_MG:
        sv_setsv_mg(GOPERL_SVARG(a), GOPERL_SVARG(b));
        return 0;
    case GOPERL_OP_SV_SETIV:
        sv_setiv(GOPERL_SVARG(a), (IV)(int64_t)b);
        return 0;
    case GOPERL_OP_SV_SETUV:
        sv_setuv(GOPERL_SVARG(a), (UV)b);
        return 0;
    case GOPERL_OP_SV_SETNV: { /* b = IEEE bits */
        union { uint64_t u; double d; } cvt;
        cvt.u = b;
        sv_setnv(GOPERL_SVARG(a), (NV)cvt.d);
        return 0;
    }
    case GOPERL_OP_SV_SETPVN:
        sv_setpvn(GOPERL_SVARG(a), s ? s : "", (STRLEN)b);
        return 0;
    case GOPERL_OP_SV_NV: { /* -> IEEE bits */
        union { uint64_t u; double d; } cvt;
        cvt.d = (double)SvNV(GOPERL_SVARG(a));
        return cvt.u;
    }
    case GOPERL_OP_NEW_NV: { /* a = IEEE bits */
        union { uint64_t u; double d; } cvt;
        cvt.u = a;
        return (uint64_t)(uintptr_t)newSVnv((NV)cvt.d);
    }
    case GOPERL_OP_NEW_SVSV:
        return (uint64_t)(uintptr_t)newSVsv(GOPERL_SVARG(a));
    case GOPERL_OP_SV_MORTALCOPY:
        return (uint64_t)(uintptr_t)sv_mortalcopy(GOPERL_SVARG(a));
    case GOPERL_OP_SV_CATSV:
        sv_catsv(GOPERL_SVARG(a), GOPERL_SVARG(b));
        return 0;
    case GOPERL_OP_SV_CATSV_NOMG:
        sv_catsv_nomg(GOPERL_SVARG(a), GOPERL_SVARG(b));
        return 0;
    case GOPERL_OP_SV_CATPVN:
        sv_catpvn(GOPERL_SVARG(a), s ? s : "", (STRLEN)b);
        return 0;
    case GOPERL_OP_SV_TRUE:
        return SvTRUE(GOPERL_SVARG(a)) ? 1 : 0;
    case GOPERL_OP_SV_INFO: { /* flag bitset (mirrored by the SDK's synthetic
                * SVf_ / GOPERL_INFO_ constants) */
        SV *sv = goperl_sv_arg(aTHX_ a, op, true);
        uint64_t f = 0;
        if (SvOK(sv)) f |= GOPERL_INFO_OK;
        if (SvROK(sv)) f |= GOPERL_INFO_ROK;
        if (SvPOK(sv)) f |= GOPERL_INFO_POK;
        if (SvNIOK(sv)) f |= GOPERL_INFO_NIOK;
        if (SvUTF8(sv)) f |= GOPERL_INFO_UTF8;
        /* Not sv_isobject, which runs the get magic of sv first: this is
         * a look at the flags, what SvOK and SvPOK are, and a module that
         * has fetched a tied value once must not fetch it again by asking.
         * (The SDK's sv_isobject runs the magic itself.) */
        if (SvROK(sv) && SvOBJECT(SvRV(sv))) f |= GOPERL_INFO_ISOBJ;
        if (SvREADONLY(sv)) f |= GOPERL_INFO_READONLY;
        if (SvTYPE(sv) == SVt_PVCV) f |= GOPERL_INFO_ISCV;
        if (SvTYPE(sv) == SVt_PVGV) f |= GOPERL_INFO_ISGV;
        if (SvIOK(sv)) f |= GOPERL_INFO_IOK;
        if (SvNOK(sv)) f |= GOPERL_INFO_NOK;
        if (SvPOKp(sv)) f |= GOPERL_INFO_POKp;
        if (SvNIOKp(sv)) f |= GOPERL_INFO_NIOKp;
        /* the flag itself, as SvIsUV is: an integer too large for an IV
         * that perl kept only privately (SvIOKp, a string above UV_MAX
         * numified) has it without SvIOK */
        if (SvIsUV(sv)) f |= GOPERL_INFO_ISUV;
        if (SvRMAGICAL(sv)) f |= GOPERL_INFO_RMAGICAL;
        if (SvOBJECT(sv)) f |= GOPERL_INFO_OBJECT;
        if (SvAMAGIC(sv)) f |= 65536;           /* GOPERL_INFO_AMAGIC */
        if (SvIOKp(sv)) f |= 131072;            /* GOPERL_INFO_IOKp */
        if (SvNOKp(sv)) f |= 262144;            /* GOPERL_INFO_NOKp */
        return f;
    }
    case GOPERL_OP_SV_CUR_SET: {
        SV *sv = GOPERL_SVARG(a);
        if (SvTYPE(sv) >= SVt_PV && goperl_pv_host_string(aTHX_ sv, SvFLAGS(sv), SvPVX(sv), (STRLEN)b))
            goperl_pv_host_refuse(aTHX);
        SvCUR_set(sv, (STRLEN)b);
        return 0;
    }
    case GOPERL_OP_SV_GROW: { /* (c: sv_grow, not SvGROW) -> new PVX
                 * (linear-memory offset) */
        SV *const sv = GOPERL_SVARG(a);
        char *p = c ? sv_grow(sv, (STRLEN)b) : SvGROW(sv, (STRLEN)b);
        return GOPERL_ADDR(p);
    }
    case GOPERL_OP_SV_EQ:
        return sv_eq(GOPERL_SVARG(a), GOPERL_SVARG(b)) ? 1 : 0;
    case GOPERL_OP_SV_CMP:
        return (uint64_t)(int64_t)sv_cmp(GOPERL_SVARG(a), GOPERL_SVARG(b));
    case GOPERL_OP_SV_RVWEAKEN:
        sv_rvweaken(GOPERL_SVARG(a));
        return 0;
    case GOPERL_OP_SV_2CV: { /* b = lref; c = 1: -> the address of [the cv]
                 * [the glob][the stash], what sv_2cv answers in its
                 * out-parameters too (a glob of a sub that is not defined
                 * gives no cv and the glob: Devel::NYTProf names the sub of
                 * a call that will die so); else the cv */
        HV *stash = NULL;
        GV *gv = NULL;
        CV *cv = sv_2cv(GOPERL_SVARG(a), &stash, &gv, (I32)(int64_t)b);
        if (!c) return (uint64_t)(uintptr_t)cv;
        static goperl_gword_t answer[3];
        answer[0] = GOPERL_ADDR(cv);
        answer[1] = GOPERL_ADDR(gv);
        answer[2] = GOPERL_ADDR(stash);
        return GOPERL_ADDR(answer);
    }
    case GOPERL_OP_GET_SV:
        return (uint64_t)(uintptr_t)get_sv(s ? s : "", (I32)(int64_t)a);
    case GOPERL_OP_GET_CV:
        return (uint64_t)(uintptr_t)get_cv(s ? s : "", (I32)(int64_t)a);
    case GOPERL_OP_GV_FETCH: /* a = flags, b = svtype */
        return (uint64_t)(uintptr_t)gv_fetchpv(s ? s : "", (I32)(int64_t)a,
                                               (svtype)(int64_t)b);
    case GOPERL_OP_AV_STORE: { /* b = sv, c = idx; takes ownership like the C API */
        AV *const av = GOPERL_AVARG(a);
        if (goperl_holds_foreign((SV *)av)) goperl_slot_unforeign(av_fetch(av, (SSize_t)(int64_t)c, 0));
        av_store(av, (SSize_t)(int64_t)c, goperl_store_into(aTHX_ (SV *)av, GOPERL_STORED(b)));
        return 0;
    }
    case GOPERL_OP_AV_EXTEND:
        av_extend(GOPERL_AVARG(a), (SSize_t)(int64_t)b);
        return 0;
    case GOPERL_OP_AV_FILL:
        av_fill(GOPERL_AVARG(a), (SSize_t)(int64_t)b);
        return 0;
    case GOPERL_OP_AV_READ: { /* like AV_FETCH but lval-safe (never NULL slot) */
        SV **slot = av_fetch(GOPERL_AVARG(a), (SSize_t)(int64_t)b, TRUE);
        return goperl_two(slot ? (uint64_t)(uintptr_t)*slot : 0, GOPERL_ADDR(slot));
    }
    case GOPERL_OP_HV_FETCH_ENT: { /* (b = keysv, c: bit 0 lval, bit 1 an
                 * entry made by the fetch has no value, HV_FETCH_EMPTY_HE)
                 * -> HE token */
        if (c & 2)
            return (uint64_t)(uintptr_t)hv_common(GOPERL_HVARG_COMMON(a), GOPERL_SVARG(b), NULL, 0, 0,
                                                  HV_FETCH_LVALUE | HV_FETCH_EMPTY_HE, NULL, 0);
        HE *he = hv_fetch_ent(GOPERL_HVARG_COMMON(a), GOPERL_SVARG(b), c ? TRUE : FALSE, 0U);
        return (uint64_t)(uintptr_t)he;
    }
    case GOPERL_OP_HV_STORE_ENT: { /* b = keysv, c = sv; takes value
                 * ownership -> the HE token (callers treat NULL as failure
                 * and roll their reference back) */
        HV *const hv = GOPERL_HVARG_COMMON(a);
        SV *const key = GOPERL_SVARG(b);
        if (goperl_holds_foreign((SV *)hv)) {
            HE *const was = hv_fetch_ent(hv, key, 0, 0U);
            if (was) goperl_slot_unforeign(&HeVAL(was));
        }
        HE *he = hv_store_ent(hv, key, goperl_store_into(aTHX_ (SV *)hv, GOPERL_STORED(c)), 0U);
        return (uint64_t)(uintptr_t)he;
    }
    case GOPERL_OP_HV_EXISTS_ENT: /* b = keysv; c = more of hv_common's action
                 * (HV_DISABLE_UVAR_XKEY: no uvar magic of the hash is asked,
                 * as English::Name's own lookups ask it not to be) */
        return hv_common(GOPERL_HVARG_COMMON(a), GOPERL_SVARG(b), NULL, 0, 0,
                         HV_FETCH_ISEXISTS | ((int)c & HV_DISABLE_UVAR_XKEY), NULL, 0)
                   ? 1
                   : 0;
    case GOPERL_OP_HE_VAL:
        return (uint64_t)(uintptr_t)HeVAL(GOPERL_MEMARG(HE, a));
    case GOPERL_OP_HV_ITERINIT:
        return (uint64_t)(int64_t)hv_iterinit(GOPERL_HVARG(a));
    case GOPERL_OP_HV_ITERNEXT: /* b = the flags of hv_iternext_flags */
        return (uint64_t)(uintptr_t)hv_iternext_flags(GOPERL_HVARG(a), (I32)(int64_t)b);
    case GOPERL_OP_HV_ITERKEYSV:
        return (uint64_t)(uintptr_t)hv_iterkeysv(GOPERL_MEMARG(HE, a));
    case GOPERL_OP_HV_ITERVAL: /* a = hv, b = he */
        return (uint64_t)(uintptr_t)hv_iterval(GOPERL_HVARG(a),
                                               GOPERL_MEMARG(HE, b));
    case GOPERL_OP_ENTER: ENTER; return 0;
    case GOPERL_OP_LEAVE: LEAVE; return 0;
    case GOPERL_OP_SAVETMPS: SAVETMPS; return 0;
    case GOPERL_OP_FREETMPS: FREETMPS; return 0;
    case GOPERL_OP_CALL_SV:   /* a = sv, c = flags; s+b = the arguments, a word each */
    case GOPERL_OP_CALL_METHOD: { /* c = flags; s = "name\0" + the arguments */
        I32 flags = (I32)(int64_t)c;
        if (op == GOPERL_OP_CALL_SV && !GOPERL_SVARG(a))
            goperl_arg_refuse(aTHX_ op, a, "the sub to call");
        const char *args = s;
        size_t args_len = (size_t)b;
        const char *method = NULL;
        if (op == 66) {
            method = s ? s : "";
            size_t nl = strlen(method) + 1;
            args = s + nl;
            args_len = args_len >= nl ? args_len - nl : 0;
        }
        size_t n = args_len / 8;
        dSP;
        PUSHMARK(SP);
        EXTEND(SP, (SSize_t)n);
        for (size_t i = 0; i < n; i++) {
            goperl_gword_t tok;
            memcpy(&tok, args + i * 8, 8);
            /* a null one too, as perl pushes it: the sub called finds that
             * element of @_ missing (BEGIN::Lift calls with the null of a
             * block it could not make) */
            PUSHs(GOPERL_SVARG(tok));
        }
        PUTBACK;
        /* The flags are the caller's, as they are (G_SCALAR/G_LIST/G_VOID,
         * G_DISCARD, G_EVAL, ...): wantarray in the callee is honest. A
         * caller that asked for G_EVAL reads $@ itself, as perl's call_sv
         * leaves it to (the truth of $@ is not asked here, which would run
         * the bool overloading of an exception object).
         *
         * Without G_EVAL, a die in the callee unwinds as in perl: to the
         * interpreter's catch point beyond the native call, $SIG{__DIE__}
         * run once, no eval of the bridge's in caller(), the exception
         * whole. It leaves this operation by the jump a die inside any
         * operation makes (the host ends the native call and the
         * interpreter goes on unwinding when it has returned: the SDK's
         * GOPERL_FAILED_JUMP). */
        const I32 count = op == 66 ? call_method(method, flags)
                                   : call_sv(GOPERL_SVARG(a), flags);
        SPAGAIN;
        /* The results as perl leaves them on the stack for the caller: no
         * reference is taken on them, and the array that hands them over
         * does not own them. A sub called for its effect alone can leave
         * values that are gone by now (Future::XS's callbacks end with a
         * bare return, after the scope of the call freed what they had
         * pushed), and perl leaves them alone; taking a reference on one
         * and dropping it with the array freed it a second time.
         *
         * They go over as words (goperl_call_results), not in an array of
         * the interpreter's: call_sv leaves no temporary of its own behind,
         * and a mortal array was one more for every call made in one scope
         * (HAMT's each, which calls back for each element). */
        const uint64_t words = goperl_call_results(aTHX_ count);
        /* -> the address of the results, a word each; second: the count */
        return goperl_two(words, (uint32_t)count);
    }
    case GOPERL_OP_ERRSV:
        return (uint64_t)(uintptr_t)ERRSV;
    case GOPERL_OP_SAVE_OP: /* scope-save the current PL_op (the SAVEOP() macro) */
        save_op();
        return 0;
    case GOPERL_OP_RUN_PP: {
        /* a = op_private<<40 | op_flags<<32 | op_type;
         * s = words: [marks][op_targ][an offset for each mark][the SVs ...],
         * b = its length in bytes.
         *
         * The pp function of an op type, run for an op native code made
         * itself and called through PL_ppaddr (`BINOP myop; ... PL_op =
         * (OP *)&myop; PL_ppaddr[myop.op_type](aTHX)`). It finds what the
         * native function has on its stack: every value, and a mark above
         * as many of them as each offset says. It takes what it takes of
         * both, as any pp function does, and the answer is what is on the
         * stack afterwards and how many of the marks are gone: an array of
         * the count values; second: taken<<32 | count.
         *
         * The op is the type, the flags, the private flags and the target
         * of the module's op and nothing else: no children, no other op. */
        const uint32_t op_type = (uint32_t)a;
        if (op_type >= OP_max || b < 16) return 0;
        goperl_gword_t nmarks, targ;
        memcpy(&nmarks, s, 8);
        memcpy(&targ, s + 8, 8);
        if (nmarks > b / 8 || (uint64_t)16 + (uint64_t)nmarks * 8 > b) return 0;
        const char *const mark_offs = s + 16;
        const char *const toks = mark_offs + (size_t)nmarks * 8;
        const size_t n = ((size_t)b - 16 - (size_t)nmarks * 8) / 8;
        const SSize_t base = PL_stack_sp - PL_stack_base;
        const SSize_t marks_before = PL_markstack_ptr - PL_markstack;
        dSP;
        EXTEND(SP, (SSize_t)n);
        goperl_gword_t next_mark = 0;
        for (size_t i = 0; i <= n; i++) {
            /* the marks above i values */
            while (next_mark < nmarks) {
                goperl_gword_t off;
                memcpy(&off, mark_offs + (size_t)next_mark * 8, 8);
                if (off > i) break;
                PUSHMARK(SP);
                next_mark++;
            }
            if (i == n) break;
            goperl_gword_t tok;
            memcpy(&tok, toks + i * 8, 8);
            PUSHs(GOPERL_SVARG(tok));
        }
        PUTBACK;
        union {
            OP op;
            UNOP unop;
            BINOP binop;
            LOGOP logop;
            LISTOP listop;
            PMOP pmop;
            SVOP svop;
            PADOP padop;
            PVOP pvop;
            LOOP loop;
            UNOP_AUX unop_aux;
            METHOP methop;
        } scratch;
        memset(&scratch, 0, sizeof scratch);
        scratch.op.op_type = op_type;
        scratch.op.op_flags = (U8)(a >> 32);
        scratch.op.op_private = (U8)(a >> 40);
        scratch.op.op_targ = (PADOFFSET)targ;
        scratch.op.op_ppaddr = PL_ppaddr[op_type];
        OP *const saved = PL_op;
        PL_op = &scratch.op;
        scratch.op.op_ppaddr(aTHX);
        PL_op = saved;
        SSize_t taken = marks_before + (SSize_t)next_mark - (PL_markstack_ptr - PL_markstack);
        if (taken < 0) taken = 0;
        if (taken > (SSize_t)next_mark) taken = (SSize_t)next_mark;
        PL_markstack_ptr = PL_markstack + marks_before;
        SSize_t count = (PL_stack_sp - PL_stack_base) - base;
        if (count < 0) count = 0;
        AV *const res = newAV();
        for (SSize_t i = 0; i < count; i++)
            av_store(res, i, SvREFCNT_inc(PL_stack_base[base + 1 + i]));
        PL_stack_sp = PL_stack_base + base;
        sv_2mortal((SV *)res);
        return goperl_two(GOPERL_ADDR(res), ((uint64_t)(uint32_t)taken << 32) | (uint32_t)count);
    }
    case GOPERL_OP_MAGIC_ATTACH: { /* b = obj token, c = host id */
        SV *obj = GOPERL_SVARG(b);
        MAGIC *mg = sv_magicext(GOPERL_SVARG(a), obj, PERL_MAGIC_ext,
                                goperl_anchor_vtbl(0), NULL, 0);
        mg->mg_private = 0;
        mg->mg_ptr = (char *)(uintptr_t)c;
        mg->mg_flags |= MGf_LOCAL; /* see goperl_host_mg_local */
        return 0;
    }
    case GOPERL_OP_MAGIC_ID: {
        SV *sv = GOPERL_SVARG(a);
        if (SvTYPE(sv) < SVt_PVMG) return 0;
        for (MAGIC *mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic)
            if (goperl_is_anchor(mg))
                return GOPERL_ADDR(mg->mg_ptr);
        return 0;
    }
    case GOPERL_OP_MAGIC_UNATTACH: {
        MAGIC *mg = goperl_anchor_of(aTHX_ GOPERL_SVARG(a));
        if (mg) sv_unmagicext(GOPERL_SVARG(a), PERL_MAGIC_ext, mg->mg_virtual);
        return 0;
    }
    case GOPERL_OP_PLVAR_GET: {
        switch ((int)a) {
        case GOPERL_PL_DIEHOOK: return (uint64_t)(uintptr_t)PL_diehook;
        case GOPERL_PL_WARNHOOK: return (uint64_t)(uintptr_t)PL_warnhook;
        case GOPERL_PL_SV_UNDEF: return (uint64_t)(uintptr_t)&PL_sv_undef;
        case GOPERL_PL_SV_YES: return (uint64_t)(uintptr_t)&PL_sv_yes;
        case GOPERL_PL_SV_NO: return (uint64_t)(uintptr_t)&PL_sv_no;
        case GOPERL_PL_CURCOP: return (uint64_t)(uintptr_t)PL_curcop;
        case GOPERL_PL_OP: /* second: what the op is now */
            return goperl_two((uint64_t)(uintptr_t)PL_op, goperl_op_fields(PL_op));
        case GOPERL_PL_PERLDB: return (uint64_t)(int64_t)PL_perldb;
        case GOPERL_PL_DBSUB: return (uint64_t)(uintptr_t)PL_DBsub;
        case GOPERL_PL_DBSINGLE: return (uint64_t)(uintptr_t)PL_DBsingle;
        case GOPERL_PL_ENDAV: return (uint64_t)(uintptr_t)(SV *)PL_endav;
        case GOPERL_PL_CHECKAV: return (uint64_t)(uintptr_t)(SV *)PL_checkav;
        case GOPERL_PL_INITAV: return (uint64_t)(uintptr_t)(SV *)PL_initav;
        case GOPERL_PL_MAIN_CV: return (uint64_t)(uintptr_t)(SV *)PL_main_cv;
        case GOPERL_PL_DEBSTASH: return (uint64_t)(uintptr_t)(SV *)PL_debstash;
        case GOPERL_PL_SAWAMPERSAND: return (uint64_t)PL_sawampersand;
        case GOPERL_PL_SCOPESTACK_IX: return (uint64_t)(int64_t)PL_scopestack_ix;
        case GOPERL_PL_EXIT_FLAGS: return (uint64_t)PL_exit_flags;
        case GOPERL_PL_BASETIME: return (uint64_t)(int64_t)PL_basetime;
        case GOPERL_PL_MODGLOBAL: return (uint64_t)(uintptr_t)(SV *)PL_modglobal;
        case GOPERL_PL_MINUS_C: return PL_minus_c ? 1 : 0;
        case GOPERL_PL_DEFGV: return (uint64_t)(uintptr_t)PL_defgv;
        case GOPERL_PL_HINTGV: return (uint64_t)(uintptr_t)PL_hintgv;
        case GOPERL_PL_COMPCV: return (uint64_t)(uintptr_t)PL_compcv;
        case GOPERL_PL_CURSTASH: return (uint64_t)(uintptr_t)(SV *)PL_curstash;
        case GOPERL_PL_DOWARN: return (uint64_t)PL_dowarn;
        case GOPERL_PL_HINTS: return (uint64_t)PL_hints;
        case GOPERL_PL_SUB_GENERATION: return (uint64_t)PL_sub_generation;
        case GOPERL_PL_ERRGV: return (uint64_t)(uintptr_t)PL_errgv;
        case GOPERL_PL_ERRSV: return (uint64_t)(uintptr_t)ERRSV;
        case GOPERL_PL_DEFSV: return (uint64_t)(uintptr_t)DEFSV;
        case GOPERL_PL_ERRNO: return (uint64_t)(int64_t)errno;
        case GOPERL_PL_STATUSVALUE: return (uint64_t)(int64_t)PL_statusvalue;
        case GOPERL_PL_OP_FREED:
            return (uint64_t)(uintptr_t)&g_ops_freed_count;
        case GOPERL_PL_COMPILING: return (uint64_t)(uintptr_t)(OP *)&PL_compiling;
        case GOPERL_PL_WARN_ALL: return (uint64_t)(uintptr_t)pWARN_ALL;
        case GOPERL_PL_WARN_NONE: return (uint64_t)(uintptr_t)pWARN_NONE;
        case GOPERL_PL_ERRORS: return (uint64_t)(uintptr_t)PL_errors;
        case GOPERL_PL_SCOPESTACK: return (uint64_t)(uintptr_t)PL_scopestack;
        case GOPERL_PL_SAVEBEGIN: return PL_savebegin ? 1 : 0;
        case GOPERL_PL_CURPAD: return (uint64_t)(uintptr_t)PL_curpad;
        case GOPERL_PL_COMPPAD: return (uint64_t)(uintptr_t)(SV *)PL_comppad;
        case GOPERL_PL_COMPPAD_NAME: return (uint64_t)(uintptr_t)PL_comppad_name;
        case GOPERL_PL_CURPM: return (uint64_t)(uintptr_t)(OP *)PL_curpm;
        case GOPERL_PL_RESTARTOP: return (uint64_t)(uintptr_t)PL_restartop;
        case GOPERL_PL_MARKSTACK_IX: return (uint64_t)(int64_t)(PL_markstack_ptr - PL_markstack);
        case GOPERL_PL_STACK_IX: return (uint64_t)(int64_t)(PL_stack_sp - PL_stack_base);
        case GOPERL_PL_MARKSTACK: return (uint64_t)(uintptr_t)PL_markstack;
        case GOPERL_PL_SRAND_CALLED: return PL_srand_called ? 1 : 0;
        case GOPERL_PL_SV_COUNT: return (uint64_t)PL_sv_count;
        case GOPERL_PL_SIG_PENDING: return (uint64_t)(int64_t)PL_sig_pending;
        case GOPERL_PL_PID: /* this process, which may be a fork child */
            return (uint64_t)(int64_t)PerlProc_getpid();
        case GOPERL_PL_DEFSTASH: return (uint64_t)(uintptr_t)(SV *)PL_defstash;
        case GOPERL_PL_INCGV: return (uint64_t)(uintptr_t)PL_incgv;
        case GOPERL_PL_STDERRGV: return (uint64_t)(uintptr_t)PL_stderrgv;
        case GOPERL_PL_DEFOUTGV: return (uint64_t)(uintptr_t)PL_defoutgv;
        case GOPERL_PL_RS: return (uint64_t)(uintptr_t)PL_rs;
        case GOPERL_PL_ORS: return (uint64_t)(uintptr_t)PL_ors_sv;
        case GOPERL_PL_SORTCOP: return (uint64_t)(uintptr_t)PL_sortcop;
        case GOPERL_PL_SIGHANDLERP:
            return PL_sighandlerp == goperl_sighandler_native ? g_sighandler_native : 0;
        case GOPERL_PL_SV_ARENAROOT: return (uint64_t)(uintptr_t)PL_sv_arenaroot;
        case GOPERL_PL_IN_EVAL: return (uint64_t)PL_in_eval;
        case GOPERL_PL_PHASE: return (uint64_t)PL_phase;
        case GOPERL_PL_TAINTING: return TAINTING_get ? 1 : 0;
        case GOPERL_PL_TAINTED: return TAINT_get ? 1 : 0;
        case GOPERL_PL_DIRTY: return PL_dirty ? 1 : 0;
        case GOPERL_PL_SAVESTACK_IX: return (uint64_t)(int64_t)PL_savestack_ix;
        case GOPERL_PL_TMPS_IX: return (uint64_t)(int64_t)PL_tmps_ix;
        case GOPERL_PL_STDINGV: return (uint64_t)(uintptr_t)PL_stdingv;
        case GOPERL_PL_ARGVGV: return (uint64_t)(uintptr_t)PL_argvgv;
        case GOPERL_PL_ENVGV: return (uint64_t)(uintptr_t)PL_envgv;
        case GOPERL_PL_LOCALIZING: return (uint64_t)PL_localizing;
        case GOPERL_PL_BEGINAV: return (uint64_t)(uintptr_t)(SV *)PL_beginav;
        case GOPERL_PL_UNITCHECKAV: return (uint64_t)(uintptr_t)(SV *)PL_unitcheckav;
        case GOPERL_PL_SV_ZERO: return (uint64_t)(uintptr_t)&PL_sv_zero;
        case GOPERL_PL_SV_PLACEHOLDER: return (uint64_t)(uintptr_t)&PL_sv_placeholder;
        case GOPERL_PL_STRTAB: return (uint64_t)(uintptr_t)(SV *)PL_strtab;
        case GOPERL_PL_STATGV: return (uint64_t)(uintptr_t)PL_statgv;
        case GOPERL_PL_TMPS_FLOOR: return (uint64_t)(int64_t)PL_tmps_floor;
        case GOPERL_PL_ORIGFILENAME: { /* second: the length */
            if (!PL_origfilename) return 0;
            return goperl_two(GOPERL_ADDR(PL_origfilename), (uint64_t)strlen(PL_origfilename));
        }
        case GOPERL_PL_UNSAFE: return PL_unsafe ? 1 : 0;
        case GOPERL_PL_DESTRUCT_LEVEL: return (uint64_t)(int64_t)PL_perl_destruct_level;
        case GOPERL_PL_LASTGV: return (uint64_t)(uintptr_t)PL_last_in_gv;
        case GOPERL_PL_COP_SEQMAX: return (uint64_t)PL_cop_seqmax;
        case GOPERL_PL_FIRSTGV: return (uint64_t)(uintptr_t)PL_firstgv;
        case GOPERL_PL_SECONDGV: return (uint64_t)(uintptr_t)PL_secondgv;
        case GOPERL_PL_PADIX: return (uint64_t)(int64_t)PL_padix;
        case GOPERL_PL_CONSTPADIX: return (uint64_t)(int64_t)PL_constpadix;
        case GOPERL_PL_COMPPAD_NAME_FILL: return (uint64_t)(int64_t)PL_comppad_name_fill;
        case GOPERL_PL_MIN_INTRO_PENDING: return (uint64_t)(int64_t)PL_min_intro_pending;
        case GOPERL_PL_MAX_INTRO_PENDING: return (uint64_t)(int64_t)PL_max_intro_pending;
        case GOPERL_PL_CV_HAS_EVAL: return PL_cv_has_eval ? 1 : 0;
        case GOPERL_PL_PAD_RESET_PENDING: return PL_pad_reset_pending ? 1 : 0;
        case GOPERL_PL_SUBLINE: return (uint64_t)(int64_t)PL_subline;
        case GOPERL_PL_SUBNAME: return (uint64_t)(uintptr_t)PL_subname;
        case GOPERL_PL_CURSTNAME: return (uint64_t)(uintptr_t)PL_curstname;
        case GOPERL_PL_PADIX_FLOOR: return (uint64_t)(int64_t)PL_padix_floor;
        case GOPERL_PL_MULTIDEREF_PC: return (uint64_t)(uintptr_t)PL_multideref_pc;
        case GOPERL_PL_NOMEMOK: return PL_nomemok ? 1 : 0;
        case GOPERL_PL_MINUS_N: return PL_minus_n ? 1 : 0;
        case GOPERL_PL_MINUS_P: return PL_minus_p ? 1 : 0;
        case GOPERL_PL_RUNOPS: return PL_runops == goperl_runops_native ? g_runops_native : 0;
        case GOPERL_PL_ARGVOUTGV: return (uint64_t)(uintptr_t)PL_argvoutgv;
        case GOPERL_PL_GLOBALSTASH: return (uint64_t)(uintptr_t)(SV *)PL_globalstash;
        case GOPERL_PL_SIGNALS: return (uint64_t)PL_signals;
        case GOPERL_PL_OP_MASK: return (uint64_t)(uintptr_t)PL_op_mask;
        case GOPERL_PL_LASTSTATVAL: return (uint64_t)(int64_t)PL_laststatval;
        case GOPERL_PL_LASTSTYPE: return (uint64_t)PL_laststype;
        case GOPERL_PL_STATNAME: return (uint64_t)(uintptr_t)PL_statname;
        case GOPERL_PL_CURSTACK: return (uint64_t)(uintptr_t)(SV *)PL_curstack;
        case GOPERL_PL_MAINSTACK: return (uint64_t)(uintptr_t)(SV *)PL_mainstack;
        case GOPERL_PL_IN_CLEAN_OBJS: return PL_in_clean_objs ? 1 : 0;
        case GOPERL_PL_IN_CLEAN_ALL: return PL_in_clean_all ? 1 : 0;
        case GOPERL_PL_ORIGARGC: return (uint64_t)(int64_t)PL_origargc;
        case GOPERL_PL_ORIGARGV: return (uint64_t)(uintptr_t)PL_origargv;
        }
        return 0;
    }
    case GOPERL_OP_PLVAR_SET: { /* b = SV token or raw value */
        switch ((int)a) {
        case GOPERL_PL_DIEHOOK: PL_diehook = GOPERL_SVARG(b); return 0;
        case GOPERL_PL_WARNHOOK: PL_warnhook = GOPERL_SVARG(b); return 0;
        case GOPERL_PL_PERLDB: PL_perldb = (int)(int64_t)b; return 0;
        case GOPERL_PL_DBSUB: PL_DBsub = GOPERL_GVARG(b); return 0;
        case GOPERL_PL_DBSINGLE: PL_DBsingle = GOPERL_SVARG(b); return 0;
        case GOPERL_PL_ENDAV: PL_endav = GOPERL_AVARG(b); return 0;
        case GOPERL_PL_CHECKAV: PL_checkav = GOPERL_AVARG(b); return 0;
        case GOPERL_PL_INITAV: PL_initav = GOPERL_AVARG(b); return 0;
        case GOPERL_PL_EXIT_FLAGS: PL_exit_flags = (U8)b; return 0;
        case GOPERL_PL_HINTS: PL_hints = (U32)b; return 0;
        case GOPERL_PL_DEFOUTGV: PL_defoutgv = GOPERL_GVARG(b); return 0;
        case GOPERL_PL_RS: PL_rs = GOPERL_SVARG(b); return 0;
        case GOPERL_PL_ORS: PL_ors_sv = GOPERL_SVARG(b); return 0;
        case GOPERL_PL_SORTCOP: /* a place for a comparator's own keys too
                                  * (Sort::Key), which perl does not read */
            PL_sortcop = (OP *)GOPERL_STORED(b);
            return 0;
        case GOPERL_PL_SIGHANDLERP:
            g_sighandler_native = (uint32_t)b;
            PL_sighandlerp = b ? goperl_sighandler_native : Perl_sighandler;
            return 0;
        case GOPERL_PL_CURSTASH: PL_curstash = GOPERL_HVARG(b); return 0;
        case GOPERL_PL_TAINTED: TAINT_set(cBOOL(b)); return 0;
        case GOPERL_PL_TAINTING: TAINTING_set(cBOOL(b)); return 0;
        case GOPERL_PL_ERRSV: GvSV(PL_errgv) = GOPERL_SVARG(b); return 0;
        case GOPERL_PL_DEFSV: DEFSV_set(GOPERL_SVARG(b)); return 0;
        case GOPERL_PL_ERRNO: errno = (int)(int64_t)b; return 0;
        case GOPERL_PL_STATUSVALUE: PL_statusvalue = (I32)(int64_t)b; return 0;
        case GOPERL_PL_BEGINAV: PL_beginav = GOPERL_AVARG(b); return 0;
        case GOPERL_PL_UNITCHECKAV: PL_unitcheckav = GOPERL_AVARG(b); return 0;
        case GOPERL_PL_DESTRUCT_LEVEL: PL_perl_destruct_level = (signed char)(int64_t)b; return 0;
        case GOPERL_PL_LOCALIZING: PL_localizing = (U8)b; return 0;
        case GOPERL_PL_SUB_GENERATION: PL_sub_generation = (U32)b; return 0;
        case GOPERL_PL_SAVEBEGIN: PL_savebegin = cBOOL(b); return 0;
        case GOPERL_PL_SRAND_CALLED: PL_srand_called = cBOOL(b); return 0;
        case GOPERL_PL_CURCOP: PL_curcop = GOPERL_MEMARG(COP, b); return 0;
        case GOPERL_PL_COMPCV: PL_compcv = (CV *)GOPERL_SVARG(b); return 0;
        case GOPERL_PL_RESTARTOP: PL_restartop = GOPERL_MEMARG(OP, b); return 0;
        case GOPERL_PL_ERRORS: PL_errors = GOPERL_SVARG(b); return 0;
        case GOPERL_PL_FIRSTGV: PL_firstgv = GOPERL_GVARG(b); return 0;
        case GOPERL_PL_SECONDGV: PL_secondgv = GOPERL_GVARG(b); return 0;
        case GOPERL_PL_STRTAB: PL_strtab = GOPERL_HVARG(b); return 0;
        /* The pad in use: its values go with it (PAD_SET_CUR_NOSAVE), as a
         * module that makes another pad the one in use also has it. */
        case GOPERL_PL_COMPPAD:
            PL_comppad = GOPERL_AVARG(b);
            PL_curpad = PL_comppad ? AvARRAY(PL_comppad) : NULL;
            return 0;
        case GOPERL_PL_COMPPAD_NAME: PL_comppad_name = GOPERL_MEMARG(PADNAMELIST, b); return 0;
        /* b: the array whose values PL_curpad is to be */
        case GOPERL_PL_CURPAD: PL_curpad = GOPERL_SVARG(b) ? AvARRAY(GOPERL_AVARG(b)) : NULL; return 0;
        case GOPERL_PL_COP_SEQMAX: PL_cop_seqmax = (U32)b; return 0;
        case GOPERL_PL_PADIX: PL_padix = (PADOFFSET)(int64_t)b; return 0;
        case GOPERL_PL_CONSTPADIX: PL_constpadix = (PADOFFSET)(int64_t)b; return 0;
        case GOPERL_PL_COMPPAD_NAME_FILL: PL_comppad_name_fill = (PADOFFSET)(int64_t)b; return 0;
        case GOPERL_PL_MIN_INTRO_PENDING: PL_min_intro_pending = (PADOFFSET)(int64_t)b; return 0;
        case GOPERL_PL_MAX_INTRO_PENDING: PL_max_intro_pending = (PADOFFSET)(int64_t)b; return 0;
        case GOPERL_PL_CV_HAS_EVAL: PL_cv_has_eval = cBOOL(b); return 0;
        case GOPERL_PL_PAD_RESET_PENDING: PL_pad_reset_pending = cBOOL(b); return 0;
        case GOPERL_PL_SUBLINE: PL_subline = (I32)(int64_t)b; return 0;
        case GOPERL_PL_SUBNAME: PL_subname = GOPERL_SVARG(b); return 0;
        case GOPERL_PL_CURSTNAME: PL_curstname = GOPERL_SVARG(b); return 0;
        case GOPERL_PL_PADIX_FLOOR: PL_padix_floor = (PADOFFSET)(int64_t)b; return 0;
        case GOPERL_PL_MULTIDEREF_PC: PL_multideref_pc = (UNOP_AUX_item *)(uintptr_t)b; return 0;
        case GOPERL_PL_NOMEMOK: PL_nomemok = cBOOL(b); return 0;
        case GOPERL_PL_MINUS_N: PL_minus_n = cBOOL(b); return 0;
        case GOPERL_PL_MINUS_P: PL_minus_p = cBOOL(b); return 0;
        case GOPERL_PL_RUNOPS:
            g_runops_native = (uint32_t)b;
            PL_runops = b ? goperl_runops_native : runops_interruptible;
            return 0;
        case GOPERL_PL_ARGVOUTGV: PL_argvoutgv = GOPERL_GVARG(b); return 0;
        case GOPERL_PL_GLOBALSTASH: PL_globalstash = GOPERL_HVARG(b); return 0;
        case GOPERL_PL_SIGNALS: PL_signals = (U32)b; return 0;
        /* b: the address of a mask of MAXO bytes in this memory, or 0 */
        case GOPERL_PL_OP_MASK: PL_op_mask = (char *)(uintptr_t)b; return 0;
        case GOPERL_PL_LASTSTATVAL: PL_laststatval = (I32)(int64_t)b; return 0;
        case GOPERL_PL_LASTSTYPE: PL_laststype = (U16)b; return 0;
        case GOPERL_PL_STATNAME: PL_statname = GOPERL_SVARG(b); return 0;
        case GOPERL_PL_TMPS_FLOOR: PL_tmps_floor = (SSize_t)(int64_t)b; return 0;
        case GOPERL_PL_CURPM: PL_curpm = GOPERL_MEMARG(PMOP, b); return 0;
        case GOPERL_PL_DEFSTASH: PL_defstash = GOPERL_HVARG(b); return 0;
        case GOPERL_PL_PARSER:
            if (!b) PL_parser = NULL;
            return 0;
        }
        return 0;
    }
    case GOPERL_OP_SV_UTF8_ON:
        SvUTF8_on(GOPERL_SVARG(a));
        return 0;
    case GOPERL_OP_SV_POK_ON: {
        SV *sv = GOPERL_SVARG(a);
        if (SvTYPE(sv) >= SVt_PV && goperl_pv_host_string(aTHX_ sv, SVf_POK, SvPVX(sv), SvCUR(sv)))
            goperl_pv_host_refuse(aTHX);
        SvPOK_on(sv);
        return 0;
    }
    case GOPERL_OP_SV_DUMP: /* b: a handle to dump into, c: the depth of
                 * nesting, as do_sv_dump has them; sv_dump without a handle */
        if (b)
            do_sv_dump(0, GOPERL_MEMARG(PerlIO, b), GOPERL_SVARG(a), 0, (I32)(int64_t)c, FALSE, 0);
        else
            sv_dump(GOPERL_SVARG(a));
        return 0;
    case GOPERL_OP_SV_REFCNT_DEC: /* a freed scalar is perl's to warn of */
        SvREFCNT_dec(goperl_sv_arg(aTHX_ a, op, true));
        return 0;
    case GOPERL_OP_SV_UNMAGIC: /* a = sv, b = how */
        sv_unmagic(GOPERL_SVARG(a), (int)(int64_t)b);
        return 0;
    case GOPERL_OP_SAVE_HOOK: { /* scope-save a hook variable (a = a GOPERL_PL_ hook id,
                * b = 0 save_sptr / 1 save_generic_svref). The host assigns
                * the new value separately via PLVAR_SET. */
        SV **loc = ((int)a == GOPERL_PL_DIEHOOK) ? &PL_diehook : &PL_warnhook;
        if (b) save_generic_svref(loc);
        else   save_sptr(loc);
        return 0;
    }
    case GOPERL_OP_NEW_XS: { /* bind the generic native thunk as sub s with fn_id a
                * and return the CV token (newXS + XSANY, like
                * perl_register_native_xs but with the CV surfaced so the host
                * can model CvXSUBANY/alias dispatch). With b the sub has
                * no name (newXS(NULL, ...)): it is in no glob, and the one
                * reference to it is the caller's. */
        CV *cv = newXS(b ? NULL : (s ? s : ""), XS_goperl_native_thunk, __FILE__);
        CvXSUBANY(cv).any_i32 = (I32)(int64_t)a;
        return (uint64_t)(uintptr_t)cv;
    }
    case GOPERL_OP_SV_STASH: { /* the blessing stash of an object SV, else 0 */
        SV *sv = GOPERL_SVARG(a);
        return SvOBJECT(sv) ? (uint64_t)(uintptr_t)SvSTASH(sv) : 0;
    }
    case GOPERL_OP_NEW_SV:
        return (uint64_t)(uintptr_t)newSV((STRLEN)a);
    case GOPERL_OP_SV_UPGRADE:
        sv_upgrade(GOPERL_SVARG(a), (svtype)(int64_t)b);
        return 0;
    case GOPERL_OP_SV_UTF8_UPGRADE:
        sv_utf8_upgrade(GOPERL_SVARG(a));
        return 0;
    case GOPERL_OP_SAVE_DELETE: /* (b = signed klen, negative = UTF-8 key) */
        save_delete(GOPERL_HVARG(a),
                    savepvn(s ? s : "", (I32)((int64_t)b < 0 ? -(int64_t)b
                                                             : (int64_t)b)),
                    (I32)(int64_t)b);
        return 0;
    case GOPERL_OP_SAVE_HELEM: { /* (b = keysv, c = flags) */
        HV *hv = GOPERL_HVARG(a);
        SV *key = GOPERL_SVARG(b);
        HE *he = hv_fetch_ent(hv, key, TRUE, 0U);
        if (he) save_helem_flags(hv, key, &HeVAL(he), (U32)c);
        return 0;
    }
    case GOPERL_OP_NEW_PVN_SHARE: /* (b = signed len, negative = UTF-8) */
        return (uint64_t)(uintptr_t)newSVpvn_share(s ? s : "",
                                                   (SSize_t)(int64_t)b, 0U);
    case GOPERL_OP_SV_LEN:
        return (uint64_t)sv_len(GOPERL_SVARG(a));
    case GOPERL_OP_NEW_HVHV:
        return (uint64_t)(uintptr_t)(SV *)newHVhv(GOPERL_HVARG(a));
    case GOPERL_OP_AV_MAKE: { /* (s+b = the values, a word each) */
        size_t n = (size_t)b / 8;
        for (size_t i = 0; i < n; i++) {
            goperl_gword_t tok;
            memcpy(&tok, s + i * 8, 8);
            if (!GOPERL_SVARG(tok)) goperl_arg_refuse(aTHX_ op, tok, "an element");
        }
        SV **ary = n ? (SV **)malloc(n * sizeof(SV *)) : NULL;
        for (size_t i = 0; i < n; i++) {
            goperl_gword_t tok;
            memcpy(&tok, s + i * 8, 8);
            ary[i] = (SV *)(uintptr_t)tok;
        }
        AV *av = av_make((SSize_t)n, ary ? ary : (SV **)&ary);
        free(ary);
        return (uint64_t)(uintptr_t)(SV *)av;
    }
    /* perl's answer as it is: grok_number's flags for a string (the
     * number is fractional, negative, ...), the flags of the numbers it
     * holds otherwise */
    case GOPERL_OP_LOOKS_LIKE_NUMBER:
        return (uint64_t)(uint32_t)looks_like_number(GOPERL_SVARG(a));
    case GOPERL_OP_SV_AMAGIC:
        return SvAMAGIC(GOPERL_SVARG(a)) ? 1 : 0;
    case GOPERL_OP_AMAGIC_CALL: { /* (a = sv, b = AMGf flags, c = method); the
                * right operand is fixed at &PL_sv_undef (the deref-overload
                * calling pattern). */
        SV *res = amagic_call(GOPERL_SVARG(a), &PL_sv_undef, (int)(int64_t)c,
                              (int)(int64_t)b);
        return (uint64_t)(uintptr_t)res;
    }
    case GOPERL_OP_WARN:
        Perl_warn(aTHX_ "%s", s ? s : "");
        return 0;
    case GOPERL_OP_GET_HV:
        return (uint64_t)(uintptr_t)(SV *)get_hv(s ? s : "", (I32)(int64_t)a);
    case GOPERL_OP_AV_STORE_RAW: { /* (b = sv, c = idx): direct AvARRAY slot write
                * with NO refcount side effects — the host's mirror-flush
                * primitive matching a native `AvARRAY(av)[i] = sv`. Any slot
                * the array has room for: code fills the slots of an array it
                * has extended and sets AvFILLp afterwards. */
        AV *av = GOPERL_AVARG(a);
        SSize_t idx = (SSize_t)(int64_t)c;
        if (SvTYPE((SV *)av) == SVt_PVAV && idx >= 0 && idx <= AvMAX(av))
            AvARRAY(av)[idx] = goperl_store_into(aTHX_ (SV *)av, GOPERL_STORED(b));
        return 0;
    }

    /* ---- v4: pp hooks, save-stack destructors, and the OP/COP/CV/GV/
     * context introspection surface interpreter-hooking XS (the
     * Devel::NYTProf class) is built on. */
    case GOPERL_OP_PP_HOOK_SET: { /* (a = op_type, b = enable) */
        uint32_t t = (uint32_t)a;
        if (t >= MAXO) return 0;
        uint8_t on = b ? 1 : 0;
        if (!g_pp_orig_saved) {
            memcpy(g_pp_orig, PL_ppaddr, sizeof g_pp_orig);
            g_pp_orig_saved = true;
        }
        g_pp_hooked[t] = on;
        PL_ppaddr[t] = on ? goperl_pp_type_hook : g_pp_orig[t];
        return 0;
    }
    case GOPERL_OP_RUN_ORIGINAL: { /* (a = op_type): execute the real pp for the
                * CURRENT PL_op. The guest PL_ppaddr is never patched, so
                * the table entry is always the original. The type may
                * differ from PL_op->op_type (pp_entersub with type 0). */
        uint32_t t = (uint32_t)a;
        /* b: the op to run, when the host runs the ops itself (a run loop
         * of native code makes each one PL_op, as perl's loop does) */
        if (b) PL_op = GOPERL_MEMARG(OP, b);
        if (t >= MAXO || !PL_op) return 0;
        OP *next = goperl_pp_original(t)(aTHX);
        /* second: what the next op is now, for the shadow a run loop of
         * native code runs it by */
        return goperl_two((uint64_t)(uintptr_t)next, goperl_op_fields(next));
    }
    case GOPERL_OP_SAVE_DESTRUCTOR: /* (a = host id) */
        save_destructor_x(goperl_dtor_thunk, (void *)(uintptr_t)a);
        return 0;
    case GOPERL_OP_OP_FIELDS: /* targ<<32 | private<<24 | flags<<16 | type */
        return goperl_op_fields(GOPERL_MEMARG(OP, a));
    case GOPERL_OP_OP_PTR: { /* b = a GOPERL_OPPTR_ selector */
        OP *o = GOPERL_MEMARG(OP, a);
        switch ((int)b) {
        case GOPERL_OPPTR_NEXT: return (uint64_t)(uintptr_t)o->op_next;
        case GOPERL_OPPTR_SIBLING: return (uint64_t)(uintptr_t)OpSIBLING(o);
        case GOPERL_OPPTR_FIRST:
            if (o->op_flags & OPf_KIDS)
                return (uint64_t)(uintptr_t)cUNOPx(o)->op_first;
            return 0;
        case GOPERL_OPPTR_REDOOP:
            if ((PL_opargs[o->op_type] & OA_CLASS_MASK) == OA_LOOP)
                return (uint64_t)(uintptr_t)cLOOPx(o)->op_redoop;
            return 0;
        case GOPERL_OPPTR_LAST: {
            U32 cls = PL_opargs[o->op_type] & OA_CLASS_MASK;
            if (cls == OA_BINOP || cls == OA_LISTOP || cls == OA_PMOP ||
                cls == OA_LOOP ||
                (o->op_type == OP_CUSTOM && (o->op_flags & OPf_KIDS)))
                return (uint64_t)(uintptr_t)cBINOPx(o)->op_last;
            return 0;
        }
        case GOPERL_OPPTR_OTHER: {
            U32 cls = PL_opargs[o->op_type] & OA_CLASS_MASK;
            if (cls == OA_LOGOP || o->op_type == OP_CUSTOM)
                return (uint64_t)(uintptr_t)cLOGOPx(o)->op_other;
            return 0;
        }
        case GOPERL_OPPTR_MORESIB:
            return OpHAS_SIBLING(o) ? 1 : 0;
        case GOPERL_OPPTR_IS_HOOKED:
            return goperl_pp_is_hook(o->op_ppaddr) ? 1 : 0;
        }
        return 0;
    }
    case GOPERL_OP_COP_LINE: /* (the caller vouches that a is a COP: PL_curcop,
               * blk_oldcop, or a type-checked nextstate/dbstate — this also
               * covers static COPs like PL_compiling whose op_type is 0) */
        return (uint64_t)CopLINE(GOPERL_MEMARG(COP, a));
    /* The operations that answer a string of this memory (COP_FILE,
     * COP_STASHPV, GV_NAME, HV_NAME, OP_NAME_STR, CV_FILE) answer its
     * address, or 0 for none; second: its length. */
    case GOPERL_OP_COP_FILE: {
        const char *f = CopFILE(GOPERL_MEMARG(COP, a));
        if (!f) return 0;
        return goperl_two(GOPERL_ADDR(f), (uint64_t)strlen(f));
    }
    case GOPERL_OP_COP_STASHPV: {
        const char *s2 = CopSTASHPV(GOPERL_MEMARG(COP, a));
        if (!s2) return 0;
        return goperl_two(GOPERL_ADDR(s2), (uint64_t)strlen(s2));
    }
    case GOPERL_OP_CV_INFO: { /* depth<<32 | isxsub */
        CV *cv = GOPERL_CVARG(a);
        if (SvTYPE((SV *)cv) != SVt_PVCV) return 0;
        return ((uint64_t)(uint32_t)CvDEPTH(cv) << 32) |
               (CvISXSUB(cv) ? 1u : 0u);
    }
    case GOPERL_OP_CV_PTR: { /* b = a GOPERL_CVPTR_ selector */
        CV *cv = GOPERL_CVARG(a);
        if (SvTYPE((SV *)cv) != SVt_PVCV) return 0;
        switch ((int)b) {
        case GOPERL_CVPTR_START: return CvISXSUB(cv) ? 0 : (uint64_t)(uintptr_t)CvSTART(cv);
        case GOPERL_CVPTR_GV: return (uint64_t)(uintptr_t)CvGV(cv);
        case GOPERL_CVPTR_STASH: return (uint64_t)(uintptr_t)(SV *)CvSTASH(cv);
        case GOPERL_CVPTR_ROOT: return CvISXSUB(cv) ? 0 : (uint64_t)(uintptr_t)CvROOT(cv);
        }
        return 0;
    }
    case GOPERL_OP_GV_PTR: { /* b = a GOPERL_GVPTR_ selector */
        GV *gv = GOPERL_GVARG(a);
        if (!isGV_with_GP((SV *)gv)) return 0;
        switch ((int)b) {
        case GOPERL_GVPTR_STASH: return (uint64_t)(uintptr_t)(SV *)GvSTASH(gv);
        case GOPERL_GVPTR_CVU: return (uint64_t)(uintptr_t)(SV *)GvCVu(gv);
        case GOPERL_GVPTR_HV: return (uint64_t)(uintptr_t)(SV *)GvHV(gv);
        case GOPERL_GVPTR_AV: return (uint64_t)(uintptr_t)(SV *)GvAV(gv);
        case GOPERL_GVPTR_EGV: return (uint64_t)(uintptr_t)(SV *)GvEGV(gv);
        case GOPERL_GVPTR_SV: return (uint64_t)(uintptr_t)GvSV(gv);
        case GOPERL_GVPTR_IO: return (uint64_t)(uintptr_t)(SV *)GvIO(gv);
        case GOPERL_GVPTR_CV: /* also when the glob is a method-cache entry,
                 * where GvCVu gives nothing */
            return (uint64_t)(uintptr_t)(SV *)GvCV(gv);
        }
        return 0;
    }
    case GOPERL_OP_GV_NAME: {
        GV *gv = GOPERL_GVARG(a);
        if (!isGV_with_GP((SV *)gv)) return 0;
        const char *n = GvNAME(gv);
        if (!n) return 0;
        return goperl_two(GOPERL_ADDR(n), (uint64_t)GvNAMELEN(gv));
    }
    case GOPERL_OP_SV_ISGV_GP:
        return isGV_with_GP(GOPERL_SVARG(a)) ? 1 : 0;
    case GOPERL_OP_HV_NAME: { /* (0 for anonymous stashes) */
        HV *hv = GOPERL_HVARG(a);
        if (SvTYPE((SV *)hv) != SVt_PVHV) return 0;
        const char *n = HvNAME(hv);
        if (!n) return 0;
        return goperl_two(GOPERL_ADDR(n), (uint64_t)HvNAMELEN(hv));
    }
    case GOPERL_OP_SI_GET: { /* a = si token or 0 for PL_curstackinfo;
                 * b = a GOPERL_SI_ selector */
        PERL_SI *si = a ? (PERL_SI *)(uintptr_t)a : PL_curstackinfo;
        if (!si) return 0;
        switch ((int)b) {
        case GOPERL_SI_SELF: return (uint64_t)(uintptr_t)si;
        case GOPERL_SI_PREV: return (uint64_t)(uintptr_t)si->si_prev;
        case GOPERL_SI_TYPE: return (uint64_t)(int64_t)si->si_type;
        case GOPERL_SI_CXIX: return (uint64_t)(int64_t)si->si_cxix;
        }
        return 0;
    }
    case GOPERL_OP_CX_FIELDS: { /* (a = si token or 0, b = ix) -> CxTYPE, or
                 * GOPERL_CX_TYPE_NONE when ix is out of range */
        PERL_SI *si = a ? (PERL_SI *)(uintptr_t)a : PL_curstackinfo;
        if (!si || (I32)(int64_t)b < 0 || (I32)(int64_t)b > si->si_cxix)
            return (uint64_t)GOPERL_CX_TYPE_NONE;
        PERL_CONTEXT *cx = &si->si_cxstack[(I32)(int64_t)b];
        /* the whole type byte (CXTYPEMASK and the CXp_ bits), and the
         * context the block was entered in above it */
        return (uint64_t)cx->cx_type | ((uint64_t)cx->blk_gimme << 8);
    }
    case GOPERL_OP_CX_PTR: { /* a = si token or 0,
                 * b = ix<<8 | a GOPERL_CXPTR_ selector */
        PERL_SI *si = a ? (PERL_SI *)(uintptr_t)a : PL_curstackinfo;
        I32 ix = (I32)(int64_t)(b >> 8);
        if (!si || ix < 0 || ix > si->si_cxix) return 0;
        PERL_CONTEXT *cx = &si->si_cxstack[ix];
        switch ((int)(b & 0xFF)) {
        case GOPERL_CXPTR_OLDCOP: return (uint64_t)(uintptr_t)(OP *)cx->blk_oldcop;
        case GOPERL_CXPTR_CV:
            if (CxTYPE(cx) == CXt_SUB || CxTYPE(cx) == CXt_FORMAT)
                return (uint64_t)(uintptr_t)(SV *)cx->blk_sub.cv;
            return 0;
        case GOPERL_CXPTR_MYOP:
            if (CxTYPE_is_LOOP(cx))
                return (uint64_t)(uintptr_t)(OP *)cx->blk_loop.my_op;
            return 0;
        }
        return 0;
    }
    case GOPERL_OP_OP_NAME_STR: { /* PL_op_name[type] */
        uint32_t t = (uint32_t)a;
        if (t >= MAXO) return 0;
        const char *n = PL_op_name[t];
        return goperl_two(GOPERL_ADDR(n), (uint64_t)strlen(n));
    }
    case GOPERL_OP_CV_FILE: { /* CvFILE */
        CV *cv = GOPERL_CVARG(a);
        if (SvTYPE((SV *)cv) != SVt_PVCV) return 0;
        const char *f = CvFILE(cv);
        if (!f) return 0;
        return goperl_two(GOPERL_ADDR(f), (uint64_t)strlen(f));
    }
    case GOPERL_OP_GV_FETCHFILE: /* (s = name, b = len) */
        return (uint64_t)(uintptr_t)gv_fetchfile_flags(s ? s : "",
                                                       (STRLEN)b, 0);
    case GOPERL_OP_HV_CLEAR:
        hv_clear(GOPERL_HVARG(a));
        return 0;
    case GOPERL_OP_HV_DELETE: { /* a = hv, s = the key's bytes, b = flags,
                 * c = the key's length */
        SV *r = hv_delete(GOPERL_HVARG_COMMON(a), s ? s : "", (I32)(int64_t)c, (I32)(int64_t)b);
        return (uint64_t)(uintptr_t)r;
    }
    case GOPERL_OP_AV_EXISTS:
        return av_exists(GOPERL_AVARG(a), (SSize_t)(int64_t)b) ? 1 : 0;
    case GOPERL_OP_SV_UTF8_OFF:
        SvUTF8_off(GOPERL_SVARG(a));
        return 0;
    case GOPERL_OP_SAVE_SCALAR: /* (a = gv) -> localized SV */
        return (uint64_t)(uintptr_t)save_scalar(GOPERL_GVARG(a));
    case GOPERL_OP_SV_READONLY_ON:
        SvREADONLY_on(GOPERL_SVARG(a));
        return 0;
    case GOPERL_OP_EVAL_PV: { /* (s = code, a = croak_on_error) */
        SV *r = eval_pv(s ? s : "", (I32)(int64_t)a);
        return (uint64_t)(uintptr_t)r;
    }
    case GOPERL_OP_AV_UNSHIFT:
        av_unshift(GOPERL_AVARG(a), (SSize_t)(int64_t)b);
        return 0;
    case GOPERL_OP_NEW_CONSTSUB: { /* (a = stash, s = name, b = sv; takes over the
                 * sv reference like the C API) */
        CV *cv = newCONSTSUB(GOPERL_HVARG(a), s ? s : "", GOPERL_SVARG(b));
        return (uint64_t)(uintptr_t)cv;
    }
    case GOPERL_OP_SV_PVX_RAW: { /* the raw PV buffer pointer, NO stringification
                 * (the SvPVX lvalue-buffer idiom: sv_grow then write). */
        SV *sv = GOPERL_SVARG(a);
        if (SvTYPE(sv) < SVt_PV) return 0;
        return GOPERL_ADDR(SvPVX(sv));
    }

    /* ---- v5: the Class::MOP/Moose surface. */
    case GOPERL_OP_HV_PKG_GEN: { /* mro generation of a stash ->
                 * has_meta<<32 | pkg_gen (0 when the hash has no aux/meta).
                 * The moral equivalent of mro::get_pkg_gen($stash), read the
                 * same way Moose's mop.c reads it. */
        HV *hv = GOPERL_HVARG(a);
        if (b == 1) { /* HvMROMETA, which makes the meta when there is none:
                 * pkg_gen<<32 | cache_gen */
            if (SvTYPE((SV *)hv) != SVt_PVHV) return 0;
            struct mro_meta *meta = HvMROMETA(hv);
            return ((uint64_t)meta->pkg_gen << 32) | (uint32_t)meta->cache_gen;
        }
        if (SvTYPE((SV *)hv) != SVt_PVHV || !HvHasAUX(hv)) return 0;
        struct mro_meta *m = HvAUX(hv)->xhv_mro_meta;
        if (!m) return 0;
        return (1ull << 32) | (uint32_t)m->pkg_gen;
    }
    case GOPERL_OP_SV_REFCNT: {
        /* the count of something that is no scalar is the word where a
         * scalar's count would be, as perl's SvREFCNT reads it: the header
         * of an SV arena keeps the number of its slots there, which a walk
         * of the arenas reads (Devel::Leak). Reading it changes nothing; an
         * address outside the memory is refused. */
        const SV *const sv = (const SV *)(uintptr_t)a;
        if (!a || goperl_sv_head(aTHX_ sv)) return (uint64_t)SvREFCNT(goperl_sv_arg(aTHX_ a, op, true));
        const uint64_t end = (uint64_t)__builtin_wasm_memory_size(0) << 16;
        if (a >= end || end - a < sizeof(SV)) goperl_arg_refuse(aTHX_ op, a, "a scalar");
        U32 refcnt;
        memcpy(&refcnt, (const char *)sv + offsetof(SV, sv_refcnt), sizeof refcnt);
        return (uint64_t)refcnt;
    }
    case GOPERL_OP_GV_INIT: { /* expand a stash stub into a real glob
                 * (b = stash, c = flags, s = name) -> the gv token */
        GV *gv = GOPERL_GVARG(a);
        HV *stash = GOPERL_HVARG(b);
        gv_init_pvn(gv, stash, s ? s : "", slen, (U32)c);
        return (uint64_t)(uintptr_t)gv;
    }
    case GOPERL_OP_GV_AMG: { /* Gv_AMG: does the stash have a (freshly
                 * updated) overload magic table? */
        HV *stash = GOPERL_HVARG(a);
        if (SvTYPE((SV *)stash) != SVt_PVHV) return 0;
        return Gv_AMG(stash) ? 1 : 0;
    }
    case GOPERL_OP_SV_AMAGIC_SET: /* b ? SvAMAGIC_on : SvAMAGIC_off (a must
                 * be a reference, per the core macros) */
        if (!SvROK(GOPERL_SVARG(a))) return 0;
        if (b) SvAMAGIC_on(GOPERL_SVARG(a));
        else SvAMAGIC_off(GOPERL_SVARG(a));
        return 1;
    case GOPERL_OP_GV_FETCHMETHOD: { /* method resolution with ISA walk
                 * (a = stash, b = autoload flag, s = name) -> GV token */
        HV *stash = GOPERL_HVARG(a);
        if (SvTYPE((SV *)stash) != SVt_PVHV) return 0;
        GV *gv = gv_fetchmethod_autoload(stash, s ? s : "", b ? TRUE : FALSE);
        return (uint64_t)(uintptr_t)gv;
    }
    case GOPERL_OP_SV_READONLY_OFF:
        SvREADONLY_off(GOPERL_SVARG(a));
        return 0;
    case GOPERL_OP_SV_PV_FORCE: { /* -> the writable PV; second: its length */
        STRLEN len = 0;
        char *p = SvPV_force(GOPERL_SVARG(a), len);
        return goperl_two(GOPERL_ADDR(p), (uint64_t)len);
    }
    case GOPERL_OP_SV_CHOP: { /* b = byte offset from SvPVX to chop to */
        SV *sv = GOPERL_SVARG(a);
        if (SvTYPE(sv) < SVt_PV || !SvPOK(sv)) return 0;
        sv_chop(sv, SvPVX(sv) + (STRLEN)b);
        return 0;
    }
    case GOPERL_OP_SV_INSERT: /* b = off, c = replace_len, s = little */
        sv_insert(GOPERL_SVARG(a), (STRLEN)b, (STRLEN)c, s ? s : "", slen);
        return 0;
    case GOPERL_OP_SV_UTF8_DECODE:
        return sv_utf8_decode(GOPERL_SVARG(a)) ? 1 : 0;
    case GOPERL_OP_SV_UTF8_DOWNGRADE: /* b = fail_ok */
        return sv_utf8_downgrade(GOPERL_SVARG(a), b ? TRUE : FALSE) ? 1 : 0;
    case GOPERL_OP_MG_SET: { /* SvSETMAGIC semantics: only a set-magical
                 * SV has a magic chain to run (SvMAGIC on smaller bodies
                 * reads past the struct). */
        SV *sv = GOPERL_SVARG(a);
        if (SvSMAGICAL(sv)) mg_set(sv);
        return 0;
    }
    case GOPERL_OP_GIMME_V: { /* the context the running XSUB was called in:
                 * the want bits of the calling op, and only without them
                 * the context of the enclosing sub (an XSUB pushes no
                 * context of its own, so block_gimme alone answers for the
                 * caller's caller). a: the op native code has as PL_op. */
        goperl_native_plop plop;
        goperl_native_plop_enter(aTHX_ &plop, a & 7);
        const U8 gimme = GIMME_V;
        goperl_native_plop_leave(aTHX_ &plop);
        return (uint64_t)gimme;
    }
    case GOPERL_OP_CKWARN: /* a = warning category; b = ckWARN_d variant */
        return (b ? ckWARN_d((U32)a) : ckWARN((U32)a)) ? 1 : 0;
    case GOPERL_OP_SV_LEN_BUF: /* SvLEN: the ALLOCATED buffer size */
        return (SvTYPE(GOPERL_SVARG(a)) < SVt_PV)
                   ? 0
                   : (uint64_t)SvLEN(GOPERL_SVARG(a));
    case GOPERL_OP_HV_STORE_KLEN: { /* a = hv, b = value token, c = utf8<<32 |
                 * key length, s = the key's bytes; takes over the value
                 * ref -> stored SV token. hv_common with JUST_SV returns
                 * the SLOT (SV**). */
        HV *const hv = GOPERL_HVARG_COMMON(a);
        const int kflags = (c >> 32) ? HVhek_UTF8 : 0;
        if (goperl_holds_foreign((SV *)hv))
            goperl_slot_unforeign((SV **)hv_common(hv, NULL, s ? s : "", (STRLEN)(uint32_t)c,
                                                   kflags, HV_FETCH_JUST_SV, NULL, 0));
        SV *val = goperl_store_into(aTHX_ (SV *)hv, GOPERL_STORED(b));
        SV **svp = (SV **)hv_common(hv, NULL, s ? s : "",
                                    (STRLEN)(uint32_t)c, kflags,
                                    HV_FETCH_ISSTORE | HV_FETCH_JUST_SV, val, 0);
        /* second: the address of the slot (HeVAL of the entry) */
        return goperl_two(svp ? (uint64_t)(uintptr_t)*svp : 0, GOPERL_ADDR(svp));
    }
    case GOPERL_OP_HV_FETCH_KLEN: { /* a = hv, b = lval, c = utf8<<32 | key
                 * length, s = the key's bytes */
        SV **svp = (SV **)hv_common(GOPERL_HVARG_COMMON(a), NULL, s ? s : "",
                                    (STRLEN)(uint32_t)c, (c >> 32) ? HVhek_UTF8 : 0,
                                    HV_FETCH_JUST_SV | (b ? HV_FETCH_LVALUE : 0), NULL, 0);
        /* second: the address of the slot (HeVAL of the entry) */
        return goperl_two(svp ? (uint64_t)(uintptr_t)*svp : 0, GOPERL_ADDR(svp));
    }
    case GOPERL_OP_HV_DELETE_ENT: { /* b = keysv, c = flags */
        SV *r = hv_delete_ent(GOPERL_HVARG_COMMON(a), GOPERL_SVARG(b), (I32)(int64_t)c, 0);
        return (uint64_t)(uintptr_t)r;
    }
    case GOPERL_OP_AV_POP:
        return (uint64_t)(uintptr_t)av_pop(GOPERL_AVARG(a));
    case GOPERL_OP_COP_HINTS: /* CopHINTS of a caller-vouched COP */
        return (uint64_t)CopHINTS_get(GOPERL_MEMARG(COP, a));
    case GOPERL_OP_SV_IS_BOOL:
        return SvIsBOOL(GOPERL_SVARG(a)) ? 1 : 0;
    case GOPERL_OP_SV_POK_ONLY:
        SvPOK_only(GOPERL_SVARG(a));
        return 0;
    case GOPERL_OP_EVAL_SV: { /* a = sv, c = flags; results like CALL_SV:
                 * their words; second: died<<63 | count */
        I32 flags = (I32)(int64_t)c;
        I32 count = eval_sv(GOPERL_SVARG(a), flags);
        const uint64_t words = goperl_call_results(aTHX_ count);
        /* eval_sv traps a die in $@ and with G_RETHROW throws it again
         * itself; the truth of $@ is not asked here, which would run the
         * bool overloading of an exception object. */
        const uint64_t died = 0;
        return goperl_two(words, (died << 63) | (uint32_t)count);
    }
    case GOPERL_OP_AV_SHIFT:
        return (uint64_t)(uintptr_t)av_shift(GOPERL_AVARG(a));
    case GOPERL_OP_SV_2IO: /* the IO sv behind a handle-ish sv (croaks the
                 * way the real one does on non-handles; b = no_croak) */
        if (b) {
            IO *io = NULL;
            dJMPENV;
            int jmp;
            JMPENV_PUSH(jmp);
            if (jmp == 0) io = sv_2io(GOPERL_SVARG(a));
            JMPENV_POP;
            return (uint64_t)(uintptr_t)(SV *)io;
        }
        return (uint64_t)(uintptr_t)(SV *)sv_2io(GOPERL_SVARG(a));
    case GOPERL_OP_SV_FORCE_NORMAL:
        sv_force_normal(GOPERL_SVARG(a));
        return 0;
    case GOPERL_OP_PERLIO_OPEN: { /* s = "mode\0path" -> PerlIO token */
        const char *mode = s ? s : "r";
        const char *path = mode + strlen(mode) + 1;
        PerlIO *io = PerlIO_open(path, mode);
        return (uint64_t)(uintptr_t)io;
    }
    case GOPERL_OP_PERLIO_CLOSE:
        return (uint64_t)(int64_t)PerlIO_close(GOPERL_MEMARG(PerlIO, a));
    case GOPERL_OP_PERLIO_PUTS: /* s+b = bytes */
        return (uint64_t)(int64_t)PerlIO_write(GOPERL_MEMARG(PerlIO, a),
                                               s ? s : "", (Size_t)b);
    case GOPERL_OP_PERLIO_FLUSH:
        return (uint64_t)(int64_t)PerlIO_flush(GOPERL_MEMARG(PerlIO, a));
    case GOPERL_OP_PERLIO_WRITE:
        return (uint64_t)(int64_t)PerlIO_write(GOPERL_MEMARG(PerlIO, a),
                                               s ? s : "", (Size_t)b);
    case GOPERL_OP_IO_OFP: { /* output PerlIO of an IO sv (IoOFP) */
        IO *io = GOPERL_IOARG(a);
        if (SvTYPE((SV *)io) != SVt_PVIO) return 0;
        return (uint64_t)(uintptr_t)IoOFP(io);
    }
    case GOPERL_OP_SV_CUR: /* raw SvCUR — NO stringification/get-magic
                 * (SvPV on an undef buffer warns; SvCUR must not) */
        return (SvTYPE(GOPERL_SVARG(a)) < SVt_PV)
                   ? 0
                   : (uint64_t)SvCUR(GOPERL_SVARG(a));
    case GOPERL_OP_PERLIO_DEF_LAYER: { /* s = name, a = funcs_id, b = mask.
                 * Clone the :perlio buffer layer, swap in the forwarding
                 * hooks the mask asks for, and register it. */
        if (goperlio_nclasses >= GOPERLIO_MAX_LAYERS) return 0;
        goperlio_class_t *cls = &goperlio_classes[goperlio_nclasses++];
        memcpy(&cls->funcs, &PerlIO_perlio, sizeof(PerlIO_funcs));
        strncpy(cls->name, s ? s : "goperlio", sizeof(cls->name) - 1);
        cls->funcs.name = cls->name;
        cls->funcs.size = sizeof(GoperlIOL);
        cls->funcs.kind |= PERLIO_K_UTF8; /* allow :utf8-ish layers */
        cls->funcs_id = (uint32_t)a;
        cls->mask = (uint32_t)b;
        cls->funcs.Pushed = goperlio_pushed;
        cls->funcs.Popped = goperlio_popped;
        if (b & GOPERLIO_MASK_FILL) cls->funcs.Fill = goperlio_fill;
        PerlIO_define_layer(aTHX_ &cls->funcs);
        return 1;
    }
    case GOPERL_OP_PERLIO_NEXT_READ: { /* a = ff, b = dst, c = len */
        PerlIO *f = GOPERL_MEMARG(PerlIO, a);
        PerlIO *n = PerlIOValid(f) ? PerlIONext(f) : NULL;
        if (!PerlIOValid(n)) return (uint64_t)(int64_t)-1;
        SSize_t got = PerlIO_read(n, (void *)(uintptr_t)b, (Size_t)c);
        return (uint64_t)(int64_t)got;
    }
    case GOPERL_OP_PERLIO_NEXT_FASTGETS: {
        PerlIO *f = GOPERL_MEMARG(PerlIO, a);
        PerlIO *n = PerlIOValid(f) ? PerlIONext(f) : NULL;
        return (PerlIOValid(n) && PerlIO_fast_gets(n)) ? 1 : 0;
    }
    case GOPERL_OP_PERLIO_NEXT_GETCNT: {
        PerlIO *n = PerlIONext(GOPERL_MEMARG(PerlIO, a));
        return (uint64_t)(int64_t)PerlIO_get_cnt(n);
    }
    case GOPERL_OP_PERLIO_NEXT_GETPTR: {
        PerlIO *n = PerlIONext(GOPERL_MEMARG(PerlIO, a));
        return GOPERL_ADDR(PerlIO_get_ptr(n));
    }
    case GOPERL_OP_PERLIO_NEXT_SETPTRCNT: { /* b = ptr, c = cnt */
        PerlIO *n = PerlIONext(GOPERL_MEMARG(PerlIO, a));
        PerlIO_set_ptrcnt(n, (STDCHAR *)(uintptr_t)b, (SSize_t)(int64_t)c);
        return 0;
    }
    case GOPERL_OP_PERLIO_NEXT_FILL: {
        PerlIO *n = PerlIONext(GOPERL_MEMARG(PerlIO, a));
        return (uint64_t)(int64_t)PerlIO_fill(n);
    }
    case GOPERL_OP_PERLIO_STATE: { /* b: 0 = eof|error<<1 of NEXT,
                 * 1 = flush NEXT, 2 = flush SELF, 3 = the flags of NEXT */
        PerlIO *f = GOPERL_MEMARG(PerlIO, a);
        switch ((int)b) {
        case 0: {
            PerlIO *n = PerlIOValid(f) ? PerlIONext(f) : NULL;
            if (!PerlIOValid(n)) return 1; /* treat as EOF */
            return (PerlIO_eof(n) ? 1 : 0) | (PerlIO_error(n) ? 2 : 0);
        }
        case 1: {
            PerlIO *n = PerlIOValid(f) ? PerlIONext(f) : NULL;
            return PerlIOValid(n) ? (uint64_t)(int64_t)PerlIO_flush(n) : 0;
        }
        case 2:
            return (uint64_t)(int64_t)PerlIO_flush(f);
        case 3: {
            PerlIO *n = PerlIOValid(f) ? PerlIONext(f) : NULL;
            return PerlIOValid(n) ? (uint64_t)PerlIOBase(n)->flags : 0;
        }
        }
        return 0;
    }
    case GOPERL_OP_OPTREE_NEW: { /* a = class<<32|type, b = flags,
                 * s = kid tokens (u64 each; count fixed by class) */
        int cls = (int)(a >> 32);
        I32 type = (I32)(uint32_t)(a & 0xFFFFFFFFu);
        I32 flags = (I32)(uint32_t)b;
        uint64_t k1 = 0, k2 = 0, k3 = 0;
        if (s) {
            memcpy(&k1, s, 8);
            memcpy(&k2, s + 8, 8);
            if (cls == GOPERL_OPC_CONDOP) memcpy(&k3, s + 16, 8);
        }
        OP *first = GOPERL_MEMARG(OP, k1);
        OP *second = GOPERL_MEMARG(OP, k2);
        OP *o = NULL;
        switch (cls) {
        case GOPERL_OPC_BASEOP: o = newOP(type, flags); break;
        case GOPERL_OPC_UNOP: o = newUNOP(type, flags, first); break;
        case GOPERL_OPC_BINOP: o = newBINOP(type, flags, first, second); break;
        case GOPERL_OPC_LISTOP: o = newLISTOP(type, flags, first, second); break;
        case GOPERL_OPC_LOGOP: o = newLOGOP(type, flags, first, second); break;
        case GOPERL_OPC_SVOP:
            o = newSVOP(type, flags, GOPERL_SVARG(k1));
            break;
        case GOPERL_OPC_PVOP:
            Perl_croak(aTHX_ "goperl: newPVOP is not bridged");
            break;
        case GOPERL_OPC_GVOP:
            o = newGVOP(type, flags, GOPERL_GVARG(k1));
            break;
        case GOPERL_OPC_METHOP_NAMED:
            o = newMETHOP_named(type, flags, GOPERL_SVARG(k1));
            break;
        case GOPERL_OPC_STATEOP:
            if (k1)
                Perl_croak(aTHX_ "goperl: newSTATEOP labels are not bridged");
            o = newSTATEOP(flags, NULL, second);
            break;
        case GOPERL_OPC_CONDOP:
            o = newCONDOP(flags, first, second, GOPERL_MEMARG(OP, k3));
            break;
        case GOPERL_OPC_SLICEOP: o = newSLICEOP(flags, first, second); break;
        case GOPERL_OPC_ASSIGNOP:
            /* type is the operator of an assignment operator (OP_ADD for
             * +=), 0 for plain assignment */
            o = newASSIGNOP(flags, first, type, second);
            break;
        case GOPERL_OPC_RAW: {
            /* backing for a module-defined op struct: LOGOP-shaped slab
             * memory the host will type/link/hook through OP_SET */
            o = (OP *)Perl_Slab_Alloc(aTHX_ sizeof(LOGOP));
            o->op_type = OP_NULL;
            o->op_ppaddr = PL_ppaddr[OP_NULL];
            g_raw_ops.insert(o);
            break;
        }
        default: Perl_croak(aTHX_ "goperl: unknown op class %d", cls);
        }
        if (type == OP_CUSTOM && o && cls != GOPERL_OPC_RAW) {
            static const OPclass built[] = {OPclass_BASEOP, OPclass_UNOP, OPclass_BINOP,
                                            OPclass_LISTOP, OPclass_LOGOP, OPclass_SVOP,
                                            OPclass_PVOP, OPclass_SVOP, OPclass_METHOP};
            /* a constructor may wrap the op it made (newLOGOP) */
            OP *made = o->op_type == OP_CUSTOM ? o
                       : ((o->op_flags & OPf_KIDS) && cUNOPx(o)->op_first &&
                          cUNOPx(o)->op_first->op_type == OP_CUSTOM)
                           ? cUNOPx(o)->op_first
                           : NULL;
            if (made && cls >= 0 && cls < (int)(sizeof built / sizeof built[0]))
                g_custom_built[made] = built[cls];
        }
        return (uint64_t)(uintptr_t)o;
    }
    case GOPERL_OP_OPTREE_MISC: { /* a = sel<<32|aux, b = aux2,
                 * s = op tokens (u64 each) */
        int sel = (int)(a >> 32);
        I32 aux = (I32)(uint32_t)(a & 0xFFFFFFFFu);
        uint64_t k1 = 0, k2 = 0, k3 = 0;
        if (s) {
            memcpy(&k1, s, 8);
            memcpy(&k2, s + 8, 8);
            if (sel == GOPERL_OPM_SIBLING_SPLICE) memcpy(&k3, s + 16, 8);
        }
        OP *o1 = GOPERL_MEMARG(OP, k1);
        OP *o2 = GOPERL_MEMARG(OP, k2);
        switch (sel) {
        case GOPERL_OPM_APPEND_ELEM:
            return (uint64_t)(uintptr_t)op_append_elem(aux, o1, o2);
        case GOPERL_OPM_APPEND_LIST:
            return (uint64_t)(uintptr_t)op_append_list(aux, o1, o2);
        case GOPERL_OPM_PREPEND_ELEM:
            return (uint64_t)(uintptr_t)op_prepend_elem(aux, o1, o2);
        case GOPERL_OPM_CONVERT_LIST:
            return (uint64_t)(uintptr_t)op_convert_list(aux, (I32)(uint32_t)b,
                                                        o1);
        case GOPERL_OPM_CONTEXTUALIZE:
            return (uint64_t)(uintptr_t)op_contextualize(o1, aux);
        case GOPERL_OPM_SCOPE: return (uint64_t)(uintptr_t)op_scope(o1);
        case GOPERL_OPM_LINKLIST:
            if (!o1) return 0;
            return (uint64_t)(uintptr_t)(o1->op_next ? o1->op_next
                                                     : op_linklist(o1));
        case GOPERL_OPM_FREE:
            op_free(o1);
            return 0;
        case GOPERL_OPM_NULL:
            op_null(o1);
            return 0;
        case GOPERL_OPM_SIBLING_SPLICE:
            return (uint64_t)(uintptr_t)op_sibling_splice(
                o1, o2, (int)aux, GOPERL_MEMARG(OP, k3));
        }
        Perl_croak(aTHX_ "goperl: unknown optree helper %d", sel);
    }
    case GOPERL_OP_OP_SET: { /* a = op token, b = the value, c = the field
                 * (with op_moresib in bit 16 for OPF_SIBPARENT) */
        OP *o = GOPERL_MEMARG(OP, a);
        int sel = (int)c;
        uint64_t val = b;
        switch (sel & 0xFFFF) {
        case GOPERL_OPF_NEXT: o->op_next = GOPERL_MEMARG(OP, val); return 0;
        case GOPERL_OPF_SIBPARENT:
            o->op_sibparent = GOPERL_MEMARG(OP, val);
            o->op_moresib = (sel >> 16) & 1;
            return 0;
        case GOPERL_OPF_FIRST: cUNOPx(o)->op_first = GOPERL_MEMARG(OP, val); return 0;
        case GOPERL_OPF_LAST: cBINOPx(o)->op_last = GOPERL_MEMARG(OP, val); return 0;
        case GOPERL_OPF_OTHER: cLOGOPx(o)->op_other = GOPERL_MEMARG(OP, val); return 0;
        case GOPERL_OPF_TARG: o->op_targ = (PADOFFSET)val; return 0;
        case GOPERL_OPF_TYPE: o->op_type = (OPCODE)val; return 0;
        case GOPERL_OPF_FLAGS: o->op_flags = (U8)val; return 0;
        case GOPERL_OPF_PRIVATE: o->op_private = (U8)val; return 0;
        case GOPERL_OPF_PPADDR_HOOK: /* val: the registered custom op, or 0 */
            o->op_ppaddr = (val >= 1 && val <= g_xop_n) ? goperl_pp_tramps[val - 1]
                                                       : goperl_pp_hooked_op;
            return 0;
        case GOPERL_OPF_PPADDR_OWN: o->op_ppaddr = PL_ppaddr[o->op_type]; return 0;
        }
        Perl_croak(aTHX_ "goperl: unknown op field %d", sel);
    }
    case GOPERL_OP_LEX: { /* a = selector, b = per-selector argument */
        if (!PL_parser)
            Perl_croak(aTHX_ "goperl: lexer call outside of parsing");
        switch ((int)a) {
        case GOPERL_LEX_READ_SPACE: lex_read_space((U32)b); return 0;
        case GOPERL_LEX_PEEK_UNICHAR:
            return (uint64_t)(int64_t)lex_peek_unichar((U32)b);
        case GOPERL_LEX_READ_UNICHAR:
            return (uint64_t)(int64_t)lex_read_unichar((U32)b);
        case GOPERL_LEX_READ_TO:
            lex_read_to((char *)(uintptr_t)b);
            return 0;
        case GOPERL_LEX_BUFUTF8: return lex_bufutf8() ? 1 : 0;
        case GOPERL_LEX_STUFF_PVN: /* b = flags<<32|len */
            lex_stuff_pvn(s ? s : "", (STRLEN)(uint32_t)b, (U32)(b >> 32));
            return 0;
        case GOPERL_LEX_UNSTUFF: /* b = the address in the buffer to discard to */
            lex_unstuff((char *)(uintptr_t)b);
            return 0;
        case GOPERL_LEX_GROW_LINESTR: /* b = the length -> the buffer's address */
            return (uint64_t)(uintptr_t)lex_grow_linestr((STRLEN)b);
        case GOPERL_LEX_NEXT_CHUNK: /* b = flags */
            return lex_next_chunk((U32)b) ? 1 : 0;
        case GOPERL_LEX_DISCARD_TO: /* b = an address in the buffer */
            lex_discard_to((char *)(uintptr_t)b);
            return 0;
        case GOPERL_LEX_SCAN_WORD: {
            static struct {
                goperl_gword_t end;
                goperl_gword_t len;
                char word[256];
            } r;
            goperl_gword_t w[2] = {0, 0};
            if (s) memcpy(w, s, sizeof w);
            STRLEN destlen = w[0] < sizeof r.word ? (STRLEN)w[0] : sizeof r.word;
            STRLEN len = 0;
            char *end = Perl_scan_word(aTHX_ (char *)(uintptr_t)b, r.word,
                                       destlen, (int)w[1], &len);
            r.end = GOPERL_ADDR(end);
            r.len = (goperl_gword_t)len;
            return (uint64_t)(uintptr_t)&r;
        }
        case GOPERL_LEX_SCAN_STR: {
            static struct {
                goperl_gword_t end;
                goperl_gword_t delim;
            } r;
            goperl_gword_t w[3] = {0, 0, 0};
            if (s) memcpy(w, s, sizeof w);
            char *delim = NULL;
            char *end = Perl_scan_str(aTHX_ (char *)(uintptr_t)b, (int)w[0],
                                      (int)w[1], (int)w[2], &delim);
            r.end = GOPERL_ADDR(end);
            r.delim = GOPERL_ADDR(delim);
            return (uint64_t)(uintptr_t)&r;
        }
        case GOPERL_LEX_SKIPSPACE: {
            goperl_gword_t flags = 0;
            if (s) memcpy(&flags, s, sizeof flags);
            return (uint64_t)(uintptr_t)Perl_skipspace_flags(
                aTHX_ (char *)(uintptr_t)b, (U32)flags);
        }
        }
        Perl_croak(aTHX_ "goperl: unknown lexer call %d", (int)a);
    }
    case GOPERL_OP_PARSE: { /* a = selector, b = flags -> op token */
        if (!PL_parser)
            Perl_croak(aTHX_ "goperl: parse call outside of parsing");
        U32 flags = (U32)b;
        OP *o = NULL;
        switch ((int)a) {
        case GOPERL_PARSE_BLOCK: o = parse_block(flags); break;
        case GOPERL_PARSE_TERMEXPR: o = parse_termexpr(flags); break;
        case GOPERL_PARSE_LISTEXPR: o = parse_listexpr(flags); break;
        case GOPERL_PARSE_ARITHEXPR: o = parse_arithexpr(flags); break;
        case GOPERL_PARSE_FULLEXPR: o = parse_fullexpr(flags); break;
        case GOPERL_PARSE_FULLSTMT: o = parse_fullstmt(flags); break;
        case GOPERL_PARSE_STMTSEQ: o = parse_stmtseq(flags); break;
        case GOPERL_PARSE_BARESTMT: o = parse_barestmt(flags); break;
        default: Perl_croak(aTHX_ "goperl: unknown parse call %d", (int)a);
        }
        return (uint64_t)(uintptr_t)o;
    }
    case GOPERL_OP_PARSER_GET: { /* a = field id */
        switch ((int)a) {
        case GOPERL_PARSER_PRESENT: return PL_parser ? 1 : 0;
        case GOPERL_PARSER_LINESTR:
            return (uint64_t)(uintptr_t)PL_parser->linestr;
        case GOPERL_PARSER_BUFPTR:
            return (uint64_t)(uintptr_t)PL_parser->bufptr;
        case GOPERL_PARSER_BUFEND:
            return (uint64_t)(uintptr_t)PL_parser->bufend;
        case GOPERL_PARSER_OLDBUFPTR:
            return (uint64_t)(uintptr_t)PL_parser->oldbufptr;
        case GOPERL_PARSER_LINESTART:
            return (uint64_t)(uintptr_t)PL_parser->linestart;
        case GOPERL_PARSER_ERROR_COUNT:
            return (uint64_t)(uint32_t)PL_parser->error_count;
        case GOPERL_PARSER_IN_MY: return (uint64_t)PL_parser->in_my;
        case GOPERL_PARSER_LEX_FLAGS: return (uint64_t)PL_parser->lex_flags;
        case GOPERL_PARSER_PREAMBLING:
            return (uint64_t)PL_parser->preambling;
        case GOPERL_PARSER_PV:
            return (uint64_t)(uintptr_t)SvPVX(PL_parser->linestr);
        }
        return 0;
    }
    case GOPERL_OP_PARSER_SET: { /* a = field id, b = value */
        if (!PL_parser) return 0;
        switch ((int)a) {
        case 100 + GOPERL_PARSERW_EXPECT: PL_parser->expect = (U8)b; return 0;
        case 100 + GOPERL_PARSERW_COPLINE: PL_parser->copline = (line_t)b; return 0;
        case 100 + GOPERL_PARSERW_PREAMBLING: PL_parser->preambling = (line_t)b; return 0;
        case 100 + GOPERL_PARSERW_IN_MY_STASH:
            PL_parser->in_my_stash = GOPERL_HVARG(b);
            return 0;
        case GOPERL_PARSER_BUFPTR:
            PL_parser->bufptr = (char *)(uintptr_t)b;
            return 0;
        case GOPERL_PARSER_ERROR_COUNT:
            PL_parser->error_count = (I32)(int32_t)(uint32_t)b;
            return 0;
        case GOPERL_PARSER_IN_MY:
            PL_parser->in_my = (U16)b;
            return 0;
        /* the fields native code that lexes by itself stores into (a
         * scanner of its own over the buffer): pointers are addresses of
         * this memory, values are stored as they are, as C code compiled
         * against perl's parser.h does */
#define GOPERL_PSET_PTR(w, field) \
        case 100 + (w): PL_parser->field = (char *)(uintptr_t)b; return 0;
        GOPERL_PSET_PTR(GOPERL_PARSERW_BUFPTR, bufptr)
        GOPERL_PSET_PTR(GOPERL_PARSERW_BUFEND, bufend)
        GOPERL_PSET_PTR(GOPERL_PARSERW_OLDBUFPTR, oldbufptr)
        GOPERL_PSET_PTR(GOPERL_PARSERW_OLDOLDBUFPTR, oldoldbufptr)
        GOPERL_PSET_PTR(GOPERL_PARSERW_LINESTART, linestart)
        GOPERL_PSET_PTR(GOPERL_PARSERW_LAST_UNI, last_uni)
        GOPERL_PSET_PTR(GOPERL_PARSERW_LAST_LOP, last_lop)
        GOPERL_PSET_PTR(GOPERL_PARSERW_LEX_BRACKSTACK, lex_brackstack)
#undef GOPERL_PSET_PTR
        case 100 + GOPERL_PARSERW_LEX_STUFF:
            PL_parser->lex_stuff = GOPERL_SVARG(b);
            return 0;
        case 100 + GOPERL_PARSERW_LEX_REPL:
            PL_parser->lex_repl = GOPERL_SVARG(b);
            return 0;
        case 100 + GOPERL_PARSERW_RSFP:
            PL_parser->rsfp = GOPERL_MEMARG(PerlIO, b);
            return 0;
        case 100 + GOPERL_PARSERW_RSFP_FILTERS:
            PL_parser->rsfp_filters = GOPERL_AVARG(b);
            return 0;
        case 100 + GOPERL_PARSERW_ERROR_COUNT: PL_parser->error_count = (I32)(int32_t)(uint32_t)b; return 0;
        case 100 + GOPERL_PARSERW_LEX_STATE: PL_parser->lex_state = (U8)b; return 0;
        case 100 + GOPERL_PARSERW_LEX_BRACKETS: PL_parser->lex_brackets = (I32)(int32_t)(uint32_t)b; return 0;
        case 100 + GOPERL_PARSERW_LEX_FORMBRACK: PL_parser->lex_formbrack = (I32)(int32_t)(uint32_t)b; return 0;
        case 100 + GOPERL_PARSERW_LEX_INWHAT: PL_parser->lex_inwhat = (U16)b; return 0;
        case 100 + GOPERL_PARSERW_MULTI_START: PL_parser->multi_start = (line_t)b; return 0;
        case 100 + GOPERL_PARSERW_MULTI_END: PL_parser->multi_end = (line_t)b; return 0;
        case 100 + GOPERL_PARSERW_MULTI_OPEN: PL_parser->multi_open = (UV)b; return 0;
        case 100 + GOPERL_PARSERW_MULTI_CLOSE: PL_parser->multi_close = (UV)b; return 0;
        case 100 + GOPERL_PARSERW_LAST_LOP_OP: PL_parser->last_lop_op = (OPCODE)b; return 0;
        }
        return 0;
    }
    case GOPERL_OP_BLOCK: { /* a: 0 = block_start(b=full) -> floor,
                 * 1 = block_end(b=floor, s=[u64 seq]) -> op token */
        if ((int)a == 0) return (uint64_t)(uint32_t)block_start((int)b);
        uint64_t k1 = 0;
        if (s) memcpy(&k1, s, 8);
        return (uint64_t)(uintptr_t)block_end((I32)(uint32_t)b, GOPERL_MEMARG(OP, k1));
    }
    case GOPERL_OP_PAD: { /* a = selector */
        switch ((int)a) {
        case GOPERL_PAD_ALLOC:
            return (uint64_t)pad_alloc((I32)(uint32_t)(b >> 32),
                                       (U32)(uint32_t)b);
        case GOPERL_PAD_ADD_NAME_PVN: /* b = flags<<32|len, s = name */
            return (uint64_t)pad_add_name_pvn(s ? s : "", (STRLEN)(uint32_t)b,
                                              (U32)(b >> 32), NULL, NULL);
        case GOPERL_PAD_FINDMY_PVN: /* b = flags<<32|len, s = name */
            return (uint64_t)pad_findmy_pvn(s ? s : "", (STRLEN)(uint32_t)b,
                                            (U32)(b >> 32));
        case GOPERL_PAD_INTRO_MY: return (uint64_t)intro_my();
        case GOPERL_PAD_SETSV: { /* b = sv token, c = pad index */
            PADOFFSET ix = (PADOFFSET)c;
            SV *sv = GOPERL_SVARG(b);
            PAD_SETSV(ix, sv);
            return 0;
        }
        case GOPERL_PAD_SV_FETCH: /* b = pad index (the RUNNING sub's pad) */
            return (uint64_t)(uintptr_t)PAD_SV((PADOFFSET)b);
        case GOPERL_PAD_CV_PADLIST: {
            CV *cv = GOPERL_CVARG(b);
            if (!cv || (SvTYPE((SV *)cv) != SVt_PVCV && SvTYPE((SV *)cv) != SVt_PVFM) ||
                CvISXSUB(cv))
                return 0;
            return (uint64_t)(uintptr_t)CvPADLIST(cv);
        }
        case GOPERL_PAD_PADLIST: {
            static std::vector<goperl_gword_t> words;
            PADLIST *pl = GOPERL_MEMARG(PADLIST, b);
            if (!pl) return 0;
            words.clear();
            words.push_back((goperl_gword_t)(int64_t)PadlistMAX(pl));
            words.push_back(GOPERL_ADDR(PadlistNAMES(pl)));
            for (SSize_t i = 0; i <= PadlistMAX(pl); i++)
                words.push_back(GOPERL_ADDR(PadlistARRAY(pl)[i]));
            return (uint64_t)(uintptr_t)words.data();
        }
        case GOPERL_PAD_NAMELIST: {
            static std::vector<goperl_gword_t> words;
            PADNAMELIST *pnl = GOPERL_MEMARG(PADNAMELIST, b);
            if (!pnl) return 0;
            words.clear();
            words.push_back((goperl_gword_t)(int64_t)PadnamelistMAX(pnl));
            words.push_back((goperl_gword_t)(int64_t)PadnamelistMAXNAMED(pnl));
            for (SSize_t i = 0; i <= PadnamelistMAX(pnl); i++)
                words.push_back(GOPERL_ADDR(PadnamelistARRAY(pnl)[i]));
            return (uint64_t)(uintptr_t)words.data();
        }
        case GOPERL_PAD_NAME: {
            static goperl_gword_t w[8];
            PADNAME *pn = GOPERL_MEMARG(PADNAME, b);
            if (!pn) return 0;
            w[0] = GOPERL_ADDR(PadnamePV(pn));
            w[1] = (goperl_gword_t)PadnameLEN(pn);
            w[2] = (goperl_gword_t)PadnameFLAGS(pn);
            w[3] = (goperl_gword_t)PadnameREFCNT(pn);
            w[4] = (goperl_gword_t)COP_SEQ_RANGE_LOW(pn);
            w[5] = (goperl_gword_t)COP_SEQ_RANGE_HIGH(pn);
            w[6] = GOPERL_ADDR(PadnameOURSTASH(pn));
            w[7] = GOPERL_ADDR(PadnameTYPE(pn));
            return (uint64_t)(uintptr_t)w;
        }
        case GOPERL_PAD_SAVE_CLEARSV:
            save_clearsv(&PAD_SVl((PADOFFSET)b));
            return 0;
        case GOPERL_PAD_NAMELIST_STORE: {
            goperl_gword_t pn = 0;
            if (s && slen >= sizeof pn) memcpy(&pn, s, sizeof pn);
            padnamelist_store(GOPERL_MEMARG(PADNAMELIST, b), (SSize_t)(int64_t)c,
                              GOPERL_MEMARG(PADNAME, pn));
            return 0;
        }
        }
        Perl_croak(aTHX_ "goperl: unknown pad call %d", (int)a);
    }
    case GOPERL_OP_KEYWORD_ENABLE: { /* a: 0 = keyword chain, 1 = infix */
        if ((int)a == 0) {
            if (PL_keyword_plugin != goperl_keyword_plugin) {
                g_next_keyword_plugin = PL_keyword_plugin;
                PL_keyword_plugin = goperl_keyword_plugin;
            }
        } else {
            if (PL_infix_plugin != goperl_infix_plugin) {
                g_next_infix_plugin = PL_infix_plugin;
                PL_infix_plugin = goperl_infix_plugin;
            }
        }
        return 0;
    }
    case GOPERL_OP_CHARCLASS: { /* a = class id, b = code point */
        UV cp = (UV)b;
        switch ((int)a) {
        case GOPERL_CC_IDFIRST: return isIDFIRST_uni(cp) ? 1 : 0;
        case GOPERL_CC_IDCONT: return isIDCONT_uni(cp) ? 1 : 0;
        case GOPERL_CC_WORDCHAR: return isWORDCHAR_uni(cp) ? 1 : 0;
        case GOPERL_CC_SPACE: return isSPACE_uni(cp) ? 1 : 0;
        case GOPERL_CC_DIGIT: return isDIGIT_uni(cp) ? 1 : 0;
        case GOPERL_CC_ALPHA: return isALPHA_uni(cp) ? 1 : 0;
        case GOPERL_CC_UPPER: return isUPPER_uni(cp) ? 1 : 0;
        }
        return 0;
    }
    case GOPERL_OP_SAVE_MISC: { /* a = a GOPERL_SAVE_ selector; b = sv token
                 * (save_gp: c = empty) */
        SV *sv = GOPERL_SVARG(b);
        switch ((int)a) {
        case GOPERL_SAVE_FREESV: save_freesv(sv); return 0;
        case GOPERL_SAVE_MORTALIZESV: save_mortalizesv(sv); return 0;
        case GOPERL_SAVE_ITEM: save_item(sv); return 0;
        case GOPERL_SAVE_ARY: return (uint64_t)(uintptr_t)(SV *)save_ary((GV *)sv);
        case GOPERL_SAVE_HASH: return (uint64_t)(uintptr_t)(SV *)save_hash((GV *)sv);
        case GOPERL_SAVE_GP: save_gp((GV *)sv, (I32)(int64_t)c); return 0;
        case GOPERL_SAVE_HINTS: SAVEHINTS(); return 0;
        case GOPERL_SAVE_COMPILE_WARNINGS: SAVECOMPILEWARNINGS(); return 0;
        case GOPERL_SAVE_DEFSV: SAVE_DEFSV; return 0;
        }
        Perl_croak(aTHX_ "goperl: unknown save call %d", (int)a);
    }
    case GOPERL_OP_SV_CLASSIFY: { /* a = sv token; b = len<<32|flags<<8|mode;
                 * s = name bytes (mode 0) or an 8-byte sv token */
        SV *sv = GOPERL_SVARG(a);
        int mode = (int)(b & 0xFF);
        U32 flags = (U32)((b >> 8) & 0xFFFFFF);
        STRLEN len = (STRLEN)(uint32_t)(b >> 32);
        uint64_t k1 = 0;
        if (mode == 1 || mode == 2 || mode == 5 || mode == 6) {
            if (s) memcpy(&k1, s, 8);
        }
        SV *other = GOPERL_SVARG(k1);
        switch (mode) {
        case 0: return sv_derived_from_pvn(sv, s ? s : "", len, flags) ? 1 : 0;
        case 1: return sv_derived_from_sv(sv, other, flags) ? 1 : 0;
        case 2: return sv_isa_sv(sv, other) ? 1 : 0;
        case 3: {
            HV *hv = (HV *)sv;
            return (uint64_t)(uint32_t)HvNAMELEN_get(hv);
        }
        case 4: return HvNAMEUTF8((HV *)sv) ? 1 : 0;
        case 5: return sv_numeq_flags(sv, other, flags) ? 1 : 0;
        case 6: return sv_streq_flags(sv, other, flags) ? 1 : 0;
        }
        Perl_croak(aTHX_ "goperl: unknown sv classify mode %d", mode);
    }
    case GOPERL_OP_SV_FLAGS: { /* a = sv; b = clear<<32 | set; -> the flags
                 * after the change. SvFLAGS itself and every SvXXX_on/_off
                 * macro of sv.h are this one operation. */
        SV *sv = goperl_sv_arg(aTHX_ a, op, true);
        U32 clear = (U32)(b >> 32), set = (U32)b;
        if ((set & (SVf_POK | SVp_POK)) && SvTYPE(sv) >= SVt_PV &&
            goperl_pv_host_string(aTHX_ sv, (SvFLAGS(sv) & ~clear) | set, SvPVX(sv), SvCUR(sv)))
            goperl_pv_host_refuse(aTHX);
        if (clear || set) SvFLAGS(sv) = (SvFLAGS(sv) & ~clear) | set;
        return (uint64_t)SvFLAGS(sv);
    }
    case GOPERL_OP_SV_SLOT_SET: { /* a = sv, c = a GOPERL_SVSLOT_ selector;
                 * b = the value: the SvXXX_set macros of sv.h */
        SV *sv = GOPERL_SVARG(a);
        switch ((int)c) {
        case GOPERL_SVSLOT_RV: SvRV_set(sv, GOPERL_STORED(b)); return 0;
        case GOPERL_SVSLOT_STASH: SvSTASH_set(sv, (HV *)GOPERL_STORED(b)); return 0;
        /* The SvXXX_set macros write into the body the SV has: one that
         * is too small for the slot is grown first, as the callers of
         * these macros do with SvUPGRADE. */
        case GOPERL_SVSLOT_IV:
            if (SvTYPE(sv) != SVt_IV && SvTYPE(sv) < SVt_PVIV) sv_upgrade(sv, SVt_PVIV);
            SvIV_set(sv, (IV)(int64_t)b);
            return 0;
        case GOPERL_SVSLOT_UV:
            if (SvTYPE(sv) != SVt_IV && SvTYPE(sv) < SVt_PVIV) sv_upgrade(sv, SVt_PVIV);
            SvUV_set(sv, (UV)b);
            return 0;
        case GOPERL_SVSLOT_NV: {
            NV nv;
            memcpy(&nv, &b, sizeof nv);
            if (SvTYPE(sv) != SVt_NV && SvTYPE(sv) < SVt_PVNV)
                sv_upgrade(sv, SvTYPE(sv) == SVt_NULL ? SVt_NV : SVt_PVNV);
            SvNV_set(sv, nv);
            return 0;
        }
        case GOPERL_SVSLOT_PV:
            if (goperl_pv_host_string(aTHX_ sv, SvFLAGS(sv), (char *)(uintptr_t)b, SvCUR(sv)))
                goperl_pv_host_refuse(aTHX);
            SvPV_set(sv, (char *)(uintptr_t)b);
            return 0;
        case GOPERL_SVSLOT_LEN: SvLEN_set(sv, (STRLEN)b); return 0;
        case GOPERL_SVSLOT_GV_STASH:
            if (isGV_with_GP(sv)) GvSTASH((GV *)sv) = (HV *)GOPERL_STORED(b);
            return 0;
        case GOPERL_SVSLOT_GV_CVGEN:
            if (isGV_with_GP(sv)) GvCVGEN((GV *)sv) = (U32)b;
            return 0;
        }
        Perl_croak(aTHX_ "goperl: unknown sv slot %d", (int)c);
    }
    case GOPERL_OP_SV_SLOT_GET: { /* a = sv, c = a GOPERL_SVSLOT_ selector
                 * -> what the body slot holds, as the SvXXX_set op takes
                 * it. A body too small to have the slot reads as zero. */
        SV *sv = goperl_sv_arg(aTHX_ a, op, true); /* also a reference just freed */
        const svtype t = SvTYPE(sv);
        switch ((int)c) {
        case GOPERL_SVSLOT_RV:
            /* the slot itself, as SvRV reads it, whatever the flags say:
             * it still holds the target after SvROK_off, and code that
             * reads it from a reference that was just freed gets what
             * perl gives it there (Doubly::Linked does). */
            return (uint64_t)(uintptr_t)sv->sv_u.svu_rv;
        case GOPERL_SVSLOT_STASH:
            return t >= SVt_PVMG ? (uint64_t)(uintptr_t)(SV *)SvSTASH(sv) : 0;
        case GOPERL_SVSLOT_IV:
            if (t != SVt_IV && t < SVt_PVIV) return 0;
            if (SvROK(sv) || isGV_with_GP(sv)) return 0;
            return (uint64_t)(int64_t)SvIVX(sv);
        case GOPERL_SVSLOT_UV:
            if (t != SVt_IV && t < SVt_PVIV) return 0;
            if (SvROK(sv) || isGV_with_GP(sv)) return 0;
            return (uint64_t)SvUVX(sv);
        case GOPERL_SVSLOT_NV: {
            if (isGV_with_GP(sv)) return 0;
            uint64_t bits;
            /* a body without the slot has no bits to give: its value */
            const NV nv = (t != SVt_NV && t < SVt_PVNV) ? SvNV_nomg(sv) : SvNVX(sv);
            memcpy(&bits, &nv, sizeof bits);
            return bits;
        }
        case GOPERL_SVSLOT_GV_STASH:
            return isGV_with_GP(sv) ? (uint64_t)(uintptr_t)(SV *)GvSTASH((GV *)sv) : 0;
        case GOPERL_SVSLOT_GV_CVGEN:
            return isGV_with_GP(sv) ? (uint64_t)GvCVGEN((GV *)sv) : 0;
        }
        Perl_croak(aTHX_ "goperl: unknown sv slot %d", (int)c);
    }
    case GOPERL_OP_CALLER_CX: { /* a = level, b bit 0: the frame the debugger
                 * hides -> the stackinfo token, or 0 when there is no such
                 * frame; second: the context index + 1 */
        const PERL_CONTEXT *dbcx = NULL;
        const PERL_CONTEXT *cx = caller_cx((I32)(int64_t)a, &dbcx);
        if (b & 1) cx = dbcx;
        if (!cx) return goperl_two(0, 0);
        for (PERL_SI *si = PL_curstackinfo; si; si = si->si_prev) {
            if (cx >= si->si_cxstack && cx <= si->si_cxstack + si->si_cxix)
                return goperl_two(GOPERL_ADDR(si), (uint64_t)(cx - si->si_cxstack + 1));
        }
        return goperl_two(0, 0);
    }
    case GOPERL_OP_MEM_FREE: /* a = a block of the interpreter's allocator
                 * (the string buffer a module took from an SV) */
        Safefree((void *)(uintptr_t)a);
        return 0;
    case GOPERL_OP_MEM_REALLOC: { /* a = such a block, b = its new size */
        char *block = (char *)(uintptr_t)a;
        Renew(block, (size_t)b, char);
        return (uint64_t)(uintptr_t)block;
    }
    case GOPERL_OP_DO_OPENN: { /* a = gv; s = five words (as_raw, rawmode,
                 * rawperm, supplied handle, argument count), the arguments,
                 * a word each, then the mode text; b = the length of all of
                 * it */
        goperl_gword_t head[5];
        if (!s || b < sizeof head) Perl_croak(aTHX_ "goperl: do_openn: short request");
        memcpy(head, s, sizeof head);
        const size_t num = (size_t)head[4];
        if (num > b / 8 || b < sizeof head + num * 8)
            Perl_croak(aTHX_ "goperl: do_openn: short request");
        SV **svs = NULL;
        if (num) {
            Newx(svs, num, SV *);
            SAVEFREEPV(svs);
            for (size_t i = 0; i < num; i++) {
                goperl_gword_t tok;
                memcpy(&tok, s + sizeof head + i * 8, 8);
                svs[i] = GOPERL_SVARG(tok);
            }
        }
        const char *mode = s + sizeof head + num * 8;
        const size_t mode_len = (size_t)b - sizeof head - num * 8;
        return do_openn(GOPERL_GVARG(a), mode, (I32)mode_len, (int)head[0],
                        (int)head[1], (int)head[2],
                        GOPERL_MEMARG(PerlIO, head[3]), svs, (I32)num)
                   ? 1
                   : 0;
    }
    case GOPERL_OP_PERLIO: { /* a = the handle (or IO sv), c = a GOPERL_PIO_
                 * selector; b and s = the remaining arguments */
        PerlIO *f = GOPERL_MEMARG(PerlIO, a);
        const int fn = (int)(uint32_t)c;
        switch (fn) {
        case GOPERL_PIO_STDIN: return (uint64_t)(uintptr_t)PerlIO_stdin();
        case GOPERL_PIO_STDOUT: return (uint64_t)(uintptr_t)PerlIO_stdout();
        case GOPERL_PIO_STDERR: return (uint64_t)(uintptr_t)PerlIO_stderr();
        case GOPERL_PIO_FDOPEN: /* b = descriptor, s = mode */
            return (uint64_t)(uintptr_t)PerlIO_fdopen((int)(int64_t)b, s ? s : "r");
        case GOPERL_PIO_IFP: {
            IO *io = GOPERL_IOARG(a);
            if (!io || SvTYPE((SV *)io) != SVt_PVIO) return 0;
            return (uint64_t)(uintptr_t)IoIFP(io);
        }
        case GOPERL_PIO_OFP: {
            IO *io = GOPERL_IOARG(a);
            if (!io || SvTYPE((SV *)io) != SVt_PVIO) return 0;
            return (uint64_t)(uintptr_t)IoOFP(io);
        }
        case GOPERL_PIO_VALID: return PerlIOValid(f) ? 1 : 0;
        case GOPERL_PIO_LAYERS: {
            static std::vector<goperl_gword_t> w;
            w.assign(1, 0);
            for (PerlIO *l = f; l && *l; l = PerlIONext(l)) {
                w.push_back(GOPERL_ADDR(PerlIONext(l)));
                w.push_back(GOPERL_ADDR((*l)->tab->name));
                w.push_back((goperl_gword_t)(*l)->tab->kind);
                w.push_back((goperl_gword_t)(*l)->flags);
                if (b == 1) {
                    if ((*l)->tab->kind & PERLIO_K_BUFFERED) {
                        const PerlIOBuf *pb = PerlIOSelf(l, PerlIOBuf);
                        w.push_back(GOPERL_ADDR(pb->buf));
                        w.push_back(GOPERL_ADDR(pb->end));
                        w.push_back(GOPERL_ADDR(pb->ptr));
                        w.push_back((goperl_gword_t)pb->bufsiz);
                        w.push_back((goperl_gword_t)(int64_t)pb->posn);
                    } else {
                        w.insert(w.end(), 5, 0);
                    }
                }
                w[0]++;
            }
            return (uint64_t)(uintptr_t)w.data();
        }
        case GOPERL_PIO_FIND_FILE:
        case GOPERL_PIO_EXPORT_FILE: {
            static goperl_gword_t w[5];
            memset(w, 0, sizeof w);
            w[0] = (goperl_gword_t)-1;
            /* the stream of a :stdio layer the handle has, or a new one,
             * which is unbuffered: the host opens its own on the same
             * descriptor, also unbuffered, so that what the program and
             * the native code write reaches the descriptor in the order
             * they write it (through perl's one stream) */
            bool have = false;
            if (fn == GOPERL_PIO_FIND_FILE)
                for (PerlIO *l = f; l && *l; l = PerlIONext(l))
                    if ((*l)->tab == &PerlIO_stdio) have = true;
            FILE *stdio = have ? PerlIO_findFILE(f)
                               : PerlIO_exportFILE(f, (s && *s) ? s : NULL);
            if (stdio && !have) setvbuf(stdio, NULL, _IONBF, 0);
            const int fd = stdio ? fileno(stdio) : -1;
            struct stat st;
            if (fd >= 0 && fstat(fd, &st) == 0) {
                w[0] = (goperl_gword_t)fd;
                w[1] = GOPERL_ADDR(stdio);
                w[2] = (goperl_gword_t)st.st_dev;
                w[3] = (goperl_gword_t)st.st_ino;
                w[4] = (goperl_gword_t)(int64_t)lseek(fd, 0, SEEK_CUR);
            }
            return (uint64_t)(uintptr_t)w;
        }
        case GOPERL_PIO_RELEASE_FILE:
            PerlIO_releaseFILE(f, (FILE *)(uintptr_t)b);
            return 0;
        case GOPERL_PIO_OPENN: {
            goperl_gword_t head[4];
            if (!s || b < sizeof head) Perl_croak(aTHX_ "goperl: PerlIO_openn: short request");
            memcpy(head, s, sizeof head);
            const size_t narg = (size_t)head[3];
            if (narg > b / 8 || b < sizeof head + narg * 8 + 2)
                Perl_croak(aTHX_ "goperl: PerlIO_openn: short request");
            SV **args = NULL;
            if (narg) {
                Newx(args, narg, SV *);
                SAVEFREEPV(args);
                for (size_t i = 0; i < narg; i++) {
                    goperl_gword_t tok;
                    memcpy(&tok, s + sizeof head + i * 8, 8);
                    args[i] = GOPERL_SVARG(tok);
                }
            }
            const char *layers = s + sizeof head + narg * 8;
            const char *mode = layers + strlen(layers) + 1;
            return (uint64_t)(uintptr_t)PerlIO_openn(aTHX_ *layers ? layers : NULL, mode,
                                                     (int)(int64_t)head[0], (int)(int64_t)head[1],
                                                     (int)(int64_t)head[2], f, (int)narg, args);
        }
        }
        /* Everything below works on an open handle; on anything else it
         * fails the way the function does on a closed one. */
        if (!PerlIOValid(f)) {
            SETERRNO(EBADF, SS_IVCHAN);
            return fn == GOPERL_PIO_EOF ? 1 : (uint64_t)(int64_t)-1;
        }
        switch (fn) {
        case GOPERL_PIO_FILENO: return (uint64_t)(int64_t)PerlIO_fileno(f);
        case GOPERL_PIO_ISUTF8: return PerlIO_isutf8(f) ? 1 : 0;
        case GOPERL_PIO_READ: { /* b = count -> the buffer; second: the count
                 * read (negative on error). The bytes stay valid until the
                 * next read operation. */
            static char buf[65536];
            Size_t want = (Size_t)b;
            if (want > sizeof buf) want = sizeof buf;
            const SSize_t n = PerlIO_read(f, buf, want);
            return goperl_two(GOPERL_ADDR(buf), (uint64_t)(int64_t)n);
        }
        case GOPERL_PIO_EOF: return PerlIO_eof(f) ? 1 : 0;
        case GOPERL_PIO_ERROR: return PerlIO_error(f) ? 1 : 0;
        case GOPERL_PIO_CLEARERR: PerlIO_clearerr(f); return 0;
        case GOPERL_PIO_SEEK_SET:
            return (uint64_t)(int64_t)PerlIO_seek(f, (Off_t)(int64_t)b, SEEK_SET);
        case GOPERL_PIO_SEEK_CUR:
            return (uint64_t)(int64_t)PerlIO_seek(f, (Off_t)(int64_t)b, SEEK_CUR);
        case GOPERL_PIO_SEEK_END:
            return (uint64_t)(int64_t)PerlIO_seek(f, (Off_t)(int64_t)b, SEEK_END);
        case GOPERL_PIO_TELL: return (uint64_t)(int64_t)PerlIO_tell(f);
        case GOPERL_PIO_GETC: return (uint64_t)(int64_t)PerlIO_getc(f);
        case GOPERL_PIO_UNGETC:
            return (uint64_t)(int64_t)PerlIO_ungetc(f, (int)(int64_t)b);
        case GOPERL_PIO_UNREAD: /* s + b = bytes */
            return (uint64_t)(int64_t)PerlIO_unread(f, s ? s : "", (Size_t)b);
        case GOPERL_PIO_BINMODE: /* b = iotype<<32 | open mode, s = layers */
            return PerlIO_binmode(aTHX_ f, (int)(b >> 32), (int)(uint32_t)b,
                                  (s && *s) ? s : NULL) ? 1 : 0;
        case GOPERL_PIO_APPLY_LAYERS: { /* s = the mode, NUL, the layers */
            const char *mode = s ? s : "";
            const char *names = mode + strlen(mode) + 1;
            return (uint64_t)(int64_t)PerlIO_apply_layers(
                aTHX_ f, *mode ? mode : NULL, names);
        }
        case GOPERL_PIO_SETLINEBUF: PerlIO_setlinebuf(f); return 0;
        /* the read buffer of the handle; pointers are linear-memory ones */
        case GOPERL_PIO_FAST_GETS: return PerlIO_fast_gets(f) ? 1 : 0;
        case GOPERL_PIO_HAS_CNTPTR: return PerlIO_has_cntptr(f) ? 1 : 0;
        case GOPERL_PIO_CANSET_CNT: return PerlIO_canset_cnt(f) ? 1 : 0;
        case GOPERL_PIO_HAS_BASE: return PerlIO_has_base(f) ? 1 : 0;
        case GOPERL_PIO_GET_CNT: return (uint64_t)(int64_t)PerlIO_get_cnt(f);
        case GOPERL_PIO_GET_PTR: return (uint64_t)(uintptr_t)PerlIO_get_ptr(f);
        case GOPERL_PIO_GET_BASE: return (uint64_t)(uintptr_t)PerlIO_get_base(f);
        case GOPERL_PIO_GET_BUFSIZ: return (uint64_t)PerlIO_get_bufsiz(f);
        case GOPERL_PIO_SET_PTRCNT: /* b = ptr, c = count<<32 */
            PerlIO_set_ptrcnt(f, (STDCHAR *)(uintptr_t)b, (SSize_t)(int32_t)(uint32_t)(c >> 32));
            return 0;
        case GOPERL_PIO_FILL: return (uint64_t)(int64_t)PerlIO_fill(f);
        }
        Perl_croak(aTHX_ "goperl: unknown PerlIO call %d", fn);
    }
    case GOPERL_OP_SV_CALL: { /* a = sv, c = a GOPERL_SVCALL_ selector; b = the
                 * remaining argument of that API function (the other SV of
                 * the two-SV calls), and c's high half a 32-bit value that
                 * goes with it (hi: flags, a form) */
        SV *sv = (SV *)(uintptr_t)a;
        switch ((int)(uint32_t)c) { /* the selectors that take no scalar in a */
        case GOPERL_SVCALL_TAINT_ENV:
        case GOPERL_SVCALL_MAGIC_IS_VALUE:
            break;
        default: (void)GOPERL_SVARG(a);
        }
        U32 hi = (U32)(c >> 32);
        switch ((int)(uint32_t)c) {
        case GOPERL_SVCALL_MG_GET: return (uint64_t)(int64_t)mg_get(sv);
        case GOPERL_SVCALL_MG_CLEAR: return (uint64_t)(int64_t)mg_clear(sv);
        case GOPERL_SVCALL_MG_FREE: return (uint64_t)(int64_t)mg_free(sv);
        case GOPERL_SVCALL_MG_MAGICAL: /* b bit 0: native code's chain has
                 * no uvar magic any more (goperl_uvar_drop) */
            if (b & 1) goperl_uvar_drop(aTHX_ sv);
            mg_magical(sv);
            return 0;
        case GOPERL_SVCALL_MG_SIZE: return (uint64_t)mg_size(sv);
        case GOPERL_SVCALL_BACKOFF: sv_backoff(sv); return 0;
        case GOPERL_SVCALL_UNREF: sv_unref_flags(sv, (U32)b); return 0;
        case GOPERL_SVCALL_RVUNWEAKEN: sv_rvunweaken(sv); return 0;
        case GOPERL_SVCALL_UTF8_ENCODE: sv_utf8_encode(sv); return 0;
        case GOPERL_SVCALL_LEN_UTF8: return (uint64_t)sv_len_utf8(sv);
        case GOPERL_SVCALL_DEC:
            if (b) sv_dec_nomg(sv); else sv_dec(sv);
            return 0;
        case GOPERL_SVCALL_INC:
            if (b) sv_inc_nomg(sv); else sv_inc(sv);
            return 0;
        case GOPERL_SVCALL_TAINTED: return sv_tainted(sv) ? 1 : 0;
        case GOPERL_SVCALL_TAINT: sv_taint(sv); return 0;
        case GOPERL_SVCALL_UNTAINT: sv_untaint(sv); return 0;
        case GOPERL_SVCALL_REF: return (uint64_t)(uintptr_t)sv_ref(NULL, sv, (int)b);
        case GOPERL_SVCALL_CV_CONST_SV:
            return (uint64_t)(uintptr_t)cv_const_sv((const CV *)sv);
        case GOPERL_SVCALL_CV_UNDEF: cv_undef((CV *)sv); return 0;
        case GOPERL_SVCALL_POS_U2B:
            return (uint64_t)sv_pos_u2b_flags(sv, (STRLEN)b, NULL, SV_GMAGIC | SV_CONST_RETURN);
        case GOPERL_SVCALL_POS_B2U:
            return (uint64_t)sv_pos_b2u_flags(sv, (STRLEN)b, SV_GMAGIC | SV_CONST_RETURN);
        case GOPERL_SVCALL_SET_UNDEF: sv_set_undef(sv); return 0;
        case GOPERL_SVCALL_SHRINK_TO_CUR: SvPV_shrink_to_cur(sv); return 0;
        case GOPERL_SVCALL_AV_DELETE: /* b = index (two's complement), hi = flags */
            return (uint64_t)(uintptr_t)av_delete((AV *)sv, (SSize_t)(int64_t)b, (I32)hi);
        case GOPERL_SVCALL_VOK: return SvVOK(sv) ? 1 : 0;
        case GOPERL_SVCALL_HV_USEDKEYS: return (uint64_t)HvUSEDKEYS((HV *)sv);
        case GOPERL_SVCALL_HV_TOTALKEYS: return (uint64_t)HvTOTALKEYS((HV *)sv);
        case GOPERL_SVCALL_HV_FILL: return (uint64_t)HvFILL((HV *)sv);
        case GOPERL_SVCALL_HV_MAX: return (uint64_t)HvMAX((HV *)sv);
        case GOPERL_SVCALL_AV_MAX: return (uint64_t)(int64_t)AvMAX((AV *)sv);
        case GOPERL_SVCALL_AV_BODY: {
            if (SvTYPE(sv) != SVt_PVAV) return goperl_two(0, 0);
            AV *const av = (AV *)sv;
            const SSize_t n = AvFILLp(av) + 1;
            return goperl_two(GOPERL_ADDR(AvARRAY(av)), (uint64_t)(n > 0 ? n : 0));
        }
        case GOPERL_SVCALL_UTF8_UPGRADE:
            return (uint64_t)sv_utf8_upgrade_flags(sv, (I32)b);
        case GOPERL_SVCALL_NEWSVSV: /* sv may be NULL, as for newSVsv */
            return (uint64_t)(uintptr_t)newSVsv_flags(sv, (I32)b);
        case GOPERL_SVCALL_NEWAVAV: return (uint64_t)(uintptr_t)(SV *)newAVav((AV *)sv);
        case GOPERL_SVCALL_NEWAVHV: return (uint64_t)(uintptr_t)(SV *)newAVhv((HV *)sv);
        case GOPERL_SVCALL_SETRV_INC: sv_setrv_inc(sv, GOPERL_SVARG(b)); return 0;
        case GOPERL_SVCALL_SETRV_NOINC: sv_setrv_noinc(sv, GOPERL_SVARG(b)); return 0;
        case GOPERL_SVCALL_SETSV: /* b = source, hi = flags */
            sv_setsv_flags(sv, GOPERL_SVARG(b), (I32)hi);
            return 0;
        case GOPERL_SVCALL_CATSV:
            sv_catsv_flags(sv, GOPERL_SVARG(b), (I32)hi);
            return 0;
        case GOPERL_SVCALL_CMP:
            return (uint64_t)(int64_t)sv_cmp_flags(sv, GOPERL_SVARG(b), hi);
        case GOPERL_SVCALL_CMP_LOCALE:
            return (uint64_t)(int64_t)sv_cmp_locale_flags(sv, GOPERL_SVARG(b), hi);
        case GOPERL_SVCALL_NUMCMP:
            return (uint64_t)(int64_t)sv_numcmp_flags(sv, GOPERL_SVARG(b), hi);
        case GOPERL_SVCALL_EQ: return sv_eq_flags(sv, GOPERL_SVARG(b), hi) ? 1 : 0;
        case GOPERL_SVCALL_MRO_LINEAR_ISA:
            return (uint64_t)(uintptr_t)(SV *)mro_get_linear_isa((HV *)sv);
        case GOPERL_SVCALL_MRO_METHOD_CHANGED: mro_method_changed_in((HV *)sv); return 0;
        /* the value accessors with explicit get-magic control: b carries
         * SV_GMAGIC for the plain form, 0 for the _nomg form */
        case GOPERL_SVCALL_2BOOL:
            return ((b & SV_GMAGIC) ? SvTRUE(sv) : SvTRUE_nomg(sv)) ? 1 : 0;
        case GOPERL_SVCALL_2IV:
            return goperl_sv_integer(aTHX_ sv, false, !(b & SV_GMAGIC));
        case GOPERL_SVCALL_2UV:
            return goperl_sv_integer(aTHX_ sv, true, !(b & SV_GMAGIC));
        case GOPERL_SVCALL_2NV: {
            NV nv = (b & SV_GMAGIC) ? SvNV(sv) : SvNV_nomg(sv);
            uint64_t bits;
            memcpy(&bits, &nv, sizeof bits);
            return bits;
        }
        /* The string forms answer the address of the string (0 for none);
         * second: its length. */
        case GOPERL_SVCALL_2PV: {
            STRLEN len = 0;
            const char *p = SvPV_flags(sv, len, (I32)b);
            return goperl_two(GOPERL_ADDR(p), (uint64_t)len);
        }
        case GOPERL_SVCALL_2PVBYTE: { /* dies with "Wide character" when the
                 * string has no byte form */
            STRLEN len = 0;
            const char *p = (b & SV_GMAGIC) ? SvPVbyte(sv, len) : SvPVbyte_nomg(sv, len);
            return goperl_two(GOPERL_ADDR(p), (uint64_t)len);
        }
        case GOPERL_SVCALL_PV_AS: {
            STRLEN len = 0;
            const bool mg = (b & SV_GMAGIC) != 0;
            const char *p;
            switch (hi) {
            case 1: p = mg ? SvPVutf8(sv, len) : SvPVutf8_nomg(sv, len); break;
            case 2: p = mg ? SvPVbyte(sv, len) : SvPVbyte_nomg(sv, len); break;
            case 0x101:
                p = mg ? SvPVutf8_or_null(sv, len) : SvPVutf8_or_null_nomg(sv, len);
                break;
            case 0x102:
                p = mg ? SvPVbyte_or_null(sv, len) : SvPVbyte_or_null_nomg(sv, len);
                break;
            default: Perl_croak(aTHX_ "goperl: unknown string form %u", (unsigned)hi);
            }
            return goperl_two(GOPERL_ADDR(p), (uint64_t)len);
        }
        case GOPERL_SVCALL_PV_FORCE_AS: {
            STRLEN len = 0;
            char *p;
            switch (hi) {
            case 0: p = SvPV_force_flags(sv, len, (U32)(uint32_t)b); break;
            case 1: p = SvPVutf8_force(sv, len); break;
            case 2: p = SvPVbyte_force(sv, len); break;
            default: Perl_croak(aTHX_ "goperl: unknown string form %u", (unsigned)hi);
            }
            return goperl_two(GOPERL_ADDR(p), (uint64_t)len);
        }
        case GOPERL_SVCALL_UNI_DISPLAY: { /* sv = dsv; b = ssv, hi = pvlim<<8 |
                 * flags */
            const char *p = sv_uni_display(sv, GOPERL_SVARG(b), (STRLEN)(hi >> 8), (UV)(hi & 0xFF));
            return goperl_two(GOPERL_ADDR(p), (uint64_t)SvCUR(sv));
        }
        case GOPERL_SVCALL_HV_KSPLIT: hv_ksplit((HV *)sv, (IV)(int64_t)b); return 0;
        case GOPERL_SVCALL_TAINT_ENV: taint_env(); return 0;
        case GOPERL_SVCALL_TIED: {
            MAGIC *const mg = SvTIED_mg(sv, (int)(int64_t)b);
            if (!mg) return 0;
            return GOPERL_ADDR(SvTIED_obj(sv, mg));
        }
        case GOPERL_SVCALL_MAGIC_IS_VALUE:
            return PERL_MAGIC_TYPE_IS_VALUE_MAGIC((uint32_t)a) ? 1 : 0;
        case GOPERL_SVCALL_AMAGIC: { /* hi >> 24: the op native code has as
                * PL_op, whose context amagic_call takes */
            goperl_native_plop plop;
            goperl_native_plop_enter(aTHX_ &plop, (hi >> 24) & 7);
            SV *const res = amagic_call(sv, GOPERL_SVARG(b), (int)(hi & 0xff), (int)((hi >> 8) & 0xffff));
            goperl_native_plop_leave(aTHX_ &plop);
            return (uint64_t)(uintptr_t)res;
        }
        case GOPERL_SVCALL_CLEAR_UNDEF: /* SvOK_off with its body release */
            SvOK_off(sv);
            return 0;
        case GOPERL_SVCALL_IO_TYPE: return (uint64_t)(U8)IoTYPE((IO *)sv);
        case GOPERL_SVCALL_IO_FLAGS: return (uint64_t)IoFLAGS((IO *)sv);
        case GOPERL_SVCALL_IO_LINES: return (uint64_t)(int64_t)IoLINES((IO *)sv);
        case GOPERL_SVCALL_THROW: { /* the exception the running native XSUB
                 * ends with; a copy, as die takes one, which $SIG{__DIE__}
                 * sees now */
            SV *const thrown = goperl_die_hook(aTHX_ sv);
            SvREFCNT_dec(goperl_thrown_sv);
            goperl_thrown_sv = newSVsv(thrown);
            return 0;
        }
        case GOPERL_SVCALL_SETPV_BUFSIZE: /* b = buffer size, hi = cur -> PVX */
            return GOPERL_ADDR(sv_setpv_bufsize(sv, (STRLEN)hi, (STRLEN)b));
        }
        Perl_croak(aTHX_ "goperl: unknown sv call %d", (int)(uint32_t)c);
    }
    case GOPERL_OP_GV_CALL: { /* a = gv or cv, c = a GOPERL_GVCALL_ selector;
                 * b = the remaining argument, and c's high half a 32-bit
                 * value that goes with it (hi) */
        SV *sv = GOPERL_SVARG(a);
        GV *gv = (GV *)sv;
        CV *cv = (CV *)sv;
        const U32 hi = (U32)(c >> 32);
        int fn = (int)(uint32_t)c;
        if (fn < GOPERL_GVCALL_FIRST_CV) {
            if (!isGV_with_GP(sv)) return 0;
        } else if (SvTYPE(sv) != SVt_PVCV && SvTYPE(sv) != SVt_PVFM) {
            return 0;
        }
        switch (fn) {
        case GOPERL_GVCALL_SVN: return (uint64_t)(uintptr_t)GvSVn(gv);
        case GOPERL_GVCALL_AVN: return (uint64_t)(uintptr_t)(SV *)GvAVn(gv);
        case GOPERL_GVCALL_HVN: return (uint64_t)(uintptr_t)(SV *)GvHVn(gv);
        case GOPERL_GVCALL_ION: return (uint64_t)(uintptr_t)(SV *)GvIOn(gv);
        case GOPERL_GVCALL_CV_SET: GvCV_set(gv, GOPERL_CVARG(b)); return 0;
        case GOPERL_GVCALL_FLAGS: { /* b = set, hi = clear */
            U32 clear = hi, set = (U32)b;
            if (clear || set) GvFLAGS(gv) = (GvFLAGS(gv) & ~clear) | set;
            return (uint64_t)GvFLAGS(gv);
        }
        case GOPERL_GVCALL_CVGEN: return (uint64_t)GvCVGEN(gv);
        case GOPERL_GVCALL_CVGEN_SET: GvCVGEN(gv) = (U32)b; return 0;
        case GOPERL_GVCALL_STASH_SET:
            GvSTASH(gv) = GOPERL_HVARG(b);
            return 0;
        case GOPERL_GVCALL_FORM: return (uint64_t)(uintptr_t)(SV *)GvFORM(gv);
        case GOPERL_GVCALL_FULLNAME: /* b = sv, hi = keepmain<<1 | effective */
            if (hi & 1)
                gv_efullname4(GOPERL_SVARG(b), gv, NULL, cBOOL((hi >> 1) & 1));
            else
                gv_fullname4(GOPERL_SVARG(b), gv, NULL, cBOOL((hi >> 1) & 1));
            return 0;
        case GOPERL_GVCALL_GP: return (uint64_t)(uintptr_t)GvGP(gv);
        case GOPERL_GVCALL_FILE: { /* -> the name, or 0; second: its length */
            const char *file = GvFILE(gv);
            if (!file) return goperl_two(0, 0);
            return goperl_two(GOPERL_ADDR(file), (uint64_t)strlen(file));
        }
        case GOPERL_GVCALL_LINE: return (uint64_t)GvLINE(gv);
        case GOPERL_GVCALL_NAME_UTF8: return GvNAMEUTF8(gv) ? 1 : 0;
        case GOPERL_GVCALL_SV_SET: GvSV(gv) = GOPERL_SVARG(b); return 0;
        case GOPERL_GVCALL_AV_SET: GvAV(gv) = GOPERL_AVARG(b); return 0;
        case GOPERL_GVCALL_HV_SET: GvHV(gv) = GOPERL_HVARG(b); return 0;
        case GOPERL_GVCALL_IO_SET: GvIOp(gv) = GOPERL_IOARG(b); return 0;
        case GOPERL_GVCALL_CV_GV_SET: CvGV_set(cv, GOPERL_GVARG(b)); return 0;
        case GOPERL_GVCALL_CV_STASH_SET: CvSTASH_set(cv, GOPERL_HVARG(b)); return 0;
        case GOPERL_GVCALL_CV_FLAGS: { /* b = set, hi = clear */
            U32 clear = hi, set = (U32)b;
            if (clear || set) CvFLAGS(cv) = (CvFLAGS(cv) & ~clear) | set;
            return (uint64_t)CvFLAGS(cv);
        }
        case GOPERL_GVCALL_CV_OUTSIDE: return (uint64_t)(uintptr_t)(SV *)CvOUTSIDE(cv);
        case GOPERL_GVCALL_CV_PROTO: { /* -> the prototype, or 0; second: its
                 * length */
            if (!SvPOK(sv)) return goperl_two(0, 0);
            return goperl_two(GOPERL_ADDR(CvPROTO(cv)), (uint64_t)CvPROTOLEN(cv));
        }
        case GOPERL_GVCALL_CV_NATIVE: /* what `CvXSUB(cv) = fn` is for a
                 * native function: the generic thunk, which finds the
                 * function by its id. Whether the CV is an XSUB at all
                 * stays the caller's to say (CvISXSUB_on), as with perl. */
            CvXSUB(cv) = XS_goperl_native_thunk;
            CvXSUBANY(cv).any_i32 = (I32)(int64_t)b;
            return 0;
        }
        Perl_croak(aTHX_ "goperl: unknown gv call %d", fn);
    }
    case GOPERL_OP_SV_STR: { /* a = sv, c = a GOPERL_SVSTR_ selector;
                 * b = flags<<32 | byte length of s */
        SV *sv = (SV *)(uintptr_t)a;
        switch ((int)c) { /* the selectors that take no scalar in a */
        case GOPERL_SVSTR_GV_FETCHPVN:
        case GOPERL_SVSTR_GET_CVN:
        case GOPERL_SVSTR_WHICHSIG:
        case GOPERL_SVSTR_SETENV:
        case GOPERL_SVSTR_NEWPVN:
        case GOPERL_SVSTR_WARN:
        case GOPERL_SVSTR_THROW:
        case GOPERL_SVSTR_TAINT_PROPER:
        case GOPERL_SVSTR_WARNER: /* the category */
            break;
        case GOPERL_SVSTR_COP_HINT:
            (void)GOPERL_MEMARG(COP, a);
            break;
        default: (void)GOPERL_SVARG(a);
        }
        STRLEN len = (STRLEN)(uint32_t)b;
        U32 flags = (U32)(b >> 32);
        const char *str = s ? s : "";
        switch ((int)c) {
        case GOPERL_SVSTR_CATPVN: sv_catpvn_flags(sv, str, len, (I32)flags); return 0;
        case GOPERL_SVSTR_NEWSVRV: /* flags bit 0: no class */
            return (uint64_t)(uintptr_t)newSVrv(sv, (flags & 1) ? NULL : str);
        case GOPERL_SVSTR_DOES: return sv_does_pvn(sv, str, len, flags) ? 1 : 0;
        case GOPERL_SVSTR_FETCHMETH: /* flags = level (signed byte)<<24 | flags */
            return (uint64_t)(uintptr_t)gv_fetchmeth_pvn(
                (HV *)sv, str, len, (I32)(int8_t)(flags >> 24), flags & 0xFFFFFF);
        case GOPERL_SVSTR_FETCHMETH_AUTOLOAD:
            return (uint64_t)(uintptr_t)gv_fetchmeth_pvn_autoload(
                (HV *)sv, str, len, (I32)(int8_t)(flags >> 24), flags & 0xFFFFFF);
        case GOPERL_SVSTR_FETCHMETHOD:
            return (uint64_t)(uintptr_t)gv_fetchmethod_pvn_flags((HV *)sv, str, len, flags);
        case GOPERL_SVSTR_GV_FETCHPVN: /* the sv slot carries the svtype */
            return (uint64_t)(uintptr_t)gv_fetchpvn_flags(
                str, len, (I32)flags, (svtype)(uint32_t)a);
        case GOPERL_SVSTR_GET_CVN:
            return (uint64_t)(uintptr_t)(SV *)get_cvn_flags(str, len, (I32)flags);
        case GOPERL_SVSTR_WHICHSIG: return (uint64_t)(int64_t)whichsig_pvn(str, len);
        case GOPERL_SVSTR_SETENV: { /* str = name NUL value; flags bit 0:
                 * remove the variable */
            const char *val = str + strlen(str) + 1;
            my_setenv(str, (flags & 1) ? NULL : val);
            return 0;
        }
        case GOPERL_SVSTR_COP_HINT: /* sv slot = COP token, 0 for PL_curcop */
            return (uint64_t)(uintptr_t)cop_hints_fetch_pvn(
                sv ? (const COP *)sv : PL_curcop, str, len, 0, flags);
        case GOPERL_SVSTR_NEWPVN:
            return (uint64_t)(uintptr_t)newSVpvn_flags(
                str, len, ((flags & 1) ? SVf_UTF8 : 0) | ((flags & 2) ? SVs_TEMP : 0));
        case GOPERL_SVSTR_WARN: /* warn with the text as a string of its
                 * encoding; the place and $SIG{__WARN__} are perl's */
            warn_sv(newSVpvn_flags(str, len, SVs_TEMP | ((flags & 1) ? SVf_UTF8 : 0)));
            return 0;
        case GOPERL_SVSTR_THROW: { /* the message the running native XSUB
                 * dies with (see GOPERL_SVCALL_THROW) */
            SV *const thrown = goperl_die_hook(
                aTHX_ newSVpvn_flags(str, len, SVs_TEMP | ((flags & 1) ? SVf_UTF8 : 0)));
            SvREFCNT_dec(goperl_thrown_sv);
            goperl_thrown_sv = newSVsv(thrown);
            return 0;
        }
        case GOPERL_SVSTR_GV_NAME_SET:
            gv_name_set((GV *)sv, str, (U32)len, flags);
            return 0;
        case GOPERL_SVSTR_HV_NAME_SET: /* the name of a stash (a hash without
                 * one too); flags: HVhek_UTF8, and bit 31: no name (the
                 * name is taken away) */
            if (SvTYPE(sv) != SVt_PVHV) return 0;
            if (flags & 0x80000000u)
                hv_name_set((HV *)sv, NULL, 0, flags & ~0x80000000u);
            else
                hv_name_set((HV *)sv, str, (U32)len, flags);
            return 0;
        case GOPERL_SVSTR_WARNER: {
            SV *const text =
                newSVpvn_flags(str, len, SVs_TEMP | ((flags & 1) ? SVf_UTF8 : 0));
            const U32 category = (U32)(uintptr_t)sv;
            switch ((flags >> 1) & 3) {
            case 0: Perl_warner(aTHX_ category, "%" SVf, SVfARG(text)); break;
            case 1: Perl_ck_warner(aTHX_ category, "%" SVf, SVfARG(text)); break;
            default: Perl_ck_warner_d(aTHX_ category, "%" SVf, SVfARG(text)); break;
            }
            return 0;
        }
        case GOPERL_SVSTR_TAINT_PROPER: /* dies when tainted data is in
                 * use (PL_tainted), naming the operation */
            taint_proper(NULL, str);
            return 0;
        case GOPERL_SVSTR_SETPVN: /* what sv_setpvf does with its formatted
                 * text: the SV is emptied, keeping its encoding, and the
                 * text appended as what it is */
            SvPVCLEAR(sv);
            sv_catpvn_flags(sv, str, len, (flags & 1) ? SV_CATUTF8 : SV_CATBYTES);
            return 0;
        }
        Perl_croak(aTHX_ "goperl: unknown sv string call %d", (int)c);
    }
    case GOPERL_OP_FIELD: { /* a = token, c = a GOPERL_FIELD_ selector;
                 * b = the value of a store */
        SV *sv = (SV *)(uintptr_t)a;
        switch ((int)c) { /* the selectors that take no scalar in a */
        case GOPERL_FIELD_OP_SV:
        case GOPERL_FIELD_OP_GV:
            break;
        case GOPERL_FIELD_IS_FREED:
            (void)goperl_sv_arg(aTHX_ a, op, true);
            break;
        default: (void)GOPERL_SVARG(a);
        }
        switch ((int)c) {
        case GOPERL_FIELD_REFCNT_SET: /* the count itself: nothing is freed */
            SvREFCNT(sv) = (U32)b;
            return 0;
        case GOPERL_FIELD_HVAUX_BACKREFS:
            if (SvTYPE(sv) != SVt_PVHV || !HvHasAUX((HV *)sv)) return 0;
            return (uint64_t)(uintptr_t)(SV *)HvAUX((HV *)sv)->xhv_backreferences;
        case GOPERL_FIELD_HVAUX_RITER:
            if (SvTYPE(sv) != SVt_PVHV) return 0;
            return (uint64_t)(int64_t)HvRITER_get((HV *)sv);
        case GOPERL_FIELD_HVAUX_RITER_SET:
            if (SvTYPE(sv) == SVt_PVHV) HvRITER_set((HV *)sv, (I32)(int64_t)b);
            return 0;
        case GOPERL_FIELD_HVAUX_EITER:
            if (SvTYPE(sv) != SVt_PVHV) return 0;
            return (uint64_t)(uintptr_t)HvEITER_get((HV *)sv);
        case GOPERL_FIELD_HVAUX_EITER_SET:
            if (SvTYPE(sv) == SVt_PVHV)
                HvEITER_set((HV *)sv, GOPERL_MEMARG(HE, b));
            return 0;
        case GOPERL_FIELD_AV_FILLP:
            if (SvTYPE(sv) != SVt_PVAV) return (uint64_t)(int64_t)-1;
            return (uint64_t)(int64_t)AvFILLp((AV *)sv);
        case GOPERL_FIELD_AV_FILLP_SET:
            if (SvTYPE(sv) == SVt_PVAV) AvFILLp((AV *)sv) = (SSize_t)(int64_t)b;
            return 0;
        case GOPERL_FIELD_CV_DEPTH_SET:
            if (SvTYPE(sv) == SVt_PVCV || SvTYPE(sv) == SVt_PVFM)
                CvDEPTH((CV *)sv) = (I32)(int64_t)b;
            return 0;
        case GOPERL_FIELD_LV_TYPE:
            return SvTYPE(sv) == SVt_PVLV ? (uint64_t)(unsigned char)LvTYPE(sv) : 0;
        case GOPERL_FIELD_LV_TARG:
            return SvTYPE(sv) == SVt_PVLV ? (uint64_t)(uintptr_t)LvTARG(sv) : 0;
        case GOPERL_FIELD_LV_TYPE_SET:
            if (SvTYPE(sv) == SVt_PVLV) LvTYPE(sv) = (char)b;
            return 0;
        case GOPERL_FIELD_LV_TARG_SET: /* the plain store: no count changes */
            if (SvTYPE(sv) == SVt_PVLV) LvTARG(sv) = GOPERL_SVARG(b);
            return 0;
        case GOPERL_FIELD_SHARED_HASH:
            return SvIsCOW_shared_hash(sv) ? (uint64_t)SvSHARED_HASH(sv) : 0;
        case GOPERL_FIELD_OP_SV: { /* cSVOPx_sv: the SV of an SVOP or METHOP */
            OP *o = GOPERL_MEMARG(OP, a);
            const U32 cls = PL_opargs[o->op_type] & OA_CLASS_MASK;
            if (cls == OA_METHOP)
                return (o->op_flags & OPf_KIDS) ? 0 : (uint64_t)(uintptr_t)cMETHOPx_meth(o);
            if (cls == OA_SVOP || cls == OA_PVOP_OR_SVOP || cls == OA_FILESTATOP ||
                o->op_type == OP_CUSTOM)
                return (uint64_t)(uintptr_t)cSVOPx_sv(o);
            return 0;
        }
        case GOPERL_FIELD_OP_GV: { /* cGVOPx_gv */
            OP *o = GOPERL_MEMARG(OP, a);
            const U32 cls = PL_opargs[o->op_type] & OA_CLASS_MASK;
            if (cls == OA_SVOP || cls == OA_PADOP || cls == OA_PVOP_OR_SVOP || cls == OA_FILESTATOP)
                return (uint64_t)(uintptr_t)(SV *)cGVOPx_gv(o);
            return 0;
        }
        case GOPERL_FIELD_IS_FREED: return SvIS_FREED(sv) ? 1 : 0;
        case GOPERL_FIELD_HVAUX_FLAGS:
            if (SvTYPE(sv) != SVt_PVHV || !HvHasAUX((HV *)sv)) return 0;
            return (uint64_t)HvAUX((HV *)sv)->xhv_aux_flags;
        case GOPERL_FIELD_GV_REFCNT:
            return isGV_with_GP(sv) ? (uint64_t)GvREFCNT((GV *)sv) : 0;
        case GOPERL_FIELD_CV_OUTSIDE_SET: /* the plain store: no count changes */
            if (SvTYPE(sv) == SVt_PVCV || SvTYPE(sv) == SVt_PVFM)
                CvOUTSIDE((CV *)sv) = GOPERL_CVARG(b);
            return 0;
        case GOPERL_FIELD_IO_IFP_SET:
            if (SvTYPE(sv) == SVt_PVIO) IoIFP((IO *)sv) = GOPERL_MEMARG(PerlIO, b);
            return 0;
        case GOPERL_FIELD_IO_OFP_SET:
            if (SvTYPE(sv) == SVt_PVIO) IoOFP((IO *)sv) = GOPERL_MEMARG(PerlIO, b);
            return 0;
        case GOPERL_FIELD_IO_TYPE_SET:
            if (SvTYPE(sv) == SVt_PVIO) IoTYPE((IO *)sv) = (char)b;
            return 0;
        case GOPERL_FIELD_IO_FLAGS_SET:
            if (SvTYPE(sv) == SVt_PVIO) IoFLAGS((IO *)sv) = (U8)b;
            return 0;
        case GOPERL_FIELD_IO_WORD: {
            if (SvTYPE(sv) != SVt_PVIO) return 0;
            IO *io = (IO *)sv;
            switch ((int)b) {
            case GOPERL_IOWORD_LINES: return (uint64_t)(int64_t)IoLINES(io);
            case GOPERL_IOWORD_PAGE: return (uint64_t)(int64_t)IoPAGE(io);
            case GOPERL_IOWORD_PAGE_LEN: return (uint64_t)(int64_t)IoPAGE_LEN(io);
            case GOPERL_IOWORD_LINES_LEFT: return (uint64_t)(int64_t)IoLINES_LEFT(io);
            case GOPERL_IOWORD_TOP_GV: return GOPERL_ADDR(IoTOP_GV(io));
            case GOPERL_IOWORD_FMT_GV: return GOPERL_ADDR(IoFMT_GV(io));
            case GOPERL_IOWORD_BOTTOM_GV: return GOPERL_ADDR(IoBOTTOM_GV(io));
            case GOPERL_IOWORD_TOP_NAME: return GOPERL_ADDR(IoTOP_NAME(io));
            case GOPERL_IOWORD_FMT_NAME: return GOPERL_ADDR(IoFMT_NAME(io));
            case GOPERL_IOWORD_BOTTOM_NAME: return GOPERL_ADDR(IoBOTTOM_NAME(io));
            }
            Perl_croak(aTHX_ "goperl: unknown word %d of an IO", (int)b);
        }
        case GOPERL_FIELD_IO_WORD_SET: {
            if (SvTYPE(sv) != SVt_PVIO) return 0;
            IO *io = (IO *)sv;
            /* a name the host gave as text is the interpreter's copy, which
             * it frees with the IO, as perl frees the savepv it was given */
            char *name = s ? savepv(s) : (char *)(uintptr_t)b;
            switch ((int)(c >> 32)) {
            case GOPERL_IOWORD_LINES: IoLINES(io) = (IV)(int64_t)b; return 0;
            case GOPERL_IOWORD_PAGE: IoPAGE(io) = (IV)(int64_t)b; return 0;
            case GOPERL_IOWORD_PAGE_LEN: IoPAGE_LEN(io) = (IV)(int64_t)b; return 0;
            case GOPERL_IOWORD_LINES_LEFT: IoLINES_LEFT(io) = (IV)(int64_t)b; return 0;
            case GOPERL_IOWORD_TOP_GV: IoTOP_GV(io) = GOPERL_GVARG(b); return 0;
            case GOPERL_IOWORD_FMT_GV: IoFMT_GV(io) = GOPERL_GVARG(b); return 0;
            case GOPERL_IOWORD_BOTTOM_GV: IoBOTTOM_GV(io) = GOPERL_GVARG(b); return 0;
            case GOPERL_IOWORD_TOP_NAME: IoTOP_NAME(io) = name; return 0;
            case GOPERL_IOWORD_FMT_NAME: IoFMT_NAME(io) = name; return 0;
            case GOPERL_IOWORD_BOTTOM_NAME: IoBOTTOM_NAME(io) = name; return 0;
            }
            Perl_croak(aTHX_ "goperl: unknown word %d of an IO", (int)(c >> 32));
        }
        }
        Perl_croak(aTHX_ "goperl: unknown field %d", (int)c);
    }
    case GOPERL_OP_REGEXP: { /* a = token, c = a GOPERL_RX_ selector */
        SV *sv = GOPERL_SVARG(a);
        switch ((int)c) {
        case GOPERL_RX_OF_SV: return (uint64_t)(uintptr_t)(SV *)SvRX(sv);
        case GOPERL_RX_COMP: return (uint64_t)(uintptr_t)(SV *)pregcomp(sv, (U32)b);
        case GOPERL_RX_COMP_PV: { /* the pattern as bytes; flags bit 31: UTF-8 */
            const U32 flags = (U32)(b >> 32);
            SV *pat = newSVpvn_flags(s ? s : "", (STRLEN)(uint32_t)b,
                                     SVs_TEMP | ((flags & 0x80000000u) ? SVf_UTF8 : 0));
            return (uint64_t)(uintptr_t)(SV *)pregcomp(pat, flags & 0x7FFFFFFFu);
        }
        case GOPERL_RX_EXEC: { /* s = words [stringarg][strend][strbeg][minend]
                 * [screamer][nosave], addresses of this memory */
            goperl_gword_t w[6];
            if (!s || b < sizeof w) Perl_croak(aTHX_ "goperl: short pregexec request");
            memcpy(w, s, sizeof w);
            return (uint64_t)(int64_t)pregexec(
                (REGEXP *)sv, (char *)(uintptr_t)w[0], (char *)(uintptr_t)w[1],
                (char *)(uintptr_t)w[2], (SSize_t)(int64_t)w[3],
                GOPERL_SVARG(w[4]), (U32)w[5]);
        }
        case GOPERL_RX_EXEC_FLAGS: {
            goperl_gword_t w[7];
            if (!s || b < sizeof w) Perl_croak(aTHX_ "goperl: short regexec request");
            memcpy(w, s, sizeof w);
            return (uint64_t)(int64_t)CALLREGEXEC(
                (REGEXP *)sv, (char *)(uintptr_t)w[0], (char *)(uintptr_t)w[1],
                (char *)(uintptr_t)w[2], (SSize_t)(int64_t)w[3],
                GOPERL_SVARG(w[4]), (void *)(uintptr_t)w[6], (U32)w[5]);
        }
        case GOPERL_RX_BODY: { /* -> the address of the words */
            static goperl_gword_t body[20];
            REGEXP *rx = (REGEXP *)sv;
            if (!rx || !isREGEXP(sv)) return 0;
            regexp *r = ReANY(rx);
            body[0] = GOPERL_ADDR(r->engine);
            body[1] = GOPERL_ADDR(r->mother_re);
            body[2] = GOPERL_ADDR(r->paren_names);
            body[3] = r->extflags;
            body[4] = r->nparens;
            body[5] = (goperl_gword_t)(int64_t)r->minlen;
            body[6] = (goperl_gword_t)(int64_t)r->minlenret;
            body[7] = (goperl_gword_t)r->gofs;
            body[8] = r->intflags;
            body[9] = r->lastparen;
            body[10] = r->lastcloseparen;
            body[11] = GOPERL_ADDR(r->subbeg);
            body[12] = (goperl_gword_t)(int64_t)r->sublen;
            body[13] = (goperl_gword_t)(int64_t)r->suboffset;
            body[14] = (goperl_gword_t)(int64_t)r->subcoffset;
            body[15] = (goperl_gword_t)r->compflags;
            body[16] = GOPERL_ADDR(RX_PRECOMP(rx));
            body[17] = (goperl_gword_t)RX_PRELEN(rx);
            body[18] = GOPERL_ADDR(RX_WRAPPED(rx));
            body[19] = (goperl_gword_t)RX_WRAPLEN(rx);
            return (uint64_t)(uintptr_t)body;
        }
        case GOPERL_RX_OFFS: { /* -> the start of the group; second: its end
                 * (both -1 when the group did not match) */
            REGEXP *rx = (REGEXP *)sv;
            if (!rx || !isREGEXP(sv)) return goperl_two((uint64_t)-1, (uint64_t)-1);
            regexp *r = ReANY(rx);
            if ((U32)b > r->nparens || !r->offs)
                return goperl_two((uint64_t)-1, (uint64_t)-1);
            return goperl_two((uint64_t)(int64_t)r->offs[(U32)b].start,
                              (uint64_t)(int64_t)r->offs[(U32)b].end);
        }
        }
        Perl_croak(aTHX_ "goperl: unknown regexp call %d", (int)c);
    }
    case GOPERL_OP_HOOK: { /* a = a GOPERL_HOOK_ selector */
        switch ((int)a) {
        case GOPERL_HOOK_NEWOP_SET: {
            const uint32_t type = (uint32_t)(b >> 1);
            if (type >= MAXO) return 0;
            if (b & 1) {
                if (PL_check[type] != goperl_newop_tramps[type]) {
                    g_newop_orig[type] = PL_check[type];
                    PL_check[type] = goperl_newop_tramps[type];
                }
            } else if (PL_check[type] == goperl_newop_tramps[type]) {
                PL_check[type] = g_newop_orig[type];
                g_newop_orig[type] = NULL;
            }
            return 0;
        }
        case GOPERL_HOOK_NEWOP_ORIG: { /* the checker a host one replaced, or
                 * the interpreter's when none did: b = the op, c = its type */
            const uint32_t type = (uint32_t)c;
            OP *o = GOPERL_MEMARG(OP, b);
            if (type >= MAXO) return (uint64_t)(uintptr_t)o;
            Perl_check_t fn = PL_check[type] == goperl_newop_tramps[type] ? g_newop_orig[type] : PL_check[type];
            return (uint64_t)(uintptr_t)(fn ? fn(aTHX_ o) : o);
        }
        case GOPERL_HOOK_PEEP_SET: {
            const int which = (int)((b >> 1) & 1);
            peep_t *slot = which ? &PL_rpeepp : &PL_peepp;
            const peep_t tramp = which ? goperl_rpeep_tramp : goperl_peep_tramp;
            if (b & 1) {
                if (*slot != tramp) {
                    g_peep_orig[which] = *slot;
                    *slot = tramp;
                }
            } else if (*slot == tramp) {
                *slot = g_peep_orig[which];
                g_peep_orig[which] = NULL;
            }
            return 0;
        }
        case GOPERL_HOOK_PEEP_ORIG: { /* b = the op, c = which */
            const int which = (int)(c & 1);
            OP *o = GOPERL_MEMARG(OP, b);
            const peep_t tramp = which ? goperl_rpeep_tramp : goperl_peep_tramp;
            peep_t fn = (which ? PL_rpeepp : PL_peepp) == tramp ? g_peep_orig[which]
                                                                 : (which ? PL_rpeepp : PL_peepp);
            if (fn) fn(aTHX_ o);
            return 0;
        }
        case GOPERL_HOOK_ATEXIT:
            call_atexit(goperl_atexit_thunk, (void *)(uintptr_t)b);
            return 0;
        case GOPERL_HOOK_UNWIND: {
            /* the running native call is the innermost */
            if (g_native_marks.empty()) return 0;
            const goperl_native_mark m = g_native_marks.back();
            while (PL_scopestack_ix > m.scope_ix) pop_scope();
            if (PL_savestack_ix > m.save_ix) leave_scope(m.save_ix);
            return 0;
        }
        }
        Perl_croak(aTHX_ "goperl: unknown hook call %d", (int)a);
    }
    case GOPERL_OP_PP_STACK: { /* a = a GOPERL_PPSTACK_ selector */
        switch ((int)a) {
        case GOPERL_PPSTACK_PULL: { /* -> the words; second: their number */
            static std::vector<goperl_gword_t> state;
            state.clear();
            goperl_pp_stack_words(aTHX_ state);
            return goperl_two(GOPERL_ADDR(state.data()), (uint64_t)state.size());
        }
        case GOPERL_PPSTACK_PUSH:
            if (!s || !goperl_pp_stack_apply(aTHX_ (const unsigned char *)s, (size_t)b))
                Perl_croak(aTHX_ "goperl: malformed stack state");
            return 0;
        }
        Perl_croak(aTHX_ "goperl: unknown stack call %d", (int)a);
    }
    case GOPERL_OP_OP_DUMP: { /* a = op token; b = cap<<8 | mode -> the
                 * address of the records; second: their number */
        static std::vector<goperl_gword_t> records;
        OP *o = GOPERL_MEMARG(OP, a);
        records.clear();
        if ((int)(b & 0x7F) == GOPERL_OPDUMP_FREED) {
            records.swap(g_ops_freed);
            g_ops_freed_count = 0;
            return goperl_two(GOPERL_ADDR(records.data()), (uint64_t)records.size());
        }
        if (!o) return goperl_two(0, 0);
        const size_t cap = (size_t)(b >> 8);
        goperl_op_dump(aTHX_ o, (int)(b & 0x7F), cBOOL(b & 0x80), cap ? cap : 1, records);
        for (size_t i = 0; i < records.size(); i += GOPERL_OPREC_WORDS)
            g_ops_described.insert((OP *)(uintptr_t)records[i]);
        return goperl_two(GOPERL_ADDR(records.data()),
                          (uint64_t)(records.size() / GOPERL_OPREC_WORDS));
    }
    case GOPERL_OP_CALL: { /* a = token, c = a GOPERL_CALL_ selector, and in
                 * its high half a 32-bit value that goes with b (hi) */
        SV *sv = (SV *)(uintptr_t)a;
        switch ((int)(uint32_t)c) { /* the selectors that take no scalar in a */
        case GOPERL_CALL_NEW_WARNINGS:
        case GOPERL_CALL_HASH_SEED:
        case GOPERL_CALL_LVALUE_SUB:
        case GOPERL_CALL_CASEMAP:
        case GOPERL_CALL_XOP:
        case GOPERL_CALL_COP_SAVE:
        case GOPERL_CALL_ASYNC:
        case GOPERL_CALL_NEWGVGEN:
        case GOPERL_CALL_RANDOM:
        case GOPERL_CALL_SAVE_RE_CONTEXT:
        case GOPERL_CALL_PARSER:
        case GOPERL_CALL_ATOF:
        case GOPERL_CALL_COP_SET:
            break;
        default: (void)GOPERL_SVARG(a);
        }
        const uint32_t hi = (uint32_t)(c >> 32);
        switch ((int)(uint32_t)c) {
        case GOPERL_CALL_PV_DISPLAY:
        case GOPERL_CALL_PV_ESCAPE:
        case GOPERL_CALL_PV_PRETTY: { /* s = words [count][max][flags][len of
                 * the buffer for pv_display][n1][n2], the start and end
                 * colour strings of n1 and n2 bytes, then the text; b = all
                 * of it. -> the address of the result in the SV; second
                 * (pv_escape): how many bytes of the text it took */
            goperl_gword_t w[6];
            if (!s || b < sizeof w) Perl_croak(aTHX_ "goperl: short display request");
            memcpy(w, s, sizeof w);
            if (w[4] > b || w[5] > b || w[0] > b || (uint64_t)sizeof w + w[4] + w[5] + w[0] > b)
                Perl_croak(aTHX_ "goperl: short display request");
            const char *c1 = s + sizeof w, *c2 = c1 + w[4], *text = c2 + w[5];
            switch ((int)(uint32_t)c) {
            case GOPERL_CALL_PV_DISPLAY:
                return (uint64_t)(uintptr_t)pv_display(sv, text, (STRLEN)w[0], (STRLEN)w[3],
                                                       (STRLEN)w[1]);
            case GOPERL_CALL_PV_ESCAPE: {
                STRLEN escaped = 0;
                char *r = pv_escape(sv, text, (STRLEN)w[0], (STRLEN)w[1], &escaped, (U32)w[2]);
                return goperl_two(GOPERL_ADDR(r), (uint64_t)escaped);
            }
            default: {
                /* the colour strings as C strings, as the function takes them */
                const std::string start(c1, (size_t)w[4]), end(c2, (size_t)w[5]);
                char *r = pv_pretty(sv, text, (STRLEN)w[0], (STRLEN)w[1],
                                    w[4] ? start.c_str() : NULL, w[5] ? end.c_str() : NULL,
                                    (U32)w[2]);
                return (uint64_t)(uintptr_t)r;
            }
            }
        }
        case GOPERL_CALL_CV_CLONE: return (uint64_t)(uintptr_t)(SV *)cv_clone((CV *)sv);
        case GOPERL_CALL_NEW_WARNINGS: { /* a = the old buffer's address (not
                 * a token), s = the bits, b = their size -> the new buffer.
                 * The SDK's headers carry WARNsize and WARN_ALLstring. */
            static_assert(WARNsize == 21, "the SDK's warnings.h values are this build's");
            return (uint64_t)(uintptr_t)Perl_new_warnings_bitfield(
                aTHX_ (char *)(uintptr_t)a, s ? s : "", (STRLEN)b);
        }
        case GOPERL_CALL_GETCWD_SV: return getcwd_sv(sv) ? 1 : 0;
        case GOPERL_CALL_MESS_SV: /* b = consume */
            return (uint64_t)(uintptr_t)mess_sv(sv, cBOOL(b));
        case GOPERL_CALL_MG_FIND: { /* the interpreter's own magic of a type
                 * (what the host attached is the host's to find): [type]
                 * [flags][len][obj][ptr][private] */
            static goperl_gword_t w[6];
            if (SvTYPE(sv) < SVt_PVMG) return 0;
            for (MAGIC *mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic) {
                if (mg->mg_type != (char)(int)b) continue;
                if (goperl_is_anchor(mg)) continue;
                w[0] = (goperl_gword_t)(unsigned char)mg->mg_type;
                w[1] = mg->mg_flags;
                w[2] = (goperl_gword_t)(int64_t)mg->mg_len;
                w[3] = GOPERL_ADDR(mg->mg_obj);
                w[4] = GOPERL_ADDR(mg->mg_ptr);
                w[5] = mg->mg_private;
                return (uint64_t)(uintptr_t)w;
            }
            return 0;
        }
        case GOPERL_CALL_POS_GET: {
            STRLEN pos = 0;
            const bool found = sv_regex_global_pos_get(sv, &pos, hi);
            return goperl_two(found ? 1 : 0, (uint64_t)pos);
        }
        case GOPERL_CALL_POS_SET: sv_regex_global_pos_set(sv, (STRLEN)b, hi); return 0;
        case GOPERL_CALL_POS_CLEAR: sv_regex_global_pos_clear(sv); return 0;
        case GOPERL_CALL_HASH_SEED: {
            /* the SDK's PERL_HASH_SEED_BYTES is this build's */
            static_assert(PERL_HASH_SEED_BYTES == 32, "the SDK's PERL_HASH_SEED_BYTES");
            static struct {
                goperl_gword_t count;
                U8 bytes[PERL_HASH_SEED_BYTES];
            } seed;
            seed.count = PERL_HASH_SEED_BYTES;
            memcpy(seed.bytes, PL_hash_seed, PERL_HASH_SEED_BYTES);
            return (uint64_t)(uintptr_t)&seed;
        }
        case GOPERL_CALL_LVALUE_SUB:
            if (b == 1)
                return (PL_op && (PL_op->op_private & OPpMAYBE_LVSUB) && is_lvalue_sub()) ? 1 : 0;
            return (uint64_t)(int64_t)is_lvalue_sub();
        case GOPERL_CALL_CV_NAME:
            return (uint64_t)(uintptr_t)cv_name((CV *)sv, b ? GOPERL_SVARG(b) : NULL, hi);
        case GOPERL_CALL_MG_LIST: { /* every MAGIC of the interpreter's own on
                 * the SV, as MG_FIND has one */
            static std::vector<goperl_gword_t> w;
            w.assign(1, 0);
            if (SvTYPE(sv) < SVt_PVMG) return (uint64_t)(uintptr_t)w.data();
            for (MAGIC *mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic) {
                if (goperl_is_anchor(mg)) continue;
                w.push_back((goperl_gword_t)(unsigned char)mg->mg_type);
                w.push_back(mg->mg_flags);
                w.push_back((goperl_gword_t)(int64_t)mg->mg_len);
                w.push_back(GOPERL_ADDR(mg->mg_obj));
                w.push_back(GOPERL_ADDR(mg->mg_ptr));
                w.push_back(mg->mg_private);
                w[0]++;
            }
            return (uint64_t)(uintptr_t)w.data();
        }
        case GOPERL_CALL_CASEMAP: {
            static struct {
                goperl_gword_t first;
                goperl_gword_t len;
                U8 bytes[UTF8_MAXBYTES_CASE + 1];
            } r;
            const UV cp = (UV)b;
            STRLEN len = 0;
            UV first;
            switch ((uint32_t)a) {
            case 0: first = toUPPER_uvchr(cp, r.bytes, &len); break;
            case 1: first = toLOWER_uvchr(cp, r.bytes, &len); break;
            case 2: first = toTITLE_uvchr(cp, r.bytes, &len); break;
            default: first = toFOLD_uvchr(cp, r.bytes, &len); break;
            }
            r.first = (goperl_gword_t)first;
            r.len = (goperl_gword_t)len;
            return (uint64_t)(uintptr_t)&r;
        }
        case GOPERL_CALL_UVAR: { /* b bit 0: the module has uf_val, bit 1:
                 * uf_set */
            struct ufuncs uf;
            uf.uf_val = (b & 1) ? goperl_uvar_val : NULL;
            uf.uf_set = (b & 2) ? goperl_uvar_set : NULL;
            uf.uf_index = 0;
            sv_magic(sv, NULL, PERL_MAGIC_uvar, (const char *)&uf, sizeof uf);
            return 0;
        }
        case GOPERL_CALL_XOP: {
            if (g_xop_n >= GOPERL_PP_TRAMPS)
                Perl_croak(aTHX_ "goperl: too many registered custom ops");
            const char *name = s ? s : "";
            const char *desc = name + strlen(name) + 1;
            XOP *xop = &g_xops[g_xop_n];
            XopENTRY_set(xop, xop_name, savepv(name));
            XopENTRY_set(xop, xop_desc, savepv(desc));
            XopENTRY_set(xop, xop_class, (U32)b);
            Perl_custom_op_register(aTHX_ goperl_pp_tramps[g_xop_n], xop);
            return (uint64_t)(++g_xop_n);
        }
        case GOPERL_CALL_COP_SAVE: {
            COP *cop = GOPERL_MEMARG(COP, a);
            if (b == 2)
                SAVECOPSTASH_FREE(cop);
            else if (b)
                SAVECOPFILE_FREE(cop);
            else
                SAVECOPLINE(cop);
            return 0;
        }
        case GOPERL_CALL_LEX_START:
            lex_start(sv, NULL, (U32)b);
            return 0;
        case GOPERL_CALL_ASYNC:
            /* native code that loops says where it can be interrupted */
            return goperl_deliver_pending(aTHX) ? 1 : 0;
        case GOPERL_CALL_SCAN_VERSION: { /* b = the address of the text, or 0
                 * for the text in s; hi = qv */
            const char *const text = b ? (const char *)(uintptr_t)b : (s ? s : "");
            const char *const end = scan_version(text, sv, cBOOL(hi));
            return (uint64_t)(end - text);
        }
        case GOPERL_CALL_NEWGVGEN:
            return (uint64_t)(uintptr_t)newGVgen_flags(s ? s : "main", (U32)b);
        case GOPERL_CALL_RANDOM:
            switch ((int)(b & 3)) {
            case 0: {
                const double d = (double)Drand01();
                uint64_t bits;
                memcpy(&bits, &d, sizeof bits);
                return bits;
            }
            case 1:
                seedDrand01((Rand_seed_t)(b >> 2));
                return 0;
            default:
                return (uint64_t)seed();
            }
        case GOPERL_CALL_QERROR: /* a = the message (an SV), or 0 */
            qerror(sv);
            return 0;
        case GOPERL_CALL_SAVE_RE_CONTEXT:
            save_re_context();
            return 0;
        case GOPERL_CALL_SV_PEEK: /* the text is a mortal's buffer */
            return (uint64_t)(uintptr_t)sv_peek(sv);
        case GOPERL_CALL_SV_RECODE:
            return (uint64_t)(uintptr_t)sv_recode_to_utf8(sv, GOPERL_SVARG(b));
        case GOPERL_CALL_SV_CAT_DECODE: { /* a = dsv; s = words [encoding][ssv]
                 * [offset][tlen] and the terminator's bytes -> the result;
                 * second: the offset */
            goperl_gword_t w[4] = {0, 0, 0, 0};
            if (!s) return goperl_two(0, 0);
            memcpy(w, s, sizeof w);
            int offset = (int)(int64_t)w[2];
            const bool r = sv_cat_decode(sv, GOPERL_SVARG(w[0]), GOPERL_SVARG(w[1]),
                                         &offset, (char *)(s + sizeof w), (int)w[3]);
            return goperl_two(r ? 1 : 0, (uint64_t)(int64_t)offset);
        }
        case GOPERL_CALL_PARSER: {
            static goperl_gword_t w[GOPERL_PARSERW_WORDS];
            yy_parser *ps = PL_parser;
            if (!ps) return 0;
            w[GOPERL_PARSERW_LINESTR] = GOPERL_ADDR(ps->linestr);
            w[GOPERL_PARSERW_PV] = ps->linestr ? GOPERL_ADDR(SvPVX(ps->linestr)) : 0;
            w[GOPERL_PARSERW_BUFPTR] = GOPERL_ADDR(ps->bufptr);
            w[GOPERL_PARSERW_BUFEND] = GOPERL_ADDR(ps->bufend);
            w[GOPERL_PARSERW_OLDBUFPTR] = GOPERL_ADDR(ps->oldbufptr);
            w[GOPERL_PARSERW_OLDOLDBUFPTR] = GOPERL_ADDR(ps->oldoldbufptr);
            w[GOPERL_PARSERW_LINESTART] = GOPERL_ADDR(ps->linestart);
            w[GOPERL_PARSERW_ERROR_COUNT] = (goperl_gword_t)ps->error_count;
            w[GOPERL_PARSERW_IN_MY] = ps->in_my;
            w[GOPERL_PARSERW_LEX_FLAGS] = ps->lex_flags;
            w[GOPERL_PARSERW_PREAMBLING] = (goperl_gword_t)ps->preambling;
            w[GOPERL_PARSERW_EXPECT] = ps->expect;
            w[GOPERL_PARSERW_LEX_STATE] = ps->lex_state;
            w[GOPERL_PARSERW_LEX_BRACKETS] = (goperl_gword_t)ps->lex_brackets;
            w[GOPERL_PARSERW_LEX_INWHAT] = ps->lex_inwhat;
            w[GOPERL_PARSERW_MULTI_START] = (goperl_gword_t)ps->multi_start;
            w[GOPERL_PARSERW_MULTI_END] = (goperl_gword_t)ps->multi_end;
            w[GOPERL_PARSERW_MULTI_OPEN] = (goperl_gword_t)ps->multi_open;
            w[GOPERL_PARSERW_MULTI_CLOSE] = (goperl_gword_t)ps->multi_close;
            w[GOPERL_PARSERW_COPLINE] = (goperl_gword_t)ps->copline;
            w[GOPERL_PARSERW_IN_MY_STASH] = GOPERL_ADDR(ps->in_my_stash);
            w[GOPERL_PARSERW_LAST_UNI] = GOPERL_ADDR(ps->last_uni);
            w[GOPERL_PARSERW_LAST_LOP] = GOPERL_ADDR(ps->last_lop);
            w[GOPERL_PARSERW_RSFP] = GOPERL_ADDR(ps->rsfp);
            w[GOPERL_PARSERW_RSFP_FILTERS] = GOPERL_ADDR(ps->rsfp_filters);
            w[GOPERL_PARSERW_LEX_FORMBRACK] = (goperl_gword_t)ps->lex_formbrack;
            w[GOPERL_PARSERW_LEX_STUFF] = GOPERL_ADDR(ps->lex_stuff);
            w[GOPERL_PARSERW_LEX_REPL] = GOPERL_ADDR(ps->lex_repl);
            w[GOPERL_PARSERW_LEX_BRACKSTACK] = GOPERL_ADDR(ps->lex_brackstack);
            w[GOPERL_PARSERW_TOKENBUF] = GOPERL_ADDR(ps->tokenbuf);
            w[GOPERL_PARSERW_FILTERED] = ps->filtered;
            w[GOPERL_PARSERW_LAST_LOP_OP] = (goperl_gword_t)ps->last_lop_op;
            return (uint64_t)(uintptr_t)w;
        }
        case GOPERL_CALL_ATOF: {
            const NV nv = my_atof(s ? s : "");
            uint64_t bits;
            memcpy(&bits, &nv, sizeof bits);
            return bits;
        }
        case GOPERL_CALL_AMT: {
            static goperl_gword_t w[4 + NofAMmeth];
            if (SvTYPE(sv) != SVt_PVHV) return 0;
            MAGIC *mg = mg_find(sv, PERL_MAGIC_overload_table);
            if (!mg || !mg->mg_ptr) return 0;
            const AMT *amt = (const AMT *)mg->mg_ptr;
            w[0] = amt->flags;
            w[1] = amt->fallback;
            w[2] = amt->was_ok_sub;
            w[3] = 0;
            if (AMT_AMAGIC(amt)) {
                w[3] = NofAMmeth;
                for (int i = 0; i < NofAMmeth; i++)
                    w[4 + i] = GOPERL_ADDR(amt->table[i]);
            }
            return (uint64_t)(uintptr_t)w;
        }
        case GOPERL_CALL_COP_SET: { /* the token is a COP; b = the value,
                 * hi = the field */
            COP *cop = GOPERL_MEMARG(COP, a);
            const uint64_t val = b;
            switch ((int)hi) {
            case GOPERL_COPF_LINE: CopLINE_set(cop, (line_t)val); return 0;
            case GOPERL_COPF_WARNINGS: cop->cop_warnings = (char *)(uintptr_t)val; return 0;
            case GOPERL_COPF_HINTS: cop->cop_hints = (U32)val; return 0;
            case GOPERL_COPF_STASH: CopSTASH_set(cop, GOPERL_HVARG(val)); return 0;
            case GOPERL_COPF_FILE: CopFILE_set(cop, s ? s : ""); return 0;
            case GOPERL_COPF_FEATURES: cop->cop_features.bits[0] = (U32)val; return 0;
            case GOPERL_COPF_FILE_FREE: CopFILE_free(cop); return 0;
            }
            Perl_croak(aTHX_ "goperl: unknown cop field %d", (int)hi);
        }
        }
        Perl_croak(aTHX_ "goperl: unknown call %d", (int)(uint32_t)c);
    }
    case GOPERL_OP_TREE: { /* a = a GOPERL_TREE_ selector; s = words */
        goperl_gword_t w[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        if (s && b) memcpy(w, s, b < sizeof w ? (size_t)b : sizeof w);
#define GOPERL_W_OP(i) (GOPERL_MEMARG(OP, w[i]))
#define GOPERL_W_SV(i) (GOPERL_SVARG(w[i]))
        switch ((int)a) {
        case GOPERL_TREE_NEWREF:
            switch (w[0]) {
            case 0: return (uint64_t)(uintptr_t)newCVREF((I32)w[1], GOPERL_W_OP(2));
            case 1: return (uint64_t)(uintptr_t)newAVREF(GOPERL_W_OP(2));
            case 2: return (uint64_t)(uintptr_t)newHVREF(GOPERL_W_OP(2));
            case 3: return (uint64_t)(uintptr_t)newSVREF(GOPERL_W_OP(2));
            case 4: return (uint64_t)(uintptr_t)newGVREF((I32)w[1], GOPERL_W_OP(2));
            }
            break;
        case GOPERL_TREE_NEWFOROP:
            return (uint64_t)(uintptr_t)newFOROP((I32)w[0], GOPERL_W_OP(1), GOPERL_W_OP(2),
                                                 GOPERL_W_OP(3), GOPERL_W_OP(4));
        case GOPERL_TREE_NEWNULLLIST: return (uint64_t)(uintptr_t)newNULLLIST();
        case GOPERL_TREE_NEWDEFSVOP: return (uint64_t)(uintptr_t)newDEFSVOP();
        case GOPERL_TREE_NEWRANGE:
            return (uint64_t)(uintptr_t)newRANGE((I32)w[0], GOPERL_W_OP(1), GOPERL_W_OP(2));
        case GOPERL_TREE_NEWWHILEOP:
            return (uint64_t)(uintptr_t)newWHILEOP((I32)w[0], (I32)w[1], (LOOP *)GOPERL_W_OP(2),
                                                   GOPERL_W_OP(3), GOPERL_W_OP(4),
                                                   GOPERL_W_OP(5), (I32)w[6]);
        case GOPERL_TREE_NEWLOOPOP:
            return (uint64_t)(uintptr_t)newLOOPOP((I32)w[0], (I32)w[1], GOPERL_W_OP(2),
                                                  GOPERL_W_OP(3));
        case GOPERL_TREE_NEWLOOPEX:
            return (uint64_t)(uintptr_t)newLOOPEX((I32)w[0], GOPERL_W_OP(1));
        case GOPERL_TREE_NEWANON:
            return (uint64_t)(uintptr_t)(w[0] ? newANONHASH(GOPERL_W_OP(1))
                                              : newANONLIST(GOPERL_W_OP(1)));
        case GOPERL_TREE_OP_LVALUE:
            return (uint64_t)(uintptr_t)op_lvalue(GOPERL_W_OP(0), (I32)w[1]);
        case GOPERL_TREE_START_SUBPARSE:
            return (uint64_t)(int64_t)start_subparse((I32)w[0], (U32)w[1]);
        case GOPERL_TREE_NEWATTRSUB:
            return (uint64_t)(uintptr_t)(SV *)newATTRSUB((I32)w[0], GOPERL_W_OP(1),
                                                         GOPERL_W_OP(2), GOPERL_W_OP(3),
                                                         GOPERL_W_OP(4));
        case GOPERL_TREE_NEWANONATTRSUB:
            return (uint64_t)(uintptr_t)newANONATTRSUB((I32)w[0], GOPERL_W_OP(1),
                                                       GOPERL_W_OP(2), GOPERL_W_OP(3));
        case GOPERL_TREE_PAD_ADD_NAME_SV:
            return (uint64_t)pad_add_name_sv(GOPERL_W_SV(0), (U32)w[1], (HV *)GOPERL_W_SV(2),
                                             (HV *)GOPERL_W_SV(3));
        case GOPERL_TREE_FIND_RUNCV:
            return (uint64_t)(uintptr_t)(SV *)find_runcv(NULL);
        case GOPERL_TREE_SAVE_CLEARSV:
            save_clearsv(&PAD_SVl((PADOFFSET)w[0]));
            return 0;
        case GOPERL_TREE_LEX_STUFF_SV:
            lex_stuff_sv(GOPERL_W_SV(0), (U32)w[1]);
            return 0;
        case GOPERL_TREE_RV2CV_OP_CV:
            return (uint64_t)(uintptr_t)(SV *)rv2cv_op_cv(GOPERL_W_OP(0), (U32)w[1]);
        case GOPERL_TREE_CK_ARGS:
            switch (w[0]) {
            case 0:
                return (uint64_t)(uintptr_t)ck_entersub_args_proto_or_list(
                    GOPERL_W_OP(1), (GV *)GOPERL_W_SV(2), GOPERL_W_SV(3));
            case 1:
                return (uint64_t)(uintptr_t)ck_entersub_args_proto(
                    GOPERL_W_OP(1), (GV *)GOPERL_W_SV(2), GOPERL_W_SV(3));
            case 2: return (uint64_t)(uintptr_t)ck_entersub_args_list(GOPERL_W_OP(1));
            }
            break;
        case GOPERL_TREE_CALLCK_SET: {
            CV *cv = (CV *)GOPERL_W_SV(0);
            if (!w[1]) { /* the interpreter's own checker again */
                cv_set_call_checker_flags(cv, Perl_ck_entersub_args_proto_or_list,
                                          GOPERL_W_SV(2), (U32)w[3]);
                return 0;
            }
            AV *pair = newAV();
            av_push(pair, newSVuv(w[1]));
            av_push(pair, SvREFCNT_inc(GOPERL_W_SV(2)));
            cv_set_call_checker_flags(cv, goperl_callck_tramp, (SV *)pair, (U32)w[3]);
            SvREFCNT_dec((SV *)pair); /* the sub's magic holds it now */
            return 0;
        }
        case GOPERL_TREE_CALLCK_GET: { /* [which: 0 a checker of the
                 * interpreter that is not its default, 1 a host function,
                 * 2 the default][host id][ckobj][flags] */
            static goperl_gword_t r[4];
            Perl_call_checker fn;
            SV *obj;
            U32 flags;
            cv_get_call_checker_flags((CV *)GOPERL_W_SV(0), (U32)w[1], &fn, &obj, &flags);
            r[0] = fn == Perl_ck_entersub_args_proto_or_list ? 2 : 0;
            r[1] = 0;
            r[2] = GOPERL_ADDR(obj);
            r[3] = flags;
            if (fn == goperl_callck_tramp) {
                SV **idp = av_fetch((AV *)obj, 0, 0);
                SV **objp = av_fetch((AV *)obj, 1, 0);
                r[0] = 1;
                r[1] = idp ? (goperl_gword_t)SvUV(*idp) : 0;
                r[2] = GOPERL_ADDR(objp ? *objp : NULL);
            }
            return (uint64_t)(uintptr_t)r;
        }
        case GOPERL_TREE_CXINC: return (uint64_t)(int64_t)cxinc();
        case GOPERL_TREE_NEWUNOP_AUX: {
            UNOP_AUX_item *aux = NULL;
            switch (w[3]) {
            case 1: aux = INT2PTR(UNOP_AUX_item *, (IV)(int64_t)w[4]); break;
            case 2: {
                struct op_argcheck_aux *ca = (struct op_argcheck_aux *)PerlMemShared_malloc(
                    sizeof(struct op_argcheck_aux));
                ca->params = (UV)w[4];
                ca->opt_params = (UV)w[5];
                ca->slurpy = (char)w[6];
                aux = (UNOP_AUX_item *)ca;
                break;
            }
            }
            OP *o = newUNOP_AUX((I32)w[0], (I32)w[1], GOPERL_W_OP(2), aux);
            if ((I32)w[0] == OP_CUSTOM) g_custom_built[o] = OPclass_UNOP_AUX;
            return (uint64_t)(uintptr_t)o;
        }
        case GOPERL_TREE_PARSE_SUBSIGNATURE:
            return (uint64_t)(uintptr_t)parse_subsignature((U32)w[0]);
        case GOPERL_TREE_NEWMYSUB:
            return (uint64_t)(uintptr_t)(SV *)newMYSUB((I32)w[0], GOPERL_W_OP(1),
                                                       GOPERL_W_OP(2), GOPERL_W_OP(3),
                                                       GOPERL_W_OP(4));
        case GOPERL_TREE_CV_NAME: {
            CV *cv = (CV *)GOPERL_W_SV(0);
            if (w[1]) { /* CvNAME_HEK_set: the name follows the four words */
                const char *name = (s && b > 32) ? s + 32 : "";
                const I32 len = (I32)w[3];
                U32 hash;
                PERL_HASH(hash, name, len);
                HEK *hek = share_hek(name, w[2] ? -len : len, hash);
                if (CvNAMED(cv) && CvNAME_HEK(cv)) unshare_hek(CvNAME_HEK(cv));
                ((XPVCV *)MUTABLE_PTR(SvANY(cv)))->xcv_gv_u.xcv_hek = hek;
                CvNAMED_on(cv);
                return 0;
            }
            HEK *hek = CvNAME_HEK(cv);
            if (!hek) return goperl_two(0, 0);
            return goperl_two(GOPERL_ADDR(HEK_KEY(hek)),
                              ((uint64_t)HEK_LEN(hek) << 1) | (HEK_UTF8(hek) ? 1u : 0u));
        }
        case GOPERL_TREE_NEWPVOP: { /* the string follows the two words; the
                 * op owns a copy */
            const char *str = (s && b > 16) ? s + 16 : "";
            return (uint64_t)(uintptr_t)newPVOP((I32)w[0], (I32)w[1], savepv(str));
        }
        }
#undef GOPERL_W_OP
#undef GOPERL_W_SV
        Perl_croak(aTHX_ "goperl: unknown tree call %d", (int)a);
    }
    case GOPERL_OP_CX: { /* a = the stackinfo token (0: the current one),
                 * c = a GOPERL_CX_ selector */
        PERL_SI *si = a ? (PERL_SI *)(uintptr_t)a : PL_curstackinfo;
        switch ((int)c) {
        case GOPERL_CX_DUMP: { /* b = index -> the address of these words:
                 * [type][gimme][u16][oldsaveix][oldsp][oldmarksp]
                 * [oldscopesp][oldcop][oldpm][old_tmpsfloor], then by type:
                 * sub and format [cv][retop][olddepth][savearray]
                 * [prevcomppad]; eval [retop][old_namesv][cur_text][cv]
                 * [old_eval_root]; loop [my_op] */
            static goperl_gword_t w[16];
            const I32 ix = (I32)(int64_t)b;
            /* the context above the top is the one perl popped last, which
             * it leaves as it was (Variable::Magic reads its type): its type
             * and numbers, and none of its ops or scalars, which may be gone
             * by now */
            const bool ghost = si && ix == si->si_cxix + 1 && ix <= si->si_cxmax;
            if (!si || ix < 0 || (ix > si->si_cxix && !ghost)) return 0;
            PERL_CONTEXT *cx = &si->si_cxstack[ix];
            memset(w, 0, sizeof w);
            w[0] = cx->cx_type;
            if (ghost) {
                if (CxTYPE(cx) == CXt_SUBST) return (uint64_t)(uintptr_t)w;
                w[1] = cx->blk_gimme;
                w[2] = cx->blk_u16;
                w[3] = (goperl_gword_t)cx->blk_oldsaveix;
                w[4] = (goperl_gword_t)(int64_t)cx->blk_oldsp;
                w[5] = (goperl_gword_t)cx->blk_oldmarksp;
                w[6] = (goperl_gword_t)cx->blk_oldscopesp;
                w[9] = (goperl_gword_t)(int64_t)cx->blk_old_tmpsfloor;
                return (uint64_t)(uintptr_t)w;
            }
            if (CxTYPE(cx) == CXt_SUBST) return (uint64_t)(uintptr_t)w;
            w[1] = cx->blk_gimme;
            w[2] = cx->blk_u16;
            w[3] = (goperl_gword_t)cx->blk_oldsaveix;
            w[4] = (goperl_gword_t)(int64_t)cx->blk_oldsp;
            w[5] = (goperl_gword_t)cx->blk_oldmarksp;
            w[6] = (goperl_gword_t)cx->blk_oldscopesp;
            w[7] = GOPERL_ADDR(cx->blk_oldcop);
            w[8] = GOPERL_ADDR(cx->blk_oldpm);
            w[9] = (goperl_gword_t)(int64_t)cx->blk_old_tmpsfloor;
            switch (CxTYPE(cx)) {
            case CXt_SUB:
                w[10] = GOPERL_ADDR(cx->blk_sub.cv);
                w[11] = GOPERL_ADDR(cx->blk_sub.retop);
                w[12] = (goperl_gword_t)cx->blk_sub.olddepth;
                w[13] = GOPERL_ADDR(cx->blk_sub.savearray);
                w[14] = GOPERL_ADDR(cx->blk_sub.prevcomppad);
                break;
            case CXt_FORMAT:
                w[10] = GOPERL_ADDR(cx->blk_format.cv);
                w[11] = GOPERL_ADDR(cx->blk_format.retop);
                break;
            case CXt_EVAL:
                w[10] = GOPERL_ADDR(cx->blk_eval.retop);
                w[11] = GOPERL_ADDR(cx->blk_eval.old_namesv);
                w[12] = GOPERL_ADDR(cx->blk_eval.cur_text);
                w[13] = GOPERL_ADDR(cx->blk_eval.cv);
                w[14] = GOPERL_ADDR(cx->blk_eval.old_eval_root);
                break;
            default:
                if (CxTYPE_is_LOOP(cx))
                    w[10] = GOPERL_ADDR(cx->blk_loop.my_op);
                break;
            }
            return (uint64_t)(uintptr_t)w;
        }
        case GOPERL_CX_DOUNWIND:
            dounwind((I32)(int64_t)b);
            return 0;
        case GOPERL_CX_MAX:
            return si ? (uint64_t)(int64_t)si->si_cxmax : 0;
        case GOPERL_CX_SET: { /* native code stored into a context, as code
                 * compiled against perl's headers does (cx->blk_sub.retop =
                 * op): the field takes the value, nothing else happens */
            /* s = words [index][field][value]; b = their length */
            goperl_gword_t w[3] = {0, 0, 0};
            if (!s || b < sizeof w) Perl_croak(aTHX_ "goperl: short context store");
            memcpy(w, s, sizeof w);
            const I32 ix = (I32)(int64_t)w[0];
            const goperl_gword_t v = w[2];
            if (!si || ix < 0 || ix > si->si_cxix) return 0;
            PERL_CONTEXT *cx = &si->si_cxstack[ix];
            switch (w[1]) {
            case 0: cx->cx_type = (U8)v; return 1;
            case 1: cx->blk_gimme = (U8)v; return 1;
            case 2: cx->blk_u16 = (U16)v; return 1;
            case 3: cx->blk_oldsaveix = (I32)v; return 1;
            case 4: cx->blk_oldsp = (SSize_t)(int64_t)v; return 1;
            case 5: cx->blk_oldmarksp = (I32)v; return 1;
            case 6: cx->blk_oldscopesp = (I32)v; return 1;
            case 7: cx->blk_oldcop = GOPERL_MEMARG(COP, v); return 1;
            case 8: cx->blk_oldpm = GOPERL_MEMARG(PMOP, v); return 1;
            case 9: cx->blk_old_tmpsfloor = (SSize_t)(int64_t)v; return 1;
            }
            switch (CxTYPE(cx)) {
            case CXt_SUB:
                switch (w[1]) {
                case 10: cx->blk_sub.cv = GOPERL_CVARG(v); return 1;
                case 11: cx->blk_sub.retop = GOPERL_MEMARG(OP, v); return 1;
                case 12: cx->blk_sub.olddepth = (I32)v; return 1;
                case 13: cx->blk_sub.savearray = GOPERL_AVARG(v); return 1;
                case 14: cx->blk_sub.prevcomppad = (PAD *)(uintptr_t)v; return 1;
                }
                break;
            case CXt_FORMAT:
                switch (w[1]) {
                case 10: cx->blk_format.cv = GOPERL_CVARG(v); return 1;
                case 11: cx->blk_format.retop = GOPERL_MEMARG(OP, v); return 1;
                }
                break;
            case CXt_EVAL:
                switch (w[1]) {
                case 10: cx->blk_eval.retop = GOPERL_MEMARG(OP, v); return 1;
                case 11: cx->blk_eval.old_namesv = GOPERL_SVARG(v); return 1;
                case 12: cx->blk_eval.cur_text = GOPERL_SVARG(v); return 1;
                case 13: cx->blk_eval.cv = GOPERL_CVARG(v); return 1;
                case 14: cx->blk_eval.old_eval_root = GOPERL_MEMARG(OP, v); return 1;
                }
                break;
            default:
                if (CxTYPE_is_LOOP(cx) && w[1] == 10) {
                    cx->blk_loop.my_op = (LOOP *)(uintptr_t)v;
                    return 1;
                }
                break;
            }
            Perl_croak(aTHX_ "goperl: context field %u cannot be stored into", (unsigned)w[1]);
        }
        }
        Perl_croak(aTHX_ "goperl: unknown context call %d", (int)c);
    }
    case GOPERL_OP_INTERNAL:
        return goperl_internal_op(aTHX_ (int)(uint32_t)c, a, b, c, s);
    case GOPERL_OP_SV_MAGIC_STD: { /* real sv_magic: BEHAVIORAL core magic
                 * (ties and friends) must live guest-side to take effect
                 * (b = obj token, c = how, s+len = name or empty; with c's
                 * high half 1, s is a UTF-8 cache of positions, which the
                 * magic owns a copy of: mg_len -1; with 2, s is the 8 bytes
                 * of the name SV: HEf_SVKEY) */
        if ((c >> 32) == 2) {
            uint64_t name = 0;
            if (s) memcpy(&name, s, sizeof name);
            sv_magic(GOPERL_SVARG(a), GOPERL_SVARG(b), (int)(uint32_t)c,
                     (const char *)(uintptr_t)name, HEf_SVKEY);
            return 0;
        }
        if ((c >> 32) == 1) {
            STRLEN *cache;
            Newxz(cache, PERL_MAGIC_UTF8_CACHESIZE * 2, STRLEN);
            if (s) memcpy(cache, s, PERL_MAGIC_UTF8_CACHESIZE * 2 * sizeof(STRLEN));
            sv_magic(GOPERL_SVARG(a), GOPERL_SVARG(b), (int)(uint32_t)c, (const char *)cache, -1);
            return 0;
        }
        sv_magic(GOPERL_SVARG(a), GOPERL_SVARG(b), (int)(int64_t)c, s && *s ? s : NULL,
                 s ? (I32)strlen(s) : 0);
        return 0;
    }
    case GOPERL_OP_SV_MAGIC_SET_HOOK: { /* gives the anchor of sv the hooks
                 * of the mask b as well (see goperl_anchor_vtbl; no mask is
                 * set magic alone); mg_magical refreshes the SV's magic
                 * flags so that the interpreter calls them. */
        SV *sv = GOPERL_SVARG(a);
        MAGIC *mg = goperl_anchor_of(aTHX_ sv);
        if (!mg) return 0;
        const unsigned have = (unsigned)(mg->mg_virtual - &goperl_anchor_vtbls[0]);
        const unsigned want = have | (b ? (unsigned)b : 2u);
        mg->mg_virtual = (MGVTBL *)goperl_anchor_vtbl(want);
        if (want & 16) mg->mg_flags |= MGf_COPY;
        mg_magical(sv);
        return 1;
    }
    }
    return 0;
}
