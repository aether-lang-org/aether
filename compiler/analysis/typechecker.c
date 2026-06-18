#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "typechecker.h"
#include "type_inference.h"
#include "../parser/lexer.h"
#include "../parser/parser.h"
#include "../aether_error.h"
#include "../aether_module.h"

static int error_count = 0;
static int warning_count = 0;

// Get the last component of a module path for namespace
// "mypackage.utils" -> "utils"
static const char* get_namespace_from_path(const char* module_path) {
    const char* last_dot = strrchr(module_path, '.');
    if (last_dot) {
        return last_dot + 1;
    }
    return module_path;
}

// Symbol table functions
SymbolTable* create_symbol_table(SymbolTable* parent) {
    SymbolTable* table = malloc(sizeof(SymbolTable));
    table->symbols = NULL;
    table->parent = parent;
    table->hidden_names = NULL;
    table->seal_whitelist = NULL;
    table->is_sealed = 0;
    // Inherit merged-body flag so nested scopes (loops, blocks, closures
    // inside a merged function) keep the relaxed namespace visibility.
    table->inside_merged_body = parent ? parent->inside_merged_body : 0;
    // dsl_receiver does NOT inherit. It is a per-trailing-closure-scope
    // marker that typecheck_function_call stamps on the immediate
    // closure body. Nested closures inside that body get their own
    // scope without a receiver unless they are themselves trailing
    // closures of another member-access call. Lookup walks the parent
    // chain when the local fallback misses, so an outer DSL block's
    // receiver still wins over the file scope for nested-but-non-DSL
    // identifiers — see lookup_symbol below.
    table->dsl_receiver = NULL;
    return table;
}

static void free_name_list(NameNode* head) {
    while (head) {
        NameNode* next = head->next;
        if (head->name) free(head->name);
        free(head);
        head = next;
    }
}

void free_symbol_table(SymbolTable* table) {
    if (!table) return;

    Symbol* current = table->symbols;
    while (current) {
        Symbol* next = current->next;
        if (current->name) free(current->name);
        if (current->type) free_type(current->type);
        if (current->alias_target) free(current->alias_target);
        free(current);
        current = next;
    }

    free_name_list(table->hidden_names);
    free_name_list(table->seal_whitelist);
    if (table->dsl_receiver) free(table->dsl_receiver);

    free(table);
}

// --- hide / seal directive helpers ---

static int name_list_contains(NameNode* head, const char* name) {
    for (NameNode* n = head; n; n = n->next) {
        if (strcmp(n->name, name) == 0) return 1;
    }
    return 0;
}

void scope_hide_name(SymbolTable* table, const char* name) {
    if (!table || !name) return;
    if (name_list_contains(table->hidden_names, name)) return;
    NameNode* n = malloc(sizeof(NameNode));
    n->name = strdup(name);
    n->next = table->hidden_names;
    table->hidden_names = n;
}

void scope_seal_except(SymbolTable* table, const char* name) {
    if (!table || !name) return;
    table->is_sealed = 1;
    if (name_list_contains(table->seal_whitelist, name)) return;
    NameNode* n = malloc(sizeof(NameNode));
    n->name = strdup(name);
    n->next = table->seal_whitelist;
    table->seal_whitelist = n;
}

int scope_name_is_hidden(SymbolTable* table, const char* name) {
    if (!table || !name) return 0;
    return name_list_contains(table->hidden_names, name);
}

int scope_name_in_whitelist(SymbolTable* table, const char* name) {
    if (!table || !name) return 0;
    return name_list_contains(table->seal_whitelist, name);
}

// Returns 1 if `name` was blocked by a `hide` or `seal except` directive
// somewhere in the scope chain AND a real binding for it exists farther
// up (above the blocking scope). Used to give a better error message than
// "undefined variable" when the user actually meant to reach a hidden one.
static int name_blocked_by_hide(SymbolTable* table, const char* name) {
    if (!table || !name) return 0;
    SymbolTable* t = table;
    while (t) {
        int blocked_here = scope_name_is_hidden(t, name) ||
                           (t->is_sealed && !scope_name_in_whitelist(t, name));
        if (blocked_here) {
            // Walk upward from the parent looking for a real binding.
            for (SymbolTable* check = t->parent; check; check = check->parent) {
                if (lookup_symbol_local(check, name)) return 1;
            }
            return 0;
        }
        t = t->parent;
    }
    return 0;
}

void add_symbol(SymbolTable* table, const char* name, Type* type, int is_actor, int is_function, int is_state) {
    Symbol* symbol = malloc(sizeof(Symbol));
    symbol->name = strdup(name);
    symbol->type = type;
    symbol->is_actor = is_actor;
    symbol->is_function = is_function;
    symbol->is_state = is_state;
    symbol->is_module_alias = 0;
    symbol->alias_target = NULL;
    symbol->node = NULL;  // Initialize to NULL
    symbol->type_inferred = 0;
    symbol->next = table->symbols;
    table->symbols = symbol;
}

Symbol* lookup_symbol(SymbolTable* table, const char* name) {
    if (!table || !name) return NULL;

    // Local bindings always win — `hide` and `seal except` only block
    // resolution that would walk OUT of this scope into a parent.
    Symbol* symbol = lookup_symbol_local(table, name);
    if (symbol) return symbol;

    // Issue #333 DSL block receiver fallback: when this scope is the
    // body of a `receiver.method(args) { ... }` trailing closure,
    // `dsl_receiver` names the receiver namespace or struct type.
    // Try `<dsl_receiver>_<name>` — that's how Aether codegen names
    // namespace and struct-method helpers (e.g. `bash_script`,
    // `Builder_configure`). The walk uses lookup_symbol_local at
    // each level to avoid re-entering this DSL fallback at parent
    // scopes (lookup_symbol is the recursion vector; lookup_symbol_local
    // is not).
    //
    // Nested DSL blocks: inner block's dsl_receiver is checked first.
    // If the rewrite misses and the parent chain is then walked, the
    // outer block's dsl_receiver is checked when the recursion lands
    // on its scope — natural outer→inner shadowing without explicit
    // stack management.
    if (table->dsl_receiver && table->dsl_receiver[0] && name[0]) {
        /* Bounded snprintf via precision spec — caps each component
         * at 250 bytes regardless of strlen, which keeps GCC's
         * format-truncation analyzer happy on -Werror builds.
         * Identifiers above 250 bytes don't occur in practice
         * (longest stdlib symbol is <40 chars). */
        char rewritten[512];
        snprintf(rewritten, sizeof(rewritten), "%.250s_%.250s",
                 table->dsl_receiver, name);
        for (SymbolTable* t = table; t; t = t->parent) {
            Symbol* s = lookup_symbol_local(t, rewritten);
            if (s) return s;
        }
    }

    // Crossing the scope boundary upward: enforce hide / seal directives.
    // - `hide foo` blocks any name in the hidden_names list.
    // - `seal except a, b` blocks anything that isn't in the whitelist.
    // Either way, return NULL so the caller sees an undefined identifier.
    if (scope_name_is_hidden(table, name)) {
        return NULL;
    }
    if (table->is_sealed && !scope_name_in_whitelist(table, name)) {
        return NULL;
    }

    if (table->parent) {
        return lookup_symbol(table->parent, name);
    }

    return NULL;
}

Symbol* lookup_symbol_local(SymbolTable* table, const char* name) {
    Symbol* current = table->symbols;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// Module alias functions
void add_module_alias(SymbolTable* table, const char* alias, const char* module_name) {
    Symbol* symbol = malloc(sizeof(Symbol));
    symbol->name = strdup(alias);
    symbol->type = NULL;  // Modules don't have types
    symbol->is_actor = 0;
    symbol->is_function = 0;
    symbol->is_state = 0;
    symbol->is_module_alias = 1;
    symbol->alias_target = strdup(module_name);
    symbol->node = NULL;
    symbol->next = table->symbols;
    table->symbols = symbol;
}

Symbol* resolve_module_alias(SymbolTable* table, const char* name) {
    Symbol* symbol = lookup_symbol(table, name);
    if (symbol && symbol->is_module_alias) {
        return symbol;
    }
    return NULL;
}

// Track imported namespaces for qualified function calls.
//
// Two parallel sets exist (issue #243 sealed-scope follow-up):
//
//   imported_namespaces[]      — every namespace registered during
//     orchestration, including ones the user did not explicitly import
//     but that were pulled in transitively by module_merge_into_program's
//     BFS pass. Cloned function bodies of merged modules need this set
//     to resolve their internal qualified calls (e.g. a cloned
//     `client_post_json` calling `json.stringify(...)` even though the
//     user only wrote `import std.http.client`).
//
//   user_explicit_namespaces[] — only namespaces the user explicitly
//     wrote `import` for. Synthetic AST_IMPORT_STATEMENT nodes that
//     module_merge_into_program injects (annotated "synthetic") are
//     skipped. User-code qualified calls resolve against this stricter
//     set so a user can't accidentally call into a transitively-pulled-
//     in module they never asked for.
static char* imported_namespaces[64];
static int namespace_count = 0;
static char* user_explicit_namespaces[64];
static int user_explicit_namespace_count = 0;

// Import alias table: maps short names to dotted qualified names
// for selective imports (e.g. "release" -> "build.release")
#define MAX_IMPORT_ALIASES 512
static struct {
    char short_name[128];
    char qualified_name[256];
} import_aliases[MAX_IMPORT_ALIASES];
static int import_alias_count = 0;

static void add_import_alias(const char* short_name, const char* qualified) {
    if (import_alias_count < MAX_IMPORT_ALIASES) {
        snprintf(import_aliases[import_alias_count].short_name, sizeof(import_aliases[0].short_name), "%s", short_name);
        snprintf(import_aliases[import_alias_count].qualified_name, sizeof(import_aliases[0].qualified_name), "%s", qualified);
        import_alias_count++;
    }
}

static const char* find_import_alias(const char* name) {
    for (int i = 0; i < import_alias_count; i++) {
        if (strcmp(import_aliases[i].short_name, name) == 0) {
            return import_aliases[i].qualified_name;
        }
    }
    return NULL;
}

void register_namespace(const char* ns) {
    if (namespace_count < 64) {
        // Check if already registered
        for (int i = 0; i < namespace_count; i++) {
            if (strcmp(imported_namespaces[i], ns) == 0) return;
        }
        imported_namespaces[namespace_count++] = strdup(ns);
    }
}

// Issue #243 sealed-scope follow-up. Records that the user *explicitly*
// wrote `import <module>` for the given namespace leaf. Called from the
// AST_IMPORT_STATEMENT visitor only when the import is not flagged
// synthetic (`annotation == "synthetic"`) — which means the import came
// from the user's source rather than from the BFS transitive-merge
// pass. Used by user-code qualified-call resolution to reject
// `lib_b.shout()` when the user only wrote `import lib_a`.
static void register_user_explicit_namespace(const char* ns) {
    if (user_explicit_namespace_count < 64) {
        for (int i = 0; i < user_explicit_namespace_count; i++) {
            if (strcmp(user_explicit_namespaces[i], ns) == 0) return;
        }
        user_explicit_namespaces[user_explicit_namespace_count++] = strdup(ns);
    }
}

static int is_user_explicit_namespace(const char* name) {
    for (int i = 0; i < user_explicit_namespace_count; i++) {
        if (strcmp(user_explicit_namespaces[i], name) == 0) return 1;
    }
    return 0;
}

// Forward decl — defined below alongside the global registry it gates.
int is_imported_namespace(const char* name);

// Gate for qualified-call resolution. A qualified call `mod.fn()` is
// allowed if either:
//   - The caller is inside a merged-module body (typechecker propagates
//     SymbolTable::inside_merged_body from the cloned function decl);
//     in that case ANY transitively-merged namespace is fair game,
//     because cloned bodies need to call into their original module's
//     transitive deps to compile.
//   - The caller is user code (inside_merged_body == 0); in that case
//     only namespaces the user explicitly imported are visible. This
//     closes the encapsulation hole left after the round-1 BFS-merge
//     fix for issue #243.
//
// `table` may be NULL during early symbol-table population; treat NULL
// as user-context (the strict path) — early registration paths don't
// resolve qualified user calls, so this is safe.
int is_visible_namespace(const char* name, SymbolTable* table) {
    /* Single channel: the SymbolTable's inside_merged_body flag.
     * Both walkers (the typechecker — which creates per-function
     * child tables — and the type-inference pass — which walks
     * against the global symbol table directly) flip this flag
     * transiently while inside a `is_imported` function body, then
     * restore it on exit. Save/restore is the standard scope-
     * stack pattern; no global mutable state required. */
    if (table && table->inside_merged_body) {
        return is_imported_namespace(name);
    }
    return is_user_explicit_namespace(name);
}

// Per-module selective-import filter.
//
// When a user writes `import std.math (sqrt)`, only `sqrt` should be
// callable via the qualified form `math.sqrt(...)`. Previously this was
// enforced by filtering externs out of the symbol table entirely, but
// that broke Aether-native stdlib wrappers (like `http.get` calling
// `http_get_raw` internally) which need every extern from their own
// module visible in the symbol table regardless of the user's selection.
//
// The fix is two-layer: externs are always added to the symbol table so
// merged module functions can resolve them, and a separate visibility
// filter enforces the user's selection list at qualified-call sites only.
// Unqualified calls from within merged stdlib functions bypass the filter
// because they're implementation detail, not user-facing surface.
typedef struct {
    char* namespace;   // e.g. "math"
    char* func_name;   // short name as written in the user's selection list
} SelectiveImportEntry;

#define MAX_SELECTIVE_IMPORTS 256
static SelectiveImportEntry selective_imports[MAX_SELECTIVE_IMPORTS];
static int selective_import_count = 0;
static int selective_import_modules_count = 0;
// Tracks which namespaces had a selection list at all. Namespace without
// an entry here means "no filter — everything allowed", which matches
// the non-selective import semantics.
static char* selective_import_modules[MAX_SELECTIVE_IMPORTS];

// Tracks namespaces that ALSO have a non-selective import in the same
// file. When both forms coexist (`import std.fs.file_exists` followed
// by `import std.fs`), the non-selective form makes the whole namespace
// accessible — the per-symbol form just adds bare-name binding.
// Without this, is_selective_import_blocked rejected fs.read because
// fs had a filter from the per-symbol import. Issue #252.
static char* nonselective_import_modules[MAX_SELECTIVE_IMPORTS];
static int nonselective_import_modules_count = 0;

static void selective_import_reset(void) {
    for (int i = 0; i < selective_import_count; i++) {
        free(selective_imports[i].namespace);
        free(selective_imports[i].func_name);
    }
    selective_import_count = 0;
    for (int i = 0; i < selective_import_modules_count; i++) {
        free(selective_import_modules[i]);
    }
    selective_import_modules_count = 0;
    for (int i = 0; i < nonselective_import_modules_count; i++) {
        free(nonselective_import_modules[i]);
    }
    nonselective_import_modules_count = 0;
}

static void selective_import_mark_nonselective(const char* ns) {
    for (int i = 0; i < nonselective_import_modules_count; i++) {
        if (strcmp(nonselective_import_modules[i], ns) == 0) return;
    }
    if (nonselective_import_modules_count < MAX_SELECTIVE_IMPORTS) {
        nonselective_import_modules[nonselective_import_modules_count++] = strdup(ns);
    }
}

static int has_nonselective_import(const char* ns) {
    for (int i = 0; i < nonselective_import_modules_count; i++) {
        if (strcmp(nonselective_import_modules[i], ns) == 0) return 1;
    }
    return 0;
}

static void selective_import_mark_module(const char* ns) {
    for (int i = 0; i < selective_import_modules_count; i++) {
        if (strcmp(selective_import_modules[i], ns) == 0) return;
    }
    if (selective_import_modules_count < MAX_SELECTIVE_IMPORTS) {
        selective_import_modules[selective_import_modules_count++] = strdup(ns);
    }
}

static void selective_import_add(const char* ns, const char* func_name) {
    selective_import_mark_module(ns);
    if (selective_import_count < MAX_SELECTIVE_IMPORTS) {
        selective_imports[selective_import_count].namespace = strdup(ns);
        selective_imports[selective_import_count].func_name = strdup(func_name);
        selective_import_count++;
    }
}

static int module_has_selective_filter(const char* ns) {
    for (int i = 0; i < selective_import_modules_count; i++) {
        if (strcmp(selective_import_modules[i], ns) == 0) return 1;
    }
    return 0;
}

// Returns 1 if the user wrote a selective import for `ns` AND `func_name`
// is not in the selection list. Returns 0 when there's no filter, when
// the name is in the list, or when the user *also* wrote a non-selective
// import of the same namespace (in which case the whole namespace is
// accessible — the per-symbol form just adds the bare-name binding).
// Used only at qualified-call sites.
static int is_selective_import_blocked(const char* ns, const char* func_name) {
    if (!module_has_selective_filter(ns)) return 0;
    // If a non-selective import exists for this namespace, it overrides
    // the filter — fs.read should resolve when both `import std.fs.file_exists`
    // and `import std.fs` are present. Issue #252.
    if (has_nonselective_import(ns)) return 0;
    for (int i = 0; i < selective_import_count; i++) {
        if (strcmp(selective_imports[i].namespace, ns) == 0 &&
            strcmp(selective_imports[i].func_name, func_name) == 0) {
            return 0;
        }
    }
    return 1;
}

/* Note: the (selective-import + local-def-with-same-name) shadow
 * check that issue #436 facet A targeted lives at the orchestration
 * layer instead — see check_selective_import_shadow in
 * compiler/aether_module.c. That position catches the collision
 * BEFORE the merger renames the local def, so the diagnostic can
 * cite the original source location and the original import path
 * without reconstructing them post-merge. */

// Check if a symbol is blocked by export visibility.
// Returns 1 if blocked (module has exports and symbol isn't one), 0 if allowed.
static int is_export_blocked(const char* namespace, const char* symbol) {
    if (!global_module_registry) return 0;
    AetherModule* mod = module_find(namespace);
    return (mod && mod->export_count > 0 && !module_is_exported(mod, symbol));
}

int is_imported_namespace(const char* name) {
    for (int i = 0; i < namespace_count; i++) {
        if (strcmp(imported_namespaces[i], name) == 0) return 1;
    }
    return 0;
}

Symbol* lookup_qualified_symbol(SymbolTable* table, const char* qualified_name) {
    if (!table || !qualified_name) return NULL;
    // Split qualified name on '.'
    char* name_copy = strdup(qualified_name);
    char* dot = strchr(name_copy, '.');

    if (dot) {
        *dot = '\0';
        const char* prefix = name_copy;
        const char* suffix = dot + 1;

        // Enforce hide / seal on the prefix before any namespace resolution.
        // `hide http` must block both bare `http` AND `http.get(url)`.
        if (scope_name_is_hidden(table, prefix) ||
            (table->is_sealed && !scope_name_in_whitelist(table, prefix))) {
            free(name_copy);
            return NULL;
        }

        // Check if prefix is a module alias
        Symbol* alias_sym = resolve_module_alias(table, prefix);
        if (alias_sym && alias_sym->alias_target) {
            // Reconstruct with actual module name
            char resolved_name[512];
            snprintf(resolved_name, sizeof(resolved_name), "%s.%s",
                    alias_sym->alias_target, suffix);
            free(name_copy);
            return lookup_symbol(table, resolved_name);
        }

        // Check if prefix is a namespace visible from this scope.
        // Issue #243: user code can only see namespaces it explicitly
        // imported; merged-body code can see all transitively-merged
        // namespaces. is_visible_namespace picks the right set based
        // on the table's inside_merged_body flag.
        // Convert string.new -> string_new
        if (is_visible_namespace(prefix, table)) {
            // Enforce export visibility
            if (is_export_blocked(prefix, suffix)) {
                free(name_copy);
                return NULL;
            }
            // Enforce selective-import visibility: if the user wrote
            // `import std.math (sqrt)` then `math.pow` must be rejected
            // even though `math_pow` is in the symbol table (so that
            // merged stdlib wrappers can call it internally).
            if (is_selective_import_blocked(prefix, suffix)) {
                free(name_copy);
                return NULL;
            }
            char c_func_name[512];
            snprintf(c_func_name, sizeof(c_func_name), "%s_%s", prefix, suffix);
            Symbol* sym = lookup_symbol(table, c_func_name);
            free(name_copy);
            return sym;
        }
    }

    free(name_copy);
    return lookup_symbol(table, qualified_name);
}

void type_error(const char* message, int line, int column) {
    AetherErrorCode code = AETHER_ERR_TYPE_MISMATCH;
    if (strstr(message, "not exported")) code = AETHER_ERR_NOT_EXPORTED;
    else if (strstr(message, "is hidden in this scope") ||
             strstr(message, "it is hidden in this scope")) code = AETHER_ERR_HIDDEN_NAME;
    else if (strstr(message, "Undefined variable")) code = AETHER_ERR_UNDEFINED_VAR;
    else if (strstr(message, "Undefined function") || strstr(message, "Unknown function"))
        code = AETHER_ERR_UNDEFINED_FUNC;
    else if (strstr(message, "Undefined type") || strstr(message, "Unknown type"))
        code = AETHER_ERR_UNDEFINED_TYPE;
    else if (strstr(message, "Redefinition") || strstr(message, "redefinition"))
        code = AETHER_ERR_REDEFINITION;
    aether_error_with_code(message, line, column, code);
    error_count++;
}

void type_warning(const char* message, int line, int column) {
    AetherError w = {
        .filename = NULL, .source_code = NULL,
        .line = line, .column = column,
        .message = message, .suggestion = NULL,
        .context = NULL, .code = AETHER_ERR_NONE
    };
    aether_warning_report(&w);
    warning_count++;
}

// Return a human-readable type name (static buffer — for error messages only)
static const char* type_name(Type* t) {
    if (!t) return "unknown";
    /* C ABI alias — report the spelling the user wrote (size_t, ...). */
    if (t->c_alias) return t->c_alias;
    switch (t->kind) {
        case TYPE_INT:      return "int";
        case TYPE_INT64:    return "long";
        case TYPE_UINT64:   return "uint64";
        case TYPE_UINT32:   return "uint32";
        case TYPE_UINT16:   return "uint16";
        case TYPE_UINT8:    return "uint8";
        case TYPE_DURATION: return "Duration";
        case TYPE_FLOAT:    return "float";
        case TYPE_LONGDOUBLE: return "longdouble";
        case TYPE_BOOL:     return "bool";
        case TYPE_BYTE:     return "byte";
        case TYPE_STRING:   return "string";
        case TYPE_VOID:     return "void";
        case TYPE_PTR:      return "ptr";
        case TYPE_ACTOR_REF: return "actor_ref";
        case TYPE_MESSAGE:  return "message";
        case TYPE_ARRAY:    return "array";
        case TYPE_STRUCT:   return t->struct_name ? t->struct_name : "struct";
        case TYPE_FUNCTION:  return "closure";
        case TYPE_TUPLE:    return "tuple";
        case TYPE_UNKNOWN:  return "unknown";
        default:            return "unknown";
    }
}

static int is_integer_scalar(TypeKind kind) {
    return kind == TYPE_INT || kind == TYPE_INT64 || kind == TYPE_UINT64 ||
           kind == TYPE_UINT32 || kind == TYPE_UINT16 || kind == TYPE_UINT8;
}

static int is_numeric_scalar(TypeKind kind) {
    return kind == TYPE_INT || kind == TYPE_INT64 || kind == TYPE_UINT64 ||
           kind == TYPE_UINT32 || kind == TYPE_UINT16 || kind == TYPE_UINT8 ||
           kind == TYPE_FLOAT || kind == TYPE_LONGDOUBLE;
}

static TypeKind wider_integer_kind(TypeKind a, TypeKind b) {
    if (a == TYPE_UINT64 || b == TYPE_UINT64) return TYPE_UINT64;
    if (a == TYPE_INT64 || b == TYPE_INT64) return TYPE_INT64;
    /* uint32/16/8 all fit int's value range for arithmetic. */
    return TYPE_INT;
}

/* #697: downward integer-width propagation.
 *
 * Type inference is bottom-up, so a sub-expression like `byte << 24`
 * (int << int) is typed 32-bit `int` even when its result is immediately
 * combined into a 64-bit value (`u | (byte << 24)`). C then evaluates the
 * shift in 32-bit — overflowing the sign bit and sign-extending into the
 * high 32 bits of the uint64 — silently corrupting binary-protocol codecs.
 *
 * When a binary integer op resolves to a 64-bit type, walk its operands
 * and re-type any narrower-int arithmetic operand whose VALUE is computed
 * (a nested arith/bitwise/left-shift binary expression) to the same 64-bit
 * kind, so codegen widens it before the operation. Leaf operands
 * (variables, literals, calls) keep their own type and are cast at the use
 * site by codegen. `>>` is intentionally NOT widened: promoting a signed
 * right-shift to unsigned would turn an arithmetic shift into a logical
 * one and change the value of negative operands. Division/modulo are
 * likewise excluded (their 32-bit result is well-defined and widening
 * could only change a value the source already committed to). */
static void propagate_int_width_64(ASTNode* node, TypeKind wide) {
    if (!node || node->type != AST_BINARY_EXPRESSION || !node->node_type) return;
    /* Only widen nodes currently typed as 32-bit int — leave anything
     * already 64-bit (or non-integer) alone. */
    if (node->node_type->kind != TYPE_INT) return;
    if (!node->value) return;
    const char* op = node->value;
    int width_sensitive =
        strcmp(op, "<<") == 0 || strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
        strcmp(op, "*") == 0  || strcmp(op, "|") == 0 || strcmp(op, "&") == 0 ||
        strcmp(op, "^") == 0;
    if (!width_sensitive) return;
    node->node_type->kind = wide;
    if (node->child_count >= 2) {
        propagate_int_width_64(node->children[0], wide);
        propagate_int_width_64(node->children[1], wide);
    }
}

// Count the number of formal parameters of a function definition node
// Skips _ctx parameters (auto-injected by builder context)
static int count_function_params(ASTNode* func) {
    if (!func || func->child_count == 0) return 0;
    int count = 0;
    // Last child is the function body; everything before it may be params or a guard
    for (int i = 0; i < func->child_count - 1; i++) {
        ASTNode* child = func->children[i];
        if (child->type == AST_VARIABLE_DECLARATION ||
            child->type == AST_PATTERN_VARIABLE ||
            child->type == AST_PATTERN_LITERAL) {
            count++;
        }
        // AST_GUARD_CLAUSE is skipped (not a parameter)
    }
    return count;
}

// Returns 1 if the parameter has a default expression attached
// (Phase A2.1 — default function arguments). Default expressions are
// stored as the first child of an AST_PATTERN_VARIABLE / AST_VARIABLE_
// DECLARATION node by the parser, with annotation="has_default" so
// they are distinguishable from struct/list-pattern children.
static int param_has_default(ASTNode* param) {
    return param && param->annotation &&
           strcmp(param->annotation, "has_default") == 0 &&
           param->child_count > 0 && param->children[0] != NULL;
}

// Phase A2.2 (issue #265 close): rewrite source-location intrinsic
// AST nodes inside a cloned default expression to reflect the
// caller's location instead of the function-definition's. Called on
// the clone before it is appended to the call's child list.
//
//   __LINE__  — codegen emits `expr->line`. Overwrite with the
//               call site's line so the caller's site is captured.
//   __FILE__  — codegen reads `gen->source_file` (per-TU global), not
//               `expr->line`, so no per-node rewrite needed; the
//               value is naturally the file holding the call.
//   __func__  — codegen emits the literal C99 `__func__` keyword,
//               which the C compiler resolves to the enclosing C
//               function. Since codegen mirrors Aether function
//               names, this is the calling Aether function's name —
//               exactly the caller-site semantics we want. No
//               rewrite needed.
static void rewrite_caller_site_intrinsics(ASTNode* node, int call_line, int call_column) {
    if (!node) return;
    if (node->type == AST_IDENTIFIER && node->value &&
        strcmp(node->value, "__LINE__") == 0) {
        node->line = call_line;
        node->column = call_column;
    }
    for (int i = 0; i < node->child_count; i++) {
        rewrite_caller_site_intrinsics(node->children[i], call_line, call_column);
    }
}

// Counts required (non-defaulted) parameters. Defaults trail required
// (Python rule: once a default appears, every subsequent parameter
// must also have one). The rule is enforced separately at function-
// declaration time; this helper just counts.
static int count_required_params(ASTNode* func) {
    if (!func || func->child_count == 0) return 0;
    int count = 0;
    for (int i = 0; i < func->child_count - 1; i++) {
        ASTNode* child = func->children[i];
        if (child->type == AST_GUARD_CLAUSE) continue;
        if (child->type != AST_VARIABLE_DECLARATION &&
            child->type != AST_PATTERN_VARIABLE &&
            child->type != AST_PATTERN_LITERAL) continue;
        if (param_has_default(child)) {
            return count;  // first defaulted param ends the required prefix
        }
        count++;
    }
    return count;
}

// Returns 1 if the function's first parameter is _ctx: ptr. Such functions
// can be called either with _ctx passed explicitly (got == expected) or with
// _ctx auto-injected by the builder-DSL runtime (got == expected - 1).
static int has_ctx_first_param(ASTNode* func) {
    if (!func || func->child_count == 0) return 0;
    for (int i = 0; i < func->child_count - 1; i++) {
        ASTNode* child = func->children[i];
        if (child->type == AST_VARIABLE_DECLARATION ||
            child->type == AST_PATTERN_VARIABLE ||
            child->type == AST_PATTERN_LITERAL) {
            return child->value && strcmp(child->value, "_ctx") == 0 &&
                   child->node_type && child->node_type->kind == TYPE_PTR;
        }
    }
    return 0;
}

/* Returns 1 if `init` is an integer literal whose value is outside
 * 0..255, which would silently truncate when assigned to a `byte`-
 * typed slot. Returns 0 if it's in range, not a literal, or not an
 * integer literal at all (those cases are accepted; runtime
 * truncation matches how other narrowings behave).
 *
 * Catches the obvious typo `b: byte = 256` at compile time via a
 * literal-range check + runtime truncate for non-literal int. */
static int byte_assignment_literal_out_of_range(ASTNode* init) {
    if (!init || init->type != AST_LITERAL || !init->value) return 0;
    if (!init->node_type) return 0;
    /* Only int / int64 literals get the range check. Float / string /
     * bool literals fail the type-compatibility check earlier and
     * never reach here. */
    if (init->node_type->kind != TYPE_INT && init->node_type->kind != TYPE_INT64) {
        return 0;
    }
    /* Parse the literal text. Aether accepts decimal, hex (0x), octal
     * (0o), and binary (0b) integer literals. strtoll handles decimal
     * and 0x out of the box; octal/binary need a lighter touch but
     * the values for byte-range bounds (0..255) are small enough that
     * any of these representations is fine to parse. */
    const char* s = init->value;
    long long v = 0;
    if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        v = strtoll(s + 2, NULL, 2);
    } else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
        v = strtoll(s + 2, NULL, 8);
    } else {
        v = strtoll(s, NULL, 0);  /* handles decimal and 0x / 0X */
    }
    return (v < 0 || v > 255) ? 1 : 0;
}

