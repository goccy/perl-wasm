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

extern "C" {
#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>
#include <perliol.h>
}

#include <cstdint>
#include <cstdio>
#include <string>

/* ---- static extension bootstraps ----------------------------------------
 * Mirrors the xs_init generated into perlmain.c by ExtUtils::Miniperl from the
 * static_ext list. KEEP IN SYNC with STATIC_EXT in scripts/wasi-configure.sh:
 * one boot_<Pkg> per statically linked XS extension. Each boot_* symbol lives
 * in the corresponding lib/auto/<Mod>/<Mod>.a archive, resolved at the
 * perl.wasm link. DynaLoader is the special first entry. */
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
void boot_Time__HiRes(pTHX_ CV *cv);
void boot_Time__Piece(pTHX_ CV *cv);
void boot_Unicode__Collate(pTHX_ CV *cv);
void boot_Unicode__Normalize(pTHX_ CV *cv);
void boot_mro(pTHX_ CV *cv);
void boot_re(pTHX_ CV *cv);
}

static void xs_init(pTHX) {
    static const char file[] = __FILE__;
    dXSUB_SYS;
    PERL_UNUSED_CONTEXT;

    /* DynaLoader is the special case (bootstraps the loader itself). */
    newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, file);
    newXS("attributes::bootstrap", boot_attributes, file);
    newXS("B::bootstrap", boot_B, file);
    newXS("Compress::Raw::Bzip2::bootstrap", boot_Compress__Raw__Bzip2, file);
    newXS("Compress::Raw::Zlib::bootstrap", boot_Compress__Raw__Zlib, file);
    newXS("Cwd::bootstrap", boot_Cwd, file);
    newXS("Data::Dumper::bootstrap", boot_Data__Dumper, file);
    newXS("Devel::Peek::bootstrap", boot_Devel__Peek, file);
    newXS("Digest::MD5::bootstrap", boot_Digest__MD5, file);
    newXS("Digest::SHA::bootstrap", boot_Digest__SHA, file);
    newXS("Encode::bootstrap", boot_Encode, file);
    newXS("Fcntl::bootstrap", boot_Fcntl, file);
    newXS("File::DosGlob::bootstrap", boot_File__DosGlob, file);
    newXS("File::Glob::bootstrap", boot_File__Glob, file);
    newXS("Filter::Util::Call::bootstrap", boot_Filter__Util__Call, file);
    newXS("Hash::Util::bootstrap", boot_Hash__Util, file);
    newXS("Hash::Util::FieldHash::bootstrap", boot_Hash__Util__FieldHash, file);
    newXS("I18N::Langinfo::bootstrap", boot_I18N__Langinfo, file);
    newXS("IO::bootstrap", boot_IO, file);
    newXS("List::Util::bootstrap", boot_List__Util, file);
    newXS("Math::BigInt::FastCalc::bootstrap", boot_Math__BigInt__FastCalc, file);
    newXS("MIME::Base64::bootstrap", boot_MIME__Base64, file);
    newXS("Opcode::bootstrap", boot_Opcode, file);
    newXS("POSIX::bootstrap", boot_POSIX, file);
    newXS("PerlIO::encoding::bootstrap", boot_PerlIO__encoding, file);
    newXS("PerlIO::via::bootstrap", boot_PerlIO__via, file);
    newXS("SDBM_File::bootstrap", boot_SDBM_File, file);
    newXS("Socket::bootstrap", boot_Socket, file);
    newXS("Storable::bootstrap", boot_Storable, file);
    newXS("Sys::Hostname::bootstrap", boot_Sys__Hostname, file);
    newXS("Time::HiRes::bootstrap", boot_Time__HiRes, file);
    newXS("Time::Piece::bootstrap", boot_Time__Piece, file);
    newXS("Unicode::Collate::bootstrap", boot_Unicode__Collate, file);
    newXS("Unicode::Normalize::bootstrap", boot_Unicode__Normalize, file);
    newXS("mro::bootstrap", boot_mro, file);
    newXS("re::bootstrap", boot_re, file);
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
 * that op type. Payload [u32 op][u32 op_type][u32 n][u32 stack-top tokens];
 * response [1][u32 next_op] on success, [0]+croak-message on failure. The
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

/* Reserved callback method id for PerlIO layer hooks: a host-registered
 * layer (op PERLIO_DEF_LAYER) gets its CUSTOM slots (Pushed/Fill/Popped)
 * run on the host; the stock buffered behavior stays guest-native. The
 * payload starts [u32 hook][u32 funcs_id][u32 ff][u32 layer]; responses
 * are hook-specific, [0]+message croaks in the guest. */
#define GOPERL_PERLIO_METHOD_ID (-6)

