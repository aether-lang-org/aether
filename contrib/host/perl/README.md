# contrib.host.perl — Embedded Perl

`import contrib.host.perl` lets an Aether program embed Perl
in-process: `aether_perl_run_sandboxed(perms, "<source>")` evaluates
Perl with permission-checked access controls.

## How it loads libperl

The bridge `dlopen`s libperl at the **deploy host's** runtime — there
is no `-lperl` on the link line. The produced binary works against
whatever Perl 5.x minor version the deploy host has (5.30 - 5.40
supported).

Discovery order at first `aether_perl_*` call:
1. `${AETHER_PERL_SONAME}` env var (orchestrator-supplied exact).
2. `libperl.so` (Debian-style unversioned symlink; present with
   libperl-dev or sometimes the runtime alone).
3. Fallback list: `libperl.so.5.40` down to `libperl.so.5.30`
   (Debian/Fedora versioned soname).

If none load, the bridge prints a clear stderr message naming the
env-var escape hatch.

## What `ae build` does for you automatically

When your program has `import contrib.host.perl`, `ae build`:
1. Links `libaether_host_perl.a` (the in-tree bridge) automatically.
2. Does NOT add any libperl link flags — the bridge dlopens libperl
   at runtime.

`ae build` errors with a clear actionable message if the bridge .a
hasn't been built: build the image with
`aether-build --with=perl --rebuild-image` (containerised) or
`make contrib MODULES=perl && make install-contrib` (installed
toolchain).

## `aether.toml` — usually empty for perl

```toml
# nothing required for contrib.host.perl
[[bin]]
name = "myapp"
path = "myapp.ae"
```

If the deploy host's Perl isn't discoverable via the default order,
set `AETHER_PERL_SONAME` in the build environment:

```sh
AETHER_PERL_SONAME=libperl.so.5.36 ae build myapp.ae
```

## Usage

```aether
import contrib.host.perl

main() {
    aether_perl_run("$| = 1; print \"hello from perl\\n\"")
}
```

Function names are prefixed `aether_perl_` to avoid collision with
Perl's own `perl_run`/`perl_init` symbols.

## Implementation notes

- All Perl C-API access goes through a `dlsym` function-pointer
  table — see `aether_host_perl.c`. The bridge .a has NO unresolved
  Perl symbols (`nm -u` confirms).
- Perl's headers use the `pTHX_` context-passing convention; macros
  like `eval_pv`, `SvTRUE(ERRSV)`, `SvPV_nolen(ERRSV)` expand to
  `Perl_*` functions taking the interpreter pointer as first arg.
  The bridge dlsyms those `Perl_*` functions directly and passes
  `my_perl` explicitly. **No SV-struct-layout assumptions** — all
  field access goes through the layout-agnostic `Perl_*` accessor
  functions (`Perl_sv_true`, `Perl_sv_2pv_flags`).
- `SV_GMAGIC` constant (= 2) is baked. Stable across Perl 5.x —
  same shape as Ruby's `Qnil`-as-literal in the ruby bridge.
- `ERRSV` macro replaced with `Perl_get_sv(my_perl, "@", 0)` which
  fetches the `$@` SV via Perl's symbol-table lookup (no
  `PL_errgv`-struct-field access).
