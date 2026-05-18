#include "engine.h"

int main() {
  Box *b = engine_render("(x^2 + 1) / sqrt(y)");
  box_print(b);
  box_free(b);
}
