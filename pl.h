/* pl.h — thin Perl embedding API exported to wasm / Go.
 *
 * Named pl.h, NOT perl.h, so it does not collide with Perl's own <perl.h>,
 * which the bridge (perl.cc) includes alongside this. `wasmify parse-headers
 * --header pl.h` feeds this surface to the generator.
 *
 * This is the ONLY surface wasmify exports from libperl. Perl's full C API
 * (the perlapi / XS surface) stays internal; Go callers see just these
 * functions. Pinned against Perl v5.42.2.
 *
 * It is a C++ header (compiled into the wasmify bridge as C++): string OUTPUTS
 * use `std::string*`, matching the bridge generator's string-output handling,
 * and string INPUTS use `const char*` (the bridge passes `.c_str()`). The
 * interpreter handle is an opaque integer token (uint64), which keeps the
 * generator unambiguous (a pointer-to-opaque-struct parameter is otherwise
 * misread as an output param) and is the conventional FFI handle idiom.
 *
 * Threading model: one wasm instance == one PerlInterpreter == one handle.
 * Multiple interpreters == multiple wasm2go module instances, each with its
 * own linear memory. Perl is built -Dusemultiplicity (implicit interpreter
 * context, pTHX) but WITHOUT ithreads — the wasm target is single-threaded.
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

/* Evaluate `src` in the interpreter and return the result as a JSON string:
 *
 *   {"ok":<bool>,"result":<string>,"stdout":<string>,"stderr":<string>,
 *    "error":<string>}
 *
 * `src` is run via eval_pv (a string eval in scalar context). "result" holds
 * the stringification of the returned scalar. "stdout"/"stderr" hold anything
 * printed to STDOUT/STDERR during the eval (captured by reopening them onto
 * in-memory scalars around the call). On a Perl-level die (or a host interrupt,
 * see below), "ok" is false and "error" holds $@ (the contents of ERRSV).
 * Package/lexical-our state persists across calls on the same handle (REPL-like).
 *
 * A single JSON string return is used because the bridge generator surfaces
 * only one response value to Go; bundling the outputs keeps one round-trip and
 * one atomic result. The Go wrapper unmarshals it. */
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
 * the eval_pv trap, so perl_eval returns cleanly with ok=false and the message
 * in "error" — exactly like a Perl-level die.
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

/* ---- Go <-> Perl bridge --------------------------------------------------
 *
 * Values cross the boundary as JSON: scalars map to JSON strings/numbers,
 * array refs to arrays, hash refs to objects (encoded/decoded guest-side by
 * JSON::PP from the standard library, loaded lazily on first use). */

/* Call the named Perl subroutine in list context. `sub_name` is a fully
 * qualified sub name ("main::handler", "My::App::run") or a main:: sub name;
 * `args_json` is a JSON array holding the argument list (NULL/empty means no
 * arguments). Returns
 *
 *   {"ok":<bool>,"result":<array>,"error":<string>}
 *
 * where "result" is the sub's return list re-encoded as a JSON array. On a
 * Perl-level die (including "no such sub"), "ok" is false and "error" holds
 * $@. Unlike perl_eval, STDOUT/STDERR are NOT redirected: prints go to the
 * instance's WASI fds. */
std::string perl_call(uint64_t h, const char *sub_name, const char *args_json);

/* Register the Go-side dispatcher for this instance. `callback_id` is the id
 * the host returned when registering its callback handler; every Perl->Go
 * call from this interpreter is routed to it. Perl code reaches Go through
 * main::__plwasm_go_invoke($func_id, $payload_json) — an XS installed by
 * perl_new that forwards to the wasmify callback import — and the
 * main::__plwasm_go_call glue that wraps it with JSON encode/decode (the
 * host binds a named Perl sub to a Go function by eval'ing a one-line sub
 * that calls the glue with the Go function's id). */
void perl_set_go_dispatcher(uint64_t h, int32_t callback_id);

#endif /* PERLEMBED_H */
