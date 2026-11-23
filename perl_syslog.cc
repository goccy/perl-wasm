/* perl_syslog.cc — a message the system logger did not take is a Perl
 * exception.
 *
 * openlog, syslog, closelog and setlogmask are wasmify's (its
 * host_syslog.cc), as Sys::Syslog's "native" mechanism calls them. The
 * functions of syslog(3) return nothing, so a real one loses a message
 * silently when no logger takes it. Here the host says whether there is a
 * logger at all (an embedder has to enable it), and a program must be able
 * to tell: a message that cannot be delivered is a Perl exception, raised
 * from openlog or syslog, which call this. Sys::Syslog treats a failing
 * openlog as "this mechanism is not available" and goes on to its next one
 * (see the patch to connect_native in scripts/wasi-configure.sh).
 */
#include <EXTERN.h>
#include <perl.h>

#include <cerrno>
#include <cstring>

#include "wasmify_posix.h"

/* The text ends in a newline: it describes the system, not a line of the
 * program. */
extern "C" void __wasmify_syslog_undelivered(int error) {
    dTHX;
    if (error == ENOSYS) Perl_croak(aTHX_ "no system logger is available to this program\n");
    Perl_croak(aTHX_ "the system logger does not take messages: %s\n", strerror(error));
}
