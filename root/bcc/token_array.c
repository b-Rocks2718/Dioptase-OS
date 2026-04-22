#include "../crt/stdlib.h"
#include "../crt/stdio.h"
#include "../crt/print.h"
#include "../crt/string.h"

#include "token_array.h"

static struct Token* grow_token_array(struct Token* tokens, size_t old_capacity) {
  size_t new_capacity = old_capacity * 2;
  struct Token* grown = malloc(sizeof(struct Token) * new_capacity);

  if (grown == NULL) {
    return NULL;
  }

  memcpy(grown, tokens, sizeof(struct Token) * old_capacity);
  free(tokens);
  return grown;
}

struct TokenArray* create_token_array(size_t capacity){
  struct Token* tokens = malloc(sizeof(struct Token) * capacity);

  struct TokenArray* arr = malloc(sizeof(struct TokenArray));

  arr->capacity = capacity;
  arr->size = 0;
  arr->tokens = tokens;

  return arr;
}

void token_array_append(struct TokenArray* arr, struct Token* value){
  if (arr->size == arr->capacity){
    struct Token* grown = grow_token_array(arr->tokens, arr->capacity);
    if (grown == NULL) {
      puts("Token array allocation failed\n");
      exit(1);
    }
    arr->tokens = grown;
    arr->capacity = arr->capacity * 2;
  } 
    
  arr->tokens[arr->size] = *value;
  arr->size++;

  free(value);
}

struct Token token_array_get(struct TokenArray* arr, size_t i){
  // no checks on i, might regret this later
  return arr->tokens[i];
}

void destroy_token_array(struct TokenArray* arr){
  for (int i = 0; i < arr->size; ++i){
    if (arr->tokens[i].type == IDENT) free(arr->tokens[i].data.ident_name);
    else if (arr->tokens[i].type == STRING_LIT) free(arr->tokens[i].data.string_val);
  }
  free(arr->tokens);
  free(arr);
}
