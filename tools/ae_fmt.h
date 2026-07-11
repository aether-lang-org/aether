#ifndef AE_FMT_H
#define AE_FMT_H

#include <stddef.h>

// Format Aether source text.
//
// Returns a newly malloc'd, NUL-terminated formatted string (the caller owns
// it), or NULL on a hard error (unterminated string / comment), in which case
// *err (if non-NULL) is set to a static description.
//
// The transformation is whitespace-only and comment-preserving: the sequence
// of significant tokens is never reordered, dropped, or merged, and the
// contents of string literals and comments are copied verbatim. Only the
// whitespace *between* tokens (indentation, inter-token spacing, blank lines)
// is normalized. Because the lexer already separates `a-b` into the same three
// tokens as `a - b`, re-spacing between already-separated tokens cannot change
// how the source parses; the formatter therefore preserves program semantics
// by construction (it only ever inserts a separator where two tokens would
// otherwise fuse).
char* ae_format_source(const char* src, const char** err);

// Convenience: format `src` and report whether the result differs from the
// input. Returns the formatted string (caller frees) or NULL on error;
// *changed is set to 1 if formatting altered the text, 0 if already formatted.
char* ae_format_source_changed(const char* src, int* changed, const char** err);

#endif
