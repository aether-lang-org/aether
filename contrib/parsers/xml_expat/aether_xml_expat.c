/* contrib/xml/expat — thin libexpat veneer for Aether.
 *
 * SAX-style streaming XML parser. Mirrors libexpat's native model:
 * caller registers per-event handlers (start element, end element,
 * character data), then feeds bytes into the parser; libexpat
 * invokes the registered handlers as it walks the input.
 *
 * Aether callback shape:
 *   The Aether-side surface (contrib/xml/expat/module.ae) registers
 *   handlers as raw C function pointers via `expr as fn(args)`. This
 *   matches the qsort/_as fn_ idiom already in the language
 *   (tests/regression/test_fn_address_via_as_fn.ae) and avoids the
 *   `_AeClosure` adapter machinery that would be needed for closures
 *   with captured state. Captured-state closures are NOT supported
 *   for SAX handlers in v1 — pass a small user-data ptr through
 *   xml_parser_set_user_data and read it inside the handler if you
 *   need per-parse state. (libexpat's user-data slot is exposed for
 *   that purpose; we forward it verbatim.)
 *
 * Surface:
 *   xml_parser_new()                     -> XML_Parser*  (NULL on OOM)
 *   xml_parser_free(p)                   -> void
 *   xml_parser_set_user_data(p, ud)      -> void
 *   xml_parser_get_user_data(p)          -> void*
 *   xml_parser_set_start_handler(p, fn)  -> void   fn = void(*)(void* ud, const char* name, const char** atts)
 *   xml_parser_set_end_handler(p, fn)    -> void   fn = void(*)(void* ud, const char* name)
 *   xml_parser_set_text_handler(p, fn)   -> void   fn = void(*)(void* ud, const char* s, int len)
 *   xml_parse_chunk(p, buf, len, final)  -> int    1 on success, 0 on parse error
 *   xml_parser_error_string(p)           -> const char*  (NULL when no error)
 *   xml_parser_error_line(p)             -> int
 *   xml_parser_error_column(p)           -> int
 *   xml_attr_count(atts)                 -> int    number of name/value PAIRS in the atts array
 *   xml_attr_name(atts, i)               -> const char*
 *   xml_attr_value(atts, i)              -> const char*
 *
 * Build:
 *   - pkg-config --libs expat → -lexpat (system package: libexpat-dev on
 *     Debian, expat-devel on Fedora, brew install expat on macOS).
 *   - aether.toml: add this file to [[bin]] extra_sources and
 *     `link_flags = "-lexpat"` to [build].
 *   - This is deliberately a C-only dependency. The Aether toolchain
 *     does not auto-detect libexpat (same rationale as sqlite —
 *     contrib/-tier means opt-in build-time linkage). The integration
 *     test probes for libexpat and SKIPs if it's missing.
 */

#include <expat.h>
#include <stdlib.h>
#include <string.h>

/* Aether surface — symbol names follow contrib/sqlite's `xml_` prefix
 * convention so they don't collide with libc / other libraries. The
 * Aether module.ae registers them as bare-named externs. */

XML_Parser xml_parser_new(void) {
    return XML_ParserCreate(NULL);  /* NULL = no explicit encoding;
                                     * expat handles XML decl
                                     * autodetect. */
}

void xml_parser_free(XML_Parser p) {
    if (p) XML_ParserFree(p);
}

void xml_parser_set_user_data(XML_Parser p, void* ud) {
    if (p) XML_SetUserData(p, ud);
}

void* xml_parser_get_user_data(XML_Parser p) {
    return p ? XML_GetUserData(p) : NULL;
}

/* The handler-setter functions cast the incoming ptr (a raw C
 * function pointer produced Aether-side by `expr as fn(...)`) back
 * to the expected expat callback shape. libexpat passes user-data
 * as the first arg (set via XML_SetUserData), then the element-
 * specific data. */
typedef void (*xml_start_fn)(void* ud, const char* name, const char** atts);
typedef void (*xml_end_fn)(void* ud, const char* name);
typedef void (*xml_text_fn)(void* ud, const char* s, int len);

void xml_parser_set_start_handler(XML_Parser p, void* fn) {
    if (!p) return;
    XML_SetStartElementHandler(p, (XML_StartElementHandler)(xml_start_fn)fn);
}

void xml_parser_set_end_handler(XML_Parser p, void* fn) {
    if (!p) return;
    XML_SetEndElementHandler(p, (XML_EndElementHandler)(xml_end_fn)fn);
}

void xml_parser_set_text_handler(XML_Parser p, void* fn) {
    if (!p) return;
    XML_SetCharacterDataHandler(p, (XML_CharacterDataHandler)(xml_text_fn)fn);
}

/* Parse a chunk of XML bytes. `final` is 1 when this is the last
 * chunk in the document (flushes any pending state); 0 for
 * intermediate chunks. Returns 1 on success, 0 on parse error.
 * Use xml_parser_error_string/line/column on the parser to retrieve
 * the failure detail. */
