#ifndef PARSER_H
#define PARSER_H

#include <stdbool.h>
#include "tokens.h"
#include "../ast.h"

typedef struct {
    Token** tokens;
    int token_count;
    int current_token;
    int suppress_errors;  // Flag to suppress error messages (for testing)
    int parsing_builder;  // Flag: inside builder function definition (enables 'with' clause)
    int in_condition;     // Flag: inside if/while/for condition — suppresses
                          // trailing-block parsing on function calls so the
                          // `{` belongs to the if/while body, not a trailing
                          // closure on the last call in the condition.
    int when_top_level;   // Flag: the `when` static-if currently being parsed
                          // sits at top level, so its arm items are parsed as
                          // declarations (parse_top_level_decl) rather than
                          // statements. Saved/restored around nested `when`s.
} Parser;

// Parser functions
Parser* create_parser(Token** tokens, int token_count);
void free_parser(Parser* parser);
ASTNode* parse_program(Parser* parser);
ASTNode* parse_module_declaration(Parser* parser);
ASTNode* parse_import_statement(Parser* parser);
ASTNode* parse_export_statement(Parser* parser);
ASTNode* parse_exports_list(Parser* parser);
ASTNode* parse_actor_definition(Parser* parser);
ASTNode* parse_function_definition(Parser* parser);
ASTNode* parse_extern_declaration(Parser* parser);
ASTNode* parse_extern_struct_field(Parser* parser);
ASTNode* parse_pattern(Parser* parser);
ASTNode* parse_struct_pattern(Parser* parser);
ASTNode* parse_list_pattern(Parser* parser);
ASTNode* parse_main_function(Parser* parser);
ASTNode* parse_struct_definition(Parser* parser);
ASTNode* parse_statement(Parser* parser);
ASTNode* parse_expression(Parser* parser);
ASTNode* parse_primary_expression(Parser* parser);
Type* parse_type(Parser* parser);

// Additional parsing functions
ASTNode* parse_binary_expression(Parser* parser, int precedence);
ASTNode* parse_unary_expression(Parser* parser);
ASTNode* parse_variable_declaration(Parser* parser);
ASTNode* parse_variable_declaration_with_semicolon(Parser* parser, bool expect_semicolon);
ASTNode* parse_python_style_declaration(Parser* parser);
ASTNode* parse_if_statement(Parser* parser);
ASTNode* parse_when_statement(Parser* parser);   // compile-time `when` / static-if (#483)
ASTNode* parse_top_level_decl(Parser* parser);   // one top-level declaration (used by parse_program and top-level `when`)
ASTNode* parse_for_loop(Parser* parser);
ASTNode* parse_while_loop(Parser* parser);
ASTNode* parse_switch_statement(Parser* parser);
ASTNode* parse_case_statement(Parser* parser);
ASTNode* parse_match_statement(Parser* parser);
ASTNode* parse_match_case(Parser* parser);
ASTNode* parse_return_statement(Parser* parser);
ASTNode* parse_print_statement(Parser* parser);
ASTNode* parse_send_statement(Parser* parser);
ASTNode* parse_spawn_actor_statement(Parser* parser);
ASTNode* parse_block(Parser* parser);
ASTNode* parse_receive_statement(Parser* parser);
ASTNode* parse_defer_statement(Parser* parser);
ASTNode* parse_try_statement(Parser* parser);
ASTNode* parse_panic_statement(Parser* parser);

// Actor V2 parsing functions
ASTNode* parse_message_definition(Parser* parser);
ASTNode* parse_message_pattern(Parser* parser);
ASTNode* parse_reply_statement(Parser* parser);
ASTNode* parse_message_constructor(Parser* parser);

// Closure parsing
ASTNode* parse_closure_expression(Parser* parser);

// Utility functions
Token* peek_token(Parser* parser);
Token* peek_ahead(Parser* parser, int offset);
Token* advance_token(Parser* parser);
Token* expect_token(Parser* parser, AeTokenType expected);
int is_at_end(Parser* parser);
int match_token(Parser* parser, AeTokenType type);
void parser_error(Parser* parser, const char* message);
int get_operator_precedence(AeTokenType type);

#endif
