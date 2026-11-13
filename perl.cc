/* perl.cc — Perl embedding bridge (implements pl.h) for wasm32-wasi + Go.
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
 */
#include "pl.h"

/* The C++ standard headers MUST come before Perl's: perl.h defines
 * short-name macros (do_open, do_close, ...) that break any libc++ header
 * textually included after it. */
#include <cstdint>
#include <cstdio>
#include <cstring>
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
 * scripts/wasi-configure.sh and with scripts/bundled-cpan.txt.
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
/* CPAN distributions bundled on top of the core (scripts/bundled-cpan.txt) */
void boot_Class__Load__XS(pTHX_ CV *cv);
void boot_Class__XSAccessor(pTHX_ CV *cv);
void boot_Clone(pTHX_ CV *cv);
void boot_Cpanel__JSON__XS(pTHX_ CV *cv);
void boot_Devel__Caller(pTHX_ CV *cv);
void boot_Devel__Size(pTHX_ CV *cv);
void boot_Digest__SHA1(pTHX_ CV *cv);
void boot_HTML__Parser(pTHX_ CV *cv);
void boot_List__MoreUtils__XS(pTHX_ CV *cv);
void boot_Package__Stash__XS(pTHX_ CV *cv);
void boot_PadWalker(pTHX_ CV *cv);
void boot_Params__Util(pTHX_ CV *cv);
void boot_Set__Object(pTHX_ CV *cv);
void boot_Sub__Identify(pTHX_ CV *cv);
void boot_Sub__Name(pTHX_ CV *cv);
void boot_Test__LeakTrace(pTHX_ CV *cv);
void boot_Text__CSV_XS(pTHX_ CV *cv);
void boot_Want(pTHX_ CV *cv);
void boot_YAML__XS__LibYAML(pTHX_ CV *cv);
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
    {"Class::Load::XS", boot_Class__Load__XS},
    {"Class::XSAccessor", boot_Class__XSAccessor},
    {"Clone", boot_Clone},
    {"Cpanel::JSON::XS", boot_Cpanel__JSON__XS},
    {"Devel::Caller", boot_Devel__Caller},
    {"Devel::Size", boot_Devel__Size},
    {"Digest::SHA1", boot_Digest__SHA1},
    {"HTML::Parser", boot_HTML__Parser},
    {"List::MoreUtils::XS", boot_List__MoreUtils__XS},
    {"Package::Stash::XS", boot_Package__Stash__XS},
    {"PadWalker", boot_PadWalker},
    {"Params::Util", boot_Params__Util},
    {"Set::Object", boot_Set__Object},
    {"Sub::Identify", boot_Sub__Identify},
    {"Sub::Name", boot_Sub__Name},
    {"Test::LeakTrace", boot_Test__LeakTrace},
    {"Text::CSV_XS", boot_Text__CSV_XS},
    {"Want", boot_Want},
    {"YAML::XS::LibYAML", boot_YAML__XS__LibYAML},
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

static void site_configure(const char *site_dir, const char *site_arch) {
    g_site_config.clear();
    g_site_arch_dir.clear();
    g_site_lib_dir.clear();
    if (!site_dir || !*site_dir) return;
    std::string root(site_dir);
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    const std::string under = root == "/" ? "" : root;
    const std::string stem = under + "/site_perl";
    g_site_lib_dir = stem + "/" PERL_VERSION_STRING;
    g_site_arch_dir = g_site_lib_dir + "/" + (site_arch && *site_arch ? site_arch : ARCHNAME);
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

static void xs_init(pTHX) {
    static const char file[] = __FILE__;
    dXSUB_SYS;
    PERL_UNUSED_CONTEXT;

    /* The loader itself, and its way to the table above. */
    newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, file);
    newXS("DynaLoader::dl_static_boot", XS_DynaLoader_dl_static_boot, file);
    newXS("Internals::site_directories", XS_Internals_site_directories, file);
    newXS("Internals::host_numbers", XS_Internals_host_numbers, file);
}

/* ---- interpreter state --------------------------------------------------- */
static PerlInterpreter *g_my_perl = nullptr;

/* ---- Perl -> Go dispatch -------------------------------------------------
 * The wasmify callback import: the ONE host entry point every C++->Go (here:
 * Perl->Go) call goes through. Declared here rather than via the generated
 * bridge header so perl.cc stays self-contained; the signature must match
 * bridge/api_bridge.h's WASM_IMPORT(wasmify, callback_invoke). The i64 result
 * packs (resp_ptr << 32) | resp_len; the response buffer transfers to us (we
 * free it), while the request buffer only lends: the host reads it during the
 * call and we keep ownership (it lives in a Perl SV). */
__attribute__((import_module("wasmify"), import_name("callback_invoke")))
extern "C" int64_t wasmify_callback_invoke(int32_t callback_id, int32_t method_id,
                                           void *req, size_t req_len);

/* The Go-side callback id every Perl->Go call from this instance dispatches
 * to. 0 = no dispatcher registered (perl_set_go_dispatcher not called). */
static int32_t g_go_cb = 0;

/* Reserved callback method id for native-XS dispatch (see pl.h). Ordinary
 * bound Go functions use positive ids. */
#define GOPERL_NATIVE_METHOD_ID (-1)

/* Reserved callback method id for host-side magic teardown: when an SV
 * carrying goperl anchor magic (perl_xs_helper op SV_MAGIC_ATTACH) is freed,
 * the guest notifies the host with the u32 host id so it can run the native
 * module's svt_free and drop its mirror MAGIC chain. */
#define GOPERL_MG_FREE_METHOD_ID (-2)

/* Reserved callback method id for pp hooks: a native module that patched a
 * PL_ppaddr slot (host-side proxy table) gets called for every execution of
 * that op type. Payload [u32 op][u32 op_type] and the perl stack (see
 * goperl_pp_stack_words); response [1][u32 next_op] and what the hook made
 * of the stack on success, [0]+croak-message on failure. The
 * hook runs the ORIGINAL pp itself (perl_xs_helper op RUN_ORIGINAL) and
 * decides the next op, exactly like a real PL_ppaddr replacement. */
#define GOPERL_PP_HOOK_METHOD_ID (-3)
#define GOPERL_KEYWORD_METHOD_ID (-7)

/* Reserved callback method id for save-stack destructors registered via
 * perl_xs_helper op SAVE_DESTRUCTOR: fires with the u32 host id when the
 * enclosing guest scope pops (normally or during die unwinding). */
#define GOPERL_DTOR_METHOD_ID (-4)

/* Reserved callback method id for host-side set-magic: when a guest SV whose
 * anchor was upgraded via op SV_MAGIC_SET_HOOK has its value assigned, the
 * guest forwards the u32 host id so the host can run the svt_set hooks of
 * its mirror MAGIC chain (e.g. Moose's re-export flag magic, which clears
 * itself when the glob is overwritten). */
#define GOPERL_MG_SET_METHOD_ID (-5)
/* `local` on an SV that carries the anchor: the payload is the anchor's
 * host id and the token of the SV that takes the variable's place. */
#define GOPERL_MG_LOCAL_METHOD_ID (-8)

/* Reserved callback method id for PerlIO layer hooks: a host-registered
 * layer (op PERLIO_DEF_LAYER) gets its CUSTOM slots (Pushed/Fill/Popped)
 * run on the host; the stock buffered behavior stays guest-native. The
 * payload starts [u32 hook][u32 funcs_id][u32 ff][u32 layer]; responses
 * are hook-specific, [0]+message croaks in the guest. */
#define GOPERL_PERLIO_METHOD_ID (-6)

/* Reserved callback method id for calls of native code that go through one
 * channel: the payload starts with a GOPERL_HOST_ kind, the words that
 * follow are the kind's own. The response is [1] and the kind's result, or
 * [0] and the message to croak with. */
#define GOPERL_HOST_METHOD_ID (-9)
enum {
    GOPERL_HOST_NEWOP = 0, /* an op checker: [type][op] -> [op] */
    GOPERL_HOST_PEEP = 1,  /* a peephole hook: [which][op] */
    GOPERL_HOST_ATEXIT = 2, /* a function given to call_atexit: [id] */
    GOPERL_HOST_CALLCK = 3, /* a call checker: [id][entersubop][namegv][ckobj] -> [op] */
    GOPERL_HOST_MAGIC = 4,  /* a hook of the magic native code attached:
                             * [hook][sv][nsv][name][namlen] -> [result] */
    GOPERL_HOST_UVAR = 5    /* a function of uvar magic: [is_set][arg][sv][key] -> [key][result] */
};
/* The hooks of GOPERL_HOST_MAGIC, and the bits of an anchor's mask. */
enum {
    GOPERL_MGHOOK_GET = 0,
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
    croak_sv(sv_2mortal(thrown));
}

/* A callback of native code that did not ask for G_EVAL (CALL_SV and
 * CALL_METHOD below). Its die cannot unwind the host's C frames, so the
 * call is made inside an eval scope all the same; but a call with G_EVAL
 * empties $@ when it starts and again when the callee returns, which a
 * call without G_EVAL never does, and code relies on that: a method
 * implemented in C that calls back into Perl must not lose the $@ of the
 * eval before it.
 *
 * So the eval scope is opened with G_KEEPERR, which leaves $@ alone on the
 * way in and on the way out, and the call goes through the XSUB below,
 * which takes the scope out of keep-error mode again before it calls the
 * real target: a die in the callee then sets $@ as any die does, instead
 * of turning into an "(in cleanup)" warning. Whether the callee returned
 * is recorded here, not read out of $@, which may hold an older error or a
 * false exception value. */
struct goperl_plain_call {
    SV *target;         /* the code to call, or NULL for a method call */
    const char *method; /* the method name then (the invocant is ST(0)) */
    I32 flags;          /* the caller's flags, without G_DISCARD */
    bool returned;
};
/* The call XS_goperl_plain_call is about to make: set right before the
 * call_sv that enters it and taken (cleared) on entry, so a callback
 * nested in the callee sets its own. */
static goperl_plain_call *goperl_plain_call_pending = NULL;
static CV *goperl_plain_call_cv = NULL;

XS(XS_goperl_plain_call);
XS(XS_goperl_plain_call) {
    dXSARGS;
    goperl_plain_call *call = goperl_plain_call_pending;
    goperl_plain_call_pending = NULL;
    if (!call) Perl_croak(aTHX_ "goperl: no native callback is pending");
    PL_in_eval &= ~EVAL_KEEPERR;
    /* the arguments stay where they are and become the callee's */
    PUSHMARK(SP - items);
    const I32 count = call->method ? call_method(call->method, call->flags)
                                   : call_sv(call->target, call->flags);
    call->returned = true;
    /* the results are where the arguments were */
    XSRETURN(count);
}

/* Native-XSUB request payload: GOPERL_NATIVE_HDR_WORDS u32 header words
 * (fn_id, CV token, item count) followed by the frame's SV tokens. */
#define GOPERL_NATIVE_HDR_WORDS 3

/* Connects this module instance to the host's signal delivery
 * (perl_signal.cc). Every entry point that runs Perl code calls it: the
 * instance may be a snapshot copy the host has not seen yet. */
extern "C" void wasi_signal_attach(pTHX);
extern "C" void perl_wasm_locale_start_up(int on);
/* Ends the process by a signal, as the signal's default action does
 * (perl_signal.cc). Does not return. */
extern "C" __attribute__((noreturn)) void wasi_signal_die(int sig);

/* Host-writable interrupt flag. The host trips a runaway eval by storing 1 here
 * via a plain linear-memory write (perl_interrupt_addr returns its address); no
 * wasm code runs on the instance. The interruptible run loop clears it and
 * croaks. `volatile` so the compiler always reloads it in the op loop. */
static volatile uint32_t g_interrupt = 0;

/* pp hooks: a host-native module (via the go-perl XS SDK) can claim op
 * types; the run loop then routes every execution of those types to the
 * host (GOPERL_PP_HOOK_METHOD_ID) instead of the pp function. The guest PL_ppaddr
 * itself is never patched — the "original" op is always available to the
 * hook through the RUN_ORIGINAL helper. */
static uint8_t g_pp_hooked[MAXO];
static uint32_t g_pp_hook_count = 0;

/* Per-op host dispatch: a native module wrote one op's op_ppaddr; that op
 * (and only that op) executes through the pp-hook callback. */
static OP *goperl_pp_hooked_op(pTHX);

static void goperl_native_enter(pTHX);
static void goperl_native_leave(void);

/* Host keyword/infix plugin forwarding (go-perl XS SDK, method -7). */
static Perl_keyword_plugin_t g_next_keyword_plugin = NULL;
static Perl_infix_plugin_t g_next_infix_plugin = NULL;

static int goperl_keyword_plugin(pTHX_ char *keyword_ptr, STRLEN keyword_len,
                                 OP **op_ptr) {
    if (g_go_cb && keyword_len > 0 && keyword_len < 256) {
        unsigned char buf[4 + 256];
        uint32_t kind = 0;
        memcpy(buf, &kind, 4);
        memcpy(buf + 4, keyword_ptr, keyword_len);
        goperl_native_enter(aTHX);
        int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_KEYWORD_METHOD_ID,
                                             buf, 4 + (uint32_t)keyword_len);
        goperl_native_leave();
        uint32_t resp_ptr = (uint32_t)((uint64_t)rc >> 32);
        uint32_t resp_len = (uint32_t)((uint64_t)rc & 0xFFFFFFFFu);
        if (resp_ptr != 0 && resp_len >= 1) {
            unsigned char *resp = (unsigned char *)(uintptr_t)resp_ptr;
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
            if (resp_len >= 10 && resp[1]) {
                uint32_t ret = 0, tok = 0;
                memcpy(&ret, resp + 2, 4);
                memcpy(&tok, resp + 6, 4);
                free(resp);
                *op_ptr = (OP *)(uintptr_t)tok;
                return (int)ret;
            }
            free(resp);
        }
    }
    return (*g_next_keyword_plugin)(aTHX_ keyword_ptr, keyword_len, op_ptr);
}

