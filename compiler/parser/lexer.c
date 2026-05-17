#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include "tokens.h"
#include "lexer.h"

#define MAX_IDENTIFIER_LENGTH 256

static const char* source;
static int source_length;
static int current_pos;
static int current_line;
static int current_column;

void lexer_init(const char* src) {
    source = src;
    source_length = strlen(src);
    current_pos = 0;
    current_line = 1;
    current_column = 1;
}

void lexer_save(LexerState* out) {
    out->source        = source;
    out->source_length = source_length;
    out->current_pos   = current_pos;
    out->current_line  = current_line;
    out->current_column = current_column;
}

void lexer_restore(const LexerState* in) {
    source        = in->source;
    source_length = in->source_length;
    current_pos   = in->current_pos;
    current_line  = in->current_line;
    current_column = in->current_column;
}

char peek() {
    if (current_pos >= source_length) return '\0';
    return source[current_pos];
}

char advance() {
    if (current_pos >= source_length) return '\0';
    char c = source[current_pos++];
    if (c == '\n') {
        current_line++;
        current_column = 1;
    } else {
        current_column++;
    }
    return c;
}

void skip_whitespace() {
    while (current_pos < source_length) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r') {
            advance();
        } else if (c == '\n') {
            advance();
        } else {
            break;
        }
    }
}

int skip_comment() {
    if (peek() == '/' && current_pos + 1 < source_length && source[current_pos + 1] == '/') {
        // Single line comment
        while (current_pos < source_length && peek() != '\n') {
            advance();
        }
        return 1;
    } else if (peek() == '/' && current_pos + 1 < source_length && source[current_pos + 1] == '*') {
        // Multi-line comment
        advance(); // skip /
        advance(); // skip *
        int found_end = 0;
        while (current_pos < source_length) {
            if (peek() == '*' && current_pos + 1 < source_length && source[current_pos + 1] == '/') {
                advance(); // skip *
                advance(); // skip /
                found_end = 1;
                break;
            }
            advance();
        }
        if (!found_end) {
            fprintf(stderr, "Error: unterminated multi-line comment at line %d\n", current_line);
        }
        return 1;
    }
    return 0;
}

Token* read_string() {
    advance(); // skip opening quote
    int capacity = MAX_IDENTIFIER_LENGTH;
    char* buffer = malloc(capacity);
    int i = 0;
    bool has_interp = false;

    while (current_pos < source_length && peek() != '"') {
        if (i >= capacity - 3) {
            capacity *= 2;
            char* new_buf = realloc(buffer, capacity);
            if (!new_buf) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); }
            buffer = new_buf;
        }
        if (peek() == '$' && current_pos + 1 < source_length && source[current_pos + 1] == '{') {
            has_interp = true;
            // Store raw ${...} content without escape processing
            buffer[i++] = advance(); // $
            buffer[i++] = advance(); // {
        } else if (peek() == '\\') {
            if (has_interp) {
                // In interpolated strings, keep escape sequences raw so parser can handle them
                buffer[i++] = advance(); // backslash
                if (current_pos < source_length) {
                    char esc = peek();
                    buffer[i++] = advance(); // escaped char (e.g. 'x', '0', 'n')
                    if (esc == 'x') {
                        // Copy up to 2 hex digits so parser sees \xNN together
                        int d = 0;
                        while (d < 2 && current_pos < source_length &&
                               isxdigit((unsigned char)peek())) {
                            if (i >= capacity - 3) { capacity *= 2; char* nb = realloc(buffer, capacity); if (!nb) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); } buffer = nb; }
                            buffer[i++] = advance();
                            d++;
                        }
                    } else if (esc >= '0' && esc <= '7') {
                        // Copy up to 2 more octal digits so parser sees \NNN together
                        int d = 0;
                        while (d < 2 && current_pos < source_length &&
                               peek() >= '0' && peek() <= '7') {
                            if (i >= capacity - 3) { capacity *= 2; char* nb = realloc(buffer, capacity); if (!nb) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); } buffer = nb; }
                            buffer[i++] = advance();
                            d++;
                        }
                    }
                }
            } else {
                advance(); // skip backslash
                char c = advance();
                switch (c) {
                    case 'n': buffer[i++] = '\n'; break;
                    case 't': buffer[i++] = '\t'; break;
                    case 'r': buffer[i++] = '\r'; break;
                    case '\\': buffer[i++] = '\\'; break;
                    case '"': buffer[i++] = '"'; break;
                    case 'x': {  // \xNN hex escape (1-2 hex digits)
                        int val = 0, digits = 0;
                        while (digits < 2 && current_pos < source_length &&
                               isxdigit((unsigned char)peek())) {
                            char h = advance();
                            val = val * 16 + (h >= 'a' ? h - 'a' + 10 :
                                              h >= 'A' ? h - 'A' + 10 : h - '0');
                            digits++;
                        }
                        if (digits == 0) { buffer[i++] = 'x'; break; }
                        buffer[i++] = (char)val;
                        break;
                    }
                    case '0': case '1': case '2': case '3':
                    case '4': case '5': case '6': case '7': {  // \NNN octal (1-3 digits)
                        int val = c - '0', digits = 1;
                        while (digits < 3 && current_pos < source_length &&
                               peek() >= '0' && peek() <= '7') {
                            val = val * 8 + (advance() - '0');
                            digits++;
                        }
                        buffer[i++] = (char)(val & 0xFF);
                        break;
                    }
                    default: buffer[i++] = c; break;
                }
            }
        } else {
            buffer[i++] = advance();
        }
    }

    if (current_pos < source_length && peek() == '"') {
        advance(); // skip closing quote
    } else {
        // Unterminated string
        free(buffer);
        return create_token(TOKEN_ERROR, "unterminated string literal", current_line, current_column);
    }

    buffer[i] = '\0';
    AeTokenType tok_type = has_interp ? TOKEN_INTERP_STRING : TOKEN_STRING_LITERAL;
    Token* token = create_token(tok_type, buffer, current_line, current_column);
    free(buffer);
    return token;
}

