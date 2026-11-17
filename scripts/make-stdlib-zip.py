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

perl's manual pages (the perl5/pod/ source directory, with the pages the
build generates: perlapi.pod, perlmodlib.pod, perluniprops.pod, ...) are
shipped under pod/, where perl installs them (installperl: <privlib>/pod):
the diagnostics pragma (and splain) read the explanations of perl's messages
from pod/perldiag.pod, perldoc finds perlfunc.pod there, and a program that
browses the documentation of the installed perl looks there for perl.pod and
the pages it names.

perl's programs (prove, perldoc, pod2usage, enc2xs and the others utils.lst
names, which installperl installs into the directory of the perl command)
are shipped under bin/, with the #! line the build gave them: the embedding
application puts them beside the path that runs it as the perl command,
with that path on their #! line, and %Config names that directory (see
below).

The *.pod files perl installs into its library directory next to the modules
ARE kept (Config.pod, CORE.pod, perlfaq*.pod, ...): they are part of an
installed perl's library, and programs look for them there (a module's
documentation is found beside its .pm).

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

The name of the architecture is the host's: one interpreter runs on every
host the embedding application is built for, and the native modules a
program loads are built for the host. The interpreter is started with it
(Internals::archname, see perl.cc), and the shipped Config.pm and
Config_heavy.pl report it as archname and myarchname. The site's
architecture directory is named after it: the build configures sitearch as
sitelib/archname, and without site directories the shipped files report
the build's sitelib with the run-time name below it (the build's own name,
which config.sh holds, is not shown).

The path of the perl command (perlpath, and startperl, the #! line for a
program that is installed) is $^X when that is an absolute path: the
embedding application sets $^X to what runs it as perl, if anything does.
The directory of the perl command and of perl's programs (bin, scriptdir:
the directory of $^X, as perl's relocatable installations have it) is then
the directory $^X is in; what is installed goes to the site's directory of
programs (installbin, installscript), as perl's own library does.

An embedding application that runs the interpreter for a build on another
system than the interpreter's (a native module built with the host's C
compiler) gives %Config the values of that system in %Config::Override
before Config.pm is loaded (from a preload that loads no module): Config.pm
and Config_heavy.pl report them in every view of %Config (the hash, the
text of config_sh, config_re and config_vars, myconfig, perl -V) in place of
the values above, and perl -V says the library was built for the system
%Config names (osname).

The error numbers of Errno are those the program sees. Errno.pm holds the
numbers of the C library perl was built with; a program that is shown the
numbers of the system it runs on (see perl_numbers.cc) gets that system's
names and numbers there too: the shipped Errno.pm asks the interpreter for
them (Internals::host_numbers) when it is loaded. Errno.pm also dies when
%Config's archname and osvers are not the build's; at run time archname is
the host's name of the architecture, and the archive ships with the
interpreter whose C library the table is of, so the shipped Errno.pm does
not compare them. See present_errno.

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

# The list of perl's programs, by their path under perl5 (installperl's).
PROGRAMS_LIST = "utils.lst"

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

# The perl command is the path $^X names, when it names one: perl's
# programs are beside it (scriptdir), as in perl's relocatable installations.
my $bindir;
if ($^X =~ m{\\A/}) {
    ($bindir = $^X) =~ s{/+[^/]*\\z}{};
    $bindir = '/' if $bindir eq '';
}

# The values an embedding application gave in place of the build's, by its
# preload, before this file was loaded.
our %Override;

# The site directories are the ones the interpreter was started with, when
# it was started with any.
my %site = Internals::site_directories();

# The name of the architecture is the host's, which the interpreter was
# started with.
my $archname = Internals::archname();

