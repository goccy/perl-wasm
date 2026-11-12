/* perlwasm_ext_names.h -- the default, for a directory that renames nothing.
 *
 * Every compile of the perl build force-includes a file of this name
 * (ccflags in scripts/wasi-configure.sh). The directories a bundled CPAN
 * distribution compiles in hold their own, which gives the distribution's
 * external symbols names of their own (scripts/ext-symbols.py); everywhere
 * else the include finds this one. */