/* Is this expression a compile-time constant — i.e. valid as the
 * RHS of `const X = ...`?
 *
 * Aether's `const` is substitution-at-each-use: the compiler inlines
 * the RHS expression at every reference rather than allocating
 * storage for the value. That works for `const PI = 3.14` (each use
 * inlines a float literal) but is silently wrong for
 * `const G = make_thing()` — every reference to G would re-call
 * make_thing(), allocating fresh state each time. The previous
 * behaviour was to accept it and produce wrong-but-runs code; per
 * Nico's design call we now reject these at compile time.
 *
 * Allowed RHS forms:
 *   - Literal (int / float / bool / string / null)
 *   - Identifier referring to another const
 *   - Unary expression on a const-expression operand
 *   - Binary expression where both operands are const-expressions
 *   - Interpolated string ("${X}") where every interpolated value
 *     is itself a const-expression — useful for building
 *     concatenated literal-ish strings at the const layer
 *
 * Disallowed:
 *   - Function calls (the headline trap Nico flagged)
 *   - Member access (could be a namespaced call; treat as non-const)
 *   - Struct literals (would allocate fresh per use)
 *   - Array / sequence literals
 *   - Anything else
 *
 * `table` may be NULL — when called from the global pre-pass, the
 * const-symbol's binding existence in the table is what we'd check;
 * we treat unknown identifiers as non-const to err on the safe
 * side. The check is robust to NULL.
 */
static int is_const_expression(ASTNode* expr, SymbolTable* table) {
    if (!expr) return 0;
    switch (expr->type) {
        case AST_LITERAL:
        case AST_NULL_LITERAL:
            return 1;
        case AST_IDENTIFIER: {
            /* Allow only if the named symbol is itself a const.
             * The symbol's `is_const` flag is set when the
             * registration path stored it under a const declaration.
             * We don't have a strict bit today, so fall back to
             * "the binding exists in the table" — this is permissive
             * (a non-const identifier slips through) but it doesn't
             * regress against the current behaviour, and the actual
             * trap (`const G = make_thing()`) is gated by the
             * AST_FUNCTION_CALL case below. */
            if (!table || !expr->value) return 1;
            (void)lookup_symbol;  /* avoid unused-when-NULL noise */
            return 1;
        }
        case AST_UNARY_EXPRESSION:
            return expr->child_count > 0 &&
                   is_const_expression(expr->children[0], table);
        case AST_BINARY_EXPRESSION:
            return expr->child_count >= 2 &&
                   is_const_expression(expr->children[0], table) &&
                   is_const_expression(expr->children[1], table);
        case AST_STRING_INTERP: {
            /* Each interp child must itself be a const-expression
             * (the literal-text children are AST_LITERALs which
             * pass; the substituted-value children must too). */
            for (int i = 0; i < expr->child_count; i++) {
                if (!is_const_expression(expr->children[i], table)) return 0;
            }
            return 1;
        }
        case AST_FUNCTION_CALL:
        case AST_MEMBER_ACCESS:
        case AST_STRUCT_LITERAL:
        case AST_ARRAY_LITERAL:
            return 0;
        default:
            return 0;
    }
}

// Type compatibility functions
int is_type_compatible(Type* from, Type* to) {
    if (!from || !to) return 0;
    
    // Unknown types match anything (for inference)
    if (from->kind == TYPE_UNKNOWN || to->kind == TYPE_UNKNOWN) return 1;
    
    // Exact match
    if (types_equal(from, to)) return 1;
    
    // Numeric conversions
    if (from->kind == TYPE_INT && to->kind == TYPE_FLOAT) return 1;
    if (from->kind == TYPE_FLOAT && to->kind == TYPE_INT) return 1;
    // int promotes to long/uint64 without loss
    if (from->kind == TYPE_INT && to->kind == TYPE_INT64) return 1;
    if (from->kind == TYPE_INT && to->kind == TYPE_UINT64) return 1;
    if (from->kind == TYPE_INT64 && to->kind == TYPE_INT) return 1;
    if (from->kind == TYPE_UINT64 && to->kind == TYPE_INT) return 1;
    if (from->kind == TYPE_UINT64 && to->kind == TYPE_INT64) return 1;
    if (from->kind == TYPE_INT64 && to->kind == TYPE_UINT64) return 1;
    // uint32/uint16/uint8 — the underlying kinds of the narrow C ABI
    // aliases (uint32_t, ...). They interconvert freely with every
    // integer scalar; the C compiler does the actual narrowing.
    {
        int from_small = (from->kind == TYPE_UINT32 ||
                          from->kind == TYPE_UINT16 ||
                          from->kind == TYPE_UINT8);
        int to_small   = (to->kind == TYPE_UINT32 ||
                          to->kind == TYPE_UINT16 ||
                          to->kind == TYPE_UINT8);
        if (from_small && is_integer_scalar(to->kind)) return 1;
        if (to_small && is_integer_scalar(from->kind)) return 1;
    }
    // long <-> float compatibility
    if (from->kind == TYPE_INT64 && to->kind == TYPE_FLOAT) return 1;
    if (from->kind == TYPE_FLOAT && to->kind == TYPE_INT64) return 1;
    // #749: longdouble interconverts with int / int64 / uint64 / float.
    // It is the widest C float; the C compiler performs the conversion.
    if (from->kind == TYPE_LONGDOUBLE &&
        (to->kind == TYPE_INT || to->kind == TYPE_INT64 ||
         to->kind == TYPE_UINT64 || to->kind == TYPE_FLOAT)) return 1;
    if (to->kind == TYPE_LONGDOUBLE &&
        (from->kind == TYPE_INT || from->kind == TYPE_INT64 ||
         from->kind == TYPE_UINT64 || from->kind == TYPE_FLOAT)) return 1;

    // Array compatibility
    if (from->kind == TYPE_ARRAY && to->kind == TYPE_ARRAY) {
        return is_type_compatible(from->element_type, to->element_type);
    }
    
    // Actor reference compatibility
    // Bare actor_ref (no type parameter) is compatible with any actor_ref
    if (from->kind == TYPE_ACTOR_REF && to->kind == TYPE_ACTOR_REF) {
        if (!from->element_type || !to->element_type) return 1;
        return is_type_compatible(from->element_type, to->element_type);
    }

    // Actor refs stored in int/ptr state fields (common wiring pattern: state ref = 0)
    if (from->kind == TYPE_ACTOR_REF &&
        (to->kind == TYPE_INT || to->kind == TYPE_INT64 || to->kind == TYPE_PTR)) return 1;
    if (to->kind == TYPE_ACTOR_REF &&
        (from->kind == TYPE_INT || from->kind == TYPE_INT64 || from->kind == TYPE_PTR)) return 1;

    // int ↔ ptr compatibility (e.g. x = 0 then x = ptr_func(), or passing 0 to ptr param)
    if (from->kind == TYPE_INT && to->kind == TYPE_PTR) return 1;
    if (from->kind == TYPE_PTR && to->kind == TYPE_INT) return 1;

    /* fn ↔ ptr compatibility (the Option B1 boxing shape). A closure
     * stored as ptr (struct field, list/map element) and recovered
     * back to a `fn` slot — codegen wraps the cross-direction
     * transitions in _aether_box_closure / _aether_unbox_closure
     * (compiler/codegen/codegen_expr.c at the AST_FUNCTION_CALL
     * arg-emit loop and at struct-field assignment).
     *
     * Without this rule, `h.cb = c` where `h.cb: ptr` and `c: fn`
     * fails at typecheck with E0200 — exactly the gap the aether-ui
     * CVG port hit in `grammar_element.ae` for 17 callback fields
     * (`onClick`, `bindFill`, `whenPredicate`, etc.).  Raw-C-fn-
     * pointer form (TYPE_FUNCTION with is_fnptr=1, storage = void*)
     * has always been ptr-compatible by virtue of the `as fn`
     * cast contract; this extends the same compatibility to the
     * `_AeClosure`-shaped form (is_fnptr=0). */
    if (from->kind == TYPE_FUNCTION && to->kind == TYPE_PTR) return 1;
    if (from->kind == TYPE_PTR && to->kind == TYPE_FUNCTION) return 1;

    // byte → int / int64 / float: safe widenings.
    // Reverse direction (int → byte) is intentionally NOT here — it's
    // gated at the assignment site so out-of-range integer literals
    // produce a compile-time error rather than silent truncation.
    // See `check_byte_assignment_literal_range` in the assignment path.
    if (from->kind == TYPE_BYTE &&
        (to->kind == TYPE_INT || to->kind == TYPE_INT64 ||
         to->kind == TYPE_UINT64 || to->kind == TYPE_FLOAT)) return 1;
    // int → byte allowed for non-literal int (runtime truncate is the
    // contract, matching how other narrowings behave). Literal-range
    // check happens elsewhere.
    if (from->kind == TYPE_INT && to->kind == TYPE_BYTE) return 1;
    if (from->kind == TYPE_INT64 && to->kind == TYPE_BYTE) return 1;

    return 0;
}

int is_assignable(Type* from, Type* to) {
    return is_type_compatible(from, to);
}

int is_callable(Type* type) {
    if (!type) return 0;
    switch (type->kind) {
        case TYPE_INT:
        case TYPE_INT64:
        case TYPE_UINT64:
        case TYPE_DURATION:
        case TYPE_FLOAT:
        case TYPE_LONGDOUBLE:
        case TYPE_BOOL:
        case TYPE_STRING:
        case TYPE_VOID:
        case TYPE_ARRAY:
        case TYPE_WILDCARD:
        case TYPE_PTR:
            return 0;
        default:
            return 1;
    }
}

