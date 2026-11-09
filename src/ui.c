/* SPDX-License-Identifier: MIT */
#include "ui.h"

void bu_ui_text(cairo_t *cr, const char *text, double x, double y, double width,
                double size, gboolean bold, PangoAlignment alignment, int lines)
{
  g_autoptr(PangoLayout) layout = pango_cairo_create_layout(cr);
  PangoFontDescription *font = pango_font_description_new();
  pango_font_description_set_family(font, "Noto Sans CJK JP");
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
  pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);
  pango_layout_set_text(layout, text ? text : "", -1);
  pango_layout_set_width(layout, MAX(1, width) * PANGO_SCALE);
  pango_layout_set_height(layout, -MAX(1, lines));
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_alignment(layout, alignment);
  cairo_move_to(cr, x, y); pango_cairo_show_layout(cr, layout);
}

void bu_ui_background(bWindow *window, const char *title, const char *subtitle)
{
  cairo_t *cr = window->renderer;
  cairo_set_source_rgb(cr, .035, .075, .12); cairo_paint(cr);
  cairo_set_source_rgb(cr, 1, 1, 1);
  bu_ui_text(cr, title, 12, 9, window->width - 24, 20, TRUE, PANGO_ALIGN_LEFT, 1);
  cairo_set_source_rgb(cr, .6, .77, .88);
  bu_ui_text(cr, subtitle, 12, 41, window->width - 24, 11, FALSE, PANGO_ALIGN_LEFT, 1);
}

void bu_ui_footer(bWindow *window, const char *left, const char *center, const char *right)
{
  cairo_t *cr = window->renderer;
  double width = window->width, y = window->height - 32, third = width / 3;
  cairo_set_source_rgb(cr, .01, .025, .04);
  cairo_rectangle(cr, 0, y, width, 32); cairo_fill(cr);
  cairo_set_source_rgb(cr, 1, 1, 1);
  bu_ui_text(cr, left, 6, y + 6, third - 8, 13, FALSE, PANGO_ALIGN_LEFT, 1);
  bu_ui_text(cr, center, third, y + 6, third, 13, TRUE, PANGO_ALIGN_CENTER, 1);
  bu_ui_text(cr, right, third * 2, y + 6, third - 6, 13, FALSE, PANGO_ALIGN_RIGHT, 1);
}

gboolean bu_ui_left(xkb_keysym_t key) { return key == BANANUI_KEY_SoftLeft || key == XKB_KEY_F1; }
gboolean bu_ui_right(xkb_keysym_t key) { return key == BANANUI_KEY_SoftRight || key == XKB_KEY_F2; }