/* Host->guest response protocol (native XS / pp hook): byte 0 is the status,
 * GOPERL_RESP_OK followed by the result payload, GOPERL_RESP_FAIL followed by
 * the croak message bytes. */
#define GOPERL_RESP_FAIL 0
#define GOPERL_RESP_OK 1

/* pp-hook request payload: GOPERL_PP_HOOK_HDR_WORDS u32 header words
 * (op token, op type, peek count) followed by at most
 * GOPERL_PP_HOOK_PEEK_MAX stack-top SV tokens. */
#define GOPERL_PP_HOOK_HDR_WORDS 3
#define GOPERL_PP_HOOK_PEEK_MAX 8

/* Native-XSUB request payload: GOPERL_NATIVE_HDR_WORDS u32 header words
 * (fn_id, CV token, item count) followed by the frame's SV tokens. */
#define GOPERL_NATIVE_HDR_WORDS 3

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
        int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_KEYWORD_METHOD_ID,
                                             buf, 4 + (uint32_t)keyword_len);
        uint32_t resp_ptr = (uint32_t)((uint64_t)rc >> 32);
        uint32_t resp_len = (uint32_t)((uint64_t)rc & 0xFFFFFFFFu);
        if (resp_ptr != 0 && resp_len >= 1) {
            unsigned char *resp = (unsigned char *)(uintptr_t)resp_ptr;
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

static OP *goperl_call_pp_hook(pTHX_ OP *op) {
    if (g_go_cb == 0) return op->op_ppaddr(aTHX);
    /* payload: op token, op type, and a peek at the top of the perl stack
     * (hooks like pp_entersub_profiler read *SP to identify the callee). */
    uint32_t buf[GOPERL_PP_HOOK_HDR_WORDS + GOPERL_PP_HOOK_PEEK_MAX];
    SSize_t depth = PL_stack_sp - PL_stack_base;
    uint32_t n = depth > GOPERL_PP_HOOK_PEEK_MAX
                     ? GOPERL_PP_HOOK_PEEK_MAX
                     : (depth > 0 ? (uint32_t)depth : 0);
    buf[0] = (uint32_t)(uintptr_t)op;
    buf[1] = (uint32_t)op->op_type;
    buf[2] = n;
    for (uint32_t i = 0; i < n; i++)
        buf[GOPERL_PP_HOOK_HDR_WORDS + i] =
            (uint32_t)(uintptr_t)PL_stack_sp[-(SSize_t)(n - 1 - i)];
    int64_t rc = wasmify_callback_invoke(
        g_go_cb, GOPERL_PP_HOOK_METHOD_ID, buf,
        sizeof(uint32_t) * (GOPERL_PP_HOOK_HDR_WORDS + n));
    uint32_t resp_ptr = (uint32_t)((uint64_t)rc >> 32);
    uint32_t resp_len = (uint32_t)((uint64_t)rc & 0xFFFFFFFFu);
    if (resp_ptr == 0 || resp_len < 1)
        Perl_croak(aTHX_ "pp hook: empty response");
    unsigned char *resp = (unsigned char *)(uintptr_t)resp_ptr;
    if (resp[0] == GOPERL_RESP_FAIL) {
        char msg[512];
        size_t ml = resp_len - 1;
        if (ml > sizeof(msg) - 1) ml = sizeof(msg) - 1;
        memcpy(msg, resp + 1, ml);
        msg[ml] = '\0';
        free(resp);
        Perl_croak(aTHX_ "%s", msg);
    }
    uint32_t next = 0;
    if (resp_len >= 5) memcpy(&next, resp + 1, 4);
    free(resp);
    return (OP *)(uintptr_t)next;
}

static OP *goperl_pp_hooked_op(pTHX) {
    return goperl_call_pp_hook(aTHX_ PL_op);
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
 * Perl_runops_standard (see run.c), plus the pp-hook dispatch above. */
static int runops_interruptible(pTHX) {
    OP *op = PL_op;
    while ((PL_op = op = goperl_exec_op(aTHX_ op))) {
        if (g_interrupt) {
            g_interrupt = 0;
            Perl_croak(aTHX_ "Perl execution interrupted");
        }
    }
    PERL_ASYNC_CHECK();
    return 0;
}

/* ---- helpers ------------------------------------------------------------- */
static std::string sv_to_std(pTHX_ SV *sv) {
    if (!sv || !SvOK(sv)) return std::string();
    STRLEN len = 0;
    const char *p = SvPV(sv, len);
    return std::string(p, static_cast<size_t>(len));
}

static void json_escape(const std::string &in, std::string &out) {
    for (unsigned char c : in) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
}

static std::string json_field(const char *key, const std::string &val, bool comma) {
    std::string s = "\"";
    s += key;
    s += "\":\"";
    json_escape(val, s);
    s += "\"";
    if (comma) s += ",";
    return s;
}

/* XS trampoline: main::__plwasm_go_invoke($func_id, $payload) -> $response.
 * Forwards the payload bytes to the host through wasmify_callback_invoke and
 * returns the response bytes as a Perl string. The JSON encode/decode around
 * it lives in Perl (GO_BRIDGE_GLUE below); this function only moves bytes. */
XS(XS___plwasm_go_invoke);
XS(XS___plwasm_go_invoke) {
    dXSARGS;
    if (items != 2) Perl_croak(aTHX_ "usage: __plwasm_go_invoke(func_id, payload)");
    if (g_go_cb == 0)
        Perl_croak(aTHX_ "no Go dispatcher registered for this instance");
    IV func_id = SvIV(ST(0));
    STRLEN len = 0;
    const char *payload = SvPV(ST(1), len);
    int64_t rc = wasmify_callback_invoke(g_go_cb, static_cast<int32_t>(func_id),
                                         const_cast<char *>(payload),
                                         static_cast<size_t>(len));
    uint32_t resp_ptr = static_cast<uint32_t>(static_cast<uint64_t>(rc) >> 32);
    uint32_t resp_len = static_cast<uint32_t>(static_cast<uint64_t>(rc) & 0xFFFFFFFFu);
    SV *out;
    if (resp_ptr != 0 && resp_len != 0) {
        out = newSVpvn(reinterpret_cast<const char *>(static_cast<uintptr_t>(resp_ptr)),
                       static_cast<STRLEN>(resp_len));
        free(reinterpret_cast<void *>(static_cast<uintptr_t>(resp_ptr)));
    } else {
        out = newSVpvn("", 0);
    }
    ST(0) = sv_2mortal(out);
    XSRETURN(1);
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

    int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_NATIVE_METHOD_ID,
                                         payload, payload_len);
    free(payload);

    uint32_t resp_ptr = (uint32_t)((uint64_t)rc >> 32);
    uint32_t resp_len = (uint32_t)((uint64_t)rc & 0xFFFFFFFFu);
    if (resp_ptr == 0 || resp_len < 1)
        Perl_croak(aTHX_ "native XS dispatch: empty response");
    unsigned char *resp = (unsigned char *)(uintptr_t)resp_ptr;

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

/* Bridge glue, eval'd once at perl_new. JSON::PP + Scalar::Util (both in the
 * shipped stdlib / static XS) load lazily on the first bridge call so
 * interpreters that never cross the boundary don't pay for them.
 *
 * Value protocol (both directions): a call's argument/return list is a JSON
 * array of TAGGED nodes - JSON is only the carrier, the node tag is the
 * semantics:
 *
 *   {"k":"u"}                       undef
 *   {"k":"d","v":<scalar>}          plain scalar (number/string/bool) BY VALUE
 *   {"k":"j","v":<structure>}       composite data BY VALUE (fresh refs on
 *                                   decode - carries data, not identity)
 *   {"k":"r","h":<id>,"t":<reftype>,"c":<class>?}   a REFERENCE by HANDLE
 *
 * References are never serialised: __plwasm_enc pins the actual SV in a
 * registry (refcount held, id deduped by refaddr so the same SV always gets
 * the same id) and only the id crosses. Decoding an "r" node returns THE SAME
 * reference, so identity, aliasing, and blessedness survive a round trip
 * through Go. Every "r" node handed to the host carries one pin the HOST
 * owns — its wrapper's finalizer/Free releases it via __plwasm_release (or
 * batched, __plwasm_release_all) — so host-side liveness and the guest
 * refcount stay aligned in both directions: the registry keeps the SV alive
 * while the host can still reach it, and Perl's own refcounting resumes when
 * the last pin drops. utf8 mode: the C boundary carries bytes, so
 * encode/decode speak UTF-8 octets and non-ASCII round-trips. */
static const char *GO_BRIDGE_GLUE =
    "sub main::__plwasm_json {"
    "  $main::__plwasm_json_obj ||= do {"
    "    require JSON::PP;"
    "    require Scalar::Util;"
    "    JSON::PP->new->utf8->allow_nonref;"
    "  };"
    "}"
    /* --- reference-handle registry (identity-preserving pins) --- */
    "sub main::__plwasm_pin {"
    "  my ($r) = @_;"
    "  my $addr = Scalar::Util::refaddr($r);"
    "  my $id = $main::__plwasm_addr{$addr};"
    "  if (!defined $id) {"
    "    $id = ++$main::__plwasm_next_h;"
    "    $main::__plwasm_reg{$id} = $r;"
    "    $main::__plwasm_addr{$addr} = $id;"
    "  }"
    "  $main::__plwasm_pins{$id}++;"
    "  return $id;"
    "}"
    "sub main::__plwasm_release {"
    "  my ($id) = @_;"
    "  return 0 unless exists $main::__plwasm_reg{$id};"
    "  if (--$main::__plwasm_pins{$id} <= 0) {"
    "    delete $main::__plwasm_addr{Scalar::Util::refaddr($main::__plwasm_reg{$id})};"
    "    delete $main::__plwasm_reg{$id};"
    "    delete $main::__plwasm_pins{$id};"
    "  }"
    "  return 1;"
    "}"
    /* Batched release: the host's finalizer queue drains through one call.
     * Returns the number of still-live handles (test observability). */
    "sub main::__plwasm_release_all {"
    "  __plwasm_release($_) for @_;"
    "  return 0 + keys %main::__plwasm_reg;"
    "}"
    "sub main::__plwasm_handle {"
    "  my ($id) = @_;"
    "  my $r = $main::__plwasm_reg{$id};"
    "  die \"stale Perl reference handle $id\\n\" unless $r;"
    "  return $r;"
    "}"
    /* --- tagged value codec --- */
    "sub main::__plwasm_enc {"
    "  my ($v) = @_;"
    "  return { k => 'u' } unless defined $v;"
    "  if (ref $v) {"
    "    return { k => 'd', v => $v }"
    "      if Scalar::Util::blessed($v) && $v->isa('JSON::PP::Boolean');"
    "    my $n = { k => 'r', h => __plwasm_pin($v), t => Scalar::Util::reftype($v) };"
    "    my $c = Scalar::Util::blessed($v);"
    "    $n->{c} = $c if defined $c;"
    "    return $n;"
    "  }"
    "  return { k => 'd', v => $v };"
    "}"
    "sub main::__plwasm_dec {"
    "  my ($n) = @_;"
    "  my $k = $n->{k};"
    "  return undef   if $k eq 'u';"
    "  return $n->{v} if $k eq 'd' || $k eq 'j';"
    "  return __plwasm_handle($n->{h}) if $k eq 'r';"
    /* A host (Go) function value: materialise a closure over its id. Calling
     * it dispatches back to the host like any bound sub, so Perl can store
     * it, pass it around, and call it later - an ordinary code ref. */
    "  if ($k eq 'f') {"
    "    my $id = $n->{h};"
    "    return sub { main::__plwasm_go_call($id, @_) };"
    "  }"
    "  die \"unknown bridge value kind '$k'\\n\";"
    "}"
    /* --- Go -> Perl dispatch (perl_call) --- */
    /* Symbolic sub deref (&{$name}) needs no `no strict 'refs'`: this glue
     * compiles without `use strict`, and pulling the pragma in would load
     * strict.pm DURING perl_new - making boot depend on a readable stdlib,
     * which a custom-FS instance does not have yet at that point. */
    "sub main::__plwasm_call_dispatch {"
    "  my ($name, $args_json) = @_;"
    "  my @args = map { __plwasm_dec($_) } @{ __plwasm_json()->decode($args_json) };"
    "  my @ret = &{$name}(@args);"
    "  return __plwasm_json()->encode([ map { __plwasm_enc($_) } @ret ]);"
    "}"
    /* --- Perl -> Go dispatch (bound subs) ---
     * Reference arguments are pinned by __plwasm_enc and OWNED by the host
     * side: its wrapper attaches a finalizer/Free that releases each pin, so
     * a handler may simply keep a reference beyond the call. */
    "sub main::__plwasm_go_call {"
    "  my ($id, @args) = @_;"
    "  __plwasm_json();" /* load JSON::PP + Scalar::Util before encoding */
    "  my @nodes = map { __plwasm_enc($_) } @args;"
    "  my $resp = __plwasm_json()->decode("
    "      main::__plwasm_go_invoke($id, __plwasm_json()->encode(\\@nodes)));"
    "  die $resp->{error} unless $resp->{ok};"
    "  my @out = map { __plwasm_dec($_) } @{$resp->{result}};"
    "  return wantarray ? @out : $out[0];"
    "}"
    /* --- handle operations the host drives through perl_call --- */
    "sub main::__plwasm_method_call {"
    "  my ($id, $method, @args) = @_;"
    "  return __plwasm_handle($id)->$method(@args);"
    "}"
    "sub main::__plwasm_invoke_code {"
    "  my ($id, @args) = @_;"
    "  my $code = __plwasm_handle($id);"
    "  die \"handle $id is not a CODE reference\\n\""
    "    unless Scalar::Util::reftype($code) eq 'CODE';"
    "  return $code->(@args);"
    "}"
    "sub main::__plwasm_export {"
    "  my ($id) = @_;"
    /* Deep-copy the referenced structure as data. The TOP-LEVEL blessing is
     * peeled so a hash/array-based object exports its underlying structure;
     * NESTED blessed values convert via TO_JSON when they offer it and
     * degrade to null otherwise (allow_blessed), as do unknowns (code/glob).
     * Returns the JSON text; the host decodes it. */
    "  my $r = __plwasm_handle($id);"
    "  my $t = Scalar::Util::reftype($r) || '';"
    "  my $plain = $t eq 'HASH'   ? { %$r }"
    "            : $t eq 'ARRAY'  ? [ @$r ]"
    "            : $t eq 'SCALAR' ? $$r"
    "            : $t eq 'REF'    ? $$r"
    "            : $r;"
    "  my $j = JSON::PP->new->utf8->allow_nonref->allow_blessed->convert_blessed"
    "      ->allow_unknown;"
    "  return $j->encode($plain);"
    "}";

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

/* ---- public API (pl.h) --------------------------------------------------- */
uint64_t perl_new(const char *lib_dir) {
    if (g_my_perl) return 0; /* one interpreter per instance */

    /* Pin the C locale before the interpreter initialises its own. wasi-libc's
     * setlocale(LC_ALL, "") returns a positional composite (e.g.
     * "C.UTF-8;C;C;C;C;C") that Perl's LC_ALL parser rejects — it treats the
     * ';'-joined values as name=value pairs and panics ("needs an '=' to split
     * name=value") during construct. Forcing LC_ALL=C makes every category
     * uniform so the query returns a single "C". overwrite=0 respects a locale
     * the host deliberately put in the guest environment. */
    setenv("LC_ALL", "C", 0);

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
    perl_construct(g_my_perl);
    PL_exit_flags |= PERL_EXIT_DESTRUCT_END;

    /* Bootstrap args: perl [-I<lib_dir>] -e 0 — parse an empty program so the
     * interpreter is ready to eval, prepending lib_dir to @INC when given. */
    std::string incarg;
    const char *argv[5];
    int argc = 0;
    argv[argc++] = "perl";
    if (lib_dir && *lib_dir) {
        incarg = std::string("-I") + lib_dir;
        argv[argc++] = incarg.c_str();
    }
    argv[argc++] = "-e";
    argv[argc++] = "0";
    argv[argc] = nullptr;

    if (perl_parse(g_my_perl, xs_init, argc, const_cast<char **>(argv), nullptr)) {
        perl_destruct(g_my_perl);
        perl_free(g_my_perl);
        g_my_perl = nullptr;
        return 0;
    }
    perl_run(g_my_perl); /* runs the empty program; leaves the interp live */

    /* Install the interruptible run loop AFTER parse/run so the bootstrap
     * itself is never interrupted. */
    PL_runops = runops_interruptible;
    g_interrupt = 0;

    /* Install the Go bridge: the XS byte-mover plus the Perl-side JSON glue
     * (see GO_BRIDGE_GLUE). Cheap - JSON::PP itself loads lazily. */
    {
        PERL_SET_CONTEXT(g_my_perl);
        dTHX;
        newXS("main::__plwasm_go_invoke", XS___plwasm_go_invoke, __FILE__);
        eval_pv(GO_BRIDGE_GLUE, TRUE);
    }
    g_go_cb = 0;

    return 1; /* opaque handle */
}

std::string perl_eval(uint64_t h, const char *src) {
    if (!g_my_perl || h == 0) {
        return std::string("{\"ok\":false,\"result\":\"\",\"stdout\":\"\","
                           "\"stderr\":\"\",\"error\":\"no interpreter\"}");
    }
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;

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

    std::string result_s = ok ? sv_to_std(aTHX_ res_sv) : std::string();
    std::string out_s    = sv_to_std(aTHX_ out_sv);
    std::string err_s    = sv_to_std(aTHX_ err_sv);
    std::string error_s  = ok ? std::string() : sv_to_std(aTHX_ errsv_sv);

    std::string j = "{\"ok\":";
    j += ok ? "true" : "false";
    j += ",";
    j += json_field("result", result_s, true);
    j += json_field("stdout", out_s, true);
    j += json_field("stderr", err_s, true);
    j += json_field("error", error_s, false);
    j += "}";
    return j;
}

void perl_close(uint64_t h) {
    if (!g_my_perl || h == 0) return;
    PERL_SET_CONTEXT(g_my_perl);
    PL_perl_destruct_level = 2;
    perl_destruct(g_my_perl);
    perl_free(g_my_perl);
    g_my_perl = nullptr;
    /* PERL_SYS_TERM is intentionally NOT called here: it is process teardown,
     * and the instance may create a fresh interpreter afterwards. */
}

uint32_t perl_interrupt_addr(uint64_t h) {
    (void)h;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_interrupt));
}