// Type inference functions
Type* infer_type(ASTNode* expr, SymbolTable* table) {
    if (!expr) return NULL;
    
    switch (expr->type) {
        case AST_LITERAL:
            return clone_type(expr->node_type);

        case AST_TUPLE_UNWRAP: {
            /* `expr!` — unwrap-or-trap: the result is the operand tuple's
             * FIRST slot type. Operand must be a tuple (a (value, err)
             * return); a non-tuple operand is a type error reported in
             * typecheck_expression. */
            if (expr->child_count == 0 || !expr->children[0])
                return create_type(TYPE_UNKNOWN);
            Type* operand = infer_type(expr->children[0], table);
            Type* result = create_type(TYPE_UNKNOWN);
            if (operand && operand->kind == TYPE_TUPLE &&
                operand->tuple_count >= 1 && operand->tuple_types[0]) {
                free_type(result);
                result = clone_type(operand->tuple_types[0]);
            }
            if (operand) free_type(operand);
            return result;
        }

        case AST_NULL_LITERAL:
            return create_type(TYPE_PTR);

        case AST_SIZEOF:
        case AST_OFFSETOF:
            // Compile-time layout builtins; both lower to a C int.
            return create_type(TYPE_INT);

        case AST_VA_START:
            // Opaque va_list cookie, modelled as a ptr.
            return create_type(TYPE_PTR);

        case AST_VA_ARG:
            // Yields whatever C type the call requested (parser set it).
            return expr->node_type ? clone_type(expr->node_type)
                                   : create_type(TYPE_PTR);

        case AST_VA_END:
            return create_type(TYPE_VOID);

        case AST_PTR_AS_STRUCT_CAST: {
            /* `expr as *StructName` — view the operand as a pointer to
             * StructName. Operand must be ptr-typed. Result is
             * TYPE_PTR with element_type = TYPE_STRUCT{name}, mirroring
             * how TYPE_ACTOR_REF carries its underlying struct. The
             * spelled type at the source level is `*StructName`. */
            if (!expr->value) return create_type(TYPE_UNKNOWN);
            if (expr->child_count == 0 || !expr->children[0])
                return create_type(TYPE_UNKNOWN);
            Type* operand = infer_type(expr->children[0], table);
            if (operand) {
                /* Aether already coerces int<->ptr in places, so we
                 * accept ptr OR int64 OR int as the operand. anything
                 * else is a type error (a struct value isn't a ptr;
                 * neither is a string). */
                int operand_ok = operand->kind == TYPE_PTR ||
                                 operand->kind == TYPE_INT ||
                                 operand->kind == TYPE_INT64 ||
                                 operand->kind == TYPE_UNKNOWN;
                free_type(operand);
                if (!operand_ok) {
                    type_error("`as *T` cast operand must be a `ptr` value",
                               expr->line, expr->column);
                    return create_type(TYPE_UNKNOWN);
                }
            }
            /* Validate the named struct exists. */
            Symbol* struct_sym = lookup_symbol(table, expr->value);
            if (!struct_sym || !struct_sym->type ||
                struct_sym->type->kind != TYPE_STRUCT) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "`as *%s` — '%s' is not a struct type", expr->value, expr->value);
                type_error(msg, expr->line, expr->column);
                return create_type(TYPE_UNKNOWN);
            }
            Type* inner = create_type(TYPE_STRUCT);
            inner->struct_name = strdup(expr->value);
            Type* result = create_type(TYPE_PTR);
            result->element_type = inner;
            return result;
        }

        case AST_PTR_AS_ARRAY_CAST: {
            /* `expr as T[]` — view the operand as a typed C array.
             * Operand must be ptr-typed. Result type is the TYPE_ARRAY
             * the parser stashed on expr->node_type. AST_ARRAY_ACCESS
             * on the result emits `((T*)(expr))[i]` which scales by
             * sizeof(T) via C semantics. */
            if (expr->child_count == 0 || !expr->children[0])
                return create_type(TYPE_UNKNOWN);
            Type* operand = infer_type(expr->children[0], table);
            if (operand) {
                int operand_ok = operand->kind == TYPE_PTR ||
                                 operand->kind == TYPE_INT ||
                                 operand->kind == TYPE_INT64 ||
                                 operand->kind == TYPE_UNKNOWN;
                free_type(operand);
                if (!operand_ok) {
                    type_error("`as T[]` cast operand must be a `ptr` value",
                               expr->line, expr->column);
                    return create_type(TYPE_UNKNOWN);
                }
            }
            if (expr->node_type) return clone_type(expr->node_type);
            return create_type(TYPE_UNKNOWN);
        }

        case AST_PTR_AS_FN_CAST: {
            /* `expr as fn(T1, T2, ...) -> R` — view the operand as a
             * typed C function pointer. Operand must be ptr-typed
             * (or int/int64, since Aether already coerces those to
             * ptr in places). Result type is the TYPE_FUNCTION that
             * the parser populated on expr->node_type. */
            if (expr->child_count == 0 || !expr->children[0])
                return create_type(TYPE_UNKNOWN);
            Type* operand = infer_type(expr->children[0], table);
            if (operand) {
                int operand_ok = operand->kind == TYPE_PTR ||
                                 operand->kind == TYPE_INT ||
                                 operand->kind == TYPE_INT64 ||
                                 operand->kind == TYPE_UINT64 ||
                                 operand->kind == TYPE_UNKNOWN;
                free_type(operand);
                if (!operand_ok) {
                    type_error("`as fn(...)` cast operand must be a `ptr` value",
                               expr->line, expr->column);
                    return create_type(TYPE_UNKNOWN);
                }
            }
            /* The parser stashed the TYPE_FUNCTION (with signature)
             * on the node — return it (copy preserved). */
            if (expr->node_type) {
                /* Don't free — caller may inspect; return a borrow
                 * via a shallow clone to keep ownership clean. */
                Type* dup = create_type(TYPE_FUNCTION);
                dup->is_fnptr = 1;  /* cast → raw fn-pointer */
                dup->param_count = expr->node_type->param_count;
                if (dup->param_count > 0) {
                    dup->param_types = malloc((size_t)dup->param_count * sizeof(Type*));
                    for (int i = 0; i < dup->param_count; i++) {
                        Type* src = expr->node_type->param_types[i];
                        Type* d = create_type(src->kind);
                        d->struct_name = src->struct_name ? strdup(src->struct_name) : NULL;
                        d->element_type = src->element_type;  /* shallow */
                        dup->param_types[i] = d;
                    }
                }
                if (expr->node_type->return_type) {
                    Type* src = expr->node_type->return_type;
                    Type* d = create_type(src->kind);
                    d->struct_name = src->struct_name ? strdup(src->struct_name) : NULL;
                    d->element_type = src->element_type;
                    dup->return_type = d;
                }
                return dup;
            }
            Type* fb = create_type(TYPE_FUNCTION);
            fb->is_fnptr = 1;
            return fb;
        }

        case AST_IF_EXPRESSION:
            // Type is the type of the then-branch expression
            if (expr->child_count >= 2) {
                return infer_type(expr->children[1], table);
            }
            return create_type(TYPE_UNKNOWN);

        case AST_STRING_INTERP:
            return create_type(TYPE_STRING);

        case AST_MATCH_STATEMENT:
            // Return type of first arm's result expression
            if (expr->node_type && expr->node_type->kind != TYPE_UNKNOWN) {
                return clone_type(expr->node_type);
            }
            if (expr->child_count >= 2) {
                ASTNode* first_arm = expr->children[1];
                if (first_arm && first_arm->child_count >= 2) {
                    return infer_type(first_arm->children[1], table);
                }
            }
            return create_type(TYPE_UNKNOWN);

        case AST_ARRAY_LITERAL:
            // Return the inferred array type
            return expr->node_type ? clone_type(expr->node_type) : create_type(TYPE_UNKNOWN);
            
        case AST_IDENTIFIER: {
            Symbol* symbol = lookup_symbol(table, expr->value);
            return (symbol && symbol->type) ? clone_type(symbol->type) : create_type(TYPE_UNKNOWN);
        }
        
        case AST_BINARY_EXPRESSION:
            return infer_binary_type(expr->children[0], expr->children[1],
                                   get_token_type_from_string(expr->value));

        case AST_UNARY_EXPRESSION:
            return infer_unary_type(expr->children[0], 
                                  get_token_type_from_string(expr->value));
            
        case AST_FUNCTION_CALL: {
            Symbol* symbol = lookup_qualified_symbol(table, expr->value);
            if (symbol && symbol->is_function && symbol->type
                && symbol->type->kind != TYPE_VOID
                && symbol->type->kind != TYPE_UNKNOWN) {
                return clone_type(symbol->type);
            }
            /* Calling an fn-typed local: `fp(a, b)` where `fp` is a
             * variable typed `fn(T1, T2, ...) -> R` (typically from
             * `... as fn(...) -> R`).  Return R as the call's type. */
            if (symbol && !symbol->is_function && symbol->type &&
                symbol->type->kind == TYPE_FUNCTION &&
                symbol->type->return_type) {
                return clone_type(symbol->type->return_type);
            }
            return create_type(TYPE_UNKNOWN);
        }
        
        case AST_ACTOR_REF:
            return create_type(TYPE_ACTOR_REF);
            
        case AST_STRUCT_LITERAL:
            // Return the struct type from node_type (set during type inference)
            return expr->node_type ? clone_type(expr->node_type) : create_type(TYPE_UNKNOWN);
            
        case AST_ARRAY_ACCESS:
            // Return the element type from array access (set during type inference)
            return expr->node_type ? clone_type(expr->node_type) : create_type(TYPE_UNKNOWN);

        case AST_MEMBER_ACCESS: {
            // Enforce export visibility before resolving. Use the
            // strict per-scope `is_visible_namespace` check (issue #243
            // sealed scopes) so user code that did not import a
            // transitively-pulled-in module gets a clear "not visible"
            // error rather than the looser "not exported" message.
            if (expr->child_count > 0 && expr->children[0] &&
                expr->children[0]->type == AST_IDENTIFIER && expr->children[0]->value &&
                is_visible_namespace(expr->children[0]->value, table) && expr->value &&
                is_export_blocked(expr->children[0]->value, expr->value)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "'%s' is not exported from module '%s'",
                         expr->value, expr->children[0]->value);
                type_error(msg, expr->line, expr->column);
                return create_type(TYPE_UNKNOWN);
            }
            // If node_type already set, use it
            if (expr->node_type && expr->node_type->kind != TYPE_UNKNOWN)
                return clone_type(expr->node_type);
            // Namespace-qualified constant access: mymath.PI_APPROX -> mymath_PI_APPROX
            if (expr->child_count > 0 && expr->children[0] &&
                expr->children[0]->type == AST_IDENTIFIER && expr->children[0]->value &&
                is_visible_namespace(expr->children[0]->value, table) && expr->value) {
                char qualified[512];
                snprintf(qualified, sizeof(qualified), "%s_%s",
                         expr->children[0]->value, expr->value);
                Symbol* sym = lookup_symbol(table, qualified);
                if (sym && sym->type) {
                    // Rewrite node in-place for codegen
                    expr->type = AST_IDENTIFIER;
                    free(expr->value);
                    expr->value = strdup(qualified);
                    expr->node_type = clone_type(sym->type);
                    return clone_type(sym->type);
                }
            }
            // Look up the struct/actor type and find the field type
            if (expr->child_count > 0 && expr->children[0]) {
                Type* base_type = infer_type(expr->children[0], table);
                if (base_type && base_type->kind == TYPE_DURATION) {
                    Type* out = NULL;
                    if (expr->value && strcmp(expr->value, "ns") == 0) {
                        out = create_type(TYPE_INT64);
                    } else if (expr->value &&
                               (strcmp(expr->value, "us") == 0 || strcmp(expr->value, "ms") == 0 ||
                                strcmp(expr->value, "s") == 0 || strcmp(expr->value, "m") == 0 ||
                                strcmp(expr->value, "h") == 0 || strcmp(expr->value, "d") == 0)) {
                        out = create_type(TYPE_FLOAT);
                    }
                    free_type(base_type);
                    if (out) return out;
                    return create_type(TYPE_UNKNOWN);
                }
                // Struct field lookup
                if (base_type && base_type->kind == TYPE_STRUCT && base_type->struct_name) {
                    Symbol* struct_sym = lookup_symbol(table, base_type->struct_name);
                    if (struct_sym && struct_sym->node) {
                        ASTNode* struct_def = struct_sym->node;
                        for (int fi = 0; fi < struct_def->child_count; fi++) {
                            ASTNode* field = struct_def->children[fi];
                            if (field && field->value && strcmp(field->value, expr->value) == 0) {
                                if (field->node_type && field->node_type->kind != TYPE_UNKNOWN)
                                    return clone_type(field->node_type);
                                break;
                            }
                        }
                    }
                }
                // Pointer-to-struct field lookup: `*StructName` field access
                // resolves through the underlying struct's field list, same
                // as a value-struct. Only the codegen access mode differs
                // (`->field` vs `.field`).
                if (base_type && base_type->kind == TYPE_PTR && base_type->element_type &&
                    base_type->element_type->kind == TYPE_STRUCT &&
                    base_type->element_type->struct_name) {
                    Symbol* struct_sym = lookup_symbol(table, base_type->element_type->struct_name);
                    if (struct_sym && struct_sym->node) {
                        ASTNode* struct_def = struct_sym->node;
                        for (int fi = 0; fi < struct_def->child_count; fi++) {
                            ASTNode* field = struct_def->children[fi];
                            if (field && field->value && strcmp(field->value, expr->value) == 0) {
                                if (field->node_type && field->node_type->kind != TYPE_UNKNOWN)
                                    return clone_type(field->node_type);
                                break;
                            }
                        }
                    }
                }
                // Actor ref field lookup — look up state declarations in the actor definition
                if (base_type && base_type->kind == TYPE_ACTOR_REF && base_type->element_type &&
                    base_type->element_type->kind == TYPE_STRUCT && base_type->element_type->struct_name) {
                    Symbol* actor_sym = lookup_symbol(table, base_type->element_type->struct_name);
                    if (actor_sym && actor_sym->node) {
                        ASTNode* actor_def = actor_sym->node;
                        for (int fi = 0; fi < actor_def->child_count; fi++) {
                            ASTNode* field = actor_def->children[fi];
                            if (field && field->type == AST_STATE_DECLARATION &&
                                field->value && strcmp(field->value, expr->value) == 0) {
                                if (field->node_type && field->node_type->kind != TYPE_UNKNOWN)
                                    return clone_type(field->node_type);
                                break;
                            }
                        }
                    }
                }
            }
            return create_type(TYPE_UNKNOWN);
        }

        default:
            return create_type(TYPE_UNKNOWN);
    }
}

Type* infer_binary_type(ASTNode* left, ASTNode* right, AeTokenType operator) {
    Type* left_type = left ? left->node_type : NULL;
    Type* right_type = right ? right->node_type : NULL;

    // Duration comparisons are only valid against another Duration.
    if (operator == TOKEN_EQUALS || operator == TOKEN_NOT_EQUALS ||
        operator == TOKEN_LESS || operator == TOKEN_LESS_EQUAL ||
        operator == TOKEN_GREATER || operator == TOKEN_GREATER_EQUAL) {
        if (left_type && right_type &&
            (left_type->kind == TYPE_DURATION || right_type->kind == TYPE_DURATION)) {
            if (left_type->kind == TYPE_DURATION && right_type->kind == TYPE_DURATION) {
                return create_type(TYPE_BOOL);
            }
            return create_type(TYPE_UNKNOWN);
        }
    }

    // Comparison and logical operators always produce bool, even with unknown operands
    switch (operator) {
        case TOKEN_EQUALS:
        case TOKEN_NOT_EQUALS:
        case TOKEN_LESS:
        case TOKEN_LESS_EQUAL:
        case TOKEN_GREATER:
        case TOKEN_GREATER_EQUAL:
        case TOKEN_AND:
        case TOKEN_OR:
            return create_type(TYPE_BOOL);
        default:
            break;
    }

    if (!left_type || !right_type) return create_type(TYPE_UNKNOWN);

    switch (operator) {
        case TOKEN_PLUS:
        case TOKEN_MINUS:
        case TOKEN_MULTIPLY:
        case TOKEN_DIVIDE:
        case TOKEN_MODULO:
            // Numeric operations
            if (left_type->kind == TYPE_UNKNOWN || right_type->kind == TYPE_UNKNOWN) {
                // If either type is unknown (e.g., unresolved parameter), allow it
                return create_type(TYPE_UNKNOWN);
            }
            if ((operator == TOKEN_PLUS || operator == TOKEN_MINUS) &&
                left_type->kind == TYPE_DURATION && right_type->kind == TYPE_DURATION) {
                return create_type(TYPE_DURATION);
            }
            if (operator == TOKEN_MULTIPLY &&
                ((left_type->kind == TYPE_DURATION && is_numeric_scalar(right_type->kind)) ||
                 (right_type->kind == TYPE_DURATION && is_numeric_scalar(left_type->kind)))) {
                return create_type(TYPE_DURATION);
            }
            if (operator == TOKEN_DIVIDE &&
                left_type->kind == TYPE_DURATION && is_numeric_scalar(right_type->kind)) {
                return create_type(TYPE_DURATION);
            }
            if (operator == TOKEN_DIVIDE &&
                left_type->kind == TYPE_DURATION && right_type->kind == TYPE_DURATION) {
                return create_type(TYPE_FLOAT);
            }
            if (left_type->kind == TYPE_DURATION || right_type->kind == TYPE_DURATION) {
                return create_type(TYPE_UNKNOWN);
            }
            // #749: longdouble is the widest numeric — wins over float/int.
            if (left_type->kind == TYPE_LONGDOUBLE || right_type->kind == TYPE_LONGDOUBLE) {
                return create_type(TYPE_LONGDOUBLE);
            }
            if (left_type->kind == TYPE_FLOAT || right_type->kind == TYPE_FLOAT) {
                return create_type(TYPE_FLOAT);
            }
            // byte arithmetic: byte op byte → byte; mixed byte/int → int
            // (the wider type wins, matching how int op int64 → int64).
            // This keeps `b1 + b2` typed as byte for tag-byte / NaN-boxing
            // patterns while letting `b + 1` widen for general arithmetic.
            if (left_type->kind == TYPE_BYTE && right_type->kind == TYPE_BYTE) {
                return create_type(TYPE_BYTE);
            }
            if ((left_type->kind == TYPE_BYTE || right_type->kind == TYPE_BYTE) &&
                (is_integer_scalar(left_type->kind) || is_integer_scalar(right_type->kind))) {
                /* Pick the wider int side */
                return create_type(wider_integer_kind(left_type->kind, right_type->kind));
            }
            if (left_type->kind == TYPE_INT && right_type->kind == TYPE_INT) {
                return create_type(TYPE_INT);
            }
            // ptr arithmetic. For + and -, result is ptr (real
            // pointer offset arithmetic — used by C interop like the
            // mquickjs port). For * / %, fall back to int (legacy
            // "ptr-as-int" use case where ptr boxed an integer
            // value). ptr - ptr → int64 (ptrdiff_t-style). The old
            // unconditional `→ int` rule silently truncated 64-bit
            // pointers when stored in inferred-type locals.
            if (operator == TOKEN_PLUS || operator == TOKEN_MINUS) {
                if ((left_type->kind == TYPE_PTR && (right_type->kind == TYPE_INT || right_type->kind == TYPE_INT64)) ||
                    ((left_type->kind == TYPE_INT || left_type->kind == TYPE_INT64) && right_type->kind == TYPE_PTR)) {
                    return create_type(TYPE_PTR);
                }
                if (left_type->kind == TYPE_PTR && right_type->kind == TYPE_PTR &&
                    operator == TOKEN_MINUS) {
                    return create_type(TYPE_INT64);
                }
                /* Array + int → ptr (and the int + array commutative
                 * form).  Used when an `extern struct` flex-array
                 * field is treated as a byte buffer with offset
                 * arithmetic: `(p as *JSString).buf + idx` is the
                 * idiomatic way to materialise a pointer-to-element
                 * for handoff to a memcpy-style C extern.  In C the
                 * array decays to a pointer at expression level, so
                 * this rule matches the C semantics. */
                if ((left_type->kind == TYPE_ARRAY && (right_type->kind == TYPE_INT || right_type->kind == TYPE_INT64)) ||
                    ((left_type->kind == TYPE_INT || left_type->kind == TYPE_INT64) && right_type->kind == TYPE_ARRAY)) {
                    return create_type(TYPE_PTR);
                }
            }
            if ((left_type->kind == TYPE_PTR && right_type->kind == TYPE_INT) ||
                (left_type->kind == TYPE_INT && right_type->kind == TYPE_PTR) ||
                (left_type->kind == TYPE_PTR && right_type->kind == TYPE_PTR)) {
                return create_type(TYPE_INT);
            }
            // Promote integer operations to the wider integer type.
            if (is_integer_scalar(left_type->kind) && is_integer_scalar(right_type->kind)) {
                return create_type(wider_integer_kind(left_type->kind, right_type->kind));
            }
            if (left_type->kind == TYPE_STRING && right_type->kind == TYPE_STRING) {
                return create_type(TYPE_STRING);
            }
            break;
            
        case TOKEN_AMPERSAND:
        case TOKEN_PIPE:
        case TOKEN_CARET:
        case TOKEN_LSHIFT:
        case TOKEN_RSHIFT:
            // Bitwise operations: integer operands, result matches wider type
            if (left_type->kind == TYPE_UNKNOWN || right_type->kind == TYPE_UNKNOWN) {
                return create_type(TYPE_UNKNOWN);
            }
            // byte op byte → byte. NaN-boxing / packed-tag code does
            // `tag & 0x07`, `flags | 0x80`, etc. — keeping the result
            // typed `byte` lets the value flow back into a `byte` field
            // without an explicit narrowing.
            if (left_type->kind == TYPE_BYTE && right_type->kind == TYPE_BYTE) {
                return create_type(TYPE_BYTE);
            }
            if ((left_type->kind == TYPE_BYTE || right_type->kind == TYPE_BYTE) &&
                (is_integer_scalar(left_type->kind) || is_integer_scalar(right_type->kind))) {
                return create_type(wider_integer_kind(left_type->kind, right_type->kind));
            }
            if (left_type->kind == TYPE_INT && right_type->kind == TYPE_INT) {
                return create_type(TYPE_INT);
            }
            if (is_integer_scalar(left_type->kind) && is_integer_scalar(right_type->kind)) {
                return create_type(wider_integer_kind(left_type->kind, right_type->kind));
            }
            break;

        case TOKEN_ASSIGN:
            return clone_type(right_type);

        default:
            break;
    }
    
    return create_type(TYPE_UNKNOWN);
}

Type* infer_unary_type(ASTNode* operand, AeTokenType operator) {
    Type* operand_type = operand ? operand->node_type : NULL;
    if (!operand_type) return create_type(TYPE_UNKNOWN);
    
    switch (operator) {
        case TOKEN_NOT:
            return create_type(TYPE_BOOL);
            
        case TOKEN_TILDE:
            return clone_type(operand_type); // Bitwise NOT: same integer type

        case TOKEN_MINUS:
        case TOKEN_INCREMENT:
        case TOKEN_DECREMENT:
            return clone_type(operand_type); // Same type as operand
            
        default:
            return create_type(TYPE_UNKNOWN);
    }
}

AeTokenType get_token_type_from_string(const char* str) {
    if (!str) return TOKEN_ERROR;
    
    if (strcmp(str, "+") == 0) return TOKEN_PLUS;
    if (strcmp(str, "-") == 0) return TOKEN_MINUS;
    if (strcmp(str, "*") == 0) return TOKEN_MULTIPLY;
    if (strcmp(str, "/") == 0) return TOKEN_DIVIDE;
    if (strcmp(str, "%") == 0) return TOKEN_MODULO;
    if (strcmp(str, "==") == 0) return TOKEN_EQUALS;
    if (strcmp(str, "!=") == 0) return TOKEN_NOT_EQUALS;
    if (strcmp(str, "<") == 0) return TOKEN_LESS;
    if (strcmp(str, "<=") == 0) return TOKEN_LESS_EQUAL;
    if (strcmp(str, ">") == 0) return TOKEN_GREATER;
    if (strcmp(str, ">=") == 0) return TOKEN_GREATER_EQUAL;
    if (strcmp(str, "&&") == 0) return TOKEN_AND;
    if (strcmp(str, "||") == 0) return TOKEN_OR;
    if (strcmp(str, "=") == 0) return TOKEN_ASSIGN;
    if (strcmp(str, "!") == 0) return TOKEN_NOT;
    if (strcmp(str, "++") == 0) return TOKEN_INCREMENT;
    if (strcmp(str, "--") == 0) return TOKEN_DECREMENT;
    if (strcmp(str, "&") == 0) return TOKEN_AMPERSAND;
    if (strcmp(str, "|") == 0) return TOKEN_PIPE;
    if (strcmp(str, "^") == 0) return TOKEN_CARET;
    if (strcmp(str, "~") == 0) return TOKEN_TILDE;
    if (strcmp(str, "<<") == 0) return TOKEN_LSHIFT;
    if (strcmp(str, ">>") == 0) return TOKEN_RSHIFT;

    return TOKEN_ERROR;
}

// --- Unused variable analysis ---

#define MAX_TRACKED_VARS 256

typedef struct {
    const char* name;
    int line;
    int col;
    int used;
} TrackedVar;

// Collect all AST_IDENTIFIER references in a subtree (excluding declarations)
static void collect_references(ASTNode* node, TrackedVar* vars, int var_count) {
    if (!node) return;

    // An identifier in expression position is a reference
    if (node->type == AST_IDENTIFIER && node->value) {
        for (int i = 0; i < var_count; i++) {
            if (strcmp(vars[i].name, node->value) == 0) {
                vars[i].used = 1;
            }
        }
    }

    // A call through a local is a use of that local: `g(p)` where `g` was
    // assigned via `f as fn(ptr) -> int` parses as AST_FUNCTION_CALL with
    // the callee name in node->value (no AST_IDENTIFIER child), so match
    // the call name against tracked variables too.
    if (node->type == AST_FUNCTION_CALL && node->value) {
        for (int i = 0; i < var_count; i++) {
            if (strcmp(vars[i].name, node->value) == 0) {
                vars[i].used = 1;
            }
        }
    }

    // Match statements with list patterns implicitly reference <expr>_len variables
    // (the codegen generates: int _match_len = <expr>_len;)
    if (node->type == AST_MATCH_STATEMENT && node->child_count > 0) {
        ASTNode* match_expr = node->children[0];
        if (match_expr && match_expr->type == AST_IDENTIFIER && match_expr->value) {
            char len_name[256];
            snprintf(len_name, sizeof(len_name), "%s_len", match_expr->value);
            for (int i = 0; i < var_count; i++) {
                if (strcmp(vars[i].name, len_name) == 0) {
                    vars[i].used = 1;
                }
            }
        }
    }

    // For variable declarations, the RHS is a reference but the name itself is not
    if (node->type == AST_VARIABLE_DECLARATION) {
        // Only walk children (RHS expression), not the declaration name
        for (int i = 0; i < node->child_count; i++) {
            collect_references(node->children[i], vars, var_count);
        }
        return;
    }

    for (int i = 0; i < node->child_count; i++) {
        collect_references(node->children[i], vars, var_count);
    }
}

// Collect variable declarations from a block (non-recursive into nested functions)
static int collect_declarations(ASTNode* node, TrackedVar* vars, int var_count) {
    if (!node || var_count >= MAX_TRACKED_VARS) return var_count;

    if (node->type == AST_VARIABLE_DECLARATION && node->value) {
        // Skip _ prefixed names (intentional discard)
        if (node->value[0] != '_') {
            vars[var_count].name = node->value;
            vars[var_count].line = node->line;
            vars[var_count].col = node->column;
            vars[var_count].used = 0;
            var_count++;
        }
    }

    // Don't recurse into nested function definitions or actor definitions
    if (node->type == AST_FUNCTION_DEFINITION || node->type == AST_BUILDER_FUNCTION || node->type == AST_ACTOR_DEFINITION) {
        return var_count;
    }

    for (int i = 0; i < node->child_count; i++) {
        var_count = collect_declarations(node->children[i], vars, var_count);
    }
    return var_count;
}

static void check_unused_variables(ASTNode* body) {
    if (!body) return;

    TrackedVar vars[MAX_TRACKED_VARS];
    int var_count = 0;

    // Collect declarations
    for (int i = 0; i < body->child_count; i++) {
        var_count = collect_declarations(body->children[i], vars, var_count);
    }

    if (var_count == 0) return;

    // Collect references
    for (int i = 0; i < body->child_count; i++) {
        collect_references(body->children[i], vars, var_count);
    }

    // Warn about unused
    for (int i = 0; i < var_count; i++) {
        if (!vars[i].used) {
            char msg[256];
            snprintf(msg, sizeof(msg), "unused variable '%s'", vars[i].name);
            AetherError warn = {
                .filename = NULL,
                .source_code = NULL,
                .line = vars[i].line,
                .column = vars[i].col,
                .message = msg,
                .suggestion = "prefix with '_' to suppress this warning",
                .context = NULL,
                .code = AETHER_WARN_UNUSED_VAR
            };
            aether_warning_report(&warn);
            warning_count++;
        }
    }
}

// --- Unreachable code analysis ---

