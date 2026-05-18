#pragma once
#include <stdlib.h>

typedef struct {
  char **lines;
  int width;
  int height;
  int baseline;
} Box;

Box *box_new(int width, int height, int baseline);
void box_free(Box *b);
void box_clear(Box *b);
void box_blit(Box *dst, Box *src, int row_off, int col_off);
void box_print(Box *b);
Box *box_from_str(const char *s);
