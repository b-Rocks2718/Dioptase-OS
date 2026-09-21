#ifndef LEXER_H
#define LEXER_H

// Lex a preprocessed source buffer into a TokenArray.
// Prog is a NUL-terminated source buffer.
// Returns a TokenArray on success or NULL on error.
// The buffer backing token slices must remain valid.
struct TokenArray* lex(char* prog);

#endif // LEXER_H
