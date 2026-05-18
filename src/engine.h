/*
 * engine.c
 * ASCII Math Rendering Engine
 *
 * Implements: box.h, compose.h, ast.h, lexer.h, render.h
 *
 * Build order of concepts in this file:
 *   1. Box primitives     (box_new, box_free, box_clear, box_blit, box_print)
 *   2. Composition        (box_hstack, box_fraction, box_power, box_sqrt,
 * box_parens)
 *   3. AST nodes          (ast_new_leaf, ast_new_node, ast_free)
 *   4. Lexer              (lex, token_list_free)
 *   5. Parser             (internal — parse_expr, parse_term, etc.)
 *   6. Renderer           (render)
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "box.h"
#include "compose.h"
#include "lexer.h"
#include "render.h"

/* =========================================================================
 * SECTION 1 — BOX PRIMITIVES
 *
 * A Box is a 2D grid of characters with a "baseline" row — the row that
 * sits on the math line when composing side by side.
 *
 * Memory layout:
 *
 *   Box
 *   ├── lines  ──► char* [0]  ──► "x + 1   \0"
 *   │            char* [1]  ──► "---------\0"
 *   │            char* [2]  ──► "  y      \0"
 *   ├── width    = 9
 *   ├── height   = 3
 *   └── baseline = 1   ← the bar row is the math line
 *
 * =========================================================================*/

/*
 * box_new — allocate a blank Box of given dimensions.
 *
 * Every cell starts as '\0' (calloc zeroes memory).
 * Call box_clear() after if you want spaces instead of nulls.
 *
 * baseline: which row index is the "math line".
 *   - For a plain symbol "x", baseline = 0 (only row).
 *   - For a fraction, baseline = the bar row.
 *   - For a power x^2, baseline = the row where x sits.
 */
Box *box_new(int width, int height, int baseline) {
  Box *b = malloc(sizeof(Box));
  if (!b) {
    perror("box_new: malloc Box");
    exit(1);
  }

  /* Allocate the array of row-pointers first.
   * This is NOT the strings yet — just an array of addresses. */
  b->lines = malloc(height * sizeof(char *));
  if (!b->lines) {
    perror("box_new: malloc lines");
    exit(1);
  }

  /* Now allocate each individual row string.
   * width+1 for the null terminator.
   * calloc zeroes everything so every row starts as an empty string. */
  for (int i = 0; i < height; i++) {
    b->lines[i] = calloc(width + 1, sizeof(char));
    if (!b->lines[i]) {
      perror("box_new: calloc row");
      exit(1);
    }
  }

  b->width = width;
  b->height = height;
  b->baseline = baseline;
  return b;
}

/*
 * box_free — free in reverse allocation order.
 *
 * MUST free inner strings before freeing the pointer array,
 * and the pointer array before freeing the struct.
 * Freeing the struct first would lose the pointer to b->lines
 * and leak all the row strings.
 */
void box_free(Box *b) {
  if (!b)
    return;
  for (int i = 0; i < b->height; i++)
    free(b->lines[i]);
  free(b->lines);
  free(b);
}

/*
 * box_clear — fill every cell with a space character.
 *
 * After box_new, cells are '\0'. We want ' ' so that when we print
 * rows we get proper spacing between blitted regions.
 *
 * We use memset per row rather than a 2D memset because the rows
 * are separately allocated — they are NOT contiguous in memory.
 */
void box_clear(Box *b) {
  for (int i = 0; i < b->height; i++)
    memset(b->lines[i], ' ', b->width);
  /* Keep the null terminator at width */
}

/*
 * box_blit — copy src into dst at position (row_off, col_off).
 *
 * "Blitting" means writing one grid into a region of another.
 * We copy row by row using memcpy.
 *
 * dst->lines[row_off + i] + col_off
 *   └── pointer arithmetic: shift col_off bytes into the row string,
 *       so memcpy writes into the middle of the destination row
 *       without touching what's to the left.
 *
 * We do NOT copy the null terminator (we copy exactly src->width bytes).
 * The dst row's own null terminator stays at dst->width.
 */
void box_blit(Box *dst, Box *src, int row_off, int col_off) {
  for (int i = 0; i < src->height; i++)
    memcpy(dst->lines[row_off + i] + col_off, src->lines[i], src->width);
}

/*
 * box_print — print the box to stdout, trimming trailing spaces.
 *
 * We walk backwards from width to find the last non-space character,
 * then printf only up to that point. This keeps output clean.
 */