Token* read_number() {
    int capacity = MAX_IDENTIFIER_LENGTH;
    char* buffer = malloc(capacity);
    int i = 0;

    // Check for hex (0x), octal (0o), binary (0b) prefixes
    if (peek() == '0' && current_pos + 1 < source_length) {
        char next = source[current_pos + 1];
        if (next == 'x' || next == 'X') {
            // Hex literal: 0x[0-9a-fA-F]+
            buffer[i++] = advance(); // '0'
            buffer[i++] = advance(); // 'x'
            while (current_pos < source_length && (isxdigit(peek()) || peek() == '_')) {
                if (peek() == '_') { advance(); continue; }
                if (i >= capacity - 1) { capacity *= 2; char* nb = realloc(buffer, capacity); if (!nb) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); } buffer = nb; }
                buffer[i++] = advance();
            }
            buffer[i] = '\0';
            Token* token = create_token(TOKEN_NUMBER, buffer, current_line, current_column);
            free(buffer);
            return token;
        } else if (next == 'o' || next == 'O') {
            // Octal literal: 0o[0-7]+
            buffer[i++] = advance(); // '0'
            buffer[i++] = advance(); // 'o'
            while (current_pos < source_length && ((peek() >= '0' && peek() <= '7') || peek() == '_')) {
                if (peek() == '_') { advance(); continue; }
                if (i >= capacity - 1) { capacity *= 2; char* nb = realloc(buffer, capacity); if (!nb) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); } buffer = nb; }
                buffer[i++] = advance();
            }
            buffer[i] = '\0';
            Token* token = create_token(TOKEN_NUMBER, buffer, current_line, current_column);
            free(buffer);
            return token;
        } else if (next == 'b' || next == 'B') {
            // Binary literal: 0b[01]+
            buffer[i++] = advance(); // '0'
            buffer[i++] = advance(); // 'b'
            while (current_pos < source_length && (peek() == '0' || peek() == '1' || peek() == '_')) {
                if (peek() == '_') { advance(); continue; }
                if (i >= capacity - 1) { capacity *= 2; char* nb = realloc(buffer, capacity); if (!nb) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); } buffer = nb; }
                buffer[i++] = advance();
            }
            buffer[i] = '\0';
            Token* token = create_token(TOKEN_NUMBER, buffer, current_line, current_column);
            free(buffer);
            return token;
        }
    }

    // Decimal literal (with optional dot for floats)
    // Don't consume '.' if followed by another '.' (range operator '..')
    while (current_pos < source_length && (isdigit(peek()) || (peek() == '.' && (current_pos + 1 >= source_length || source[current_pos + 1] != '.')))) {
        if (i >= capacity - 1) {
            capacity *= 2;
            char* new_buf = realloc(buffer, capacity);
            if (!new_buf) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); }
            buffer = new_buf;
        }
        buffer[i++] = advance();
    }

    buffer[i] = '\0';
    Token* token = create_token(TOKEN_NUMBER, buffer, current_line, current_column);
    free(buffer);
    return token;
}

