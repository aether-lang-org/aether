// aether_host_ruby.c — Embedded Ruby Language Host Module
//
// Embeds Ruby (CRuby/MRI) in the Aether process. Ruby's File.open,
// ENV[], Kernel#system etc. go through libc and are intercepted by
// the LD_PRELOAD sandbox layer.
//
// TODO (ctr_notes.md Bug 6 follow-up): convert this bridge from
// direct calls to dlopen + dlsym (same pattern as
// contrib/host/python/aether_host_python.c after PR #606). The ruby
// rewrite is bulkier than python's because Ruby's C API leans
// heavily on macros that touch VALUE type tags
// (`NIL_P`, `StringValueCStr`, `Qnil`, `RUBY_INIT_STACK`). Those
// macros can't be dlsym'd directly — they need to be re-expressed
// against a function-pointer table that dlsym's the underlying
// helpers (rb_string_value_cstr, etc.). RUBY_INIT_STACK in
// particular captures the calling stack pointer inline — it has to
// be replicated by hand or replaced with a libruby init API that
// doesn't need it. Worth a focused session, not a rushed bundle.
//
// Until then: the v0.209.0 --with=ruby image layer apt-installs
// ruby-dev, which both supplies the bridge's compile-time headers
// AND provides libruby that the produced binary links against
// (DT_NEEDED libruby-3.X.so.X). End-user binaries are ABI-locked to
// the deploy host's Ruby minor — same constraint the pre-dlopen
// python bridge had, accepted as a known limitation until the
// dlopen rewrite lands.

#include "aether_host_ruby.h"
#include "../../../runtime/aether_sandbox.h"
#include "../../../runtime/aether_shared_map.h"

#ifdef AETHER_HAS_RUBY
#include <ruby.h>
#include <string.h>

static int ruby_initialized = 0;

// Bridge-owned permission stack. Self-contained — see comment in
// contrib/host/tcl/aether_host_tcl.c for rationale.
static void* ruby_perms_stack[64];
static int   ruby_perms_depth = 0;

// Permission checker — shared pattern with other host modules
extern int list_size(void*);
extern void* list_get_raw(void*, int);

static int pattern_match(const char* pat, const char* resource) {
    // Normalize IPv4-mapped IPv6 addresses so a grant for "10.0.0.1"
    // matches a TCP resource reported as "::ffff:10.0.0.1" (and
    // vice versa). Safe for non-TCP categories because "::ffff:"
    // doesn't appear in filesystem paths, env var names, or exec
    // command strings.
    if (pat && strncmp(pat, "::ffff:", 7) == 0) pat += 7;
    if (resource && strncmp(resource, "::ffff:", 7) == 0) resource += 7;
    int plen = strlen(pat);
    int rlen = strlen(resource);
    if (plen == 1 && pat[0] == '*') return 1;
    if (plen > 1 && pat[plen-1] == '*') {
        if (strncmp(pat, resource, plen-1) == 0) return 1;
    }
    if (plen > 1 && pat[0] == '*') {
        int slen = plen - 1;
        if (rlen >= slen && strcmp(resource + rlen - slen, pat + 1) == 0) return 1;
    }
    return strcmp(pat, resource) == 0;
}

static int perms_allow(void* perms, const char* category, const char* resource) {
    if (!perms) return 1;
    int n = list_size(perms);
    if (n == 0) return 0;
    for (int i = 0; i < n; i += 2) {
        const char* cat = (const char*)list_get_raw(perms, i);
        const char* pat = (const char*)list_get_raw(perms, i + 1);
        if (!cat || !pat) continue;
        if (cat[0] == '*' && pat[0] == '*') return 1;
        if (strcmp(cat, category) == 0 && pattern_match(pat, resource)) return 1;
    }
    return 0;
}

static int host_ruby_checker(const char* category, const char* resource) {
    if (ruby_perms_depth <= 0) return 1;
    for (int level = 0; level < ruby_perms_depth; level++) {
        if (!perms_allow(ruby_perms_stack[level], category, resource)) return 0;
    }
    return 1;
}

int ruby_init_host(void) {
    if (ruby_initialized) return 0;

    // Ruby requires RUBY_INIT_STACK on the main thread
    RUBY_INIT_STACK;
    ruby_init();
    ruby_init_loadpath();
    ruby_initialized = 1;
    return 0;
}

void ruby_finalize_host(void) {
    if (ruby_initialized) {
        ruby_finalize();
        ruby_initialized = 0;
    }
}

// Safe eval — catches exceptions and prints them
static int eval_ruby(const char* code) {
    int state = 0;
    rb_eval_string_protect(code, &state);
    if (state) {
        VALUE err = rb_errinfo();
        if (!NIL_P(err)) {
            VALUE msg = rb_funcall(err, rb_intern("message"), 0);
            fprintf(stderr, "[ruby] %s\n", StringValueCStr(msg));
        }
        rb_set_errinfo(Qnil);
        return -1;
    }
    return 0;
}

