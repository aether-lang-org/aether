#ifndef CODEGEN_INTERNAL_H
#define CODEGEN_INTERNAL_H

#include "codegen.h"
#include "../parser/lexer.h"
#include "../parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

/* Utilities (codegen.c) */
void indent(CodeGenerator* gen);
void unindent(CodeGenerator* gen);
void print_indent(CodeGenerator* gen);
void print_line(CodeGenerator* gen, const char* format, ...);
void codegen_maybe_emit_line(CodeGenerator* gen, const ASTNode* node);
const char* get_c_type(Type* type);
int is_c_reserved_word(const char* name);
const char* safe_c_name(const char* name);
const char* get_c_operator(const char* aether_op);
void generate_type(CodeGenerator* gen, Type* type);
int is_var_declared(CodeGenerator* gen, const char* var_name);
void mark_var_declared(CodeGenerator* gen, const char* var_name);
void clear_declared_vars(CodeGenerator* gen);
int is_heap_string_var(CodeGenerator* gen, const char* var_name);
void mark_heap_string_var(CodeGenerator* gen, const char* var_name);
void clear_heap_string_vars(CodeGenerator* gen);

/* Defer management (codegen.c) */
void push_defer(CodeGenerator* gen, ASTNode* stmt);
void push_auto_defer(CodeGenerator* gen, const char* free_fn, const char* var_name);
void emit_defers_for_scope(CodeGenerator* gen);
void emit_all_defers(CodeGenerator* gen);
void emit_all_defers_protected(CodeGenerator* gen, char** protected_names, int protected_count);
void enter_scope(CodeGenerator* gen);
void exit_scope(CodeGenerator* gen);

/* Expression generation (codegen_expr.c) */
void generate_expression(CodeGenerator* gen, ASTNode* expr);

/* Message field helpers (codegen_expr.c) — shared with codegen_stmt.c */
MessageFieldDef* find_msg_field(MessageDef* msg_def, const char* name);
void emit_message_field_init(CodeGenerator* gen, MessageFieldDef* fdef, ASTNode* rhs);
void emit_message_array_hoists(CodeGenerator* gen, ASTNode* message, MessageDef* msg_def);

/* Statement generation (codegen_stmt.c) */
void generate_statement(CodeGenerator* gen, ASTNode* stmt);
/* Hoist variables first-declared inside if-statement branches at the
   enclosing function-body scope when they're referenced outside the
   if-block. Closes #278. Called from codegen_func.c before iterating
   the body's top-level statements. */
void hoist_if_branch_vars(CodeGenerator* gen, ASTNode* body);

/* Hoist `_heap_<name>` companion trackers for every string variable
   in `body` to function-entry scope. Closes #405 — the architectural
   blocker that kept user-defined `-> string` functions from
   participating in the heap-string-reassignment wrapper. Called from
   codegen_func.c immediately after hoist_if_branch_vars. */
void hoist_heap_string_trackers(CodeGenerator* gen, ASTNode* body);

/* Set the program-root AST that `is_heap_string_expr` should consult
   when deciding whether a user-defined `-> string` function returns
   a heap-allocated string. Called once at codegen entry from
   generate_program; cleared with NULL at exit so iterative codegen
   sessions (LSP, REPL) don't see a stale pointer. */
void codegen_set_program_for_heap_lookup(ASTNode* program);

/* Actor generation (codegen_actor.c) */
void generate_actor_definition(CodeGenerator* gen, ASTNode* actor);

/* Extern function registry — tracks param types for call-site cast emission */
void register_extern_func(CodeGenerator* gen, ASTNode* ext);
int is_extern_func(CodeGenerator* gen, const char* func_name);
TypeKind lookup_extern_param_kind(CodeGenerator* gen, const char* func_name, int param_idx);
int is_aether_extern_param(CodeGenerator* gen, const char* func_name, int param_idx);
const char* lookup_extern_c_name(CodeGenerator* gen, const char* func_name);

/* Builder function registry — functions where block configures first, then function executes */
int is_builder_func_reg(CodeGenerator* gen, const char* func_name);
const char* get_builder_factory(CodeGenerator* gen, const char* func_name);

/* Function/struct generation (codegen_func.c) */
int has_return_value(ASTNode* node);
/* @c_callback annotation helpers (#235). A function declared with
   `@c_callback aether_name(...)` (or `@c_callback("c_sym") aether_name(...)`)
   gets a stable, externally-visible C symbol so it can be passed across
   module boundaries as a function pointer to C externs. Codegen drops
   the `static` storage class for these even when imported, and uses
   the chosen C symbol at the C-decl name slot and at every value-
   position reference. */
int is_c_callback(ASTNode* func);
const char* c_callback_symbol(ASTNode* func);
/* Look up a top-level @c_callback function by its current AST value
   (after any import-rename pass) and return its bound C symbol — or
   NULL when no such callback exists. Used at value-position
   AST_IDENTIFIER emission so a function name passed as a function
   pointer resolves to the linked symbol, not the Aether-side name. */
const char* lookup_c_callback_symbol(CodeGenerator* gen, const char* name);
void generate_extern_declaration(CodeGenerator* gen, ASTNode* ext);
void generate_function_definition(CodeGenerator* gen, ASTNode* func);
void generate_struct_definition(CodeGenerator* gen, ASTNode* struct_def);
void generate_combined_function(CodeGenerator* gen, ASTNode** clauses, int clause_count);

/* Main/program (codegen.c) */
void generate_main_function(CodeGenerator* gen, ASTNode* main);

/* Closure support (codegen_expr.c) */
void discover_closures(CodeGenerator* gen, ASTNode* node);
void emit_closure_definitions(CodeGenerator* gen);
/* L4 validation: reject closures inside actor handlers that write to
   actor state fields. Run after discover_closures, before codegen.
   Returns 1 on success, 0 if errors were reported. */
int validate_closure_state_mutations(CodeGenerator* gen, ASTNode* program);
/* Route 1 promotion queries (populated by discover_closures): */
void get_promoted_names_for_func(CodeGenerator* gen, const char* func_name,
                                 char*** out_names, int* out_count);
int is_promoted_capture(CodeGenerator* gen, const char* name);

/* Internal helpers shared across files */
int contains_send_expression(ASTNode* node);
const char* get_single_int_field(MessageDef* msg_def);
void generate_default_return_value(CodeGenerator* gen, Type* type);
int is_function_generated(CodeGenerator* gen, const char* func_name);
void mark_function_generated(CodeGenerator* gen, const char* func_name);
int count_function_clauses(ASTNode* program, const char* func_name);
ASTNode** collect_function_clauses(ASTNode* program, const char* func_name, int* out_count);

#endif