void box_print(Box *b) {
  for (int i = 0; i < b->height; i++) {
    int end = b->width;
    while (end > 0 && b->lines[i][end - 1] == ' ')
      end--;
    printf("%.*s\n", end, b->lines[i]);
  }
}
Box *box_from_str(const char *s) {
  int w = (int)strlen(s);
  Box *b = box_new(w, 1, 0);
  memcpy(b->lines[0], s, w);
  return b;
}

/* =========================================================================
 * SECTION 2 — COMPOSITION FUNCTIONS
 *
 * These are the heart of the engine. Each function takes one or two
 * Box pointers and returns a NEW Box that represents the composed result.
 *
 * The caller is responsible for freeing both the inputs and the output.
 * (In render.c we free intermediates after each composition step.)
 *
 * =========================================================================*/

/*
 * box_hstack — place two boxes side by side, aligned on their baselines.
 *
 * Example: left = "x^2", right = "y", both baseline=0
 *
 *   2          2
 *  x    +   y      →   x  + y
 *
 * The one with the higher baseline needs to be shifted DOWN so both
 * baselines land on the same output row.
 *
 * above = rows above the baseline (including baseline itself? No —
 *         baseline is row index, so rows above = baseline count)
 * below = rows below the baseline
 *
 * Total height = above + 1 + below
 *   (the +1 is the baseline row itself)
 */
Box *box_hstack(Box *left, Box *right, int gap) {
  /* How many rows sit strictly above the baseline in each box */
  int above =
      (left->baseline > right->baseline) ? left->baseline : right->baseline;

  /* How many rows sit strictly below the baseline in each box */
  int left_below = left->height - left->baseline - 1;
  int right_below = right->height - right->baseline - 1;
  int below = (left_below > right_below) ? left_below : right_below;

  int h = above + 1 + below;
  int w = left->width + gap + right->width;

  Box *out = box_new(w, h, above);
  box_clear(out);

  /* Blit left: shift it down so its baseline lands on row `above` */
  int left_top = above - left->baseline;
  box_blit(out, left, left_top, 0);

  /* Blit right: same logic, but offset right by left->width + gap cols */
  int right_top = above - right->baseline;
  box_blit(out, right, right_top, left->width + gap);

  return out;
}

/*
 * box_fraction — render num/den as a stacked fraction with a bar.
 *
 *      numerator
 *     -----------    ← bar row — this becomes the baseline
 *     denominator
 *
 * Width is the wider of the two, plus 2 padding chars (one each side).
 * Both num and den are centered horizontally over the bar.
 *
 * Baseline of the output = the bar row = num->height (0-indexed).
 */
Box *box_fraction(Box *num, Box *den) {
  int inner_w = (num->width > den->width) ? num->width : den->width;
  int w = inner_w + 2; /* 1 space padding on each side */
  int h = num->height + 1 + den->height;

  /* baseline = bar row index = num->height */
  Box *out = box_new(w, h, num->height);
  box_clear(out);

  /* Draw the fraction bar across the full width */
  memset(out->lines[num->height], '-', w);

  /* Center the numerator above the bar */
  int num_x = (w - num->width) / 2;
  box_blit(out, num, 0, num_x);

  /* Center the denominator below the bar */
  int den_x = (w - den->width) / 2;
  box_blit(out, den, num->height + 1, den_x);

  return out;
}

/*
 * box_power — render base^exp with exp raised above-right.
 *
 *    exp
 *   base
 *
 * The exponent sits at the TOP of the output box.
 * The base sits at the BOTTOM of the output box.
 * Width = base->width + exp->width (they don't overlap horizontally).
 * Height = base->height + exp->height (stacked, no overlap).
 *
 * Baseline: the baseline of the base box, shifted down by exp->height
 * because the base now starts at row exp->height in the output.
 */
Box *box_power(Box *base, Box *exp) {
  int w = base->width + exp->width;
  int h = base->height + exp->height;

  /* Baseline = where base's baseline sits in the combined grid */
  Box *out = box_new(w, h, exp->height + base->baseline);
  box_clear(out);

  /* Exponent goes at the top-right */
  box_blit(out, exp, 0, base->width);

  /* Base goes at the bottom-left */
  box_blit(out, base, exp->height, 0);

  return out;
}

