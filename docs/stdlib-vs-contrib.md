# `std/` vs `contrib/` — placement decisions

When adding a new Aether module that wraps an external C library or
provides a capability not covered by the core language, the first
question is: **does it belong in `std/` or `contrib/`?**

This document captures the rubric used to make that call and applies
it to the modules currently shipped on each side. It complements
[stdlib-module-pattern.md](stdlib-module-pattern.md), which covers
the *shape* of a module once you know *where* it lives.

## The rubric

Ask four questions. A `std/` module should answer "yes" to all four.
If any one is "no", it belongs in `contrib/`.

1. **Is this something a typical Aether project expects to find in
   the standard library?** The bar is what a developer coming from
   Go, Rust, Python, or Node would consider "batteries included".
   File I/O, HTTP, hashing, compression — yes. A database driver, a
   JSON schema validator, a WebSocket framework — no.

2. **Does it have a single obvious API shape?** `std/` modules don't
   have competing implementations. There is one way to hash with
   SHA-256; the Aether wrapper is a thin Go-style veneer over
   whatever library we picked. A database driver, by contrast, can
   reasonably expose a "prepared statement + bind + fetch" API, an
   "execute returning rows" API, or an ORM-ish builder. Not in
   `std/`.

3. **Are the dependencies minimal and well-scoped?** A `std/` module
   adds to the baseline cost of building Aether. `OpenSSL` is
   already a dependency (auto-detected via `pkg-config`, linked
   through `$(OPENSSL_LDFLAGS)`), so `std.cryptography` is free. `zlib` is similarly ambient on every
   POSIX box. `libnghttp2` is auto-detected via `pkg-config` —
   present on every distro that ships HTTP-aware tools, gracefully
   stubbed out when absent. `SQLite` is a 4 MiB amalgamation —
   significant weight for projects that don't need it.

4. **Is the API surface stable and small?** `std/` modules have a
   stability commitment — changes ripple to every Aether user. If
   the API has many knobs, many optional parameters, or many places
   where a "just use the library directly" escape hatch makes
   sense, that friction belongs in `contrib/` where the module can
   evolve without the stability constraint.

## Modules currently in `std/`

Each clears all four rubric questions. Listed roughly by purpose:

| Domain | Modules |
|---|---|
| Strings + bytes | `string`, `bytes`, `intarr`, `floatarr` (packed-double twin of `intarr`), `regex` (PCRE2), `url` (RFC 3986 percent-encoding), `xml` |
| Identifiers | `uuid` (UUID v4/v7, RFC 9562) |
| Collections | `list`, `map`, `collections` |
| File system + paths | `file`, `dir`, `path`, `fs` |
| Networking | `net` (low-level TCP/HTTP client), `tcp`, `http` (high-level: `http.server`, `http.client`, `http.middleware`, `http.server.h2` for HTTP/2) |
| Numeric + time | `math` |
| Serialisation | `json` |
| Hashing + crypto primitives | `cryptography` (SHA-1/256, MD4/MD5, HMAC-SHA256, Base64, CSPRNG, streaming digests — wraps OpenSSL; public-key/ciphers live in `contrib/cryptography/`) |
| Compression | `zlib` (deflate/inflate, ambient on POSIX) |
| Process + env | `os`, `io`, `dl` (dlopen) |
| Memory primitives | `arena` (bulk allocator), `cas` (content-addressed store, sha256 + atomic-rename) |
| Concurrency | `actors` (name → actor_ref registry) |
| Configuration | `config` (process-wide string→string KV — `set-during-init`, `read-everywhere` shape) |
| Logging | `log` |
| Host integration | `host` (event-emit primitives for `--emit=lib`) |