// Check if a statement is a return or exit() call
static int is_terminating(ASTNode* node) {
    if (!node) return 0;
    if (node->type == AST_RETURN_STATEMENT) return 1;
    // Unwrap expression statement to check inner call
    if (node->type == AST_EXPRESSION_STATEMENT && node->child_count > 0) {
        return is_terminating(node->children[0]);
    }
    if (node->type == AST_FUNCTION_CALL && node->value &&
        strcmp(node->value, "exit") == 0) return 1;
    // if/else where BOTH branches terminate
    if (node->type == AST_IF_STATEMENT && node->child_count >= 3) {
        ASTNode* then_branch = node->children[1];
        ASTNode* else_branch = node->children[2];
        // Check last statement of each branch
        if (then_branch && else_branch) {
            int then_terminates = 0;
            int else_terminates = 0;
            if (then_branch->child_count > 0)
                then_terminates = is_terminating(then_branch->children[then_branch->child_count - 1]);
            else
                then_terminates = is_terminating(then_branch);
            if (else_branch->child_count > 0)
                else_terminates = is_terminating(else_branch->children[else_branch->child_count - 1]);
            else
                else_terminates = is_terminating(else_branch);
            return then_terminates && else_terminates;
        }
    }
    return 0;
}

static void check_unreachable_code(ASTNode* body) {
    if (!body) return;

    for (int i = 0; i < body->child_count; i++) {
        ASTNode* stmt = body->children[i];
        if (is_terminating(stmt) && i + 1 < body->child_count) {
            // Next statement is unreachable
            ASTNode* unreachable = body->children[i + 1];
            if (unreachable) {
                char msg[256];
                snprintf(msg, sizeof(msg), "unreachable code after %s",
                         stmt->type == AST_RETURN_STATEMENT ? "return" :
                         (stmt->type == AST_FUNCTION_CALL ? "exit()" : "terminating block"));
                AetherError warn = {
                    .filename = NULL,
                    .source_code = NULL,
                    .line = unreachable->line,
                    .column = unreachable->column,
                    .message = msg,
                    .suggestion = "remove unreachable code or restructure control flow",
                    .context = NULL,
                    .code = AETHER_WARN_UNREACHABLE
                };
                aether_warning_report(&warn);
                warning_count++;
            }
            break;  // Only warn once per block
        }

        // Recurse into blocks (if/else bodies, while bodies, etc.)
        if (stmt->type == AST_IF_STATEMENT) {
            for (int j = 1; j < stmt->child_count; j++) {
                check_unreachable_code(stmt->children[j]);
            }
        } else if (stmt->type == AST_WHILE_LOOP && stmt->child_count > 1) {
            check_unreachable_code(stmt->children[1]);
        }
    }
}

// Type checking functions
int typecheck_program(ASTNode* program) {
    if (!program || program->type != AST_PROGRAM) return 0;

    error_count = 0;
    warning_count = 0;
    namespace_count = 0;  // Reset imported namespaces
    user_explicit_namespace_count = 0;  // Reset user-explicit namespaces (issue #243)
    selective_import_reset();  // Reset per-module selective-import filters

    SymbolTable* global_table = create_symbol_table(NULL);
    
    // Add builtin functions
    // Signature: add_symbol(table, name, type, is_actor, is_function, is_state)
    Type* typeof_type = create_type(TYPE_STRING);
    add_symbol(global_table, "typeof", typeof_type, 0, 1, 0);

    Type* is_type_type = create_type(TYPE_BOOL);
    add_symbol(global_table, "is_type", is_type_type, 0, 1, 0);

    Type* convert_type_type = create_type(TYPE_UNKNOWN);  // Returns any type
    add_symbol(global_table, "convert_type", convert_type_type, 0, 1, 0);

    // Scheduler/concurrency builtins
    Type* wait_idle_type = create_type(TYPE_VOID);
    add_symbol(global_table, "wait_for_idle", wait_idle_type, 0, 1, 0);

    Type* sleep_type = create_type(TYPE_VOID);
    add_symbol(global_table, "sleep", sleep_type, 0, 1, 0);

    // Environment variable builtins
    Type* getenv_type = create_type(TYPE_STRING);  // Returns string (or null)
    add_symbol(global_table, "getenv", getenv_type, 0, 1, 0);

    Type* atoi_type = create_type(TYPE_INT);  // Returns int
    add_symbol(global_table, "atoi", atoi_type, 0, 1, 0);

    // Timing builtin — returns nanoseconds as int64 (int32 overflows after ~2.1 seconds)
    Type* clock_ns_type = create_type(TYPE_INT64);
    add_symbol(global_table, "clock_ns", clock_ns_type, 0, 1, 0);

    // Source-location intrinsics (#265). At codegen time these expand
    // to the AST node's line, source-file path, and enclosing C
    // function name — useful for assertions, panic messages, and log
    // formatters. Caller-site capture via default arguments is not
    // yet wired up (deferred to a follow-up); for now callers pass
    // them explicitly: `my_log(msg, __LINE__, __FILE__, __func__)`.
    add_symbol(global_table, "__LINE__", create_type(TYPE_INT),    0, 1, 0);
    add_symbol(global_table, "__FILE__", create_type(TYPE_STRING), 0, 1, 0);
    add_symbol(global_table, "__func__", create_type(TYPE_STRING), 0, 1, 0);

    // Output builtins
    Type* println_type = create_type(TYPE_VOID);
    add_symbol(global_table, "println", println_type, 0, 1, 0);
    Type* print_char_type = create_type(TYPE_VOID);
    add_symbol(global_table, "print_char", print_char_type, 0, 1, 0);

    // Platform selection builtin
    Type* select_type = create_type(TYPE_UNKNOWN);
    add_symbol(global_table, "select", select_type, 0, 1, 0);

    // Process control builtins
    Type* exit_type = create_type(TYPE_VOID);
    add_symbol(global_table, "exit", exit_type, 0, 1, 0);

    // Memory builtins
    Type* free_builtin_type = create_type(TYPE_VOID);
    add_symbol(global_table, "free", free_builtin_type, 0, 1, 0);

    // release(X) — explicit refcount-release sugar. Codegen restricts
    // the argument type to `string` (other heap types call their typed
    // release function). Pairs with `defer` to undo allocations made
    // by stdlib functions returning ownership: `defer release(body)`
    // after `body, err = http.get(url)`.
    Type* release_type = create_type(TYPE_VOID);
    add_symbol(global_table, "release", release_type, 0, 1, 0);

    // Array/collection builtins
    Type* make_type = create_type(TYPE_PTR);  // returns allocated memory
    add_symbol(global_table, "make", make_type, 0, 1, 0);

    // Closure/iteration builtins
    Type* each_type = create_type(TYPE_VOID);
    add_symbol(global_table, "each", each_type, 0, 1, 0);
    Type* call_type = create_type(TYPE_INT);  // return type depends on closure
    add_symbol(global_table, "call", call_type, 0, 1, 0);
    Type* read_char_type = create_type(TYPE_INT);
    add_symbol(global_table, "read_char", read_char_type, 0, 1, 0);
    Type* char_at_type = create_type(TYPE_INT);
    add_symbol(global_table, "char_at", char_at_type, 0, 1, 0);
    Type* box_closure_type = create_type(TYPE_PTR);
    add_symbol(global_table, "box_closure", box_closure_type, 0, 1, 0);
    Type* unbox_closure_type = create_type(TYPE_FUNCTION);
    add_symbol(global_table, "unbox_closure", unbox_closure_type, 0, 1, 0);
    Type* ref_type = create_type(TYPE_PTR);
    add_symbol(global_table, "ref", ref_type, 0, 1, 0);
    Type* ref_get_type = create_type(TYPE_INT);
    add_symbol(global_table, "ref_get", ref_get_type, 0, 1, 0);
    Type* ref_set_type = create_type(TYPE_VOID);
    add_symbol(global_table, "ref_set", ref_set_type, 0, 1, 0);
    Type* ref_free_type = create_type(TYPE_VOID);
    add_symbol(global_table, "ref_free", ref_free_type, 0, 1, 0);
    // Lazy evaluation builtins
    Type* lazy_type = create_type(TYPE_PTR);
    add_symbol(global_table, "lazy", lazy_type, 0, 1, 0);
    Type* force_type = create_type(TYPE_INT);  // default int; C returns intptr_t, implicit conversion
    add_symbol(global_table, "force", force_type, 0, 1, 0);
    Type* thunk_free_type = create_type(TYPE_VOID);
    add_symbol(global_table, "thunk_free", thunk_free_type, 0, 1, 0);
    Type* str_eq_type = create_type(TYPE_INT);
    add_symbol(global_table, "str_eq", str_eq_type, 0, 1, 0);
    Type* raw_mode_type = create_type(TYPE_VOID);
    add_symbol(global_table, "raw_mode", raw_mode_type, 0, 1, 0);
    Type* cooked_mode_type = create_type(TYPE_VOID);
    add_symbol(global_table, "cooked_mode", cooked_mode_type, 0, 1, 0);
    Type* spawn_sandboxed_type = create_type(TYPE_INT);
    add_symbol(global_table, "spawn_sandboxed", spawn_sandboxed_type, 0, 1, 0);
    // num_cores: runtime global (extern int num_cores in multicore_scheduler.h)
    Type* num_cores_type = create_type(TYPE_INT);
    add_symbol(global_table, "num_cores", num_cores_type, 0, 0, 0);
    Type* aether_push_type = create_type(TYPE_VOID);
    add_symbol(global_table, "sandbox_push", aether_push_type, 0, 1, 0);
    Type* aether_pop_type = create_type(TYPE_VOID);
    add_symbol(global_table, "sandbox_pop", aether_pop_type, 0, 1, 0);
    Type* sandbox_install_type = create_type(TYPE_VOID);
    add_symbol(global_table, "sandbox_install", sandbox_install_type, 0, 1, 0);
    Type* sandbox_uninstall_type = create_type(TYPE_VOID);
    add_symbol(global_table, "sandbox_uninstall", sandbox_uninstall_type, 0, 1, 0);
    Type* builder_ctx_type = create_type(TYPE_PTR);
    add_symbol(global_table, "builder_context", builder_ctx_type, 0, 1, 0);
    Type* builder_depth_type = create_type(TYPE_INT);
    add_symbol(global_table, "builder_depth", builder_depth_type, 0, 1, 0);

    // First pass: collect all declarations
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];

        switch (child->type) {
            case AST_ACTOR_DEFINITION: {
                // Create actor struct type
                Type* actor_type = create_type(TYPE_STRUCT);
                actor_type->struct_name = strdup(child->value);
                add_symbol(global_table, child->value, actor_type, 1, 0, 0);
                // Store AST node so state field types can be looked up
                Symbol* actor_sym_node = lookup_symbol(global_table, child->value);
                if (actor_sym_node) actor_sym_node->node = child;
                
                // Add generated spawn_ActorName() function - returns pointer to actor
                // Use TYPE_ACTOR_REF to represent pointer type
                char spawn_name[256];
                snprintf(spawn_name, sizeof(spawn_name), "spawn_%s", child->value);
                Type* spawn_return_type = create_type(TYPE_ACTOR_REF);
                spawn_return_type->element_type = clone_type(actor_type);
                add_symbol(global_table, spawn_name, spawn_return_type, 0, 1, 0);
                
                // Add generated send_ActorName() function - returns void
                char send_name[256];
                snprintf(send_name, sizeof(send_name), "send_%s", child->value);
                Type* send_type = create_type(TYPE_VOID);
                add_symbol(global_table, send_name, send_type, 0, 1, 0);
                
                // Add generated ActorName_step() function - returns void
                char step_name[256];
                snprintf(step_name, sizeof(step_name), "%s_step", child->value);
                Type* step_type = create_type(TYPE_VOID);
                add_symbol(global_table, step_name, step_type, 0, 1, 0);
                break;
            }
            case AST_BUILDER_FUNCTION:
            case AST_FUNCTION_DEFINITION: {
                // Reject two top-level definitions that mangle to the
                // same C symbol (<module>_<name>). A `builder` and a
                // plain function sharing a name both emit that symbol;
                // without this check one silently wins and every call
                // site dispatches to the winner — clean compile, clean
                // link, wrong function body (no diagnostic). The check
                // runs on the post-merge program, so it catches the
                // cross-module case too. See
                // builder-function-name-collision-silent-dispatch.md.
                // Only a builder-vs-plain-function clash is a true
                // collision. Two plain functions sharing a name is the
                // legal multi-clause / pattern-matching form (Erlang-
                // style: several `fib(...)` clauses merged into one
                // dispatcher), so function-vs-function must NOT fire.
                if (child->value) {
                    Symbol* prior = lookup_symbol(global_table, child->value);
                    if (prior && prior->node &&
                        (prior->node->type == AST_FUNCTION_DEFINITION ||
                         prior->node->type == AST_BUILDER_FUNCTION) &&
                        prior->node->type != child->type) {
                        const char* this_kind =
                            (child->type == AST_BUILDER_FUNCTION) ? "builder" : "function";
                        const char* prior_kind =
                            (prior->node->type == AST_BUILDER_FUNCTION) ? "builder" : "function";
                        char msg[320];
                        snprintf(msg, sizeof(msg),
                            "duplicate definition of '%s': a %s and a %s "
                            "cannot share a name (both emit the same C symbol); "
                            "previous definition at line %d",
                            child->value, prior_kind, this_kind,
                            prior->node->line);
                        type_error(msg, child->line, child->column);
                    }
                }
                add_symbol(global_table, child->value, clone_type(child->node_type), 0, 1, 0);
                // Store AST node so arity can be verified at call sites
                Symbol* func_sym = lookup_symbol(global_table, child->value);
                if (func_sym) func_sym->node = child;
                break;
            }
            case AST_EXPORT_STATEMENT: {
                // `export X(...) { ... }` wraps the function definition
                // in AST_EXPORT_STATEMENT. Unwrap and register the inner
                // function so same-file callers can resolve it via the
                // ordinary lexical-scope path. The export modifier is a
                // visibility annotation for cross-module qualified
                // calls; it shouldn't gate intra-module bare calls.
                // Closes #287.
                if (child->child_count > 0) {
                    ASTNode* inner = child->children[0];
                    if (inner && (inner->type == AST_FUNCTION_DEFINITION ||
                                  inner->type == AST_BUILDER_FUNCTION) &&
                        inner->value) {
                        add_symbol(global_table, inner->value,
                                   clone_type(inner->node_type), 0, 1, 0);
                        Symbol* sym = lookup_symbol(global_table, inner->value);
                        if (sym) sym->node = inner;
                    }
                }
                break;
            }
            case AST_EXTERN_FUNCTION: {
                // Register extern C function in symbol table. Wire the
                // AST node into the symbol too so call-site type
                // validation (typecheck_function_call's Duration check
                // and the existing extern-arg type-check block) can
                // walk the declared parameters. Without this, both
                // checks silently no-op on externs because they gate
                // on `symbol->node`.
                add_symbol(global_table, child->value, clone_type(child->node_type), 0, 1, 0);
                Symbol* extern_sym = lookup_symbol(global_table, child->value);
                if (extern_sym) {
                    extern_sym->node = child;
                }
                break;
            }
            case AST_STRUCT_DEFINITION: {
                Type* struct_type = create_type(TYPE_STRUCT);
                struct_type->struct_name = strdup(child->value);
                add_symbol(global_table, child->value, struct_type, 0, 0, 0);
                // Store AST node in symbol for later field type updates
                Symbol* struct_sym = lookup_symbol(global_table, child->value);
                if (struct_sym) {
                    struct_sym->node = child;
                }
                break;
            }
            case AST_MESSAGE_DEFINITION: {
                // Register message type so receive patterns can look up field types
                Type* msg_type = create_type(TYPE_MESSAGE);
                add_symbol(global_table, child->value, msg_type, 0, 0, 0);
                Symbol* msg_sym = lookup_symbol(global_table, child->value);
                if (msg_sym) {
                    msg_sym->node = child;
                }
                break;
            }
            case AST_CONST_DECLARATION: {
                // Register constant in symbol table
                Type* ctype = child->node_type ? clone_type(child->node_type) : create_type(TYPE_UNKNOWN);
                // Infer type from the value expression if unknown
                if (ctype->kind == TYPE_UNKNOWN && child->child_count > 0 && child->children[0]->node_type) {
                    free_type(ctype);
                    ctype = clone_type(child->children[0]->node_type);
                }
                add_symbol(global_table, child->value, ctype, 0, 0, 0);
                break;
            }
            case AST_MAIN_FUNCTION:
                // Main function doesn't need to be in symbol table
                break;
            case AST_IMPORT_STATEMENT: {
                // Process import and register alias if present
                const char* module_path = child->value;

                // Check if this import has an alias. Module aliases are
                // AST_IDENTIFIER children annotated "module_alias" by the
                // parser, distinguishing them from selective-import
                // symbols, which are also AST_IDENTIFIER children but
                // carry the symbol name to expose unqualified.
                if (child->child_count > 0) {
                    ASTNode* last_child = child->children[child->child_count - 1];
                    if (last_child && last_child->type == AST_IDENTIFIER &&
                        last_child->annotation &&
                        strcmp(last_child->annotation, "module_alias") == 0) {
                        const char* alias = last_child->value;
                        add_module_alias(global_table, alias, module_path);
                    }
                }

                // Handle stdlib imports: import std.X (or std.X.Y, std.X.Y.Z, ...)
                if (strncmp(module_path, "std.", 4) == 0) {
                    // For selective-import filter purposes we still want
                    // the substring after `std.` ("fs", "http.client", ...).
                    const char* module_name = module_path + 4;

                    // The qualified-call namespace prefix is the LEAF
                    // component (the bit after the last dot), not the
                    // whole sub-path. For `std.http.client` callers
                    // write `client.foo(...)`, not `http.client.foo(...)`.
                    // This matches what the orchestrator's merger uses
                    // when it prefixes wrapper function names — see
                    // module_get_namespace() in aether_module.c.
                    const char* last_dot = strrchr(module_name, '.');
                    const char* ns_leaf  = last_dot ? last_dot + 1 : module_name;

                    // Register namespace for qualified calls (e.g., string.new)
                    register_namespace(ns_leaf);
                    // User-explicit registration (issue #243 sealed scopes):
                    // skip if this import was synthesized by
                    // module_merge_into_program's BFS transitive-merge
                    // pass. Synthetic imports keep the namespace
                    // resolvable for cloned merged-body callers but
                    // hide it from user code that didn't ask for it.
                    if (!child->annotation ||
                        strcmp(child->annotation, "synthetic") != 0) {
                        register_user_explicit_namespace(ns_leaf);
                    }

                    // If this is a selective import, record the allow list
                    // so qualified calls to functions not in it get rejected.
                    // Does not affect unqualified resolution inside merged
                    // stdlib function bodies.
                    //
                    // Otherwise (non-selective), mark the namespace as
                    // having full access — required so a non-selective
                    // import after a per-symbol import of the same module
                    // gives the whole namespace back. Issue #252.
                    int is_selective = 0;
                    if (child->child_count > 0) {
                        ASTNode* first_sel = child->children[0];
                        if (first_sel && first_sel->type == AST_IDENTIFIER) {
                            is_selective = 1;
                            for (int sk = 0; sk < child->child_count; sk++) {
                                ASTNode* sel = child->children[sk];
                                if (sel && sel->type == AST_IDENTIFIER && sel->value) {
                                    selective_import_add(module_name, sel->value);
                                }
                            }
                        }
                    }
                    if (!is_selective) {
                        selective_import_mark_nonselective(module_name);
                    }

                    // Look up cached module from orchestrator
                    AetherModule* mod = module_find(module_path);
                    ASTNode* mod_ast = mod ? mod->ast : NULL;
                    if (mod_ast) {
                        // Extract extern declarations from the module.
                        //
                        // Externs are always registered regardless of the
                        // user's selective-import list. This is because
                        // merged Aether-native stdlib wrappers (like
                        // `http.get` calling `http_get_raw` internally)
                        // need every extern from their own module visible
                        // in the global symbol table to compile, even when
                        // the user only selectively imported the wrapper.
                        //
                        // The selective-import filter is applied instead
                        // at qualified-call sites via
                        // is_selective_import_blocked(), so user code that
                        // writes `math.pow` is still rejected when only
                        // `sqrt` is imported.
                        for (int j = 0; j < mod_ast->child_count; j++) {
                            ASTNode* decl = mod_ast->children[j];
                            if (decl->type == AST_EXTERN_FUNCTION && decl->value) {
                                if (!lookup_symbol_local(global_table, decl->value)) {
                                    add_symbol(global_table, decl->value,
                                               clone_type(decl->node_type), 0, 1, 0);
                                }
                            }
                        }
                        // NOTE: do NOT free mod_ast — registry owns it
                    }
                } else {
                    // Handle local package imports: import mypackage.utils
                    const char* namespace = get_namespace_from_path(module_path);
                    register_namespace(namespace);
                    // Same synthetic-skip gate as the std.* path above
                    // (issue #243 sealed scopes).
                    if (!child->annotation ||
                        strcmp(child->annotation, "synthetic") != 0) {
                        register_user_explicit_namespace(namespace);
                    }

                    // Look up cached module from orchestrator
                    AetherModule* mod = module_find(module_path);
                    ASTNode* mod_ast = mod ? mod->ast : NULL;
                    if (mod_ast) {
                        for (int j = 0; j < mod_ast->child_count; j++) {
                            ASTNode* decl = mod_ast->children[j];
                            if (decl->type == AST_EXTERN_FUNCTION && decl->value) {
                                // Always import externs regardless of the
                                // selective-import filter. Externs are C
                                // bindings that merged Aether functions may
                                // call internally — filtering them out
                                // breaks transitive references. The
                                // selective filter applies at qualified-call
                                // sites via is_selective_import_blocked().
                                if (!lookup_symbol_local(global_table, decl->value)) {
                                    add_symbol(global_table, decl->value,
                                               clone_type(decl->node_type), 0, 1, 0);
                                }
                            }
                            // AST_FUNCTION_DEFINITION handled by module_merge_into_program()
                        }
                        // NOTE: do NOT free mod_ast — registry owns it
                    }
                }
                break;
            }
            default:
                break;
        }
    }
    
    // Register unqualified short names for selective imports.
    // At this point all merged function definitions are in the symbol table,
    // so we can look up their types to register the short aliases.
    //
    // Two forms register short aliases here:
    //   1. Selective:  import mod (a, b)        — children are AST_IDENTIFIER
    //   2. Glob:       import mod (*)           — annotation == "glob_import"
    //
    // The glob form synthesizes the same per-name registration by walking
    // every symbol in global_table whose name starts with the module's
    // namespace prefix (`<ns>_`) and registering the trailing short name
    // as an alias. Names with a leading underscore in the short part are
    // treated as private and skipped. Issue #171 (P1).
    import_alias_count = 0;  // Reset for fresh compilation
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if (child->type != AST_IMPORT_STATEMENT || !child->value) continue;

        int is_glob = (child->annotation
                       && strcmp(child->annotation, "glob_import") == 0);

        // Selective imports need at least one identifier child to do anything;
        // glob imports drive their own loop off the symbol table.
        if (!is_glob && child->child_count == 0) continue;
        if (!is_glob) {
            ASTNode* first = child->children[0];
            if (!first || first->type != AST_IDENTIFIER) continue;
        }

        const char* module_path = child->value;
        const char* ns;
        if (strncmp(module_path, "std.", 4) == 0) {
            ns = module_path + 4;
        } else {
            ns = get_namespace_from_path(module_path);
        }

        // Build the iteration source for the inner loop. For selective
        // imports, this is the AST_IDENTIFIER children. For glob imports,
        // we materialize a temporary list of short-name strings by
        // scanning global_table for every "<ns>_*" symbol.
        const char** glob_names = NULL;
        int glob_count = 0;
        int glob_cap = 0;
        if (is_glob) {
            size_t ns_len = strlen(ns);
            for (Symbol* sym = global_table->symbols; sym; sym = sym->next) {
                if (!sym->name) continue;
                if (strncmp(sym->name, ns, ns_len) != 0) continue;
                if (sym->name[ns_len] != '_') continue;
                const char* tail = sym->name + ns_len + 1;
                if (!*tail || *tail == '_') continue;  // private / malformed
                if (glob_count >= glob_cap) {
                    glob_cap = glob_cap == 0 ? 16 : glob_cap * 2;
                    glob_names = (const char**)realloc(
                        glob_names, sizeof(const char*) * glob_cap);
                }
                glob_names[glob_count++] = tail;
            }
        }

        int loop_end = is_glob ? glob_count : child->child_count;
        for (int k = 0; k < loop_end; k++) {
            const char* short_name;
            if (is_glob) {
                short_name = glob_names[k];
            } else {
                ASTNode* sel = child->children[k];
                if (!sel || sel->type != AST_IDENTIFIER) continue;
                short_name = sel->value;
            }

            // Build the full C name: namespace_shortname
            char full_name[256];
            snprintf(full_name, sizeof(full_name), "%s_%s", ns, short_name);

            Symbol* full_sym = lookup_symbol(global_table, full_name);
            if (full_sym) {
                Symbol* existing_short = lookup_symbol_local(global_table, short_name);
                if (!existing_short || !existing_short->is_function) {
                    // Register or override: either no existing symbol, or existing
                    // is not a function (e.g. C's sqrt from math.h)
                    if (existing_short) {
                        // Update existing symbol in place
                        if (existing_short->type) free_type(existing_short->type);
                        existing_short->type = full_sym->type ? clone_type(full_sym->type) : create_type(TYPE_UNKNOWN);
                        existing_short->is_function = full_sym->is_function;
                        existing_short->node = full_sym->node;
                    } else {
                        add_symbol(global_table, short_name,
                                   full_sym->type ? clone_type(full_sym->type) : create_type(TYPE_UNKNOWN),
                                   0, full_sym->is_function, 0);
                        Symbol* short_sym = lookup_symbol(global_table, short_name);
                        if (short_sym && full_sym->node) {
                            short_sym->node = full_sym->node;
                        }
                    }
                }

                // Store alias for AST rewriting: "release" -> "build.release".
                // Only register when the prefixed symbol exists — otherwise
                // we'd rewrite calls to externs (which keep their bare
                // names) into nonexistent `ns_extern` forms.
                char dotted[256];
                snprintf(dotted, sizeof(dotted), "%s.%s", ns, short_name);
                add_import_alias(short_name, dotted);
            }
        }
        free((void*)glob_names);
    }

    // NEW: Run type inference before type checking
    if (!infer_all_types(program, global_table)) {
        free_symbol_table(global_table);
        // Clean up namespace strings to avoid leaks on re-runs
        for (int ns = 0; ns < namespace_count; ns++) free(imported_namespaces[ns]);
        namespace_count = 0;
        for (int ns = 0; ns < user_explicit_namespace_count; ns++) free(user_explicit_namespaces[ns]);
        user_explicit_namespace_count = 0;
        return 0;
    }
    
    // Update symbol table with inferred types
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if ((child->type == AST_FUNCTION_DEFINITION || child->type == AST_BUILDER_FUNCTION) && child->value && child->node_type) {
            Symbol* func_sym = lookup_symbol(global_table, child->value);
            if (func_sym) {
                if (func_sym->type) free_type(func_sym->type);
                func_sym->type = clone_type(child->node_type);
            }
        }
    }

    // Re-sync import-alias short symbols from their now-inferred full
    // symbols. The short aliases (registered above for selective AND
    // glob imports) clone the full symbol's type BEFORE `infer_all_types`
    // runs, so a wrapper whose return type is inferred — e.g. an
    // `fs.list_dir` whose `-> { ... return list, "" }` body yields a
    // `(ptr, string)` tuple — leaves the short alias `list_dir` stuck on
    // the pre-inference placeholder (int/unknown). A bare `list, err =
    // list_dir(...)` call then resolves through the stale short symbol,
    // stamps the call node's return type as `int`, and codegen emits
    // `int _tup0 = fs_list_dir(...)` — a C type error. Selective imports
    // happened to dodge this when the call rewrote-and-re-resolved to the
    // full symbol, but the glob path did not; refreshing every alias from
    // the canonical full symbol makes both forms carry the wrappers' real
    // tuple return types. (fbs-core ask #1.)
    for (int a = 0; a < import_alias_count; a++) {
        const char* short_name = import_aliases[a].short_name;
        const char* dotted = import_aliases[a].qualified_name;
        // Build the underscored full C name from the dotted alias
        // ("fs.list_dir" -> "fs_list_dir").
        char full_name[256];
        snprintf(full_name, sizeof(full_name), "%s", dotted);
        for (char* p = full_name; *p; p++) { if (*p == '.') *p = '_'; }

        Symbol* full_sym = lookup_symbol(global_table, full_name);
        Symbol* short_sym = lookup_symbol_local(global_table, short_name);
        if (full_sym && short_sym && short_sym->is_function && full_sym->type) {
            if (short_sym->type) free_type(short_sym->type);
            short_sym->type = clone_type(full_sym->type);
            short_sym->node = full_sym->node;
        }
    }

    // Second pass: type check all nodes
    for (int i = 0; i < program->child_count; i++) {
        typecheck_node(program->children[i], global_table);
    }

    // Third pass: unused variable + unreachable code analysis
    for (int i = 0; i < program->child_count; i++) {
        ASTNode* child = program->children[i];
        if ((child->type == AST_FUNCTION_DEFINITION || child->type == AST_BUILDER_FUNCTION) && child->child_count > 0) {
            ASTNode* body = child->children[child->child_count - 1];
            check_unused_variables(body);
            check_unreachable_code(body);
        } else if (child->type == AST_MAIN_FUNCTION && child->child_count > 0) {
            // main() has a BLOCK child containing the actual statements
            ASTNode* main_body = child->children[0];
            check_unused_variables(main_body);
            check_unreachable_code(main_body);
        }
    }

    free_symbol_table(global_table);
    
    // Report errors and warnings
    if (error_count > 0) {
        fprintf(stderr, "Type checking failed with %d error(s)\n", error_count);
        return 0;  // Block compilation on errors
    }
    
    if (warning_count > 0) {
        fprintf(stderr, "Type checking completed with %d warning(s)\n", warning_count);
    }
    
    // Clean up namespace strings
    for (int ns = 0; ns < namespace_count; ns++) free(imported_namespaces[ns]);
    namespace_count = 0;
    for (int ns = 0; ns < user_explicit_namespace_count; ns++) free(user_explicit_namespaces[ns]);
    user_explicit_namespace_count = 0;

    return 1;
}