/*
 * box_sqrt — render sqrt(inner) with the radical symbol.
 *
 * For a 1-row inner expression:
 *   ______
 *  \/ expr
 *
 * For a multi-row inner expression:
 *    ______
 *   | expr
 *  \/ expr
 *
 * The left rail uses ' |' for all rows except the last which uses '\/'
 * The top row is all underscores (the vinculum / overline).
 *
 * Width  = inner->width + 3   ("\" + "/" + " " prefix)
 * Height = inner->height + 1  (+1 for the top bar row)
 *
 * Baseline: inner->baseline + 1  (shifted down 1 for the top bar row)
 */
Box *box_sqrt(Box *inner) {
  int iw = inner->width;
  int ih = inner->height;
  int w = iw + 3;
  int h = ih + 1;

  Box *out = box_new(w, h, inner->baseline + 1);
  box_clear(out);

  /* Row 0: top bar — two spaces then underscores over the content */
  out->lines[0][0] = ' ';
  out->lines[0][1] = ' ';
  for (int j = 2; j < w; j++)
    out->lines[0][j] = '_';

  /* Rows 1..(h-2): vertical bar on the left for tall expressions */
  for (int i = 1; i < h - 1; i++) {
    out->lines[i][0] = ' ';
    out->lines[i][1] = '|';
    out->lines[i][2] = ' ';
  }

  /* Last row: the \/ symbol */
  out->lines[h - 1][0] = '\\';
  out->lines[h - 1][1] = '/';
  out->lines[h - 1][2] = ' ';

  /* Blit the inner content: starts at row 1, col 3 */
  box_blit(out, inner, 1, 3);

  return out;
}

/*
 * box_parens — wrap a box in stretchy parentheses.
 *
 * For a 1-row box:   (expr)
 * For a 3-row box:
 *   / expr \
 *   | expr |
 *   \ expr /
 *
 * Width  = inner->width + 2   (one char each side)
 * Height = inner->height      (parens stretch to match)
 * Baseline stays the same.
 */
Box *box_parens(Box *inner) {
  int w = inner->width + 2;
  int h = inner->height;

  Box *out = box_new(w, h, inner->baseline);
  box_clear(out);

  if (h == 1) {
    /* Simple single-line parens */
    out->lines[0][0] = '(';
    out->lines[0][w - 1] = ')';
  } else {
    /* Multi-line stretchy parens */
    out->lines[0][0] = '/';
    out->lines[0][w - 1] = '\\';
    for (int i = 1; i < h - 1; i++) {
      out->lines[i][0] = '|';
      out->lines[i][w - 1] = '|';
    }
    out->lines[h - 1][0] = '\\';
    out->lines[h - 1][w - 1] = '/';
  }

  /* Blit inner content at col offset 1 */
  box_blit(out, inner, 0, 1);

  return out;
}

/* =========================================================================
 * SECTION 3 — AST NODES
 *
 * The AST (Abstract Syntax Tree) is the data structure produced by the
 * parser. Each node represents one mathematical operation or value.
 *
 *   (x^2 + 1) / sqrt(y)
 *
 *          DIV
 *         /   \
 *       ADD   SQRT
 *      /   \    \
 *    POW    1    y
 *   /   \
 *  x     2
 *
 * =========================================================================*/

/*
 * ast_new_leaf — create a terminal node (number or variable).
 * Left and right children are NULL.
 */
ASTNode *ast_new_leaf(NodeType type, const char *value) {
  ASTNode *n = malloc(sizeof(ASTNode));
  if (!n) {
    perror("ast_new_leaf");
    exit(1);
  }
  n->type = type;
  n->left = NULL;
  n->right = NULL;
  strncpy(n->value, value, 63);
  n->value[63] = '\0';
  return n;
}

/*
 * ast_new_node — create an operator node with children.
 * value is empty for operators (we use type to identify them).
 */
ASTNode *ast_new_node(NodeType type, ASTNode *left, ASTNode *right) {
  ASTNode *n = malloc(sizeof(ASTNode));
  if (!n) {
    perror("ast_new_node");
    exit(1);
  }
  n->type = type;
  n->left = left;
  n->right = right;
  n->value[0] = '\0';
  return n;
}

/*
 * ast_free — recursively free the entire tree post-order.
 * Post-order: free children before parent, so we never lose
 * a child pointer by freeing the parent first.
 */
void ast_free(ASTNode *node) {
  if (!node)
    return;
  ast_free(node->left);
  ast_free(node->right);
  free(node);
}

