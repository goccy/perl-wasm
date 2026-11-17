#!/bin/sh
# programs-and-pods.sh — perl's programs and the manual pages its build
# generates, made in the perl5 tree with the host miniperl (run from perl5,
# after the library is assembled).
#
# make would make them (the utilities target, pod/perltoc.pod and the pages
# before it), but its rules for them depend on miniperl's own objects, which
# a cross build does not have: they are made here by the commands of those
# rules. make-stdlib-zip.py ships the programs utils.lst names and the .pod
# files of pod/, as installperl installs them.
set -e

# The programs: utils/<name> from utils/<name>.PL, for every program of
# utils.lst that is made in utils/ (the others come from the modules they
# belong to, made with them: pod2usage, pod2man, ...).
for name in $(sed -n 's|^utils/\([^[:space:]#]*\).*|\1|p' utils.lst); do
  (cd utils && ../miniperl -I../lib "$name.PL")
done

# The manual pages the build generates (Makefile.SH: pod/perlintern.pod,
# pod/perlapi.pod, pod/perlmodlib.pod, pod/perl5440delta.pod, extra.pods,
# pod/perltoc.pod).
./miniperl -Ilib autodoc.pl
./miniperl -Ilib pod/perlmodlib.PL -q
version=$(./miniperl -Ilib -MConfig -e 'print "$Config{PERL_REVISION}$Config{PERL_VERSION}$Config{PERL_SUBVERSION}"')
cp pod/perldelta.pod "pod/perl${version}delta.pod"
for readme in $(grep -l '^=[a-z]' README.* | grep -v README.vms); do
  cp "$readme" "pod/perl${readme#README.}.pod"
done
./miniperl -Ilib pod/buildtoc -q
