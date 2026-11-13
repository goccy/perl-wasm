#!/usr/bin/env python3
"""Build the embeddable Perl standard-library zip from the assembled lib/ tree.

perl.wasm is a bare interpreter: the pure-Perl half of the standard library
(strict.pm, warnings.pm, Config.pm, the unicore/ Unicode tables, ...) lives on
disk, not in the wasm. This produces `perl_stdlib.zip`, a trimmed copy of the
assembled `perl5/lib/` tree that an embedding application (go-perl) unpacks at
runtime and passes to perl_new(lib_dir) as the @INC search path. It is published
as a release asset alongside perl.wasm and the wasm2go bundle and attested, so
consumers verify it the same way — and it is locked to the exact Perl version
perl.wasm was built from (the submodule is pinned at v5.44.0).

IMPORTANT: run this AGAINST THE ASSEMBLED lib/, i.e. AFTER `make` has run the
nonxs pm_to_blib step. A pristine checkout's lib/ holds only the ~73 core .pm;
the dual-life modules from cpan/ dist/ ext/ are copied in during the build, and
the generated Config.pm / Config_heavy.pl are written there too. Zipping a
pre-build lib/ silently ships an incomplete stdlib.

Trim (everything that is meaningless in the embedded, statically-linked build):
  - auto/**/*.a  — the XS static archives are already linked into perl.wasm;
    XSLoader::load short-circuits for static_ext, so the .a is never read.
  - auto/**/*.ld / auto/**/extralibs.ld — MakeMaker linker hints (build-only).
  - **/.exists — MakeMaker directory-stamp markers (build-only).
  - any pod/ tree — manual pages kept apart from the modules.
  - *.t and any t/ or test/ directory — module test suites.
  - *.bs — bootstrap stubs (unused for static ext).
Under unicore/ the archive holds what perl's own installperl installs: the
generated tables (*.pl, *.pm, the lib/ and To/ trees) and `version`, and of
the Unicode Character Database source files only the three a running perl
reads (Blocks.txt, SpecialCasing.txt, NamedSequences.txt). The other source
.txt files, the table generator and the property test script TestProp.pl
serve the build and its tests; a program that finds a UCD source file under
unicore/ takes that as an invitation to parse it.

One manual page is shipped, as pod/perldiag.pod: the diagnostics pragma (and
splain) read the explanations of perl's messages from that file in the
library directory.

The *.pod files perl installs into its library directory next to the modules
ARE kept (Config.pod, CORE.pod, perlfaq*.pod, ...): they are part of an
installed perl's library, and programs look for them there (a module's
documentation is found beside its .pm). The manual pages of the perl5/pod/
source directory (perlfunc.pod and the rest) are not in lib/ and so are not
shipped.

%Config names the library directory the library is in at run time. The
build configures perl for a library under /usr/lib/perl5, where nothing is:
the archive is unpacked wherever the embedding application puts it, on a
host filesystem or an in-memory one. So the shipped Config.pm takes the
directory it was loaded from for the library directory (privlibexp and
archlibexp: the archive holds both kinds of file in one tree), and the
shipped Config_heavy.pl gives the same answer for every spelling of the two
(privlib, privlibexp, installprivlib, archlib, archlibexp, installarchlib).
See relocate_config.

The site directories are not known before run time either: the embedding
application names the directory they are under when it starts the
interpreter, or names none. The interpreter answers for them
(Internals::site_directories: the %Config keys of the site directories it
was started with and their values, see perl.cc), and the shipped Config.pm
and Config_heavy.pl report what it says; without any, they keep the values
of the build's configuration. With site directories, they are also where
perl's own library would be installed (installprivlib, installarchlib): the
library directory is a copy the embedding application unpacked for itself,
which may be replaced as a whole, and what an installation writes beside the
library (ExtUtils::MakeMaker's perllocal.pod, under installarchlib) belongs
to the user's installations.

The path of the perl command (perlpath, and startperl, the #! line for a
program that is installed) is $^X when that is an absolute path: the
embedding application sets $^X to what runs it as perl, if anything does.

The error numbers of Errno are those the program sees. Errno.pm holds the
numbers of the C library perl was built with; a program that is shown the
numbers of the system it runs on (see perl_numbers.cc) gets that system's
names and numbers there too: the shipped Errno.pm asks the interpreter for
them (Internals::host_numbers) when it is loaded. See present_errno.

The archive is written deterministically (sorted entries, fixed 1980 timestamps,
fixed 0644 perms) so the same lib/ always yields byte-identical bytes.

Usage: make-stdlib-zip.py <perl5/lib dir> <output zip>
"""

import os
import re
import sys
import zipfile

# Directory names excluded at ANY depth (with everything beneath them).
ANYWHERE_DIR_EXCLUDES = {"t", "test", "tests", "pod"}

# installperl's rules for lib/unicore: the UCD source files that are
# installed, and the other names that are (`version`, and *.pl / *.pm except
# the property test script).
UNICORE_TXT = {"Blocks.txt", "SpecialCasing.txt", "NamedSequences.txt"}
UNICORE_OTHER = re.compile(r"\A(?:version|\w+\.p[lm])\Z")