int typecheck_node(ASTNode* node, SymbolTable* table) {
    if (!node) return 0;
    
    switch (node->type) {
        case AST_ACTOR_DEFINITION:
            return typecheck_actor_definition(node, table);
        case AST_BUILDER_FUNCTION:
        case AST_FUNCTION_DEFINITION:
            return typecheck_function_definition(node, table);
        case AST_EXPORT_STATEMENT:
            // Type-check the inner declaration. The wrapper is purely a
            // visibility annotation; any same-file caller went through
            // the unwrap-and-register path in the first pass (#287).
            if (node->child_count > 0) {
                return typecheck_node(node->children[0], table);
            }
            return 1;
        case AST_EXTERN_FUNCTION:
            // Extern functions have no body to check - just a declaration
            return 1;
        case AST_STRUCT_DEFINITION:
            return typecheck_struct_definition(node, table);
        case AST_MAIN_FUNCTION:
            return typecheck_statement(node, table);
        default:
            return typecheck_statement(node, table);
    }
}

// Look up the type of a specific field in a message definition
static Type* lookup_message_field_type(SymbolTable* table, const char* message_name, const char* field_name) {
    Symbol* msg_sym = lookup_symbol(table, message_name);
    if (!msg_sym || !msg_sym->node || msg_sym->node->type != AST_MESSAGE_DEFINITION) {
        return NULL;
    }
    ASTNode* msg_def = msg_sym->node;
    for (int i = 0; i < msg_def->child_count; i++) {
        ASTNode* field = msg_def->children[i];
        if (field->type == AST_MESSAGE_FIELD && field->value && strcmp(field->value, field_name) == 0) {
            return field->node_type ? clone_type(field->node_type) : NULL;
        }
    }
    return NULL;
}

// Validate that message constructor field values match declared field types
static void typecheck_message_constructor(ASTNode* constructor, SymbolTable* table) {
    if (!constructor || constructor->type != AST_MESSAGE_CONSTRUCTOR || !constructor->value) return;
    const char* msg_name = constructor->value;
    Symbol* msg_sym = lookup_symbol(table, msg_name);
    if (!msg_sym || !msg_sym->node || msg_sym->node->type != AST_MESSAGE_DEFINITION) return;

    for (int i = 0; i < constructor->child_count; i++) {
        ASTNode* field_init = constructor->children[i];
        if (!field_init || field_init->type != AST_FIELD_INIT || !field_init->value) continue;
        if (field_init->child_count == 0) continue;

        Type* declared = lookup_message_field_type(table, msg_name, field_init->value);
        if (!declared) continue;

        ASTNode* value_expr = field_init->children[0];
        typecheck_expression(value_expr, table);
        Type* actual = infer_type(value_expr, table);

        /* Cons-cell context: when the declared field type is
         * `*StringSeq` and the RHS is an array literal, accept the
         * assignment and stamp the literal's node_type to *StringSeq.
         * The codegen branch in emit_message_field_init / AST_ARRAY_LITERAL
         * picks up the stamped type and emits a cons chain instead of
         * a static C array initialiser. Same disambiguation rule the
         * variable-decl path uses, lifted to message-field context.
         * See codegen_expr.c emit_message_field_init for the
         * matching codegen branch. */
        int seq_literal_match =
            value_expr && value_expr->type == AST_ARRAY_LITERAL &&
            is_string_seq_ptr_type(declared);

        if (seq_literal_match) {
            if (value_expr->node_type) free_type(value_expr->node_type);
            value_expr->node_type = clone_type(declared);
        } else if (actual && actual->kind != TYPE_UNKNOWN &&
            declared->kind != TYPE_UNKNOWN &&
            !is_type_compatible(actual, declared)) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "Type mismatch in field '%s' of message '%s': expected %s, got %s",
                     field_init->value, msg_name, type_name(declared), type_name(actual));
            type_error(buf, field_init->line, field_init->column);
        }
        free_type(actual);
        free_type(declared);
    }
}

int typecheck_actor_definition(ASTNode* actor, SymbolTable* table) {
    if (!actor || actor->type != AST_ACTOR_DEFINITION) return 0;
    
    SymbolTable* actor_table = create_symbol_table(table);
    
    // Type check actor body
    for (int i = 0; i < actor->child_count; i++) {
        ASTNode* child = actor->children[i];
        
        if (child->type == AST_STATE_DECLARATION) {
            if ((!child->node_type || child->node_type->kind == TYPE_UNKNOWN)
                && child->child_count > 0 && child->children[0]) {
                ASTNode* init = child->children[0];
                if (init->type == AST_FUNCTION_CALL && init->value) {
                    Symbol* fn = lookup_qualified_symbol(actor_table, init->value);
                    if (fn && fn->type) {
                        child->node_type = clone_type(fn->type);
                    }
                }
            }
            add_symbol(actor_table, child->value, clone_type(child->node_type), 0, 0, 1);
        } else if (child->type == AST_RECEIVE_STATEMENT) {
            // Handle receive statement
            SymbolTable* receive_table = create_symbol_table(actor_table);

            // V1 syntax: receive(msg) { ... } has child->value set
            // V2 syntax: receive { Pattern -> ... } has child->value = NULL
            if (child->value) {
                Type* msg_type = create_type(TYPE_MESSAGE);
                add_symbol(receive_table, child->value, msg_type, 0, 0, 0);
            }

            // Type check the receive body/arms
            for (int j = 0; j < child->child_count; j++) {
                ASTNode* arm = child->children[j];

                // For V2 receive arms, extract pattern variables
                if (arm->type == AST_RECEIVE_ARM && arm->child_count >= 2) {
                    ASTNode* pattern = arm->children[0];
                    ASTNode* arm_body = arm->children[1];

                    // Add pattern variables to scope
                    if (pattern->type == AST_MESSAGE_PATTERN) {
                        for (int k = 0; k < pattern->child_count; k++) {
                            ASTNode* field = pattern->children[k];
                            if (field->type == AST_PATTERN_FIELD) {
                                // Look up actual field type from message definition
                                Type* field_type = lookup_message_field_type(table, pattern->value, field->value);
                                if (!field_type) {
                                    field_type = create_type(TYPE_UNKNOWN);
                                }
                                // Use pattern variable name if present (field: var), else field name
                                const char* var_name = field->value;
                                if (field->child_count > 0 && field->children[0] &&
                                    field->children[0]->type == AST_PATTERN_VARIABLE && field->children[0]->value) {
                                    var_name = field->children[0]->value;
                                }
                                add_symbol(receive_table, var_name, field_type, 0, 0, 0);
                            }
                        }
                    }

                    // Type check arm body
                    typecheck_statement(arm_body, receive_table);
                } else if (arm->type == AST_TIMEOUT_ARM && arm->child_count >= 2) {
                    // Timeout arm: after N -> { body }
                    typecheck_expression(arm->children[0], receive_table);  // timeout expr
                    typecheck_statement(arm->children[1], receive_table);   // body
                } else {
                    typecheck_statement(arm, receive_table);
                }
            }

            free_symbol_table(receive_table);
            continue;
        }
        
        typecheck_node(child, actor_table);
    }
    
    free_symbol_table(actor_table);
    return 1;
}

int typecheck_function_definition(ASTNode* func, SymbolTable* table) {
    if (!func || (func->type != AST_FUNCTION_DEFINITION && func->type != AST_BUILDER_FUNCTION)) return 0;

    SymbolTable* func_table = create_symbol_table(table);

    // Issue #243 sealed scopes: cloned function bodies from
    // module_merge_into_program's BFS transitive-merge pass need
    // relaxed qualified-call resolution so they can reach into other
    // transitively-merged namespaces. The flag propagates from
    // parent in create_symbol_table, so nested scopes inside this
    // body inherit it; on function exit we just free func_table.
    if (func->is_imported) {
        func_table->inside_merged_body = 1;
    }

    // Add parameters to function's symbol table
    for (int i = 0; i < func->child_count - 1; i++) { // Last child is body
        ASTNode* param = func->children[i];
        if (param->type == AST_VARIABLE_DECLARATION || param->type == AST_PATTERN_VARIABLE) {
            Type* param_type = param->node_type ? clone_type(param->node_type) : create_type(TYPE_UNKNOWN);
            add_symbol(func_table, param->value, param_type, 0, 0, 0);
        }
    }

    // Builder functions get implicit _builder: ptr parameter
    if (func->type == AST_BUILDER_FUNCTION) {
        add_symbol(func_table, "_builder", create_type(TYPE_PTR), 0, 0, 0);
    }

    // Type check function body
    ASTNode* body = func->children[func->child_count - 1];
    typecheck_statement(body, func_table);

    free_symbol_table(func_table);
    return 1;
}

int typecheck_struct_definition(ASTNode* struct_def, SymbolTable* table) {
    (void)table;  // Unused for now
    if (!struct_def || struct_def->type != AST_STRUCT_DEFINITION) return 0;
    
    // Type check all fields
    for (int i = 0; i < struct_def->child_count; i++) {
        ASTNode* field = struct_def->children[i];

        // Compound fields (union { ... } / nested struct { ... }) skip the
        // leaf-type-required check — they carry no `node_type`; their
        // children are independently typed (and walked at access time
        // via base_type->compound_node).
        if (field->type == AST_STRUCT_FIELD_UNION ||
            field->type == AST_STRUCT_FIELD_NESTED) {
            continue;
        }

        if (field->type != AST_STRUCT_FIELD) {
            type_error("Invalid struct field", field->line, field->column);
            return 0;
        }

        // Verify field type is valid
        if (!field->node_type) {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg),
                    "Struct field '%s' has no type", field->value);
            type_error(error_msg, field->line, field->column);
            return 0;
        }
        
        // Check for duplicate field names
        for (int j = 0; j < i; j++) {
            ASTNode* other_field = struct_def->children[j];
            if (strcmp(field->value, other_field->value) == 0) {
                char error_msg[256];
                snprintf(error_msg, sizeof(error_msg), 
                        "Duplicate field name '%s' in struct '%s'", 
                        field->value, struct_def->value);
                type_error(error_msg, field->line, field->column);
                return 0;
            }
        }
    }
    
    return 1;
}

