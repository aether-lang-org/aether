/*
 * Aether Programming Language Compiler
 * Copyright (c) 2025 Aether Programming Language Contributors
 * 
 * This file is part of Aether.
 * Licensed under the MIT License. See LICENSE file in the project root.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#ifdef _WIN32
#include <process.h>
#  if defined(_MSC_VER) && !defined(getpid)
#    define getpid _getpid
#  endif
#else
#include <unistd.h>
#endif
#include "parser/tokens.h"
#include "ast.h"
#include "parser/parser.h"
#include "analysis/typechecker.h"
#include "analysis/derive.h"
#include "codegen/optimizer.h"
#include "codegen/codegen.h"
#include "aether_error.h"
#include "aether_module.h"
#include "../lsp/aether_lsp.h"

// Compiler limits
#define MAX_TOKENS 50000

// Version is set by Makefile from VERSION file
#ifndef AETHER_VERSION
#define AETHER_VERSION "0.0.0-dev"
#endif

// Constants for better maintainability
#define DEFAULT_MAX_ACTORS 1000
#define DEFAULT_WORKER_THREADS 4

// Global flags
static bool verbose_mode = false;
static bool dump_ast_mode = false;
static bool emit_c_mode = false;
static bool check_only_mode = false;
static bool preempt_mode = false;
// Issue #348 — suppress runtime emission of `requires` / `ensures`
// contract checks. Default is OFF (contracts checked at runtime).
// `--no-contracts` is the analog of C's `-DNDEBUG`: zero per-call
// cost, intended for release builds where the contracts have been
// validated upstream.
static bool no_contracts_mode = false;
static const char* emit_header_path = NULL;

// --emit=<exe|lib|both> — which artifact(s) to produce.
// exe (default): emit `int main(int, char**)`, no aether_* alias stubs.
// lib          : omit main(), emit aether_<name> alias stubs for every top-level
//                Aether function, and refuse to link capability-heavy stdlib modules.
// both         : emit both — executable and library symbols live in one .c file.
static bool emit_exe = true;
static bool emit_lib = false;

// --emit-main=<func> — with --emit=lib, emit a thin main(argc,argv)
// shim that calls the named Aether function. Closes the exe/lib
// symmetry: one .c ships as both a loadable lib AND a binary. Issue
// #268.3. NULL when not set; the codegen ignores the shim when the
// flag is absent or when --emit=lib isn't active.
static const char* emit_main_target = NULL;

// --with=<capability>[,<capability>...] — capability opt-ins for
// --emit=lib. Default is capability-empty (every capability-gated
// stdlib import is rejected); a project that IS the host — linking
// the emitted .c into its own binary rather than embedding it as a
// user script — opts into the subset it needs with a comma-separated
// list. Flag is a no-op without --emit=lib.
static bool with_fs = false;
static bool with_net = false;
static bool with_os = false;

// --emit-namespace-manifest: walk a manifest.ae's AST, extract the
// namespace/input/event/bindings calls, and write a JSON description
// to stdout. Used by `ae build --namespace <dir>` to learn about the
// namespace before codegen so it can synthesize the discovery struct.
// Implies --check (no codegen output).
static bool emit_namespace_manifest = false;

// --emit-namespace-describe: like --emit-namespace-manifest but writes
// a C source file containing a static const AetherNamespaceManifest
// and the aether_describe() definition. The C file is then linked
// into the namespace .so so the host can call aether_describe() at
// runtime. Output path is the second positional arg (the input is
// the manifest.ae).
static bool emit_namespace_describe = false;

// --list-functions: walk the AST, print one line per top-level Aether
// function definition: `<name>|<return_type>|<param_name>:<param_type>,...`
// Used by the namespace pipeline's per-language SDK generators to learn
// what aether_<name>() exports the .so will contain.
static bool list_functions_mode = false;

// --diagnose=ownership: print the per-function string-ownership
// verdicts the codegen would use at the wrapper terminator
// (codegen_stmt.c:1611-1631) — without running codegen. Helps
// downstreams localise the "heap-string handed to a collection then
// variable reassigned" UAF shape that the heap-string-tracker fix
// can unmask.
static bool diagnose_ownership_mode = false;

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
    #define access _access
    #define F_OK 0
    #define PATH_SEP '\\'
#else
    #include <unistd.h>
    #define PATH_SEP '/'
#endif

// Helper to check file existence
int compiler_file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

// Derive header filename from output path (output.c -> output.h)
static char* derive_header_path(const char* output_path) {
    size_t len = strlen(output_path);
    char* header_path = malloc(len + 1);
    strcpy(header_path, output_path);

    // Replace .c with .h, or append .h if no .c extension
    if (len > 2 && header_path[len-2] == '.' && header_path[len-1] == 'c') {
        header_path[len-1] = 'h';
    } else {
        header_path = realloc(header_path, len + 3);
        strcat(header_path, ".h");
    }
    return header_path;
}

// Print a summary line if any errors were recorded
static void report_compilation_failure(void) {
    int n = aether_error_count();
    if (n > 0) {
        fprintf(stderr, "aborting: %d error(s) found\n", n);
    }
}

// JSON helpers for --emit-namespace-manifest. Quote a string literal,
// handling backslash and double-quote. Newlines/tabs are unlikely in
// manifest fields but escape them for safety.
static void json_emit_str(FILE* out, const char* s) {
    if (!s) { fputs("null", out); return; }
    fputc('"', out);
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c == '\n') { fputs("\\n", out); }
        else if (c == '\t') { fputs("\\t", out); }
        else if (c < 0x20) { fprintf(out, "\\u%04x", c); }
        else { fputc(c, out); }
    }
    fputc('"', out);
}

// Captured manifest used by both the JSON emitter and the C-describe
// emitter. Borrowed pointers into the parsed AST's literal storage —
// valid for the lifetime of the parse.
typedef struct {
    const char* ns_name;
    const char* java_pkg;
    const char* java_class;
    const char* py_module;
    const char* rb_module;
    const char* go_package;
    struct { const char* name; const char* type_sig; } inputs[64];
    int input_count;
    struct { const char* name; const char* carries; } events[64];
    int event_count;
} ExtractedManifest;

// Walk a parsed program AST and capture every top-level manifest
// builder call (namespace/input/event/bindings/java/python/go) found
// in the body of any top-level function. The function ordering inside
// the AST preserves the user's declaration order — inputs and events
// appear in the generated SDK in the order the manifest declared them.
//
// We deliberately do NOT execute the manifest at compile time; we
// inspect the AST. That sidesteps the need to dlopen the just-built
// library inside `aetherc` and matches how documented contract
// generators (gRPC's protoc-gen-*, openapi-generator, etc.) work:
// description is structural, not behavioural.
/* Recursively walk an AST node looking for manifest-builder calls.
 * Handles both the flat form (`setup() { describe("x") input(...) }`)
 * and the nested form (`abi() { describe("x") { input(...) bindings()
 * { java(...) } } }`).
 *
 * In the nested form, an `AST_FUNCTION_CALL` like `describe("trading")
 * { ... }` has the trailing block as a child node (typically the last
 * child, of type AST_CLOSURE with value "trailing", or AST_BLOCK).
 * We recurse into anything that looks like a child block. */
