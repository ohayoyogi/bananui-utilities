/* SPDX-License-Identifier: MIT */
#include "note-store.h"
#include "ui.h"
#include <glib-unix.h>
#include <string.h>

typedef enum { LIST, EDIT, LEAVE, DELETE_NOTE, CONFLICT } Screen;
typedef enum { NO_ACTION, NEW, OPEN, SAVE, BACK, SHOW_DELETE, CONFIRM, CANCEL, QUIT } Action;
typedef struct {
  GApplication *application;
  bWindow *window;
  bCard *editor;
  bInputWidget *input;
  BuNoteStore *store;
  GPtrArray *notes;
  BuDraft *draft;
  Screen screen;
  Action action;
  guint selected, choice, deferred, draft_timer, poll, term, interrupt;
  gboolean loading, draft_blocked, failed;
  char *message, *directory;
  gint64 message_until;
} App;

static void message(App *app, const char *value)
{
  g_free(app->message); app->message = g_strdup(value);
  app->message_until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  if (app->window) bRequestFrame(app->window);
}

static gboolean dirty(App *app)
{
  return app->draft && g_strcmp0(app->draft->text, app->draft->original);
}

static void capture_text(App *app)
{
  if (!app->draft || app->loading) return;
  g_free(app->draft->text); app->draft->text = bGetInputText(app->input);
}

static gboolean flush_draft(App *app)
{
  if (app->draft_timer) { g_source_remove(app->draft_timer); app->draft_timer = 0; }
  if (!app->draft) return TRUE;
  g_autoptr(GError) error = NULL;
  gboolean success = dirty(app) ? bu_note_store_save_draft(app->store, app->draft, &error) :
                                 bu_note_store_clear_draft(app->store, &error);
  if (!success) {
    message(app, error->message);
    g_printerr("下書きを保存できません: %s\n", error->message);
  }
  return success;
}

static gboolean draft_timeout(gpointer data)
{
  App *app = data;
  app->draft_timer = 0;
  flush_draft(app);
  return G_SOURCE_REMOVE;
}

static int changed(void *event, void *data)
{
  App *app = data;
  if (app->loading || app->screen != EDIT || !app->draft) return 1;
  char *text = bGetInputText(app->input);
  if (!g_strcmp0(text, app->draft->text)) { g_free(text); return 1; }
  g_free(app->draft->text); app->draft->text = text;
  if (app->draft_timer) g_source_remove(app->draft_timer);
  app->draft_timer = g_timeout_add(500, draft_timeout, app);
  return 1;
}

static void show(App *app, Screen screen)
{
  if (app->screen == EDIT) {
    capture_text(app);
    app->loading = TRUE;
    bUnfocusCard(app->window, app->editor);
    app->loading = FALSE;
  }
  app->screen = screen;
  if (screen == DELETE_NOTE || screen == LEAVE || screen == CONFLICT)
    g_clear_pointer(&app->message, g_free);
  if (screen == EDIT) {
    app->loading = TRUE;
    bSetInputText(app->input, app->draft->text);
    app->loading = FALSE;
    bFocusBox(app->window, app->editor, app->input->box);
    bFocusCard(app->window, app->editor);
  }
  bRequestFrame(app->window);
}

static void refresh_list(App *app)
{
  g_autoptr(GError) error = NULL;
  GPtrArray *notes = bu_note_store_list(app->store, &error);
  if (!notes) { message(app, error->message); return; }
  g_clear_pointer(&app->notes, g_ptr_array_unref); app->notes = notes;
  app->selected = notes->len ? MIN(app->selected, notes->len - 1) : 0;
  if (app->draft)
    for (guint i = 0; i < notes->len; i++) {
      BuNoteSummary *note = g_ptr_array_index(notes, i);
      if (g_str_equal(note->id, app->draft->id)) app->selected = i;
    }
}

static void open_editor(App *app, gboolean create)
{
  if (app->draft_blocked) { message(app, "下書きの保存先を確認してください"); return; }
  BuDraft *draft = g_new0(BuDraft, 1);
  if (create) {
    draft->id = g_uuid_string_random(); draft->text = g_strdup("");
  } else {
    if (!app->notes->len) { bu_draft_free(draft); return; }
    BuNoteSummary *note = g_ptr_array_index(app->notes, app->selected);
    draft->id = g_strdup(note->id);
    g_autoptr(GError) error = NULL;
    draft->text = bu_note_store_read(app->store, draft->id, &draft->etag, &error);
    if (!draft->text) { message(app, error->message); bu_draft_free(draft); return; }
  }
  draft->original = g_strdup(draft->text);
  app->loading = TRUE;
  bSetInputText(app->input, ""); /* Each document opens at its beginning. */
  app->loading = FALSE;
  bu_draft_free(app->draft); app->draft = draft;
  show(app, EDIT);
}