static STRLEN goperl_infix_plugin(pTHX_ char *opname, STRLEN oplen,
                                  struct Perl_custom_infix **def) {
    if (g_go_cb && oplen > 0 && oplen < 256) {
        unsigned char buf[4 + 256];
        uint32_t kind = 1;
        memcpy(buf, &kind, 4);
        memcpy(buf + 4, opname, oplen);
        int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_KEYWORD_METHOD_ID,
                                             buf, 4 + (uint32_t)oplen);
        uint32_t resp_ptr = (uint32_t)((uint64_t)rc >> 32);
        uint32_t resp_len = (uint32_t)((uint64_t)rc & 0xFFFFFFFFu);
        if (resp_ptr != 0 && resp_len >= 1) {
            unsigned char *resp = (unsigned char *)(uintptr_t)resp_ptr;
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
 * jumps (GOPERL_HOOK_UNWIND).
 *
 * A die that passes through a native call from inside leaves the call's
 * entry behind; such an entry is from a deeper context than any call that
 * is still running, and is dropped when it is found on top. */
struct goperl_native_mark {
    I32 save_ix;  /* PL_savestack_ix */
    I32 scope_ix; /* PL_scopestack_ix */
    I32 cx_ix;    /* cxstack_ix */
    PERL_SI *si;
};
static std::vector<goperl_native_mark> g_native_marks;
static void goperl_native_marks_settle(pTHX) {
    while (!g_native_marks.empty()) {
        const goperl_native_mark &m = g_native_marks.back();
        if (m.si == PL_curstackinfo && m.cx_ix <= cxstack_ix &&
            m.save_ix <= PL_savestack_ix)
            break;
        g_native_marks.pop_back();
    }
}
static void goperl_native_enter(pTHX) {
    goperl_native_marks_settle(aTHX);
    g_native_marks.push_back({PL_savestack_ix, PL_scopestack_ix, cxstack_ix, PL_curstackinfo});
}
static void goperl_native_leave(void) {
    if (!g_native_marks.empty()) g_native_marks.pop_back();
}

/* One call of native code through the host-call channel: kind and its
 * words go out, the response comes back (the caller frees it; *rlen is its
 * length, the result starts at byte 1). A failure reported by the host is
 * this interpreter's die. */
static unsigned char *goperl_host_call(pTHX_ uint32_t kind, const uint32_t *words,
                                       uint32_t nwords, uint32_t *rlen) {
    uint32_t buf[1 + 8];
    if (nwords > 8) Perl_croak(aTHX_ "goperl: host call with too many words");
    buf[0] = kind;
    for (uint32_t i = 0; i < nwords; i++) buf[1 + i] = words[i];
    goperl_native_enter(aTHX);
    int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_HOST_METHOD_ID, buf,
                                         sizeof(uint32_t) * (1 + nwords));
    goperl_native_leave();
    uint32_t resp_ptr = (uint32_t)((uint64_t)rc >> 32);
    uint32_t resp_len = (uint32_t)((uint64_t)rc & 0xFFFFFFFFu);
    if (resp_ptr == 0 || resp_len < 1)
        Perl_croak(aTHX_ "goperl: host call %u: empty response", (unsigned)kind);
    unsigned char *resp = (unsigned char *)(uintptr_t)resp_ptr;
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
    const uint32_t words[2] = {(uint32_t)type, (uint32_t)(uintptr_t)o};
    uint32_t rlen = 0;
    unsigned char *resp = goperl_host_call(aTHX_ GOPERL_HOST_NEWOP, words, 2, &rlen);
    uint32_t tok = 0;
    if (rlen >= 5) memcpy(&tok, resp + 1, 4);
    free(resp);
    return (OP *)(uintptr_t)tok;
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
    const uint32_t words[4] = {(uint32_t)SvUV(*idp), (uint32_t)(uintptr_t)entersubop,
                               (uint32_t)(uintptr_t)namegv,
                               (uint32_t)(uintptr_t)(objp ? *objp : NULL)};
    uint32_t rlen = 0;
    unsigned char *resp = goperl_host_call(aTHX_ GOPERL_HOST_CALLCK, words, 4, &rlen);
    uint32_t tok = 0;
    if (rlen >= 5) memcpy(&tok, resp + 1, 4);
    free(resp);
    return (OP *)(uintptr_t)tok;
}

/* The peephole optimizer hooks of native code (PL_peepp and PL_rpeepp),
 * the same way: [0] is PL_peepp's previous value, [1] PL_rpeepp's. */
static peep_t g_peep_orig[2];

static void goperl_peep_dispatch(pTHX_ int which, OP *o) {
    if (g_go_cb == 0) {
        if (g_peep_orig[which]) g_peep_orig[which](aTHX_ o);
        return;
    }
    const uint32_t words[2] = {(uint32_t)which, (uint32_t)(uintptr_t)o};
    uint32_t rlen = 0;
    free(goperl_host_call(aTHX_ GOPERL_HOST_PEEP, words, 2, &rlen));
}
static void goperl_peep_tramp(pTHX_ OP *o) { goperl_peep_dispatch(aTHX_ 0, o); }
static void goperl_rpeep_tramp(pTHX_ OP *o) { goperl_peep_dispatch(aTHX_ 1, o); }

/* A function native code gave to call_atexit: it runs when the interpreter
 * is destroyed. */
static void goperl_atexit_thunk(pTHX_ void *p) {
    if (g_go_cb == 0) return;
    const uint32_t words[1] = {(uint32_t)(uintptr_t)p};
    uint32_t rlen = 0;
    free(goperl_host_call(aTHX_ GOPERL_HOST_ATEXIT, words, 1, &rlen));
}

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
 * The state, as u32 words: the depth of the stack, the index of the top
 * mark, how many marks follow (the top GOPERL_PP_MARKS_MAX at most), those
 * marks from the lowest, then the values of the stack from index 1. */
#define GOPERL_PP_MARKS_MAX 32
static void goperl_pp_stack_words(pTHX_ std::vector<uint32_t> &out) {
    SSize_t depth = PL_stack_sp - PL_stack_base;
    if (depth < 0) depth = 0;
    const SSize_t top = PL_markstack_ptr - PL_markstack;
    const SSize_t nmarks = top > GOPERL_PP_MARKS_MAX ? GOPERL_PP_MARKS_MAX : (top > 0 ? top : 0);
    out.push_back((uint32_t)depth);
    out.push_back((uint32_t)(int32_t)top);
    out.push_back((uint32_t)nmarks);
    for (SSize_t i = top - nmarks + 1; i <= top; i++)
        out.push_back((uint32_t)(int32_t)PL_markstack[i]);
    for (SSize_t i = 1; i <= depth; i++)
        out.push_back((uint32_t)(uintptr_t)PL_stack_base[i]);
}

/* What the host's copy became: [depth][lo] and the values of the indices
 * above lo up to depth, then [top mark][mlo] and the marks above mlo up to
 * the top. Returns false when the words are not that. */
static bool goperl_pp_stack_apply(pTHX_ const unsigned char *p, size_t len) {
    if (len < 8) return false;
    int32_t depth, lo;
    memcpy(&depth, p, 4);
    memcpy(&lo, p + 4, 4);
    if (depth < 0 || lo < 0 || lo > depth) return false;
    const size_t nvals = (size_t)(depth - lo);
    if (len < 8 + nvals * 4 + 8) return false;
    const SSize_t have = PL_stack_max - PL_stack_base;
    if (depth >= have) {
        SV **sp = PL_stack_sp;
        (void)stack_grow(sp, sp, depth - (PL_stack_sp - PL_stack_base) + 1);
    }
    for (size_t i = 0; i < nvals; i++) {
        uint32_t tok;
        memcpy(&tok, p + 8 + i * 4, 4);
        PL_stack_base[lo + 1 + (SSize_t)i] = (SV *)(uintptr_t)tok;
    }
    PL_stack_sp = PL_stack_base + depth;
    const unsigned char *q = p + 8 + nvals * 4;
    int32_t top, mlo;
    memcpy(&top, q, 4);
    memcpy(&mlo, q + 4, 4);
    if (top < 0 || mlo < 0 || mlo > top) return false;
    const size_t nmarks = (size_t)(top - mlo);
    if (len < 8 + nvals * 4 + 8 + nmarks * 4) return false;
    while (PL_markstack + top >= PL_markstack_max) (void)markstack_grow();
    for (size_t i = 0; i < nmarks; i++) {
        int32_t m;
        memcpy(&m, q + 8 + i * 4, 4);
        PL_markstack[mlo + 1 + (SSize_t)i] = (Stack_off_t)m;
    }
    PL_markstack_ptr = PL_markstack + top;
    return true;
}

static OP *goperl_call_pp_hook(pTHX_ OP *op) {
    if (g_go_cb == 0) return op->op_ppaddr(aTHX);
    /* payload: op token, op type, and the stack as it is */
    std::vector<uint32_t> buf;
    buf.push_back((uint32_t)(uintptr_t)op);
    buf.push_back((uint32_t)op->op_type);
    goperl_pp_stack_words(aTHX_ buf);
    goperl_native_enter(aTHX);
    int64_t rc = wasmify_callback_invoke(
        g_go_cb, GOPERL_PP_HOOK_METHOD_ID, buf.data(),
        (uint32_t)(sizeof(uint32_t) * buf.size()));
    goperl_native_leave();
    /* nothing with a destructor may be alive across a croak */
    std::vector<uint32_t>().swap(buf);
    uint32_t resp_ptr = (uint32_t)((uint64_t)rc >> 32);
    uint32_t resp_len = (uint32_t)((uint64_t)rc & 0xFFFFFFFFu);
    if (resp_ptr == 0 || resp_len < 1)
        Perl_croak(aTHX_ "pp hook: empty response");
    unsigned char *resp = (unsigned char *)(uintptr_t)resp_ptr;
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
    uint32_t next = 0;
    if (resp_len >= 5) memcpy(&next, resp + 1, 4);
    const bool applied = resp_len <= 5 || goperl_pp_stack_apply(aTHX_ resp + 5, resp_len - 5);
    free(resp);
    if (!applied) Perl_croak(aTHX_ "pp hook: malformed stack state");
    return (OP *)(uintptr_t)next;
}
static OP *goperl_pp_hooked_op(pTHX) {
    return goperl_call_pp_hook(aTHX_ PL_op);
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
    if (fn == goperl_pp_hooked_op) return true;
    for (uint32_t i = 0; i < g_xop_n; i++)
        if (goperl_pp_tramps[i] == fn) return true;
    return false;
}

static inline OP *goperl_exec_op(pTHX_ OP *op) {
    if (g_pp_hook_count && op->op_type < MAXO && g_pp_hooked[op->op_type])
        return goperl_call_pp_hook(aTHX_ op);
    return op->op_ppaddr(aTHX);
}

/* Save-stack destructor bridging save_destructor_x for host modules: fires
 * the host id over GOPERL_DTOR_METHOD_ID when the guest scope pops. */
static void goperl_dtor_thunk(pTHX_ void *p) {
    uint32_t id = (uint32_t)(uintptr_t)p;
    if (g_go_cb == 0) return;
    int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_DTOR_METHOD_ID, &id,
                                         sizeof(id));
    uint32_t resp_ptr = (uint32_t)((uint64_t)rc >> 32);
    if (resp_ptr != 0) free((void *)(uintptr_t)resp_ptr);
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
 * past that call, so the delivery is called as code an eval catches. */
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
    call_sv((SV *)deliver, G_VOID | G_DISCARD | G_NOARGS | G_EVAL);
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
    uint32_t resp_ptr = static_cast<uint32_t>(static_cast<uint64_t>(rc) >> 32);
    uint32_t resp_len = static_cast<uint32_t>(static_cast<uint64_t>(rc) & 0xFFFFFFFFu);
    if (resp_ptr == 0 || resp_len < 1)
        Perl_croak(aTHX_ "Go function dispatch: empty response");
    char *resp = reinterpret_cast<char *>(static_cast<uintptr_t>(resp_ptr));

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
XS(XS_goperl_native_thunk);
XS(XS_goperl_native_thunk) {
    dXSARGS;
    int32_t fn_id = XSANY.any_i32;
    if (g_go_cb == 0)
        Perl_croak(aTHX_ "no Go dispatcher registered for this instance");

    size_t payload_len =
        sizeof(uint32_t) * (GOPERL_NATIVE_HDR_WORDS + (size_t)items);
    uint32_t *payload = (uint32_t *)malloc(payload_len);
    if (!payload) Perl_croak(aTHX_ "native XS dispatch: out of memory");
    payload[0] = (uint32_t)fn_id;
    payload[1] = (uint32_t)(uintptr_t)cv; /* real XSUBs receive their CV */
    payload[2] = (uint32_t)items;
    for (I32 i = 0; i < items; i++)
        payload[GOPERL_NATIVE_HDR_WORDS + i] = (uint32_t)(uintptr_t)ST(i);

    goperl_native_enter(aTHX);
    int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_NATIVE_METHOD_ID,
                                         payload, payload_len);
    goperl_native_leave();
    free(payload);

    uint32_t resp_ptr = (uint32_t)((uint64_t)rc >> 32);
    uint32_t resp_len = (uint32_t)((uint64_t)rc & 0xFFFFFFFFu);
    if (resp_ptr == 0 || resp_len < 1)
        Perl_croak(aTHX_ "native XS dispatch: empty response");
    unsigned char *resp = (unsigned char *)(uintptr_t)resp_ptr;

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
    if (resp_len < 5) { free(resp); Perl_croak(aTHX_ "native XS dispatch: short response"); }
    uint32_t nret;
    memcpy(&nret, resp + 1, sizeof(nret));
    if (resp_len < 5 + (size_t)nret * 4) {
        free(resp);
        Perl_croak(aTHX_ "native XS dispatch: truncated response");
    }
    XSprePUSH;
    EXTEND(SP, (SSize_t)nret);
    for (uint32_t k = 0; k < nret; k++) {
        uint32_t tok;
        memcpy(&tok, resp + 5 + k * 4, sizeof(tok));
        ST(k) = (SV *)(uintptr_t)tok;
    }
    free(resp);
    XSRETURN(nret);
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
uint64_t perl_new(const char *lib_dir, const char *site_dir, const char *site_arch) {
    if (g_my_perl) return 0; /* one interpreter per instance */
    site_configure(site_dir, site_arch);
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
 * PERL_SYS_TERM is intentionally NOT called: it is process teardown, and
 * the instance may create a fresh interpreter afterwards. */
static int destroy_interpreter() {
    PERL_SET_CONTEXT(g_my_perl);
    {
        dTHX;
        reg_clear(aTHX); /* drop registry pins so destruct sees true counts */
        SvREFCNT_dec(g_inc_from_environment);
        g_inc_from_environment = nullptr;
    }
    PL_perl_destruct_level = 2;
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

uint32_t perl_interrupt_addr(uint64_t h) {
    (void)h;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_interrupt));
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
    default: /* TARGET_OP */
        count = call_sv((SV *)g_op_cv[gc.op], gc.gimme | G_EVAL);
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
        uint32_t id = (uint32_t)(uintptr_t)mg->mg_ptr;
        int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_MG_FREE_METHOD_ID,
                                             &id, sizeof(id));
        uint32_t resp_ptr = (uint32_t)((uint64_t)rc >> 32);
        if (resp_ptr != 0) free((void *)(uintptr_t)resp_ptr);
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
 * under an anchor of its own. */
static int goperl_host_mg_local(pTHX_ SV *nsv, MAGIC *mg) {
    if (g_go_cb != 0) {
        uint32_t req[2] = {(uint32_t)(uintptr_t)mg->mg_ptr, (uint32_t)(uintptr_t)nsv};
        int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_MG_LOCAL_METHOD_ID,
                                             req, sizeof(req));
        uint32_t resp_ptr = (uint32_t)((uint64_t)rc >> 32);
        if (resp_ptr != 0) free((void *)(uintptr_t)resp_ptr);
    }
    return 0;
}

/* Set magic of the anchor: assigning to the SV fires the host id over
 * GOPERL_MG_SET_METHOD_ID so mirror svt_set hooks run. */
static int goperl_host_mg_set(pTHX_ SV *sv, MAGIC *mg) {
    PERL_UNUSED_ARG(sv);
    if (g_go_cb != 0) {
        uint32_t id = (uint32_t)(uintptr_t)mg->mg_ptr;
        int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_MG_SET_METHOD_ID,
                                             &id, sizeof(id));
        uint32_t resp_ptr = (uint32_t)((uint64_t)rc >> 32);
        if (resp_ptr != 0) free((void *)(uintptr_t)resp_ptr);
    }
    return 0;
}

static unsigned char *goperl_host_call(pTHX_ uint32_t kind, const uint32_t *words,
                                       uint32_t nwords, uint32_t *rlen);

/* The other hooks of a module's vtable: the host runs the hook of every
 * MAGIC of the SV's mirror chain that has it, and answers with what the
 * last one returned. */