/* =========================================================================
 * SECTION 4 — LEXER
 *
 * Converts the input string into a flat list of tokens.
 *
 * Input:  "(x^2 + 1) / sqrt(y)"
 * Output: LPAREN VAR(x) CARET NUM(2) PLUS NUM(1) RPAREN SLASH SQRT LPAREN
 * VAR(y) RPAREN EOF
 *
 * Strategy: walk the string char by char, classify each character,
 * emit a token. For multi-char tokens (numbers, variables, "sqrt")
 * we consume as many chars as belong to that token.
 *
 * =========================================================================*/

/* Internal helper — grow the token list if full */
static void tl_push(TokenList *tl, Token tok) {
  if (tl->count >= tl->capacity) {
    tl->capacity *= 2;
    tl->tokens = realloc(tl->tokens, tl->capacity * sizeof(Token));
    if (!tl->tokens) {
      perror("tl_push realloc");
      exit(1);
    }
  }
  tl->tokens[tl->count++] = tok;
}

TokenList *lex(const char *input) {
  TokenList *tl = malloc(sizeof(TokenList));
  tl->capacity = 64;
  tl->count = 0;
  tl->tokens = malloc(tl->capacity * sizeof(Token));

  int i = 0;
  while (input[i] != '\0') {

    /* Skip whitespace */
    if (isspace((unsigned char)input[i])) {
      i++;
      continue;
    }

    Token tok;
    memset(tok.value, 0, sizeof(tok.value));

    /* Numbers: consume all consecutive digits */
    if (isdigit((unsigned char)input[i])) {
      int start = i;
      while (isdigit((unsigned char)input[i]))
        i++;
      int len = i - start;
      strncpy(tok.value, input + start, len);
      tok.value[len] = '\0';
      tok.type = TOK_NUM;
      tl_push(tl, tok);
      continue;
    }

    /* Words: could be a variable or "sqrt" */
    if (isalpha((unsigned char)input[i])) {
      int start = i;
      while (isalpha((unsigned char)input[i]))
        i++;
      int len = i - start;
      strncpy(tok.value, input + start, len);
      tok.value[len] = '\0';

      if (strcmp(tok.value, "sqrt") == 0)
        tok.type = TOK_SQRT;
      else
        tok.type = TOK_VAR;

      tl_push(tl, tok);
      continue;
    }

    /* Single-character tokens */
    switch (input[i]) {
    case '+':
      tok.type = TOK_PLUS;
      break;
    case '-':
      tok.type = TOK_MINUS;
      break;
    case '*':
      tok.type = TOK_STAR;
      break;
    case '/':
      tok.type = TOK_SLASH;
      break;
    case '^':
      tok.type = TOK_CARET;
      break;
    case '(':
      tok.type = TOK_LPAREN;
      break;
    case ')':
      tok.type = TOK_RPAREN;
      break;
    default:
      fprintf(stderr, "lex: unknown character '%c'\n", input[i]);
      i++;
      continue;
    }
    tok.value[0] = input[i];
    tok.value[1] = '\0';
    tl_push(tl, tok);
    i++;
  }

  /* Always terminate with EOF */
  Token eof;
  eof.type = TOK_EOF;
  eof.value[0] = '\0';
  tl_push(tl, eof);

  return tl;
}

void token_list_free(TokenList *tl) {
  free(tl->tokens);
  free(tl);
}

/* =========================================================================
 * SECTION 5 — PARSER (Recursive Descent)
 *
 * Converts a TokenList into an AST.
 *
 * Grammar (operator precedence, lowest to highest):
 *
 *   expr    → term   (('+' | '-') term)*
 *   term    → factor (('*' | '/') factor)*
 *   factor  → base   ('^' factor)?       ← right-associative
 *   base    → NUM | VAR | '(' expr ')' | 'sqrt' '(' expr ')' | '-' base
 *
 * Right-associativity for '^' means x^y^z parses as x^(y^z).
 * We achieve this by calling factor() recursively for the right side.
 *
 * The parser state is just an index into the token list.
 *
 * =========================================================================*/

/* Parser state — passed around by pointer so all functions share it */
typedef struct {
  TokenList *tl;
  int pos; /* current position in tl->tokens */
} Parser;

/* Peek at the current token without consuming it */
static Token parser_peek(Parser *p) { return p->tl->tokens[p->pos]; }

