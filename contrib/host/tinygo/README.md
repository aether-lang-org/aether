# contrib.host.tinygo — In-Process Go via c-shared

Run Go code inside the Aether process — no subprocess, no IPC, no
marshalling. Just `dlopen` the `.so` / `.dll` that
`go build -buildmode=c-shared` produces and call its exported
functions directly.

> **Toolchain note.** The bridge is named `tinygo` (and the
> `--with=tinygo` capability layer ships TinyGo) for the
> small-footprint, embedded-friendly story, but
> `tinygo build -buildmode=c-shared` currently only supports
> `wasm` targets ("buildmode c-shared is only supported on wasm
> at the moment"). For **native** linux/darwin/windows c-shared
> libraries — what this bridge dlopens — use standard
> `go build -buildmode=c-shared`. The bridge dlopens by symbol;
> it doesn't care which compiler produced the `.so`. The
> `--with=tinygo` image ships both `go` and `tinygo` so either
> path is one command away.

This is the in-process counterpart of [`contrib/host/go`](../go/),
which spawns the standard `go` toolchain as a sandboxed
subprocess. Pick the host that matches your containment story:

| | `contrib/host/go` | `contrib/host/tinygo` |
|---|---|---|
| Toolchain | Standard `go` | `tinygo` |
| Process model | Subprocess (LD_PRELOAD sandbox) | In-process (`dlopen`) |
| Latency per call | Process spawn (~ms) | Direct function call (~ns) |
| Memory model | Separate heap | Shared heap (TinyGo + Aether) |
| Goroutines | Full Go runtime | TinyGo's reduced runtime |
| Sandbox enforcement | Per-call (libc grants) | Per-process (one-time grant) |

## Prerequisites

You need a Go toolchain that can build `-buildmode=c-shared`.
On native linux/darwin/windows that's standard `go` (≥1.21);
TinyGo is fine for the wasm target only.

```bash
# macOS / Linux / Windows — standard Go
# https://go.dev/dl/

# Verify
go version
```

## Build the Go side

Mark each function you want to call from Aether with a `//export`
comment. The function name on the Aether side matches the
exported C symbol exactly (case-sensitive, no name mangling).

```go
// greet.go
package main

import "C"  // required by -buildmode=c-shared

//export Answer
func Answer() int32 { return 42 }

//export Add
func Add(a, b int32) int32 { return a + b }

//export Greet
func Greet(name *C.char) *C.char {
    msg := "hello, " + C.GoString(name)
    return C.CString(msg)  // Go-allocated, leaked unless freed (see notes)
}

func main() {}  // c-shared still requires a main() — empty body is fine
```

Build (native — use standard `go`, not `tinygo`; see toolchain
note at the top):

```bash
CGO_ENABLED=1 go build -buildmode=c-shared -o libgreet.so    greet.go  # Linux
CGO_ENABLED=1 go build -buildmode=c-shared -o libgreet.dylib greet.go  # macOS
CGO_ENABLED=1 go build -buildmode=c-shared -o libgreet.dll   greet.go  # Windows
```

For the wasm target — and only then — substitute
`tinygo build -buildmode=c-shared -target=wasm …`.

## Call from Aether

```aether
import contrib.host.tinygo

main() {
    handle, err = tinygo.load("./libgreet.so")
    if handle == null {
        println("load failed: ${err}")
        return
    }

    answer = tinygo.call_int_void(handle, "Answer")
    println("Answer = ${answer}")          // -> Answer = 42

    total = tinygo.call_int_int_int(handle, "Add", 2, 40)
    println("Add(2, 40) = ${total}")       // -> Add(2, 40) = 42

    msg = tinygo.call_str_str(handle, "Greet", "world")
    println(msg)                           // -> hello, world

    tinygo.unload(handle)
}
```

## Calling-convention surface

v1 ships pre-defined wrapper signatures for the most common
shapes:

| Aether call | Matches TinyGo c-shared signature |
|---|---|
| `tinygo.call_int_void(h, "F")` | `int F(void)` |
| `tinygo.call_int_int(h, "F", a)` | `int F(int)` |
| `tinygo.call_int_int_int(h, "F", a, b)` | `int F(int, int)` |
| `tinygo.call_void_int(h, "F", a)` | `void F(int)` |
| `tinygo.call_str_str(h, "F", s)` | `const char* F(const char*)` |

Adding a new shape is a one-line C extension in
[`aether_host_tinygo.c`](aether_host_tinygo.c) plus a matching
`extern` + wrapper in [`module.ae`](module.ae). Patches welcome.

Fully-dynamic dispatch (libffi) is intentionally out of scope for
v1: libffi is a system dependency 95% of users do not need, and
covering 80% of real call sites with five fixed shapes keeps the
contrib module dependency-free.

## Memory ownership

TinyGo's `C.CString(...)` allocates with `malloc` on the C heap
and is **not garbage-collected** by the Go runtime. Without an
explicit `C.free`, every call leaks. v1 of this module accepts
the leak for short-lived demos; for long-running programs, expose
a `Free(p *C.char)` from the Go side and call it from Aether.

Pointers returned from TinyGo are valid until the next call into
the library on the same handle, or until `tinygo.unload(handle)`.
Copy via `string_concat(s, "")` if you need to outlive that
window.

## Limitations

- **Single Go runtime per process.** `dlopen` of two distinct
  c-shared Go libraries in the same process can collide on
  runtime state. Stick to one library per Aether process.
- **No goroutines that outlive the Aether call.** The Go
  scheduler runs inside the call; spawning a goroutine that
  blocks on I/O and returns to the Aether caller before the
  goroutine completes is undefined behaviour.
- **`tinygo build -buildmode=c-shared` is wasm-only today.**
  Native linux/darwin/windows c-shared `.so`s come from standard
  `go build` (see the toolchain note at the top of this file).
  The Aether-side bridge is purely a `dlopen` + symbol lookup —
  it loads whatever c-shared the toolchain produced.
- **Symbol export is `//export Name`.** Neither standard Go nor
  TinyGo exports package-level `Name` automatically — you must
  annotate each function you want callable from C / Aether.

## Testing

The end-to-end test lives at
[`tests/integration/host_tinygo/`](../../../tests/integration/host_tinygo/) —
[`uses_tinygo.ae`](../../../tests/integration/host_tinygo/uses_tinygo.ae)
is the driver (loads the c-shared `.so` at `$TINYGO_LIB`, built from
`examples/greet.go`, and exercises one call per wrapper shape:
`Answer = 42`, `Add(2, 40) = 42`, `Negate(7) = -7`, `hello, world`),
and
[`test_host_tinygo.sh`](../../../tests/integration/host_tinygo/test_host_tinygo.sh)
is the runner. The runner **SKIPs** (never fails) when `go` is not on
PATH, matching `contrib/host/go`'s pattern so CI stays green on machines
without the toolchain. When `go` is present it builds a c-shared `.so`
with `CGO_ENABLED=1 go build -buildmode=c-shared`, loads it via
`contrib.host.tinygo`, and asserts each expected line.

## See also

- [`contrib/host/go/`](../go/) — full Go via subprocess + LD_PRELOAD
  sandbox.
- [`std/dl/module.ae`](../../../std/dl/module.ae) — the
  cross-platform `dlopen` shim this host sits on top of.
- [TinyGo c-shared docs](https://tinygo.org/docs/guides/cgo/)
