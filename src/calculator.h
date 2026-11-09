/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>
typedef struct {
  char entry[64];
  double accumulator, repeat_operand;
  char pending, repeat_operator;
  gboolean fresh, error, digit_limit;
} BuCalculator;
void bu_calculator_clear(BuCalculator *calc);
gboolean bu_calculator_key(BuCalculator *calc, char key);
const char *bu_calculator_display(const BuCalculator *calc);
const char *bu_calculator_hint(const BuCalculator *calc);
