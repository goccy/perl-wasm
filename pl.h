/* pl.h — thin Perl embedding API exported to wasm / Go.
 *
 * Named pl.h, NOT perl.h, so it does not collide with Perl's own <perl.h>,
 * which the bridge (perl.cc) includes alongside this. `wasmify parse-headers
 * --header pl.h` feeds this surface to the generator.
 *
 * This is the ONLY surface wasmify exports from libperl. Perl's full C API
 * (the perlapi / XS surface) stays internal; Go callers see just these
 * functions. Pinned against Perl v5.44.0.
 *
 * It is a C++ header (compiled into the wasmify bridge as C++): string OUTPUTS
 * use `std::string`, matching the bridge generator's string-output handling,
 * and string/byte INPUTS use `const char*` (the bridge passes `.c_str()`,
 * which points at the FULL decoded buffer — embedded NUL bytes survive — so
 * every binary input parameter travels with an explicit length parameter).
 * The interpreter handle is an opaque integer token (uint64), which keeps the
 * generator unambiguous (a pointer-to-opaque-struct parameter is otherwise
 * misread as an output param) and is the conventional FFI handle idiom.
 *
 * Threading model: one wasm instance == one PerlInterpreter == one handle.
 * Multiple interpreters == multiple wasm2go module instances, each with its
 * own linear memory. Perl is built -Dusemultiplicity (implicit interpreter
 * context, pTHX) but WITHOUT ithreads — the wasm target is single-threaded.
 *
 * ---- Typed value protocol ------------------------------------------------
 *
 * Every value crossing the boundary is a TYPED BINARY NODE (little-endian,
 * packed). Nothing is stringified and nothing rides JSON: scalars cross by
 * value with their kind, references cross by handle.
 *
 *   node   := tag:u8 payload
 *     0 undef      —
 *     1 bool       u8 (0/1)
 *     2 int        i64
 *     3 float      f64 (IEEE 754 bits)
 *     4 string     u8 utf8_flag, u32 len, len bytes (the raw PV; utf8_flag
 *                  mirrors SvUTF8 so the encoding round-trips)
 *     5 ref        u64 handle, u8 refkind, u16 class_len, class bytes
 *                  (class only when blessed; refkind: 0 SCALAR, 1 ARRAY,
 *                  2 HASH, 3 CODE, 4 GLOB, 5 IO, 6 FORMAT, 7 REGEXP,
 *                  8 OTHER)
 *     6 hostfunc   u32 id — host->guest only; decodes to a Perl closure
 *                  dispatching back to the host function bound under id
 *     7 flatten    u64 handle — host->guest only, argument lists only: the
 *                  handle's ARRAY/HASH dereferences and its CONTENTS are
 *                  pushed onto the argument list (Perl's own list-flattening
 *                  calling convention)
 *
 *   list   := count:u32 node*
 *
 * References are never serialised: the guest pins the actual referent in a
 * per-interpreter registry (refcount held, id deduplicated by referent
 * address so the same reference always gets the same id) and only the id
 * crosses. Sending a handle back dereferences to THE SAME SV, so identity,
 * aliasing, and blessedness survive round trips. Each ref node handed to the
 * host carries one pin the HOST owns; it releases pins via perl_release.
 *
 * Every operation below that returns std::string returns a RESULT ENVELOPE:
 *
 *   envelope := status:u8 payload
 *     0 ok    payload is the operation's result (see each function)
 *     1 die   u32 len, len bytes of $@'s text
 *     2 exit  i32 exit status (a guest exit() caught cleanly; the
 *             interpreter unwound back to the call frame and stays
 *             flushable/destructible)
 */
#ifndef PERLEMBED_H
#define PERLEMBED_H

#include <cstdint>
#include <string>

/* Initialize a Perl interpreter and return an opaque handle (0 on failure).
 * Call once per wasm instance.
 *
 * Internally: PERL_SYS_INIT3 (once per process), perl_alloc, perl_construct,
 * perl_parse with a minimal bootstrap (`-e 0`) so the interpreter is ready to
 * eval, then installs the interruptible run loop (see below) by overwriting
 * PL_runops. `lib_dir`, when non-empty, is prepended to @INC so pure-Perl
 * modules can be found on the runtime's WASI filesystem mount; pass NULL/empty
 * to run with only the built-in @INC. */
uint64_t perl_new(const char *lib_dir);

/* Evaluate `src` (a string eval in scalar context, REPL-like state
 * persistence). ok payload:
 *
 *   result:node stdout:(u32 len + bytes) stderr:(u32 len + bytes)
 *
 * A die envelope (status 1) is followed by the same stdout/stderr pair —
 * what the eval printed before dying is still delivered. */
std::string perl_eval(uint64_t h, const char *src);

/* Destroy the interpreter (perl_destruct + perl_free). PERL_SYS_TERM runs at
 * process teardown, not here, so the handle is fully torn down but the process
 * stays usable. */
void perl_close(uint64_t h);

