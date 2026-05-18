#pragma once
#include "box.h"

Box *box_hstack(Box *left, Box *right, int gap);
Box *box_fraction(Box *num, Box *den);
Box *box_power(Box *base, Box *exp);
Box *box_sqrt(Box *inner);
Box *box_parens(Box *inner);
