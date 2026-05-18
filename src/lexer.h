#pragma once

typedef enum {
  TOK_NUM,
  TOK_VAR,
  TOK_PLUS,
  TOK_MINUS,
  TOK_STAR,
  TOK_SLASH,
  TOK_CARET,
  TOK_LPAREN,
  TOK_RPAREN,
  TOK_SQRT,
  TOK_EOF,
} TokenType;

typedef struct {
  TokenType type;
  char value[64];
} Token;

typedef struct {
  Token *tokens;
  int count;
  int capacity;
} TokenList;

TokenList *lex(const char *input);
void token_list_free(TokenList *tl);