The `http` module is the canonical "single split done right" example
of the rubric — when both client- and server-side became too large
for one flat module, the split landed as `std.http.client`,
`std.http.middleware`, `std.http.server.*` sub-namespaces, all
under the same stability bar. The HTTP/2 surface (`std.http.server.h2`,
issue #260) is built on libnghttp2 with `AETHER_HAS_NGHTTP2`
auto-detection — same mechanism `cryptography` uses for OpenSSL
and `zlib` uses for libz.

## Modules currently in `contrib/`

| Module | Reason for `contrib/` |
|---|---|
| `contrib/sqlite/` | DB driver — fails rubric Q1 (not universal) and Q3 (4 MiB amalgamation). |
| `contrib/cryptography/{rsa,aes,des3,sm4,chacha20poly1305,ed25519,ed448,p256,p384,p521,secp256k1,x25519,x448,pem,asn1}/` | Public-key crypto, symmetric ciphers, and encodings — each family is a separate sub-decision with its own large API surface; fails Q2 (one obvious shape) and Q4 (stability). The hash/HMAC/Base64/CSPRNG primitives that *do* clear the rubric live in `std.cryptography`. |
| `contrib/tinyweb/` | Builder DSL for HTTP services — competes with `std.http.server`'s closure-config approach; opinionated shape that's better off evolving without the stability constraint. |
| `contrib/templating/{native,liquid}/` | Escape-correct output emission — `native` (Aether-as-template-language) and `liquid` (Shopify Liquid port). Opinionated template surfaces; fail Q2. |
| `contrib/parsers/xml_expat/` | Expat-backed streaming XML parser — third-party C dependency; fails Q3 (`std.xml` covers the in-tree case). |
| `contrib/host/{python,lua,perl,ruby,js,tcl,go,java,factor,aether,…}/` | Per-language embedding bridges — each one is a separate sub-decision that wouldn't share an API; fails Q2. |

## Spun out to sibling repos

A `contrib/` module can graduate further — out of this repo entirely
into a sibling project that consumes Aether downstream. This keeps the
core compiler / runtime / stdlib repo focused while the spun-out
project iterates on its own release cadence.

| Sibling repo | Replaces |
|---|---|
| [aether-lang-org/aether-ui](https://github.com/aether-lang-org/aether-ui) | The widget toolkit (GTK4 / AppKit / Win32 backends + AetherUIDriver HTTP test server), formerly `contrib/aether_ui/`. |
| [aether-lang-org/aeb](https://github.com/aether-lang-org/aeb) | The build system (formerly aetherBuild / `aeb-link`); a downstream consumer of `share/aether/MANIFEST` and the runtime sources. |
| servirtium-vcr (`integration/climate_interop/`) | The Servirtium climate-API record/replay harness, formerly `contrib/climate_http_tests/`. The VCR tapes + record-then-replay tests live alongside the other-language reference impls there. |

## Migration test

A module can always move later — `std/` → `contrib/` is painful
(breaks every caller), but `contrib/` → `std/` is low-cost (add an
alias import path, deprecate the `contrib/` one, remove after a
release cycle). When in doubt, **start in `contrib/`**. The only
things that should start in `std/` are the ones that clearly pass
all four rubric questions on day one.

The recent additions of `std.arena` and `std.cas` are good worked
examples: each is a tiny universal primitive (bulk allocator;
content-addressed sha256 store) with a single obvious shape and no
new dependencies, all four rubric questions answer "yes," and they
landed directly in `std/`.

## Cross-reference

- [stdlib-module-pattern.md](stdlib-module-pattern.md) — how to
  *shape* a module once you know where it lives.
- [stdlib-reference.md](stdlib-reference.md) — full surface
  reference for every `std.X` module currently shipped.
- [CONTRIBUTING.md](../CONTRIBUTING.md) — PR process, including
  the `[current]` CHANGELOG convention.
- [contrib/tinyweb/README.md](../contrib/tinyweb/README.md),
  [contrib/templating/README.md](../contrib/templating/README.md) —
  reference examples of `contrib/` modules that clear three of
  the four rubric questions but failed on opinionated API shape
  (Q2) or specific niche (Q1).