/* Build the perl_call error envelope. "result" is always a valid (empty)
 * array so the caller can decode the document with one shape. */
static std::string call_error_json(const std::string &msg) {
    std::string j = "{\"ok\":false,\"result\":[],";
    j += json_field("error", msg, false);
    j += "}";
    return j;
}

/* perl_call_body is the guarded half of perl_call: everything that runs
 * under the JMPENV the wrapper pushes. Split out so a longjmp (a guest
 * exit()) does not jump over this function's C++ locals. */
static std::string perl_call_body(pTHX_ const char *sub_name, const char *args_json) {
    dSP;

    ENTER;
    SAVETMPS;
    PUSHMARK(SP);
    EXTEND(SP, 2);
    mPUSHs(newSVpv(sub_name, 0));
    mPUSHs(newSVpv(args_json && *args_json ? args_json : "[]", 0));
    PUTBACK;

    /* G_EVAL: a die inside the sub (or the JSON decode) lands in ERRSV instead
     * of longjmp'ing past us. The dispatcher runs the target sub in list
     * context internally and returns ONE scalar (the encoded result array). */
    int count = call_pv("__plwasm_call_dispatch", G_SCALAR | G_EVAL);
    SPAGAIN;

    std::string encoded;
    bool ok = true;
    std::string err;
    if (SvTRUE(ERRSV)) {
        ok = false;
        err = sv_to_std(aTHX_ ERRSV);
        if (count > 0) (void)POPs; /* discard the undef the failed call left */
    } else if (count > 0) {
        SV *sv = POPs;
        encoded = sv_to_std(aTHX_ sv);
    }
    PUTBACK;
    FREETMPS;
    LEAVE;

    if (!ok) return call_error_json(err);
    std::string j = "{\"ok\":true,\"result\":";
    /* `encoded` is already JSON (the dispatcher's encode(\@ret)); embed raw. */
    j += encoded.empty() ? "[]" : encoded;
    j += ",\"error\":\"\"}";
    return j;
}