static void extract_manifest_walk(ExtractedManifest* m, ASTNode* node) {
    if (!node) return;

    /* If this is an AST_FUNCTION_CALL, capture builder-call args. */
    if (node->type == AST_FUNCTION_CALL && node->value) {
        const char* name = node->value;
        #define ARG_STR(n) (node->child_count > (n) && node->children[(n)] \
                            && node->children[(n)]->type == AST_LITERAL \
                            ? node->children[(n)]->value : NULL)
        if (strcmp(name, "describe") == 0) {
            if (ARG_STR(0)) m->ns_name = ARG_STR(0);
        } else if (strcmp(name, "input") == 0) {
            if (m->input_count < 64 && ARG_STR(0) && ARG_STR(1)) {
                m->inputs[m->input_count].name = ARG_STR(0);
                m->inputs[m->input_count].type_sig = ARG_STR(1);
                m->input_count++;
            }
        } else if (strcmp(name, "event") == 0) {
            if (m->event_count < 64 && ARG_STR(0) && ARG_STR(1)) {
                m->events[m->event_count].name = ARG_STR(0);
                m->events[m->event_count].carries = ARG_STR(1);
                m->event_count++;
            }
        } else if (strcmp(name, "java") == 0) {
            if (ARG_STR(0)) m->java_pkg = ARG_STR(0);
            if (ARG_STR(1)) m->java_class = ARG_STR(1);
        } else if (strcmp(name, "python") == 0) {
            if (ARG_STR(0)) m->py_module = ARG_STR(0);
        } else if (strcmp(name, "ruby") == 0) {
            if (ARG_STR(0)) m->rb_module = ARG_STR(0);
        } else if (strcmp(name, "go") == 0) {
            if (ARG_STR(0)) m->go_package = ARG_STR(0);
        }
        #undef ARG_STR
    }

    /* Recurse into all children — picks up nested trailing blocks
     * (the trailing block of `describe("x") { ... }` is a child of
     * the call node) and the ordinary statements of any function body. */
    for (int i = 0; i < node->child_count; i++) {
        extract_manifest_walk(m, node->children[i]);
    }
}

static void extract_manifest(ExtractedManifest* m, ASTNode* program) {
    memset(m, 0, sizeof(*m));
    if (!program) return;

    /* Walk every top-level function (`abi`, `setup`, `main`, whatever).
     * Skip externs and the import statements themselves — we only
     * want to find calls inside function bodies. */
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* fn = program->children[i];
        if (!fn || (fn->type != AST_FUNCTION_DEFINITION
                 && fn->type != AST_BUILDER_FUNCTION
                 && fn->type != AST_MAIN_FUNCTION)) continue;
        extract_manifest_walk(m, fn);
    }
}

static void emit_manifest_json(FILE* out, ASTNode* program) {
    if (!out) return;
    ExtractedManifest m;
    extract_manifest(&m, program);

    fputs("{\n  \"namespace\": ", out); json_emit_str(out, m.ns_name); fputs(",\n", out);

    fputs("  \"inputs\": [", out);
    for (int i = 0; i < m.input_count; i++) {
        if (i > 0) fputs(", ", out);
        fputs("{\"name\": ", out); json_emit_str(out, m.inputs[i].name);
        fputs(", \"type\": ",     out); json_emit_str(out, m.inputs[i].type_sig);
        fputs("}", out);
    }
    fputs("],\n", out);

    fputs("  \"events\": [", out);
    for (int i = 0; i < m.event_count; i++) {
        if (i > 0) fputs(", ", out);
        fputs("{\"name\": ",    out); json_emit_str(out, m.events[i].name);
        fputs(", \"carries\": ", out); json_emit_str(out, m.events[i].carries);
        fputs("}", out);
    }
    fputs("],\n", out);

    fputs("  \"bindings\": {\n", out);
    fputs("    \"java\": {\"package\": ", out); json_emit_str(out, m.java_pkg);
    fputs(", \"class\": ", out); json_emit_str(out, m.java_class); fputs("},\n", out);
    fputs("    \"python\": {\"module\": ", out); json_emit_str(out, m.py_module); fputs("},\n", out);
    fputs("    \"ruby\": {\"module\": ", out); json_emit_str(out, m.rb_module); fputs("},\n", out);
    fputs("    \"go\": {\"package\": ", out); json_emit_str(out, m.go_package); fputs("}\n", out);
    fputs("  }\n", out);
    fputs("}\n", out);
}

// C-quote a string for embedding in a static initializer. Same escape
// rules as JSON above but for C string literals.
static void c_emit_str(FILE* out, const char* s) {
    if (!s) { fputs("NULL", out); return; }
    fputc('"', out);
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c == '\n') { fputs("\\n", out); }
        else if (c == '\t') { fputs("\\t", out); }
        else if (c < 0x20) { fprintf(out, "\\x%02x\"\"", c); }
        else { fputc(c, out); }
    }
    fputc('"', out);
}

// Walk a parsed program AST and emit one line per top-level function:
//   <name>|<return_aether_type>|<param_name>:<param_aether_type>,...
//
// Each parameter is `name:type`; multiple parameters comma-separated;
// no parameters → empty string after the second |. Return type is the
// Aether type spelling we infer from the function definition's
// node_type (or "void" if unset). Used by the namespace pipeline to
// generate per-language SDKs that map Aether function signatures to
// host-language method signatures.
//
// We skip functions whose name starts with `_` (treated as private)
// and the synthesized main() that the namespace pipeline injects.
static const char* aether_type_spelling(Type* t) {
    if (!t) return "void";
    switch (t->kind) {
        case TYPE_INT:    return "int";
        case TYPE_INT64:  return "long";
        case TYPE_UINT64: return "ulong";
        case TYPE_FLOAT:  return "float";
        case TYPE_BOOL:   return "bool";
        case TYPE_STRING: return "string";
        case TYPE_PTR:    return "ptr";
        case TYPE_VOID:   return "void";
        default:          return "any";
    }
}

