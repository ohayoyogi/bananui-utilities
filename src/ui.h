/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <bananui/bananui.h>
#include <pango/pangocairo.h>
void bu_ui_text(cairo_t *cr, const char *text, double x, double y, double width,
                double size, gboolean bold, PangoAlignment alignment, int lines);
void bu_ui_background(bWindow *window, const char *title, const char *subtitle);
void bu_ui_footer(bWindow *window, const char *left, const char *center, const char *right);
gboolean bu_ui_left(xkb_keysym_t key);
gboolean bu_ui_right(xkb_keysym_t key);