static gboolean save_note(App *app, gboolean copy)
{
  if (app->screen == EDIT) capture_text(app);
  g_autofree char *new_id = copy ? g_uuid_string_random() : NULL;
  const char *id = copy ? new_id : app->draft->id;
  const char *etag = copy ? NULL : app->draft->etag;
  g_autofree char *new_etag = NULL;
  g_autoptr(GError) error = NULL;
  if (!bu_note_store_save(app->store, id, app->draft->text, etag, &new_etag, &error)) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
      flush_draft(app); show(app, CONFLICT);
    } else message(app, error->message);
    return FALSE;
  }
  if (copy) { g_free(app->draft->id); app->draft->id = g_strdup(new_id); }
  g_free(app->draft->etag); app->draft->etag = g_steal_pointer(&new_etag);
  g_free(app->draft->original); app->draft->original = g_strdup(app->draft->text);
  if (flush_draft(app)) message(app, "保存しました");
  return TRUE;
}

static void discard_and_list(App *app)
{
  g_autoptr(GError) error = NULL;
  if (!bu_note_store_clear_draft(app->store, &error)) { message(app, error->message); return; }
  show(app, LIST);
  bu_draft_free(app->draft); app->draft = NULL;
  refresh_list(app);
}

static gboolean run_action(gpointer data)
{
  App *app = data;
  Action action = app->action;
  app->action = NO_ACTION; app->deferred = 0;
  if (action == NEW) open_editor(app, TRUE);
  else if (action == OPEN) open_editor(app, FALSE);
  else if (action == SAVE) save_note(app, FALSE);
  else if (action == QUIT) {
    if (app->screen == EDIT) capture_text(app);
    if (flush_draft(app)) g_application_quit(app->application);
    else app->window->closing = 0;
  }
  else if (action == SHOW_DELETE && app->notes->len) show(app, DELETE_NOTE);
  else if (action == BACK) {
    if (app->screen == EDIT && dirty(app)) {
      flush_draft(app); app->choice = 0; show(app, LEAVE);
    } else { show(app, LIST); refresh_list(app); }
  } else if (action == CANCEL) show(app, app->screen == DELETE_NOTE ? LIST : EDIT);
  else if (action == CONFIRM) {
    if (app->screen == DELETE_NOTE) {
      BuNoteSummary *note = g_ptr_array_index(app->notes, app->selected);
      g_autoptr(GError) error = NULL;
      if (bu_note_store_trash(app->store, note->id, &error)) {
        show(app, LIST); refresh_list(app); message(app, "ゴミ箱へ移しました");
      } else message(app, error->message);
    } else if (app->screen == CONFLICT) {
      if (save_note(app, TRUE)) { show(app, LIST); refresh_list(app); }
    } else if (app->screen == LEAVE) {
      if (app->choice == 0) { if (save_note(app, FALSE)) { show(app, LIST); refresh_list(app); } }
      else if (app->choice == 1) discard_and_list(app);
      else show(app, EDIT);
    }
  }
  return G_SOURCE_REMOVE;
}

static void queue(App *app, Action action)
{
  if (app->deferred) return;
  app->action = action;
  app->deferred = g_idle_add(run_action, app);
}

static int keydown(void *event, void *data)
{
  App *app = data;
  xkb_keysym_t key = ((bKeyEvent *)event)->sym;
  if (key == BANANUI_KEY_EndCall) { queue(app, QUIT); return 0; }
  if (app->screen == LIST) {
    if (key == XKB_KEY_Up && app->selected) app->selected--;
    else if (key == XKB_KEY_Down && app->selected + 1 < app->notes->len) app->selected++;
    else if (bu_ui_left(key)) queue(app, NEW);
    else if (key == XKB_KEY_Return) queue(app, OPEN);
    else if (bu_ui_right(key) || key == XKB_KEY_Delete) queue(app, SHOW_DELETE);
    else if (key == XKB_KEY_BackSpace || key == XKB_KEY_Escape) queue(app, QUIT);
  } else if (app->screen == EDIT) {
    if (bu_ui_left(key)) queue(app, SAVE);
    else if (bu_ui_right(key) || key == XKB_KEY_Escape) queue(app, BACK);
    else return 1; /* Shared input widget handles text, Return and navigation. */
  } else {
    if (key == XKB_KEY_Up && app->choice) app->choice--;
    else if (key == XKB_KEY_Down && app->choice < 2) app->choice++;
    else if (key == XKB_KEY_Return || bu_ui_left(key)) queue(app, CONFIRM);
    else if (bu_ui_right(key) || key == XKB_KEY_BackSpace || key == XKB_KEY_Escape) queue(app, CANCEL);
  }
  bRequestFrame(app->window);
  return 0;
}