static void emit_function_list(FILE* out, ASTNode* program) {
    if (!out || !program) return;
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* fn = program->children[i];
        if (!fn || (fn->type != AST_FUNCTION_DEFINITION
                 && fn->type != AST_BUILDER_FUNCTION)) continue;
        if (!fn->value) continue;
        if (fn->value[0] == '_') continue;
        if (fn->is_imported) continue;

        fprintf(out, "%s|%s|", fn->value, aether_type_spelling(fn->node_type));

        int param_count = 0;
        for (int j = 0; j < fn->child_count; j++) {
            ASTNode* c = fn->children[j];
            if (!c) continue;
            if (c->type == AST_GUARD_CLAUSE) continue;
            if (c->type == AST_BLOCK) continue;
            if (c->type != AST_VARIABLE_DECLARATION
             && c->type != AST_PATTERN_VARIABLE) continue;
            if (param_count > 0) fputc(',', out);
            fprintf(out, "%s:%s",
                c->value ? c->value : "_unnamed",
                aether_type_spelling(c->node_type));
            param_count++;
        }
        fputc('\n', out);
    }
}

// Emit a self-contained C source file declaring a static const
// AetherNamespaceManifest populated from the parsed manifest AST,
// plus an aether_describe() exported function. This file is linked
// into the namespace .so so the host can introspect at runtime.
static void emit_describe_c(FILE* out, ASTNode* program) {
    if (!out) return;
    ExtractedManifest m;
    extract_manifest(&m, program);

    /* The describe stub is intentionally self-contained — it doesn't
     * #include aether_host.h so that it can be linked into a .so even
     * when the host's include path isn't known to aetherc. We define
     * a *layout-compatible* struct under a different name and cast at
     * the boundary. The host #includes aether_host.h and sees the
     * canonical names. */
    fputs("/* Auto-generated by aetherc --emit-namespace-describe. DO NOT EDIT. */\n", out);
    fputs("#include <stdint.h>\n", out);
    fputs("#include <stddef.h>\n\n", out);
    fputs("struct AetherInputDecl { const char* name; const char* type_signature; };\n", out);
    fputs("struct AetherEventDecl { const char* name; const char* carries_type; };\n", out);
    fputs("struct AetherJavaBinding   { const char* package_name; const char* class_name; };\n", out);
    fputs("struct AetherPythonBinding { const char* module_name; };\n", out);
    fputs("struct AetherRubyBinding   { const char* module_name; };\n", out);
    fputs("struct AetherGoBinding     { const char* package_name; };\n", out);
    fputs("struct AetherNamespaceManifest {\n", out);
    fputs("    const char* namespace_name;\n", out);
    fputs("    int input_count;\n", out);
    fputs("    struct AetherInputDecl inputs[64];\n", out);
    fputs("    int event_count;\n", out);
    fputs("    struct AetherEventDecl events[64];\n", out);
    fputs("    struct AetherJavaBinding   java;\n", out);
    fputs("    struct AetherPythonBinding python;\n", out);
    fputs("    struct AetherRubyBinding   ruby;\n", out);
    fputs("    struct AetherGoBinding     go;\n", out);
    fputs("};\n\n", out);

    fputs("static const struct AetherNamespaceManifest g_aether_describe = {\n", out);
    fputs("    .namespace_name = ", out); c_emit_str(out, m.ns_name); fputs(",\n", out);
    fprintf(out, "    .input_count = %d,\n", m.input_count);
    fputs("    .inputs = {\n", out);
    for (int i = 0; i < m.input_count; i++) {
        fputs("        { ", out); c_emit_str(out, m.inputs[i].name);
        fputs(", ", out); c_emit_str(out, m.inputs[i].type_sig);
        fputs(" },\n", out);
    }
    fputs("    },\n", out);
    fprintf(out, "    .event_count = %d,\n", m.event_count);
    fputs("    .events = {\n", out);
    for (int i = 0; i < m.event_count; i++) {
        fputs("        { ", out); c_emit_str(out, m.events[i].name);
        fputs(", ", out); c_emit_str(out, m.events[i].carries);
        fputs(" },\n", out);
    }
    fputs("    },\n", out);
    fputs("    .java = { ", out); c_emit_str(out, m.java_pkg);
    fputs(", ", out); c_emit_str(out, m.java_class); fputs(" },\n", out);
    fputs("    .python = { ", out); c_emit_str(out, m.py_module); fputs(" },\n", out);
    fputs("    .ruby = { ", out);   c_emit_str(out, m.rb_module); fputs(" },\n", out);
    fputs("    .go = { ", out); c_emit_str(out, m.go_package); fputs(" },\n", out);
    fputs("};\n\n", out);

    /* aether_describe() is the runtime discovery entry point. Hosts
     * cast the returned pointer to AetherNamespaceManifest* (declared
     * canonically in runtime/aether_host.h). */
    fputs("const struct AetherNamespaceManifest* aether_describe(void) {\n", out);
    fputs("    return &g_aether_describe;\n", out);
    fputs("}\n", out);
}

