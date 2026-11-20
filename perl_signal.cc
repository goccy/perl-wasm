/* perl_signal.cc — perl's side of the signals of a process that cannot be
 * interrupted.
 *
 * The signal layer itself — the dispositions, the mask, the signal sets,
 * delivery, the interval timer and the default actions — is wasmify's
 * (its host_signal.cc). perl's own C-level handler only notes a signal
 * and sets PL_sig_pending, and the interpreter runs the %SIG handler at
 * its next safe point (PERL_ASYNC_CHECK). What is perl's here: the host
 * stores 1 into PL_sig_pending when it records a signal (the word is
 * registered with the host), and the interpreter then calls
 * PL_signalhook, which takes the recorded signals in through wasmify's
 * layer — perl's handler queues them in PL_psig_pend — before perl
 * dispatches its handlers.
 */
#include <EXTERN.h>
#include <perl.h>

#include "wasmify_posix.h"

namespace {

/* PL_signalhook: called by the interpreter at a safe point when
 * PL_sig_pending is set — by perl's own C-level handler, or by the host.
 * It brings the host's recorded signals in (perl's handler, called by
 * wasmify's delivery, queues them in PL_psig_pend) and then lets perl
 * dispatch.
 *
 * Perl_despatch_signals clears PL_sig_pending before it runs the handlers.
 * A signal the host records after the take below but before that clear
 * would sit unnoticed, its flag wiped; so the host is asked once more when
 * the dispatch is over — also when a handler leaves it by dying, hence the
 * save-stack destructor. Whatever that second take brings in is queued and
 * sets PL_sig_pending again, for the next safe point. */
void absorb_destructor(pTHX_ void *) { __wasmify_signal_absorb(); }

void signal_hook(pTHX) {
    ENTER;
    SAVEDESTRUCTOR_X(absorb_destructor, NULL);
    __wasmify_signal_absorb();
    Perl_despatch_signals(aTHX);
    LEAVE;
}

} /* namespace */

extern "C" {

/* Connects the interpreter of THIS module instance to the host's signal
 * delivery: registers PL_sig_pending as the word the host raises, installs
 * the safe-point hook, and tells the host the dispositions. An instance
 * may be a copy of the one that did this before — made from a snapshot of
 * its memory, or by fork — for which the host knows none of it; so this is
 * called whenever the interpreter is entered, and the host answers at once
 * when it has nothing to learn. */
void wasi_signal_attach(pTHX) {
    PL_signalhook = signal_hook;
    if (__wasmify_host_proc_signal_register((void *)&PL_sig_pending) == 0)
        __wasmify_signal_report();
}

} /* extern "C" */