int xml_parse_chunk(XML_Parser p, const char* buf, int len, int final) {
    if (!p || !buf) return 0;
    enum XML_Status status = XML_Parse(p, buf, len, final ? 1 : 0);
    return status == XML_STATUS_OK ? 1 : 0;
}

const char* xml_parser_error_string(XML_Parser p) {
    if (!p) return NULL;
    enum XML_Error code = XML_GetErrorCode(p);
    if (code == XML_ERROR_NONE) return NULL;
    return XML_ErrorString(code);
}

int xml_parser_error_line(XML_Parser p) {
    if (!p) return 0;
    return (int)XML_GetCurrentLineNumber(p);
}

int xml_parser_error_column(XML_Parser p) {
    if (!p) return 0;
    return (int)XML_GetCurrentColumnNumber(p);
}

/* Attribute-array accessors. libexpat passes attributes as a
 * NULL-terminated array of alternating name/value pairs:
 *
 *     atts = ["k1", "v1", "k2", "v2", NULL]
 *
 * xml_attr_count returns the PAIR count (2 in the example);
 * xml_attr_name and xml_attr_value index into the pair-space, so
 * xml_attr_name(atts, 0) == "k1" and xml_attr_value(atts, 0) ==
 * "v1". This is the same indexing convention as libexpat's own
 * documentation; we just hide the *2 striding from Aether-side
 * loops. */
int xml_attr_count(const char** atts) {
    if (!atts) return 0;
    int n = 0;
    while (atts[n * 2] != NULL) n++;
    return n;
}

const char* xml_attr_name(const char** atts, int i) {
    if (!atts || i < 0) return NULL;
    /* Trust the caller has consulted xml_attr_count(); we don't
     * bound-check against the NULL terminator on every access. */
    return atts[i * 2];
}

const char* xml_attr_value(const char** atts, int i) {
    if (!atts || i < 0) return NULL;
    return atts[i * 2 + 1];
}

/* ---- Closure-aware handler registration --------------------------------
 *
 * The bare-fn registration above (xml_parser_set_start_handler etc.) is
 * fine for top-level named functions but loses captured-state closures
 * — the user-data slot is the only state channel and the caller has to
 * thread a struct through it by hand.
 *
 * The trio below accepts an Aether `_AeClosure` value passed as a
 * boxed pointer (the codegen's `_aether_box_closure(...)` shape).
 * Each handler unpacks the closure into:
 *   - `.fn`  — the C-typed function pointer for _closure_fn_N (Aether's
 *              hoisted closure-body function); we register it as the
 *              libexpat callback directly. Its first parameter is the
 *              env pointer, so the (env, args) ABI matches libexpat's
 *              (user_data, args) call shape exactly.
 *   - `.env` — the heap-allocated captured-state struct; we stash it
 *              as the parser's user_data so libexpat hands it back as
 *              the first argument on each callback.
 *
 * IMPORTANT: a single libexpat parser only has ONE user_data slot. So
 * the captured-state path supports ONE closure-style handler per
 * parser, not three separate captured-state handlers. If the caller
 * needs all three event types with captured state, the closure must
 * pack everything it cares about into the same captured environment
 * (the natural Aether shape is a single `parse_with` trailing block
 * whose body sees `on_start`/`on_end`/`on_text` as nested calls;
 * those all close over the same enclosing scope).
 *
 * For the v1 builder veneer (`expat.parse_with`) we register all
 * three handlers as variants of the SAME closure (or a small dispatch
 * shim built by the wrapper) so the env is shared. The registration
 * helpers below take the boxed closure for ONE event type. The
 * builder constructs three boxed closures from the trailing block's
 * three nested handlers and uses xml_parser_set_closure_env to set
 * the common env once. */

/* _AeClosure layout mirror — must match the typedef the codegen emits
 * in every generated program (see compiler/codegen/codegen.c around
 * `typedef struct { void (*fn)(void); void* env; } _AeClosure`).
 * Keep in sync. */
typedef struct {
    void (*fn)(void);
    void* env;
} ae_closure_layout;

/* Set the parser's user_data to a specific env pointer. The builder
 * veneer calls this once after registering the closure handlers so
 * all three handlers see the same captured environment. */
void xml_parser_set_closure_env(XML_Parser p, void* env) {
    if (!p) return;
    XML_SetUserData(p, env);
}

/* Register a closure as the start-element handler. `boxed` is the
 * pointer returned by `_aether_box_closure(c)` — i.e. a malloc'd
 * `_AeClosure` we deref here to get the (.fn, .env) pair. The .env
 * is NOT installed as user_data by this call; the caller (typically
 * the parse_with veneer) calls xml_parser_set_closure_env once with
 * the shared env. */
void xml_parser_set_start_closure(XML_Parser p, void* boxed) {
    if (!p || !boxed) return;
    ae_closure_layout* c = (ae_closure_layout*)boxed;
    XML_SetStartElementHandler(p, (XML_StartElementHandler)c->fn);
}

void xml_parser_set_end_closure(XML_Parser p, void* boxed) {
    if (!p || !boxed) return;
    ae_closure_layout* c = (ae_closure_layout*)boxed;
    XML_SetEndElementHandler(p, (XML_EndElementHandler)c->fn);
}