// Compile aether source to C
int compile_source(const char* input_path, const char* output_path) {
    // Read input file
    FILE *input = fopen(input_path, "r");
    if (!input) {
        perror("Error opening input file");
        return 0;
    }
    
    fseek(input, 0, SEEK_END);
    long file_size = ftell(input);
    fseek(input, 0, SEEK_SET);
    
    char *source = malloc(file_size + 1);
    if (!source) {
        perror("Memory allocation error");
        fclose(input);
        return 0;
    }
    
    size_t bytes_read = fread(source, 1, file_size, input);
    fclose(input);
    if (bytes_read == 0 && file_size > 0) {
        fprintf(stderr, "Error: Failed to read file\n");
        free(source);
        return 0;
    }
    // On Windows text mode, bytes_read may be less than file_size due to line ending conversion
    // Null-terminate at actual bytes read
    source[bytes_read] = '\0';
    
    if (verbose_mode) printf("Compiling %s...\n", input_path);

    aether_error_init(input_path, source);

    // Step 1: Lexical Analysis
    if (verbose_mode) {
        printf("[Phase 1/5] Lexical Analysis...\n");
    }
    
    lexer_init(source);
    
    Token* tokens[MAX_TOKENS];
    int token_count = 0;
    
    while (token_count < MAX_TOKENS - 1) {
        Token* token = next_token();
        tokens[token_count] = token;
        token_count++;
        
        if (token->type == TOKEN_EOF) {
            break;
        }
        
        if (token->type == TOKEN_ERROR) {
            aether_error_with_code(token->value, token->line, token->column,
                                   AETHER_ERR_SYNTAX);
            // Cleanup tokens
            for (int i = 0; i < token_count; i++) {
                free_token(tokens[i]);
            }
            free(source);
            return 0;
        }
    }
    
    // Check for token overflow (file too large)
    if (token_count >= MAX_TOKENS - 1 && tokens[token_count - 1]->type != TOKEN_EOF) {
        fprintf(stderr, "error: source file exceeds maximum token limit (%d tokens)\n", MAX_TOKENS);
        fprintf(stderr, "  help: split into multiple files using imports\n");
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free(source);
        return 0;
    }

    if (verbose_mode) printf("Generated %d tokens\n", token_count);

    // Step 2: Parsing
    if (verbose_mode) printf("Step 2: Parsing...\n");
    Parser* parser = create_parser(tokens, token_count);
    ASTNode* program = parse_program(parser);

    // Stamp every parsed node with its source path so codegen can
    // emit `#line N "path"` directives. Imported-module nodes get
    // their own path stamped by module_parse_file before clone, so
    // re-stamping the merged tree later is a no-op for them.
    if (program) ast_stamp_source_file(program, input_path);

    if (!program) {
        report_compilation_failure();
        // Cleanup
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free(source);
        return 0;
    }

    /* Parse can complete and return a partial program even when the
     * parser recorded errors (e.g. reserved-keyword-as-function-name
     * cases that we skip + diagnose rather than abort). Don't proceed
     * to codegen in that case — the partial AST will silently drop
     * the offending decl and downstream compilation produces a
     * binary that calls a nonexistent C function. */
    if (aether_error_count() > 0) {
        report_compilation_failure();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) free_token(tokens[i]);
        free_parser(parser);
        free(source);
        return 0;
    }

    if (verbose_mode) printf("Parse successful\n");

    // --dump-ast: print the AST and exit (no codegen)
    if (dump_ast_mode) {
        print_ast(program, 0);
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free(source);
        return 1;  // success
    }

    // --emit-namespace-manifest: walk the AST for std.host builder calls
    // and emit JSON to stdout. Used by `ae build --namespace`. No codegen.
    if (emit_namespace_manifest) {
        emit_manifest_json(stdout, program);
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free(source);
        return 1;  // success
    }

    // --emit-namespace-describe: walk the AST and emit a self-contained
    // C file with the embedded AetherNamespaceManifest + aether_describe()
    // stub. Output path is `output_path` (the second positional argument).
    if (emit_namespace_describe) {
        FILE* out = fopen(output_path, "w");
        if (!out) {
            perror("Error opening output file");
            free_ast_node(program);
            for (int i = 0; i < token_count; i++) free_token(tokens[i]);
            free_parser(parser);
            free(source);
            return 0;
        }
        emit_describe_c(out, program);
        fclose(out);
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) free_token(tokens[i]);
        free_parser(parser);
        free(source);
        return 1;
    }

    // Step 2.5: Module Orchestration
    if (verbose_mode) printf("[Phase 2.5/5] Module resolution...\n");
    module_set_source_dir(input_path);
    if (!module_orchestrate(program)) {
        report_compilation_failure();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free(source);
        return 0;
    }
    if (verbose_mode) printf("Module resolution successful\n");

    // Step 2.6: Merge pure Aether module functions into program AST
    module_merge_into_program(program);

    // Step 2.65: Tree-shake imported functions the program never calls.
    // Reduces typecheck and gcc compile time on programs that only use
    // a slice of large stdlib modules. Must run after merge (so the
    // closure can see merged helpers) and before typecheck (so dead
    // bodies don't slow it down). See module_prune_unreachable.
    module_prune_unreachable(program);

    // Step 2.67: `@derive(...)` synthesizer (#338).
    // Walks AST_STRUCT_DEFINITION nodes carrying a `derive:<list>`
    // annotation, synthesizes the helper function definitions
    // (T_eq today; format / clone / hash in follow-up commits),
    // and inserts them as siblings into the program. Runs AFTER
    // module_prune_unreachable so synthesized functions don't get
    // pruned as "unused" before the call sites that need them
    // type-check, and BEFORE typecheck_program so synthesized
    // bodies type-check normally.
    if (derive_synthesize_pass(program) != 0) {
        module_registry_shutdown();
        free_ast_node(program);
        for (int k = 0; k < token_count; k++) free_token(tokens[k]);
        free_parser(parser);
        free(source);
        return 0;
    }

    // Step 2.7: --emit=lib capability-empty check.
    // In lib mode the output is consumed by another process (Java host,
    // Python script, etc.) that owns network/filesystem/process access.
    // An embedded Aether script that opens sockets or writes files is
    // a capability escalation — fail the build and point the user to the
    // documented pattern (host does I/O, script returns data).
    //
    // Projects that ARE the host (compile .ae + handwritten C into one
    // binary) opt into specific capabilities with --with=fs / --with=net
    // / --with=os. The gate stays default-deny; --with is the explicit
    // acknowledgement that the host owns and audits the surface it's
    // enabling. Keeping the capability categories coarse (three buckets)
    // matches the three banned-import groupings below.
    if (emit_lib) {
        // Each entry is (module_name, granted_flag, capability_name).
        // When granted_flag is non-zero, that module is allowed; otherwise
        // it's rejected with a message that names the --with flag needed.
        struct { const char* module; bool granted; const char* cap; } gated[] = {
            { "std.fs",   with_fs,  "fs"  },
            { "std.net",  with_net, "net" },
            { "std.http", with_net, "net" },
            { "std.tcp",  with_net, "net" },
            { "std.os",   with_os,  "os"  },
        };
        int num_gated = (int)(sizeof(gated) / sizeof(gated[0]));
        for (int i = 0; i < program->child_count; i++) {
            ASTNode* child = program->children[i];
            if (!child || child->type != AST_IMPORT_STATEMENT || !child->value) continue;
            for (int g = 0; g < num_gated; g++) {
                if (strcmp(child->value, gated[g].module) != 0) continue;
                if (gated[g].granted) break;  // opted in; allow.
                fprintf(stderr,
                    "Error: --emit=lib rejects 'import %s' without --with=%s.\n"
                    "\n"
                    "       The library ABI is capability-empty by default so an\n"
                    "       embedded Aether script can't escalate beyond what its\n"
                    "       host grants. Pass --with=%s if the binary linking this\n"
                    "       library is itself the capability owner (i.e. you're\n"
                    "       writing systems code that compiles .ae and handwritten\n"
                    "       C into one executable). Multiple capabilities can be\n"
                    "       comma-separated: --with=%s,os\n",
                    gated[g].module, gated[g].cap, gated[g].cap, gated[g].cap);
                module_registry_shutdown();
                free_ast_node(program);
                for (int k = 0; k < token_count; k++) free_token(tokens[k]);
                free_parser(parser);
                free(source);
                return 0;
            }
        }
    }

    // Step 3: Type Checking
    if (verbose_mode) printf("Step 3: Type checking...\n");
    if (!typecheck_program(program)) {
        report_compilation_failure();
        // Cleanup
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free(source);
        return 0;
    }
    
    if (verbose_mode) printf("Type checking successful\n");

    // --list-functions: post-typecheck so node_type is populated.
    if (list_functions_mode) {
        emit_function_list(stdout, program);
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) free_token(tokens[i]);
        free_parser(parser);
        free(source);
        return 1;
    }

    // --diagnose=ownership: print per-function string-ownership
    // verdicts and exit. Post-typecheck so node_type is populated for
    // the user-fn structural-escape recursion.
    if (diagnose_ownership_mode) {
        codegen_diagnose_ownership(program, stdout);
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) free_token(tokens[i]);
        free_parser(parser);
        free(source);
        return 1;
    }

    // --check: stop after typecheck + type inference, no codegen
    if (check_only_mode) {
        int warnings = aether_warning_count();
        if (warnings > 0) {
            fprintf(stderr, "OK: %d warning(s)\n", warnings);
        } else {
            fprintf(stderr, "OK: no errors\n");
        }
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free(source);
        return 1;  // success
    }

    // Step 3.5: Optimization (AST-level passes: constant folding, dead code, tail calls)
    if (verbose_mode) printf("Step 3.5: Optimizing...\n");
    program = optimize_ast(program);

    // Step 4: Code Generation
    if (verbose_mode) printf("Step 4: Generating C code...\n");
    FILE *output = fopen(output_path, "w");
    if (!output) {
        perror("Error opening output file");
        // Cleanup
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free(source);
        return 0;
    }
    
    // Open header file if --emit-header was specified
    FILE* header_output = NULL;
    char* header_path = NULL;
    if (emit_header_path) {
        if (strcmp(emit_header_path, "auto") == 0) {
            header_path = derive_header_path(output_path);
        } else {
            header_path = strdup(emit_header_path);
        }
        header_output = fopen(header_path, "w");
        if (!header_output) {
            fprintf(stderr, "Warning: Could not open header file %s\n", header_path);
        }
    }

    CodeGenerator* codegen;
    if (header_output) {
        codegen = create_code_generator_with_header(output, header_output, header_path);
        if (verbose_mode) printf("Also generating header: %s\n", header_path);
    } else {
        codegen = create_code_generator(output);
    }
    if (preempt_mode) codegen->preempt_loops = 1;
    if (no_contracts_mode) codegen->no_contracts = 1;
    codegen->emit_exe = emit_exe ? 1 : 0;
    codegen->emit_lib = emit_lib ? 1 : 0;
    codegen->emit_main_target = emit_main_target;  // NULL when not requested
    // Source path so codegen can expand `__FILE__` literally (#265).
    codegen->source_file = input_path;
    int errors_before_codegen = aether_error_count();
    generate_program(codegen, program);
    fclose(output);
    if (header_output) {
        fclose(header_output);
    }
    if (header_path) {
        free(header_path);
    }

    // If codegen reported new errors (e.g. L4 closure/state validation),
    // abort with a non-zero exit so callers see the compile failure rather
    // than a half-written output file. Only count errors reported by
    // codegen itself — parse-phase errors are handled separately to avoid
    // regressing legacy tests that silently tolerate parser noise.
    if (aether_error_count() > errors_before_codegen) {
        report_compilation_failure();
        // Remove the partial output to avoid downstream build steps
        // picking up an incomplete file.
        remove(output_path);
        module_registry_shutdown();
        free_ast_node(program);
        for (int i = 0; i < token_count; i++) {
            free_token(tokens[i]);
        }
        free_parser(parser);
        free_code_generator(codegen);
        free(source);
        return 0;
    }

    if (verbose_mode) {
        printf("Code generation successful\n");
        // Print all optimization stats here — series/linear loop collapse happens during codegen,
        // so stats must be printed after generate_program(), not before it.
        print_optimization_stats();
    }

    // Cleanup
    module_registry_shutdown();
    free_ast_node(program);
    for (int i = 0; i < token_count; i++) {
        free_token(tokens[i]);
    }
    free_parser(parser);
    free_code_generator(codegen);
    free(source);

    return 1;
}

