package Time::HiRes;

# Pure-Perl Time::HiRes emulation for the wasm32-wasi build.
#
# The real Time::HiRes is an XS module this build cannot link: wasi-libc's
# clockid_t is a struct pointer (CLOCK_REALTIME = &_CLOCK_REALTIME) while
# Time::HiRes moves clock ids through Perl as integers, so its
# clock_gettime/clock_getres are type-incompatible with the platform, and
# interval timers (setitimer/ualarm) do not exist under wasi at all. Leaving
# the module out entirely, though, breaks a large slice of CPAN - Mojolicious,
# Plack::Middleware::AccessLog, anything that opportunistically imports
# gettimeofday - so this file provides the portable subset in pure Perl.
#
# Clock source: POSIX::clock() scaled by the platform's CLOCKS_PER_SEC.
# Under wasi's emulated process clocks, clock() is backed by the monotonic
# clock and clock_t counts nanoseconds (CLOCKS_PER_SEC = 1e9) - the macro and
# the counter come from the same libc, so the pair is consistent by
# definition. (POSIX::times() was rejected: its tick rate is NOT the
# sysconf(_SC_CLK_TCK) it advertises under this emulation.) time() anchors
# the monotonic counter to the epoch second observed at module load, giving
# epoch time that advances monotonically with sub-microsecond resolution but
# up to one second of constant offset from the true epoch time.
#
# Deliberately absent, so feature probes fail cleanly instead of lying:
# clock_gettime, clock_getres, CLOCK_* constants, ualarm, setitimer,
# getitimer, and the *alarm interval timers. Callers that probe with
# eval { Time::HiRes::clock_gettime(...) } (Mojo::Util's steady-clock check,
# for one) fall back to Time::HiRes::time, which is exactly this emulation.

use strict;
use warnings;
use POSIX ();
use Exporter 'import';

our $VERSION = '1.9700';
our @EXPORT_OK = qw(
    usleep nanosleep sleep
    gettimeofday time tv_interval
);

my $PER_SEC = eval { &POSIX::CLOCKS_PER_SEC } || 1_000_000_000;
my $CLOCK0  = POSIX::clock();
my $T0      = CORE::time();

# _now is the one clock read; time() is its public name. Internal callers use
# _now because a bare time() inside this package is ambiguous with CORE::time.
sub _now {
    return $T0 + (POSIX::clock() - $CLOCK0) / $PER_SEC;
}

sub time { return _now() }

sub gettimeofday {
    my $t = _now();
    return $t unless wantarray;
    my $s = int($t);
    return ($s, int(($t - $s) * 1_000_000));
}

sub tv_interval {
    my ($t0, $t1) = @_;
    $t1 = [gettimeofday()] unless defined $t1;
    return ($t1->[0] - $t0->[0]) + ($t1->[1] - $t0->[1]) / 1_000_000;
}

# The sleep family rides on 4-arg select, which the wasi host implements via
# poll(2)-style clock subscriptions. Fractional seconds are honoured to the
# host's poll resolution.
sub sleep {
    my ($seconds) = @_;
    return CORE::sleep($seconds) if !defined $seconds || $seconds == int($seconds);
    select(undef, undef, undef, $seconds);
    return $seconds;
}

sub usleep {
    my ($usec) = @_;
    select(undef, undef, undef, ($usec || 0) / 1_000_000);
    return $usec;
}

sub nanosleep {
    my ($nsec) = @_;
    select(undef, undef, undef, ($nsec || 0) / 1_000_000_000);
    return $nsec;
}

1;