std::string perl_call(uint64_t h, const char *sub_name, const char *args_json) {
    if (!g_my_perl || h == 0) return call_error_json("no interpreter");
    if (!sub_name || !*sub_name) return call_error_json("empty sub name");
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;

    /* Catch a guest exit(): my_exit unwinds with JMPENV_JUMP(2), and without
     * a live JMPENV here it would fall through to the C exit() - a wasi
     * proc_exit that aborts the wasm mid-call, before PerlIO ever flushes.
     * Mirroring perl_run's own catch (perl.c), the exit unwinds cleanly back
     * to this frame, the interpreter stays destructible (flush + END blocks
     * run at perl_close), and the status is reported in the envelope's
     * "exit" field for the host to turn into a process exit. */
    dJMPENV;
    int jmp;
    I32 oldscope = PL_scopestack_ix;
    std::string out;
    JMPENV_PUSH(jmp);
    switch (jmp) {
    case 0:
        out = perl_call_body(aTHX_ sub_name, args_json);
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
        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      "{\"ok\":false,\"result\":[],\"exit\":%d,\"error\":\"\"}",
                      (int)STATUS_EXIT);
        out = buf;
        break;
    }
    default:
        out = call_error_json("perl_call: unexpected longjmp");
        break;
    }
    JMPENV_POP;
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

