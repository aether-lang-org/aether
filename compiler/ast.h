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
    AST_STRUCT_FIELD_UNION,    // Compound field inside `extern struct`:
                               //   field_name: union { sub_fields... }
                               // `value` holds the field name; children are
                               // AST_STRUCT_FIELD / AST_STRUCT_FIELD_UNION /
                               // AST_STRUCT_FIELD_NESTED nodes (the union
                               // members).
    AST_STRUCT_FIELD_NESTED,   // Compound field — a nested struct inside an
                               // extern struct (typically appears inside a
                               // union). Same shape as AST_STRUCT_FIELD_UNION
                               // but emits a `struct { ... }` instead of a
                               // `union { ... }` body.
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
    AST_WHEN_STATEMENT,        // compile-time `when` / static-if (issue #483)
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
    // `expr!` — unwrap-or-trap on a (value, err) tuple: yields the first
    // slot, panics if the trailing error slot is non-empty. Single child:
    // the tuple-returning operand.
    AST_TUPLE_UNWRAP,
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
    AST_PTR_AS_ARRAY_CAST,  // `expr as T[]` — view a raw ptr as a typed
                            // C array (element_type[]). children[0] = expr
                            // (must be ptr-typed); node_type carries
                            // TYPE_ARRAY with element_type populated and
                            // array_size = -1. Codegen emits
                            // `((T*)(expr))`; AST_ARRAY_ACCESS on the
                            // result then emits `((T*)(expr))[i]` which
                            // C scales by `sizeof(T)`. No bounds check —
                            // matches `as *StructName`'s
                            // trust-the-author posture (the same systems-
                            // programming escape hatch). Used by ports
                            // of C code that need to index a malloc'd
                            // typed buffer without the `mem.get_int(p,
                            // 4*i)` boilerplate.
    AST_PTR_AS_FN_CAST,     // `expr as fn(T1, T2, ...) -> R` — view a
                            // raw ptr as a typed C function pointer.
                            // children[0] = expr (must be ptr-typed);
                            // node_type carries the TYPE_FUNCTION with
                            // signature populated.  Codegen at the
                            // value-use site (call-expression) emits
                            // the matching C function-pointer cast
                            // before invocation.  Storage of the
                            // resulting value stays `void*`; the
                            // signature is only consulted to synthesise
                            // the cast at call sites and to typecheck
                            // arity/types of the call's arguments.
    AST_IF_EXPRESSION,      // if cond { expr } else { expr } — value-producing

    // Compile-time layout builtins over extern/struct types. Both
    // yield an int. `value` holds the struct type name; for OFFSETOF
    // children[0] is an AST_IDENTIFIER naming the field. Codegen emits
    // C's sizeof(T) / offsetof(T, field) so the value always matches
    // the real C struct layout (no hand-maintained offset constants).
    AST_SIZEOF,             // sizeof(TypeName)
    AST_OFFSETOF,           // offsetof(TypeName, fieldName)
    AST_PURITY_QUERY,       // __pure(funcName) — folds to a compile-time bool (#522)

    // heap.new(T) — zero-initialised heap allocation of a POD struct,
    // returning `*T` (issue #564). `value` holds the struct type name;
    // node_type is TYPE_PTR with element_type = TYPE_STRUCT{name}.
    // POD-only: the typechecker rejects any struct with a `string` or
    // other heap-managed field (those need an ownership model first).
    // Codegen emits `((T*)calloc(1, sizeof(T)))`. Freed with the
    // ordinary call `heap.free(p)`, codegen-lowered to `free(p)`.
    AST_HEAP_NEW,           // heap.new(TypeName)

    // C variadic-consumer intrinsics. Let an Aether function declared
    // with a trailing `...` param read its varargs the way C does.
    //   AST_VA_START — `va_start()`; yields an opaque ptr (va_list
    //     cookie). The variadic function's prologue emits the hidden
    //     `va_list` decl + `va_start(..., <last named param>)`; this
    //     node just yields its address. No children.
    //   AST_VA_ARG   — `va_arg(vap, TYPE)`; children[0] = the cookie
    //     expr; node_type = the requested type. Emits
    //     `va_arg(*(va_list*)vap, <ctype>)`.
    //   AST_VA_END   — `va_end(vap)`; children[0] = the cookie expr.
    //     Emits `va_end(*(va_list*)vap)`. Yields void.
    AST_VA_START,
    AST_VA_ARG,
    AST_VA_END,

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
    AST_PRINT_STATEMENT,

    // #480 distinct types. Appended at the END of the enum on purpose:
    // inserting mid-enum shifts later values and (with incremental builds)
    // leaves stale .o compiled against the old numbering.
    AST_DISTINCT_TYPE_DEF,  // `type Name = distinct Base` — `value` is Name,
                            // `node_type` is the base Type. Zero-cost: emits no
                            // C; the typechecker registers Name as a nominally
                            // distinct type over Base.
    AST_VALUE_CAST          // `expr as T` for a scalar / distinct target — a
                            // zero-cost nominal (un)wrap or numeric conversion.
                            // children[0] = operand; node_type = target type.
} ASTNodeType;