Token* read_identifier() {
    int capacity = MAX_IDENTIFIER_LENGTH;
    char* buffer = malloc(capacity);
    int i = 0;
    
    while (current_pos < source_length && (isalnum(peek()) || peek() == '_')) {
        if (i >= capacity - 1) {
            capacity *= 2;
            char* new_buf = realloc(buffer, capacity);
            if (!new_buf) { free(buffer); return create_token(TOKEN_ERROR, "out of memory", current_line, current_column); }
            buffer = new_buf;
        }
        buffer[i++] = advance();
    }
    
    buffer[i] = '\0';

    // Check if it's a keyword - create token then free buffer
    Token* token;
    if (strcmp(buffer, "actor") == 0) token = create_token(TOKEN_ACTOR, buffer, current_line, current_column);
    else if (strcmp(buffer, "main") == 0) token = create_token(TOKEN_MAIN, buffer, current_line, current_column);
    else if (strcmp(buffer, "func") == 0) token = create_token(TOKEN_FUNC, buffer, current_line, current_column);
    else if (strcmp(buffer, "let") == 0) token = create_token(TOKEN_LET, buffer, current_line, current_column);
    else if (strcmp(buffer, "var") == 0) token = create_token(TOKEN_VAR, buffer, current_line, current_column);
    else if (strcmp(buffer, "if") == 0) token = create_token(TOKEN_IF, buffer, current_line, current_column);
    else if (strcmp(buffer, "else") == 0) token = create_token(TOKEN_ELSE, buffer, current_line, current_column);
    else if (strcmp(buffer, "for") == 0) token = create_token(TOKEN_FOR, buffer, current_line, current_column);
    else if (strcmp(buffer, "while") == 0) token = create_token(TOKEN_WHILE, buffer, current_line, current_column);
    else if (strcmp(buffer, "switch") == 0) token = create_token(TOKEN_SWITCH, buffer, current_line, current_column);
    else if (strcmp(buffer, "case") == 0) token = create_token(TOKEN_CASE, buffer, current_line, current_column);
    else if (strcmp(buffer, "default") == 0) token = create_token(TOKEN_DEFAULT, buffer, current_line, current_column);
    else if (strcmp(buffer, "break") == 0) token = create_token(TOKEN_BREAK, buffer, current_line, current_column);
    else if (strcmp(buffer, "continue") == 0) token = create_token(TOKEN_CONTINUE, buffer, current_line, current_column);
    else if (strcmp(buffer, "return") == 0) token = create_token(TOKEN_RETURN, buffer, current_line, current_column);
    else if (strcmp(buffer, "defer") == 0) token = create_token(TOKEN_DEFER, buffer, current_line, current_column);
    else if (strcmp(buffer, "builder") == 0) token = create_token(TOKEN_BUILDER, buffer, current_line, current_column);
    else if (strcmp(buffer, "match") == 0) token = create_token(TOKEN_MATCH, buffer, current_line, current_column);
    else if (strcmp(buffer, "when") == 0) token = create_token(TOKEN_WHEN, buffer, current_line, current_column);
    else if (strcmp(buffer, "receive") == 0) token = create_token(TOKEN_RECEIVE, buffer, current_line, current_column);
    else if (strcmp(buffer, "send") == 0) token = create_token(TOKEN_SEND, buffer, current_line, current_column);
    else if (strcmp(buffer, "spawn_actor") == 0) token = create_token(TOKEN_SPAWN_ACTOR, buffer, current_line, current_column);
    else if (strcmp(buffer, "spawn") == 0) token = create_token(TOKEN_SPAWN, buffer, current_line, current_column);
    else if (strcmp(buffer, "make") == 0) token = create_token(TOKEN_MAKE, buffer, current_line, current_column);
    else if (strcmp(buffer, "self") == 0) token = create_token(TOKEN_SELF, buffer, current_line, current_column);
    else if (strcmp(buffer, "state") == 0) token = create_token(TOKEN_STATE, buffer, current_line, current_column);
    else if (strcmp(buffer, "struct") == 0) token = create_token(TOKEN_STRUCT, buffer, current_line, current_column);
    else if (strcmp(buffer, "union") == 0) token = create_token(TOKEN_UNION, buffer, current_line, current_column);
    else if (strcmp(buffer, "import") == 0) token = create_token(TOKEN_IMPORT, buffer, current_line, current_column);
    else if (strcmp(buffer, "as") == 0) token = create_token(TOKEN_AS, buffer, current_line, current_column);
    else if (strcmp(buffer, "export") == 0) token = create_token(TOKEN_EXPORT, buffer, current_line, current_column);
    else if (strcmp(buffer, "exports") == 0) token = create_token(TOKEN_EXPORTS, buffer, current_line, current_column);
    else if (strcmp(buffer, "module") == 0) token = create_token(TOKEN_MODULE, buffer, current_line, current_column);
    else if (strcmp(buffer, "message") == 0) token = create_token(TOKEN_MESSAGE_KEYWORD, buffer, current_line, current_column);
    else if (strcmp(buffer, "reply") == 0) token = create_token(TOKEN_REPLY, buffer, current_line, current_column);
    else if (strcmp(buffer, "extern") == 0) token = create_token(TOKEN_EXTERN, buffer, current_line, current_column);
    else if (strcmp(buffer, "null") == 0) token = create_token(TOKEN_NULL, buffer, current_line, current_column);
    else if (strcmp(buffer, "const") == 0) token = create_token(TOKEN_CONST, buffer, current_line, current_column);
    else if (strcmp(buffer, "in") == 0) token = create_token(TOKEN_IN, buffer, current_line, current_column);
    else if (strcmp(buffer, "after") == 0) token = create_token(TOKEN_AFTER, buffer, current_line, current_column);
    else if (strcmp(buffer, "callback") == 0) token = create_token(TOKEN_CALLBACK, buffer, current_line, current_column);
    else if (strcmp(buffer, "hide") == 0) token = create_token(TOKEN_HIDE, buffer, current_line, current_column);
    else if (strcmp(buffer, "seal") == 0) token = create_token(TOKEN_SEAL, buffer, current_line, current_column);
    else if (strcmp(buffer, "except") == 0) token = create_token(TOKEN_EXCEPT, buffer, current_line, current_column);
    else if (strcmp(buffer, "try") == 0) token = create_token(TOKEN_TRY, buffer, current_line, current_column);
    else if (strcmp(buffer, "catch") == 0) token = create_token(TOKEN_CATCH, buffer, current_line, current_column);
    else if (strcmp(buffer, "panic") == 0) token = create_token(TOKEN_PANIC, buffer, current_line, current_column);
    else if (strcmp(buffer, "requires") == 0) token = create_token(TOKEN_REQUIRES, buffer, current_line, current_column);
    else if (strcmp(buffer, "ensures") == 0) token = create_token(TOKEN_ENSURES, buffer, current_line, current_column);
    else if (strcmp(buffer, "ptr") == 0) token = create_token(TOKEN_PTR, buffer, current_line, current_column);
    else if (strcmp(buffer, "int") == 0) token = create_token(TOKEN_INT, buffer, current_line, current_column);
    else if (strcmp(buffer, "long") == 0) token = create_token(TOKEN_INT64, buffer, current_line, current_column);
    else if (strcmp(buffer, "uint64") == 0) token = create_token(TOKEN_UINT64, buffer, current_line, current_column);
    else if (strcmp(buffer, "float") == 0) token = create_token(TOKEN_FLOAT, buffer, current_line, current_column);
    else if (strcmp(buffer, "bool") == 0) token = create_token(TOKEN_BOOL, buffer, current_line, current_column);
    else if (strcmp(buffer, "byte") == 0) token = create_token(TOKEN_BYTE, buffer, current_line, current_column);
    else if (strcmp(buffer, "string") == 0) token = create_token(TOKEN_STRING, buffer, current_line, current_column);
    else if (strcmp(buffer, "ActorRef") == 0 || strcmp(buffer, "actor_ref") == 0) token = create_token(TOKEN_ACTOR_REF, buffer, current_line, current_column);
    else if (strcmp(buffer, "Message") == 0) token = create_token(TOKEN_MESSAGE, buffer, current_line, current_column);
    else if (strcmp(buffer, "true") == 0) token = create_token(TOKEN_TRUE, buffer, current_line, current_column);
    else if (strcmp(buffer, "false") == 0) token = create_token(TOKEN_FALSE, buffer, current_line, current_column);
    else if (strcmp(buffer, "print") == 0) token = create_token(TOKEN_PRINT, buffer, current_line, current_column);
    else token = create_token(TOKEN_IDENTIFIER, buffer, current_line, current_column);

    free(buffer);
    return token;
}

