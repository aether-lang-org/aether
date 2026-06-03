# contrib.host.python — Embedded CPython

`import contrib.host.python` lets an Aether program embed CPython
in-process: `python.run_sandboxed(perms, "<source>")` evaluates Python
inside the calling process, with permission-checked access controls.

The build needs CPython's headers at compile time and `libpython` at
link time. Two recipes — pick whichever matches how you're building.

## Recipe A — single-box dev (Linux with `python3-dev` installed)

```bash
# Debian/Ubuntu
sudo apt install python3-dev

# Fedora/Rocky
sudo dnf install python3-devel

# Verify
python3-config --includes
```

Then in `aether.toml`, set the env vars **before** invoking `ae`:

```sh
export AETHER_PYTHON_CFLAGS="$(python3-config --includes) -DAETHER_HAS_PYTHON"
export AETHER_PYTHON_LDFLAGS="$(python3-config --ldflags --embed)"
ae build app.ae
```

```toml
# aether.toml
[build]
cflags     = "${AETHER_PYTHON_CFLAGS}"
link_flags = "${AETHER_PYTHON_LDFLAGS}"
```

`aether.toml` values support `${VAR}` substitution for the
`AETHER_*` env-var allowlist (other names warn + expand empty).
Shell `$(...)` is **not** expanded by `ae`; run it in your own
shell when populating the env var as above.

## Recipe B — split build/deploy (container compile, deploy on host)

This is the aeb-ctr / Bazzite-style flow where the toolchain runs in
a container that has the vendored headers but no `python3-config`
or `libpython`, while the produced binary runs on a deploy host that
has both.

The orchestrator probes the **deploy host** once, then passes the
flags into the container as env vars. The container's `ae build`
expands them via `${VAR}` substitution in `aether.toml`.

Host-side probe (works on hosts with `python3` but **without**
`python3-config`, e.g. Fedora-derived immutable distros):

```sh
# On the deploy host:
python3 -c '
import sysconfig as s
v = s.get_config_var
print("-L"+v("LIBDIR"), "-lpython"+v("LDVERSION"),
      v("LIBS") or "", v("SYSLIBS") or "")
'
# → e.g.  -L/usr/lib64 -lpython3.14 -ldl -lm
```

Capture that and pass it into the container build:

```sh
AETHER_PYTHON_LDFLAGS="$(python3 -c '...probe above...')" \
podman run -e AETHER_PYTHON_LDFLAGS ... aether-builder app.ae
```

The container's `aether.toml`:

```toml
[build]
# Headers are vendored in the image at /opt/aether/include/python/,
# already on the container's include path — no -I needed.
cflags     = "-DAETHER_HAS_PYTHON"
link_flags = "${AETHER_PYTHON_LDFLAGS}"
```

The produced binary `dlopen`s `libpython` lazily, so the deploy
host needs the python3 runtime package (not python3-dev). On
Bazzite-derived hosts `libpython3.X.so.1.0` ships in the base
image already.

## Usage

```aether
import contrib.host.python
python.run_sandboxed(perms, "print('hello')")
```
