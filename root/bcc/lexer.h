#ifndef LEXER_H
#define LEXER_H

// Purpose: Lex a preprocessed source buffer into a TokenArray.
// Inputs: prog is a NUL-terminated source buffer.
// Outputs: Returns a TokenArray on success or NULL on error.
// Invariants/Assumptions: The buffer backing token slices must remain valid.
struct TokenArray* lex(char* prog);

#endif // LEXER_H
