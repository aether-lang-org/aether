#ifndef AST_H
#define AST_H

#include "parser/tokens.h"

typedef enum {
    // Program structure
    AST_PROGRAM,
    AST_MODULE_DECLARATION,
    AST_IMPORT_STATEMENT,
    AST_EXPORT_STATEMENT,
    AST_EXPORTS_LIST,        // top-of-file `exports (a, b, c)` declaration —
                             // children are AST_IDENTIFIER nodes naming the
                             // module's public API. Replaces per-function
                             // AST_EXPORT_STATEMENT for modules using the
                             // Erlang-style list form.
    AST_ACTOR_DEFINITION,
    AST_FUNCTION_DEFINITION,
    AST_FUNCTION_CLAUSE,
    AST_MAIN_FUNCTION,
    AST_STRUCT_DEFINITION,
    AST_STRUCT_FIELD,
    AST_EXTERN_FUNCTION,      // External C function declaration
    AST_BUILDER_FUNCTION,     // Builder function: block configures first, then function executes
    AST_CONST_DECLARATION,    // Top-level constant: const NAME = value

    // Statements
    AST_BLOCK,
    AST_VARIABLE_DECLARATION,
    AST_TUPLE_DESTRUCTURE,      // a, b = func() — multiple lvalues
    AST_ASSIGNMENT,
    AST_COMPOUND_ASSIGNMENT,  // x += expr, x -= expr, etc.
    AST_IF_STATEMENT,
    AST_FOR_LOOP,
    AST_WHILE_LOOP,
    AST_SWITCH_STATEMENT,
    AST_CASE_STATEMENT,
    AST_RETURN_STATEMENT,
    AST_BREAK_STATEMENT,
    AST_CONTINUE_STATEMENT,
    AST_DEFER_STATEMENT,
    AST_EXPRESSION_STATEMENT,
    AST_MATCH_STATEMENT,
    AST_MATCH_ARM,
    AST_PATTERN_LITERAL,
    AST_PATTERN_VARIABLE,
    AST_PATTERN_STRUCT,
    AST_PATTERN_LIST,
    AST_PATTERN_CONS,
    AST_GUARD_CLAUSE,
    AST_REQUIRES_CLAUSE,       // `requires <expr>` precondition (issue #348)
    AST_ENSURES_CLAUSE,        // `ensures <expr>` postcondition  (issue #348)
    AST_RECEIVE_STATEMENT,
    AST_SEND_STATEMENT,
    AST_SPAWN_ACTOR_STATEMENT,
    AST_STATE_DECLARATION,
    AST_HIDE_DIRECTIVE,        // hide name1, name2  — block named outer bindings in this scope
    AST_SEAL_DIRECTIVE,        // seal except a, b   — block all outer bindings except whitelist
    AST_TRY_STATEMENT,         // try { body } catch e { handler } — cooperative panic recovery
    AST_CATCH_CLAUSE,          // catch name { body }  — attached as child of AST_TRY_STATEMENT
    AST_PANIC_STATEMENT,       // panic("reason") — unwinds to innermost try or actor barrier

    // Actor V2 - Message system
    AST_MESSAGE_DEFINITION,
    AST_MESSAGE_FIELD,
    AST_RECEIVE_ARM,
    AST_MESSAGE_PATTERN,
    AST_PATTERN_FIELD,
    AST_WILDCARD_PATTERN,
    AST_TIMEOUT_ARM,
    AST_REPLY_STATEMENT,
    AST_MESSAGE_CONSTRUCTOR,
    AST_FIELD_INIT,
    AST_SEND_FIRE_FORGET,
    AST_SEND_ASK,
    
    // Expressions
    AST_BINARY_EXPRESSION,
    AST_UNARY_EXPRESSION,
    AST_FUNCTION_CALL,
    AST_ACTOR_REF,
    AST_IDENTIFIER,
    AST_LITERAL,
    AST_ARRAY_LITERAL,
    AST_ARRAY_ACCESS,
    AST_MEMBER_ACCESS,
    AST_STRUCT_LITERAL,
    AST_STRING_INTERP,      // interpolated string "Hello ${expr}"
    AST_NULL_LITERAL,       // null pointer literal
    AST_PTR_AS_STRUCT_CAST, // `expr as *StructName` — view a raw ptr as
                            // a pointer-to-struct. children[0] = expr
                            // (must be ptr-typed); value = struct name.
                            // Result type is TYPE_PTR with element_type
                            // = TYPE_STRUCT{name}; member-access codegen
                            // emits `->field` not `.field`.
    AST_IF_EXPRESSION,      // if cond { expr } else { expr } — value-producing

    // Closures
    AST_CLOSURE,            // |params| -> expr  OR  |params| { block }
    AST_CLOSURE_PARAM,      // parameter in a closure: name [: type]

    // Named arguments
    AST_NAMED_ARG,          // name: expr in function call arguments

    // Types
    AST_TYPE_ANNOTATION,
    AST_ACTOR_REF_TYPE,
    AST_ARRAY_TYPE,
    
    // Special
    AST_PRINT_STATEMENT
} ASTNodeType;