static int render(void *event, void *data)
{
  App *app = data;
  bWindow *window = app->window;
  cairo_t *cr = window->renderer;
  g_autofree char *subtitle = app->screen == EDIT && app->draft ?
    g_strdup_printf("%s · %ld文字", dirty(app) ? "未保存・下書きを自動保存" : "保存済み", g_utf8_strlen(app->draft->text, -1)) :
    app->screen == LIST ? g_strdup_printf("%u件 · 更新が新しい順", app->notes->len) :
    g_strdup(app->screen == DELETE_NOTE ? "削除の確認" : "保存の確認");
  bu_ui_background(window, "メモ帳", subtitle);
  if (app->screen == EDIT) bRenderCard(window, app->editor);
  else if (app->screen == LIST) {
    guint visible = MAX(1, (window->height - 130) / 62);
    guint start = (app->selected / visible) * visible;
    if (!app->notes->len) {
      cairo_set_source_rgb(cr, .7, .84, .94);
      bu_ui_text(cr, "メモはありません\n左キーで新規作成", 16, 124, window->width - 32, 15, FALSE, PANGO_ALIGN_CENTER, 2);
    }
    for (guint i = start; i < MIN(app->notes->len, start + visible); i++) {
      BuNoteSummary *note = g_ptr_array_index(app->notes, i);
      double y = 73 + (i - start) * 62;
      cairo_set_source_rgb(cr, i == app->selected ? .14 : .065, i == app->selected ? .32 : .13, i == app->selected ? .44 : .2);
      cairo_rectangle(cr, 8, y, window->width - 16, 57); cairo_fill(cr);
      cairo_set_source_rgb(cr, 1, 1, 1);
      bu_ui_text(cr, note->title, 16, y + 5, window->width - 32, 15, TRUE, PANGO_ALIGN_LEFT, 1);
      g_autoptr(GDateTime) modified = g_date_time_new_from_unix_local(note->modified);
      g_autofree char *date = modified ? g_date_time_format(modified, "%m/%d %H:%M") : g_strdup("");
      cairo_set_source_rgb(cr, .66, .81, .9);
      bu_ui_text(cr, date, 16, y + 32, window->width - 32, 10, FALSE, PANGO_ALIGN_LEFT, 1);
    }
  } else {
    cairo_set_source_rgb(cr, 1, 1, 1);
    const char *prompt = app->screen == DELETE_NOTE ? "ゴミ箱へ移しますか？" :
      app->screen == CONFLICT ? "元のメモが変更されています。\n別のメモとして保存しますか？" : "編集内容をどうしますか？";
    bu_ui_text(cr, prompt, 16, 91, window->width - 32, 16, TRUE, PANGO_ALIGN_LEFT, 3);
    if (app->screen == DELETE_NOTE && app->notes->len) {
      BuNoteSummary *note = g_ptr_array_index(app->notes, app->selected);
      cairo_set_source_rgb(cr, .68, .84, .93);
      bu_ui_text(cr, note->title, 16, 156, window->width - 32, 14, FALSE, PANGO_ALIGN_LEFT, 2);
    }
    if (app->screen == LEAVE) {
      const char *choices[] = { "保存して一覧へ", "変更を破棄して一覧へ", "編集を続ける" };
      for (guint i = 0; i < 3; i++) {
        double y = 174 + i * 43;
        cairo_set_source_rgb(cr, i == app->choice ? .17 : .06, i == app->choice ? .37 : .12, i == app->choice ? .49 : .18);
        cairo_rectangle(cr, 8, y, window->width - 16, 39); cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        bu_ui_text(cr, choices[i], 16, y + 8, window->width - 32, 14, FALSE, PANGO_ALIGN_LEFT, 1);
      }
    }
  }
  if (app->message) {
    cairo_set_source_rgb(cr, 1, .82, .48);
    bu_ui_text(cr, app->message, 10, window->height - 62, window->width - 20, 11, FALSE, PANGO_ALIGN_CENTER, 1);
  }
  if (app->screen == LIST) bu_ui_footer(window, "新規", app->notes->len ? "開く" : "", app->notes->len ? "削除" : "");
  else if (app->screen == EDIT) bu_ui_footer(window, "保存", "改行", "一覧");
  else bu_ui_footer(window, app->screen == CONFLICT ? "別名保存" : app->screen == DELETE_NOTE ? "削除" : "決定",
    app->screen == DELETE_NOTE ? "削除" : "決定", "戻る");
  return 1;
}