/* Consume and return the current token, advance pos */
static Token parser_consume(Parser *p) { return p->tl->tokens[p->pos++]; }

/* Consume current token only if it matches expected type */
static void parser_expect(Parser *p, TokenType type) {
  Token t = parser_consume(p);
  if (t.type != type) {
    fprintf(stderr, "parser: expected token %d, got %d ('%s')\n", type, t.type,
            t.value);
    exit(1);
  }
}

/* Forward declarations for mutual recursion */
static ASTNode *parse_expr(Parser *p);
static ASTNode *parse_term(Parser *p);
static ASTNode *parse_factor(Parser *p);
static ASTNode *parse_base(Parser *p);

/*
 * parse_expr — handles + and -
 * Loops to handle chains like a + b + c
 */
static ASTNode *parse_expr(Parser *p) {
  ASTNode *left = parse_term(p);

  while (parser_peek(p).type == TOK_PLUS || parser_peek(p).type == TOK_MINUS) {
    Token op = parser_consume(p);
    ASTNode *right = parse_term(p);
    NodeType t = (op.type == TOK_PLUS) ? NODE_ADD : NODE_SUB;
    left = ast_new_node(t, left, right);
  }

  return left;
}

/*
 * parse_term — handles * and /
 * / becomes NODE_DIV which renders as a fraction.
 */
static ASTNode *parse_term(Parser *p) {
  ASTNode *left = parse_factor(p);

  while (parser_peek(p).type == TOK_STAR || parser_peek(p).type == TOK_SLASH) {
    Token op = parser_consume(p);
    ASTNode *right = parse_factor(p);
    NodeType t = (op.type == TOK_STAR) ? NODE_MUL : NODE_DIV;
    left = ast_new_node(t, left, right);
  }

  return left;
}

/*
 * parse_factor — handles ^ (right-associative)
 * x^y^z → x^(y^z) because we recurse into parse_factor again
 * instead of parse_base.
 */
static ASTNode *parse_factor(Parser *p) {
  ASTNode *base = parse_base(p);

  if (parser_peek(p).type == TOK_CARET) {
    parser_consume(p);              /* eat the ^ */
    ASTNode *exp = parse_factor(p); /* recurse for right-assoc */
    return ast_new_node(NODE_POW, base, exp);
  }

  return base;
}

/*
 * parse_base — handles atoms: numbers, variables, parens, sqrt, unary minus
 */
static ASTNode *parse_base(Parser *p) {
  Token t = parser_peek(p);

  if (t.type == TOK_NUM) {
    parser_consume(p);
    return ast_new_leaf(NODE_NUM, t.value);
  }

  if (t.type == TOK_VAR) {
    parser_consume(p);
    return ast_new_leaf(NODE_VAR, t.value);
  }

  if (t.type == TOK_LPAREN) {
    parser_consume(p); /* eat '(' */
    ASTNode *inner = parse_expr(p);
    parser_expect(p, TOK_RPAREN); /* eat ')' */
    return inner;
  }

  if (t.type == TOK_SQRT) {
    parser_consume(p); /* eat 'sqrt' */
    parser_expect(p, TOK_LPAREN);
    ASTNode *inner = parse_expr(p);
    parser_expect(p, TOK_RPAREN);
    return ast_new_node(NODE_SQRT, inner, NULL);
  }

  if (t.type == TOK_MINUS) {
    parser_consume(p); /* eat '-' */
    ASTNode *operand = parse_base(p);
    return ast_new_node(NODE_NEG, operand, NULL);
  }

  fprintf(stderr, "parse_base: unexpected token '%s' (type %d)\n", t.value,
          t.type);
  exit(1);
}

/* Public entry point for the parser */
static ASTNode *parse(TokenList *tl) {
  Parser p = {tl, 0};
  return parse_expr(&p);
}

/* =========================================================================
 * SECTION 6 — RENDERER
 *
 * Walks the AST and produces a Box for each node.
 * This is a simple recursive post-order traversal:
 *   render children first, then compose their boxes.
 *
 * Each binary operator creates a new box from its two children's boxes,
 * then frees the children's boxes (they've been consumed).
 *
 * =========================================================================*/

/*
 * needs_parens — decide if a child node should be wrapped in parentheses.
 *
 * We add parens when a lower-precedence operation appears as a child
 * of a higher-precedence one. For example:
 *   render (a+b)^2 — the ADD needs parens inside the POW.
 *   render a/(b+c) — the ADD needs parens inside the DIV.
 *
 * This is a simple heuristic; a full implementation would track
 * operator precedence more carefully.
 */