typedef enum {
    TYPE_INT,
    TYPE_INT64,
    TYPE_UINT64,
    TYPE_FLOAT,
    TYPE_BOOL,
    TYPE_BYTE,          // unsigned 8-bit (`unsigned char` in C). Type-precision
                        // for struct fields, function params, returns, locals.
                        // For bulk byte storage, use std.bytes (the mutable
                        // buffer) — `byte` is the single-octet primitive only.
    TYPE_STRING,
    TYPE_ACTOR_REF,
    TYPE_MESSAGE,
    TYPE_ARRAY,
    TYPE_STRUCT,
    TYPE_VOID,
    TYPE_PTR,           // void* for C interop
    TYPE_WILDCARD,
    TYPE_TUPLE,         // (T1, T2, ...) for multiple return values
    TYPE_FUNCTION,      // |param_types| -> return_type (closures)
    TYPE_UNKNOWN
} TypeKind;

typedef struct Type {
    TypeKind kind;
    struct Type* element_type; // For arrays and actor refs
    int array_size; // For fixed-size arrays
    char* struct_name; // For struct types
    // Tuple support (multiple return values)
    struct Type** tuple_types;  // Array of element types (NULL if not tuple)
    int tuple_count;            // Number of tuple elements (0 if not tuple)
    // Function/closure type support
    struct Type** param_types;  // Parameter types (NULL if not function type)
    int param_count;            // Number of parameters (0 if not function type)
    struct Type* return_type;   // Return type (NULL if not function type)
} Type;

typedef struct ASTNode {
    ASTNodeType type;
    char* value;                // For literals, identifiers, etc.
    Type* node_type;           // Type information for this node
    struct ASTNode** children;  // Array of child nodes
    int child_count;
    int line;
    int column;
    char* annotation;          // Optional metadata (e.g., defer factory name)
    int is_imported;           // 1 if cloned in from another module by
                               // module_merge_into_program; codegen emits
                               // such functions as `static` so each TU gets
                               // a private copy and the linker doesn't see
                               // them as duplicate symbols.
    char* source_file;         // Originating .ae path (set by ast_stamp_source_file
                               // after parse). Codegen uses this to emit `#line N
                               // "path"` directives so gcc/gdb/gcov see .ae line
                               // numbers, not the merged-.c position. NULL for
                               // synthetic nodes the parser/typechecker invent
                               // out of thin air; codegen falls back to the last
                               // known file in that case.
} ASTNode;

// Type functions
Type* create_type(TypeKind kind);
Type* create_array_type(Type* element_type, int size);
Type* create_actor_ref_type(Type* actor_type);
Type* create_tuple_type(int count, ...);  // create_tuple_type(2, type_a, type_b)
Type* create_function_type(int param_count, Type** param_types, Type* return_type);
void free_type(Type* type);
const char* type_to_string(Type* type);
int types_equal(Type* a, Type* b);
Type* clone_type(Type* type);

/* True when `t` is a typed pointer to the cons-cell `StringSeq`
 * runtime struct (see std/collections/aether_stringseq.h) — i.e.
 * Aether-side `*StringSeq`. Used by typechecker + codegen to
 * dispatch on cons-cell-typed match expressions, literal targets,
 * and field types. Centralised here so the struct-name literal
 * lives in exactly one place. */
int is_string_seq_ptr_type(const Type* t);

/* Build a fresh `*StringSeq` Type. Caller owns and must `free_type`
 * it. */
Type* make_string_seq_ptr_type(void);

// AST Node functions
ASTNode* create_ast_node(ASTNodeType type, const char* value, int line, int column);
void add_child(ASTNode* parent, ASTNode* child);
void free_ast_node(ASTNode* node);
ASTNode* clone_ast_node(ASTNode* node);
void print_ast(ASTNode* node, int indent);
const char* ast_node_type_to_string(ASTNodeType type);

// Recursively stamp `source_file` on every node in the subtree that
// doesn't already have one. Idempotent — nodes cloned from imported
// modules already carry their original file, so re-stamping the merged
// program leaves them alone. Called once per file right after
// parse_program returns. Codegen reads node->source_file to emit
// `#line N "path"` directives so gcc/gdb/gcov see .ae line numbers.
void ast_stamp_source_file(ASTNode* node, const char* path);

// Utility functions
ASTNode* create_literal_node(Token* token);
ASTNode* create_identifier_node(Token* token);
ASTNode* create_binary_expression(ASTNode* left, ASTNode* right, Token* operator);
ASTNode* create_unary_expression(ASTNode* operand, Token* operator);

#endif