Token* next_token() {
    skip_whitespace();
    
    if (current_pos >= source_length) {
        return create_token(TOKEN_EOF, NULL, current_line, current_column);
    }
    
    char c = peek();
    
    // Handle comments
    if (c == '/' && skip_comment()) {
        return next_token(); // Recursively get next token after comment
    }
    
    // Handle string literals
    if (c == '"') {
        return read_string();
    }
    
    // Handle numbers
    if (isdigit(c)) {
        return read_number();
    }
    
    // Handle identifiers and keywords
    if (isalpha(c) || c == '_') {
        return read_identifier();
    }
    
    // Handle operators and delimiters
    switch (c) {
        case '+':
            advance();
            if (peek() == '+') {
                advance();
                return create_token(TOKEN_INCREMENT, "++", current_line, current_column);
            }
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_PLUS_ASSIGN, "+=", current_line, current_column);
            }
            return create_token(TOKEN_PLUS, "+", current_line, current_column);
        case '-':
            advance();
            if (peek() == '>') {
                advance();
                return create_token(TOKEN_ARROW, "->", current_line, current_column);
            }
            if (peek() == '-') {
                advance();
                return create_token(TOKEN_DECREMENT, "--", current_line, current_column);
            }
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_MINUS_ASSIGN, "-=", current_line, current_column);
            }
            return create_token(TOKEN_MINUS, "-", current_line, current_column);
        case '*':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_MULTIPLY_ASSIGN, "*=", current_line, current_column);
            }
            return create_token(TOKEN_MULTIPLY, "*", current_line, current_column);
        case '/':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_DIVIDE_ASSIGN, "/=", current_line, current_column);
            }
            return create_token(TOKEN_DIVIDE, "/", current_line, current_column);
        case '%':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_MODULO_ASSIGN, "%=", current_line, current_column);
            }
            return create_token(TOKEN_MODULO, "%", current_line, current_column);
        case '=':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_EQUALS, "==", current_line, current_column);
            }
            return create_token(TOKEN_ASSIGN, "=", current_line, current_column);
        case '!':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_NOT_EQUALS, "!=", current_line, current_column);
            }
            // In Actor V2, ! is the fire-and-forget operator
            // Also used as logical NOT - context determines usage
            return create_token(TOKEN_EXCLAIM, "!", current_line, current_column);
        case '<':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_LESS_EQUAL, "<=", current_line, current_column);
            }
            if (peek() == '<') {
                advance();
                if (peek() == '=') {
                    advance();
                    return create_token(TOKEN_LSHIFT_ASSIGN, "<<=", current_line, current_column);
                }
                // Heredoc: <<MARKER ... MARKER
                // Content is a literal string (no interpolation).
                // Preserves all whitespace and newlines exactly.
                // Use regular "..." strings for interpolation.
                if (isalpha(peek()) || peek() == '_') {
                    // Read the marker name
                    char marker[MAX_IDENTIFIER_LENGTH];
                    int mlen = 0;
                    while ((isalnum(peek()) || peek() == '_') && mlen < MAX_IDENTIFIER_LENGTH - 1) {
                        marker[mlen++] = advance();
                    }
                    marker[mlen] = '\0';

                    // Skip to end of current line (past the marker)
                    while (peek() != '\n' && peek() != '\0') advance();
                    if (peek() == '\n') advance();

                    // Collect everything until a line that is exactly the marker
                    int start_line = current_line;
                    int buf_capacity = 4096;
                    char* buf = malloc(buf_capacity);
                    if (!buf) return create_token(TOKEN_ERROR, "out of memory", current_line, current_column);
                    int blen = 0;
                    while (current_pos < source_length) {
                        // Check if current line starts with the marker
                        // Skip optional \r before checking (Windows CRLF)
                        int check_pos = current_pos;
                        int match = 1;
                        for (int mi = 0; mi < mlen; mi++) {
                            if (check_pos + mi >= source_length || source[check_pos + mi] != marker[mi]) {
                                match = 0;
                                break;
                            }
                        }
                        // Marker must be followed by newline, \r\n, or EOF
                        if (match) {
                            int after = check_pos + mlen;
                            if (after >= source_length ||
                                source[after] == '\n' ||
                                (source[after] == '\r' && after + 1 < source_length && source[after + 1] == '\n')) {
                                // Skip past the marker and line ending
                                for (int mi = 0; mi < mlen; mi++) advance();
                                if (peek() == '\r') advance();
                                if (peek() == '\n') advance();
                                break;
                            }
                        }
                        // Not the marker — copy this char to buffer
                        // Skip \r in \r\n sequences (normalize to \n)
                        char ch = peek();
                        if (ch != '\r' || (current_pos + 1 < source_length && source[current_pos + 1] != '\n')) {
                            // Grow buffer if needed
                            if (blen >= buf_capacity - 1) {
                                buf_capacity *= 2;
                                char* new_buf = realloc(buf, buf_capacity);
                                if (!new_buf) {
                                    free(buf);
                                    return create_token(TOKEN_ERROR, "heredoc too large", current_line, current_column);
                                }
                                buf = new_buf;
                            }
                            buf[blen++] = ch;
                        }
                        advance();
                    }
                    // Strip the final newline before the marker line.
                    // Content between the markers is preserved exactly,
                    // but the newline immediately before the closing
                    // marker is an artifact of the syntax, not content.
                    if (blen > 0 && buf[blen-1] == '\n') blen--;
                    buf[blen] = '\0';

                    Token* tok = create_token(TOKEN_STRING_LITERAL, buf, start_line, current_column);
                    free(buf);
                    return tok;
                }
                return create_token(TOKEN_LSHIFT, "<<", current_line, current_column);
            }
            return create_token(TOKEN_LESS, "<", current_line, current_column);
        case '>':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_GREATER_EQUAL, ">=", current_line, current_column);
            }
            if (peek() == '>') {
                advance();
                if (peek() == '=') {
                    advance();
                    return create_token(TOKEN_RSHIFT_ASSIGN, ">>=", current_line, current_column);
                }
                return create_token(TOKEN_RSHIFT, ">>", current_line, current_column);
            }
            return create_token(TOKEN_GREATER, ">", current_line, current_column);
        case '&':
            advance();
            if (peek() == '&') {
                advance();
                return create_token(TOKEN_AND, "&&", current_line, current_column);
            }
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_AND_ASSIGN, "&=", current_line, current_column);
            }
            return create_token(TOKEN_AMPERSAND, "&", current_line, current_column);
        case '|':
            advance();
            if (peek() == '|') {
                advance();
                return create_token(TOKEN_OR, "||", current_line, current_column);
            }
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_OR_ASSIGN, "|=", current_line, current_column);
            }
            return create_token(TOKEN_PIPE, "|", current_line, current_column);
        case '^':
            advance();
            if (peek() == '=') {
                advance();
                return create_token(TOKEN_XOR_ASSIGN, "^=", current_line, current_column);
            }
            return create_token(TOKEN_CARET, "^", current_line, current_column);
        case '~': advance(); return create_token(TOKEN_TILDE, "~", current_line, current_column);
        case '(': advance(); return create_token(TOKEN_LEFT_PAREN, "(", current_line, current_column);
        case ')': advance(); return create_token(TOKEN_RIGHT_PAREN, ")", current_line, current_column);
        case '{': advance(); return create_token(TOKEN_LEFT_BRACE, "{", current_line, current_column);
        case '}': advance(); return create_token(TOKEN_RIGHT_BRACE, "}", current_line, current_column);
        case '[': advance(); return create_token(TOKEN_LEFT_BRACKET, "[", current_line, current_column);
        case ']': advance(); return create_token(TOKEN_RIGHT_BRACKET, "]", current_line, current_column);
        case ';': advance(); return create_token(TOKEN_SEMICOLON, ";", current_line, current_column);
        case ',': advance(); return create_token(TOKEN_COMMA, ",", current_line, current_column);
        case '.':
            advance();
            if (peek() == '.') {
                advance();
                if (peek() == '.') {
                    advance();
                    return create_token(TOKEN_DOTDOTDOT, "...", current_line, current_column);
                }
                return create_token(TOKEN_DOTDOT, "..", current_line, current_column);
            }
            return create_token(TOKEN_DOT, ".", current_line, current_column);
        case ':': advance(); return create_token(TOKEN_COLON, ":", current_line, current_column);
        case '?': advance(); return create_token(TOKEN_QUESTION, "?", current_line, current_column);
        case '@': advance(); return create_token(TOKEN_AT, "@", current_line, current_column);
        default: {
            advance();
            char err_ch[2] = { c, '\0' };
            return create_token(TOKEN_ERROR, err_ch, current_line, current_column);
        }
    }
}