# The manual page that ships, by its path under perl5/pod.
MANUAL_PAGES = ["perldiag.pod"]

# Fixed DOS timestamp (1980-01-01 00:00:00) for reproducible archives.
FIXED_DATE_TIME = (1980, 1, 1, 0, 0, 0)


def included(rel_path: str) -> bool:
    parts = rel_path.split("/")
    base = parts[-1]
    if any(p in ANYWHERE_DIR_EXCLUDES for p in parts[:-1]):
        return False
    if base == ".exists":
        return False
    if parts[0] == "unicore":
        if base.endswith(".txt"):
            return base in UNICORE_TXT
        return base != "TestProp.pl" and UNICORE_OTHER.match(base) is not None
    for ext in (".a", ".ld", ".bs", ".t"):
        if rel_path.endswith(ext):
            return False
    return True


# What Config.pm computes before it ties %Config: the directory it was
# loaded from.
CONFIG_PM_LIBDIR = """\
# The library is unpacked wherever the embedding application puts it: the
# library directories are the directory this file was loaded from.
my $libdir = __FILE__;
$libdir =~ s{/+[^/]*\\z}{} or $libdir = '.';
$libdir = '/' if $libdir eq '';

# The site directories are the ones the interpreter was started with, when
# it was started with any.
my %site = Internals::site_directories();

"""

# What Config_heavy.pl does to the text of config.sh it holds in $_, right
# after it has it: the library directories become the one Config.pm found.
# A value is written in the form the text has for it — in single quotes, or,
# for a directory with a single quote in its name, in double quotes, which
# the reader of the text evaluates as a Perl string.
CONFIG_HEAVY_LIBDIR = """\
# The library directories are where the library is at run time: the answer
# Config.pm found when it was loaded.
{
    my $libdir = $Config::Config{privlibexp};
    my $quoted = $libdir =~ /'/ ? '"' . quotemeta($libdir) . '"' : "'$libdir'";
    s/^((?:install)?(?:priv|arch)lib(?:exp)?)=.*$/$1=$quoted/mg;
}
# The site directories are the ones the interpreter was started with, when
# it was started with any: it names every key that is one of them.
{
    my $set = sub {
        my ($key, $value) = @_;
        my $quoted = $value =~ /'/ ? '"' . quotemeta($value) . '"' : "'$value'";
        s/^\\Q$key\\E=.*$/$key=$quoted/m;
    };
    my %site = Internals::site_directories();
    for my $key (keys %site) {
        $set->($key, $site{$key});
    }
    # What is installed goes to the site directories, also what perl would
    # install beside its own library: the library directory is not a place
    # to install into.
    $set->('installprivlib', $site{installsitelib}) if defined $site{installsitelib};
    $set->('installarchlib', $site{installsitearch}) if defined $site{installsitearch};
    # The perl command is the one this interpreter says it can be run by.
    if ($^X =~ m{\\A/}) {
        $set->('perlpath', $^X);
        $set->('startperl', "#!$^X");
    }
    # The signals have the numbers the program is shown, when those are
    # another system's than the build's: the names in the order of their
    # numbers (a number without a name is NUMnn), then the second names of
    # numbers that have two, as Configure lists them.
    my @signals = Internals::host_numbers('signal');
    my %built;
    {
        my @names = /^sig_name='(.*)'$/m ? split(' ', $1) : ();
        my @numbers = /^sig_num='(.*)'$/m ? split(' ', $1) : ();
        @built{@names} = @numbers;
    }
    my %shown;
    for (my $i = 0; $i < @signals; $i += 2) {
        (my $name = $signals[$i]) =~ s/\\ASIG//;
        $shown{$name} = $signals[$i + 1];
    }
    # (a system that numbers them as the build does keeps the build's list,
    # which has the real-time signals too)
    if (grep { exists $built{$_} && $built{$_} != $shown{$_} } keys %shown) {
        my (@name, @alias);
        while (my ($name, $number) = splice(@signals, 0, 2)) {
            $name =~ s/\\ASIG//;
            if (defined $name[$number]) { push @alias, [$name, $number] }
            else { $name[$number] = $name }
        }
        $name[0] = 'ZERO';
        my @names = map { $name[$_] // "NUM$_" } 0 .. $#name;
        my @numbers = (0 .. $#name);
        push @names, map { $_->[0] } @alias;
        push @numbers, map { $_->[1] } @alias;
        $set->('sig_name', join(' ', @names) . ' ');
        $set->('sig_num', join(' ', @numbers) . ' ');
        $set->('sig_name_init', join(', ', (map { qq{"$_"} } @names), 0));
        $set->('sig_num_init', join(', ', @numbers, 0));
        $set->('sig_count', scalar @name);
        $set->('sig_size', scalar @names);
    }
}
"""

LIBDIR_KEYS = ("archlibexp", "privlibexp")
# The site directories among the values Config.pm holds itself.
SITE_KEYS = ("sitearchexp", "sitelibexp")