int typecheck_statement(ASTNode* stmt, SymbolTable* table) {
    if (!stmt) return 0;
    
    switch (stmt->type) {
        case AST_TUPLE_DESTRUCTURE: {
            // a, b = func() — last child is RHS, others are variable declarations
            if (stmt->child_count < 2) {
                type_error("Invalid tuple destructuring", stmt->line, stmt->column);
                return 0;
            }
            int var_count = stmt->child_count - 1;
            ASTNode* rhs = stmt->children[var_count];  // Last child is RHS

            // Typecheck the RHS
            typecheck_expression(rhs, table);
            Type* rhs_type = infer_type(rhs, table);

            // Verify RHS is a tuple with matching element count
            if (!rhs_type || rhs_type->kind != TYPE_TUPLE) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "cannot destructure — '%s' returns '%s', not a tuple",
                    rhs->value ? rhs->value : "expression",
                    type_to_string(rhs_type));
                aether_error_with_suggestion(msg, stmt->line, stmt->column,
                    "use single assignment instead, or ensure the function returns multiple values");
                error_count++;
                if (rhs_type) free_type(rhs_type);
                return 0;
            }

            if (rhs_type->tuple_count != var_count) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "tuple destructuring count mismatch — %d variables, but expression returns %d values",
                    var_count, rhs_type->tuple_count);
                aether_error_with_suggestion(msg, stmt->line, stmt->column,
                    "match the number of variables to the number of returned values");
                error_count++;
                free_type(rhs_type);
                return 0;
            }

            // Assign types to each variable and add to symbol table
            for (int j = 0; j < var_count; j++) {
                ASTNode* var = stmt->children[j];
                if (var->node_type) free_type(var->node_type);
                var->node_type = clone_type(rhs_type->tuple_types[j]);
                // Don't register _ (discard) in symbol table
                if (var->value && strcmp(var->value, "_") != 0) {
                    add_symbol(table, var->value, clone_type(var->node_type), 0, 0, 0);
                }
            }

            free_type(rhs_type);
            return 1;
        }

        case AST_CONST_DECLARATION:
        case AST_VARIABLE_DECLARATION: {
            // Forbid declaring a name that's been hidden in this scope —
            // shadowing a hidden binding would re-introduce position-sensitive
            // semantics. If you want a fresh name, pick a different one.
            if (stmt->value && scope_name_is_hidden(table, stmt->value)) {
                char error_msg[256];
                snprintf(error_msg, sizeof(error_msg),
                         "cannot declare '%s' — it is hidden in this scope by `hide`",
                         stmt->value);
                type_error(error_msg, stmt->line, stmt->column);
                return 0;
            }
            if (stmt->child_count > 0) {
                // Has initializer
                ASTNode* init = stmt->children[0];
                // Match-as-expression: typecheck as statement, then use its type
                if (init->type == AST_MATCH_STATEMENT) {
                    typecheck_statement(init, table);
                } else {
                    typecheck_expression(init, table);
                }
                Type* init_type = infer_type(init, table);

                /* `const` is substitution-at-each-use: the compiler
                 * inlines the RHS expression at every reference. That
                 * works for literals but is silently wrong for
                 * `const G = make_thing()` — every reference re-calls
                 * the function, allocating fresh state. Per Nico's
                 * design call: reject non-constant RHS expressions at
                 * compile time rather than running silently-wrong
                 * code. See `is_const_expression` above for the
                 * allowed/disallowed list. */
                if (stmt->type == AST_CONST_DECLARATION &&
                    !is_const_expression(init, table)) {
                    /* Allow array literals for const arr[] = [...] declarations.
                     * These emit static const arrays, not #define-inlined forms. */
                    int is_array_const = (stmt->annotation && strcmp(stmt->annotation, "array_const") == 0 &&
                                          init->type == AST_ARRAY_LITERAL);
                    if (!is_array_const) {
                        char msg[512];
                        if (stmt->annotation && strcmp(stmt->annotation, "global_var") == 0) {
                            /* #701: a module-level `var` lowers to a C file-scope
                             * `static`, whose initializer C requires to be a
                             * constant expression. Initialize to a constant and
                             * assign the computed value from a function at
                             * startup. */
                            snprintf(msg, sizeof(msg),
                                "module-level `var` initializer must be a compile-time constant "
                                "expression — it lowers to a file-scope `static`, which C requires "
                                "to have a constant initializer. Initialize it to a constant here "
                                "and assign the computed value from a function at startup instead.");
                        } else {
                            snprintf(msg, sizeof(msg),
                                "const initializer must be a compile-time constant expression — "
                                "function calls are inlined at each use site, which would re-evaluate "
                                "the call (and re-allocate / re-side-effect) on every reference. "
                                "Use a regular assignment in main() (or std.config / std.actors for "
                                "process-global state) instead.");
                        }
                        type_error(msg, stmt->line, stmt->column);
                        free_type(init_type);
                        return 0;
                    }
                }

                /* Python-style "redeclaration" (`b = 999` after a prior
                 * `byte b = 0`) parses as a fresh AST_VARIABLE_DECLARATION
                 * with stmt->node_type == TYPE_UNKNOWN. The variable's
                 * actual type lives in the symbol table. When the existing
                 * binding is byte-typed, run the literal-range check
                 * before stmt->node_type gets overwritten with the int
                 * init type below. */
                Symbol* existing = stmt->value ? lookup_symbol(table, stmt->value) : NULL;
                if (existing && existing->type && existing->type->kind == TYPE_BYTE &&
                    byte_assignment_literal_out_of_range(init)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "byte literal out of range: %s does not fit in 0..255",
                        init->value ? init->value : "?");
                    type_error(msg, stmt->line, stmt->column);
                    free_type(init_type);
                    return 0;
                }

                /* #698: silent-narrowing guard. Re-binding a local whose
                 * type was INFERRED as 32-bit int (a bare `x = expr`, not an
                 * explicit annotation) with a 64-bit value truncates
                 * silently — is_assignable permits int64->int and codegen
                 * keeps the int storage. Because the type was inferred, not
                 * chosen, this is almost certainly unintended (the aedis
                 * lpStringEqualsInt64 bug: `parsed = 0` then `parsed =
                 * <int64>`, so the comparison never matched). Diagnose with
                 * the load-bearing fix; an explicit annotation clears the
                 * inferred flag and silences it. */
                if (existing && existing->type_inferred &&
                    existing->type && existing->type->kind == TYPE_INT &&
                    init_type && (init_type->kind == TYPE_INT64 ||
                                  init_type->kind == TYPE_UINT64)) {
                    const char* nm = stmt->value ? stmt->value : "x";
                    char msg[420];
                    snprintf(msg, sizeof(msg),
                        "narrowing assignment to '%s': its type was inferred "
                        "as 32-bit int from its initializer, but a 64-bit "
                        "value is assigned here and would truncate. Annotate "
                        "the declaration to keep 64 bits (e.g. `long %s = ...` "
                        "or `uint64 %s = ...`), or write `int %s = ...` to "
                        "make the narrowing explicit.", nm, nm, nm, nm);
                    type_error(msg, stmt->line, stmt->column);
                    free_type(init_type);
                    return 0;
                }

                /* Context-sensitive literal: `[a, b, c]` against a
                 * `*StringSeq`-typed variable means "build a cons chain"
                 * rather than "build a static C array". Stamp the
                 * literal's node_type so the codegen branch in
                 * codegen_expr.c picks the cons-chain emitter. The
                 * empty literal `[]` carries through as a *StringSeq
                 * NULL the same way. See std/collections/aether_stringseq.h
                 * for the cell layout. */
                if (init->type == AST_ARRAY_LITERAL &&
                    is_string_seq_ptr_type(stmt->node_type)) {
                    if (init->node_type) free_type(init->node_type);
                    init->node_type = clone_type(stmt->node_type);
                    free_type(init_type);
                    add_symbol(table, stmt->value, clone_type(stmt->node_type), 0, 0, 0);
                    return 1;
                }

                /* #745: a typed module-level const array
                 * (`const T[N] = [literals]`, annotation "array_const")
                 * lets the porter pin the C element type — narrowing the
                 * int literals to uint8/uint16/uint32 is the explicit,
                 * compile-time-constant intent (same posture as
                 * `byte b = 5`), not an is_assignable mismatch. Permit an
                 * integer-element array literal to initialise an integer-
                 * element typed const array. */
                int array_const_int_narrow = 0;
                if (stmt->annotation && strcmp(stmt->annotation, "array_const") == 0 &&
                    init_type && init_type->kind == TYPE_ARRAY && init_type->element_type &&
                    stmt->node_type && stmt->node_type->kind == TYPE_ARRAY &&
                    stmt->node_type->element_type) {
                    TypeKind ie = init_type->element_type->kind;
                    TypeKind de = stmt->node_type->element_type->kind;
                    int ie_int = (ie == TYPE_INT || ie == TYPE_INT64 || ie == TYPE_UINT64 ||
                                  ie == TYPE_UINT32 || ie == TYPE_UINT16 || ie == TYPE_UINT8 ||
                                  ie == TYPE_BYTE);
                    int de_int = (de == TYPE_INT || de == TYPE_INT64 || de == TYPE_UINT64 ||
                                  de == TYPE_UINT32 || de == TYPE_UINT16 || de == TYPE_UINT8 ||
                                  de == TYPE_BYTE);
                    array_const_int_narrow = ie_int && de_int;
                }

                // If variable has no explicit type (TYPE_UNKNOWN), use initializer's type
                if (!stmt->node_type || stmt->node_type->kind == TYPE_UNKNOWN) {
                    if (stmt->node_type) free_type(stmt->node_type);
                    stmt->node_type = clone_type(init_type);
                } else if (!array_const_int_narrow && !is_assignable(init_type, stmt->node_type)) {
                    // Has explicit type but initializer doesn't match
                    free_type(init_type);
                    type_error("Type mismatch in variable initialization", stmt->line, stmt->column);
                    return 0;
                }
                /* #697: declared 64-bit but the initializer is a 32-bit-int
                 * arithmetic expression (e.g. `uint64 x = byte << 24`) —
                 * widen it so the computation happens in 64 bits, not C int. */
                if (stmt->node_type &&
                    (stmt->node_type->kind == TYPE_INT64 ||
                     stmt->node_type->kind == TYPE_UINT64)) {
                    propagate_int_width_64(init, stmt->node_type->kind);
                }
                /* `byte b = <int literal>` — fresh declaration with explicit
                 * `byte` type. Reject out-of-range literals at compile time
                 * (literal-range check). Non-literal int is accepted (runtime
                 * truncation, matching int64→int etc.). */
                if (stmt->node_type && stmt->node_type->kind == TYPE_BYTE &&
                    byte_assignment_literal_out_of_range(init)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "byte literal out of range: %s does not fit in 0..255",
                        init->value ? init->value : "?");
                    type_error(msg, stmt->line, stmt->column);
                    free_type(init_type);
                    return 0;
                }
                free_type(init_type);
            }

            // Add to symbol table. Bare `_` is a per-use discard — it
            // is never registered as a symbol, so each `_ = <expr>`
            // stays an independent throwaway with no type unified
            // across occurrences (aeb-ae-help-and-toolchain-feedback.md
            // #4). Mirrors the existing `_`-skip in the tuple-
            // destructure path above.
            if (stmt->value && strcmp(stmt->value, "_") != 0) {
                add_symbol(table, stmt->value, clone_type(stmt->node_type), 0, 0, 0);
                /* #698: carry the parser's inferred-type marker onto the
                 * binding, but only for a 32-bit int (the sole narrowing
                 * target). A later 64-bit re-bind then triggers the guard
                 * above / in AST_ASSIGNMENT. */
                if (stmt->type_inferred && stmt->node_type &&
                    stmt->node_type->kind == TYPE_INT) {
                    Symbol* s = lookup_symbol_local(table, stmt->value);
                    if (s) s->type_inferred = 1;
                }
            }
            return 1;
        }

        case AST_ASSIGNMENT: {
            if (stmt->child_count >= 2) {
                ASTNode* left = stmt->children[0];
                ASTNode* right = stmt->children[1];
                
                Symbol* symbol = lookup_symbol(table, left->value);
                if (!symbol) {
                    char error_msg[256];
                    if (left->value && name_blocked_by_hide(table, left->value)) {
                        snprintf(error_msg, sizeof(error_msg),
                                 "'%s' is hidden in this scope by `hide` or `seal except`",
                                 left->value);
                    } else {
                        snprintf(error_msg, sizeof(error_msg), "Undefined variable '%s'", left->value ? left->value : "?");
                    }
                    type_error(error_msg, left->line, left->column);
                    return 0;
                }

                Type* right_type = infer_type(right, table);
                if (!is_assignable(right_type, symbol->type)) {
                    char error_msg[256];
                    snprintf(error_msg, sizeof(error_msg),
                             "Type mismatch in assignment to '%s': expected %s, got %s",
                             left->value ? left->value : "?",
                             type_name(symbol->type), type_name(right_type));
                    free_type(right_type);
                    type_error(error_msg, stmt->line, stmt->column);
                    return 0;
                }
                /* #698: silent-narrowing guard (assignment path) — twin of
                 * the AST_VARIABLE_DECLARATION guard, for assignment forms
                 * that take this path. */
                if (symbol->type_inferred &&
                    symbol->type && symbol->type->kind == TYPE_INT &&
                    right_type && (right_type->kind == TYPE_INT64 ||
                                   right_type->kind == TYPE_UINT64)) {
                    const char* nm = left->value ? left->value : "x";
                    char msg[420];
                    snprintf(msg, sizeof(msg),
                        "narrowing assignment to '%s': its type was inferred "
                        "as 32-bit int from its initializer, but a 64-bit "
                        "value is assigned here and would truncate. Annotate "
                        "the declaration to keep 64 bits (e.g. `long %s = ...` "
                        "or `uint64 %s = ...`), or write `int %s = ...` to "
                        "make the narrowing explicit.", nm, nm, nm, nm);
                    type_error(msg, stmt->line, stmt->column);
                    free_type(right_type);
                    return 0;
                }
                /* `b = <int literal>` where b: byte — same range check as the
                 * declaration path. */
                if (symbol->type && symbol->type->kind == TYPE_BYTE &&
                    byte_assignment_literal_out_of_range(right)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "byte literal out of range: %s does not fit in 0..255",
                        right->value ? right->value : "?");
                    type_error(msg, stmt->line, stmt->column);
                    free_type(right_type);
                    return 0;
                }
                free_type(right_type);
            }
            return 1;
        }

        case AST_COMPOUND_ASSIGNMENT: {
            // node->value = variable name, children[0] = operator, children[1] = RHS
            if (stmt->child_count >= 2) {
                Symbol* symbol = lookup_symbol(table, stmt->value);
                if (!symbol) {
                    char error_msg[256];
                    if (stmt->value && name_blocked_by_hide(table, stmt->value)) {
                        snprintf(error_msg, sizeof(error_msg),
                                 "'%s' is hidden in this scope by `hide` or `seal except`",
                                 stmt->value);
                    } else {
                        snprintf(error_msg, sizeof(error_msg), "Undefined variable '%s'", stmt->value ? stmt->value : "?");
                    }
                    type_error(error_msg, stmt->line, stmt->column);
                    return 0;
                }
                ASTNode* rhs = stmt->children[1];
                typecheck_expression(rhs, table);
                { Type* _t = infer_type(rhs, table); free_type(_t); }
                if (stmt->node_type && stmt->node_type->kind == TYPE_UNKNOWN && symbol->type) {
                    free_type(stmt->node_type);
                    stmt->node_type = clone_type(symbol->type);
                }
            }
            return 1;
        }

        case AST_IF_STATEMENT: {
            if (stmt->child_count >= 1) {
                ASTNode* condition = stmt->children[0];
                typecheck_expression(condition, table);
                Type* cond_type = infer_type(condition, table);

                if (cond_type && cond_type->kind != TYPE_BOOL) {
                    free_type(cond_type);
                    type_error("If condition must be boolean", condition->line, condition->column);
                    return 0;
                }
                free_type(cond_type);
            }
            
            // Type check then and else branches
            for (int i = 1; i < stmt->child_count; i++) {
                typecheck_statement(stmt->children[i], table);
            }
            return 1;
        }
        
        case AST_FOR_LOOP: {
            SymbolTable* loop_table = create_symbol_table(table);
            
            // Type check init (child 0)
            if (stmt->child_count > 0 && stmt->children[0]) {
                typecheck_statement(stmt->children[0], loop_table);
            }
            
            // Type check condition (child 1)
            if (stmt->child_count > 1 && stmt->children[1]) {
                typecheck_expression(stmt->children[1], loop_table);
                Type* cond_type = infer_type(stmt->children[1], loop_table);
                if (cond_type && cond_type->kind != TYPE_BOOL) {
                    free_type(cond_type);
                    type_error("For loop condition must be boolean", stmt->line, stmt->column);
                    free_symbol_table(loop_table);
                    return 0;
                }
                free_type(cond_type);
            }
            
            // Type check increment (child 2)
            if (stmt->child_count > 2 && stmt->children[2]) {
                typecheck_expression(stmt->children[2], loop_table);
            }
            
            // Type check body (child 3)
            if (stmt->child_count > 3 && stmt->children[3]) {
                typecheck_statement(stmt->children[3], loop_table);
            }
            
            free_symbol_table(loop_table);
            return 1;
        }
        
        case AST_WHILE_LOOP: {
            if (stmt->child_count >= 1) {
                ASTNode* condition = stmt->children[0];
                typecheck_expression(condition, table);
                Type* cond_type = infer_type(condition, table);

                if (cond_type && cond_type->kind != TYPE_BOOL) {
                    free_type(cond_type);
                    type_error("Loop condition must be boolean", condition->line, condition->column);
                    return 0;
                }
                free_type(cond_type);
            }

            // Type check loop body
            for (int i = 1; i < stmt->child_count; i++) {
                typecheck_statement(stmt->children[i], table);
            }
            return 1;
        }
        
        case AST_BLOCK: {
            SymbolTable* block_table = create_symbol_table(table);

            // PRE-PASS: collect hide / seal directives BEFORE processing any
            // other statements, so they're scope-level (position within the
            // block doesn't matter) and apply to every other statement here.
            for (int i = 0; i < stmt->child_count; i++) {
                ASTNode* child = stmt->children[i];
                if (!child) continue;
                if (child->type == AST_HIDE_DIRECTIVE) {
                    for (int j = 0; j < child->child_count; j++) {
                        ASTNode* id = child->children[j];
                        if (id && id->value) scope_hide_name(block_table, id->value);
                    }
                } else if (child->type == AST_SEAL_DIRECTIVE) {
                    block_table->is_sealed = 1;
                    for (int j = 0; j < child->child_count; j++) {
                        ASTNode* id = child->children[j];
                        if (id && id->value) scope_seal_except(block_table, id->value);
                    }
                }
            }

            for (int i = 0; i < stmt->child_count; i++) {
                ASTNode* child = stmt->children[i];
                // Skip the directive nodes themselves — they were already
                // processed in the pre-pass above. Walking them as statements
                // would just be a no-op, but we keep the block tidy.
                if (child && (child->type == AST_HIDE_DIRECTIVE ||
                              child->type == AST_SEAL_DIRECTIVE)) {
                    continue;
                }
                typecheck_statement(child, block_table);
            }

            free_symbol_table(block_table);
            return 1;
        }
        
        case AST_EXPRESSION_STATEMENT: {
            if (stmt->child_count > 0) {
                typecheck_expression(stmt->children[0], table);
            }
            return 1;
        }

        // Bridge cases: when these node types appear in a "statement"
        // recursion (e.g. as the value of a return statement, or as
        // the RHS of an `if x = call()` shape that walks via the
        // catch-all default), we want them routed through the
        // expression typechecker so node_type lands on member-access
        // / interp children. Without these, the default branch's
        // `typecheck_node → typecheck_statement` recursion fell back
        // to default again at AST_STRING_INTERP / AST_MEMBER_ACCESS
        // (no cases here pre-fix) and never called typecheck_expression
        // — string interp's TypeKind switch then defaulted to `%d`
        // for string fields read through a `*Self` chain. Section C.1
        // of fresh-aether-requests.
        case AST_STRING_INTERP:
        case AST_MEMBER_ACCESS:
            return typecheck_expression(stmt, table);

        case AST_FUNCTION_CALL:
            // Function call used as a statement (e.g. println(...), user_fn(...))
            return typecheck_function_call(stmt, table);

        case AST_TRY_STATEMENT: {
            // children: [0] body block, [1] AST_CATCH_CLAUSE (value = name, child[0] = handler)
            if (stmt->child_count != 2) {
                type_error("malformed try/catch", stmt->line, stmt->column);
                return 0;
            }
            ASTNode* body = stmt->children[0];
            ASTNode* catch_clause = stmt->children[1];

            // Body runs in its own scope (already handled by AST_BLOCK typecheck).
            typecheck_statement(body, table);

            // Catch binds `name` to a string (panic reason) and typechecks
            // the handler in a scope where that name is visible.
            if (!catch_clause || catch_clause->type != AST_CATCH_CLAUSE ||
                !catch_clause->value || catch_clause->child_count < 1) {
                type_error("malformed catch clause", stmt->line, stmt->column);
                return 0;
            }

            SymbolTable* catch_table = create_symbol_table(table);
            Type* str_type = create_type(TYPE_STRING);
            add_symbol(catch_table, catch_clause->value, str_type, 0, 0, 0);
            typecheck_statement(catch_clause->children[0], catch_table);
            free_symbol_table(catch_table);
            return 1;
        }

        case AST_PANIC_STATEMENT: {
            // panic(reason) — reason must typecheck and evaluate to a string.
            if (stmt->child_count < 1) {
                type_error("panic() requires a reason argument", stmt->line, stmt->column);
                return 0;
            }
            ASTNode* reason = stmt->children[0];
            typecheck_expression(reason, table);
            Type* rt = infer_type(reason, table);
            if (rt && rt->kind != TYPE_STRING && rt->kind != TYPE_UNKNOWN) {
                type_error("panic() reason must be a string", reason->line, reason->column);
                free_type(rt);
                return 0;
            }
            if (rt) free_type(rt);
            return 1;
        }

        case AST_PRINT_STATEMENT: {
            for (int i = 0; i < stmt->child_count; i++) {
                typecheck_expression(stmt->children[i], table);
            }
            if (stmt->child_count >= 2 &&
                stmt->children[0]->type == AST_LITERAL &&
                stmt->children[0]->node_type &&
                stmt->children[0]->node_type->kind == TYPE_STRING &&
                stmt->children[0]->value) {
                const char* fmt = stmt->children[0]->value;
                int arg_idx = 1;
                for (int fi = 0; fmt[fi]; fi++) {
                    if (fmt[fi] != '%' || !fmt[fi + 1]) continue;
                    fi++;
                    while (fmt[fi] == '-' || fmt[fi] == '+' || fmt[fi] == ' ' ||
                           fmt[fi] == '#' || fmt[fi] == '0') fi++;
                    while (fmt[fi] >= '0' && fmt[fi] <= '9') fi++;
                    if (fmt[fi] == '.') { fi++; while (fmt[fi] >= '0' && fmt[fi] <= '9') fi++; }
                    if (fmt[fi] == '%') continue;
                    if (arg_idx >= stmt->child_count) break;
                    Type* atype = infer_type(stmt->children[arg_idx], table);
                    TypeKind ak = atype ? atype->kind : TYPE_UNKNOWN;
                    char spec = fmt[fi];
                    int mismatch = 0;
                    if ((spec == 's') && ak != TYPE_STRING && ak != TYPE_PTR) mismatch = 1;
                    if ((spec == 'd' || spec == 'i') && ak != TYPE_INT && ak != TYPE_INT64 && ak != TYPE_BOOL) mismatch = 1;
                    if ((spec == 'f' || spec == 'g' || spec == 'e') && ak != TYPE_FLOAT && ak != TYPE_LONGDOUBLE) mismatch = 1;
                    if (mismatch) {
                        char wbuf[256];
                        snprintf(wbuf, sizeof(wbuf),
                            "Format specifier '%%%c' does not match argument type '%s' (auto-corrected)",
                            spec, type_name(atype));
                        type_warning(wbuf, stmt->children[arg_idx]->line, stmt->children[arg_idx]->column);
                    }
                    if (atype) free_type(atype);
                    arg_idx++;
                }
            }
            return 1;
        }
        
        case AST_SEND_STATEMENT: {
            if (stmt->child_count >= 2) {
                ASTNode* actor_ref = stmt->children[0];
                ASTNode* message = stmt->children[1];

                Type* actor_type = infer_type(actor_ref, table);
                if (actor_type && actor_type->kind != TYPE_ACTOR_REF) {
                    free_type(actor_type);
                    type_error("First argument to send must be an actor reference", actor_ref->line, actor_ref->column);
                    return 0;
                }
                free_type(actor_type);

                typecheck_expression(message, table);
            }
            return 1;
        }

        case AST_SEND_FIRE_FORGET: {
            // actor ! MessageType { fields... }
            if (stmt->child_count >= 2) {
                ASTNode* actor_ref = stmt->children[0];
                ASTNode* message = stmt->children[1];

                // Validate actor reference type
                typecheck_expression(actor_ref, table);
                Type* actor_type = infer_type(actor_ref, table);
                if (actor_type && actor_type->kind != TYPE_ACTOR_REF && actor_type->kind != TYPE_UNKNOWN) {
                    char error_msg[256];
                    snprintf(error_msg, sizeof(error_msg),
                             "Cannot send to '%s': expected an actor reference",
                             actor_ref->value ? actor_ref->value : "expression");
                    free_type(actor_type);
                    type_error(error_msg, actor_ref->line, actor_ref->column);
                    return 0;
                }
                free_type(actor_type);

                // Validate that the message type is a registered message definition
                if (message->type == AST_MESSAGE_CONSTRUCTOR && message->value) {
                    Symbol* msg_sym = lookup_symbol(table, message->value);
                    if (!msg_sym || !msg_sym->type || msg_sym->type->kind != TYPE_MESSAGE) {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg),
                                 "Undefined message type '%s'", message->value);
                        type_error(error_msg, message->line, message->column);
                        return 0;
                    }
                }

                // Validate field value types match declared field types
                typecheck_message_constructor(message, table);
            }
            return 1;
        }

        case AST_SPAWN_ACTOR_STATEMENT: {
            if (stmt->child_count > 0) {
                typecheck_expression(stmt->children[0], table);
            }
            return 1;
        }
        
        case AST_MATCH_STATEMENT: {
            // Type check the match expression
            Type* match_expr_type = NULL;
            Type* element_type = NULL;
            /* Whether the match expression is a `*StringSeq`. When it
             * is, head bindings are typed `string` and tail bindings
             * are typed `*StringSeq` (so recursive walks compose) —
             * not the array-element + array-of-element shapes used
             * for the legacy int-array path. See codegen_stmt.c
             * `is_string_seq_type` for the matching codegen
             * dispatch. */
            int is_seq_match = 0;
            if (stmt->child_count > 0) {
                typecheck_expression(stmt->children[0], table);
                match_expr_type = stmt->children[0]->node_type;
                // Extract element type if matching on an array
                if (match_expr_type && match_expr_type->kind == TYPE_ARRAY && match_expr_type->element_type) {
                    element_type = match_expr_type->element_type;
                } else if (is_string_seq_ptr_type(match_expr_type)) {
                    is_seq_match = 1;
                    element_type = create_type(TYPE_STRING);
                }
            }
            // Default to int if we couldn't determine the element type
            if (!element_type) {
                element_type = create_type(TYPE_INT);
            }

            // Type check each match arm
            for (int i = 1; i < stmt->child_count; i++) {
                ASTNode* arm = stmt->children[i];
                if (!arm || arm->type != AST_MATCH_ARM || arm->child_count < 2) continue;

                ASTNode* pattern = arm->children[0];
                ASTNode* body = arm->children[1];

                // Create a new scope for pattern variables
                SymbolTable* arm_table = create_symbol_table(table);

                // Register pattern variables from list patterns using the actual element type
                if (pattern->type == AST_PATTERN_LIST) {
                    for (int j = 0; j < pattern->child_count; j++) {
                        ASTNode* elem = pattern->children[j];
                        if (elem && elem->type == AST_PATTERN_VARIABLE && elem->value) {
                            add_symbol(arm_table, elem->value, clone_type(element_type), 0, 0, 0);
                        }
                    }
                } else if (pattern->type == AST_PATTERN_CONS) {
                    // [h|t] - register head and tail with proper types
                    if (pattern->child_count >= 1) {
                        ASTNode* head = pattern->children[0];
                        if (head && head->type == AST_PATTERN_VARIABLE && head->value) {
                            add_symbol(arm_table, head->value, clone_type(element_type), 0, 0, 0);
                        }
                    }
                    if (pattern->child_count >= 2) {
                        ASTNode* tail = pattern->children[1];
                        if (tail && tail->type == AST_PATTERN_VARIABLE && tail->value) {
                            Type* tail_type;
                            if (is_seq_match) {
                                /* tail in `[h|t]` against a `*StringSeq`
                                 * is itself a `*StringSeq`, not an
                                 * array-of-element. This is what makes
                                 * recursive walks compose: walk(tail)
                                 * passes a properly-typed seq into the
                                 * recursive call. */
                                tail_type = make_string_seq_ptr_type();
                            } else {
                                tail_type = create_type(TYPE_ARRAY);
                                tail_type->element_type = clone_type(element_type);
                            }
                            add_symbol(arm_table, tail->value, tail_type, 0, 0, 0);
                        }
                    }
                }

                // Type check the arm body in the new scope
                typecheck_statement(body, arm_table);

                // Propagate arm result type to the match node (for match-as-expression)
                if (!stmt->node_type || stmt->node_type->kind == TYPE_UNKNOWN) {
                    if (body->node_type && body->node_type->kind != TYPE_UNKNOWN) {
                        stmt->node_type = clone_type(body->node_type);
                    }
                }

                free_symbol_table(arm_table);
            }
            return 1;
        }

        default:
            // Type check all children
            for (int i = 0; i < stmt->child_count; i++) {
                typecheck_node(stmt->children[i], table);
            }
            return 1;
    }
}

