#pragma once

typedef enum {
  NODE_NUM,
  NODE_VAR,
  NODE_ADD,
  NODE_SUB,
  NODE_MUL,
  NODE_DIV,
  NODE_POW,
  NODE_SQRT,
  NODE_NEG,
} NodeType;

typedef struct ASTNode {
  NodeType type;
  char value[64];
  struct ASTNode *left;
  struct ASTNode *right;
} ASTNode;

ASTNode *ast_new_leaf(NodeType type, const char *value);
ASTNode *ast_new_node(NodeType type, ASTNode *left, ASTNode *right);
void ast_free(ASTNode *node);