static uint32_t goperl_host_mg_hook(pTHX_ uint32_t hook, SV *sv, SV *nsv,
                                    const char *name, I32 namlen) {
    if (g_go_cb == 0) return 0;
    const uint32_t words[5] = {hook, (uint32_t)(uintptr_t)sv, (uint32_t)(uintptr_t)nsv,
                               (uint32_t)(uintptr_t)name, (uint32_t)namlen};
    uint32_t rlen = 0;
    unsigned char *resp = goperl_host_call(aTHX_ GOPERL_HOST_MAGIC, words, 5, &rlen);
    uint32_t result = 0;
    if (rlen >= 5) memcpy(&result, resp + 1, 4);
    free(resp);
    return result;
}
static int goperl_host_mg_get(pTHX_ SV *sv, MAGIC *mg) {
    PERL_UNUSED_ARG(mg);
    return (int)goperl_host_mg_hook(aTHX_ GOPERL_MGHOOK_GET, sv, NULL, NULL, 0);
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

/* uvar magic of native code (sv_magic(sv, NULL, PERL_MAGIC_uvar, &uf, ...)):
 * the functions of the ufuncs this interpreter is given are these, and the
 * module's own are the host's to call. A hash calls uf_val with the kind
 * of access and has the key in the MAGIC's mg_obj, where the function may
 * put another; a scalar calls uf_val and uf_set with the index of the
 * ufuncs, which the host knows. */
static I32 goperl_uvar_call(pTHX_ uint32_t is_set, IV arg, SV *sv) {
    if (g_go_cb == 0) return 0;
    MAGIC *mg = mg_find(sv, PERL_MAGIC_uvar);
    const uint32_t words[4] = {is_set, (uint32_t)(int32_t)arg, (uint32_t)(uintptr_t)sv,
                               (uint32_t)(uintptr_t)(mg ? mg->mg_obj : NULL)};
    uint32_t rlen = 0;
    unsigned char *resp = goperl_host_call(aTHX_ GOPERL_HOST_UVAR, words, 4, &rlen);
    uint32_t key = words[3], result = 0;
    if (rlen >= 9) {
        memcpy(&key, resp + 1, 4);
        memcpy(&result, resp + 5, 4);
    }
    free(resp);
    /* the MAGIC may be another one by now */
    mg = mg_find(sv, PERL_MAGIC_uvar);
    if (mg) mg->mg_obj = (SV *)(uintptr_t)key;
    return (I32)result;
}
static I32 goperl_uvar_val(pTHX_ IV arg, SV *sv) { return goperl_uvar_call(aTHX_ 0, arg, sv); }
static I32 goperl_uvar_set(pTHX_ IV arg, SV *sv) { return goperl_uvar_call(aTHX_ 1, arg, sv); }

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
    size_t n = 16 + extralen;
    unsigned char *buf = (unsigned char *)malloc(n);
    uint32_t head[4];
    head[0] = hook;
    head[1] = l->funcs_id;
    head[2] = (uint32_t)(uintptr_t)f;
    head[3] = (uint32_t)(uintptr_t)l;
    memcpy(buf, head, 16);
    if (extralen) memcpy(buf + 16, extra, extralen);
    int64_t rc =
        wasmify_callback_invoke(g_go_cb, GOPERL_PERLIO_METHOD_ID, buf, n);
    free(buf);
    uint32_t resp_ptr = (uint32_t)((uint64_t)rc >> 32);
    uint32_t rlen = (uint32_t)((uint64_t)rc & 0xFFFFFFFFu);
    if (resp_ptr == 0 || rlen < 1)
        Perl_croak(aTHX_ "PerlIO layer hook: empty response");
    unsigned char *resp = (unsigned char *)(uintptr_t)resp_ptr;
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
    /* extra: [u32 arg token][mode bytes NUL] */
    char extra[64];
    uint32_t argtok = (uint32_t)(uintptr_t)arg;
    memcpy(extra, &argtok, 4);
    size_t ml = mode ? strlen(mode) : 0;
    if (ml > 55) ml = 55;
    memcpy(extra + 4, mode ? mode : "", ml);
    extra[4 + ml] = '\0';
    uint32_t rlen = 0;
    unsigned char *resp =
        goperlio_call(aTHX_ GOPERLIO_HOOK_PUSHED, l, f, extra, 5 + ml, &rlen);
    if (rlen >= 9) {
        uint32_t setf, clrf;
        memcpy(&setf, resp + 1, 4);
        memcpy(&clrf, resp + 5, 4);
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
    /* extra: [u32 buf][u32 bufsiz][u32 flags] */
    uint32_t extra[3];
    extra[0] = (uint32_t)(uintptr_t)b->buf;
    extra[1] = (uint32_t)b->bufsiz;
    extra[2] = PerlIOBase(f)->flags;
    uint32_t rlen = 0;
    unsigned char *resp = goperlio_call(aTHX_ GOPERLIO_HOOK_FILL, l, f, extra,
                                        sizeof extra, &rlen);
    /* response: [1][u32 status][u32 ptrOff][u32 endOff][u32 setflags] */
    IV ret = -1;
    if (rlen >= 17) {
        uint32_t status, ptrOff, endOff, setf;
        memcpy(&status, resp + 1, 4);
        memcpy(&ptrOff, resp + 5, 4);
        memcpy(&endOff, resp + 9, 4);
        memcpy(&setf, resp + 13, 4);
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
     * s = u32 words (b of them, in bytes). */
    GOPERL_OP_TREE = 203
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
                                     * of the name the sub has by itself<<32 |
                                     * length<<1 | utf8, or 0 */
};


/* OP_DUMP modes (b = cap<<8 | mode; bit 7 of the mode: the op is a COP
 * whatever its type says, as PL_compiling is). */
enum {
    GOPERL_OPDUMP_ONE = 0,     /* the op */
    GOPERL_OPDUMP_SUBTREE = 1, /* and the ops below it */
    GOPERL_OPDUMP_REACH = 2,   /* and every op that can be reached from it */
    GOPERL_OPDUMP_FREED = 3    /* no op: the described ops freed since the
                                * last time, as tokens (address<<32 | count) */
};
/* One op in the answer of OP_DUMP, as u32 words. */
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

/* CALL selectors (a = selector<<32 | token). */
enum {
    GOPERL_CALL_PV_DISPLAY = 0,
    GOPERL_CALL_PV_ESCAPE = 1,
    GOPERL_CALL_PV_PRETTY = 2,
    GOPERL_CALL_CV_CLONE = 3,
    GOPERL_CALL_NEW_WARNINGS = 4, /* Perl_new_warnings_bitfield */
    GOPERL_CALL_GETCWD_SV = 5,
    GOPERL_CALL_COP_SET = 6,      /* b = field<<32 | value */
    GOPERL_CALL_MESS_SV = 7,
    GOPERL_CALL_MG_FIND = 8, /* b = type -> the address of the words, or 0 */
    GOPERL_CALL_ATOF = 9,    /* s = the text -> the bits of the NV */
    GOPERL_CALL_AMT = 10,    /* a = stash -> the address of the overload table's
                              * words: [flags][fallback][was_ok_sub][n], n CVs */
    GOPERL_CALL_UVAR = 11,   /* uvar magic whose functions are the host's */
    GOPERL_CALL_XOP = 12,    /* custom_op_register: b = class, s = name NUL
                              * description NUL -> the op's number */
    GOPERL_CALL_COP_SAVE = 13, /* b: 0 SAVECOPLINE, 1 SAVECOPFILE_FREE */
    GOPERL_CALL_LEX_START = 14, /* a = the line (or 0), b = flags */
    GOPERL_CALL_PARSER = 15, /* -> the address of the parser's fields, or 0 */
    GOPERL_CALL_SCAN_VERSION = 17, /* a = the SV to fill; b = qv<<32 | the
                              * address of the text in linear memory, or 0 for
                              * the text in s -> how many bytes were taken */
    GOPERL_CALL_NEWGVGEN = 18, /* s = the package, b = flags -> a new glob */
    GOPERL_CALL_RANDOM = 19, /* b low 2 bits: 0 Drand01() -> the bits of the
                              * double; 1 seedDrand01(b >> 2); 2 seed() */
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
    GOPERL_PARSERW_WORDS = 26
};
/* COP_SET fields. */
enum {
    GOPERL_COPF_LINE = 0,
    GOPERL_COPF_WARNINGS = 1,
    GOPERL_COPF_HINTS = 2,
    GOPERL_COPF_STASH = 3,
    GOPERL_COPF_FILE = 4, /* s = the name */
    GOPERL_COPF_FEATURES = 5
};

/* FIELD selectors (a = selector<<32 | token, b = the value to store). */
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
    GOPERL_FIELD_LV_TARG_SET = 19
};
static_assert(HvAUXf_IS_CLASS == 0x4, "the SDK's HvAUXf_IS_CLASS is this build's");

/* REGEXP selectors (a = selector<<32 | token). */
enum {
    GOPERL_RX_OF_SV = 0,   /* SvRX */
    GOPERL_RX_COMP = 1,    /* pregcomp(pattern sv, b = flags) */
    GOPERL_RX_EXEC = 2,    /* pregexec; s = six u32 */
    GOPERL_RX_BODY = 3,    /* the fields of the regexp, as u32 words */
    GOPERL_RX_OFFS = 4,    /* b = paren -> start<<32 | end */
    GOPERL_RX_COMP_PV = 5  /* re_compile of bytes: s = pattern, b = flags<<32 | len */
};

/* HOOK selectors (a = selector). */
enum {
    GOPERL_HOOK_NEWOP_SET = 0,  /* b = type<<1 | on */
    GOPERL_HOOK_NEWOP_ORIG = 1, /* b = type<<32 | op -> op */
    GOPERL_HOOK_PEEP_SET = 2,   /* b = which<<1 | on (0 PL_peepp, 1 PL_rpeepp) */
    GOPERL_HOOK_PEEP_ORIG = 3,  /* b = which<<32 | op */
    GOPERL_HOOK_ATEXIT = 4,     /* b = host id */
    GOPERL_HOOK_UNWIND = 5      /* leave what the running native call opened */
};

/* PP_STACK selectors (a = selector). */
enum {
    GOPERL_PPSTACK_PULL = 0, /* -> address<<32 | words of the state */
    GOPERL_PPSTACK_PUSH = 1  /* s = the state, b = its length in bytes */
};

/* CX selectors (a = selector<<32 | stackinfo token or 0). */
enum {
    GOPERL_CX_DUMP = 0,     /* b = index -> address of the fields, or 0 */
    GOPERL_CX_DOUNWIND = 1, /* b = index */
    GOPERL_CX_MAX = 2       /* cxstack_max */
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
    GOPERL_PIO_APPLY_LAYERS = 31
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
    GOPERL_SVCALL_TIED = 56, /* b = the kind of tie magic: 1<<32 | the object
                 * the SV is tied to (SvTIED_obj), or 0 when it is not tied */
    GOPERL_SVCALL_PV_AS = 57, /* the string of the SV in an encoding:
                 * b = how<<32 | SV_GMAGIC or 0, how = 1 UTF-8, 2 bytes, plus
                 * 0x100 for "or null" (an undefined value has no string)
                 * -> (linear-memory ptr << 32) | len, ptr 0 for no string */
    GOPERL_SVCALL_PV_FORCE_AS = 58, /* the SV made a string of its own, to be
                 * written to: b = how<<32 | SV_GMAGIC or 0, how = 0 as it
                 * is, 1 UTF-8, 2 bytes -> (linear-memory ptr << 32) | len */
    GOPERL_SVCALL_CMP_LOCALE = 60, /* as CMP, by the rules of the locale */
    GOPERL_SVCALL_AMAGIC = 61, /* amagic_call: the SV is the left operand, b
                                * the right one | the method's index << 32 |
                                * the AMGf_ flags << 40; the result, or 0
                                * when the operator is not overloaded */
    GOPERL_SVCALL_AV_BODY = 59 /* the body of an array as it is in memory:
                 * (AvARRAY << 32) | the number of elements (AvFILLp + 1).
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
    GOPERL_LEX_STUFF_PVN = 5
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
    GOPERL_PAD_SAVE_CLEARSV = 10 /* b = pad index */
};
enum {
    GOPERL_CC_IDFIRST = 0,
    GOPERL_CC_IDCONT = 1,
    GOPERL_CC_WORDCHAR = 2,
    GOPERL_CC_SPACE = 3,
    GOPERL_CC_DIGIT = 4,
    GOPERL_CC_ALPHA = 5
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
                             std::vector<uint32_t> &out) {
    uint32_t w[GOPERL_OPREC_WORDS];
    memset(w, 0, sizeof w);
    if (o->op_type == OP_FREED) {
        /* freed without a word to the free hook (the interpreter drops an
         * op it has just merged into another that way): its type says so,
         * and nothing else of it means anything */
        w[GOPERL_OPREC_TOK] = (uint32_t)(uintptr_t)o;
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
    w[GOPERL_OPREC_TOK] = (uint32_t)(uintptr_t)o;
    w[GOPERL_OPREC_TARG] = (uint32_t)o->op_targ;
    w[GOPERL_OPREC_TYPE] = (uint32_t)o->op_type | ((uint32_t)o->op_flags << 16) |
                           ((uint32_t)o->op_private << 24);
    uint32_t bits = (uint32_t)cls << 8;
    if (goperl_pp_is_hook(o->op_ppaddr)) bits |= GOPERL_OPBIT_HOOKED;
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
    w[GOPERL_OPREC_NEXT] = (uint32_t)(uintptr_t)o->op_next;
    w[GOPERL_OPREC_SIBPARENT] = (uint32_t)(uintptr_t)o->op_sibparent;
    if (o->op_flags & OPf_KIDS) w[GOPERL_OPREC_FIRST] = (uint32_t)(uintptr_t)cUNOPx(o)->op_first;
    if (raw) {
        /* the one link it has room for beside op_first: op_other or
         * op_last, whichever the module's struct makes of it */
        w[GOPERL_OPREC_OTHER] = (uint32_t)(uintptr_t)cLOGOPx(o)->op_other;
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
            w[GOPERL_OPREC_LAST] = (uint32_t)(uintptr_t)cBINOPx(o)->op_last;
    }
    /* A call whose children are its arguments themselves was made out of
     * the list of them (op_convert_list) and is a list op in memory, though
     * its class is that of a unary op; ck_subr tells that shape from the
     * other, a call above an ex-list, the same way. */
    if (o->op_type == OP_ENTERSUB && (o->op_flags & OPf_KIDS) && cUNOPx(o)->op_first &&
        OpHAS_SIBLING(cUNOPx(o)->op_first))
        w[GOPERL_OPREC_LAST] = (uint32_t)(uintptr_t)cLISTOPx(o)->op_last;
    switch (cls) {
    case OPclass_BINOP:
    case OPclass_LISTOP:
    case OPclass_PMOP:
        w[GOPERL_OPREC_LAST] = (uint32_t)(uintptr_t)cBINOPx(o)->op_last;
        break;
    case OPclass_LOGOP:
        w[GOPERL_OPREC_OTHER] = (uint32_t)(uintptr_t)cLOGOPx(o)->op_other;
        break;
    case OPclass_LOOP:
        w[GOPERL_OPREC_LAST] = (uint32_t)(uintptr_t)cLOOPx(o)->op_last;
        w[GOPERL_OPREC_REDOOP] = (uint32_t)(uintptr_t)cLOOPx(o)->op_redoop;
        w[GOPERL_OPREC_NEXTOP] = (uint32_t)(uintptr_t)cLOOPx(o)->op_nextop;
        w[GOPERL_OPREC_LASTOP] = (uint32_t)(uintptr_t)cLOOPx(o)->op_lastop;
        break;
    case OPclass_SVOP:
        w[GOPERL_OPREC_SV] = (uint32_t)(uintptr_t)cSVOPx_sv(o);
        break;
    case OPclass_PADOP:
        w[GOPERL_OPREC_SV] = (uint32_t)(uintptr_t)PAD_SVl(cPADOPx(o)->op_padix);
        break;
    case OPclass_METHOP:
        if (!(o->op_flags & OPf_KIDS))
            w[GOPERL_OPREC_SV] = (uint32_t)(uintptr_t)cMETHOPx_meth(o);
        break;
    case OPclass_PVOP:
        w[GOPERL_OPREC_PV] = (uint32_t)(uintptr_t)cPVOPx(o)->op_pv;
        break;
    case OPclass_UNOP_AUX:
        if (o->op_type == OP_ARGELEM)
            /* the index is kept where the pointer would be */
            w[GOPERL_OPREC_AUX_N] = (uint32_t)PTR2IV(cUNOP_AUXx(o)->op_aux);
        if (o->op_type == OP_MULTIPARAM)
            /* the op_multiparam_aux, which the host reads field by field */
            w[GOPERL_OPREC_AUX] = (uint32_t)(uintptr_t)cUNOP_AUXx(o)->op_aux;
        if (o->op_type == OP_MULTIDEREF) {
            UNOP_AUX_item *items = cUNOP_AUXx(o)->op_aux;
            w[GOPERL_OPREC_AUX] = (uint32_t)(uintptr_t)items;
            w[GOPERL_OPREC_AUX_N] = (uint32_t)items[-1].uv;
        }
        break;
    case OPclass_COP: {
        COP *cop = (COP *)o;
        const char *file = CopFILE(cop);
        const char *stash = CopSTASHPV(cop);
        w[GOPERL_OPREC_LINE] = (uint32_t)CopLINE(cop);
        w[GOPERL_OPREC_HINTS] = (uint32_t)cop->cop_hints;
        w[GOPERL_OPREC_FILE] = (uint32_t)(uintptr_t)file;
        w[GOPERL_OPREC_FILE_LEN] = file ? (uint32_t)strlen(file) : 0;
        w[GOPERL_OPREC_STASH] = (uint32_t)(uintptr_t)stash;
        w[GOPERL_OPREC_STASH_LEN] = stash ? (uint32_t)strlen(stash) : 0;
        w[GOPERL_OPREC_WARNINGS] = (uint32_t)(uintptr_t)cop->cop_warnings;
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
                           size_t cap, std::vector<uint32_t> &out) {
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
 * memory, and asks for the list when it is not zero. */
static std::unordered_set<OP *> g_ops_described;
static std::vector<uint32_t> g_ops_freed;
static uint32_t g_ops_freed_count = 0;
static Perl_ophook_t g_next_opfreehook = NULL;
static bool g_opfreehook_set = false;
static void goperl_opfreehook(pTHX_ OP *o) {
    if (!g_raw_ops.empty()) g_raw_ops.erase(o);
    if (!g_custom_built.empty()) g_custom_built.erase(o);
    if (!g_ops_described.empty()) {
        auto it = g_ops_described.find(o);
        if (it != g_ops_described.end()) {
            g_ops_described.erase(it);
            g_ops_freed.push_back((uint32_t)(uintptr_t)o);
            g_ops_freed_count = (uint32_t)g_ops_freed.size();
        }
    }
    if (g_next_opfreehook) g_next_opfreehook(aTHX_ o);
}
static void goperl_opfreehook_install(pTHX) {
    if (g_opfreehook_set) return;
    g_opfreehook_set = true;
    g_next_opfreehook = PL_opfreehook;
    PL_opfreehook = goperl_opfreehook;
}

uint64_t perl_xs_helper(uint64_t h, int32_t op, uint64_t a, uint64_t b, const char *s) {
    if (!g_my_perl || h == 0) return 0;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    switch (op) {
    case GOPERL_OP_SV_IV: /* b bit 0: as an unsigned value; bit 1: no get magic */
        return goperl_sv_integer(aTHX_ (SV *)(uintptr_t)a, cBOOL(b & 1), cBOOL(b & 2));
    case GOPERL_OP_SV_PV: { /* (linear-memory ptr << 32) | len */
        STRLEN len = 0;
        const char *p = SvPV((SV *)(uintptr_t)a, len);
        return ((uint64_t)(uint32_t)(uintptr_t)p << 32) | (uint32_t)len;
    }
    case GOPERL_OP_NEW_IV:
        return (uint64_t)(uintptr_t)newSViv((IV)(int64_t)a);
    case GOPERL_OP_NEW_PVN:
        return (uint64_t)(uintptr_t)newSVpvn(s ? s : "", (STRLEN)b);
    case GOPERL_OP_SV_MORTAL:
        return (uint64_t)(uintptr_t)sv_2mortal((SV *)(uintptr_t)a);
    case GOPERL_OP_NEW_UV:
        return (uint64_t)(uintptr_t)newSVuv((UV)a);
    case GOPERL_OP_NEW_AV:
        return (uint64_t)(uintptr_t)(SV *)newAV();
    case GOPERL_OP_NEW_HV:
        return (uint64_t)(uintptr_t)(SV *)newHV();
    case GOPERL_OP_NEW_RV_INC:
        return (uint64_t)(uintptr_t)newRV_inc((SV *)(uintptr_t)a);
    case GOPERL_OP_AV_PUSH: /* steals a ref, like the C API */
        av_push((AV *)(uintptr_t)a, (SV *)(uintptr_t)b);
        return 0;
    case GOPERL_OP_AV_LEN:
        return (uint64_t)(int64_t)av_len((AV *)(uintptr_t)a);
    case GOPERL_OP_AV_FETCH: {
        SV **slot = av_fetch((AV *)(uintptr_t)a, (SSize_t)(int64_t)b, FALSE);
        return slot ? (uint64_t)(uintptr_t)*slot : 0;
    }
    case GOPERL_OP_HV_STORE: { /* steals a ref */
        hv_store((HV *)(uintptr_t)a, s ? s : "", (I32)strlen(s ? s : ""),
                 (SV *)(uintptr_t)b, 0);
        return 0;
    }
    case GOPERL_OP_HV_FETCH: {
        SV **slot = hv_fetch((HV *)(uintptr_t)a, s ? s : "",
                             (I32)strlen(s ? s : ""), FALSE);
        return slot ? (uint64_t)(uintptr_t)*slot : 0;
    }
    case GOPERL_OP_REFCNT_INC:
        return (uint64_t)(uintptr_t)SvREFCNT_inc((SV *)(uintptr_t)a);
    case GOPERL_OP_GV_STASHPV:
        return (uint64_t)(uintptr_t)gv_stashpv(s ? s : "", (I32)(int64_t)a);
    case GOPERL_OP_SV_BLESS:
        return (uint64_t)(uintptr_t)sv_bless((SV *)(uintptr_t)a,
                                             (HV *)(uintptr_t)b);
    case GOPERL_OP_SV_RV: { /* SvRV token when SvROK, else 0 */
        SV *sv = (SV *)(uintptr_t)a;
        return SvROK(sv) ? (uint64_t)(uintptr_t)SvRV(sv) : 0;
    }
    case GOPERL_OP_SV_TYPE: /* the raw svtype enum value (the SDK pins the same
              * enum, so SVt_* comparisons agree across the boundary) */
        return (uint64_t)SvTYPE((SV *)(uintptr_t)a);
    case GOPERL_OP_SV_ISA:
        return sv_isa((SV *)(uintptr_t)a, s ? s : "") ? 1 : 0;
    case GOPERL_OP_SV_DERIVED_FROM:
        return sv_derived_from((SV *)(uintptr_t)a, s ? s : "") ? 1 : 0;
    case GOPERL_OP_SETREF_IV: /* bless rv `a` as class s pointing at IV b */
    {
        SV *rv = (SV *)(uintptr_t)a;
        sv_setiv(newSVrv(rv, s ? s : ""), (IV)(int64_t)b);
        return (uint64_t)(uintptr_t)rv;
    }

    /* ---- v3: the surface interpreter-heavy XS (Text::Xslate class)
     * compiles against. Conventions as above: a/b are u64 operands
     * (SV/AV/HV/HE tokens in the low 32 bits when packed), s+b carry
     * strings or packed u32 token arrays. */
    case GOPERL_OP_SV_SETSV:
        sv_setsv((SV *)(uintptr_t)a, (SV *)(uintptr_t)b);
        return 0;
    case GOPERL_OP_SV_SETSV_NOMG:
        sv_setsv_nomg((SV *)(uintptr_t)a, (SV *)(uintptr_t)b);
        return 0;
    case GOPERL_OP_SV_SETSV_MG:
        sv_setsv_mg((SV *)(uintptr_t)a, (SV *)(uintptr_t)b);
        return 0;
    case GOPERL_OP_SV_SETIV:
        sv_setiv((SV *)(uintptr_t)a, (IV)(int64_t)b);
        return 0;
    case GOPERL_OP_SV_SETUV:
        sv_setuv((SV *)(uintptr_t)a, (UV)b);
        return 0;
    case GOPERL_OP_SV_SETNV: { /* b = IEEE bits */
        union { uint64_t u; double d; } cvt;
        cvt.u = b;
        sv_setnv((SV *)(uintptr_t)a, (NV)cvt.d);
        return 0;
    }
    case GOPERL_OP_SV_SETPVN:
        sv_setpvn((SV *)(uintptr_t)a, s ? s : "", (STRLEN)b);
        return 0;
    case GOPERL_OP_SV_NV: { /* -> IEEE bits */
        union { uint64_t u; double d; } cvt;
        cvt.d = (double)SvNV((SV *)(uintptr_t)a);
        return cvt.u;
    }
    case GOPERL_OP_NEW_NV: { /* a = IEEE bits */
        union { uint64_t u; double d; } cvt;
        cvt.u = a;
        return (uint64_t)(uintptr_t)newSVnv((NV)cvt.d);
    }
    case GOPERL_OP_NEW_SVSV:
        return (uint64_t)(uintptr_t)newSVsv((SV *)(uintptr_t)a);
    case GOPERL_OP_SV_MORTALCOPY:
        return (uint64_t)(uintptr_t)sv_mortalcopy((SV *)(uintptr_t)a);
    case GOPERL_OP_SV_CATSV:
        sv_catsv((SV *)(uintptr_t)a, (SV *)(uintptr_t)b);
        return 0;
    case GOPERL_OP_SV_CATSV_NOMG:
        sv_catsv_nomg((SV *)(uintptr_t)a, (SV *)(uintptr_t)b);
        return 0;
    case GOPERL_OP_SV_CATPVN:
        sv_catpvn((SV *)(uintptr_t)a, s ? s : "", (STRLEN)b);
        return 0;
    case GOPERL_OP_SV_TRUE:
        return SvTRUE((SV *)(uintptr_t)a) ? 1 : 0;
    case GOPERL_OP_SV_INFO: { /* flag bitset (mirrored by the SDK's synthetic
                * SVf_ / GOPERL_INFO_ constants) */
        SV *sv = (SV *)(uintptr_t)a;
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
        if (SvIOK(sv) && SvIsUV(sv)) f |= GOPERL_INFO_ISUV;
        if (SvRMAGICAL(sv)) f |= GOPERL_INFO_RMAGICAL;
        if (SvOBJECT(sv)) f |= GOPERL_INFO_OBJECT;
        if (SvAMAGIC(sv)) f |= 65536;           /* GOPERL_INFO_AMAGIC */
        if (SvIOKp(sv)) f |= 131072;            /* GOPERL_INFO_IOKp */
        if (SvNOKp(sv)) f |= 262144;            /* GOPERL_INFO_NOKp */
        return f;
    }
    case GOPERL_OP_SV_CUR_SET:
        SvCUR_set((SV *)(uintptr_t)a, (STRLEN)b);
        return 0;
    case GOPERL_OP_SV_GROW: { /* -> new PVX (linear-memory offset) */
        char *p = SvGROW((SV *)(uintptr_t)a, (STRLEN)b);
        return (uint64_t)(uint32_t)(uintptr_t)p;
    }
    case GOPERL_OP_SV_EQ:
        return sv_eq((SV *)(uintptr_t)a, (SV *)(uintptr_t)b) ? 1 : 0;
    case GOPERL_OP_SV_CMP:
        return (uint64_t)(int64_t)sv_cmp((SV *)(uintptr_t)a, (SV *)(uintptr_t)b);
    case GOPERL_OP_SV_RVWEAKEN:
        sv_rvweaken((SV *)(uintptr_t)a);
        return 0;
    case GOPERL_OP_SV_2CV: {
        HV *stash = NULL;
        GV *gv = NULL;
        CV *cv = sv_2cv((SV *)(uintptr_t)a, &stash, &gv, (I32)(int64_t)b);
        return (uint64_t)(uintptr_t)cv;
    }
    case GOPERL_OP_GET_SV:
        return (uint64_t)(uintptr_t)get_sv(s ? s : "", (I32)(int64_t)a);
    case GOPERL_OP_GET_CV:
        return (uint64_t)(uintptr_t)get_cv(s ? s : "", (I32)(int64_t)a);
    case GOPERL_OP_GV_FETCH: /* a = flags, b = svtype */
        return (uint64_t)(uintptr_t)gv_fetchpv(s ? s : "", (I32)(int64_t)a,
                                               (svtype)(int64_t)b);
    case GOPERL_OP_AV_STORE: /* b = idx<<32 | sv; takes ownership like the C API */
        av_store((AV *)(uintptr_t)a, (SSize_t)(int32_t)(b >> 32),
                 (SV *)(uintptr_t)(uint32_t)b);
        return 0;
    case GOPERL_OP_AV_EXTEND:
        av_extend((AV *)(uintptr_t)a, (SSize_t)(int64_t)b);
        return 0;
    case GOPERL_OP_AV_FILL:
        av_fill((AV *)(uintptr_t)a, (SSize_t)(int64_t)b);
        return 0;
    case GOPERL_OP_AV_READ: { /* like AV_FETCH but lval-safe (never NULL slot) */
        SV **slot = av_fetch((AV *)(uintptr_t)a, (SSize_t)(int64_t)b, TRUE);
        return slot ? (uint64_t)(uintptr_t)*slot : 0;
    }
    case GOPERL_OP_HV_FETCH_ENT: { /* (b = lval<<32 | keysv) -> HE token */
        HE *he = hv_fetch_ent((HV *)(uintptr_t)a,
                              (SV *)(uintptr_t)(uint32_t)b,
                              (b >> 32) ? TRUE : FALSE, 0U);
        return (uint64_t)(uintptr_t)he;
    }
    case GOPERL_OP_HV_STORE_ENT: { /* b = keysv<<32 | sv; takes value
                 * ownership -> the HE token (callers treat NULL as failure
                 * and roll their reference back) */
        HE *he = hv_store_ent((HV *)(uintptr_t)a,
                              (SV *)(uintptr_t)(uint32_t)(b >> 32),
                              (SV *)(uintptr_t)(uint32_t)b, 0U);
        return (uint64_t)(uintptr_t)he;
    }
    case GOPERL_OP_HV_EXISTS_ENT: /* b = keysv */
        return hv_exists_ent((HV *)(uintptr_t)a, (SV *)(uintptr_t)b, 0U) ? 1 : 0;
    case GOPERL_OP_HE_VAL:
        return (uint64_t)(uintptr_t)HeVAL((HE *)(uintptr_t)a);
    case GOPERL_OP_HV_ITERINIT:
        return (uint64_t)(int64_t)hv_iterinit((HV *)(uintptr_t)a);
    case GOPERL_OP_HV_ITERNEXT: /* b = the flags of hv_iternext_flags */
        return (uint64_t)(uintptr_t)hv_iternext_flags((HV *)(uintptr_t)a, (I32)(int64_t)b);
    case GOPERL_OP_HV_ITERKEYSV:
        return (uint64_t)(uintptr_t)hv_iterkeysv((HE *)(uintptr_t)a);
    case GOPERL_OP_HV_ITERVAL: /* a = hv, b = he */
        return (uint64_t)(uintptr_t)hv_iterval((HV *)(uintptr_t)a,
                                               (HE *)(uintptr_t)b);
    case GOPERL_OP_ENTER: ENTER; return 0;
    case GOPERL_OP_LEAVE: LEAVE; return 0;
    case GOPERL_OP_SAVETMPS: SAVETMPS; return 0;
    case GOPERL_OP_FREETMPS: FREETMPS; return 0;
    case GOPERL_OP_CALL_SV:   /* a = flags<<32 | sv; s+b = packed u32 arg tokens */
    case GOPERL_OP_CALL_METHOD: { /* a = flags<<32; s = "name\0" + packed tokens */
        I32 flags = (I32)(int64_t)(a >> 32);
        const char *args = s;
        size_t args_len = (size_t)b;
        const char *method = NULL;
        if (op == 66) {
            method = s ? s : "";
            size_t nl = strlen(method) + 1;
            args = s + nl;
            args_len = args_len >= nl ? args_len - nl : 0;
        }
        size_t n = args_len / 4;
        dSP;
        PUSHMARK(SP);
        EXTEND(SP, (SSize_t)n);
        for (size_t i = 0; i < n; i++) {
            uint32_t tok;
            memcpy(&tok, args + i * 4, 4);
            PUSHs((SV *)(uintptr_t)tok);
        }
        PUTBACK;
        /* Context flags (G_SCALAR/G_LIST/G_VOID, G_DISCARD, ...) pass through
         * unchanged so wantarray in the callee is honest. G_EVAL is always
         * forced: an uncaught longjmp would abandon the host callback protocol
         * mid-flight. The host re-raises when the caller did not ask for
         * G_EVAL.
         *
         * A caller that did not ask for G_EVAL gets a call that treats $@
         * as a call without G_EVAL does (see goperl_plain_call). */
        I32 count;
        uint64_t died;
        if (flags & G_EVAL) {
            if (op == 66)
                count = call_method(method, flags);
            else
                count = call_sv((SV *)(uintptr_t)(uint32_t)a, flags);
            died = SvTRUE(ERRSV) ? 1u : 0u;
        } else {
            if (!goperl_plain_call_cv)
                goperl_plain_call_cv = newXS(NULL, XS_goperl_plain_call, __FILE__);
            goperl_plain_call call;
            call.target = op == 66 ? NULL : (SV *)(uintptr_t)(uint32_t)a;
            call.method = method;
            call.flags = flags & ~G_DISCARD;
            call.returned = false;
            goperl_plain_call_pending = &call;
            count = call_sv((SV *)goperl_plain_call_cv,
                            (flags & (G_WANT | G_DISCARD)) | G_EVAL | G_KEEPERR);
            died = call.returned ? 0u : 1u;
        }
        SPAGAIN;
        AV *res = newAV();
        av_extend(res, count > 0 ? count - 1 : 0);
        for (I32 i = count - 1; i >= 0; i--)
            av_store(res, i, SvREFCNT_inc(POPs));
        PUTBACK;
        sv_2mortal((SV *)res);
        return (died << 63) | ((uint64_t)(uint32_t)count << 32) |
               (uint32_t)(uintptr_t)res;
    }
    case GOPERL_OP_ERRSV:
        return (uint64_t)(uintptr_t)ERRSV;
    case GOPERL_OP_SAVE_OP: /* scope-save the current PL_op (the SAVEOP() macro) */
        save_op();
        return 0;
    case GOPERL_OP_RUN_PP: {
        /* a = op_private<<40 | op_flags<<32 | op_type;
         * s = [u32 marks][u32 op_targ][marks x u32 offset][u32 SV token ...],
         * b = its length in bytes.
         *
         * The pp function of an op type, run for an op native code made
         * itself and called through PL_ppaddr (`BINOP myop; ... PL_op =
         * (OP *)&myop; PL_ppaddr[myop.op_type](aTHX)`). It finds what the
         * native function has on its stack: every value, and a mark above
         * as many of them as each offset says. It takes what it takes of
         * both, as any pp function does, and the answer is what is on the
         * stack afterwards and how many of the marks are gone:
         * taken<<56 | count<<32 | an array of the count values.
         *
         * The op is the type, the flags, the private flags and the target
         * of the module's op and nothing else: no children, no other op. */
        const uint32_t op_type = (uint32_t)a;
        if (op_type >= OP_max || b < 8) return 0;
        uint32_t nmarks, targ;
        memcpy(&nmarks, s, 4);
        memcpy(&targ, s + 4, 4);
        if ((uint64_t)8 + (uint64_t)nmarks * 4 > b) return 0;
        const char *const mark_offs = s + 8;
        const char *const toks = mark_offs + (size_t)nmarks * 4;
        const size_t n = ((size_t)b - 8 - (size_t)nmarks * 4) / 4;
        const SSize_t base = PL_stack_sp - PL_stack_base;
        const SSize_t marks_before = PL_markstack_ptr - PL_markstack;
        dSP;
        EXTEND(SP, (SSize_t)n);
        uint32_t next_mark = 0;
        for (size_t i = 0; i <= n; i++) {
            /* the marks above i values */
            while (next_mark < nmarks) {
                uint32_t off;
                memcpy(&off, mark_offs + (size_t)next_mark * 4, 4);
                if (off > i) break;
                PUSHMARK(SP);
                next_mark++;
            }
            if (i == n) break;
            uint32_t tok;
            memcpy(&tok, toks + i * 4, 4);
            PUSHs((SV *)(uintptr_t)tok);
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
        return ((uint64_t)(taken & 0xff) << 56) | ((uint64_t)(count & 0xffffff) << 32) |
               (uint32_t)(uintptr_t)res;
    }
    case GOPERL_OP_MAGIC_ATTACH: { /* b = host_id<<32 | obj token */
        SV *obj = (SV *)(uintptr_t)(uint32_t)b;
        MAGIC *mg = sv_magicext((SV *)(uintptr_t)a, obj, PERL_MAGIC_ext,
                                goperl_anchor_vtbl(0), NULL, 0);
        mg->mg_private = 0;
        mg->mg_ptr = (char *)(uintptr_t)(uint32_t)(b >> 32);
        mg->mg_flags |= MGf_LOCAL; /* see goperl_host_mg_local */
        return 0;
    }
    case GOPERL_OP_MAGIC_ID: {
        SV *sv = (SV *)(uintptr_t)a;
        if (SvTYPE(sv) < SVt_PVMG) return 0;
        for (MAGIC *mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic)
            if (goperl_is_anchor(mg))
                return (uint64_t)(uint32_t)(uintptr_t)mg->mg_ptr;
        return 0;
    }
    case GOPERL_OP_MAGIC_UNATTACH: {
        MAGIC *mg = goperl_anchor_of(aTHX_ (SV *)(uintptr_t)a);
        if (mg) sv_unmagicext((SV *)(uintptr_t)a, PERL_MAGIC_ext, mg->mg_virtual);
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
        case GOPERL_PL_OP: return (uint64_t)(uintptr_t)PL_op;
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
            goperl_opfreehook_install(aTHX);
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
        case GOPERL_PL_ORIGFILENAME: {
            if (!PL_origfilename) return 0;
            return ((uint64_t)(uint32_t)(uintptr_t)PL_origfilename << 32) |
                   (uint32_t)strlen(PL_origfilename);
        }
        case GOPERL_PL_UNSAFE: return PL_unsafe ? 1 : 0;
        case GOPERL_PL_DESTRUCT_LEVEL: return (uint64_t)(int64_t)PL_perl_destruct_level;
        case GOPERL_PL_LASTGV: return (uint64_t)(uintptr_t)PL_last_in_gv;
        case GOPERL_PL_COP_SEQMAX: return (uint64_t)PL_cop_seqmax;
        case GOPERL_PL_FIRSTGV: return (uint64_t)(uintptr_t)PL_firstgv;
        case GOPERL_PL_SECONDGV: return (uint64_t)(uintptr_t)PL_secondgv;
        }
        return 0;
    }
    case GOPERL_OP_PLVAR_SET: { /* b = SV token or raw value */
        SV *val = (SV *)(uintptr_t)(uint32_t)b;
        switch ((int)a) {
        case GOPERL_PL_DIEHOOK: PL_diehook = val; return 0;
        case GOPERL_PL_WARNHOOK: PL_warnhook = val; return 0;
        case GOPERL_PL_PERLDB: PL_perldb = (int)(int64_t)b; return 0;
        case GOPERL_PL_DBSUB: PL_DBsub = (GV *)val; return 0;
        case GOPERL_PL_DBSINGLE: PL_DBsingle = val; return 0;
        case GOPERL_PL_ENDAV: PL_endav = (AV *)val; return 0;
        case GOPERL_PL_CHECKAV: PL_checkav = (AV *)val; return 0;
        case GOPERL_PL_INITAV: PL_initav = (AV *)val; return 0;
        case GOPERL_PL_EXIT_FLAGS: PL_exit_flags = (U8)b; return 0;
        case GOPERL_PL_HINTS: PL_hints = (U32)b; return 0;
        case GOPERL_PL_DEFOUTGV: PL_defoutgv = (GV *)val; return 0;
        case GOPERL_PL_RS: PL_rs = val; return 0;
        case GOPERL_PL_CURSTASH: PL_curstash = (HV *)val; return 0;
        case GOPERL_PL_TAINTED: TAINT_set(cBOOL(b)); return 0;
        case GOPERL_PL_TAINTING: TAINTING_set(cBOOL(b)); return 0;
        case GOPERL_PL_ERRSV: GvSV(PL_errgv) = val; return 0;
        case GOPERL_PL_DEFSV: DEFSV_set(val); return 0;
        case GOPERL_PL_ERRNO: errno = (int)(int64_t)b; return 0;
        case GOPERL_PL_STATUSVALUE: PL_statusvalue = (I32)(int64_t)b; return 0;
        case GOPERL_PL_BEGINAV: PL_beginav = (AV *)val; return 0;
        case GOPERL_PL_UNITCHECKAV: PL_unitcheckav = (AV *)val; return 0;
        case GOPERL_PL_DESTRUCT_LEVEL: PL_perl_destruct_level = (signed char)(int64_t)b; return 0;
        case GOPERL_PL_LOCALIZING: PL_localizing = (U8)b; return 0;
        case GOPERL_PL_SUB_GENERATION: PL_sub_generation = (U32)b; return 0;
        case GOPERL_PL_SAVEBEGIN: PL_savebegin = cBOOL(b); return 0;
        case GOPERL_PL_SRAND_CALLED: PL_srand_called = cBOOL(b); return 0;
        case GOPERL_PL_CURCOP: PL_curcop = (COP *)(uintptr_t)(uint32_t)b; return 0;
        case GOPERL_PL_COMPCV: PL_compcv = (CV *)val; return 0;
        case GOPERL_PL_RESTARTOP: PL_restartop = (OP *)(uintptr_t)(uint32_t)b; return 0;
        case GOPERL_PL_ERRORS: PL_errors = val; return 0;
        case GOPERL_PL_FIRSTGV: PL_firstgv = (GV *)val; return 0;
        case GOPERL_PL_SECONDGV: PL_secondgv = (GV *)val; return 0;
        }
        return 0;
    }
    case GOPERL_OP_SV_UTF8_ON:
        SvUTF8_on((SV *)(uintptr_t)a);
        return 0;
    case GOPERL_OP_SV_POK_ON:
        SvPOK_on((SV *)(uintptr_t)a);
        return 0;
    case GOPERL_OP_SV_DUMP:
        sv_dump((SV *)(uintptr_t)a);
        return 0;
    case GOPERL_OP_SV_REFCNT_DEC:
        SvREFCNT_dec((SV *)(uintptr_t)a);
        return 0;
    case GOPERL_OP_SV_UNMAGIC: /* a = sv, b = how */
        sv_unmagic((SV *)(uintptr_t)a, (int)(int64_t)b);
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
        SV *sv = (SV *)(uintptr_t)a;
        return SvOBJECT(sv) ? (uint64_t)(uintptr_t)SvSTASH(sv) : 0;
    }
    case GOPERL_OP_NEW_SV:
        return (uint64_t)(uintptr_t)newSV((STRLEN)a);
    case GOPERL_OP_SV_UPGRADE:
        sv_upgrade((SV *)(uintptr_t)a, (svtype)(int64_t)b);
        return 0;
    case GOPERL_OP_SV_UTF8_UPGRADE:
        sv_utf8_upgrade((SV *)(uintptr_t)a);
        return 0;
    case GOPERL_OP_SAVE_DELETE: /* (b = signed klen, negative = UTF-8 key) */
        save_delete((HV *)(uintptr_t)a,
                    savepvn(s ? s : "", (I32)((int64_t)b < 0 ? -(int64_t)b
                                                             : (int64_t)b)),
                    (I32)(int64_t)b);
        return 0;
    case GOPERL_OP_SAVE_HELEM: { /* (b = flags<<32 | keysv) */
        HV *hv = (HV *)(uintptr_t)a;
        SV *key = (SV *)(uintptr_t)(uint32_t)b;
        HE *he = hv_fetch_ent(hv, key, TRUE, 0U);
        if (he) save_helem_flags(hv, key, &HeVAL(he), (U32)(b >> 32));
        return 0;
    }
    case GOPERL_OP_NEW_PVN_SHARE: /* (b = signed len, negative = UTF-8) */
        return (uint64_t)(uintptr_t)newSVpvn_share(s ? s : "",
                                                   (SSize_t)(int64_t)b, 0U);
    case GOPERL_OP_SV_LEN:
        return (uint64_t)sv_len((SV *)(uintptr_t)a);
    case GOPERL_OP_NEW_HVHV:
        return (uint64_t)(uintptr_t)(SV *)newHVhv((HV *)(uintptr_t)a);
    case GOPERL_OP_AV_MAKE: { /* (s+b = packed u32 tokens) */
        size_t n = (size_t)b / 4;
        SV **ary = n ? (SV **)malloc(n * sizeof(SV *)) : NULL;
        for (size_t i = 0; i < n; i++) {
            uint32_t tok;
            memcpy(&tok, s + i * 4, 4);
            ary[i] = (SV *)(uintptr_t)tok;
        }
        AV *av = av_make((SSize_t)n, ary ? ary : (SV **)&ary);
        free(ary);
        return (uint64_t)(uintptr_t)(SV *)av;
    }
    case GOPERL_OP_LOOKS_LIKE_NUMBER:
        return looks_like_number((SV *)(uintptr_t)a) ? 1 : 0;
    case GOPERL_OP_SV_AMAGIC:
        return SvAMAGIC((SV *)(uintptr_t)a) ? 1 : 0;
    case GOPERL_OP_AMAGIC_CALL: { /* (a = method<<32 | sv, b = AMGf flags); the
                * right operand is fixed at &PL_sv_undef (the deref-overload
                * calling pattern). */
        SV *res = amagic_call((SV *)(uintptr_t)(uint32_t)a, &PL_sv_undef,
                              (int)(a >> 32), (int)(int64_t)b);
        return (uint64_t)(uintptr_t)res;
    }
    case GOPERL_OP_WARN:
        Perl_warn(aTHX_ "%s", s ? s : "");
        return 0;
    case GOPERL_OP_GET_HV:
        return (uint64_t)(uintptr_t)(SV *)get_hv(s ? s : "", (I32)(int64_t)a);
    case GOPERL_OP_AV_STORE_RAW: { /* (b = idx<<32 | sv): direct AvARRAY slot write
                * with NO refcount side effects — the host's mirror-flush
                * primitive matching a native `AvARRAY(av)[i] = sv`. Any slot
                * the array has room for: code fills the slots of an array it
                * has extended and sets AvFILLp afterwards. */
        AV *av = (AV *)(uintptr_t)a;
        SSize_t idx = (SSize_t)(int32_t)(b >> 32);
        if (SvTYPE((SV *)av) == SVt_PVAV && idx >= 0 && idx <= AvMAX(av))
            AvARRAY(av)[idx] = (SV *)(uintptr_t)(uint32_t)b;
        return 0;
    }

    /* ---- v4: pp hooks, save-stack destructors, and the OP/COP/CV/GV/
     * context introspection surface interpreter-hooking XS (the
     * Devel::NYTProf class) is built on. */
    case GOPERL_OP_PP_HOOK_SET: { /* (a = op_type, b = enable) */
        uint32_t t = (uint32_t)a;
        if (t >= MAXO) return 0;
        uint8_t on = b ? 1 : 0;
        if (g_pp_hooked[t] != on) {
            g_pp_hooked[t] = on;
            if (on) g_pp_hook_count++;
            else if (g_pp_hook_count) g_pp_hook_count--;
        }
        return 0;
    }
    case GOPERL_OP_RUN_ORIGINAL: { /* (a = op_type): execute the real pp for the
                * CURRENT PL_op. The guest PL_ppaddr is never patched, so
                * the table entry is always the original. The type may
                * differ from PL_op->op_type (pp_entersub with type 0). */
        uint32_t t = (uint32_t)a;
        if (t >= MAXO || !PL_op) return 0;
        OP *next = PL_ppaddr[t](aTHX);
        return (uint64_t)(uintptr_t)next;
    }
    case GOPERL_OP_SAVE_DESTRUCTOR: /* (a = host id) */
        save_destructor_x(goperl_dtor_thunk, (void *)(uintptr_t)(uint32_t)a);
        return 0;
    case GOPERL_OP_OP_FIELDS: { /* targ<<32 | private<<24 | flags<<16 | type */
        OP *o = (OP *)(uintptr_t)a;
        return ((uint64_t)(uint32_t)o->op_targ << 32) |
               ((uint64_t)o->op_private << 24) | ((uint64_t)o->op_flags << 16) |
               (uint64_t)o->op_type;
    }
    case GOPERL_OP_OP_PTR: { /* b = a GOPERL_OPPTR_ selector */
        OP *o = (OP *)(uintptr_t)a;
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
            U32 c = PL_opargs[o->op_type] & OA_CLASS_MASK;
            if (c == OA_BINOP || c == OA_LISTOP || c == OA_PMOP ||
                c == OA_LOOP ||
                (o->op_type == OP_CUSTOM && (o->op_flags & OPf_KIDS)))
                return (uint64_t)(uintptr_t)cBINOPx(o)->op_last;
            return 0;
        }
        case GOPERL_OPPTR_OTHER: {
            U32 c = PL_opargs[o->op_type] & OA_CLASS_MASK;
            if (c == OA_LOGOP || o->op_type == OP_CUSTOM)
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
        return (uint64_t)CopLINE((COP *)(uintptr_t)a);
    case GOPERL_OP_COP_FILE: { /* -> (ptr<<32)|len of CopFILE */
        const char *f = CopFILE((COP *)(uintptr_t)a);
        if (!f) return 0;
        return ((uint64_t)(uint32_t)(uintptr_t)f << 32) |
               (uint32_t)strlen(f);
    }
    case GOPERL_OP_COP_STASHPV: { /* -> packed pv */
        const char *s2 = CopSTASHPV((COP *)(uintptr_t)a);
        if (!s2) return 0;
        return ((uint64_t)(uint32_t)(uintptr_t)s2 << 32) |
               (uint32_t)strlen(s2);
    }
    case GOPERL_OP_CV_INFO: { /* depth<<32 | isxsub */
        CV *cv = (CV *)(uintptr_t)a;
        if (SvTYPE((SV *)cv) != SVt_PVCV) return 0;
        return ((uint64_t)(uint32_t)CvDEPTH(cv) << 32) |
               (CvISXSUB(cv) ? 1u : 0u);
    }
    case GOPERL_OP_CV_PTR: { /* b = a GOPERL_CVPTR_ selector */
        CV *cv = (CV *)(uintptr_t)a;
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
        GV *gv = (GV *)(uintptr_t)a;
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
    case GOPERL_OP_GV_NAME: { /* -> packed pv */
        GV *gv = (GV *)(uintptr_t)a;
        if (!isGV_with_GP((SV *)gv)) return 0;
        const char *n = GvNAME(gv);
        if (!n) return 0;
        return ((uint64_t)(uint32_t)(uintptr_t)n << 32) |
               (uint32_t)GvNAMELEN(gv);
    }
    case GOPERL_OP_SV_ISGV_GP:
        return isGV_with_GP((SV *)(uintptr_t)a) ? 1 : 0;
    case GOPERL_OP_HV_NAME: { /* -> packed pv (0 for anonymous stashes) */
        HV *hv = (HV *)(uintptr_t)a;
        if (SvTYPE((SV *)hv) != SVt_PVHV) return 0;
        const char *n = HvNAME(hv);
        if (!n) return 0;
        return ((uint64_t)(uint32_t)(uintptr_t)n << 32) |
               (uint32_t)HvNAMELEN(hv);
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
    case GOPERL_OP_OP_NAME_STR: { /* -> packed pv of PL_op_name[type] */
        uint32_t t = (uint32_t)a;
        if (t >= MAXO) return 0;
        const char *n = PL_op_name[t];
        return ((uint64_t)(uint32_t)(uintptr_t)n << 32) |
               (uint32_t)strlen(n);
    }
    case GOPERL_OP_CV_FILE: { /* -> packed pv of CvFILE */
        CV *cv = (CV *)(uintptr_t)a;
        if (SvTYPE((SV *)cv) != SVt_PVCV) return 0;
        const char *f = CvFILE(cv);
        if (!f) return 0;
        return ((uint64_t)(uint32_t)(uintptr_t)f << 32) |
               (uint32_t)strlen(f);
    }
    case GOPERL_OP_GV_FETCHFILE: /* (s = name, b = len) */
        return (uint64_t)(uintptr_t)gv_fetchfile_flags(s ? s : "",
                                                       (STRLEN)b, 0);
    case GOPERL_OP_HV_CLEAR:
        hv_clear((HV *)(uintptr_t)a);
        return 0;
    case GOPERL_OP_HV_DELETE: { /* a = key length<<32 | hv, s = the key's
                 * bytes, b = flags */
        SV *r = hv_delete((HV *)(uintptr_t)(uint32_t)a, s ? s : "",
                          (I32)(uint32_t)(a >> 32), (I32)(int64_t)b);
        return (uint64_t)(uintptr_t)r;
    }
    case GOPERL_OP_AV_EXISTS:
        return av_exists((AV *)(uintptr_t)a, (SSize_t)(int64_t)b) ? 1 : 0;
    case GOPERL_OP_SV_UTF8_OFF:
        SvUTF8_off((SV *)(uintptr_t)a);
        return 0;
    case GOPERL_OP_SAVE_SCALAR: /* (a = gv) -> localized SV */
        return (uint64_t)(uintptr_t)save_scalar((GV *)(uintptr_t)a);
    case GOPERL_OP_SV_READONLY_ON:
        SvREADONLY_on((SV *)(uintptr_t)a);
        return 0;
    case GOPERL_OP_EVAL_PV: { /* (s = code, a = croak_on_error) */
        SV *r = eval_pv(s ? s : "", (I32)(int64_t)a);
        return (uint64_t)(uintptr_t)r;
    }
    case GOPERL_OP_AV_UNSHIFT:
        av_unshift((AV *)(uintptr_t)a, (SSize_t)(int64_t)b);
        return 0;
    case GOPERL_OP_NEW_CONSTSUB: { /* (a = stash, s = name, b = sv; takes over the
                 * sv reference like the C API) */
        CV *cv = newCONSTSUB((HV *)(uintptr_t)a, s ? s : "",
                             (SV *)(uintptr_t)(uint32_t)b);
        return (uint64_t)(uintptr_t)cv;
    }
    case GOPERL_OP_SV_PVX_RAW: { /* the raw PV buffer pointer, NO stringification
                 * (the SvPVX lvalue-buffer idiom: sv_grow then write). */
        SV *sv = (SV *)(uintptr_t)a;
        if (SvTYPE(sv) < SVt_PV) return 0;
        return (uint64_t)(uint32_t)(uintptr_t)SvPVX(sv);
    }

    /* ---- v5: the Class::MOP/Moose surface. */
    case GOPERL_OP_HV_PKG_GEN: { /* mro generation of a stash ->
                 * has_meta<<32 | pkg_gen (0 when the hash has no aux/meta).
                 * The moral equivalent of mro::get_pkg_gen($stash), read the
                 * same way Moose's mop.c reads it. */
        HV *hv = (HV *)(uintptr_t)a;
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
    case GOPERL_OP_SV_REFCNT:
        return (uint64_t)SvREFCNT((SV *)(uintptr_t)a);
    case GOPERL_OP_GV_INIT: { /* expand a stash stub into a real glob
                 * (b = flags<<32 | stash, s = name) -> the gv token */
        GV *gv = (GV *)(uintptr_t)a;
        HV *stash = (HV *)(uintptr_t)(uint32_t)b;
        gv_init_pvn(gv, stash, s ? s : "", strlen(s ? s : ""),
                    (U32)(b >> 32));
        return (uint64_t)(uintptr_t)gv;
    }
    case GOPERL_OP_GV_AMG: { /* Gv_AMG: does the stash have a (freshly
                 * updated) overload magic table? */
        HV *stash = (HV *)(uintptr_t)a;
        if (SvTYPE((SV *)stash) != SVt_PVHV) return 0;
        return Gv_AMG(stash) ? 1 : 0;
    }
    case GOPERL_OP_SV_AMAGIC_SET: /* b ? SvAMAGIC_on : SvAMAGIC_off (a must
                 * be a reference, per the core macros) */
        if (!SvROK((SV *)(uintptr_t)a)) return 0;
        if (b) SvAMAGIC_on((SV *)(uintptr_t)a);
        else SvAMAGIC_off((SV *)(uintptr_t)a);
        return 1;
    case GOPERL_OP_GV_FETCHMETHOD: { /* method resolution with ISA walk
                 * (a = stash, b = autoload flag, s = name) -> GV token */
        HV *stash = (HV *)(uintptr_t)a;
        if (SvTYPE((SV *)stash) != SVt_PVHV) return 0;
        GV *gv = gv_fetchmethod_autoload(stash, s ? s : "", b ? TRUE : FALSE);
        return (uint64_t)(uintptr_t)gv;
    }
    case GOPERL_OP_SV_READONLY_OFF:
        SvREADONLY_off((SV *)(uintptr_t)a);
        return 0;
    case GOPERL_OP_SV_PV_FORCE: { /* -> (ptr<<32)|len of the writable PV */
        STRLEN len = 0;
        char *p = SvPV_force((SV *)(uintptr_t)a, len);
        return ((uint64_t)(uint32_t)(uintptr_t)p << 32) | (uint32_t)len;
    }
    case GOPERL_OP_SV_CHOP: { /* b = byte offset from SvPVX to chop to */
        SV *sv = (SV *)(uintptr_t)a;
        if (SvTYPE(sv) < SVt_PV || !SvPOK(sv)) return 0;
        sv_chop(sv, SvPVX(sv) + (STRLEN)b);
        return 0;
    }
    case GOPERL_OP_SV_INSERT: /* b = off<<32 | replace_len, s = little */
        sv_insert((SV *)(uintptr_t)a, (STRLEN)(b >> 32),
                  (STRLEN)(uint32_t)b, s ? s : "", strlen(s ? s : ""));
        return 0;
    case GOPERL_OP_SV_UTF8_DECODE:
        return sv_utf8_decode((SV *)(uintptr_t)a) ? 1 : 0;
    case GOPERL_OP_SV_UTF8_DOWNGRADE: /* b = fail_ok */
        return sv_utf8_downgrade((SV *)(uintptr_t)a, b ? TRUE : FALSE) ? 1 : 0;
    case GOPERL_OP_MG_SET: { /* SvSETMAGIC semantics: only a set-magical
                 * SV has a magic chain to run (SvMAGIC on smaller bodies
                 * reads past the struct). */
        SV *sv = (SV *)(uintptr_t)a;
        if (SvSMAGICAL(sv)) mg_set(sv);
        return 0;
    }
    case GOPERL_OP_GIMME_V: /* the context the running XSUB was called in:
                 * the want bits of the calling op, and only without them
                 * the context of the enclosing sub (an XSUB pushes no
                 * context of its own, so block_gimme alone answers for the
                 * caller's caller) */
        return (uint64_t)(int64_t)GIMME_V;
    case GOPERL_OP_CKWARN: /* a = warning category; b = ckWARN_d variant */
        return (b ? ckWARN_d((U32)a) : ckWARN((U32)a)) ? 1 : 0;
    case GOPERL_OP_SV_LEN_BUF: /* SvLEN: the ALLOCATED buffer size */
        return (SvTYPE((SV *)(uintptr_t)a) < SVt_PV)
                   ? 0
                   : (uint64_t)SvLEN((SV *)(uintptr_t)a);
    case GOPERL_OP_HV_STORE_KLEN: { /* a = key length<<32 | hv, b = utf8<<32 |
                 * value token, s = the key's bytes; takes over the value
                 * ref -> stored SV token. hv_common with JUST_SV returns
                 * the SLOT (SV**). */
        SV *val = (SV *)(uintptr_t)(uint32_t)b;
        SV **svp = (SV **)hv_common((HV *)(uintptr_t)(uint32_t)a, NULL, s ? s : "",
                                    (STRLEN)(uint32_t)(a >> 32),
                                    (b >> 32) ? HVhek_UTF8 : 0,
                                    HV_FETCH_ISSTORE | HV_FETCH_JUST_SV, val,
                                    0);
        return svp ? (uint64_t)(uintptr_t)*svp : 0;
    }
    case GOPERL_OP_HV_FETCH_KLEN: { /* a = key length<<32 | hv, b = utf8<<32 |
                 * lval, s = the key's bytes */
        SV **svp = (SV **)hv_common((HV *)(uintptr_t)(uint32_t)a, NULL, s ? s : "",
                                    (STRLEN)(uint32_t)(a >> 32),
                                    (b >> 32) ? HVhek_UTF8 : 0,
                                    HV_FETCH_JUST_SV |
                                        ((uint32_t)b ? HV_FETCH_LVALUE : 0),
                                    NULL, 0);
        return svp ? (uint64_t)(uintptr_t)*svp : 0;
    }
    case GOPERL_OP_HV_DELETE_ENT: { /* b = flags<<32 | keysv */
        SV *r = hv_delete_ent((HV *)(uintptr_t)a,
                              (SV *)(uintptr_t)(uint32_t)b,
                              (I32)(int64_t)(b >> 32), 0);
        return (uint64_t)(uintptr_t)r;
    }
    case GOPERL_OP_AV_POP:
        return (uint64_t)(uintptr_t)av_pop((AV *)(uintptr_t)a);
    case GOPERL_OP_COP_HINTS: /* CopHINTS of a caller-vouched COP */
        return (uint64_t)CopHINTS_get((COP *)(uintptr_t)a);
    case GOPERL_OP_SV_IS_BOOL:
        return SvIsBOOL((SV *)(uintptr_t)a) ? 1 : 0;
    case GOPERL_OP_SV_POK_ONLY:
        SvPOK_only((SV *)(uintptr_t)a);
        return 0;
    case GOPERL_OP_EVAL_SV: { /* a = flags<<32 | sv; results like CALL_SV:
                 * died<<63 | count<<32 | mortal-AV token */
        I32 flags = (I32)(int64_t)(a >> 32);
        dSP;
        I32 count = eval_sv((SV *)(uintptr_t)(uint32_t)a, flags);
        SPAGAIN;
        AV *res = newAV();
        av_extend(res, count > 0 ? count - 1 : 0);
        for (I32 i = count - 1; i >= 0; i--)
            av_store(res, i, SvREFCNT_inc(POPs));
        PUTBACK;
        sv_2mortal((SV *)res);
        uint64_t died = SvTRUE(ERRSV) ? 1u : 0u;
        return (died << 63) | ((uint64_t)(uint32_t)count << 32) |
               (uint32_t)(uintptr_t)res;
    }
    case GOPERL_OP_AV_SHIFT:
        return (uint64_t)(uintptr_t)av_shift((AV *)(uintptr_t)a);
    case GOPERL_OP_SV_2IO: /* the IO sv behind a handle-ish sv (croaks the
                 * way the real one does on non-handles; b = no_croak) */
        if (b) {
            IO *io = NULL;
            dJMPENV;
            int jmp;
            JMPENV_PUSH(jmp);
            if (jmp == 0) io = sv_2io((SV *)(uintptr_t)a);
            JMPENV_POP;
            return (uint64_t)(uintptr_t)(SV *)io;
        }
        return (uint64_t)(uintptr_t)(SV *)sv_2io((SV *)(uintptr_t)a);
    case GOPERL_OP_SV_FORCE_NORMAL:
        sv_force_normal((SV *)(uintptr_t)a);
        return 0;
    case GOPERL_OP_PERLIO_OPEN: { /* s = "mode\0path" -> PerlIO token */
        const char *mode = s ? s : "r";
        const char *path = mode + strlen(mode) + 1;
        PerlIO *io = PerlIO_open(path, mode);
        return (uint64_t)(uintptr_t)io;
    }
    case GOPERL_OP_PERLIO_CLOSE:
        return (uint64_t)(int64_t)PerlIO_close((PerlIO *)(uintptr_t)a);
    case GOPERL_OP_PERLIO_PUTS: /* s+b = bytes */
        return (uint64_t)(int64_t)PerlIO_write((PerlIO *)(uintptr_t)a,
                                               s ? s : "", (Size_t)b);
    case GOPERL_OP_PERLIO_FLUSH:
        return (uint64_t)(int64_t)PerlIO_flush((PerlIO *)(uintptr_t)a);
    case GOPERL_OP_PERLIO_WRITE:
        return (uint64_t)(int64_t)PerlIO_write((PerlIO *)(uintptr_t)a,
                                               s ? s : "", (Size_t)b);
    case GOPERL_OP_IO_OFP: { /* output PerlIO of an IO sv (IoOFP) */
        IO *io = (IO *)(uintptr_t)a;
        if (SvTYPE((SV *)io) != SVt_PVIO) return 0;
        return (uint64_t)(uintptr_t)IoOFP(io);
    }
    case GOPERL_OP_SV_CUR: /* raw SvCUR — NO stringification/get-magic
                 * (SvPV on an undef buffer warns; SvCUR must not) */
        return (SvTYPE((SV *)(uintptr_t)a) < SVt_PV)
                   ? 0
                   : (uint64_t)SvCUR((SV *)(uintptr_t)a);
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
    case GOPERL_OP_PERLIO_NEXT_READ: { /* a = ff, b = dst<<32|len */
        PerlIO *f = (PerlIO *)(uintptr_t)a;
        PerlIO *n = PerlIOValid(f) ? PerlIONext(f) : NULL;
        if (!PerlIOValid(n)) return (uint64_t)(int64_t)-1;
        SSize_t got = PerlIO_read(n, (void *)(uintptr_t)(uint32_t)(b >> 32),
                                  (Size_t)(uint32_t)b);
        return (uint64_t)(int64_t)got;
    }
    case GOPERL_OP_PERLIO_NEXT_FASTGETS: {
        PerlIO *f = (PerlIO *)(uintptr_t)a;
        PerlIO *n = PerlIOValid(f) ? PerlIONext(f) : NULL;
        return (PerlIOValid(n) && PerlIO_fast_gets(n)) ? 1 : 0;
    }
    case GOPERL_OP_PERLIO_NEXT_GETCNT: {
        PerlIO *n = PerlIONext((PerlIO *)(uintptr_t)a);
        return (uint64_t)(int64_t)PerlIO_get_cnt(n);
    }
    case GOPERL_OP_PERLIO_NEXT_GETPTR: {
        PerlIO *n = PerlIONext((PerlIO *)(uintptr_t)a);
        return (uint64_t)(uint32_t)(uintptr_t)PerlIO_get_ptr(n);
    }
    case GOPERL_OP_PERLIO_NEXT_SETPTRCNT: { /* b = ptr<<32|cnt */
        PerlIO *n = PerlIONext((PerlIO *)(uintptr_t)a);
        PerlIO_set_ptrcnt(n, (STDCHAR *)(uintptr_t)(uint32_t)(b >> 32),
                          (SSize_t)(int32_t)(uint32_t)b);
        return 0;
    }
    case GOPERL_OP_PERLIO_NEXT_FILL: {
        PerlIO *n = PerlIONext((PerlIO *)(uintptr_t)a);
        return (uint64_t)(int64_t)PerlIO_fill(n);
    }
    case GOPERL_OP_PERLIO_STATE: { /* b: 0 = eof|error<<1 of NEXT,
                 * 1 = flush NEXT, 2 = flush SELF */
        PerlIO *f = (PerlIO *)(uintptr_t)a;
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
        OP *first = (OP *)(uintptr_t)(uint32_t)k1;
        OP *second = (OP *)(uintptr_t)(uint32_t)k2;
        OP *o = NULL;
        switch (cls) {
        case GOPERL_OPC_BASEOP: o = newOP(type, flags); break;
        case GOPERL_OPC_UNOP: o = newUNOP(type, flags, first); break;
        case GOPERL_OPC_BINOP: o = newBINOP(type, flags, first, second); break;
        case GOPERL_OPC_LISTOP: o = newLISTOP(type, flags, first, second); break;
        case GOPERL_OPC_LOGOP: o = newLOGOP(type, flags, first, second); break;
        case GOPERL_OPC_SVOP:
            o = newSVOP(type, flags, (SV *)(uintptr_t)(uint32_t)k1);
            break;
        case GOPERL_OPC_PVOP:
            Perl_croak(aTHX_ "goperl: newPVOP is not bridged");
            break;
        case GOPERL_OPC_GVOP:
            o = newGVOP(type, flags, (GV *)(uintptr_t)(uint32_t)k1);
            break;
        case GOPERL_OPC_METHOP_NAMED:
            o = newMETHOP_named(type, flags, (SV *)(uintptr_t)(uint32_t)k1);
            break;
        case GOPERL_OPC_STATEOP:
            if (k1)
                Perl_croak(aTHX_ "goperl: newSTATEOP labels are not bridged");
            o = newSTATEOP(flags, NULL, second);
            break;
        case GOPERL_OPC_CONDOP:
            o = newCONDOP(flags, first, second, (OP *)(uintptr_t)(uint32_t)k3);
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
            goperl_opfreehook_install(aTHX);
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
            if (made && cls >= 0 && cls < (int)(sizeof built / sizeof built[0])) {
                goperl_opfreehook_install(aTHX);
                g_custom_built[made] = built[cls];
            }
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
        OP *o1 = (OP *)(uintptr_t)(uint32_t)k1;
        OP *o2 = (OP *)(uintptr_t)(uint32_t)k2;
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
                o1, o2, (int)aux, (OP *)(uintptr_t)(uint32_t)k3);
        }
        Perl_croak(aTHX_ "goperl: unknown optree helper %d", sel);
    }
    case GOPERL_OP_OP_SET: { /* a = op token, b = sel<<32|value */
        OP *o = (OP *)(uintptr_t)a;
        int sel = (int)(b >> 32);
        uint32_t val = (uint32_t)b;
        switch (sel & 0xFFFF) {
        case GOPERL_OPF_NEXT: o->op_next = (OP *)(uintptr_t)val; return 0;
        case GOPERL_OPF_SIBPARENT:
            /* value's bit 32 rode in with sel: sel = OPF_SIBPARENT|moresib<<?
             * — host packs moresib in val's high bit companion: the selector
             * word carries it (sel>>16). */
            o->op_sibparent = (OP *)(uintptr_t)val;
            o->op_moresib = (sel >> 16) & 1;
            return 0;
        case GOPERL_OPF_FIRST: cUNOPx(o)->op_first = (OP *)(uintptr_t)val; return 0;
        case GOPERL_OPF_LAST: cBINOPx(o)->op_last = (OP *)(uintptr_t)val; return 0;
        case GOPERL_OPF_OTHER: cLOGOPx(o)->op_other = (OP *)(uintptr_t)val; return 0;
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
            lex_read_to((char *)(uintptr_t)(uint32_t)b);
            return 0;
        case GOPERL_LEX_BUFUTF8: return lex_bufutf8() ? 1 : 0;
        case GOPERL_LEX_STUFF_PVN: /* b = flags<<32|len */
            lex_stuff_pvn(s ? s : "", (STRLEN)(uint32_t)b, (U32)(b >> 32));
            return 0;
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
        case 100 + GOPERL_PARSERW_IN_MY_STASH:
            PL_parser->in_my_stash = (HV *)(uintptr_t)(uint32_t)b;
            return 0;
        case GOPERL_PARSER_BUFPTR:
            PL_parser->bufptr = (char *)(uintptr_t)(uint32_t)b;
            return 0;
        case GOPERL_PARSER_ERROR_COUNT:
            PL_parser->error_count = (I32)(int32_t)(uint32_t)b;
            return 0;
        case GOPERL_PARSER_IN_MY:
            PL_parser->in_my = (U16)b;
            return 0;
        }
        return 0;
    }
    case GOPERL_OP_BLOCK: { /* a: 0 = block_start(b=full) -> floor,
                 * 1 = block_end(b=floor, s=[u64 seq]) -> op token */
        if ((int)a == 0) return (uint64_t)(uint32_t)block_start((int)b);
        uint64_t k1 = 0;
        if (s) memcpy(&k1, s, 8);
        return (uint64_t)(uintptr_t)block_end((I32)(uint32_t)b,
                                              (OP *)(uintptr_t)(uint32_t)k1);
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
        case GOPERL_PAD_SETSV: { /* b = sv token<<32 | pad index */
            PADOFFSET ix = (PADOFFSET)(uint32_t)b;
            SV *sv = (SV *)(uintptr_t)(uint32_t)(b >> 32);
            PAD_SETSV(ix, sv);
            return 0;
        }
        case GOPERL_PAD_SV_FETCH: /* b = pad index (the RUNNING sub's pad) */
            return (uint64_t)(uintptr_t)PAD_SV((PADOFFSET)b);
        case GOPERL_PAD_CV_PADLIST: {
            CV *cv = (CV *)(uintptr_t)(uint32_t)b;
            if (!cv || (SvTYPE((SV *)cv) != SVt_PVCV && SvTYPE((SV *)cv) != SVt_PVFM) ||
                CvISXSUB(cv))
                return 0;
            return (uint64_t)(uintptr_t)CvPADLIST(cv);
        }
        case GOPERL_PAD_PADLIST: {
            static std::vector<uint32_t> words;
            PADLIST *pl = (PADLIST *)(uintptr_t)(uint32_t)b;
            if (!pl) return 0;
            words.clear();
            words.push_back((uint32_t)(int32_t)PadlistMAX(pl));
            words.push_back((uint32_t)(uintptr_t)PadlistNAMES(pl));
            for (SSize_t i = 0; i <= PadlistMAX(pl); i++)
                words.push_back((uint32_t)(uintptr_t)(SV *)PadlistARRAY(pl)[i]);
            return (uint64_t)(uintptr_t)words.data();
        }
        case GOPERL_PAD_NAMELIST: {
            static std::vector<uint32_t> words;
            PADNAMELIST *pnl = (PADNAMELIST *)(uintptr_t)(uint32_t)b;
            if (!pnl) return 0;
            words.clear();
            words.push_back((uint32_t)(int32_t)PadnamelistMAX(pnl));
            words.push_back((uint32_t)(int32_t)PadnamelistMAXNAMED(pnl));
            for (SSize_t i = 0; i <= PadnamelistMAX(pnl); i++)
                words.push_back((uint32_t)(uintptr_t)PadnamelistARRAY(pnl)[i]);
            return (uint64_t)(uintptr_t)words.data();
        }
        case GOPERL_PAD_NAME: {
            static uint32_t w[8];
            PADNAME *pn = (PADNAME *)(uintptr_t)(uint32_t)b;
            if (!pn) return 0;
            w[0] = (uint32_t)(uintptr_t)PadnamePV(pn);
            w[1] = (uint32_t)PadnameLEN(pn);
            w[2] = (uint32_t)PadnameFLAGS(pn);
            w[3] = (uint32_t)PadnameREFCNT(pn);
            w[4] = (uint32_t)COP_SEQ_RANGE_LOW(pn);
            w[5] = (uint32_t)COP_SEQ_RANGE_HIGH(pn);
            w[6] = (uint32_t)(uintptr_t)(SV *)PadnameOURSTASH(pn);
            w[7] = (uint32_t)(uintptr_t)(SV *)PadnameTYPE(pn);
            return (uint64_t)(uintptr_t)w;
        }
        case GOPERL_PAD_SAVE_CLEARSV:
            save_clearsv(&PAD_SVl((PADOFFSET)b));
            return 0;
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
        }
        return 0;
    }
    case GOPERL_OP_SAVE_MISC: { /* a = a GOPERL_SAVE_ selector; b = sv token
                 * (save_gp: empty<<32 | gv) */
        SV *sv = (SV *)(uintptr_t)(uint32_t)b;
        switch ((int)a) {
        case GOPERL_SAVE_FREESV: save_freesv(sv); return 0;
        case GOPERL_SAVE_MORTALIZESV: save_mortalizesv(sv); return 0;
        case GOPERL_SAVE_ITEM: save_item(sv); return 0;
        case GOPERL_SAVE_ARY: return (uint64_t)(uintptr_t)(SV *)save_ary((GV *)sv);
        case GOPERL_SAVE_HASH: return (uint64_t)(uintptr_t)(SV *)save_hash((GV *)sv);
        case GOPERL_SAVE_GP: save_gp((GV *)sv, (I32)(b >> 32)); return 0;
        case GOPERL_SAVE_HINTS: SAVEHINTS(); return 0;
        case GOPERL_SAVE_COMPILE_WARNINGS: SAVECOMPILEWARNINGS(); return 0;
        case GOPERL_SAVE_DEFSV: SAVE_DEFSV; return 0;
        }
        Perl_croak(aTHX_ "goperl: unknown save call %d", (int)a);
    }
    case GOPERL_OP_SV_CLASSIFY: { /* a = sv token; b = len<<32|flags<<8|mode;
                 * s = name bytes (mode 0) or an 8-byte sv token */
        SV *sv = (SV *)(uintptr_t)a;
        int mode = (int)(b & 0xFF);
        U32 flags = (U32)((b >> 8) & 0xFFFFFF);
        STRLEN len = (STRLEN)(uint32_t)(b >> 32);
        uint64_t k1 = 0;
        if (mode == 1 || mode == 2 || mode == 5 || mode == 6) {
            if (s) memcpy(&k1, s, 8);
        }
        SV *other = (SV *)(uintptr_t)(uint32_t)k1;
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
        SV *sv = (SV *)(uintptr_t)a;
        U32 clear = (U32)(b >> 32), set = (U32)b;
        if (clear || set) SvFLAGS(sv) = (SvFLAGS(sv) & ~clear) | set;
        return (uint64_t)SvFLAGS(sv);
    }
    case GOPERL_OP_SV_SLOT_SET: { /* a = a GOPERL_SVSLOT_ selector<<32 | sv;
                 * b = the value: the SvXXX_set macros of sv.h */
        SV *sv = (SV *)(uintptr_t)(uint32_t)a;
        switch ((int)(a >> 32)) {
        case GOPERL_SVSLOT_RV: SvRV_set(sv, (SV *)(uintptr_t)(uint32_t)b); return 0;
        case GOPERL_SVSLOT_STASH: SvSTASH_set(sv, (HV *)(uintptr_t)(uint32_t)b); return 0;
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
        case GOPERL_SVSLOT_PV: SvPV_set(sv, (char *)(uintptr_t)(uint32_t)b); return 0;
        case GOPERL_SVSLOT_LEN: SvLEN_set(sv, (STRLEN)b); return 0;
        case GOPERL_SVSLOT_GV_STASH:
            if (isGV_with_GP(sv)) GvSTASH((GV *)sv) = (HV *)(uintptr_t)(uint32_t)b;
            return 0;
        case GOPERL_SVSLOT_GV_CVGEN:
            if (isGV_with_GP(sv)) GvCVGEN((GV *)sv) = (U32)b;
            return 0;
        }
        Perl_croak(aTHX_ "goperl: unknown sv slot %d", (int)(a >> 32));
    }
    case GOPERL_OP_SV_SLOT_GET: { /* a = a GOPERL_SVSLOT_ selector<<32 | sv
                 * -> what the body slot holds, as the SvXXX_set op takes
                 * it. A body too small to have the slot reads as zero. */
        SV *sv = (SV *)(uintptr_t)(uint32_t)a;
        const svtype t = SvTYPE(sv);
        switch ((int)(a >> 32)) {
        case GOPERL_SVSLOT_RV:
            return SvROK(sv) ? (uint64_t)(uintptr_t)SvRV(sv) : 0;
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
        Perl_croak(aTHX_ "goperl: unknown sv slot %d", (int)(a >> 32));
    }
    case GOPERL_OP_CALLER_CX: { /* a = level, b bit 0: the frame the debugger
                 * hides -> (stackinfo token << 32) | (context index + 1),
                 * or 0 when there is no such frame */
        const PERL_CONTEXT *dbcx = NULL;
        const PERL_CONTEXT *cx = caller_cx((I32)(int64_t)a, &dbcx);
        if (b & 1) cx = dbcx;
        if (!cx) return 0;
        for (PERL_SI *si = PL_curstackinfo; si; si = si->si_prev) {
            if (cx >= si->si_cxstack && cx <= si->si_cxstack + si->si_cxix)
                return ((uint64_t)(uintptr_t)si << 32) |
                       (uint32_t)(cx - si->si_cxstack + 1);
        }
        return 0;
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
    case GOPERL_OP_DO_OPENN: { /* a = gv; s = five u32 (as_raw, rawmode,
                 * rawperm, supplied handle, argument count), the argument
                 * tokens, then the mode text; b = the length of all of it */
        uint32_t head[5];
        if (!s || b < sizeof head) Perl_croak(aTHX_ "goperl: do_openn: short request");
        memcpy(head, s, sizeof head);
        const size_t num = head[4];
        if (b < sizeof head + num * 4) Perl_croak(aTHX_ "goperl: do_openn: short request");
        SV **svs = NULL;
        if (num) {
            Newx(svs, num, SV *);
            SAVEFREEPV(svs);
            for (size_t i = 0; i < num; i++) {
                uint32_t tok;
                memcpy(&tok, s + sizeof head + i * 4, 4);
                svs[i] = (SV *)(uintptr_t)tok;
            }
        }
        const char *mode = s + sizeof head + num * 4;
        const size_t mode_len = (size_t)b - sizeof head - num * 4;
        return do_openn((GV *)(uintptr_t)a, mode, (I32)mode_len, (int)head[0],
                        (int)head[1], (int)head[2],
                        (PerlIO *)(uintptr_t)head[3], svs, (I32)num)
                   ? 1
                   : 0;
    }
    case GOPERL_OP_PERLIO: { /* a = a GOPERL_PIO_ selector<<32 | handle (or
                 * IO sv); b and s = the remaining arguments */
        PerlIO *f = (PerlIO *)(uintptr_t)(uint32_t)a;
        const int fn = (int)(a >> 32);
        switch (fn) {
        case GOPERL_PIO_STDIN: return (uint64_t)(uintptr_t)PerlIO_stdin();
        case GOPERL_PIO_STDOUT: return (uint64_t)(uintptr_t)PerlIO_stdout();
        case GOPERL_PIO_STDERR: return (uint64_t)(uintptr_t)PerlIO_stderr();
        case GOPERL_PIO_FDOPEN: /* b = descriptor, s = mode */
            return (uint64_t)(uintptr_t)PerlIO_fdopen((int)(int64_t)b, s ? s : "r");
        case GOPERL_PIO_IFP: {
            IO *io = (IO *)(uintptr_t)(uint32_t)a;
            if (!io || SvTYPE((SV *)io) != SVt_PVIO) return 0;
            return (uint64_t)(uintptr_t)IoIFP(io);
        }
        case GOPERL_PIO_OFP: {
            IO *io = (IO *)(uintptr_t)(uint32_t)a;
            if (!io || SvTYPE((SV *)io) != SVt_PVIO) return 0;
            return (uint64_t)(uintptr_t)IoOFP(io);
        }
        case GOPERL_PIO_VALID: return PerlIOValid(f) ? 1 : 0;
        }
        /* Everything below works on an open handle; on anything else it
         * fails the way the function does on a closed one. */
        if (!PerlIOValid(f)) {
            SETERRNO(EBADF, SS_IVCHAN);
            return fn == GOPERL_PIO_EOF ? 1 : (uint64_t)(int64_t)-1;
        }
        switch (fn) {
        case GOPERL_PIO_FILENO: return (uint64_t)(int64_t)PerlIO_fileno(f);
        case GOPERL_PIO_READ: { /* b = count -> buffer<<32 | count read (as
                 * a signed 32-bit value; negative on error). The bytes
                 * stay valid until the next read operation. */
            static char buf[65536];
            Size_t want = (Size_t)b;
            if (want > sizeof buf) want = sizeof buf;
            const SSize_t n = PerlIO_read(f, buf, want);
            return ((uint64_t)(uint32_t)(uintptr_t)buf << 32) | (uint32_t)(int32_t)n;
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
        case GOPERL_PIO_SET_PTRCNT: /* b = ptr<<32 | count */
            PerlIO_set_ptrcnt(f, (STDCHAR *)(uintptr_t)(uint32_t)(b >> 32),
                              (SSize_t)(int32_t)(uint32_t)b);
            return 0;
        case GOPERL_PIO_FILL: return (uint64_t)(int64_t)PerlIO_fill(f);
        }
        Perl_croak(aTHX_ "goperl: unknown PerlIO call %d", fn);
    }
    case GOPERL_OP_SV_CALL: { /* a = a GOPERL_SVCALL_ selector<<32 | sv;
                 * b = the remaining argument of that API function */
        SV *sv = (SV *)(uintptr_t)(uint32_t)a;
        SV *other = (SV *)(uintptr_t)(uint32_t)b;
        U32 hi = (U32)(b >> 32);
        switch ((int)(a >> 32)) {
        case GOPERL_SVCALL_MG_GET: return (uint64_t)(int64_t)mg_get(sv);
        case GOPERL_SVCALL_MG_CLEAR: return (uint64_t)(int64_t)mg_clear(sv);
        case GOPERL_SVCALL_MG_FREE: return (uint64_t)(int64_t)mg_free(sv);
        case GOPERL_SVCALL_MG_MAGICAL: mg_magical(sv); return 0;
        case GOPERL_SVCALL_MG_SIZE: return (uint64_t)mg_size(sv);
        case GOPERL_SVCALL_BACKOFF: sv_backoff(sv); return 0;
        case GOPERL_SVCALL_UNREF: sv_unref_flags(sv, (U32)b); return 0;
        case GOPERL_SVCALL_RVUNWEAKEN: sv_rvunweaken(sv); return 0;
        case GOPERL_SVCALL_UTF8_ENCODE: sv_utf8_encode(sv); return 0;
        case GOPERL_SVCALL_LEN_UTF8: return (uint64_t)sv_len_utf8(sv);
        case GOPERL_SVCALL_DEC: sv_dec(sv); return 0;
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
        case GOPERL_SVCALL_AV_DELETE: /* b = flags<<32 | index (two's complement) */
            return (uint64_t)(uintptr_t)av_delete((AV *)sv, (SSize_t)(int32_t)(uint32_t)b, (I32)hi);
        case GOPERL_SVCALL_VOK: return SvVOK(sv) ? 1 : 0;
        case GOPERL_SVCALL_HV_USEDKEYS: return (uint64_t)HvUSEDKEYS((HV *)sv);
        case GOPERL_SVCALL_HV_TOTALKEYS: return (uint64_t)HvTOTALKEYS((HV *)sv);
        case GOPERL_SVCALL_HV_FILL: return (uint64_t)HvFILL((HV *)sv);
        case GOPERL_SVCALL_HV_MAX: return (uint64_t)HvMAX((HV *)sv);
        case GOPERL_SVCALL_AV_MAX: return (uint64_t)(int64_t)AvMAX((AV *)sv);
        case GOPERL_SVCALL_AV_BODY: {
            if (SvTYPE(sv) != SVt_PVAV) return 0;
            AV *const av = (AV *)sv;
            const SSize_t n = AvFILLp(av) + 1;
            return ((uint64_t)(uint32_t)(uintptr_t)AvARRAY(av) << 32) |
                   (uint32_t)(n > 0 ? n : 0);
        }
        case GOPERL_SVCALL_UTF8_UPGRADE:
            return (uint64_t)sv_utf8_upgrade_flags(sv, (I32)b);
        case GOPERL_SVCALL_NEWSVSV: /* sv may be NULL, as for newSVsv */
            return (uint64_t)(uintptr_t)newSVsv_flags(sv, (I32)b);
        case GOPERL_SVCALL_NEWAVAV: return (uint64_t)(uintptr_t)(SV *)newAVav((AV *)sv);
        case GOPERL_SVCALL_NEWAVHV: return (uint64_t)(uintptr_t)(SV *)newAVhv((HV *)sv);
        case GOPERL_SVCALL_SETRV_INC: sv_setrv_inc(sv, other); return 0;
        case GOPERL_SVCALL_SETRV_NOINC: sv_setrv_noinc(sv, other); return 0;
        case GOPERL_SVCALL_SETSV: /* b = flags<<32 | source */
            sv_setsv_flags(sv, other, (I32)hi);
            return 0;
        case GOPERL_SVCALL_CATSV:
            sv_catsv_flags(sv, other, (I32)hi);
            return 0;
        case GOPERL_SVCALL_CMP:
            return (uint64_t)(int64_t)sv_cmp_flags(sv, other, hi);
        case GOPERL_SVCALL_CMP_LOCALE:
            return (uint64_t)(int64_t)sv_cmp_locale_flags(sv, other, hi);
        case GOPERL_SVCALL_EQ: return sv_eq_flags(sv, other, hi) ? 1 : 0;
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
        case GOPERL_SVCALL_2PV: { /* -> (linear-memory ptr << 32) | len */
            STRLEN len = 0;
            const char *p = SvPV_flags(sv, len, (I32)b);
            return ((uint64_t)(uint32_t)(uintptr_t)p << 32) | (uint32_t)len;
        }
        case GOPERL_SVCALL_2PVBYTE: { /* -> (linear-memory ptr << 32) | len;
                 * dies with "Wide character" when the string has no byte form */
            STRLEN len = 0;
            const char *p = (b & SV_GMAGIC) ? SvPVbyte(sv, len) : SvPVbyte_nomg(sv, len);
            return ((uint64_t)(uint32_t)(uintptr_t)p << 32) | (uint32_t)len;
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
            return ((uint64_t)(uint32_t)(uintptr_t)p << 32) | (uint32_t)len;
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
            return ((uint64_t)(uint32_t)(uintptr_t)p << 32) | (uint32_t)len;
        }
        case GOPERL_SVCALL_UNI_DISPLAY: { /* sv = dsv; b = (pvlim<<8 | flags)<<32
                 * | ssv -> (linear-memory ptr << 32) | len */
            const char *p = sv_uni_display(sv, other, (STRLEN)(hi >> 8), (UV)(hi & 0xFF));
            return ((uint64_t)(uint32_t)(uintptr_t)p << 32) | (uint32_t)SvCUR(sv);
        }
        case GOPERL_SVCALL_HV_KSPLIT: hv_ksplit((HV *)sv, (IV)(int64_t)b); return 0;
        case GOPERL_SVCALL_TAINT_ENV: taint_env(); return 0;
        case GOPERL_SVCALL_TIED: {
            MAGIC *const mg = SvTIED_mg(sv, (int)(int64_t)b);
            if (!mg) return 0;
            return ((uint64_t)1 << 32) | (uint32_t)(uintptr_t)SvTIED_obj(sv, mg);
        }
        case GOPERL_SVCALL_MAGIC_IS_VALUE:
            return PERL_MAGIC_TYPE_IS_VALUE_MAGIC((uint32_t)a) ? 1 : 0;
        case GOPERL_SVCALL_AMAGIC:
            return (uint64_t)(uintptr_t)amagic_call(sv, other, (int)(hi & 0xff), (int)(hi >> 8));
        case GOPERL_SVCALL_CLEAR_UNDEF: /* SvOK_off with its body release */
            SvOK_off(sv);
            return 0;
        case GOPERL_SVCALL_IO_TYPE: return (uint64_t)(U8)IoTYPE((IO *)sv);
        case GOPERL_SVCALL_IO_FLAGS: return (uint64_t)IoFLAGS((IO *)sv);
        case GOPERL_SVCALL_IO_LINES: return (uint64_t)(int64_t)IoLINES((IO *)sv);
        case GOPERL_SVCALL_THROW: /* the exception the running native XSUB
                 * ends with; a copy, as die takes one */
            SvREFCNT_dec(goperl_thrown_sv);
            goperl_thrown_sv = newSVsv(sv);
            return 0;
        case GOPERL_SVCALL_SETPV_BUFSIZE: /* b = cur<<32 | buffer size -> PVX */
            return (uint64_t)(uint32_t)(uintptr_t)sv_setpv_bufsize(sv, (STRLEN)hi, (STRLEN)(uint32_t)b);
        }
        Perl_croak(aTHX_ "goperl: unknown sv call %d", (int)(a >> 32));
    }
    case GOPERL_OP_GV_CALL: { /* a = a GOPERL_GVCALL_ selector<<32 | gv or cv;
                 * b = the remaining argument */
        SV *sv = (SV *)(uintptr_t)(uint32_t)a;
        GV *gv = (GV *)sv;
        CV *cv = (CV *)sv;
        SV *other = (SV *)(uintptr_t)(uint32_t)b;
        int fn = (int)(a >> 32);
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
        case GOPERL_GVCALL_CV_SET: GvCV_set(gv, (CV *)other); return 0;
        case GOPERL_GVCALL_FLAGS: { /* b = clear<<32 | set */
            U32 clear = (U32)(b >> 32), set = (U32)b;
            if (clear || set) GvFLAGS(gv) = (GvFLAGS(gv) & ~clear) | set;
            return (uint64_t)GvFLAGS(gv);
        }
        case GOPERL_GVCALL_CVGEN: return (uint64_t)GvCVGEN(gv);
        case GOPERL_GVCALL_CVGEN_SET: GvCVGEN(gv) = (U32)b; return 0;
        case GOPERL_GVCALL_STASH_SET:
            GvSTASH(gv) = (HV *)(uintptr_t)(uint32_t)b;
            return 0;
        case GOPERL_GVCALL_FORM: return (uint64_t)(uintptr_t)(SV *)GvFORM(gv);
        case GOPERL_GVCALL_FULLNAME: /* b = (keepmain<<1 | effective)<<32 | sv */
            if ((b >> 32) & 1)
                gv_efullname4(other, gv, NULL, cBOOL((b >> 33) & 1));
            else
                gv_fullname4(other, gv, NULL, cBOOL((b >> 33) & 1));
            return 0;
        case GOPERL_GVCALL_GP: return (uint64_t)(uintptr_t)GvGP(gv);
        case GOPERL_GVCALL_FILE: {
            const char *file = GvFILE(gv);
            if (!file) return 0;
            return ((uint64_t)(uint32_t)(uintptr_t)file << 32) | (uint32_t)strlen(file);
        }
        case GOPERL_GVCALL_LINE: return (uint64_t)GvLINE(gv);
        case GOPERL_GVCALL_NAME_UTF8: return GvNAMEUTF8(gv) ? 1 : 0;
        case GOPERL_GVCALL_SV_SET: GvSV(gv) = other; return 0;
        case GOPERL_GVCALL_AV_SET: GvAV(gv) = (AV *)other; return 0;
        case GOPERL_GVCALL_HV_SET: GvHV(gv) = (HV *)other; return 0;
        case GOPERL_GVCALL_CV_GV_SET: CvGV_set(cv, (GV *)other); return 0;
        case GOPERL_GVCALL_CV_STASH_SET: CvSTASH_set(cv, (HV *)other); return 0;
        case GOPERL_GVCALL_CV_FLAGS: {
            U32 clear = (U32)(b >> 32), set = (U32)b;
            if (clear || set) CvFLAGS(cv) = (CvFLAGS(cv) & ~clear) | set;
            return (uint64_t)CvFLAGS(cv);
        }
        case GOPERL_GVCALL_CV_OUTSIDE: return (uint64_t)(uintptr_t)(SV *)CvOUTSIDE(cv);
        case GOPERL_GVCALL_CV_PROTO: {
            if (!SvPOK(sv)) return 0;
            return ((uint64_t)(uint32_t)(uintptr_t)CvPROTO(cv) << 32) |
                   (uint32_t)CvPROTOLEN(cv);
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
    case GOPERL_OP_SV_STR: { /* a = a GOPERL_SVSTR_ selector<<32 | sv;
                 * b = flags<<32 | byte length of s */
        SV *sv = (SV *)(uintptr_t)(uint32_t)a;
        STRLEN len = (STRLEN)(uint32_t)b;
        U32 flags = (U32)(b >> 32);
        const char *str = s ? s : "";
        switch ((int)(a >> 32)) {
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
        case GOPERL_SVSTR_THROW: /* the message the running native XSUB
                 * dies with (see GOPERL_SVCALL_THROW) */
            SvREFCNT_dec(goperl_thrown_sv);
            goperl_thrown_sv = newSVpvn_flags(str, len, (flags & 1) ? SVf_UTF8 : 0);
            return 0;
        case GOPERL_SVSTR_GV_NAME_SET:
            gv_name_set((GV *)sv, str, (U32)len, flags);
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
        Perl_croak(aTHX_ "goperl: unknown sv string call %d", (int)(a >> 32));
    }
    case GOPERL_OP_FIELD: { /* a = a GOPERL_FIELD_ selector<<32 | token;
                 * b = the value of a store */
        SV *sv = (SV *)(uintptr_t)(uint32_t)a;
        switch ((int)(a >> 32)) {
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
                HvEITER_set((HV *)sv, (HE *)(uintptr_t)(uint32_t)b);
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
            if (SvTYPE(sv) == SVt_PVLV) LvTARG(sv) = (SV *)(uintptr_t)(uint32_t)b;
            return 0;
        case GOPERL_FIELD_SHARED_HASH:
            return SvIsCOW_shared_hash(sv) ? (uint64_t)SvSHARED_HASH(sv) : 0;
        case GOPERL_FIELD_OP_SV: { /* cSVOPx_sv: the SV of an SVOP or METHOP */
            OP *o = (OP *)(uintptr_t)(uint32_t)a;
            const U32 c = PL_opargs[o->op_type] & OA_CLASS_MASK;
            if (c == OA_METHOP)
                return (o->op_flags & OPf_KIDS) ? 0 : (uint64_t)(uintptr_t)cMETHOPx_meth(o);
            if (c == OA_SVOP || c == OA_PVOP_OR_SVOP || c == OA_FILESTATOP ||
                o->op_type == OP_CUSTOM)
                return (uint64_t)(uintptr_t)cSVOPx_sv(o);
            return 0;
        }
        case GOPERL_FIELD_OP_GV: { /* cGVOPx_gv */
            OP *o = (OP *)(uintptr_t)(uint32_t)a;
            const U32 c = PL_opargs[o->op_type] & OA_CLASS_MASK;
            if (c == OA_SVOP || c == OA_PADOP || c == OA_PVOP_OR_SVOP || c == OA_FILESTATOP)
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
                CvOUTSIDE((CV *)sv) = (CV *)(uintptr_t)(uint32_t)b;
            return 0;
        }
        Perl_croak(aTHX_ "goperl: unknown field %d", (int)(a >> 32));
    }
    case GOPERL_OP_REGEXP: { /* a = a GOPERL_RX_ selector<<32 | token */
        SV *sv = (SV *)(uintptr_t)(uint32_t)a;
        switch ((int)(a >> 32)) {
        case GOPERL_RX_OF_SV: return (uint64_t)(uintptr_t)(SV *)SvRX(sv);
        case GOPERL_RX_COMP: return (uint64_t)(uintptr_t)(SV *)pregcomp(sv, (U32)b);
        case GOPERL_RX_COMP_PV: { /* the pattern as bytes; flags bit 31: UTF-8 */
            const U32 flags = (U32)(b >> 32);
            SV *pat = newSVpvn_flags(s ? s : "", (STRLEN)(uint32_t)b,
                                     SVs_TEMP | ((flags & 0x80000000u) ? SVf_UTF8 : 0));
            return (uint64_t)(uintptr_t)(SV *)pregcomp(pat, flags & 0x7FFFFFFFu);
        }
        case GOPERL_RX_EXEC: { /* s = [stringarg][strend][strbeg][minend]
                 * [screamer][nosave], addresses of this memory */
            uint32_t w[6];
            if (!s || b < sizeof w) Perl_croak(aTHX_ "goperl: short pregexec request");
            memcpy(w, s, sizeof w);
            return (uint64_t)(int64_t)pregexec(
                (REGEXP *)sv, (char *)(uintptr_t)w[0], (char *)(uintptr_t)w[1],
                (char *)(uintptr_t)w[2], (SSize_t)(int32_t)w[3],
                (SV *)(uintptr_t)w[4], (U32)w[5]);
        }
        case GOPERL_RX_BODY: { /* -> the address of the words */
            static uint32_t body[20];
            REGEXP *rx = (REGEXP *)sv;
            if (!rx || !isREGEXP(sv)) return 0;
            regexp *r = ReANY(rx);
            body[0] = (uint32_t)(uintptr_t)r->engine;
            body[1] = (uint32_t)(uintptr_t)(SV *)r->mother_re;
            body[2] = (uint32_t)(uintptr_t)(SV *)r->paren_names;
            body[3] = r->extflags;
            body[4] = r->nparens;
            body[5] = (uint32_t)(int32_t)r->minlen;
            body[6] = (uint32_t)(int32_t)r->minlenret;
            body[7] = (uint32_t)r->gofs;
            body[8] = r->intflags;
            body[9] = r->lastparen;
            body[10] = r->lastcloseparen;
            body[11] = (uint32_t)(uintptr_t)r->subbeg;
            body[12] = (uint32_t)(int32_t)r->sublen;
            body[13] = (uint32_t)(int32_t)r->suboffset;
            body[14] = (uint32_t)(int32_t)r->subcoffset;
            body[15] = (uint32_t)r->compflags;
            body[16] = (uint32_t)(uintptr_t)RX_PRECOMP(rx);
            body[17] = (uint32_t)RX_PRELEN(rx);
            body[18] = (uint32_t)(uintptr_t)RX_WRAPPED(rx);
            body[19] = (uint32_t)RX_WRAPLEN(rx);
            return (uint64_t)(uintptr_t)body;
        }
        case GOPERL_RX_OFFS: {
            REGEXP *rx = (REGEXP *)sv;
            if (!rx || !isREGEXP(sv)) return 0;
            regexp *r = ReANY(rx);
            if ((U32)b > r->nparens || !r->offs) return ((uint64_t)0xFFFFFFFFu << 32) | 0xFFFFFFFFu;
            return ((uint64_t)(uint32_t)(int32_t)r->offs[(U32)b].start << 32) |
                   (uint32_t)(int32_t)r->offs[(U32)b].end;
        }
        }
        Perl_croak(aTHX_ "goperl: unknown regexp call %d", (int)(a >> 32));
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
                 * the interpreter's when none did */
            const uint32_t type = (uint32_t)(b >> 32);
            OP *o = (OP *)(uintptr_t)(uint32_t)b;
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
        case GOPERL_HOOK_PEEP_ORIG: {
            const int which = (int)((b >> 32) & 1);
            OP *o = (OP *)(uintptr_t)(uint32_t)b;
            const peep_t tramp = which ? goperl_rpeep_tramp : goperl_peep_tramp;
            peep_t fn = (which ? PL_rpeepp : PL_peepp) == tramp ? g_peep_orig[which]
                                                                 : (which ? PL_rpeepp : PL_peepp);
            if (fn) fn(aTHX_ o);
            return 0;
        }
        case GOPERL_HOOK_ATEXIT:
            call_atexit(goperl_atexit_thunk, (void *)(uintptr_t)(uint32_t)b);
            return 0;
        case GOPERL_HOOK_UNWIND: {
            goperl_native_marks_settle(aTHX);
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
        case GOPERL_PPSTACK_PULL: {
            static std::vector<uint32_t> state;
            state.clear();
            goperl_pp_stack_words(aTHX_ state);
            return ((uint64_t)(uint32_t)(uintptr_t)state.data() << 32) |
                   (uint32_t)state.size();
        }
        case GOPERL_PPSTACK_PUSH:
            if (!s || !goperl_pp_stack_apply(aTHX_ (const unsigned char *)s, (size_t)b))
                Perl_croak(aTHX_ "goperl: malformed stack state");
            return 0;
        }
        Perl_croak(aTHX_ "goperl: unknown stack call %d", (int)a);
    }
    case GOPERL_OP_OP_DUMP: { /* a = op token; b = cap<<8 | mode ->
                 * address<<32 | number of records */
        static std::vector<uint32_t> records;
        OP *o = (OP *)(uintptr_t)(uint32_t)a;
        records.clear();
        if ((int)(b & 0x7F) == GOPERL_OPDUMP_FREED) {
            records.swap(g_ops_freed);
            g_ops_freed_count = 0;
            return ((uint64_t)(uint32_t)(uintptr_t)records.data() << 32) |
                   (uint32_t)records.size();
        }
        if (!o) return 0;
        goperl_opfreehook_install(aTHX);
        const size_t cap = (size_t)(b >> 8);
        goperl_op_dump(aTHX_ o, (int)(b & 0x7F), cBOOL(b & 0x80), cap ? cap : 1, records);
        for (size_t i = 0; i < records.size(); i += GOPERL_OPREC_WORDS)
            g_ops_described.insert((OP *)(uintptr_t)records[i]);
        return ((uint64_t)(uint32_t)(uintptr_t)records.data() << 32) |
               (uint32_t)(records.size() / GOPERL_OPREC_WORDS);
    }
    case GOPERL_OP_CALL: { /* a = a GOPERL_CALL_ selector<<32 | token */
        SV *sv = (SV *)(uintptr_t)(uint32_t)a;
        switch ((int)(a >> 32)) {
        case GOPERL_CALL_PV_DISPLAY:
        case GOPERL_CALL_PV_ESCAPE:
        case GOPERL_CALL_PV_PRETTY: { /* s = [count][max][flags][len of the
                 * buffer for pv_display][n1][n2], the start and end colour
                 * strings of n1 and n2 bytes, then the text; b = all of it.
                 * -> the address of the result in the SV, and above it how
                 * many bytes of the text pv_escape took */
            uint32_t w[6];
            if (!s || b < sizeof w) Perl_croak(aTHX_ "goperl: short display request");
            memcpy(w, s, sizeof w);
            if ((uint64_t)sizeof w + w[4] + w[5] + w[0] > b)
                Perl_croak(aTHX_ "goperl: short display request");
            const char *c1 = s + sizeof w, *c2 = c1 + w[4], *text = c2 + w[5];
            switch ((int)(a >> 32)) {
            case GOPERL_CALL_PV_DISPLAY:
                return (uint64_t)(uintptr_t)pv_display(sv, text, w[0], w[3], w[1]);
            case GOPERL_CALL_PV_ESCAPE: {
                STRLEN escaped = 0;
                char *r = pv_escape(sv, text, w[0], w[1], &escaped, w[2]);
                return ((uint64_t)(uint32_t)escaped << 32) | (uint32_t)(uintptr_t)r;
            }
            default: {
                /* the colour strings as C strings, as the function takes them */
                const std::string start(c1, w[4]), end(c2, w[5]);
                char *r = pv_pretty(sv, text, w[0], w[1], w[4] ? start.c_str() : NULL,
                                    w[5] ? end.c_str() : NULL, w[2]);
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
                aTHX_ (char *)(uintptr_t)(uint32_t)a, s ? s : "", (STRLEN)b);
        }
        case GOPERL_CALL_GETCWD_SV: return getcwd_sv(sv) ? 1 : 0;
        case GOPERL_CALL_MESS_SV: /* b = consume */
            return (uint64_t)(uintptr_t)mess_sv(sv, cBOOL(b));
        case GOPERL_CALL_MG_FIND: { /* the interpreter's own magic of a type
                 * (what the host attached is the host's to find): [type]
                 * [flags][len][obj][ptr][private] */
            static uint32_t w[6];
            if (SvTYPE(sv) < SVt_PVMG) return 0;
            for (MAGIC *mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic) {
                if (mg->mg_type != (char)(int)b) continue;
                if (goperl_is_anchor(mg)) continue;
                w[0] = (uint32_t)(unsigned char)mg->mg_type;
                w[1] = mg->mg_flags;
                w[2] = (uint32_t)(int32_t)mg->mg_len;
                w[3] = (uint32_t)(uintptr_t)mg->mg_obj;
                w[4] = (uint32_t)(uintptr_t)mg->mg_ptr;
                w[5] = mg->mg_private;
                return (uint64_t)(uintptr_t)w;
            }
            return 0;
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
            COP *cop = (COP *)(uintptr_t)(uint32_t)a;
            if (b)
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
        case GOPERL_CALL_SCAN_VERSION: {
            const char *const text = (uint32_t)b ? (const char *)(uintptr_t)(uint32_t)b : (s ? s : "");
            const char *const end = scan_version(text, sv, cBOOL(b >> 32));
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
        case GOPERL_CALL_PARSER: {
            static uint32_t w[GOPERL_PARSERW_WORDS];
            yy_parser *ps = PL_parser;
            if (!ps) return 0;
            w[GOPERL_PARSERW_LINESTR] = (uint32_t)(uintptr_t)ps->linestr;
            w[GOPERL_PARSERW_PV] = ps->linestr ? (uint32_t)(uintptr_t)SvPVX(ps->linestr) : 0;
            w[GOPERL_PARSERW_BUFPTR] = (uint32_t)(uintptr_t)ps->bufptr;
            w[GOPERL_PARSERW_BUFEND] = (uint32_t)(uintptr_t)ps->bufend;
            w[GOPERL_PARSERW_OLDBUFPTR] = (uint32_t)(uintptr_t)ps->oldbufptr;
            w[GOPERL_PARSERW_OLDOLDBUFPTR] = (uint32_t)(uintptr_t)ps->oldoldbufptr;
            w[GOPERL_PARSERW_LINESTART] = (uint32_t)(uintptr_t)ps->linestart;
            w[GOPERL_PARSERW_ERROR_COUNT] = (uint32_t)ps->error_count;
            w[GOPERL_PARSERW_IN_MY] = ps->in_my;
            w[GOPERL_PARSERW_LEX_FLAGS] = ps->lex_flags;
            w[GOPERL_PARSERW_PREAMBLING] = (uint32_t)ps->preambling;
            w[GOPERL_PARSERW_EXPECT] = ps->expect;
            w[GOPERL_PARSERW_LEX_STATE] = ps->lex_state;
            w[GOPERL_PARSERW_LEX_BRACKETS] = (uint32_t)ps->lex_brackets;
            w[GOPERL_PARSERW_LEX_INWHAT] = ps->lex_inwhat;
            w[GOPERL_PARSERW_MULTI_START] = (uint32_t)ps->multi_start;
            w[GOPERL_PARSERW_MULTI_END] = (uint32_t)ps->multi_end;
            w[GOPERL_PARSERW_MULTI_OPEN] = (uint32_t)ps->multi_open;
            w[GOPERL_PARSERW_MULTI_CLOSE] = (uint32_t)ps->multi_close;
            w[GOPERL_PARSERW_COPLINE] = (uint32_t)ps->copline;
            w[GOPERL_PARSERW_IN_MY_STASH] = (uint32_t)(uintptr_t)(SV *)ps->in_my_stash;
            w[GOPERL_PARSERW_LAST_UNI] = (uint32_t)(uintptr_t)ps->last_uni;
            w[GOPERL_PARSERW_LAST_LOP] = (uint32_t)(uintptr_t)ps->last_lop;
            w[GOPERL_PARSERW_RSFP] = (uint32_t)(uintptr_t)ps->rsfp;
            w[GOPERL_PARSERW_RSFP_FILTERS] = (uint32_t)(uintptr_t)(SV *)ps->rsfp_filters;
            w[GOPERL_PARSERW_LEX_FORMBRACK] = (uint32_t)ps->lex_formbrack;
            return (uint64_t)(uintptr_t)w;
        }
        case GOPERL_CALL_ATOF: {
            const NV nv = my_atof(s ? s : "");
            uint64_t bits;
            memcpy(&bits, &nv, sizeof bits);
            return bits;
        }
        case GOPERL_CALL_AMT: {
            static uint32_t w[4 + NofAMmeth];
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
                    w[4 + i] = (uint32_t)(uintptr_t)(SV *)amt->table[i];
            }
            return (uint64_t)(uintptr_t)w;
        }
        case GOPERL_CALL_COP_SET: { /* the token is a COP */
            COP *cop = (COP *)(uintptr_t)(uint32_t)a;
            const uint32_t val = (uint32_t)b;
            switch ((int)(b >> 32)) {
            case GOPERL_COPF_LINE: CopLINE_set(cop, (line_t)val); return 0;
            case GOPERL_COPF_WARNINGS: cop->cop_warnings = (char *)(uintptr_t)val; return 0;
            case GOPERL_COPF_HINTS: cop->cop_hints = val; return 0;
            case GOPERL_COPF_STASH: CopSTASH_set(cop, (HV *)(uintptr_t)val); return 0;
            case GOPERL_COPF_FILE: CopFILE_set(cop, s ? s : ""); return 0;
            case GOPERL_COPF_FEATURES: cop->cop_features.bits[0] = val; return 0;
            }
            Perl_croak(aTHX_ "goperl: unknown cop field %d", (int)(b >> 32));
        }
        }
        Perl_croak(aTHX_ "goperl: unknown call %d", (int)(a >> 32));
    }
    case GOPERL_OP_TREE: { /* a = a GOPERL_TREE_ selector; s = u32 words */
        uint32_t w[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        if (s && b) memcpy(w, s, b < sizeof w ? (size_t)b : sizeof w);
#define GOPERL_W_OP(i) ((OP *)(uintptr_t)w[i])
#define GOPERL_W_SV(i) ((SV *)(uintptr_t)w[i])
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
            static uint32_t r[4];
            Perl_call_checker fn;
            SV *obj;
            U32 flags;
            cv_get_call_checker_flags((CV *)GOPERL_W_SV(0), (U32)w[1], &fn, &obj, &flags);
            r[0] = fn == Perl_ck_entersub_args_proto_or_list ? 2 : 0;
            r[1] = 0;
            r[2] = (uint32_t)(uintptr_t)obj;
            r[3] = flags;
            if (fn == goperl_callck_tramp) {
                SV **idp = av_fetch((AV *)obj, 0, 0);
                SV **objp = av_fetch((AV *)obj, 1, 0);
                r[0] = 1;
                r[1] = idp ? (uint32_t)SvUV(*idp) : 0;
                r[2] = (uint32_t)(uintptr_t)(objp ? *objp : NULL);
            }
            return (uint64_t)(uintptr_t)r;
        }
        case GOPERL_TREE_CXINC: return (uint64_t)(int64_t)cxinc();
        case GOPERL_TREE_NEWUNOP_AUX: {
            UNOP_AUX_item *aux = NULL;
            switch (w[3]) {
            case 1: aux = INT2PTR(UNOP_AUX_item *, (IV)(int32_t)w[4]); break;
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
            if ((I32)w[0] == OP_CUSTOM) {
                goperl_opfreehook_install(aTHX);
                g_custom_built[o] = OPclass_UNOP_AUX;
            }
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
            if (w[1]) { /* CvNAME_HEK_set */
                const char *name = (s && b > 16) ? s + 16 : "";
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
            if (!hek) return 0;
            return ((uint64_t)(uintptr_t)HEK_KEY(hek) << 32) |
                   ((uint32_t)HEK_LEN(hek) << 1) | (HEK_UTF8(hek) ? 1u : 0u);
        }
        case GOPERL_TREE_NEWPVOP: { /* the string follows the two words; the
                 * op owns a copy */
            const char *str = (s && b > 8) ? s + 8 : "";
            return (uint64_t)(uintptr_t)newPVOP((I32)w[0], (I32)w[1], savepv(str));
        }
        }
#undef GOPERL_W_OP
#undef GOPERL_W_SV
        Perl_croak(aTHX_ "goperl: unknown tree call %d", (int)a);
    }
    case GOPERL_OP_CX: { /* a = a GOPERL_CX_ selector<<32 | stackinfo token
                 * (0: the current one) */
        PERL_SI *si = (uint32_t)a ? (PERL_SI *)(uintptr_t)(uint32_t)a : PL_curstackinfo;
        switch ((int)(a >> 32)) {
        case GOPERL_CX_DUMP: { /* b = index -> the address of these words:
                 * [type][gimme][u16][oldsaveix][oldsp][oldmarksp]
                 * [oldscopesp][oldcop][oldpm][old_tmpsfloor], then by type:
                 * sub and format [cv][retop][olddepth][savearray]
                 * [prevcomppad]; eval [retop][old_namesv][cur_text][cv]
                 * [old_eval_root]; loop [my_op] */
            static uint32_t w[16];
            const I32 ix = (I32)(int64_t)b;
            if (!si || ix < 0 || ix > si->si_cxix) return 0;
            PERL_CONTEXT *cx = &si->si_cxstack[ix];
            memset(w, 0, sizeof w);
            w[0] = cx->cx_type;
            if (CxTYPE(cx) == CXt_SUBST) return (uint64_t)(uintptr_t)w;
            w[1] = cx->blk_gimme;
            w[2] = cx->blk_u16;
            w[3] = (uint32_t)cx->blk_oldsaveix;
            w[4] = (uint32_t)(int32_t)cx->blk_oldsp;
            w[5] = (uint32_t)cx->blk_oldmarksp;
            w[6] = (uint32_t)cx->blk_oldscopesp;
            w[7] = (uint32_t)(uintptr_t)(OP *)cx->blk_oldcop;
            w[8] = (uint32_t)(uintptr_t)(OP *)cx->blk_oldpm;
            w[9] = (uint32_t)(int32_t)cx->blk_old_tmpsfloor;
            switch (CxTYPE(cx)) {
            case CXt_SUB:
                w[10] = (uint32_t)(uintptr_t)(SV *)cx->blk_sub.cv;
                w[11] = (uint32_t)(uintptr_t)cx->blk_sub.retop;
                w[12] = (uint32_t)cx->blk_sub.olddepth;
                w[13] = (uint32_t)(uintptr_t)(SV *)cx->blk_sub.savearray;
                w[14] = (uint32_t)(uintptr_t)(SV *)cx->blk_sub.prevcomppad;
                break;
            case CXt_FORMAT:
                w[10] = (uint32_t)(uintptr_t)(SV *)cx->blk_format.cv;
                w[11] = (uint32_t)(uintptr_t)cx->blk_format.retop;
                break;
            case CXt_EVAL:
                w[10] = (uint32_t)(uintptr_t)cx->blk_eval.retop;
                w[11] = (uint32_t)(uintptr_t)cx->blk_eval.old_namesv;
                w[12] = (uint32_t)(uintptr_t)cx->blk_eval.cur_text;
                w[13] = (uint32_t)(uintptr_t)(SV *)cx->blk_eval.cv;
                w[14] = (uint32_t)(uintptr_t)cx->blk_eval.old_eval_root;
                break;
            default:
                if (CxTYPE_is_LOOP(cx))
                    w[10] = (uint32_t)(uintptr_t)(OP *)cx->blk_loop.my_op;
                break;
            }
            return (uint64_t)(uintptr_t)w;
        }
        case GOPERL_CX_DOUNWIND:
            dounwind((I32)(int64_t)b);
            return 0;
        case GOPERL_CX_MAX:
            return si ? (uint64_t)(int64_t)si->si_cxmax : 0;
        }
        Perl_croak(aTHX_ "goperl: unknown context call %d", (int)(a >> 32));
    }
    case GOPERL_OP_SV_MAGIC_STD: { /* real sv_magic: BEHAVIORAL core magic
                 * (ties and friends) must live guest-side to take effect
                 * (b = how<<32 | obj token, s+len = name or empty) */
        sv_magic((SV *)(uintptr_t)a, (SV *)(uintptr_t)(uint32_t)b,
                 (int)(int64_t)(b >> 32), s && *s ? s : NULL,
                 s ? (I32)strlen(s) : 0);
        return 0;
    }
    case GOPERL_OP_SV_MAGIC_SET_HOOK: { /* gives the anchor of sv the hooks
                 * of the mask b as well (see goperl_anchor_vtbl; no mask is
                 * set magic alone); mg_magical refreshes the SV's magic
                 * flags so that the interpreter calls them. */
        SV *sv = (SV *)(uintptr_t)a;
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