/* ---- Interruption support ------------------------------------------------
 *
 * Lets a host watchdog goroutine abort a runaway eval (e.g. `while(1){}`)
 * WITHOUT executing any wasm/C code on that instance — which would corrupt the
 * shared linear-memory C stack. Perl gives us a cleaner hook than CPython's
 * eval-breaker: the run loop is pluggable (PL_runops). perl_new installs a
 * custom loop that, on every opcode, tests a host-writable flag word and, when
 * set, calls Perl_croak("Perl execution interrupted"). The croak longjmps to
 * the eval trap, so the eval returns cleanly with a die envelope — exactly
 * like a Perl-level die.
 *
 * To interrupt, the host performs a single plain 32-bit store into linear
 * memory (no call into the instance):
 *
 *   *(uint32_t *)perl_interrupt_addr(h) = 1;
 *
 * The guest clears the flag when it fires. Addresses are 32-bit linear-memory
 * offsets (wasm32). Like CPython's eval-breaker, this only fires at opcode
 * boundaries: a single long-running op (a pathological regex, a big sort) is
 * not preempted until it returns to the run loop. */
uint32_t perl_interrupt_addr(uint64_t h); /* &interrupt flag word (write 1 to trip) */

/* Call the named Perl subroutine in list context. `sub_name` is a fully
 * qualified sub name ("main::handler", "My::App::run") or a main:: sub name;
 * `args`/`args_len` is a node list (empty means no arguments). ok payload:
 * the return list as a node list. Unlike perl_eval, STDOUT/STDERR are NOT
 * redirected: prints go to the instance's WASI fds. */
std::string perl_call(uint64_t h, const char *sub_name,
                      const char *args, uint32_t args_len);

/* Call the CODE reference behind `code` (a ref handle). want_scalar selects
 * the calling context: 0 = list context (ok payload: node list), 1 = scalar
 * context (ok payload: node list holding the single result). */
std::string perl_invoke(uint64_t h, uint64_t code, uint32_t want_scalar,
                        const char *args, uint32_t args_len);

/* Invoke $obj->method(args...) in list context on the reference behind
 * `obj`, dispatched by Perl's own method resolution (inheritance, AUTOLOAD).
 * ok payload: the return list as a node list. */
std::string perl_method_call(uint64_t h, uint64_t obj, const char *method,
                             const char *args, uint32_t args_len);

/* Dereference the SCALAR (or REF) reference behind `ref`: $$ref. ok payload:
 * one node (a ref-to-ref yields a fresh ref node). ARRAY/HASH/CODE handles
 * need no guest call to view — their operations below take the ref handle
 * directly. */
std::string perl_deref(uint64_t h, uint64_t ref);

/* Array operations. Every `av` parameter is a ref handle whose referent is
 * an ARRAY; ties and overloads run like ordinary Perl code, and a die
 * surfaces as a die envelope. */
std::string perl_array_len(uint64_t h, uint64_t av);            /* ok: i64 */
std::string perl_array_get(uint64_t h, uint64_t av, int64_t idx);  /* ok: node */
std::string perl_array_set(uint64_t h, uint64_t av, int64_t idx,
                           const char *val, uint32_t val_len);  /* val: one node; ok: empty */
std::string perl_array_push(uint64_t h, uint64_t av,
                            const char *vals, uint32_t vals_len); /* vals: node list; ok: empty */
std::string perl_array_values(uint64_t h, uint64_t av);         /* ok: node list */

/* Hash operations. Every `hv` parameter is a ref handle whose referent is a
 * HASH. Keys travel as ONE string node (tag 4), so byte keys and utf8 keys
 * both round-trip exactly. */
std::string perl_hash_get(uint64_t h, uint64_t hv,
                          const char *key, uint32_t key_len);   /* ok: u8 exists + node */
std::string perl_hash_set(uint64_t h, uint64_t hv,
                          const char *key, uint32_t key_len,
                          const char *val, uint32_t val_len);   /* ok: empty */
std::string perl_hash_delete(uint64_t h, uint64_t hv,
                             const char *key, uint32_t key_len); /* ok: empty */
std::string perl_hash_keys(uint64_t h, uint64_t hv);            /* ok: node list of strings */

/* Materialise a fresh array/hash in the guest from a node list and return a
 * ref node to it (ok payload: one node). perl_new_hash's list alternates
 * key nodes and value nodes. */
std::string perl_new_array(uint64_t h, const char *vals, uint32_t vals_len);
std::string perl_new_hash(uint64_t h, const char *pairs, uint32_t pairs_len);

/* Release registry pins: `ids` is a packed array of u64 handle ids (host
 * finalizer queue drains through one call). ok payload: empty. */
std::string perl_release(uint64_t h, const char *ids, uint32_t ids_len);

/* Register the Go-side dispatcher for this instance. `callback_id` is the id
 * the host returned when registering its callback handler; every Perl->Go
 * call from this interpreter is routed to it. Perl code reaches Go through
 * closures over main::__plwasm_go_call (an XS installed by perl_new that
 * speaks the node protocol over the wasmify callback import); the host binds
 * a named Perl sub to a Go function by eval'ing a one-line sub that calls
 * the glue with the Go function's id. */
void perl_set_go_dispatcher(uint64_t h, int32_t callback_id);

