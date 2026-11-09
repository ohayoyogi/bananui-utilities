/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "calculator.h"
#include "ui.h"
#include <glib-unix.h>

typedef struct { GApplication *application; bWindow *window; BuCalculator calculator; guint poll, term, interrupt; } App;

static int render(void *event, void *data)
{
  App *app = data;
  bWindow *window = app->window;
  cairo_t *cr = window->renderer;
  bu_ui_background(window, "電卓", "数字キーと方向キーで操作");
  cairo_set_source_rgb(cr, .09, .18, .25);
  cairo_rectangle(cr, 8, 78, window->width - 16, 100); cairo_fill(cr);
  cairo_set_source_rgb(cr, app->calculator.error ? 1 : .88, app->calculator.error ? .5 : .98, .95);
  bu_ui_text(cr, bu_calculator_display(&app->calculator), 16, 106, window->width - 32,
    app->calculator.error ? 18 : 28, TRUE, PANGO_ALIGN_RIGHT, 1);
  cairo_set_source_rgb(cr, .65, .83, .93);
  bu_ui_text(cr, bu_calculator_hint(&app->calculator), 12, 191, window->width - 24, 12, FALSE, PANGO_ALIGN_CENTER, 2);
  cairo_set_source_rgb(cr, 1, 1, 1);
  bu_ui_text(cr, "↑ ＋     ↓ −     ← ×     → ÷", 8, 244, window->width - 16, 13, FALSE, PANGO_ALIGN_CENTER, 1);
  bu_ui_text(cr, "＊ 小数点     ＃ 符号切替", 8, 278, window->width - 16, 12, FALSE, PANGO_ALIGN_CENTER, 1);
  bu_ui_text(cr, "クリアキーで一文字消去", 8, 308, window->width - 16, 11, FALSE, PANGO_ALIGN_CENTER, 1);
  bu_ui_footer(window, "全消去", "計算", "終了");
  return 1;
}

static int keydown(void *event, void *data)
{
  App *app = data;
  bKeyEvent *key = event;
  char action = 0;
  if (bu_ui_right(key->sym) || key->sym == BANANUI_KEY_EndCall || key->sym == XKB_KEY_Escape) {
    g_application_quit(app->application); return 0;
  }
  if (bu_ui_left(key->sym) || key->sym == XKB_KEY_Delete) action = 'C';
  else if (key->sym >= XKB_KEY_0 && key->sym <= XKB_KEY_9) action = '0' + key->sym - XKB_KEY_0;
  else if (key->sym >= XKB_KEY_KP_0 && key->sym <= XKB_KEY_KP_9) action = '0' + key->sym - XKB_KEY_KP_0;
  else switch (key->sym) {
    case XKB_KEY_Up: case XKB_KEY_plus: action = '+'; break;
    case XKB_KEY_Down: case XKB_KEY_minus: action = '-'; break;
    case XKB_KEY_Left: case XKB_KEY_asterisk: action = '*'; break;
    case XKB_KEY_Right: case XKB_KEY_slash: action = '/'; break;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_equal: action = '='; break;
    case BANANUI_KEY_Star: case XKB_KEY_period: case XKB_KEY_KP_Decimal: action = '.'; break;
    case BANANUI_KEY_Pound: case XKB_KEY_numbersign: action = 'S'; break;
    case XKB_KEY_BackSpace: action = '\b'; break;
  }
  if (action) bu_calculator_key(&app->calculator, action);
  bRequestFrame(app->window);
  return 0;
}

static gboolean quit_app(gpointer data) { App *app = data; g_application_quit(app->application); return G_SOURCE_CONTINUE; }
static gboolean closed(gpointer data)
{
  App *app = data;
  if (app->window->closing) g_application_quit(app->application);
  return G_SOURCE_CONTINUE;
}

static void activate(GApplication *application, App *app)
{
  if (app->window) { bRequestFrame(app->window); return; }
  app->window = bCreateWindow("電卓", "org.bananui.Calculator");
  if (!app->window) { g_application_quit(application); return; }
  g_application_hold(application);
  bRegisterEventHandler(&app->window->redraw, render, app);
  bRegisterEventHandler(&app->window->keydown, keydown, app);
  bu_calculator_clear(&app->calculator);
  app->poll = g_timeout_add(100, closed, app);
  bRequestFrame(app->window);
}

int main(int argc, char **argv)
{
  App app = {0};
  app.application = g_application_new("org.bananui.Calculator", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
  app.term = g_unix_signal_add(SIGTERM, quit_app, &app);
  app.interrupt = g_unix_signal_add(SIGINT, quit_app, &app);
  int status = g_application_run(app.application, argc, argv);
  if (app.poll) g_source_remove(app.poll);
  g_source_remove(app.term); g_source_remove(app.interrupt);
  if (app.window) bDestroyWindow(app.window);
  g_object_unref(app.application);
  return status;
}