"""

# What Config_heavy.pl does to the text of config.sh it holds in $_, right
# after it has it: the library directories become the one Config.pm found.
# A value is written in the form the text has for it — in single quotes, or,
# for a directory with a single quote in its name, in double quotes, which
# the reader of the text evaluates as a Perl string.
CONFIG_HEAVY_LIBDIR = """\
# The library directories are where the library is at run time: the
# directory this file was loaded from, which is the one Config.pm was loaded
# from. (%Config is not asked: this file may be loaded while a method of the
# tied hash runs, and the hash reads as empty then.)
{
    my $libdir = __FILE__;
    $libdir =~ s{/+[^/]*\\z}{} or $libdir = '.';
    $libdir = '/' if $libdir eq '';
    my $quoted = $libdir =~ /'/ ? '"' . quotemeta($libdir) . '"' : "'$libdir'";
    s/^((?:install)?(?:priv|arch)lib(?:exp)?)=.*$/$1=$quoted/mg;
}
# The site directories are the ones the interpreter was started with, when
# it was started with any: it names every key that is one of them.
{
    my $set = sub {
        my ($key, $value) = @_;
        $value = 'undef' unless defined $value;
        my $quoted = $value =~ /'/ ? '"' . quotemeta($value) . '"' : "'$value'";
        s/^\\Q$key\\E=.*$/$key=$quoted/m or $_ .= "$key=$quoted\\n";
    };
    my %site = Internals::site_directories();
    for my $key (keys %site) {
        $set->($key, $site{$key});
    }
    # The name of the architecture is the host's, which the interpreter was
    # started with; the site's architecture directory is named after it
    # (sitearch is sitelib/archname) also when no site directory was given.
    my $archname = Internals::archname();
    $set->('archname', $archname);
    $set->('myarchname', $archname);
    unless (%site) {
        for my $kind (['sitearch', 'sitelib'], ['sitearchexp', 'sitelibexp'],
                      ['installsitearch', 'installsitelib']) {
            my ($arch, $lib) = @$kind;
            my ($value) = /^\\Q$lib\\E='([^']*)'$/m;
            $set->($arch, "$value/$archname") if defined $value;
        }
    }
    # What is installed goes to the site directories, also what perl would
    # install beside its own library: the library directory is not a place
    # to install into.
    $set->('installprivlib', $site{installsitelib}) if defined $site{installsitelib};
    $set->('installarchlib', $site{installsitearch}) if defined $site{installsitearch};
    # The perl command is the one this interpreter says it can be run by,
    # and perl's programs are beside it; what is installed goes to the site.
    if ($^X =~ m{\\A/}) {
        $set->('perlpath', $^X);
        $set->('startperl', "#!$^X");
        (my $bindir = $^X) =~ s{/+[^/]*\\z}{};
        $bindir = '/' if $bindir eq '';
        # ($_ is the text the values are set in)
        for my $key (qw(bin binexp scriptdir scriptdirexp)) { $set->($key, $bindir) }
        $set->('installbin', $site{installsitebin}) if defined $site{installsitebin};
        $set->('installscript', $site{installsitescript}) if defined $site{installsitescript};
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
    # The values an embedding application gave in place of the build's,
    # which Config.pm reports too.
    for my $key (sort keys %Config::Override) { $set->($key, $Config::Override{$key}) }
}
"""

LIBDIR_KEYS = ("archlibexp", "privlibexp")
# The keys of the architecture directories and of the library directory
# each is below, which the build configures as <library>/<archname>.
SITEARCH_KEYS = (("sitearch", "sitelib"), ("sitearchexp", "sitelibexp"),
                 ("installsitearch", "installsitelib"))


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

# Errno_pm.PL's comparison of %Config's architecture and system version
# with the build's (see the module docstring for why it is left out).
ERRNO_PM_ARCH_CHECK = (
    r"^\"\$Config\{'archname'\}-\$Config\{'osvers'\}\" eq\n"
    r"\"[^\"\n]*\" or\n"
    r"\tdie \"Errno architecture \([^\n]*\) does not match executable architecture "
    r"\(\$Config\{'archname'\}-\$Config\{'osvers'\}\)\";\n"
)


def present_errno(text: str) -> str:
    """Returns Errno.pm with the table taken from the interpreter when it
    shows the program the numbers of another system, and without the
    comparison of %Config's architecture with the build's (see the module
    docstring)."""
    text = replace_once(text, ERRNO_PM_ARCH_CHECK, "", "Errno.pm: the architecture check")
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
        archname = re.search(r"^    archname => '([^'\n]*)',$", text, flags=re.M)
        sitelib = re.search(r"^    sitelibexp => '([^'\n]*)',$", text, flags=re.M)
        sitearch = re.search(r"^    sitearchexp => '([^'\n]*)',$", text, flags=re.M)
        if archname is None or sitelib is None or sitearch is None:
            raise SystemExit("make-stdlib-zip: Config.pm: archname, sitelibexp or sitearchexp was not found")
        if sitearch.group(1) != sitelib.group(1) + "/" + archname.group(1):
            raise SystemExit("make-stdlib-zip: Config.pm: sitearchexp is not sitelibexp/archname")
        text = replace_once(text, r"^    archname => '[^'\n]*',$", "    archname => $archname,",
                            "Config.pm: archname")
        text = replace_once(text, r"^    sitelibexp => '[^'\n]*',$",
                            f"    sitelibexp => $site{{sitelibexp}} // '{sitelib.group(1)}',",
                            "Config.pm: sitelibexp")
        text = replace_once(text, r"^    sitearchexp => '[^'\n]*',$",
                            f"    sitearchexp => $site{{sitearchexp}} // '{sitelib.group(1)}/' . $archname,",
                            "Config.pm: sitearchexp")
        scriptdir = re.search(r"^    scriptdir => '([^'\n]*)',$", text, flags=re.M)
        if scriptdir is None:
            raise SystemExit("make-stdlib-zip: Config.pm: scriptdir was not found")
        text = replace_once(text, r"^    scriptdir => '[^'\n]*',$",
                            f"    scriptdir => $bindir // '{scriptdir.group(1)}',", "Config.pm: scriptdir")
        # The overriding values come last in the hash, so that they win.
        text = replace_once(text, r"^};\n(?![\s\S]*^};\n)", "    %Override,\n};\n",
                            "Config.pm: the end of the tied hash")
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
        archname = re.search(r"^archname='([^']*)'$", text[start:end], flags=re.M)
        if archname is None:
            raise SystemExit("make-stdlib-zip: Config_heavy.pl: archname was not found")
        for arch, lib in SITEARCH_KEYS:
            a = re.search(rf"^{arch}='([^']*)'$", text[start:end], flags=re.M)
            l = re.search(rf"^{lib}='([^']*)'$", text[start:end], flags=re.M)
            if a is None or l is None or a.group(1) != l.group(1) + "/" + archname.group(1):
                raise SystemExit(f"make-stdlib-zip: Config_heavy.pl: {arch} is not {lib}/archname")
        text = text[:end] + CONFIG_HEAVY_LIBDIR + text[end:]
        built = re.search(r"^    print \"  Built under (\w+)\\n\";$", text, flags=re.M)
        if built is None:
            raise SystemExit("make-stdlib-zip: Config_heavy.pl: the line of the system built under was not found")
        osname = built.group(1)
        text = replace_once(text, rf"^    die \"Perl lib was built for '{osname}' but is being run on '\$\^O'\"\n"
                                  rf"        unless \"{osname}\" eq \$\^O;$",
                            "    die \"Perl lib was built for '$Config{osname}' but is being run on '$^O'\"\n"
                            "        unless $Config{osname} eq $^O;",
                            "Config_heavy.pl: the comparison of the systems")
        text = replace_once(text, r"^    print \"  Built under \w+\\n\";$",
                            "    print \"  Built under $Config{osname}\\n\";", "Config_heavy.pl: built under")
        return text.encode("utf-8")
    return data


def programs(perl_dir: str) -> list[tuple[str, str]]:
    """Returns perl's programs as installperl installs them: each name and
    the file it is made of. utils.lst names a program by its path under
    perl5, or a program and a second name for it (`path # link = path`),
    one per line; a line that starts with # is a comment."""
    out = []
    with open(os.path.join(perl_dir, PROGRAMS_LIST)) as fh:
        for line in fh:
            line = line.rstrip("\n")
            if line.startswith("#") or not line.strip():
                continue
            m = re.match(r"(\S*)\s*#\s*link\s*=\s*(\S*)", line)
            if m:
                path = os.path.join(perl_dir, m.group(1))
                out.append((os.path.basename(m.group(1)), path))
                out.append((os.path.basename(m.group(2)), path))
            else:
                out.append((os.path.basename(line.strip()), os.path.join(perl_dir, line.strip())))
    return out


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

    # Each archive entry and the file it is made of.
    sources = {rel: os.path.join(lib_dir, rel) for rel in collect(lib_dir)}
    # The manual pages and the programs live beside lib/, in the source tree.
    perl_dir = os.path.dirname(os.path.abspath(lib_dir))
    pod_dir = os.path.join(perl_dir, "pod")
    for name in sorted(os.listdir(pod_dir)):
        if name.endswith(".pod"):
            if "pod/" + name in sources:
                sys.stderr.write(f"lib/ has pod/{name} too\n")
                return 1
            sources["pod/" + name] = os.path.join(pod_dir, name)
    for name, path in programs(perl_dir):
        if not os.path.isfile(path):
            sys.stderr.write(f"missing program: {path}\n")
            return 1
        if "bin/" + name in sources:
            sys.stderr.write(f"lib/ has bin/{name} too\n")
            return 1
        sources["bin/" + name] = path
    rels = sorted(sources)
    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for rel in rels:
            info = zipfile.ZipInfo(filename=rel, date_time=FIXED_DATE_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16  # regular file, rw-r--r--
            with open(sources[rel], "rb") as fh:
                data = fh.read()
            if sources[rel] == os.path.join(lib_dir, rel):
                data = relocate_config(rel, data)
            zf.writestr(info, data)
    sys.stderr.write(f"wrote {out_path}: {len(rels)} files\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