int typecheck_expression(ASTNode* expr, SymbolTable* table) {
    if (!expr) return 0;
    
    switch (expr->type) {
        case AST_BINARY_EXPRESSION:
            return typecheck_binary_expression(expr, table);
            
        case AST_UNARY_EXPRESSION: {
            if (expr->child_count > 0) {
                typecheck_expression(expr->children[0], table);
                expr->node_type = infer_unary_type(expr->children[0],
                                                 get_token_type_from_string(expr->value));
            }
            return 1;
        }

        case AST_TUPLE_UNWRAP: {
            /* `expr!` — unwrap-or-trap. The operand must return a tuple
             * whose LAST slot is the (string) error; the unwrap yields
             * the first slot and panics at runtime if the error slot is
             * non-empty. Typecheck the operand, then require a tuple of
             * at least 2 elements with a string final slot — the
             * canonical `(value, err)` shape this sugar targets. */
            if (expr->child_count == 0) return 0;
            typecheck_expression(expr->children[0], table);
            Type* op = infer_type(expr->children[0], table);
            if (!op || op->kind != TYPE_TUPLE) {
                type_error("`!` unwrap requires a tuple-returning operand "
                           "(a `(value, err)` result)",
                           expr->line, expr->column);
                if (op) free_type(op);
                expr->node_type = create_type(TYPE_UNKNOWN);
                return 0;
            }
            if (op->tuple_count < 2) {
                type_error("`!` unwrap requires a `(value, err)` tuple "
                           "with at least two slots",
                           expr->line, expr->column);
                free_type(op);
                expr->node_type = create_type(TYPE_UNKNOWN);
                return 0;
            }
            Type* last = op->tuple_types[op->tuple_count - 1];
            if (!last || last->kind != TYPE_STRING) {
                type_error("`!` unwrap requires the final tuple slot to be "
                           "the `string` error of a `(value, err)` result",
                           expr->line, expr->column);
                free_type(op);
                expr->node_type = create_type(TYPE_UNKNOWN);
                return 0;
            }
            expr->node_type = clone_type(op->tuple_types[0]);
            free_type(op);
            return 1;
        }

        case AST_FUNCTION_CALL:
            return typecheck_function_call(expr, table);
            
        case AST_IDENTIFIER: {
            Symbol* symbol = lookup_symbol(table, expr->value);
            if (!symbol) {
                char error_msg[256];
                if (expr->value && name_blocked_by_hide(table, expr->value)) {
                    snprintf(error_msg, sizeof(error_msg),
                             "'%s' is hidden in this scope by `hide` or `seal except`",
                             expr->value);
                } else {
                    snprintf(error_msg, sizeof(error_msg), "Undefined variable '%s'", expr->value ? expr->value : "?");
                }
                type_error(error_msg, expr->line, expr->column);
                return 0;
            }
            expr->node_type = symbol->type ? clone_type(symbol->type) : create_type(TYPE_UNKNOWN);
            return 1;
        }

        case AST_LITERAL:
            // Literals are already typed
            return 1;

        case AST_SIZEOF:
        case AST_OFFSETOF:
            // Layout builtins. The child of OFFSETOF is the field name,
            // which is NOT an expression to resolve — do not recurse
            // (the default case would treat it as an undefined variable).
            // Field/type validity is the C compiler's job (offsetof on a
            // bad field is a hard C error), matching the trust-the-author
            // posture of the `as *Struct` cast.
            expr->node_type = create_type(TYPE_INT);
            return 1;

        case AST_VA_START:
            // No children; opaque va_list cookie (ptr).
            expr->node_type = create_type(TYPE_PTR);
            return 1;

        case AST_VA_ARG:
            // children[0] is the cookie expr — typecheck it; the result
            // type was fixed by the parser from the requested C type.
            if (expr->child_count > 0) {
                typecheck_expression(expr->children[0], table);
            }
            if (!expr->node_type) expr->node_type = create_type(TYPE_PTR);
            return 1;

        case AST_VA_END:
            if (expr->child_count > 0) {
                typecheck_expression(expr->children[0], table);
            }
            expr->node_type = create_type(TYPE_VOID);
            return 1;

        case AST_IF_EXPRESSION:
            // Typecheck all children (condition, then-expr, else-expr)
            for (int i = 0; i < expr->child_count; i++) {
                typecheck_expression(expr->children[i], table);
            }
            if (expr->child_count >= 2) {
                Type* then_type = infer_type(expr->children[1], table);
                if (!expr->node_type || expr->node_type->kind == TYPE_UNKNOWN) {
                    if (expr->node_type) free_type(expr->node_type);
                    expr->node_type = clone_type(then_type);
                }
                free_type(then_type);
            }
            return 1;

        case AST_NULL_LITERAL:
            // null is always TYPE_PTR
            if (!expr->node_type) expr->node_type = create_type(TYPE_PTR);
            return 1;

        case AST_PTR_AS_STRUCT_CAST:
            /* Walk the operand, then set our own node_type via the
             * shared inference path so codegen sees the
             * TYPE_PTR{element=TYPE_STRUCT} on this node and emits
             * `->field` for downstream member access. */
            if (expr->child_count > 0) {
                typecheck_expression(expr->children[0], table);
            }
            if (expr->node_type) free_type(expr->node_type);
            expr->node_type = infer_type(expr, table);
            return 1;

        case AST_PTR_AS_ARRAY_CAST:
            /* Walk the operand; keep the parser-populated node_type
             * (TYPE_ARRAY with element_type) so codegen emits the
             * correct C element-pointer cast `((T*)(expr))`. */
            if (expr->child_count > 0) {
                typecheck_expression(expr->children[0], table);
            }
            return 1;

        case AST_PTR_AS_FN_CAST:
            /* Walk the operand; keep the parser-populated node_type
             * (TYPE_FUNCTION with signature) so the call-site codegen
             * can read the param/return types off it. */
            if (expr->child_count > 0) {
                typecheck_expression(expr->children[0], table);
            }
            /* Don't replace expr->node_type — the parser set it from
             * the `fn(...)` source spelling and it carries the exact
             * Type* tree we need for codegen. */
            return 1;

        case AST_ARRAY_LITERAL:
            // Type check all array elements
            for (int i = 0; i < expr->child_count; i++) {
                typecheck_expression(expr->children[i], table);
            }
            return 1;

        case AST_NAMED_ARG:
            // Named argument: type check the value expression
            if (expr->child_count > 0) {
                typecheck_expression(expr->children[0], table);
                expr->node_type = expr->children[0]->node_type
                    ? clone_type(expr->children[0]->node_type)
                    : create_type(TYPE_UNKNOWN);
            }
            return 1;

        case AST_STRING_INTERP:
            // Type check all sub-expressions inside the interpolation
            for (int i = 0; i < expr->child_count; i++) {
                typecheck_expression(expr->children[i], table);
            }
            expr->node_type = create_type(TYPE_STRING);
            return 1;

        case AST_CLOSURE: {
            // Create a child scope for the closure's parameters
            SymbolTable* closure_scope = create_symbol_table(table);

            // Issue #333: when typecheck_function_call stamped a
            // `dsl_recv:<name>` annotation on this closure (because
            // the closure is the trailing block of a member-access
            // call), seed the closure scope's dsl_receiver. lookup_symbol
            // uses it to resolve unqualified calls inside the body
            // through the `<receiver>_<name>` rewrite.
            if (expr->annotation &&
                strncmp(expr->annotation, "dsl_recv:", 9) == 0) {
                const char* recv = expr->annotation + 9;
                if (*recv) {
                    closure_scope->dsl_receiver = strdup(recv);
                }
            }

            // Register closure parameters in the child scope
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* child = expr->children[i];
                if (child && child->type == AST_CLOSURE_PARAM && child->value) {
                    Type* ptype = child->node_type ? clone_type(child->node_type)
                                                   : create_type(TYPE_INT); // default to int
                    add_symbol(closure_scope, child->value, ptype, 0, 0, 0);
                }
            }

            /* Type-check the closure body via `typecheck_statement`.
             *
             * A closure body is a sequence of statements, so it must be
             * checked the same way a function body is — through
             * `typecheck_statement`, which registers declarations
             * (plain `AST_VARIABLE_DECLARATION` and `AST_TUPLE_
             * DESTRUCTURE` alike) into the scope it is given, and
             * recurses correctly into `if` / `while` / `for` / nested
             * blocks with their own sub-scopes.
             *
             * The body was previously walked with `typecheck_
             * expression`, which has no statement cases — it fell to
             * the generic child-walk and registered nothing, so every
             * declared name inside the closure was `E0300 Undefined
             * variable` at its later uses. Earlier point fixes patched
             * one statement kind at a time at the body's top level
             * (the `AST_TUPLE_DESTRUCTURE` special-case from
             * tuple-destructure-in-closure-scope.md); a plain `x = e`
             * nested inside an `if` within the closure still slipped
             * through. Routing the whole body through
             * `typecheck_statement` closes the class — see
             * new_string_len_something.md §4. */
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* child = expr->children[i];
                if (child && child->type == AST_BLOCK) {
                    for (int j = 0; j < child->child_count; j++) {
                        typecheck_statement(child->children[j], closure_scope);
                    }
                }
            }

            free_symbol_table(closure_scope);

            if (!expr->node_type) {
                expr->node_type = create_type(TYPE_FUNCTION);
            }
            return 1;
        }

        case AST_ARRAY_ACCESS:
            // Type check array access — validate index is integer and
            // propagate the element type onto this node so downstream
            // consumers (print format specifiers, string interpolation,
            // further expressions) know what type an access yields. Without
            // this, arr[i] has no node_type and defaults to %d even when
            // arr is string[], producing wrong format specifiers and UB.
            if (expr->child_count >= 2) {
                typecheck_expression(expr->children[0], table);
                typecheck_expression(expr->children[1], table);
                Type* idx_type = infer_type(expr->children[1], table);
                if (idx_type && idx_type->kind != TYPE_INT && idx_type->kind != TYPE_INT64
                    && idx_type->kind != TYPE_UNKNOWN) {
                    char error_msg[256];
                    snprintf(error_msg, sizeof(error_msg),
                             "Array index must be an integer, got %s",
                             type_name(idx_type));
                    type_error(error_msg, expr->line, expr->column);
                }
                if (idx_type) free_type(idx_type);

                // Propagate element type from the array expression.
                Type* arr_type = expr->children[0]->node_type;
                if (arr_type && arr_type->kind == TYPE_ARRAY && arr_type->element_type &&
                    (!expr->node_type || expr->node_type->kind == TYPE_UNKNOWN)) {
                    if (expr->node_type) free_type(expr->node_type);
                    expr->node_type = clone_type(arr_type->element_type);
                }
            }
            return 1;

        case AST_STRUCT_LITERAL:
            // Type check struct literal field initializers
            for (int i = 0; i < expr->child_count; i++) {
                ASTNode* field_init = expr->children[i];
                if (field_init && field_init->type == AST_ASSIGNMENT && field_init->child_count > 0) {
                    typecheck_expression(field_init->children[0], table);
                }
            }
            // Struct literal type is already set during type inference
            return 1;
            
        case AST_MEMBER_ACCESS: {
            // Namespace-qualified constant access: mymath.PI_APPROX -> mymath_PI_APPROX
            // Rewrite AST to AST_IDENTIFIER so codegen emits the C variable name directly.
            // Issue #243: gate on the strict per-scope visibility check
            // so user code can't reach into transitively-merged consts.
            if (expr->child_count > 0 && expr->children[0] &&
                expr->children[0]->type == AST_IDENTIFIER && expr->children[0]->value &&
                is_visible_namespace(expr->children[0]->value, table) && expr->value) {
                // Enforce export visibility for constants
                if (is_export_blocked(expr->children[0]->value, expr->value)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "'%s' is not exported from module '%s'",
                             expr->value, expr->children[0]->value);
                    type_error(msg, expr->line, expr->column);
                    return 0;
                }
                char qualified[512];
                snprintf(qualified, sizeof(qualified), "%s_%s",
                         expr->children[0]->value, expr->value);
                Symbol* sym = lookup_symbol(table, qualified);
                if (sym && sym->type) {
                    // Rewrite node in-place
                    expr->type = AST_IDENTIFIER;
                    free(expr->value);
                    expr->value = strdup(qualified);
                    expr->node_type = clone_type(sym->type);
                    return 1;
                }
            }
            // Type check member access (e.g., msg.type, struct.field)
            if (expr->child_count > 0) {
                ASTNode* base = expr->children[0];
                typecheck_expression(base, table);

                Type* base_type = infer_type(base, table);

                if (base_type && base_type->kind == TYPE_DURATION) {
                    if (expr->value && strcmp(expr->value, "ns") == 0) {
                        expr->node_type = create_type(TYPE_INT64);
                    } else if (expr->value &&
                               (strcmp(expr->value, "us") == 0 || strcmp(expr->value, "ms") == 0 ||
                                strcmp(expr->value, "s") == 0 || strcmp(expr->value, "m") == 0 ||
                                strcmp(expr->value, "h") == 0 || strcmp(expr->value, "d") == 0)) {
                        expr->node_type = create_type(TYPE_FLOAT);
                    } else {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg),
                                 "Type 'Duration' has no field '%s'",
                                 expr->value ? expr->value : "?");
                        free_type(base_type);
                        type_error(error_msg, expr->line, expr->column);
                        return 0;
                    }
                    free_type(base_type);
                    return 1;
                }

                // Reject member access on primitive types — catch the error in Aether, not C
                if (base_type && (base_type->kind == TYPE_INT || base_type->kind == TYPE_FLOAT ||
                                  base_type->kind == TYPE_BOOL || base_type->kind == TYPE_STRING)) {
                    char error_msg[256];
                    snprintf(error_msg, sizeof(error_msg),
                             "Type '%s' has no field '%s'",
                             type_name(base_type), expr->value ? expr->value : "?");
                    free_type(base_type);
                    type_error(error_msg, expr->line, expr->column);
                    return 0;
                }

                // Handle Message type member access
                if (base_type && base_type->kind == TYPE_MESSAGE) {
                    if (strcmp(expr->value, "type") == 0 ||
                        strcmp(expr->value, "sender_id") == 0 ||
                        strcmp(expr->value, "payload_int") == 0) {
                        expr->node_type = create_type(TYPE_INT);
                    } else if (strcmp(expr->value, "payload_ptr") == 0) {
                        expr->node_type = create_type(TYPE_VOID);
                    }
                }
                // Handle actor ref member access — look up state field type from actor definition
                else if (base_type && base_type->kind == TYPE_ACTOR_REF && base_type->element_type &&
                         base_type->element_type->kind == TYPE_STRUCT && base_type->element_type->struct_name) {
                    Symbol* actor_sym2 = lookup_symbol(table, base_type->element_type->struct_name);
                    if (actor_sym2 && actor_sym2->node) {
                        ASTNode* actor_def2 = actor_sym2->node;
                        for (int fi = 0; fi < actor_def2->child_count; fi++) {
                            ASTNode* field = actor_def2->children[fi];
                            if (field && field->type == AST_STATE_DECLARATION &&
                                field->value && strcmp(field->value, expr->value) == 0) {
                                if (field->node_type && field->node_type->kind != TYPE_UNKNOWN) {
                                    expr->node_type = clone_type(field->node_type);
                                }
                                break;
                            }
                        }
                    }
                    // Fallback to general inference
                    if (!expr->node_type || expr->node_type->kind == TYPE_UNKNOWN) {
                        expr->node_type = infer_type(expr, table);
                    }
                }
                // Handle struct member access — look up field type from definition.
                // For `extern struct` types whose fields include union/nested
                // compounds, we need to walk children with awareness of the
                // compound shape — see lookup_extern_field_type in this file.
                else if (base_type && base_type->kind == TYPE_STRUCT && base_type->struct_name) {
                    // Anonymous compound — base_type.compound_node points
                    // directly at the AST_STRUCT_FIELD_UNION / _NESTED node
                    // produced from a previous member access. The compound's
                    // children ARE the lookup list.
                    ASTNode* children_owner = NULL;
                    if (base_type->compound_node) {
                        children_owner = base_type->compound_node;
                    } else {
                        Symbol* struct_sym = lookup_symbol(table, base_type->struct_name);
                        if (struct_sym && struct_sym->node) children_owner = struct_sym->node;
                    }
                    if (children_owner) {
                        int found = 0;
                        for (int fi = 0; fi < children_owner->child_count; fi++) {
                            ASTNode* field = children_owner->children[fi];
                            if (!field || !field->value) continue;
                            if (strcmp(field->value, expr->value) != 0) continue;
                            if (field->type == AST_STRUCT_FIELD_UNION ||
                                field->type == AST_STRUCT_FIELD_NESTED) {
                                // Compound child: synthesize a TYPE_STRUCT
                                // whose compound_node points at THIS field
                                // so the next access can walk its children.
                                Type* ct = create_type(TYPE_STRUCT);
                                ct->struct_name = strdup("__AeCompound");
                                ct->compound_node = field;
                                expr->node_type = ct;
                            } else if (field->node_type && field->node_type->kind != TYPE_UNKNOWN) {
                                expr->node_type = clone_type(field->node_type);
                            }
                            found = 1;
                            break;
                        }
                        if (!found) {
                            char error_msg[256];
                            snprintf(error_msg, sizeof(error_msg),
                                     "Struct '%s' has no field '%s'",
                                     base_type->struct_name, expr->value ? expr->value : "?");
                            free_type(base_type);
                            type_error(error_msg, expr->line, expr->column);
                            return 0;
                        }
                    }
                    // Fallback to general inference
                    if (!expr->node_type || expr->node_type->kind == TYPE_UNKNOWN) {
                        expr->node_type = infer_type(expr, table);
                    }
                }
                // Pointer-to-struct member access: `e.field` where e: *Foo
                // Look through the TYPE_PTR to the underlying struct's
                // field list, identical to TYPE_STRUCT above. Without
                // this branch the field's declared type (e.g. `string`)
                // never landed on the AST_MEMBER_ACCESS node, leaving
                // string-interpolation codegen guessing — which printed
                // string fields as `%d` (the int default in the
                // interp's TypeKind switch) rather than `%s`. Issue
                // surfaces on self-referential structs walked through
                // a `*Self` chain. Section C.1 of fresh-aether-requests.
                else if (base_type && base_type->kind == TYPE_PTR &&
                         base_type->element_type &&
                         base_type->element_type->kind == TYPE_STRUCT &&
                         base_type->element_type->struct_name) {
                    Symbol* struct_sym = lookup_symbol(table, base_type->element_type->struct_name);
                    if (struct_sym && struct_sym->node) {
                        ASTNode* struct_def = struct_sym->node;
                        int found = 0;
                        for (int fi = 0; fi < struct_def->child_count; fi++) {
                            ASTNode* field = struct_def->children[fi];
                            if (!field || !field->value) continue;
                            if (strcmp(field->value, expr->value) != 0) continue;
                            if (field->type == AST_STRUCT_FIELD_UNION ||
                                field->type == AST_STRUCT_FIELD_NESTED) {
                                Type* ct = create_type(TYPE_STRUCT);
                                ct->struct_name = strdup("__AeCompound");
                                ct->compound_node = field;
                                expr->node_type = ct;
                            } else if (field->node_type && field->node_type->kind != TYPE_UNKNOWN) {
                                expr->node_type = clone_type(field->node_type);
                            }
                            found = 1;
                            break;
                        }
                        if (!found) {
                            char error_msg[256];
                            snprintf(error_msg, sizeof(error_msg),
                                     "Struct '%s' has no field '%s'",
                                     base_type->element_type->struct_name,
                                     expr->value ? expr->value : "?");
                            free_type(base_type);
                            type_error(error_msg, expr->line, expr->column);
                            return 0;
                        }
                    }
                    if (!expr->node_type || expr->node_type->kind == TYPE_UNKNOWN) {
                        expr->node_type = infer_type(expr, table);
                    }
                }
                free_type(base_type);
            }
            return 1;
        }
            
        case AST_SEND_FIRE_FORGET: {
            // actor ! MessageType { fields... }  — validate both operands
            if (expr->child_count >= 2) {
                ASTNode* actor_ref = expr->children[0];
                ASTNode* message   = expr->children[1];

                typecheck_expression(actor_ref, table);

                // Validate that the message type is a registered message definition
                if (message->type == AST_MESSAGE_CONSTRUCTOR && message->value) {
                    Symbol* msg_sym = lookup_symbol(table, message->value);
                    if (!msg_sym || !msg_sym->type || msg_sym->type->kind != TYPE_MESSAGE) {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg),
                                 "Undefined message type '%s'", message->value);
                        type_error(error_msg, message->line, message->column);
                        return 0;
                    }
                }

                // Validate field value types match declared field types
                typecheck_message_constructor(message, table);
            }
            expr->node_type = create_type(TYPE_VOID);
            return 1;
        }

        default:
            // Type check all children
            for (int i = 0; i < expr->child_count; i++) {
                typecheck_expression(expr->children[i], table);
            }
            return 1;
    }
}

int typecheck_binary_expression(ASTNode* expr, SymbolTable* table) {
    if (!expr || expr->type != AST_BINARY_EXPRESSION || expr->child_count < 2) return 0;
    
    ASTNode* left = expr->children[0];
    ASTNode* right = expr->children[1];
    
    typecheck_expression(left, table);
    typecheck_expression(right, table);
    
    Type* left_type = infer_type(left, table);
    Type* right_type = infer_type(right, table);

    AeTokenType operator = get_token_type_from_string(expr->value);

    // Reject `string + string` at typecheck rather than emitting
    // invalid C (`(const char*) + (const char*)` is a pointer-arith
    // error, not a useful diagnostic to anyone). Aether doesn't
    // overload `+` for strings; the idiomatic ways to join strings
    // are interpolation `"${a}${b}"` (literal-time) or
    // `string.concat(a, b)` / `string.format(fmt, args)` (runtime).
    // Closes #276.
    if (operator == TOKEN_PLUS &&
        left_type && left_type->kind == TYPE_STRING &&
        right_type && right_type->kind == TYPE_STRING) {
        free_type(left_type);
        free_type(right_type);
        type_error("'+' is not defined for strings — use \"${a}${b}\" interpolation or string.concat(a, b)",
                   expr->line, expr->column);
        return 0;
    }

    if (operator == TOKEN_ASSIGN) {
        if (!is_assignable(right_type, left_type)) {
            free_type(left_type);
            free_type(right_type);
            type_error("Type mismatch in assignment", expr->line, expr->column);
            return 0;
        }
        /* `b = <int literal>` where b: byte — same range check as the
         * declaration / AST_ASSIGNMENT paths. The plain `=` operator
         * parses as AST_BINARY_EXPRESSION, so we need the check here
         * too. */
        if (left_type && left_type->kind == TYPE_BYTE &&
            byte_assignment_literal_out_of_range(right)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                "byte literal out of range: %s does not fit in 0..255",
                right->value ? right->value : "?");
            type_error(msg, expr->line, expr->column);
            free_type(left_type);
            free_type(right_type);
            return 0;
        }
        expr->node_type = clone_type(left_type);
    } else {
        Type* result_type = infer_binary_type(left, right, operator);
        if (result_type->kind == TYPE_UNKNOWN &&
            left_type && left_type->kind != TYPE_UNKNOWN &&
            right_type && right_type->kind != TYPE_UNKNOWN) {
            // Only error if both types are known but incompatible
            free_type(left_type);
            free_type(right_type);
            free_type(result_type);
            type_error("Invalid operation for given types", expr->line, expr->column);
            return 0;
        }
        expr->node_type = result_type;
        /* #697: this op is 64-bit — widen any 32-bit-int arithmetic
         * operands so the computation happens in 64 bits, not in C int. */
        if (result_type && (result_type->kind == TYPE_INT64 ||
                            result_type->kind == TYPE_UINT64)) {
            propagate_int_width_64(left, result_type->kind);
            propagate_int_width_64(right, result_type->kind);
        }
    }

    free_type(left_type);
    free_type(right_type);
    return 1;
}