// Compile C file to executable using system compiler (gcc)
int compile_c_to_exe(const char* c_file, const char* exe_file) {
    char cmd[1024];
    
    // Assume runtime is in "runtime/" relative to current dir, or check specific paths
    // For now, assume user is running from project root or has runtime folder nearby.
    // We try to locate the runtime folder.
    
    const char* runtime_path = "runtime";
    if (!compiler_file_exists("runtime/actor.c")) {
        if (compiler_file_exists("../runtime/actor.c")) {
            runtime_path = "../runtime";
        } else {
            fprintf(stderr, "Error: Could not locate Aether runtime files.\n");
            return 0;
        }
    }

#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "gcc \"%s\" \"%s\\*.c\" -o \"%s\" -I\"%s\" -O2 -lpthread", 
             c_file, runtime_path, exe_file, runtime_path);
#else
    snprintf(cmd, sizeof(cmd), "gcc \"%s\" \"%s\"/*.c -o \"%s\" -I\"%s\" -O2 -lpthread", 
             c_file, runtime_path, exe_file, runtime_path);
#endif

    if (verbose_mode) printf("Executing: %s\n", cmd);
    int result = system(cmd);
    return result == 0;
}

// ---------------------------------------------------------------------------
// --concat-ae: source-to-source merge of multiple .ae files into one
// synthetic .ae. Issue #268.1.
//
// Behaviour, line-based:
//   1. Walk each input file. For every line whose first non-whitespace
//      token is `import`, accumulate it into a deduped list (first
//      occurrence wins, order preserved). All other lines are body.
//   2. Count `main(` definitions across all files. Reject with a clear
//      error if more than one (the merge target is one whole-program
//      TU; two `main()`s would collide at link time).
//   3. Emit the synthetic file as: (banner) → (deduped imports) →
//      (each input's body, prefixed with a `// ---- <path> ----` marker
//      so diagnostics still trace back to the original source).
//
// Pure source-to-source — no parser involvement, matching the issue's
// "no `--emit=lib` involvement" contract. Edge cases the line-based
// approach skips intentionally:
//   - Multi-line `import { ... }` blocks (Aether doesn't have these
//     today; if added, this function will need to switch to lexer-
//     level tokenisation).
//   - The string literal `"import "` or the comment `// import …`
//     starting at column 0 — both would be (incorrectly) picked up
//     as imports. The simpler check is correct for the canonical
//     downstream use case (aetherBuild's generated `.build.ae` files,
//     which have imports at the file head and never inside strings).
// ---------------------------------------------------------------------------