static int needs_parens(NodeType parent, ASTNode *child) {
  if (parent == NODE_POW) {
    return (child->type == NODE_ADD || child->type == NODE_SUB ||
            child->type == NODE_MUL || child->type == NODE_DIV);
  }
  if (parent == NODE_MUL) {
    return (child->type == NODE_ADD || child->type == NODE_SUB);
  }
  return 0;
}

/* Helper — render a node and optionally wrap in parens */
static Box *render_maybe_parens(NodeType parent, ASTNode *child) {
  Box *b = render(child);
  if (needs_parens(parent, child)) {
    Box *p = box_parens(b);
    box_free(b);
    return p;
  }
  return b;
}

Box *render(ASTNode *node) {
  if (!node)
    return box_from_str("?");

  switch (node->type) {

    /* ── Leaf nodes ─────────────────────────────────────────── */

  case NODE_NUM:
  case NODE_VAR:
    return box_from_str(node->value);

    /* ── Addition: left + right ──────────────────────────────── */

  case NODE_ADD: {
    Box *l = render_maybe_parens(NODE_ADD, node->left);
    Box *op = box_from_str("+");
    Box *r = render_maybe_parens(NODE_ADD, node->right);
    Box *lr = box_hstack(l, op, 1);
    box_free(l);
    box_free(op);
    Box *out = box_hstack(lr, r, 1);
    box_free(lr);
    box_free(r);
    return out;
  }

    /* ── Subtraction: left - right ───────────────────────────── */

  case NODE_SUB: {
    Box *l = render_maybe_parens(NODE_SUB, node->left);
    Box *op = box_from_str("-");
    Box *r = render_maybe_parens(NODE_SUB, node->right);
    Box *lr = box_hstack(l, op, 1);
    box_free(l);
    box_free(op);
    Box *out = box_hstack(lr, r, 1);
    box_free(lr);
    box_free(r);
    return out;
  }

    /* ── Multiplication: left * right (or implicit) ──────────── */

  case NODE_MUL: {
    Box *l = render_maybe_parens(NODE_MUL, node->left);
    Box *op = box_from_str("*");
    Box *r = render_maybe_parens(NODE_MUL, node->right);
    Box *lr = box_hstack(l, op, 1);
    box_free(l);
    box_free(op);
    Box *out = box_hstack(lr, r, 1);
    box_free(lr);
    box_free(r);
    return out;
  }

    /* ── Division: renders as a stacked fraction ─────────────── */

  case NODE_DIV: {
    Box *num = render(node->left);
    Box *den = render(node->right);
    Box *out = box_fraction(num, den);
    box_free(num);
    box_free(den);
    return out;
  }

    /* ── Power: base^exp ─────────────────────────────────────── */

  case NODE_POW: {
    Box *base = render_maybe_parens(NODE_POW, node->left);
    Box *exp = render(node->right);
    Box *out = box_power(base, exp);
    box_free(base);
    box_free(exp);
    return out;
  }

    /* ── Square root ─────────────────────────────────────────── */

  case NODE_SQRT: {
    Box *inner = render(node->left);
    Box *out = box_sqrt(inner);
    box_free(inner);
    return out;
  }

    /* ── Unary negation: -expr ───────────────────────────────── */

  case NODE_NEG: {
    Box *minus = box_from_str("-");
    Box *inner = render_maybe_parens(NODE_NEG, node->left);
    Box *out = box_hstack(minus, inner, 0);
    box_free(minus);
    box_free(inner);
    return out;
  }

  default:
    return box_from_str("?");
  }
}

/* =========================================================================
 * box_from_str — convenience function used by render().
 *
 * Creates a 1-row Box from a C string.
 * Baseline = 0 (the only row IS the baseline).
 * =========================================================================*/

/* =========================================================================
 * engine_render — public API: string in, Box out.
 *
 * This is the single entry point you call from main.c:
 *
 *   Box *result = engine_render("(x^2 + 1) / sqrt(y)");
 *   box_print(result);
 *   box_free(result);
 *
 * =========================================================================*/

Box *engine_render(const char *input) {
  TokenList *tl = lex(input);
  ASTNode *ast = parse(tl);
  Box *out = render(ast);
  ast_free(ast);
  token_list_free(tl);
  return out;
}