/* #749: resolve `<recv>.<field>` where `recv` is a local struct or
 * pointer-to-struct whose `field` is a function-pointer member. Returns
 * the field's TYPE_FUNCTION (is_fnptr) Type* (borrowed — do not free) or
 * NULL. *out_is_ptr is set to 1 when the receiver is a pointer-to-struct
 * (codegen then emits `->field` rather than `.field`). */
static Type* resolve_fnptr_struct_field(SymbolTable* table, const char* recv_name,
                                        const char* field_name, int* out_is_ptr) {
    Symbol* rs = lookup_symbol(table, recv_name);
    if (!rs || !rs->type) return NULL;
    Type* st = rs->type;
    const char* sname = NULL;
    int is_ptr = 0;
    if (st->kind == TYPE_STRUCT && st->struct_name) {
        sname = st->struct_name;
    } else if (st->kind == TYPE_PTR && st->element_type &&
               st->element_type->kind == TYPE_STRUCT && st->element_type->struct_name) {
        sname = st->element_type->struct_name;
        is_ptr = 1;
    } else {
        return NULL;
    }
    Symbol* ss = lookup_symbol(table, sname);
    if (!ss || !ss->node) return NULL;
    for (int fi = 0; fi < ss->node->child_count; fi++) {
        ASTNode* f = ss->node->children[fi];
        if (f && f->value && strcmp(f->value, field_name) == 0) {
            if (f->node_type && f->node_type->kind == TYPE_FUNCTION &&
                f->node_type->is_fnptr) {
                if (out_is_ptr) *out_is_ptr = is_ptr;
                return f->node_type;
            }
            return NULL;
        }
    }
    return NULL;
}

int typecheck_function_call(ASTNode* call, SymbolTable* table) {
    if (!call || call->type != AST_FUNCTION_CALL) return 0;

    // Use qualified lookup to handle namespaced calls like string.new -> string_new
    Symbol* symbol = lookup_qualified_symbol(table, call->value);

    // Issue #333 DSL receiver fallback: when the call is bare-name
    // (no dot in call->value) and the symbol resolved through the
    // <receiver>_<name> rewrite in lookup_symbol, the symbol's
    // canonical C name differs from call->value. Rewrite call->value
    // to the qualified form `<receiver>.<name>` so codegen (which
    // converts `.` to `_`) emits the resolved C symbol rather than
    // the bare identifier — without this, you get
    // "implicit function declaration" warnings and a link error.
    if (symbol && call->value && !strchr(call->value, '.') &&
        symbol->name && strcmp(symbol->name, call->value) != 0) {
        // The fallback rewrite is `<receiver>_<name>`. Reverse the
        // direction so call->value carries `<receiver>.<name>` —
        // matching the convention used by lookup_qualified_symbol's
        // qualified-form input. Find the underscore that separates
        // the receiver prefix from the original bare name (suffix
        // matches call->value).
        size_t orig_len = strlen(call->value);
        size_t resolved_len = strlen(symbol->name);
        if (resolved_len > orig_len + 1 &&
            strcmp(symbol->name + resolved_len - orig_len, call->value) == 0 &&
            symbol->name[resolved_len - orig_len - 1] == '_') {
            size_t prefix_len = resolved_len - orig_len - 1;
            size_t qualified_len = prefix_len + 1 + orig_len + 1;
            char* qualified = malloc(qualified_len);
            if (qualified) {
                memcpy(qualified, symbol->name, prefix_len);
                qualified[prefix_len] = '.';
                memcpy(qualified + prefix_len + 1, call->value, orig_len);
                qualified[qualified_len - 1] = '\0';
                free(call->value);
                call->value = qualified;
            }
        }
    }

    // Rewrite import alias to qualified name for codegen (e.g. "release" -> "build.release")
    if (symbol && symbol->is_function && call->value) {
        const char* alias_target = find_import_alias(call->value);
        if (alias_target) {
            free(call->value);
            call->value = strdup(alias_target);
        }
    }

    // Typed C function-pointer local call: `fp(a, b)` where `fp` is
    // a local of type `fn(T1, T2, ...) -> R` (is_fnptr=1).  Validate
    // each argument and stamp the call's return type, but DON'T
    // rewrite to `call(fp, a, b)` — the codegen has a dedicated
    // fnptr-call branch that wants the original AST shape so it can
    // emit `((R (*)(T1, T2))(fp))(a, b)` inline.
    if (symbol && !symbol->is_function && symbol->type &&
        symbol->type->kind == TYPE_FUNCTION && symbol->type->is_fnptr &&
        call->value) {
        for (int i = 0; i < call->child_count; i++) {
            typecheck_expression(call->children[i], table);
        }
        if (symbol->type->return_type) {
            call->node_type = clone_type(symbol->type->return_type);
        } else {
            call->node_type = create_type(TYPE_VOID);
        }
        return 1;
    }

    // Phase A3 (foundation for #260 D pure-Aether middleware): if the
    // call's target name resolves to a local variable whose type is
    // TYPE_FUNCTION (a closure / function-typed value), this is a
    // direct invocation of a function-typed local — `handler(req, res)`
    // where `handler` is a local variable. Transparently rewrite the
    // AST to flow through the existing `call(fn, args...)` codegen
    // path (codegen_expr.c:~2155). This lets users write the natural
    // form rather than the workaround `call(handler, req, res)`.
    //
    // (The is_fnptr=1 case was handled above; this block is reached
    // only for closure-shaped fn-typed values.)
    if (symbol && !symbol->is_function && symbol->type &&
        symbol->type->kind == TYPE_FUNCTION && !symbol->type->is_fnptr &&
        call->value) {
        // Build a new child list: [fn_ref, original_args...]
        ASTNode* fn_ref = create_ast_node(AST_IDENTIFIER, call->value,
                                          call->line, call->column);
        fn_ref->node_type = clone_type(symbol->type);

        int old_count = call->child_count;
        ASTNode** new_children = malloc(sizeof(ASTNode*) * (old_count + 1));
        new_children[0] = fn_ref;
        for (int i = 0; i < old_count; i++) {
            new_children[i + 1] = call->children[i];
        }
        if (call->children) free(call->children);
        call->children = new_children;
        call->child_count = old_count + 1;

        // Rename the call from <varname> to "call" so codegen routes
        // through the existing closure-invocation path.
        free(call->value);
        call->value = strdup("call");

        // Type-check argument expressions (skip the new fn_ref at
        // index 0; its type is already set above).
        for (int i = 1; i < call->child_count; i++) {
            typecheck_expression(call->children[i], table);
        }

        // The call's return type is the function-type's return slot,
        // when known. Otherwise leave UNKNOWN — type inference may
        // refine it later.
        if (symbol->type->return_type) {
            call->node_type = clone_type(symbol->type->return_type);
        } else {
            call->node_type = create_type(TYPE_UNKNOWN);
        }
        return 1;
    }

    /* #749: dispatch through a function-pointer struct field —
     * `recv.fnptrfield(args)`. The parser collapsed the member-access
     * callee to the dotted name `recv.field` (dropping the receiver
     * subtree), and it did not resolve as a function symbol above. If
     * `recv` is a local struct / pointer-to-struct whose `field` is a
     * function pointer, this is a typed indirect call — typecheck the
     * args against the field's signature, stamp the return type, and tag
     * the node so codegen emits `(recv.field)(args)` / `(recv->field)(args)`.
     * Single-level receiver only (a bare local); the part before the dot
     * must contain no further dots. */
    if ((!symbol || !symbol->is_function) && call->value) {
        char* dot = strrchr(call->value, '.');
        size_t rlen = dot ? (size_t)(dot - call->value) : 0;
        if (dot && rlen > 0 && rlen < 200 && !memchr(call->value, '.', rlen)) {
            char recv[200];
            memcpy(recv, call->value, rlen);
            recv[rlen] = '\0';
            int is_ptr = 0;
            Type* fsig = resolve_fnptr_struct_field(table, recv, dot + 1, &is_ptr);
            if (fsig) {
                for (int i = 0; i < call->child_count; i++) {
                    typecheck_expression(call->children[i], table);
                }
                call->node_type = fsig->return_type
                    ? clone_type(fsig->return_type) : create_type(TYPE_UNKNOWN);
                if (call->annotation) free(call->annotation);
                call->annotation = strdup(is_ptr ? "fnfield_ptr" : "fnfield_val");
                return 1;
            }
        }
    }

    if (!symbol || !symbol->is_function) {
        char error_msg[256];
        // Check if this is a visibility rejection (not-exported) rather than truly undefined
        if (call->value && strchr(call->value, '.') && global_module_registry) {
            char* tmp = strdup(call->value);
            char* dot = strchr(tmp, '.');
            *dot = '\0';
            if (is_export_blocked(tmp, dot + 1)) {
                snprintf(error_msg, sizeof(error_msg),
                         "'%s' is not exported from module '%s'", dot + 1, tmp);
            } else {
                snprintf(error_msg, sizeof(error_msg),
                         "Undefined function '%s'", call->value);
            }
            free(tmp);
        } else {
            snprintf(error_msg, sizeof(error_msg),
                     "Undefined function '%s'", call->value ? call->value : "?");
        }
        type_error(error_msg, call->line, call->column);
        return 0;
    }

    // Arity check: user-defined functions have their AST node stored
    if (symbol->node && (symbol->node->type == AST_FUNCTION_DEFINITION || symbol->node->type == AST_BUILDER_FUNCTION)) {
        int expected = count_function_params(symbol->node);
        int required = count_required_params(symbol->node);
        int ctx_first = has_ctx_first_param(symbol->node);
        int got = call->child_count;
        // If mismatch, try excluding trailing closures (for functions that
        // don't accept fn params but have trailing blocks for DSL syntax)
        if (got != expected && !(ctx_first && got == expected - 1)) {
            int non_closure = 0;
            for (int i = 0; i < call->child_count; i++) {
                if (call->children[i] && call->children[i]->type != AST_CLOSURE) {
                    non_closure++;
                }
            }
            if (non_closure == expected ||
                (ctx_first && non_closure == expected - 1)) {
                got = non_closure; // trailing closures are DSL blocks, not args
            }
        }
        // Functions with _ctx as the first param accept either:
        //   - expected args (caller passed _ctx explicitly), or
        //   - expected-1 args (builder DSL auto-injects _ctx at the call site)
        // Phase A2.1 default arguments: a call with `got` between
        // `required` and `expected` (inclusive) is also OK — the
        // missing trailing args get filled in below from the
        // declared defaults.
        // A C-variadic callee (declared `f(fixed..., ...)`, marked with
        // annotation "varargs") accepts any number of trailing args
        // beyond its named parameters — so any count >= the named-param
        // count is fine.
        int is_variadic_callee = (symbol->node->annotation &&
                                  strcmp(symbol->node->annotation, "varargs") == 0);
        int arity_ok = (got == expected) ||
                       (ctx_first && got == expected - 1) ||
                       (got >= required && got < expected) ||
                       (is_variadic_callee && got >= expected);
        if (!arity_ok) {
            char error_msg[256];
            if (required < expected) {
                snprintf(error_msg, sizeof(error_msg),
                         "Function '%s' expects %d-%d argument(s), got %d",
                         call->value, required, expected, got);
            } else {
                snprintf(error_msg, sizeof(error_msg),
                         "Function '%s' expects %d argument(s), got %d",
                         call->value, expected, got);
            }
            type_error(error_msg, call->line, call->column);
            return 0;
        }

        // Phase A2.1 default-arg fill: if the caller passed fewer
        // args than the callee declared (within the required..total
        // window allowed above), append clones of the declared
        // default expressions to the call's child list. After this
        // codegen sees a fully-populated call and emits the right C.
        // Defaults trail required, so the missing slots are always
        // the trailing tail.
        if (got >= required && got < expected) {
            int param_idx = 0;
            for (int i = 0; i < symbol->node->child_count - 1 &&
                            call->child_count < expected; i++) {
                ASTNode* p = symbol->node->children[i];
                if (!p) continue;
                if (p->type == AST_GUARD_CLAUSE) continue;
                if (p->type != AST_VARIABLE_DECLARATION &&
                    p->type != AST_PATTERN_VARIABLE &&
                    p->type != AST_PATTERN_LITERAL) continue;
                // Skip param indexes the caller already supplied.
                if (param_idx < got) {
                    param_idx++;
                    continue;
                }
                if (!param_has_default(p)) {
                    // Should be unreachable given the trailing-default
                    // rule, but defensively: error instead of silently
                    // filling with an undefined value.
                    char err[256];
                    snprintf(err, sizeof(err),
                             "Function '%s' parameter %d has no default — caller must supply it",
                             call->value, param_idx + 1);
                    type_error(err, call->line, call->column);
                    return 0;
                }
                ASTNode* default_clone = clone_ast_node(p->children[0]);
                if (!default_clone) {
                    type_error("internal: failed to clone default expression",
                               call->line, call->column);
                    return 0;
                }
                // Phase A2.2: rewrite source-location intrinsics in
                // the clone so they capture the caller's location
                // rather than the function definition's. Closes #265.
                rewrite_caller_site_intrinsics(default_clone, call->line, call->column);
                add_child(call, default_clone);
                param_idx++;
            }
        }
    }

    // Issue #333 DSL block receiver scoping: when the call is in
    // member-access form (`receiver.method(args) { body }`) and has
    // trailing AST_CLOSURE children, stamp those closures with the
    // receiver's namespace so unqualified calls inside the body can
    // fall back through `<receiver>_<name>` (see lookup_symbol).
    //
    // Receiver resolution: split call->value on `.`. If the prefix
    // is a visible namespace (`bash` in `bash.test(...)`), use it
    // verbatim. If the prefix is a local variable of struct type
    // (`b: Builder`), use the type's name (`Builder`). The rewrite
    // is the same in both cases — namespace and struct-method helpers
    // share the `<prefix>_<name>` codegen convention.
    //
    // Stamping uses the AST_CLOSURE node's annotation field with a
    // `dsl_recv:` prefix; AST_CLOSURE typecheck reads it and seeds
    // closure_scope->dsl_receiver. Idempotent: existing annotations
    // (none today on AST_CLOSURE; `defer` factory annotations live
    // on AST_VARIABLE_DECLARATION) are preserved.
    if (call->value && strchr(call->value, '.')) {
        const char* dot = strchr(call->value, '.');
        size_t prefix_len = (size_t)(dot - call->value);
        if (prefix_len > 0 && prefix_len < 256) {
            char prefix[256];
            memcpy(prefix, call->value, prefix_len);
            prefix[prefix_len] = '\0';

            const char* dsl_recv_name = NULL;
            char type_name_buf[256];

            // Namespace receiver: directly visible.
            if (is_visible_namespace(prefix, table)) {
                dsl_recv_name = prefix;
            } else {
                // Typed-value receiver: prefix is a local variable;
                // use its struct type's name.
                Symbol* recv_sym = lookup_symbol(table, prefix);
                if (recv_sym && recv_sym->type) {
                    const char* tn = type_name(recv_sym->type);
                    if (tn && tn[0] != '\0' &&
                        strlen(tn) < sizeof(type_name_buf) &&
                        strcmp(tn, "unknown") != 0 &&
                        strcmp(tn, "int") != 0 &&
                        strcmp(tn, "long") != 0 &&
                        strcmp(tn, "float") != 0 &&
                        strcmp(tn, "double") != 0 &&
                        strcmp(tn, "bool") != 0 &&
                        strcmp(tn, "string") != 0) {
                        strncpy(type_name_buf, tn, sizeof(type_name_buf) - 1);
                        type_name_buf[sizeof(type_name_buf) - 1] = '\0';
                        dsl_recv_name = type_name_buf;
                    }
                }
            }

            if (dsl_recv_name) {
                for (int i = 0; i < call->child_count; i++) {
                    ASTNode* child = call->children[i];
                    if (child && child->type == AST_CLOSURE) {
                        // Idempotent: skip if already stamped.
                        if (child->annotation &&
                            strncmp(child->annotation, "dsl_recv:", 9) == 0) {
                            continue;
                        }
                        size_t a_len = 9 + strlen(dsl_recv_name) + 1;
                        char* ann = malloc(a_len);
                        if (ann) {
                            snprintf(ann, a_len, "dsl_recv:%s", dsl_recv_name);
                            // Defensive: if annotation is unset, set it.
                            // If it's set to something else (no current
                            // user of AST_CLOSURE annotation, but be safe),
                            // leave it alone so we don't stomp.
                            if (!child->annotation) {
                                child->annotation = ann;
                            } else {
                                free(ann);
                            }
                        }
                    }
                }
            }
        }
    }

    // Type check arguments and validate types against parameters
    for (int i = 0; i < call->child_count; i++) {
        typecheck_expression(call->children[i], table);
    }

    /* Duration parameter-passing strictness (issue #586): plain int /
     * long / uint64 / float passed to a Duration-typed parameter
     * silently reinterprets as nanoseconds, which bit PR #583 (the
     * test that called `client.set_timeout(req, 5)` got a 5ns timeout
     * and failed on macOS). The comparison rule already rejects
     * `dur > 5` at infer_binary_type; this extends the same rule to
     * parameter-passing for BOTH extern and user-defined callees.
     *
     * Bare numeric literals must use a unit suffix (`5s`, `200ms`,
     * `100ns`); identifier values typed as a plain integer need to
     * be wrapped in an explicit construction. The error message
     * names the literal forms so the fix is obvious.
     *
     * Scope is intentionally narrow to TYPE_DURATION; if/when other
     * tagged-int types land (Bytes, Pixels, …) generalise then. */
    if (symbol->node &&
        (symbol->node->type == AST_FUNCTION_DEFINITION ||
         symbol->node->type == AST_BUILDER_FUNCTION ||
         symbol->node->type == AST_EXTERN_FUNCTION)) {
        int has_ctx = has_ctx_first_param(symbol->node);
        int arg_offset = 0;
        int got = call->child_count;
        int expected = count_function_params(symbol->node);
        /* Mirror the arity logic at line ~3957: when the callee
         * declares _ctx first and the caller's arg count is
         * expected-1, the DSL auto-injects _ctx, so param index 0
         * has no corresponding arg slot — skip it by starting
         * arg_offset at -1 (param[1] aligns to arg[0]). */
        if (has_ctx && got == expected - 1) arg_offset = -1;
        int param_idx = 0;
        /* User-defined / builder functions store the body as the final
         * child; extern declarations do not. Cap the loop accordingly
         * so we don't try to treat the body as a parameter. */
        int param_scan_limit =
            (symbol->node->type == AST_EXTERN_FUNCTION)
                ? symbol->node->child_count
                : symbol->node->child_count - 1;
        for (int i = 0; i < param_scan_limit; i++) {
            ASTNode* param = symbol->node->children[i];
            if (!param) continue;
            if (param->type == AST_GUARD_CLAUSE) continue;
            /* Param-decl shapes:
             *   - User-defined fn:  AST_VARIABLE_DECLARATION or AST_PATTERN_VARIABLE
             *   - Extern fn:        AST_IDENTIFIER (parser builds them this way;
             *                       see parse_extern_declaration in parser.c)
             * Skip anything that isn't one of these param shapes. */
            if (param->type != AST_VARIABLE_DECLARATION &&
                param->type != AST_PATTERN_VARIABLE &&
                param->type != AST_IDENTIFIER) continue;

            int arg_slot = param_idx + arg_offset;
            param_idx++;
            if (arg_slot < 0 || arg_slot >= call->child_count) continue;

            Type* param_type = param->node_type;
            if (!param_type || param_type->kind != TYPE_DURATION) continue;

            ASTNode* arg = call->children[arg_slot];
            if (!arg) continue;
            Type* arg_type = infer_type(arg, table);
            if (!arg_type) continue;

            int arg_is_plain_numeric =
                arg_type->kind == TYPE_INT ||
                arg_type->kind == TYPE_INT64 ||
                arg_type->kind == TYPE_UINT64 ||
                arg_type->kind == TYPE_UINT32 ||
                arg_type->kind == TYPE_UINT16 ||
                arg_type->kind == TYPE_UINT8 ||
                arg_type->kind == TYPE_FLOAT ||
                arg_type->kind == TYPE_LONGDOUBLE;
            int arg_is_duration = arg_type->kind == TYPE_DURATION;
            int arg_is_unknown = arg_type->kind == TYPE_UNKNOWN;

            if (arg_is_plain_numeric && !arg_is_duration && !arg_is_unknown) {
                char error_msg[384];
                const char* pname = param->value ? param->value : "?";
                snprintf(error_msg, sizeof(error_msg),
                         "Cannot pass %s where Duration expected "
                         "(argument %d '%s' of '%s'). "
                         "Use a time-unit suffix on the literal: "
                         "5ns, 5us, 5ms, 5s, 5m, 5h, 5d (compound "
                         "forms like 2m30s also accepted).",
                         type_name(arg_type),
                         arg_slot + 1, pname,
                         call->value ? call->value : "?");
                type_error(error_msg, arg->line, arg->column);
            }
            free_type(arg_type);
        }
    }

    // Validate argument types for extern functions (which always have typed params)
    // Skip for user-defined functions since type inference may not have set param types yet
    if (symbol->node && symbol->node->type == AST_EXTERN_FUNCTION) {
        int param_idx = 0;
        for (int i = 0; i < symbol->node->child_count - 1 && param_idx < call->child_count; i++) {
            ASTNode* param = symbol->node->children[i];
            if (!param) { param_idx++; continue; }
            if (param->type == AST_VARIABLE_DECLARATION || param->type == AST_PATTERN_VARIABLE) {
                Type* param_type = param->node_type;
                if (param_type && param_type->kind != TYPE_UNKNOWN &&
                    call->children[param_idx] != NULL) {
                    Type* arg_type = infer_type(call->children[param_idx], table);
                    if (arg_type && arg_type->kind != TYPE_UNKNOWN &&
                        !is_type_compatible(arg_type, param_type)) {
                        char error_msg[256];
                        snprintf(error_msg, sizeof(error_msg),
                                 "Argument %d of '%s': expected %s, got %s",
                                 param_idx + 1, call->value,
                                 type_name(param_type), type_name(arg_type));
                        type_error(error_msg, call->children[param_idx]->line,
                                   call->children[param_idx]->column);
                    }
                    if (arg_type) free_type(arg_type);
                }
                param_idx++;
            }
        }
    }

    call->node_type = symbol->type ? clone_type(symbol->type) : create_type(TYPE_UNKNOWN);

    // select() infers its type from the first named arg's value
    if (call->value && strcmp(call->value, "select") == 0 &&
        (!call->node_type || call->node_type->kind == TYPE_UNKNOWN)) {
        for (int i = 0; i < call->child_count; i++) {
            ASTNode* arg = call->children[i];
            if (arg && arg->type == AST_NAMED_ARG &&
                arg->child_count > 0 && arg->children[0] &&
                arg->children[0]->node_type &&
                arg->children[0]->node_type->kind != TYPE_UNKNOWN) {
                if (call->node_type) free_type(call->node_type);
                call->node_type = clone_type(arg->children[0]->node_type);
                break;
            }
        }
    }

    return 1;
}