Token* create_token(AeTokenType type, const char* value, int line, int column) {
    Token* token = malloc(sizeof(Token));
    if (!token) return NULL;
    token->type = type;
    token->line = line;
    token->column = column;
    if (value) {
        size_t len = strlen(value);
        token->value = malloc(len + 1);
        if (!token->value) { free(token); return NULL; }
        memcpy(token->value, value, len + 1);
    } else {
        token->value = NULL;
    }
    return token;
}

void free_token(Token* token) {
    if (token) {
        if (token->value) {
            free(token->value);
        }
        free(token);
    }
}

const char* token_type_to_string(AeTokenType type) {
    switch (type) {
        case TOKEN_ACTOR: return "ACTOR";
        case TOKEN_MAIN: return "MAIN";
        case TOKEN_FUNC: return "FUNC";
        case TOKEN_LET: return "LET";
        case TOKEN_VAR: return "VAR";
        case TOKEN_IF: return "IF";
        case TOKEN_ELSE: return "ELSE";
        case TOKEN_FOR: return "FOR";
        case TOKEN_WHILE: return "WHILE";
        case TOKEN_SWITCH: return "SWITCH";
        case TOKEN_CASE: return "CASE";
        case TOKEN_DEFAULT: return "DEFAULT";
        case TOKEN_BREAK: return "BREAK";
        case TOKEN_CONTINUE: return "CONTINUE";
        case TOKEN_RETURN: return "RETURN";
        case TOKEN_BUILDER: return "BUILDER";
        case TOKEN_MATCH: return "MATCH";
        case TOKEN_WHEN: return "WHEN";
        case TOKEN_RECEIVE: return "RECEIVE";
        case TOKEN_SEND: return "SEND";
        case TOKEN_SPAWN_ACTOR: return "SPAWN_ACTOR";
        case TOKEN_SPAWN: return "SPAWN";
        case TOKEN_SELF: return "SELF";
        case TOKEN_STATE: return "STATE";
        case TOKEN_STRUCT: return "STRUCT";
        case TOKEN_UNION: return "UNION";
        case TOKEN_IMPORT: return "IMPORT";
        case TOKEN_AS: return "AS";
        case TOKEN_EXPORT: return "EXPORT";
        case TOKEN_EXPORTS: return "EXPORTS";
        case TOKEN_MODULE: return "MODULE";
        case TOKEN_MESSAGE_KEYWORD: return "MESSAGE_KEYWORD";
        case TOKEN_REPLY: return "REPLY";
        case TOKEN_INT: return "INT";
        case TOKEN_FLOAT: return "FLOAT";
        case TOKEN_BOOL: return "BOOL";
        case TOKEN_BYTE: return "BYTE";
        case TOKEN_STRING: return "STRING";
        case TOKEN_ACTOR_REF: return "ACTOR_REF";
        case TOKEN_MESSAGE: return "MESSAGE";
        case TOKEN_IDENTIFIER: return "IDENTIFIER";
        case TOKEN_NUMBER: return "NUMBER";
        case TOKEN_STRING_LITERAL: return "STRING_LITERAL";
        case TOKEN_TRUE: return "TRUE";
        case TOKEN_FALSE: return "FALSE";
        case TOKEN_PLUS: return "PLUS";
        case TOKEN_MINUS: return "MINUS";
        case TOKEN_MULTIPLY: return "MULTIPLY";
        case TOKEN_DIVIDE: return "DIVIDE";
        case TOKEN_MODULO: return "MODULO";
        case TOKEN_ASSIGN: return "ASSIGN";
        case TOKEN_EQUALS: return "EQUALS";
        case TOKEN_NOT_EQUALS: return "NOT_EQUALS";
        case TOKEN_LESS: return "LESS";
        case TOKEN_LESS_EQUAL: return "LESS_EQUAL";
        case TOKEN_GREATER: return "GREATER";
        case TOKEN_GREATER_EQUAL: return "GREATER_EQUAL";
        case TOKEN_AND: return "AND";
        case TOKEN_OR: return "OR";
        case TOKEN_NOT: return "NOT";
        case TOKEN_INCREMENT: return "INCREMENT";
        case TOKEN_DECREMENT: return "DECREMENT";
        case TOKEN_AMPERSAND: return "AMPERSAND";
        case TOKEN_CARET: return "CARET";
        case TOKEN_TILDE: return "TILDE";
        case TOKEN_LSHIFT: return "LSHIFT";
        case TOKEN_RSHIFT: return "RSHIFT";
        case TOKEN_PLUS_ASSIGN: return "PLUS_ASSIGN";
        case TOKEN_MINUS_ASSIGN: return "MINUS_ASSIGN";
        case TOKEN_MULTIPLY_ASSIGN: return "MULTIPLY_ASSIGN";
        case TOKEN_DIVIDE_ASSIGN: return "DIVIDE_ASSIGN";
        case TOKEN_MODULO_ASSIGN: return "MODULO_ASSIGN";
        case TOKEN_AND_ASSIGN: return "AND_ASSIGN";
        case TOKEN_OR_ASSIGN: return "OR_ASSIGN";
        case TOKEN_XOR_ASSIGN: return "XOR_ASSIGN";
        case TOKEN_LSHIFT_ASSIGN: return "LSHIFT_ASSIGN";
        case TOKEN_RSHIFT_ASSIGN: return "RSHIFT_ASSIGN";
        case TOKEN_NULL: return "NULL";
        case TOKEN_CALLBACK: return "CALLBACK";
        case TOKEN_IN: return "IN";
        case TOKEN_DOTDOT: return "DOTDOT";
        case TOKEN_DOTDOTDOT: return "DOTDOTDOT";
        case TOKEN_CONST: return "CONST";
        case TOKEN_TRY: return "TRY";
        case TOKEN_CATCH: return "CATCH";
        case TOKEN_PANIC: return "PANIC";
        case TOKEN_REQUIRES: return "REQUIRES";
        case TOKEN_ENSURES: return "ENSURES";
        case TOKEN_LEFT_PAREN: return "LEFT_PAREN";
        case TOKEN_RIGHT_PAREN: return "RIGHT_PAREN";
        case TOKEN_LEFT_BRACE: return "LEFT_BRACE";
        case TOKEN_RIGHT_BRACE: return "RIGHT_BRACE";
        case TOKEN_LEFT_BRACKET: return "LEFT_BRACKET";
        case TOKEN_RIGHT_BRACKET: return "RIGHT_BRACKET";
        case TOKEN_SEMICOLON: return "SEMICOLON";
        case TOKEN_COMMA: return "COMMA";
        case TOKEN_DOT: return "DOT";
        case TOKEN_COLON: return "COLON";
        case TOKEN_ARROW: return "ARROW";
        case TOKEN_PIPE: return "PIPE";
        case TOKEN_AT: return "AT";
        case TOKEN_EXCLAIM: return "EXCLAIM";
        case TOKEN_QUESTION: return "QUESTION";
        case TOKEN_PRINT: return "PRINT";
        case TOKEN_EOF: return "EOF";
        case TOKEN_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