static char* concat_ae_read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char* buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

// Trim trailing CR / spaces / tabs from a length-bounded substring.
static size_t concat_ae_trim_trailing(const char* s, size_t len) {
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) {
        len--;
    }
    return len;
}

static int concat_ae_imports_contains(char** imports, int count, const char* line, size_t len) {
    for (int i = 0; i < count; i++) {
        if (strncmp(imports[i], line, len) == 0 && imports[i][len] == '\0') return 1;
    }
    return 0;
}

static int do_concat_ae(char** inputs, int input_count, const char* output_path) {
    char** imports        = NULL;
    int    imports_count  = 0;
    int    imports_cap    = 0;
    int    main_count     = 0;
    char** bodies         = malloc(sizeof(char*) * (size_t)input_count);
    if (!bodies) {
        fprintf(stderr, "Error: --concat-ae: out of memory\n");
        return 1;
    }
    for (int i = 0; i < input_count; i++) bodies[i] = NULL;

    for (int f = 0; f < input_count; f++) {
        char* source = concat_ae_read_file(inputs[f]);
        if (!source) {
            fprintf(stderr, "Error: --concat-ae: cannot read '%s'\n", inputs[f]);
            for (int j = 0; j < f; j++) free(bodies[j]);
            free(bodies);
            for (int i = 0; i < imports_count; i++) free(imports[i]);
            free(imports);
            return 1;
        }

        size_t cap = strlen(source) + 64;
        char* body = malloc(cap);
        if (!body) {
            fprintf(stderr, "Error: --concat-ae: out of memory\n");
            free(source);
            for (int j = 0; j < f; j++) free(bodies[j]);
            free(bodies);
            for (int i = 0; i < imports_count; i++) free(imports[i]);
            free(imports);
            return 1;
        }
        size_t pos = 0;

        char* p = source;
        while (*p) {
            char* line_start = p;
            while (*p && *p != '\n') p++;
            size_t raw_len = (size_t)(p - line_start);

            // Trim leading whitespace for classification only — body
            // lines stay verbatim so indent/style is preserved.
            const char* trimmed = line_start;
            size_t trim_len = raw_len;
            while (trim_len > 0 && (*trimmed == ' ' || *trimmed == '\t')) {
                trimmed++;
                trim_len--;
            }
            trim_len = concat_ae_trim_trailing(trimmed, trim_len);

            int is_import = (trim_len >= 7 && strncmp(trimmed, "import ", 7) == 0);
            if (is_import) {
                if (!concat_ae_imports_contains(imports, imports_count, trimmed, trim_len)) {
                    if (imports_count >= imports_cap) {
                        imports_cap = imports_cap ? imports_cap * 2 : 16;
                        char** grown = realloc(imports, sizeof(char*) * (size_t)imports_cap);
                        if (!grown) {
                            fprintf(stderr, "Error: --concat-ae: out of memory\n");
                            free(body);
                            free(source);
                            for (int j = 0; j < f; j++) free(bodies[j]);
                            free(bodies);
                            for (int i = 0; i < imports_count; i++) free(imports[i]);
                            free(imports);
                            return 1;
                        }
                        imports = grown;
                    }
                    char* dup = malloc(trim_len + 1);
                    memcpy(dup, trimmed, trim_len);
                    dup[trim_len] = '\0';
                    imports[imports_count++] = dup;
                }
            } else {
                // Verbatim copy. Detect `main(` definitions before
                // appending so we can fail early on duplicates.
                if (trim_len >= 5 &&
                    strncmp(trimmed, "main", 4) == 0) {
                    const char* m = trimmed + 4;
                    while (*m == ' ' || *m == '\t') m++;
                    if (*m == '(') main_count++;
                }
                if (pos + raw_len + 2 > cap) {
                    cap = (pos + raw_len + 64) * 2;
                    char* grown = realloc(body, cap);
                    if (!grown) {
                        fprintf(stderr, "Error: --concat-ae: out of memory\n");
                        free(body);
                        free(source);
                        for (int j = 0; j < f; j++) free(bodies[j]);
                        free(bodies);
                        for (int i = 0; i < imports_count; i++) free(imports[i]);
                        free(imports);
                        return 1;
                    }
                    body = grown;
                }
                memcpy(body + pos, line_start, raw_len);
                pos += raw_len;
                body[pos++] = '\n';
            }

            if (*p == '\n') p++;
        }
        body[pos] = '\0';
        bodies[f] = body;
        free(source);
    }

    if (main_count > 1) {
        fprintf(stderr,
                "Error: --concat-ae: %d main() definitions across input files; at most one is allowed.\n",
                main_count);
        for (int i = 0; i < input_count; i++) free(bodies[i]);
        free(bodies);
        for (int i = 0; i < imports_count; i++) free(imports[i]);
        free(imports);
        return 1;
    }

    FILE* out = fopen(output_path, "w");
    if (!out) {
        fprintf(stderr, "Error: --concat-ae: cannot write '%s'\n", output_path);
        for (int i = 0; i < input_count; i++) free(bodies[i]);
        free(bodies);
        for (int i = 0; i < imports_count; i++) free(imports[i]);
        free(imports);
        return 1;
    }

    fprintf(out, "// Generated by aetherc --concat-ae from %d file(s).\n", input_count);
    fprintf(out, "// Imports deduped; bodies concatenated verbatim.\n\n");
    for (int i = 0; i < imports_count; i++) {
        fprintf(out, "%s\n", imports[i]);
    }
    if (imports_count > 0) fprintf(out, "\n");
    for (int b = 0; b < input_count; b++) {
        fprintf(out, "// ---- %s ----\n", inputs[b]);
        fputs(bodies[b], out);
        fprintf(out, "\n");
    }
    fclose(out);

    for (int i = 0; i < input_count; i++) free(bodies[i]);
    free(bodies);
    for (int i = 0; i < imports_count; i++) free(imports[i]);
    free(imports);
    return 0;
}

