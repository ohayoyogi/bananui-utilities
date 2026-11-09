/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "calculator.h"
#include <math.h>
#include <string.h>

void bu_calculator_clear(BuCalculator *calc)
{
  memset(calc, 0, sizeof *calc);
  g_strlcpy(calc->entry, "0", sizeof calc->entry);
  calc->fresh = TRUE;
}

static double value(BuCalculator *calc)
{
  return calc->fresh && !calc->pending ? calc->accumulator : g_ascii_strtod(calc->entry, NULL);
}

static gboolean evaluate(BuCalculator *calc, double left, double right, char op)
{
  double result;
  switch (op) {
    case '+': result = left + right; break;
    case '-': result = left - right; break;
    case '*': result = left * right; break;
    case '/':
      if (right == 0) {
        calc->error = TRUE;
        g_strlcpy(calc->entry, "0では割れません", sizeof calc->entry);
        return FALSE;
      }
      result = left / right; break;
    default: return FALSE;
  }
  if (!isfinite(result)) {
    calc->error = TRUE;
    g_strlcpy(calc->entry, "計算結果が大きすぎます", sizeof calc->entry);
    return FALSE;
  }
  calc->accumulator = result == 0 ? 0 : result;
  g_ascii_formatd(calc->entry, sizeof calc->entry, "%.12g", calc->accumulator);
  return TRUE;
}

gboolean bu_calculator_key(BuCalculator *calc, char key)
{
  calc->digit_limit = FALSE;
  if (key == 'C') { bu_calculator_clear(calc); return TRUE; }
  if (calc->error) {
    if (g_ascii_isdigit(key) || key == '.' || key == '\b') bu_calculator_clear(calc);
    else return TRUE;
  }
  if (g_ascii_isdigit(key)) {
    if (calc->fresh) {
      g_snprintf(calc->entry, sizeof calc->entry, "%c", key);
      calc->fresh = FALSE;
    } else {
      guint digits = 0;
      for (const char *p = calc->entry; *p; p++) digits += g_ascii_isdigit(*p) ? 1 : 0;
      if (digits >= 12) { calc->digit_limit = TRUE; return TRUE; }
      if (g_str_equal(calc->entry, "0") || g_str_equal(calc->entry, "-0")) {
        gboolean negative = calc->entry[0] == '-';
        g_snprintf(calc->entry, sizeof calc->entry, "%s%c", negative ? "-" : "", key);
      } else {
        gsize length = strlen(calc->entry);
        calc->entry[length] = key; calc->entry[length + 1] = 0;
      }
    }
    if (!calc->pending) calc->repeat_operator = 0;
    return TRUE;
  }
  if (key == '.') {
    if (calc->fresh) { g_strlcpy(calc->entry, "0", sizeof calc->entry); calc->fresh = FALSE; }
    if (!strchr(calc->entry, '.')) g_strlcat(calc->entry, ".", sizeof calc->entry);
    if (!calc->pending) calc->repeat_operator = 0;
    return TRUE;
  }
  if (key == 'S') {
    if (calc->fresh && calc->pending) { g_strlcpy(calc->entry, "0", sizeof calc->entry); calc->fresh = FALSE; }
    if (calc->fresh) {
      calc->accumulator = -calc->accumulator;
      g_ascii_formatd(calc->entry, sizeof calc->entry, "%.12g", calc->accumulator);
    } else if (calc->entry[0] == '-') memmove(calc->entry, calc->entry + 1, strlen(calc->entry));
    else {
      memmove(calc->entry + 1, calc->entry, strlen(calc->entry) + 1);
      calc->entry[0] = '-';
    }
    calc->repeat_operator = 0;
    return TRUE;
  }
  if (key == '\b') {
    if (calc->fresh) { g_strlcpy(calc->entry, "0", sizeof calc->entry); calc->fresh = FALSE; }
    else {
      gsize length = strlen(calc->entry);
      if (length <= 1 || (length == 2 && calc->entry[0] == '-')) g_strlcpy(calc->entry, "0", sizeof calc->entry);
      else calc->entry[length - 1] = 0;
    }
    calc->repeat_operator = 0;
    return TRUE;
  }
  if (key == '+' || key == '-' || key == '*' || key == '/') {
    if (calc->pending && !calc->fresh) {
      if (!evaluate(calc, calc->accumulator, g_ascii_strtod(calc->entry, NULL), calc->pending)) return TRUE;
    } else if (!calc->pending) calc->accumulator = value(calc);
    calc->pending = key; calc->fresh = TRUE; calc->repeat_operator = 0;
    return TRUE;
  }
  if (key == '=') {
    if (calc->pending) {
      double operand = calc->fresh ? calc->accumulator : g_ascii_strtod(calc->entry, NULL);
      if (!evaluate(calc, calc->accumulator, operand, calc->pending)) return TRUE;
      calc->repeat_operand = operand; calc->repeat_operator = calc->pending; calc->pending = 0;
    } else if (calc->repeat_operator) {
      if (!evaluate(calc, value(calc), calc->repeat_operand, calc->repeat_operator)) return TRUE;
    } else calc->accumulator = value(calc);
    calc->fresh = TRUE;
    return TRUE;
  }
  return FALSE;
}

const char *bu_calculator_display(const BuCalculator *calc) { return calc->entry; }
const char *bu_calculator_hint(const BuCalculator *calc)
{
  if (calc->error) return "数字を入力してやり直せます";
  if (calc->digit_limit) return "入力は12桁までです";
  switch (calc->pending) {
    case '+': return "＋  次の数を入力";
    case '-': return "−  次の数を入力";
    case '*': return "×  次の数を入力";
    case '/': return "÷  次の数を入力";
    default: return "入力した順に計算します";
  }
}