/* ---- Native XS support ---------------------------------------------------
 *
 * Lets the host register XSUBs whose implementation lives OUTSIDE the wasm -
 * in a host-native shared library the embedder dlopen'd (go-perl's native XS
 * SDK). Perl-side each such sub is an ordinary XS backed by a generic thunk
 * that forwards the call over the wasmify callback import with the reserved
 * method id -1: payload [u32 fn_id][u32 cv_token][u32 items][u32
 * sv_tokens...], response [u8 ok] then on success [u32 nret][u32
 * sv_tokens...] or on failure the croak message. The host runs the native
 * XSUB (real XSUBs receive their own CV, hence the cv token), using
 * perl_xs_helper for every SV operation, and the thunk pushes the returned
 * (mortal) SVs.
 *
 * A second reserved method id, -2, flows the OTHER way on teardown: freeing
 * a guest SV that carries goperl anchor magic (op SV_MAGIC_ATTACH) sends
 * the u32 host magic id so the host can run the native module's svt_free
 * and drop its mirror MAGIC chain.
 *
 * Reserved method id -3 carries pp hooks: for op types a native module
 * claimed (op PP_HOOK_SET), the run loop sends [u32 op][u32 op_type][u32 n]
 * [u32 stack-top tokens] instead of executing the pp, and the host answers
 * [1][u32 next_op] (or [0]+message to croak). The hook runs the true pp
 * itself through op RUN_ORIGINAL — the guest PL_ppaddr is never patched.
 *
 * Reserved method id -4 fires save-stack destructors (op SAVE_DESTRUCTOR):
 * the u32 host id is delivered when the guest scope holding it pops,
 * during normal exit and die unwinding alike.
 *
 * Reserved method id -5 fires host-side set-magic: when a guest SV whose
 * anchor magic was upgraded via op SV_MAGIC_SET_HOOK is assigned to, the
 * u32 host magic id is delivered so the host can run the svt_set hooks of
 * its mirror MAGIC chain. Anchors start set-less; the host upgrades one
 * only when a mirror entry actually carries svt_set. */

/* Bind the generic native thunk as the Perl sub `name`; fn_id is the host's
 * key for the actual native function (stored in the CV's XSANY). */
void perl_register_native_xs(uint64_t h, const char *name, int32_t fn_id);

/* SV micro-operations the host-side SDK vtable is built from. The full op
 * table lives in perl.cc (and mirrors the GOPERL_OP_* enum in the go-perl
 * native SDK header); representative entries:
 *   1  SV_IV       a=sv                  -> IV (as u64)
 *   2  SV_PV       a=sv                  -> (ptr<<32)|len into linear memory
 *   3  NEW_IV      a=iv                  -> new SV token
 *   4  NEW_PVN     s=bytes b=len         -> new SV token
 *   5  SV_MORTAL   a=sv                  -> sv (now mortal)
 *   ...
 *   23-64          sv_setsv/setiv/setnv/catsv families, NV ops, AV/HV entry
 *                  and iterator ops, ENTER/LEAVE/SAVETMPS/FREETMPS
 *   65 CALL_SV     a=flags<<32|sv, s+b=packed u32 arg tokens
 *                  -> died<<63 | count<<32 | mortal-AV token of results
 *   66 CALL_METHOD like CALL_SV but s = "name\0" + packed tokens
 *   68 SAVE_OP     scope-save PL_op (the SAVEOP() macro)
 *   69 RUN_PP_SCRATCH a=op_flags<<32|op_type, s+b=arg tokens: run one pp
 *                  function on a scratch OP in list context
 *   70-72          host-side MAGIC anchor attach / id lookup / unattach
 *   73-74,80       PL_* interpreter-variable get/set and local'ised hooks
 *   98-100         pp-hook table, RUN_ORIGINAL, save-stack destructors
 *   101-116        OP/COP/CV/GV/stash/context-stack introspection (host
 *                  shadow materialization for interpreter-hooking XS)
 *   117-127        gv_fetchfile, hv_clear/hv_delete, save_scalar, eval_pv,
 *                  newCONSTSUB, raw SvPVX, and friends
 *   128-133        the Class::MOP/Moose surface: stash mro pkg_gen, raw
 *                  refcount, gv_init, Gv_AMG / SvAMAGIC on-off, set-magic
 *                  anchor upgrade
 *   134-163        the CPAN-web surface (Cpanel::JSON::XS, HTML::Parser,
 *                  Time::Moment, DBI/DBD): method resolution, klen-true
 *                  hv store/fetch, eval_sv, SvPV_force/sv_chop/sv_insert,
 *                  utf8 decode/downgrade, guarded set-magic, caller
 *                  context, ckWARN, buffer sizes (SvLEN/SvCUR raw),
 *                  hv_delete_ent, av pop/shift, sv_2io + PerlIO handle
 *                  I/O, sv_force_normal, cop hints, real sv_magic for
 *                  behavioral (tie) magic
 * Unknown ops return 0. */
uint64_t perl_xs_helper(uint64_t h, int32_t op, uint64_t a, uint64_t b, const char *s);

#endif /* PERLEMBED_H */