// Parse the `--concat-ae <files...> -o <out>` tail and dispatch.
static int run_concat_ae(int argc, char** argv, int start) {
    const char* output = NULL;
    char** inputs = NULL;
    int input_count = 0;
    int input_cap = 0;

    for (int i = start; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --concat-ae: -o requires an output path\n");
                free(inputs);
                return 1;
            }
            output = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: --concat-ae: unknown flag '%s'\n", argv[i]);
            free(inputs);
            return 1;
        } else {
            if (input_count >= input_cap) {
                input_cap = input_cap ? input_cap * 2 : 8;
                char** grown = realloc(inputs, sizeof(char*) * (size_t)input_cap);
                if (!grown) {
                    fprintf(stderr, "Error: --concat-ae: out of memory\n");
                    free(inputs);
                    return 1;
                }
                inputs = grown;
            }
            inputs[input_count++] = argv[i];
        }
    }

    if (!output) {
        fprintf(stderr, "Error: --concat-ae requires -o <output.ae>\n");
        free(inputs);
        return 1;
    }
    if (input_count == 0) {
        fprintf(stderr, "Error: --concat-ae requires at least one input file\n");
        free(inputs);
        return 1;
    }

    int rc = do_concat_ae(inputs, input_count, output);
    free(inputs);
    return rc;
}

void print_help(const char* program_name) {
    printf("Aether Compiler v%s\n\n", AETHER_VERSION);
    printf("Usage:\n");
    printf("  %s <input.ae> <output.c>         Compile Aether to C\n", program_name);
    printf("  %s run <input.ae>                Compile and run immediately\n", program_name);
    printf("  %s lsp                           Run the language server on stdio\n", program_name);
    printf("  %s --concat-ae <files...> -o <out.ae>\n", program_name);
    printf("                                   Discover-and-dedupe source merge — emits one\n");
    printf("                                   synthetic .ae with each file's content, imports\n");
    printf("                                   deduped, accepting at most one main()\n");
    printf("\n");
    printf("Options:\n");
    printf("  --version, -v                    Show version information\n");
    printf("  --verbose                        Show detailed compilation phases and timing\n");
    printf("  --emit-c                         Print generated C code to stdout\n");
    printf("  --emit-header [path]             Generate C header for embedding (default: auto)\n");
    printf("  --emit=<exe|lib|both>            Output artifact (exe default, lib produces .so/.dylib)\n");
    printf("  --emit-main=<func>               With --emit=lib: also emit a thin main(argc,argv) shim\n");
    printf("                                   that calls <func>(). Closes the exe/lib symmetry.\n");
    printf("  --emit-namespace-manifest        Print the manifest JSON for a manifest.ae and exit\n");
    printf("  --check                          Type-check only (no code generation)\n");
    printf("  --no-contracts                   Skip runtime emission of `requires`/`ensures` checks (#348)\n");
    printf("  --dump-ast                       Print AST and exit (no code generation)\n");
    printf("  --diagnose=ownership             Print string-ownership verdicts and exit (no code generation)\n");
    printf("  --help, -h                       Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s hello.ae hello.c              Compile to C\n", program_name);
    printf("  %s run hello.ae                  Quick run\n", program_name);
    printf("  %s lsp                           Run the embedded language server\n", program_name);
    printf("  %s --concat-ae a.ae b.ae -o all.ae   Merge sources for whole-program build\n", program_name);
    printf("  %s --verbose hello.ae hello.c    Compile with timing info\n", program_name);
    printf("  %s --emit-header hello.ae hello.c  Generate hello.h for C embedding\n", program_name);
}