static const MGVTBL goperl_host_mg_vtbl = {
    NULL, NULL, NULL, NULL, goperl_host_mg_free, NULL, NULL, NULL
};

/* Set-magic variant of the anchor: assigning to the SV additionally fires
 * the host id over GOPERL_MG_SET_METHOD_ID so mirror svt_set hooks run.
 * Anchors start set-less (attaching set magic makes every write to the SV
 * pay a host round trip) and are upgraded on demand via op
 * SV_MAGIC_SET_HOOK when a mirror entry actually carries svt_set. */
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

static const MGVTBL goperl_host_mg_vtbl_set = {
    NULL, goperl_host_mg_set, NULL, NULL, goperl_host_mg_free, NULL, NULL, NULL
};

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
    GOPERL_OP_SV_CLASSIFY = 184
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
    GOPERL_OPC_RAW = 12
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
    GOPERL_OPF_PPADDR_HOOK = 9
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
    GOPERL_PAD_SV_FETCH = 5
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
#define GOPERL_SI_SELF 0       /* SI_GET: the stackinfo token itself */
#define GOPERL_SI_PREV 1       /* SI_GET: si_prev */
#define GOPERL_SI_TYPE 2       /* SI_GET: si_type */
#define GOPERL_SI_CXIX 3       /* SI_GET: si_cxix */
#define GOPERL_CXPTR_OLDCOP 0  /* CX_PTR: blk_oldcop */
#define GOPERL_CXPTR_CV 1      /* CX_PTR: blk_sub.cv (SUB/FORMAT only) */
#define GOPERL_CXPTR_MYOP 2    /* CX_PTR: blk_loop.my_op (LOOP only) */

