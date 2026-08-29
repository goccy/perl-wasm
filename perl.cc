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

/* Host-writable interrupt flag. The host trips a runaway eval by storing 1 here
 * via a plain linear-memory write (perl_interrupt_addr returns its address); no
 * wasm code runs on the instance. The interruptible run loop clears it and
 * croaks. `volatile` so the compiler always reloads it in the op loop. */
static volatile uint32_t g_interrupt = 0;

/* PL_runops replacement: checks the interrupt flag on every opcode. When set,
 * Perl_croak longjmps out to the nearest eval trap — for perl_eval that is the
 * `eval $src` inside the wrapper, so the eval returns ok=false with the message
 * in $@, exactly like a Perl-level die. Otherwise identical to
 * Perl_runops_standard (see run.c). */
static int runops_interruptible(pTHX) {
    OP *op = PL_op;
    while ((PL_op = op = op->op_ppaddr(aTHX))) {
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

    size_t payload_len = sizeof(uint32_t) * (3 + (size_t)items);
    uint32_t *payload = (uint32_t *)malloc(payload_len);
    if (!payload) Perl_croak(aTHX_ "native XS dispatch: out of memory");
    payload[0] = (uint32_t)fn_id;
    payload[1] = (uint32_t)(uintptr_t)cv; /* real XSUBs receive their CV */
    payload[2] = (uint32_t)items;
    for (I32 i = 0; i < items; i++)
        payload[3 + i] = (uint32_t)(uintptr_t)ST(i);

    int64_t rc = wasmify_callback_invoke(g_go_cb, GOPERL_NATIVE_METHOD_ID,
                                         payload, payload_len);
    free(payload);

    uint32_t resp_ptr = (uint32_t)((uint64_t)rc >> 32);
    uint32_t resp_len = (uint32_t)((uint64_t)rc & 0xFFFFFFFFu);
    if (resp_ptr == 0 || resp_len < 1)
        Perl_croak(aTHX_ "native XS dispatch: empty response");
    unsigned char *resp = (unsigned char *)(uintptr_t)resp_ptr;

    if (resp[0] == 0) { /* failure: the rest is the croak message */
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

/* Anchor magic for host-side MAGIC mirrors (perl_xs_helper ops 70-72). The
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

uint64_t perl_xs_helper(uint64_t h, int32_t op, uint64_t a, uint64_t b, const char *s) {
    if (!g_my_perl || h == 0) return 0;
    PERL_SET_CONTEXT(g_my_perl);
    dTHX;
    switch (op) {
    case 1: /* SV_IV */
        return (uint64_t)(int64_t)SvIV((SV *)(uintptr_t)a);
    case 2: { /* SV_PV: (linear-memory ptr << 32) | len */
        STRLEN len = 0;
        const char *p = SvPV((SV *)(uintptr_t)a, len);
        return ((uint64_t)(uint32_t)(uintptr_t)p << 32) | (uint32_t)len;
    }
    case 3: /* NEW_IV */
        return (uint64_t)(uintptr_t)newSViv((IV)(int64_t)a);
    case 4: /* NEW_PVN */
        return (uint64_t)(uintptr_t)newSVpvn(s ? s : "", (STRLEN)b);
    case 5: /* SV_MORTAL */
        return (uint64_t)(uintptr_t)sv_2mortal((SV *)(uintptr_t)a);
    case 6: /* NEW_UV */
        return (uint64_t)(uintptr_t)newSVuv((UV)a);
    case 7: /* NEW_AV */
        return (uint64_t)(uintptr_t)(SV *)newAV();
    case 8: /* NEW_HV */
        return (uint64_t)(uintptr_t)(SV *)newHV();
    case 9: /* NEW_RV_INC */
        return (uint64_t)(uintptr_t)newRV_inc((SV *)(uintptr_t)a);
    case 10: /* AV_PUSH (steals a ref, like the C API) */
        av_push((AV *)(uintptr_t)a, (SV *)(uintptr_t)b);
        return 0;
    case 11: /* AV_LEN */
        return (uint64_t)(int64_t)av_len((AV *)(uintptr_t)a);
    case 12: { /* AV_FETCH */
        SV **slot = av_fetch((AV *)(uintptr_t)a, (SSize_t)(int64_t)b, FALSE);
        return slot ? (uint64_t)(uintptr_t)*slot : 0;
    }
    case 13: { /* HV_STORE (steals a ref) */
        hv_store((HV *)(uintptr_t)a, s ? s : "", (I32)strlen(s ? s : ""),
                 (SV *)(uintptr_t)b, 0);
        return 0;
    }
    case 14: { /* HV_FETCH */
        SV **slot = hv_fetch((HV *)(uintptr_t)a, s ? s : "",
                             (I32)strlen(s ? s : ""), FALSE);
        return slot ? (uint64_t)(uintptr_t)*slot : 0;
    }
    case 15: /* SV_REFCNT_INC */
        return (uint64_t)(uintptr_t)SvREFCNT_inc((SV *)(uintptr_t)a);
    case 16: /* GV_STASHPV */
        return (uint64_t)(uintptr_t)gv_stashpv(s ? s : "", (I32)(int64_t)a);
    case 17: /* SV_BLESS */
        return (uint64_t)(uintptr_t)sv_bless((SV *)(uintptr_t)a,
                                             (HV *)(uintptr_t)b);
    case 18: { /* SV_RV: SvRV token when SvROK, else 0 */
        SV *sv = (SV *)(uintptr_t)a;
        return SvROK(sv) ? (uint64_t)(uintptr_t)SvRV(sv) : 0;
    }
    case 19: /* SV_TYPE: the raw svtype enum value (the SDK pins the same
              * enum, so SVt_* comparisons agree across the boundary) */
        return (uint64_t)SvTYPE((SV *)(uintptr_t)a);
    case 20: /* SV_ISA */
        return sv_isa((SV *)(uintptr_t)a, s ? s : "") ? 1 : 0;
    case 21: /* SV_DERIVED_FROM */
        return sv_derived_from((SV *)(uintptr_t)a, s ? s : "") ? 1 : 0;
    case 22: /* SETREF_IV: bless rv as class s pointing at IV b */
        return (uint64_t)(uintptr_t)sv_setref_iv((SV *)(uintptr_t)a,
                                                 s ? s : "", (IV)(int64_t)b);

    /* ---- v3: the surface interpreter-heavy XS (Text::Xslate class)
     * compiles against. Conventions as above: a/b are u64 operands
     * (SV/AV/HV/HE tokens in the low 32 bits when packed), s+b carry
     * strings or packed u32 token arrays. */
    case 23: /* SV_SETSV */
        sv_setsv((SV *)(uintptr_t)a, (SV *)(uintptr_t)b);
        return 0;
    case 24: /* SV_SETSV_NOMG */
        sv_setsv_nomg((SV *)(uintptr_t)a, (SV *)(uintptr_t)b);
        return 0;
    case 25: /* SV_SETSV_MG */
        sv_setsv_mg((SV *)(uintptr_t)a, (SV *)(uintptr_t)b);
        return 0;
    case 26: /* SV_SETIV */
        sv_setiv((SV *)(uintptr_t)a, (IV)(int64_t)b);
        return 0;
    case 27: /* SV_SETUV */
        sv_setuv((SV *)(uintptr_t)a, (UV)b);
        return 0;
    case 28: { /* SV_SETNV (b = IEEE bits) */
        union { uint64_t u; double d; } cvt;
        cvt.u = b;
        sv_setnv((SV *)(uintptr_t)a, (NV)cvt.d);
        return 0;
    }
    case 29: /* SV_SETPVN */
        sv_setpvn((SV *)(uintptr_t)a, s ? s : "", (STRLEN)b);
        return 0;
    case 30: { /* SV_NV -> IEEE bits */
        union { uint64_t u; double d; } cvt;
        cvt.d = (double)SvNV((SV *)(uintptr_t)a);
        return cvt.u;
    }
    case 31: { /* NEW_NV (a = IEEE bits) */
        union { uint64_t u; double d; } cvt;
        cvt.u = a;
        return (uint64_t)(uintptr_t)newSVnv((NV)cvt.d);
    }
    case 32: /* NEW_SVSV */
        return (uint64_t)(uintptr_t)newSVsv((SV *)(uintptr_t)a);
    case 33: /* SV_MORTALCOPY */
        return (uint64_t)(uintptr_t)sv_mortalcopy((SV *)(uintptr_t)a);
    case 34: /* SV_CATSV */
        sv_catsv((SV *)(uintptr_t)a, (SV *)(uintptr_t)b);
        return 0;
    case 35: /* SV_CATSV_NOMG */
        sv_catsv_nomg((SV *)(uintptr_t)a, (SV *)(uintptr_t)b);
        return 0;
    case 36: /* SV_CATPVN */
        sv_catpvn((SV *)(uintptr_t)a, s ? s : "", (STRLEN)b);
        return 0;
    case 37: /* SV_TRUE */
        return SvTRUE((SV *)(uintptr_t)a) ? 1 : 0;
    case 38: { /* SV_INFO: flag bitset (mirrored by the SDK's synthetic
                * SVf_ / GOPERL_INFO_ constants) */
        SV *sv = (SV *)(uintptr_t)a;
        uint64_t f = 0;
        if (SvOK(sv)) f |= 1;
        if (SvROK(sv)) f |= 2;
        if (SvPOK(sv)) f |= 4;
        if (SvNIOK(sv)) f |= 8;
        if (SvUTF8(sv)) f |= 16;
        if (sv_isobject(sv)) f |= 32;
        if (SvREADONLY(sv)) f |= 64;
        if (SvTYPE(sv) == SVt_PVCV) f |= 128;
        if (SvTYPE(sv) == SVt_PVGV) f |= 256;
        if (SvIOK(sv)) f |= 512;
        if (SvNOK(sv)) f |= 1024;
        if (SvPOKp(sv)) f |= 2048;
        if (SvNIOKp(sv)) f |= 4096;
        if (SvIOK(sv) && SvIsUV(sv)) f |= 8192;
        if (SvRMAGICAL(sv)) f |= 16384;
        if (SvOBJECT(sv)) f |= 32768;
        return f;
    }
    case 39: /* SV_CUR_SET */
        SvCUR_set((SV *)(uintptr_t)a, (STRLEN)b);
        return 0;
    case 40: { /* SV_GROW -> new PVX (linear-memory offset) */
        char *p = SvGROW((SV *)(uintptr_t)a, (STRLEN)b);
        return (uint64_t)(uint32_t)(uintptr_t)p;
    }
    case 41: /* SV_EQ */
        return sv_eq((SV *)(uintptr_t)a, (SV *)(uintptr_t)b) ? 1 : 0;
    case 42: /* SV_CMP */
        return (uint64_t)(int64_t)sv_cmp((SV *)(uintptr_t)a, (SV *)(uintptr_t)b);
    case 43: /* SV_RVWEAKEN */
        sv_rvweaken((SV *)(uintptr_t)a);
        return 0;
    case 44: { /* SV_2CV */
        HV *stash = NULL;
        GV *gv = NULL;
        CV *cv = sv_2cv((SV *)(uintptr_t)a, &stash, &gv, (I32)(int64_t)b);
        return (uint64_t)(uintptr_t)cv;
    }
    case 45: /* GET_SV */
        return (uint64_t)(uintptr_t)get_sv(s ? s : "", (I32)(int64_t)a);
    case 46: /* GET_CV */
        return (uint64_t)(uintptr_t)get_cv(s ? s : "", (I32)(int64_t)a);
    case 47: /* GV_FETCH (a = flags, b = svtype) */
        return (uint64_t)(uintptr_t)gv_fetchpv(s ? s : "", (I32)(int64_t)a,
                                               (svtype)(int64_t)b);
    case 48: /* AV_STORE (b = idx<<32 | sv; takes ownership like the C API) */
        av_store((AV *)(uintptr_t)a, (SSize_t)(int32_t)(b >> 32),
                 (SV *)(uintptr_t)(uint32_t)b);
        return 0;
    case 49: /* AV_EXTEND */
        av_extend((AV *)(uintptr_t)a, (SSize_t)(int64_t)b);
        return 0;
    case 50: /* AV_FILL */
        av_fill((AV *)(uintptr_t)a, (SSize_t)(int64_t)b);
        return 0;
    case 51: { /* AV_READ: fetch without creating (like op 12) but lval-safe */
        SV **slot = av_fetch((AV *)(uintptr_t)a, (SSize_t)(int64_t)b, TRUE);
        return slot ? (uint64_t)(uintptr_t)*slot : 0;
    }
    case 53: { /* HV_FETCH_ENT (b = lval<<32 | keysv) -> HE token */
        HE *he = hv_fetch_ent((HV *)(uintptr_t)a,
                              (SV *)(uintptr_t)(uint32_t)b,
                              (b >> 32) ? TRUE : FALSE, 0U);
        return (uint64_t)(uintptr_t)he;
    }
    case 54: { /* HV_STORE_ENT (b = keysv<<32 | sv; takes value ownership) */
        hv_store_ent((HV *)(uintptr_t)a, (SV *)(uintptr_t)(uint32_t)(b >> 32),
                     (SV *)(uintptr_t)(uint32_t)b, 0U);
        return 0;
    }
    case 55: /* HV_EXISTS_ENT (b = keysv) */
        return hv_exists_ent((HV *)(uintptr_t)a, (SV *)(uintptr_t)b, 0U) ? 1 : 0;
    case 56: /* HE_VAL */
        return (uint64_t)(uintptr_t)HeVAL((HE *)(uintptr_t)a);
    case 57: /* HV_ITERINIT */
        return (uint64_t)(int64_t)hv_iterinit((HV *)(uintptr_t)a);
    case 58: /* HV_ITERNEXT */
        return (uint64_t)(uintptr_t)hv_iternext((HV *)(uintptr_t)a);
    case 59: /* HV_ITERKEYSV */
        return (uint64_t)(uintptr_t)hv_iterkeysv((HE *)(uintptr_t)a);
    case 60: /* HV_ITERVAL (a = hv, b = he) */
        return (uint64_t)(uintptr_t)hv_iterval((HV *)(uintptr_t)a,
                                               (HE *)(uintptr_t)b);
    case 61: ENTER; return 0;
    case 62: LEAVE; return 0;
    case 63: SAVETMPS; return 0;
    case 64: FREETMPS; return 0;
    case 65:   /* CALL_SV (a = flags<<32 | sv; s+b = packed u32 arg tokens) */
    case 66: { /* CALL_METHOD (a = flags<<32; s = "name\0" + packed tokens) */
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
    case 67: /* ERRSV_TOK */
        return (uint64_t)(uintptr_t)ERRSV;
    case 68: /* SAVE_OP: scope-save the current PL_op (the SAVEOP() macro) */
        save_op();
        return 0;
    case 69: { /* RUN_PP_SCRATCH (a = op_flags<<32 | op_type; s+b = args).
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
    case 70: { /* SV_MAGIC_ATTACH (b = host_id<<32 | obj token) */
        SV *obj = (SV *)(uintptr_t)(uint32_t)b;
        MAGIC *mg = sv_magicext((SV *)(uintptr_t)a, obj, PERL_MAGIC_ext,
                                &goperl_host_mg_vtbl, NULL, 0);
        mg->mg_private = 0;
        mg->mg_ptr = (char *)(uintptr_t)(uint32_t)(b >> 32);
        return 0;
    }
    case 71: { /* SV_MAGIC_ID */
        SV *sv = (SV *)(uintptr_t)a;
        if (SvTYPE(sv) < SVt_PVMG) return 0;
        for (MAGIC *mg = SvMAGIC(sv); mg; mg = mg->mg_moremagic)
            if (mg->mg_virtual == &goperl_host_mg_vtbl)
                return (uint64_t)(uint32_t)(uintptr_t)mg->mg_ptr;
        return 0;
    }
    case 72: /* SV_MAGIC_UNATTACH */
        sv_unmagicext((SV *)(uintptr_t)a, PERL_MAGIC_ext,
                      (MGVTBL *)&goperl_host_mg_vtbl);
        return 0;
    case 73: { /* PLVAR_GET */
        switch ((int)a) {
        case 1: return (uint64_t)(uintptr_t)PL_diehook;
        case 2: return (uint64_t)(uintptr_t)PL_warnhook;
        case 3: return (uint64_t)(uintptr_t)&PL_sv_undef;
        case 4: return (uint64_t)(uintptr_t)&PL_sv_yes;
        case 5: return (uint64_t)(uintptr_t)&PL_sv_no;
        case 6: return (uint64_t)(uintptr_t)PL_curcop;
        case 7: return (uint64_t)(uintptr_t)PL_op;
        }
        return 0;
    }
    case 74: { /* PLVAR_SET (b = SV token or 0) */
        SV *val = (SV *)(uintptr_t)(uint32_t)b;
        switch ((int)a) {
        case 1: PL_diehook = val; return 0;
        case 2: PL_warnhook = val; return 0;
        }
        return 0;
    }
    case 75: /* SV_UTF8_ON */
        SvUTF8_on((SV *)(uintptr_t)a);
        return 0;
    case 76: /* SV_POK_ON */
        SvPOK_on((SV *)(uintptr_t)a);
        return 0;
    case 77: /* SV_DUMP */
        sv_dump((SV *)(uintptr_t)a);
        return 0;
    case 78: /* SV_REFCNT_DEC */
        SvREFCNT_dec((SV *)(uintptr_t)a);
        return 0;
    case 79: /* SV_UNMAGIC (a = sv, b = how) */
        sv_unmagic((SV *)(uintptr_t)a, (int)(int64_t)b);
        return 0;
    case 80: { /* SAVE_HOOK: scope-save a hook variable (a = 1 die / 2 warn,
                * b = 0 save_sptr / 1 save_generic_svref). The host assigns
                * the new value separately via PLVAR_SET. */
        SV **loc = ((int)a == 1) ? &PL_diehook : &PL_warnhook;
        if (b) save_generic_svref(loc);
        else   save_sptr(loc);
        return 0;
    }
    case 81: { /* NEW_XS: bind the generic native thunk as sub s with fn_id a
                * and return the CV token (newXS + XSANY, like
                * perl_register_native_xs but with the CV surfaced so the host
                * can model CvXSUBANY/alias dispatch). */
        CV *cv = newXS(s ? s : "", XS_goperl_native_thunk, __FILE__);
        CvXSUBANY(cv).any_i32 = (I32)(int64_t)a;
        return (uint64_t)(uintptr_t)cv;
    }
    case 82: { /* SV_STASH: the blessing stash of an object SV, else 0 */
        SV *sv = (SV *)(uintptr_t)a;
        return SvOBJECT(sv) ? (uint64_t)(uintptr_t)SvSTASH(sv) : 0;
    }
    case 83: /* NEW_SV */
        return (uint64_t)(uintptr_t)newSV((STRLEN)a);
    case 84: /* SV_UPGRADE */
        sv_upgrade((SV *)(uintptr_t)a, (svtype)(int64_t)b);
        return 0;
    case 85: /* SV_UTF8_UPGRADE */
        sv_utf8_upgrade((SV *)(uintptr_t)a);
        return 0;
    case 86: /* SAVE_DELETE (b = signed klen, negative = UTF-8 key) */
        save_delete((HV *)(uintptr_t)a,
                    savepvn(s ? s : "", (I32)((int64_t)b < 0 ? -(int64_t)b
                                                             : (int64_t)b)),
                    (I32)(int64_t)b);
        return 0;
    case 87: { /* SAVE_HELEM (b = flags<<32 | keysv) */
        HV *hv = (HV *)(uintptr_t)a;
        SV *key = (SV *)(uintptr_t)(uint32_t)b;
        HE *he = hv_fetch_ent(hv, key, TRUE, 0U);
        if (he) save_helem_flags(hv, key, &HeVAL(he), (U32)(b >> 32));
        return 0;
    }
    case 88: /* NEW_PVN_SHARE (b = signed len, negative = UTF-8) */
        return (uint64_t)(uintptr_t)newSVpvn_share(s ? s : "",
                                                   (SSize_t)(int64_t)b, 0U);
    case 89: /* SV_LEN */
        return (uint64_t)sv_len((SV *)(uintptr_t)a);
    case 90: /* NEW_HVHV */
        return (uint64_t)(uintptr_t)(SV *)newHVhv((HV *)(uintptr_t)a);
    case 91: { /* AV_MAKE (s+b = packed u32 tokens) */
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
    case 92: /* LOOKS_LIKE_NUMBER */
        return looks_like_number((SV *)(uintptr_t)a) ? 1 : 0;
    case 93: /* SV_AMAGIC */
        return SvAMAGIC((SV *)(uintptr_t)a) ? 1 : 0;
    case 94: { /* AMAGIC_CALL (a = method<<32 | sv, b = AMGf flags); the
                * right operand is fixed at &PL_sv_undef (the deref-overload
                * calling pattern). */
        SV *res = amagic_call((SV *)(uintptr_t)(uint32_t)a, &PL_sv_undef,
                              (int)(a >> 32), (int)(int64_t)b);
        return (uint64_t)(uintptr_t)res;
    }
    case 95: /* WARN */
        Perl_warn(aTHX_ "%s", s ? s : "");
        return 0;
    case 96: /* GET_HV */
        return (uint64_t)(uintptr_t)(SV *)get_hv(s ? s : "", (I32)(int64_t)a);
    case 97: { /* AV_STORE_RAW (b = idx<<32 | sv): direct AvARRAY slot write
                * with NO refcount side effects — the host's mirror-flush
                * primitive matching a native `AvARRAY(av)[i] = sv`. */
        AV *av = (AV *)(uintptr_t)a;
        SSize_t idx = (SSize_t)(int32_t)(b >> 32);
        if (SvTYPE((SV *)av) == SVt_PVAV && idx >= 0 && idx <= AvFILLp(av))
            AvARRAY(av)[idx] = (SV *)(uintptr_t)(uint32_t)b;
        return 0;
    }
    }
    return 0;
}