int main(int argc, char *argv[]) {
    // Subcommand dispatch — checked BEFORE the flag loop so flags
    // can't shadow it. `aetherc lsp` runs the embedded language
    // server on stdio (same code path as the standalone aether-lsp
    // binary, which is now a thin alias). Issue #327.
    if (argc >= 2 && strcmp(argv[1], "lsp") == 0) {
        LSPServer* server = lsp_server_create();
        if (!server) {
            fprintf(stderr, "Error: failed to create LSP server\n");
            return 1;
        }
        lsp_server_run(server);
        lsp_server_free(server);
        return 0;
    }

    // --concat-ae: same priority as `lsp` — break out before the
    // generic flag loop because the rest of argv is a flag-shaped
    // list of inputs the loop would mis-classify. Issue #268.1.
    if (argc >= 2 && strcmp(argv[1], "--concat-ae") == 0) {
        return run_concat_ae(argc, argv, 2);
    }

    // Parse flags
    int arg_offset = 1;
    while (arg_offset < argc && argv[arg_offset][0] == '-') {
        if (strcmp(argv[arg_offset], "--version") == 0 || strcmp(argv[arg_offset], "-v") == 0) {
            printf("Aether Compiler v%s\n", AETHER_VERSION);
            return 0;
        } else if (strcmp(argv[arg_offset], "--help") == 0 || strcmp(argv[arg_offset], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[arg_offset], "--verbose") == 0) {
            verbose_mode = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--emit-c") == 0) {
            emit_c_mode = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--dump-ast") == 0) {
            dump_ast_mode = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--check") == 0) {
            check_only_mode = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--preempt") == 0) {
            preempt_mode = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--no-contracts") == 0) {
            no_contracts_mode = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--diagnose=ownership") == 0) {
            diagnose_ownership_mode = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--emit-header") == 0) {
            // Check for optional explicit path argument (must end in .h)
            if (arg_offset + 1 < argc && argv[arg_offset + 1][0] != '-') {
                const char* next_arg = argv[arg_offset + 1];
                size_t len = strlen(next_arg);
                if (len > 2 && next_arg[len-2] == '.' && next_arg[len-1] == 'h') {
                    // Explicit .h path provided
                    emit_header_path = next_arg;
                    arg_offset += 2;
                } else {
                    // Next arg is probably the input file, not a header path
                    emit_header_path = "auto";
                    arg_offset++;
                }
            } else {
                emit_header_path = "auto";  // Auto-derive from output filename
                arg_offset++;
            }
        } else if (strcmp(argv[arg_offset], "--lib") == 0) {
            if (arg_offset + 1 >= argc) {
                fprintf(stderr, "--lib requires an argument\n");
                return 1;
            }
            /* First --lib resets the chain (overriding any
             * AETHER_LIB_DIR env-var seed); subsequent --lib flags
             * append. This lets the `ae` CLI emit one `--lib <X>`
             * per resolved entry without needing platform-fragile
             * separator-string quoting at the shell layer (cmd.exe +
             * MSYS2 mangle `;` inside quoted args). A human passing
             * `--lib "a:b"` still works because both entry points
             * route through the same separator parser. Issue #413. */
            static int seen_lib_flag = 0;
            if (!seen_lib_flag) {
                module_set_lib_dir(argv[arg_offset + 1]);
                seen_lib_flag = 1;
            } else {
                module_add_lib_dirs(argv[arg_offset + 1]);
            }
            arg_offset += 2;
        } else if (strncmp(argv[arg_offset], "--emit=", 7) == 0) {
            const char* val = argv[arg_offset] + 7;
            if (strcmp(val, "exe") == 0) {
                emit_exe = true;
                emit_lib = false;
            } else if (strcmp(val, "lib") == 0) {
                emit_exe = false;
                emit_lib = true;
            } else if (strcmp(val, "both") == 0) {
                emit_exe = true;
                emit_lib = true;
            } else {
                fprintf(stderr, "Error: --emit must be one of: exe, lib, both (got '%s')\n", val);
                return 1;
            }
            arg_offset++;
        } else if (strncmp(argv[arg_offset], "--emit-main=", 12) == 0) {
            // --emit-main=<func>: a follow-up to --emit=lib. The codegen
            // emits a thin main() shim calling the named function, so a
            // single .c can ship as a lib AND as an exe. Issue #268.3.
            const char* val = argv[arg_offset] + 12;
            if (!*val) {
                fprintf(stderr, "Error: --emit-main= requires a function name\n");
                return 1;
            }
            emit_main_target = val;
            arg_offset++;
        } else if (strncmp(argv[arg_offset], "--with=", 7) == 0) {
            // Comma-separated capability opt-ins for --emit=lib. Unknown
            // tokens are a hard error rather than silently ignored — a
            // typo in a capability name should fail the build, not leave
            // the user wondering why their import still gets rejected.
            const char* list = argv[arg_offset] + 7;
            const char* p = list;
            while (*p) {
                const char* start = p;
                while (*p && *p != ',') p++;
                size_t len = (size_t)(p - start);
                if (len == 2 && strncmp(start, "fs", 2) == 0) {
                    with_fs = true;
                } else if (len == 3 && strncmp(start, "net", 3) == 0) {
                    with_net = true;
                } else if (len == 2 && strncmp(start, "os", 2) == 0) {
                    with_os = true;
                } else if ((len == 11 && strncmp(start, "first-party", 11) == 0) ||
                           (len == 3  && strncmp(start, "all", 3) == 0)) {
                    // "I am the host, every capability is granted."
                    // Equivalent to fs,net,os but expresses intent
                    // (systems code, not sandboxed plugin) rather
                    // than enumerating buckets. See docs/emit-lib.md
                    // §"Opting in" for when this is appropriate.
                    with_fs = true;
                    with_net = true;
                    with_os = true;
                } else {
                    fprintf(stderr,
                        "Error: --with= got unknown capability '%.*s'. "
                        "Known: fs, net, os, first-party (alias: all).\n",
                        (int)len, start);
                    return 1;
                }
                if (*p == ',') p++;
            }
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--emit-namespace-manifest") == 0) {
            emit_namespace_manifest = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--emit-namespace-describe") == 0) {
            emit_namespace_describe = true;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "--list-functions") == 0) {
            list_functions_mode = true;
            arg_offset++;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[arg_offset]);
            fprintf(stderr, "Use --help for usage information\n");
            return 1;
        }
    }
    
    if (argc - arg_offset < 1) {
        print_help(argv[0]);
        return 1;
    }

    // Check for "run" command
    if (strcmp(argv[arg_offset], "run") == 0) {
        if (argc - arg_offset < 2) {
            fprintf(stderr, "Usage: %s run <input.ae>\n", argv[0]);
            return 1;
        }
        
        const char* input_path = argv[arg_offset + 1];
        
        // Generate temp filenames
        char c_path[256];
        char exe_path[256];
        
        // Simple temp name generation based on input
        // "test.ae" -> "test.ae.c", "test.ae.exe"
        snprintf(c_path, sizeof(c_path), "%s.c", input_path);
        snprintf(exe_path, sizeof(exe_path), "%s.exe", input_path); // .exe works on Linux too usually, or just append nothing
        
        // 1. Compile Aether -> C
        if (!compile_source(input_path, c_path)) {
            return 1;
        }
        
        // 2. Compile C -> Exe
        if (!compile_c_to_exe(c_path, exe_path)) {
            fprintf(stderr, "Build failed.\n");
            // Try to cleanup temp C file at least
            remove(c_path); 
            return 1;
        }
        
        // 3. Run Exe
        printf("Running program...\n----------------\n");
        int result = system(exe_path);
        
        // 4. Cleanup
        // Note: Temporary files are kept for debugging
        
        return result;
    }
    
    // --dump-ast only needs the input file
    if (dump_ast_mode) {
        if (!compile_source(argv[arg_offset], "/dev/null")) {
            return 1;
        }
        return 0;
    }

    // --diagnose=ownership only needs the input file. The branch in
    // compile_source emits to stdout and short-circuits before codegen.
    if (diagnose_ownership_mode) {
        if (!compile_source(argv[arg_offset], "/dev/null")) {
            return 1;
        }
        return 0;
    }

    // --emit-c: compile and print generated C to stdout
    if (emit_c_mode) {
        // Compile to a temp file, then cat to stdout
        char tmp_path[256];
        snprintf(tmp_path, sizeof(tmp_path), "/tmp/aether_emit_%d.c", (int)getpid());
        if (!compile_source(argv[arg_offset], tmp_path)) {
            return 1;
        }
        FILE* f = fopen(tmp_path, "r");
        if (f) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                fwrite(buf, 1, n, stdout);
            }
            fclose(f);
        }
        remove(tmp_path);
        return 0;
    }

    // --check: type-check only, no output file needed
    if (check_only_mode) {
        if (!compile_source(argv[arg_offset], "/dev/null")) {
            return 1;
        }
        return 0;
    }

    // Default mode: Compile to C (original behavior)
    if (argc - arg_offset < 2) {
        fprintf(stderr, "Usage: %s <input.ae> <output.c>\n", argv[0]);
        fprintf(stderr, "Use --help for more information\n");
        return 1;
    }

    if (!compile_source(argv[arg_offset], argv[arg_offset + 1])) {
        return 1;
    }
    
    if (verbose_mode) printf("Output written to %s\n", argv[arg_offset + 1]);
    return 0;
}