/* CX_FIELDS result when the context index is out of range. */
#define GOPERL_CX_TYPE_NONE 0xFFFFFFFFu

uint64_t perl_xs_helper(uint64_t h, int32_t op, uint64_t a, uint64_t b, const char *s) {
    if (!g_my_perl || h == 0) return 0;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    switch (op) {
    case GOPERL_OP_SV_IV:
        return (uint64_t)(int64_t)SvIV((SV *)(uintptr_t)a);
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
        return (uint64_t)(uintptr_t)sv_setref_iv((SV *)(uintptr_t)a,
                                                 s ? s : "", (IV)(int64_t)b);

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
        if (sv_isobject(sv)) f |= GOPERL_INFO_ISOBJ;
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
    case GOPERL_OP_HV_ITERNEXT:
        return (uint64_t)(uintptr_t)hv_iternext((HV *)(uintptr_t)a);
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
         * G_EVAL. */
        I32 count;
        if (op == 66)
            count = call_method(method, flags | G_EVAL);
        else
            count = call_sv((SV *)(uintptr_t)(uint32_t)a, flags | G_EVAL);
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
    case GOPERL_OP_ERRSV:
        return (uint64_t)(uintptr_t)ERRSV;
    case GOPERL_OP_SAVE_OP: /* scope-save the current PL_op (the SAVEOP() macro) */
        save_op();
        return 0;
    case GOPERL_OP_RUN_PP: { /* a = op_flags<<32 | op_type; s+b = packed args.
                * Executes one pp function on a scratch OP, list context,
                * returning the produced values like CALL_SV. The caller is
                * expected to have wrapped this in ENTER/SAVE_OP/LEAVE. */
        uint32_t op_type = (uint32_t)a;
        uint32_t op_flags = (uint32_t)(a >> 32);
        if (op_type >= OP_max) return 0;
        size_t n = (size_t)b / 4;
        dSP;
        PUSHMARK(SP);
        EXTEND(SP, (SSize_t)n);
        for (size_t i = 0; i < n; i++) {
            uint32_t tok;
            memcpy(&tok, s + i * 4, 4);
            PUSHs((SV *)(uintptr_t)tok);
        }
        PUTBACK;
        OP scratch;
        Zero(&scratch, 1, OP);
        scratch.op_type = op_type;
        scratch.op_flags = (U8)op_flags;
        scratch.op_ppaddr = PL_ppaddr[op_type];
        OP *saved = PL_op;
        PL_op = &scratch;
        scratch.op_ppaddr(aTHX);
        PL_op = saved;
        SPAGAIN;
        SV **mark = PL_stack_base + TOPMARK;
        POPMARK;
        I32 count = (I32)(PL_stack_sp - mark);
        AV *res = newAV();
        for (I32 i = 0; i < count; i++)
            av_store(res, i, SvREFCNT_inc(mark[i + 1]));
        PL_stack_sp = mark;
        sv_2mortal((SV *)res);
        return ((uint64_t)(uint32_t)count << 32) | (uint32_t)(uintptr_t)res;
    }
    case GOPERL_OP_MAGIC_ATTACH: { /* b = host_id<<32 | obj token */
        SV *obj = (SV *)(uintptr_t)(uint32_t)b;
        MAGIC *mg = sv_magicext((SV *)(uintptr_t)a, obj, PERL_MAGIC_ext,
                                &goperl_host_mg_vtbl, NULL, 0);
        mg->mg_private = 0;
        mg->mg_ptr = (char *)(uintptr_t)(uint32_t)(b >> 32);
        return 0;
    }
    case GOPERL_OP_MAGIC_ID: {
        SV *sv = (SV *)(uintptr_t)a;
        if (SvTYPE(sv) < SVt_PVMG) return 0;
        for (MAGIC *mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic)
            if (mg->mg_virtual == &goperl_host_mg_vtbl)
                return (uint64_t)(uint32_t)(uintptr_t)mg->mg_ptr;
        return 0;
    }
    case GOPERL_OP_MAGIC_UNATTACH:
        sv_unmagicext((SV *)(uintptr_t)a, PERL_MAGIC_ext,
                      (MGVTBL *)&goperl_host_mg_vtbl);
        return 0;
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
                * can model CvXSUBANY/alias dispatch). */
        CV *cv = newXS(s ? s : "", XS_goperl_native_thunk, __FILE__);
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
                * primitive matching a native `AvARRAY(av)[i] = sv`. */
        AV *av = (AV *)(uintptr_t)a;
        SSize_t idx = (SSize_t)(int32_t)(b >> 32);
        if (SvTYPE((SV *)av) == SVt_PVAV && idx >= 0 && idx <= AvFILLp(av))
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
            return o->op_ppaddr == goperl_pp_hooked_op ? 1 : 0;
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
        return (uint64_t)CxTYPE(cx);
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
    case GOPERL_OP_HV_DELETE: { /* (s = key, b = flags) */
        SV *r = hv_delete((HV *)(uintptr_t)a, s ? s : "",
                          (I32)strlen(s ? s : ""), (I32)(int64_t)b);
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
    case GOPERL_OP_GIMME_V: /* caller context of the innermost block */
        return (uint64_t)(int64_t)block_gimme();
    case GOPERL_OP_CKWARN: /* a = warning category; b = ckWARN_d variant */
        return (b ? ckWARN_d((U32)a) : ckWARN((U32)a)) ? 1 : 0;
    case GOPERL_OP_SV_LEN_BUF: /* SvLEN: the ALLOCATED buffer size */
        return (SvTYPE((SV *)(uintptr_t)a) < SVt_PV)
                   ? 0
                   : (uint64_t)SvLEN((SV *)(uintptr_t)a);
    case GOPERL_OP_HV_STORE_KLEN: { /* b = utf8<<32 | value token, s+len =
                 * key bytes; takes over the value ref -> stored SV token.
                 * hv_common with JUST_SV returns the SLOT (SV**). */
        SV *val = (SV *)(uintptr_t)(uint32_t)b;
        SV **svp = (SV **)hv_common((HV *)(uintptr_t)a, NULL, s ? s : "",
                                    strlen(s ? s : ""),
                                    (b >> 32) ? HVhek_UTF8 : 0,
                                    HV_FETCH_ISSTORE | HV_FETCH_JUST_SV, val,
                                    0);
        return svp ? (uint64_t)(uintptr_t)*svp : 0;
    }
    case GOPERL_OP_HV_FETCH_KLEN: { /* b = utf8<<32 | lval, s+len = key */
        SV **svp = (SV **)hv_common((HV *)(uintptr_t)a, NULL, s ? s : "",
                                    strlen(s ? s : ""),
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
        case GOPERL_OPC_RAW: {
            /* backing for a module-defined op struct: LOGOP-shaped slab
             * memory the host will type/link/hook through OP_SET */
            o = (OP *)Perl_Slab_Alloc(aTHX_ sizeof(LOGOP));
            o->op_type = OP_NULL;
            o->op_ppaddr = PL_ppaddr[OP_NULL];
            break;
        }
        default: Perl_croak(aTHX_ "goperl: unknown op class %d", cls);
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
        case GOPERL_OPF_PPADDR_HOOK: o->op_ppaddr = goperl_pp_hooked_op; return 0;
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
    case GOPERL_OP_SAVE_MISC: { /* a: 0 = save_freesv(b = sv token) */
        if ((int)a == 0) {
            save_freesv((SV *)(uintptr_t)(uint32_t)b);
            return 0;
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
    case GOPERL_OP_SV_MAGIC_STD: { /* real sv_magic: BEHAVIORAL core magic
                 * (ties and friends) must live guest-side to take effect
                 * (b = how<<32 | obj token, s+len = name or empty) */
        sv_magic((SV *)(uintptr_t)a, (SV *)(uintptr_t)(uint32_t)b,
                 (int)(int64_t)(b >> 32), s && *s ? s : NULL,
                 s ? (I32)strlen(s) : 0);
        return 0;
    }
    case GOPERL_OP_SV_MAGIC_SET_HOOK: { /* upgrade the anchor magic on sv to
                 * the set-firing vtbl (idempotent); mg_magical refreshes the
                 * SV's magic flags so assignments actually trigger it. */
        SV *sv = (SV *)(uintptr_t)a;
        if (SvTYPE(sv) < SVt_PVMG) return 0;
        for (MAGIC *mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic) {
            if (mg->mg_virtual == &goperl_host_mg_vtbl ||
                mg->mg_virtual == &goperl_host_mg_vtbl_set) {
                mg->mg_virtual = (MGVTBL *)&goperl_host_mg_vtbl_set;
                mg_magical(sv);
                return 1;
            }
        }
        return 0;
    }
    }
    return 0;
}