void xml_parser_set_text_closure(XML_Parser p, void* boxed) {
    if (!p || !boxed) return;
    ae_closure_layout* c = (ae_closure_layout*)boxed;
    XML_SetCharacterDataHandler(p, (XML_CharacterDataHandler)c->fn);
}

/* Read .env from a boxed closure — used by the parse_with veneer to
 * grab the shared env and pass it to xml_parser_set_closure_env. */
void* xml_closure_env(void* boxed) {
    if (!boxed) return NULL;
    return ((ae_closure_layout*)boxed)->env;
}

/* ---- Multi-handler closure set ----------------------------------------
 *
 * libexpat's user_data slot is shared across every registered handler,
 * so the simple "register the closure's .fn as the callback and put
 * its .env as user_data" trick only works for ONE captured-state
 * closure per parser. To support a builder block that registers
 * `on_start`, `on_end`, and `on_text` as three separate closures
 * (each potentially with its own captured env), we allocate a small
 * dispatcher struct, install it as user_data, and register three
 * C trampolines that look up the right closure-and-env in the
 * dispatcher on each event.
 *
 * Memory: the dispatcher is malloc'd by xml_handlerset_new and freed
 * by xml_handlerset_free. The Aether-side parse_with veneer is
 * responsible for the new/free pair. The dispatcher only holds
 * COPIES of the _AeClosure (.fn + .env pair) — it does NOT own the
 * closures' env memory; Aether's heap-promote tracking owns that.
 */
typedef struct {
    ae_closure_layout start;
    ae_closure_layout end;
    ae_closure_layout text;
    int has_start;
    int has_end;
    int has_text;
} ae_xml_handlerset;

void* xml_handlerset_new(void) {
    ae_xml_handlerset* s = (ae_xml_handlerset*)calloc(1, sizeof(*s));
    return (void*)s;
}

void xml_handlerset_free(void* hs) {
    if (hs) free(hs);
}

/* Each register call deep-copies the .fn and .env out of the boxed
 * closure into the handler-set's slot. The boxed closure itself can
 * then be released — but Aether's heap-string tracking will keep the
 * `.env` heap allocation alive for the duration of the surrounding
 * scope, so as long as `parse_with` runs synchronously inside that
 * scope, the env pointer stays valid. */
void xml_handlerset_set_start(void* hs, void* boxed_closure) {
    if (!hs || !boxed_closure) return;
    ae_xml_handlerset* s = (ae_xml_handlerset*)hs;
    s->start = *(ae_closure_layout*)boxed_closure;
    s->has_start = 1;
}

void xml_handlerset_set_end(void* hs, void* boxed_closure) {
    if (!hs || !boxed_closure) return;
    ae_xml_handlerset* s = (ae_xml_handlerset*)hs;
    s->end = *(ae_closure_layout*)boxed_closure;
    s->has_end = 1;
}

void xml_handlerset_set_text(void* hs, void* boxed_closure) {
    if (!hs || !boxed_closure) return;
    ae_xml_handlerset* s = (ae_xml_handlerset*)hs;
    s->text = *(ae_closure_layout*)boxed_closure;
    s->has_text = 1;
}

/* Trampolines — each receives the handler-set as user_data (set
 * below in xml_parser_install_handlerset) and dispatches to the
 * registered closure with the closure's own env as first arg. */
static void hs_trampoline_start(void* ud, const char* name, const char** atts) {
    ae_xml_handlerset* s = (ae_xml_handlerset*)ud;
    if (s && s->has_start) {
        typedef void (*fn_t)(void* env, const char* name, const char** atts);
        ((fn_t)s->start.fn)(s->start.env, name, atts);
    }
}

static void hs_trampoline_end(void* ud, const char* name) {
    ae_xml_handlerset* s = (ae_xml_handlerset*)ud;
    if (s && s->has_end) {
        typedef void (*fn_t)(void* env, const char* name);
        ((fn_t)s->end.fn)(s->end.env, name);
    }
}

static void hs_trampoline_text(void* ud, const char* text, int len) {
    ae_xml_handlerset* s = (ae_xml_handlerset*)ud;
    if (s && s->has_text) {
        typedef void (*fn_t)(void* env, const char* text, int len);
        ((fn_t)s->text.fn)(s->text.env, text, len);
    }
}

/* Wire the handler-set into the parser. Installs the trampolines as
 * libexpat handlers and stashes the handler-set as user_data. Only
 * the events for which a closure was registered (has_start /
 * has_end / has_text) actually get a trampoline installed. */
void xml_parser_install_handlerset(XML_Parser p, void* hs) {
    if (!p || !hs) return;
    ae_xml_handlerset* s = (ae_xml_handlerset*)hs;
    XML_SetUserData(p, hs);
    if (s->has_start) XML_SetStartElementHandler(p, hs_trampoline_start);
    if (s->has_end)   XML_SetEndElementHandler(p, hs_trampoline_end);
    if (s->has_text)  XML_SetCharacterDataHandler(p, hs_trampoline_text);
}