static gboolean poll(gpointer data)
{
  App *app = data;
  if (app->window->closing) queue(app, QUIT);
  if (app->message && g_get_monotonic_time() >= app->message_until) {
    g_clear_pointer(&app->message, g_free); bRequestFrame(app->window);
  }
  return G_SOURCE_CONTINUE;
}

static gboolean terminate(gpointer data) { queue(data, QUIT); return G_SOURCE_CONTINUE; }

static void activate(GApplication *application, App *app)
{
  if (app->window) { bRequestFrame(app->window); return; }
  g_autoptr(GError) error = NULL;
  app->store = bu_note_store_new(app->directory, &error);
  if (!app->store) { g_printerr("%s\n", error->message); app->failed = TRUE; g_application_quit(application); return; }
  bScript *theme = bLoadScript("themes/utilities.ipt");
  if (!theme) { app->failed = TRUE; g_application_quit(application); return; }
  bDefaultTheme(theme);
  app->window = bCreateWindow("メモ帳", "org.bananui.Notes");
  if (!app->window) { app->failed = TRUE; g_application_quit(application); return; }
  g_application_hold(application);
  app->notes = g_ptr_array_new_with_free_func(bu_note_summary_free);
  app->editor = bCreateCard("notes editor");
  app->input = bCreateInputWidget("utility input", B_INPUT_MODE_ALPHA, B_INPUT_FLAG_MULTILINE_WRAP);
  if (!app->input) { app->failed = TRUE; g_application_quit(application); return; }
  bAddBox(app->editor->box, app->input->box);
  bRegisterEventHandler(&app->input->change, changed, app);
  bRegisterEventHandler(&app->window->keydown, keydown, app);
  bRegisterEventHandler(&app->window->redraw, render, app);
  refresh_list(app);
  app->draft = bu_note_store_load_draft(app->store, &error);
  if (error) {
    /* Preserve a broken draft instead of overwriting it with the next edit. */
    g_autofree char *old_path = g_build_filename(app->store->directory, ".draft.ini", NULL);
    g_autofree char *uuid = g_uuid_string_random();
    g_autofree char *name = g_strconcat(".draft-rejected-", uuid, ".ini", NULL);
    g_autofree char *new_path = g_build_filename(app->store->directory, name, NULL);
    g_autoptr(GFile) old_file = g_file_new_for_path(old_path), new_file = g_file_new_for_path(new_path);
    app->draft_blocked = !g_file_move(old_file, new_file, G_FILE_COPY_NONE, NULL, NULL, NULL, NULL);
    message(app, app->draft_blocked ? "下書きを読み込めません。保存先を確認してください" : "読み込めない下書きを退避しました");
  } else if (app->draft) { show(app, EDIT); message(app, "編集中の下書きを復元しました"); }
  app->poll = g_timeout_add(100, poll, app);
  bRequestFrame(app->window);
}

static gint options(GApplication *application, GVariantDict *options, App *app)
{
  g_variant_dict_lookup(options, "data-dir", "s", &app->directory);
  return -1;
}

int main(int argc, char **argv)
{
  App app = {0};
  app.application = g_application_new("org.bananui.Notes", G_APPLICATION_DEFAULT_FLAGS);
  g_application_add_main_option(app.application, "data-dir", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "メモの保存先", "PATH");
  g_signal_connect(app.application, "handle-local-options", G_CALLBACK(options), &app);
  g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
  app.term = g_unix_signal_add(SIGTERM, terminate, &app);
  app.interrupt = g_unix_signal_add(SIGINT, terminate, &app);
  int status = g_application_run(app.application, argc, argv);
  if (app.deferred) g_source_remove(app.deferred);
  if (app.draft_timer) g_source_remove(app.draft_timer);
  if (app.poll) g_source_remove(app.poll);
  g_source_remove(app.term); g_source_remove(app.interrupt);
  if (app.editor) {
    app.loading = TRUE;
    if (app.editor->has_focus) bUnfocusCard(app.window, app.editor);
    bDestroyCard(app.editor);
  }
  if (app.window) bDestroyWindow(app.window);
  g_clear_pointer(&app.notes, g_ptr_array_unref);
  bu_draft_free(app.draft); bu_note_store_free(app.store);
  g_free(app.message); g_free(app.directory); g_object_unref(app.application);
  return app.failed ? 1 : status;
}