int ruby_run(const char* code) {
    if (!code) return -1;
    ruby_init_host();
    return eval_ruby(code);
}

int ruby_run_sandboxed(void* perms, const char* code) {
    if (!code) return -1;
    ruby_init_host();

    if (ruby_perms_depth >= 64) return -1;
    ruby_perms_stack[ruby_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = host_ruby_checker;

    // Scrub Ruby's ENV hash — delete vars the sandbox doesn't grant
    // Ruby caches ENV at startup like Perl
    {
        int n = list_size(perms);
        char scrub[4096];
        int pos = snprintf(scrub, sizeof(scrub),
            "_keep = {}; ");
        for (int i = 0; i < n && pos < 3900; i += 2) {
            const char* cat = (const char*)list_get_raw(perms, i);
            const char* pat = (const char*)list_get_raw(perms, i + 1);
            if (cat && strcmp(cat, "env") == 0 && pat) {
                if (strcmp(pat, "*") == 0) {
                    pos += snprintf(scrub + pos, sizeof(scrub) - pos,
                        "ENV.each_key { |k| _keep[k] = true }; ");
                } else {
                    pos += snprintf(scrub + pos, sizeof(scrub) - pos,
                        "_keep['%s'] = true; ", pat);
                }
            }
        }
        snprintf(scrub + pos, sizeof(scrub) - pos,
            "ENV.keys.each { |k| ENV.delete(k) unless _keep[k] }");
        eval_ruby(scrub);
    }

    int result = eval_ruby(code);

    _aether_sandbox_checker = prev;
    ruby_perms_depth--;

    return result;
}

// --- Shared map for Ruby ---
// Same approach as Perl: inject $_aether_input hash, provide methods,
// read $_aether_output hash back.

int ruby_run_sandboxed_with_map(void* perms, const char* code, uint64_t map_token) {
    if (!code) return -1;
    ruby_init_host();

    extern void aether_shared_map_freeze_inputs_by_token(uint64_t);
    aether_shared_map_freeze_inputs_by_token(map_token);

    // Inject inputs as $_aether_input hash
    {
        int n = aether_shared_map_count_by_token(map_token);
        char inject[8192];
        int pos = snprintf(inject, sizeof(inject),
            "$_aether_input = {}; $_aether_output = {};\n");
        for (int i = 0; i < n && pos < 7900; i++) {
            const char* k = aether_shared_map_key_at_by_token(map_token, i);
            const char* v = aether_shared_map_value_at_by_token(map_token, i);
            if (k && v) {
                pos += snprintf(inject + pos, sizeof(inject) - pos,
                    "$_aether_input['%s'] = '%s'\n", k, v);
            }
        }
        pos += snprintf(inject + pos, sizeof(inject) - pos,
            "def aether_map_get(key); $_aether_input[key]; end\n"
            "def aether_map_put(key, val); $_aether_output[key] = val; end\n");
        eval_ruby(inject);
    }

    // Env scrub
    {
        int n = list_size(perms);
        char scrub[4096];
        int pos = snprintf(scrub, sizeof(scrub), "_keep = {}; ");
        for (int i = 0; i < n && pos < 3900; i += 2) {
            const char* cat = (const char*)list_get_raw(perms, i);
            const char* pat = (const char*)list_get_raw(perms, i + 1);
            if (cat && strcmp(cat, "env") == 0 && pat) {
                if (strcmp(pat, "*") == 0) {
                    pos += snprintf(scrub + pos, sizeof(scrub) - pos,
                        "ENV.each_key { |k| _keep[k] = true }; ");
                } else {
                    pos += snprintf(scrub + pos, sizeof(scrub) - pos,
                        "_keep['%s'] = true; ", pat);
                }
            }
        }
        snprintf(scrub + pos, sizeof(scrub) - pos,
            "ENV.keys.each { |k| ENV.delete(k) unless _keep[k] }");
        eval_ruby(scrub);
    }

    if (ruby_perms_depth >= 64) return -1;
    ruby_perms_stack[ruby_perms_depth++] = perms;
    aether_sandbox_check_fn prev = _aether_sandbox_checker;
    _aether_sandbox_checker = host_ruby_checker;

    int result = eval_ruby(code);

    _aether_sandbox_checker = prev;
    ruby_perms_depth--;

    return result;
}

#else
#include <stdio.h>
int ruby_init_host(void) {
    fprintf(stderr, "error: contrib.host.ruby not available (compile with AETHER_HAS_RUBY)\n");
    return -1;
}
void ruby_finalize_host(void) {}
int ruby_run(const char* code) { (void)code; return ruby_init_host(); }
int ruby_run_sandboxed(void* perms,
    const char* code) {
  (void)perms; (void)code;
  return ruby_init_host();
}
int ruby_run_sandboxed_with_map(void* perms, const char* code,
    uint64_t token) {
  (void)perms; (void)code; (void)token;
  return ruby_init_host();
}
#endif
