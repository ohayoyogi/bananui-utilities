/* SPDX-License-Identifier: MIT
 * Integration checks against the shared widget, on an isolated compositor.
 */
#include <bananui/bananui.h>
#include <string.h>

typedef struct { bWindow *window; bCard *card; bInputWidget *input; GMainLoop *loop; guint stage, attempts; } Fixture;
static void press(Fixture *fixture, xkb_keysym_t key)
{
  bKeyEvent event = { .wnd = fixture->window, .sym = key };
  bTriggerEvent(fixture->window->keydown, &event);
}
static int render(void *event, void *data)
{
  Fixture *fixture = data;
  bRenderCard(fixture->window, fixture->card);
  return 1;
}
static gboolean check(gpointer data)
{
  Fixture *fixture = data;
  g_assert_cmpuint(fixture->attempts++, <, 50);
  if (!fixture->input->cont->lt) return G_SOURCE_CONTINUE;
  if (fixture->stage == 0) {
    g_autofree char *ascii = g_strnfill(507, 'a');
    g_autofree char *text = g_strconcat(ascii, "🐈", NULL);
    g_assert_cmpint(bSetInputText(fixture->input, text), ==, 0);
    for (guint i = 0; i < 508; i++) press(fixture, XKB_KEY_Right);
    press(fixture, XKB_KEY_BackSpace);
    g_autofree char *deleted = bGetInputText(fixture->input);
    g_assert_cmpstr(deleted, ==, ascii);
    g_print("PASS UTF-8 deletion at the 512-byte buffer boundary\n");
    bSetInputText(fixture->input, "");
    GString *lines = g_string_new(NULL);
    for (guint i = 0; i < 30; i++) g_string_append_printf(lines, "行%02u あいうえお\n", i + 1);
    bSetInputText(fixture->input, lines->str);
    g_string_free(lines, TRUE);
    fixture->stage++;
    bRequestFrame(fixture->window);
    return G_SOURCE_CONTINUE;
  }
  if (fixture->stage == 1) {
    g_print("Input layout has %d lines\n", bGetLastLine(fixture->input->cont) + 1);
    g_assert_cmpint(bGetLastLine(fixture->input->cont), >=, 29);
    for (guint i = 0; i < 30; i++) press(fixture, XKB_KEY_Down);
    fixture->stage++;
    bRequestFrame(fixture->window);
    return G_SOURCE_CONTINUE;
  }
  g_assert_cmpfloat(fixture->input->cont->yoffset, <, 0);
  g_print("PASS navigation and cursor scrolling through a long note\n");
  bSetInputText(fixture->input, ""); bSetInputText(fixture->input, "a");
  press(fixture, XKB_KEY_Right);
  bSetInputText(fixture->input, "日本");
  g_assert_true(g_utf8_validate(fixture->input->text, fixture->input->cursor, NULL));
  g_print("PASS UTF-8 cursor boundary after replacing a document\n");
  g_main_loop_quit(fixture->loop);
  return G_SOURCE_REMOVE;
}
int main(void)
{
  Fixture fixture = {0};
  bDefaultTheme(bLoadScript("themes/utilities.ipt"));
  fixture.window = bCreateWindow("Input regression", "org.bananui.InputTest");
  g_assert_nonnull(fixture.window);
  fixture.card = bCreateCard("notes editor");
  fixture.input = bCreateInputWidget("utility input", B_INPUT_MODE_ALPHA, B_INPUT_FLAG_MULTILINE_WRAP);
  g_assert_nonnull(fixture.input);
  bAddBox(fixture.card->box, fixture.input->box);
  bRegisterEventHandler(&fixture.window->redraw, render, &fixture);
  bFocusBox(fixture.window, fixture.card, fixture.input->box);
  bFocusCard(fixture.window, fixture.card);
  fixture.loop = g_main_loop_new(NULL, FALSE);
  g_timeout_add(100, check, &fixture);
  bRequestFrame(fixture.window);
  g_main_loop_run(fixture.loop);
  bUnfocusCard(fixture.window, fixture.card);
  bDestroyCard(fixture.card); bDestroyWindow(fixture.window); g_main_loop_unref(fixture.loop);
  return 0;
}
