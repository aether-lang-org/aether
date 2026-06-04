# contrib.host.ruby — Embedded CRuby

`import contrib.host.ruby` lets an Aether program embed CRuby (MRI)
in-process: `ruby.run_sandboxed(perms, "<source>")` evaluates Ruby
inside the calling process, with permission-checked access controls.

## How it loads libruby

The bridge `dlopen`s libruby at the **deploy host's** runtime — there
is no `-lruby` on the link line. This means:

- The build environment doesn't need `ruby-dev` (or any libruby at
  all). Only the headers, which are pulled in via the
  `aether-build --with=ruby` image layer.
- The produced binary works against whatever Ruby minor version the
  deploy host has — no ABI lock-in.
- A binary built today on a 3.1 machine and run on a 3.2 machine
  works as long as the host has a usable libruby3.

Discovery order at first call to `ruby.run`:

1. `${AETHER_RUBY_SONAME}` env var (orchestrator-supplied exact
   soname, e.g. `libruby-3.1.so` or `libruby.so.3.1`).
2. `libruby.so` (Fedora/Bazzite-style unversioned symlink).
3. Fallback list: `libruby-3.{4..0}.so` (Debian dash-versioned) +
   `libruby.so.3.{4..0}` (Fedora versioned).

If none load, `ruby.run*` returns -1 with a clear stderr message
naming the env-var escape hatch.

## On the deploy host

You need a Ruby 3 runtime installed. Examples:

| Host | Package | Soname shipped |
|---|---|---|
| Bazzite / Fedora | `ruby` | `libruby.so` → `libruby.so.3.X` |
| Debian / Ubuntu | `libruby3.X` | `libruby-3.X.so` (Debian convention) |
| RHEL / Rocky | `ruby-libs` | `libruby.so.3.X` |

The bridge does NOT need `ruby-dev` on the deploy host. Headers are
only needed at build time (handled by `aether-build --with=ruby`).

## What `ae build` does for you automatically

When your program has `import contrib.host.ruby`, `ae build`:

1. Links `libaether_host_ruby.a` (the in-tree bridge) onto the
   resulting binary automatically. You do NOT need
   `-laether_host_ruby` in `link_flags` — the import is the trigger.
2. Does NOT add any libruby link flags — none are needed, since the
   bridge dlopens libruby at runtime.

`ae build` errors with a clear actionable message if the bridge .a
hasn't been built: build the image with
`aether-build --with=ruby --rebuild-image` (containerised) or
`make contrib MODULES=ruby && make install-contrib` (installed
toolchain).

## `aether.toml` — usually empty for ruby

You typically don't need to set `cflags` or `link_flags` for
`contrib.host.ruby` at all:

```toml
# aether.toml — nothing required for contrib.host.ruby
[[bin]]
name = "myapp"
path = "myapp.ae"
```

If the deploy host's Ruby isn't discoverable via the default order,
set `AETHER_RUBY_SONAME` in the build environment:

```sh
AETHER_RUBY_SONAME=libruby-3.1.so ae build myapp.ae
```

## Usage

```aether
import contrib.host.ruby

main() {
    ruby.run("puts 'hello from ruby'")
}
```

Sandboxed:

```aether
import contrib.host.ruby
import std.list

main() {
    perms = list.new()
    list.add(perms, "fs_read")
    list.add(perms, "/etc/*")
    ruby.run_sandboxed(perms, "puts File.read('/etc/hostname')")
}
```

## Implementation notes

- All Ruby C-API access goes through a `dlsym` function-pointer
  table — see `aether_host_ruby.c`. The bridge .a has NO unresolved
  Ruby symbols (`nm -u` confirms).
- `RTLD_GLOBAL` at `dlopen` so Ruby C extensions loaded later
  resolve their Ruby refs against the same libruby instance.
- `RUBY_INIT_STACK` macro replaced with an explicit
  `g_rb.ruby_init_stack(&local_volatile_VALUE)` call on the main
  thread before `g_rb.ruby_init()`. Same effect, no macro.
- `Qnil` baked as `(VALUE)0x08` — the modern Ruby `USE_FLONUM=1`
  default constant (in effect since Ruby 2.0 / 2013). NIL_P
  comparisons use direct equality.
- `rb_intern` and `rb_funcall` are `#define`d as macros in
  `ruby/ruby.h` (rb_funcall expands to a GCC statement-expr block).
  The dlopen table fields are named `f_rb_intern` and `f_rb_funcall`
  to avoid the macro collision (same trap as `Py_None` in the python
  bridge).
- Ruby's `subst.h` `#define snprintf ruby_snprintf` is undef'd
  immediately after `#include <ruby.h>` — the bridge uses libc
  snprintf for plain string assembly, not Ruby's format extensions.
