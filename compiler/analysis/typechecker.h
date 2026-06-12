#ifndef TYPECHECKER_H
#define TYPECHECKER_H

#include "../ast.h"

typedef struct Symbol {
    char* name;
    Type* type;
    int is_actor;
    int is_function;
    int is_state;
    int is_module_alias;        // Indicates this is a module alias
    char* alias_target;         // The actual module name for aliases
    ASTNode* node;  // Pointer to AST node (for structs, functions, etc.)
    int type_inferred;          // 1 if this binding's type was inferred
                                // from a bare `x = expr` declaration (no
                                // annotation) and is a 32-bit int — drives
                                // the #698 silent-narrowing guard.
    struct Symbol* next;
} Symbol;

// Linked list of identifier names, used for hide / seal-except sets.
typedef struct NameNode {
    char* name;
    struct NameNode* next;
} NameNode;

typedef struct SymbolTable {
    Symbol* symbols;
    struct SymbolTable* parent;
    // Hide / seal directives that apply to this scope. They affect
    // lookups that would otherwise resolve to the parent chain — local
    // bindings in `symbols` are always visible regardless.
    NameNode* hidden_names;       // names blocked from outer scopes
    NameNode* seal_whitelist;     // if non-NULL, ONLY these names may resolve to outer scopes
    int is_sealed;                // 1 if a `seal except` directive is in effect (whitelist may be empty)
    // Issue #243 sealed-scope follow-up: 1 when this scope (or any
    // ancestor) is the body of a function that was cloned in from a
    // transitively-merged module via module_merge_into_program. Such
    // bodies legitimately need to call into other transitively-merged
    // namespaces (e.g. a cloned `client_post_json` calls
    // `json.stringify(...)`) even though the user never wrote
    // `import std.json` themselves. User code (where this flag is 0)
    // can only resolve qualified calls to namespaces it explicitly
    // imported. Propagated from parent in create_symbol_table.
    int inside_merged_body;
    // Issue #333 DSL block receiver scoping: when this scope is the
    // body of a trailing closure bound to a member-access call
    // (`receiver.method(args) { body }`), this names the receiver's
    // namespace (or struct type) so unqualified calls inside `body`
    // can fall back to `<dsl_receiver>_<name>` before erroring as
    // undefined. NULL when the scope is not a DSL block body.
    // Resolution order in lookup_symbol: local, then this fallback,
    // then parent chain. Local definitions still win; existing
    // qualified calls and selective imports unchanged.
    char* dsl_receiver;
} SymbolTable;

// Symbol table functions
SymbolTable* create_symbol_table(SymbolTable* parent);
void free_symbol_table(SymbolTable* table);
void add_symbol(SymbolTable* table, const char* name, Type* type, int is_actor, int is_function, int is_state);
Symbol* lookup_symbol(SymbolTable* table, const char* name);
Symbol* lookup_symbol_local(SymbolTable* table, const char* name);

// Hide / seal directive helpers
void scope_hide_name(SymbolTable* table, const char* name);
void scope_seal_except(SymbolTable* table, const char* name);
int  scope_name_is_hidden(SymbolTable* table, const char* name);
int  scope_name_in_whitelist(SymbolTable* table, const char* name);

// Module alias functions
void add_module_alias(SymbolTable* table, const char* alias, const char* module_name);
Symbol* resolve_module_alias(SymbolTable* table, const char* name);
Symbol* lookup_qualified_symbol(SymbolTable* table, const char* qualified_name);

// Namespace-visibility helper (issue #243). Returns 1 if a qualified
// call `<name>.<member>` is allowed from a scope whose `table` is
// passed in. Merged-body scopes (table->inside_merged_body) see all
// transitively-merged namespaces; user code only sees explicit
// imports.
int is_visible_namespace(const char* name, SymbolTable* table);

// Type checking functions
int typecheck_program(ASTNode* program);
int typecheck_node(ASTNode* node, SymbolTable* table);
int typecheck_actor_definition(ASTNode* actor, SymbolTable* table);
int typecheck_function_definition(ASTNode* func, SymbolTable* table);
int typecheck_struct_definition(ASTNode* struct_def, SymbolTable* table);
int typecheck_statement(ASTNode* stmt, SymbolTable* table);
int typecheck_expression(ASTNode* expr, SymbolTable* table);
int typecheck_binary_expression(ASTNode* expr, SymbolTable* table);
int typecheck_function_call(ASTNode* call, SymbolTable* table);

// Type inference functions
Type* infer_type(ASTNode* expr, SymbolTable* table);
Type* infer_binary_type(ASTNode* left, ASTNode* right, AeTokenType operator);
Type* infer_unary_type(ASTNode* operand, AeTokenType operator);

// Type compatibility functions
int is_type_compatible(Type* from, Type* to);
int is_assignable(Type* from, Type* to);
int is_callable(Type* type);

// Utility functions
AeTokenType get_token_type_from_string(const char* str);

// Error reporting
void type_error(const char* message, int line, int column);
void type_warning(const char* message, int line, int column);

#endif