def replace_once(text: str, pattern: str, replacement: str, what: str) -> str:
    """Replaces the one match of pattern; anything but one match is an error:
    the generated file is not the one this script knows."""
    result, count = re.subn(pattern, lambda _m: replacement, text, flags=re.M)
    if count != 1:
        raise SystemExit(f"make-stdlib-zip: {what}: {count} matches, expected 1")
    return result


# What Errno.pm does once it has the table of the build: the table of the
# system the program is shown the numbers of takes its place, when there is
# one.
ERRNO_PM_HOST = """\
    # The names and numbers of the system whose numbers the program is
    # shown, when the interpreter shows it another system's than those of
    # the C library it was built with (the table above).
    if (my %host = Internals::host_numbers('errno')) {
        %err = %host;
    }
"""

# The names of the POSIX tag are those the table has.
ERRNO_PM_TAGS = """\
@{ $EXPORT_TAGS{POSIX} } = grep { exists $err{$_} } @{ $EXPORT_TAGS{POSIX} };

"""


def present_errno(text: str) -> str:
    """Returns Errno.pm with the table taken from the interpreter when it
    shows the program the numbers of another system (see the module
    docstring)."""
    start = text.find("\nBEGIN {\n    %err = (\n")
    end = text.find("\n    );\n", start)
    if start < 0 or end < 0:
        raise SystemExit("make-stdlib-zip: Errno.pm: the table was not found")
    end += len("\n    );\n")
    text = text[:end] + ERRNO_PM_HOST + text[end:]
    return replace_once(text, r"^sub TIEHASH \{ bless \\%err \}$",
                        ERRNO_PM_TAGS + "sub TIEHASH { bless \\%err }", "Errno.pm: TIEHASH")


def relocate_config(rel_path: str, data: bytes) -> bytes:
    """Returns the content to ship for a file of lib/: Config.pm and
    Config_heavy.pl with the library directories taken from where the
    library is (see the module docstring), every other file as it is."""
    if rel_path == "Config.pm":
        text = data.decode("utf-8")
        text = replace_once(text, r"^tie %Config, 'Config', \{$",
                            CONFIG_PM_LIBDIR + "tie %Config, 'Config', {", "Config.pm: the tie")
        for key in LIBDIR_KEYS:
            text = replace_once(text, rf"^    {key} => '[^'\n]*',$", f"    {key} => $libdir,",
                                f"Config.pm: {key}")
        for key in SITE_KEYS:
            match = re.search(rf"^    {key} => ('[^'\n]*'),$", text, flags=re.M)
            if match is None:
                raise SystemExit(f"make-stdlib-zip: Config.pm: {key} was not found")
            text = replace_once(text, rf"^    {key} => '[^'\n]*',$",
                                f"    {key} => $site{{{key}}} // {match.group(1)},", f"Config.pm: {key}")
        return text.encode("utf-8")
    if rel_path == "Errno.pm":
        return present_errno(data.decode("utf-8")).encode("utf-8")
    if rel_path == "Config_heavy.pl":
        text = data.decode("utf-8")
        # The text of config.sh is the here-document assigned to $_ through
        # the alias made just before it.
        start = text.find("local *_ = \\my $a;\n$_ = <<'!END!';\n")
        if start < 0 or text.find("local *_ = \\my $a;", start + 1) >= 0:
            raise SystemExit("make-stdlib-zip: Config_heavy.pl: the config.sh text was not found")
        end = text.find("\n!END!\n", start)
        if end < 0:
            raise SystemExit("make-stdlib-zip: Config_heavy.pl: the config.sh text does not end")
        end += len("\n!END!\n")
        return (text[:end] + CONFIG_HEAVY_LIBDIR + text[end:]).encode("utf-8")
    return data


def collect(lib_dir: str) -> list[str]:
    out = []
    for root, _dirs, files in os.walk(lib_dir):
        for name in files:
            rel = os.path.relpath(os.path.join(root, name), lib_dir)
            rel = rel.replace(os.sep, "/")
            if included(rel):
                out.append(rel)
    out.sort()
    return out


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: make-stdlib-zip.py <perl5/lib dir> <output zip>\n")
        return 2
    lib_dir, out_path = sys.argv[1], sys.argv[2]
    if not os.path.isdir(lib_dir):
        sys.stderr.write(f"not a directory: {lib_dir}\n")
        return 1

    sources = {rel: lib_dir for rel in collect(lib_dir)}
    # The manual pages live beside lib/, in the source tree's pod/.
    perl_dir = os.path.dirname(os.path.abspath(lib_dir))
    for page in MANUAL_PAGES:
        if not os.path.isfile(os.path.join(perl_dir, "pod", page)):
            sys.stderr.write(f"missing manual page: {os.path.join(perl_dir, 'pod', page)}\n")
            return 1
        sources["pod/" + page] = perl_dir
    rels = sorted(sources)
    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for rel in rels:
            info = zipfile.ZipInfo(filename=rel, date_time=FIXED_DATE_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16  # regular file, rw-r--r--
            with open(os.path.join(sources[rel], rel), "rb") as fh:
                data = fh.read()
            if sources[rel] == lib_dir:
                data = relocate_config(rel, data)
            zf.writestr(info, data)
    sys.stderr.write(f"wrote {out_path}: {len(rels)} files\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