typedef enum {
    TYPE_INT,
    TYPE_INT64,
    TYPE_UINT64,
    TYPE_UINT32,        // unsigned 32-bit — underlying kind for the
    TYPE_UINT16,        // uint32_t / uint16_t / uint8_t C ABI aliases.
    TYPE_UINT8,         // No bare keyword; reached only via c_abi_alias.
    TYPE_DURATION,      // signed 64-bit nanosecond count
    TYPE_FLOAT,
    TYPE_LONGDOUBLE,    // C `long double` — widest numeric (#749). Reached
                        // via the `longdouble` type name; no source literal.
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
    // C ABI scalar alias (size_t, uint32_t, intptr_t, time_t, ...).
    // When non-NULL, codegen emits this exact C spelling instead of
    // the `kind`'s default. `kind` still governs all typechecking,
    // arithmetic, and promotion — the alias is purely the emitted
    // spelling, so a C extern prototype matches the system header.
    // NULL for every non-alias type. See redis-porting-language-gaps.md
    // "P0: C ABI Scalar Aliases".
    char* c_alias;
    // #480 distinct types: when non-NULL, this is a nominally-distinct type
    // named `distinct_name` whose machine representation is `kind` (zero cost —
    // codegen emits the base C type). Two types with different distinct names,
    // or a distinct type vs its base, are NOT compatible without an explicit
    // `as` cast, even when `kind` matches. NULL for every non-distinct type.
    char* distinct_name;
    // Tuple support (multiple return values)
    struct Type** tuple_types;  // Array of element types (NULL if not tuple)
    int tuple_count;            // Number of tuple elements (0 if not tuple)
    // Per-element heap-ownership tags for the tuple-destructure
    // heap-tracker emit (issue #420). Parallel array, length ==
    // tuple_count. Element value: 1 = the source-position is a
    // fresh heap allocation the destructured LHS now owns
    // (caller must `free` to avoid leak); 0 = borrow / non-heap /
    // unknown. Default 0 so unannotated tuple-returning externs
    // preserve the pre-#420 silent behaviour. Populated by the
    // parser's `@heap` / `@borrow` annotation handler at the
    // tuple-element position; consumed by the
    // `AST_TUPLE_DESTRUCTURE` codegen path. NULL when tuple_count
    // is 0 OR no annotation was supplied.
    int* tuple_heap_flags;
    // Function/closure type support
    struct Type** param_types;  // Parameter types (NULL if not function type)
    int param_count;            // Number of parameters (0 if not function type)
    struct Type* return_type;   // Return type (NULL if not function type)
    // 1 = this TYPE_FUNCTION represents a raw C function pointer
    // (storage = void*, call site emits typed cast).  Default 0 =
    // an _AeClosure-shaped Aether closure value (storage = _AeClosure
    // struct with .fn + .env, call site emits closure dispatch).
    // Set on cast results from `expr as fn(T1, T2, ...) -> R` and on
    // any local/param annotated with `: fn(T1, T2, ...) -> R`.
    int is_fnptr;
    // For anonymous compound types produced by `extern struct` union /
    // nested-struct fields (issue #4 — extern struct unions). Points at
    // the originating AST_STRUCT_FIELD_UNION or AST_STRUCT_FIELD_NESTED
    // node so the typechecker can resolve `expr.u.f64` by walking the
    // compound's children directly — they have no global struct name to
    // look up. NULL on all other types. Borrowed pointer (the AST owns
    // the storage), so don't free on type teardown.
    struct ASTNode* compound_node;
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
    int bit_width;             // For AST_STRUCT_FIELD nodes: bit-width
                               // annotation `name: type : NN`. 0 = plain
                               // field (no bitfield). >0 = emitted as
                               // `type name : NN;` so the C compiler
                               // handles bit-extract on access. Bitfields
                               // are only meaningful on extern structs
                               // (AST_STRUCT_DEFINITION with extern flag
                               // — see annotation slot below).
    char* source_file;         // Originating .ae path (set by ast_stamp_source_file
                               // after parse). Codegen uses this to emit `#line N
                               // "path"` directives so gcc/gdb/gcov see .ae line
                               // numbers, not the merged-.c position. NULL for
                               // synthetic nodes the parser/typechecker invent
                               // out of thin air; codegen falls back to the last
                               // known file in that case.
    int type_inferred;         // AST_VARIABLE_DECLARATION only: 1 when the
                               // declaration had NO explicit type annotation
                               // (Python-style `x = expr`), so its type is
                               // inferred from the initializer. Survives the
                               // pre-typecheck inference that fills node_type,
                               // unlike a TYPE_UNKNOWN sentinel. Drives the
                               // #698 silent-narrowing guard.
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
