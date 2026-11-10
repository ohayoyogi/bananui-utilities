/* SPDX-License-Identifier: MIT */
/* Synthetic Wayland window for launcher UI tests; it does not execute Java. */
#include "ui.h"
#include <glib-unix.h>

typedef struct { GApplication *application; bWindow *window; const char *app_id; guint value, poll, term; } Fixture;
static int draw(void *event, void *data)
{
  Fixture *fixture = data;
  bu_ui_background(fixture->window, "起動先のテスト", "GUI検証用・Javaは実行しません");
  g_autofree char *text = g_strdup_printf("入力：%u", fixture->value);
  cairo_set_source_rgb(fixture->window->renderer, 1, 1, 1);
  bu_ui_text(fixture->window->renderer, text, 12, 140, fixture->window->width - 24, 24, TRUE, PANGO_ALIGN_CENTER, 2);
  bu_ui_footer(fixture->window, "", "", "終了"); return 1;
}
static int key(void *event, void *data)
{
  Fixture *fixture = data; xkb_keysym_t sym = ((bKeyEvent *)event)->sym;
  if (sym == XKB_KEY_F2 || sym == XKB_KEY_Escape || sym == BANANUI_KEY_EndCall) g_application_quit(fixture->application);
  else if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) {
    fixture->value = (fixture->value * 10 + sym - XKB_KEY_0) % 10000; bRequestFrame(fixture->window);
  }
  return 0;
}
static gboolean tick(gpointer data)
{ Fixture *fixture = data; if (fixture->window->closing) g_application_quit(fixture->application); return G_SOURCE_CONTINUE; }
static gboolean stop(gpointer data)
{ Fixture *fixture = data; g_application_quit(fixture->application); return G_SOURCE_CONTINUE; }
static void activate(GApplication *application, Fixture *fixture)
{
  fixture->window = bCreateWindow("起動先のテスト", fixture->app_id);
  if (!fixture->window) return;
  g_application_hold(application);
  bRegisterEventHandler(&fixture->window->redraw, draw, fixture);
  bRegisterEventHandler(&fixture->window->keydown, key, fixture);
  fixture->poll = g_timeout_add(100, tick, fixture); bRequestFrame(fixture->window);
}
int main(int argc, char **argv)
{
  Fixture fixture = {0}; fixture.app_id = argc > 1 ? argv[1] : "org.bananui.J2ME";
  fixture.application = g_application_new("org.bananui.J2METestRuntime", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(fixture.application, "activate", G_CALLBACK(activate), &fixture);
  fixture.term = g_unix_signal_add(SIGTERM, stop, &fixture);
  int result = g_application_run(fixture.application, 1, argv);
  if (fixture.poll) g_source_remove(fixture.poll);
  g_source_remove(fixture.term);
  if (fixture.window) bDestroyWindow(fixture.window);
  g_object_unref(fixture.application); return result;
}
