/* SPDX-License-Identifier: MIT */
#include "j2me.h"
#include "ui.h"
#include <glib-unix.h>
#include <signal.h>

typedef enum { LIST, DETAIL, INSTALL, REMOVE, CONFIG, ERROR } Page;
typedef enum { NONE, LIST_OP, INSTALL_OP, REMOVE_OP, RUN_OP } Op;
typedef struct {
  GApplication *application; bWindow *window; BuJ2meConfig config;
  GPtrArray *suites; GSubprocess *child; Page page; Op op;
  guint selected, midlet, timeout, shutdown_timeout, poll, term, interrupt;
  char *jar, *message, *active_suite; gboolean failed, timed_out, closing_requested;
  guint skipped_suites;
} App;

static void info(App *a, const char *s) { g_free(a->message); a->message = g_strdup(s); if (a->window) bRequestFrame(a->window); }
static BuJ2meSuite *current(App *a) { return a->suites && a->selected < a->suites->len ? g_ptr_array_index(a->suites, a->selected) : NULL; }
static void refresh(App *a);
static void open_jar(App *a);
static gboolean shutdown_force_cb(gpointer data);
static int draw(void *event, void *data);
static int key(void *event, void *data);

static void create_window(App *a)
{
  bDefaultTheme(bLoadScript("themes/utilities.ipt"));
  a->window = bCreateWindow("Javaアプリ", "org.bananui.J2ME");
  if (!a->window) { a->failed = TRUE; g_application_quit(a->application); return; }
  bRegisterEventHandler(&a->window->redraw, draw, a);
  bRegisterEventHandler(&a->window->keydown, key, a);
  bRequestFrame(a->window);
}

static void raise_window(App *a)
{
  /* The shell groups activation by app ID. Recreating the idle toplevel makes a
   * JAR opened from Files become the active member of the manager group. */
  if (a->window) bDestroyWindow(a->window);
  a->window = NULL;
  create_window(a);
}

static void request_shutdown(App *a)
{
  if (a->closing_requested) return;
  a->closing_requested = TRUE;
  if (a->child) {
    g_subprocess_send_signal(a->child, SIGTERM);
    a->shutdown_timeout = g_timeout_add_seconds(3, shutdown_force_cb, a);
  } else {
    g_application_quit(a->application);
  }
}

static void open_picker(App *a)
{
  g_autofree char *program = g_find_program_in_path("bananui-files");
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocess) picker = NULL;
  if (program) picker = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
                                         program, "--pick-j2me", NULL);
  if (!picker) {
    info(a, error ? error->message : "データフォルダを起動できません");
  }
}

static void finished(App *a, const char *out, const char *err, gboolean ok)
{
  Op op = a->op; a->op = NONE;
  if (a->timeout) { g_source_remove(a->timeout); a->timeout = 0; }
  if (a->shutdown_timeout) { g_source_remove(a->shutdown_timeout); a->shutdown_timeout = 0; }
  g_clear_object(&a->child);
  if (!ok) {
    info(a, a->timed_out ? "ランタイムの応答が時間切れになりました" : err ? err : "ランタイムが失敗しました");
  }
  a->timed_out = FALSE;
  if (a->closing_requested) { g_application_quit(a->application); return; }
  if (ok && op == LIST_OP) {
    guint skipped = 0;
    g_autoptr(GError) e = NULL;
    GPtrArray *items = bu_j2me_parse_list(out, &skipped, &e);
    if (!items) info(a, e->message);
    else { g_clear_pointer(&a->suites, g_ptr_array_unref); a->suites = items;
      if (a->selected >= items->len) a->selected = items->len ? items->len - 1 : 0;
      a->skipped_suites = skipped;
      a->page = LIST;
      info(a, items->len ? "一覧を更新しました" :
              skipped ? "表示できるアプリはありません" : "登録済みアプリはありません");
      if (a->jar) open_jar(a); }
  } else if (ok && op == INSTALL_OP) {
    g_autoptr(GError) e = NULL;
    if (!bu_j2me_check_completion(out, "install", NULL, &e)) info(a, e->message);
    else { g_clear_pointer(&a->jar, g_free); a->page = LIST; info(a, "アプリを登録しました"); refresh(a); }
  } else if (ok && op == REMOVE_OP) {
    g_autoptr(GError) e = NULL;
    if (!bu_j2me_check_completion(out, "remove", a->active_suite, &e)) info(a, e->message);
    else { a->page = LIST; info(a, "アプリと保存データを削除しました"); refresh(a); }
  }
  else if (ok && op == RUN_OP) { a->page = DETAIL; info(a, "アプリが終了しました"); refresh(a); }
}

static gboolean timeout_cb(gpointer data) { App *a = data; a->timeout = 0; a->timed_out = TRUE; if (a->child) g_subprocess_force_exit(a->child); return G_SOURCE_REMOVE; }
static gboolean shutdown_force_cb(gpointer data) { App *a = data; a->shutdown_timeout = 0; if (a->child) g_subprocess_force_exit(a->child); return G_SOURCE_REMOVE; }
static gboolean window_tick(gpointer data) { App *a = data; if (a->window && a->window->closing) request_shutdown(a); return G_SOURCE_CONTINUE; }
static void communicated(GObject *src, GAsyncResult *res, gpointer data)
{
  App *a = data; g_autofree char *out = NULL, *errout = NULL; g_autoptr(GError) e = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, &errout, &e);
  ok = ok && g_subprocess_get_if_exited(G_SUBPROCESS(src)) && g_subprocess_get_exit_status(G_SUBPROCESS(src)) == 0;
  if (!ok && !e) g_set_error(&e, G_IO_ERROR, G_IO_ERROR_FAILED, "ランタイムが異常終了しました");
  finished(a, out, e ? e->message : NULL, ok);
}

static void waited(GObject *src, GAsyncResult *res, gpointer data)
{
  App *a = data;
  g_autoptr(GError) e = NULL;
  gboolean ok = g_subprocess_wait_finish(G_SUBPROCESS(src), res, &e) &&
                g_subprocess_get_if_exited(G_SUBPROCESS(src)) &&
                g_subprocess_get_exit_status(G_SUBPROCESS(src)) == 0;
  if (!ok && !e) g_set_error(&e, G_IO_ERROR, G_IO_ERROR_FAILED, "MIDlet の実行に失敗しました");
  finished(a, NULL, e ? e->message : NULL, ok);
}

static void launch(App *a, Op op, const char *id, guint ordinal, const char *jar)
{
  if (a->op != NONE) { info(a, "処理中です"); return; }
  g_autoptr(GError) e = NULL;
  GPtrArray *args = bu_j2me_build_argv(&a->config, op == LIST_OP ? "list" : op == INSTALL_OP ? "install" :
                                        op == REMOVE_OP ? "remove" : "run", id, ordinal, jar, &e);
  if (!args) { info(a, e->message); return; }
  GSubprocessFlags flags = op == RUN_OP ? G_SUBPROCESS_FLAGS_STDIN_PIPE :
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE | G_SUBPROCESS_FLAGS_STDIN_PIPE;
  a->child = g_subprocess_newv((const char * const *)args->pdata, flags, &e);
  g_ptr_array_unref(args);
  if (!a->child) { info(a, e->message); return; }
  a->op = op; a->page = CONFIG;
  g_free(a->active_suite); a->active_suite = id ? g_strdup(id) : NULL;
  info(a, op == LIST_OP ? "一覧を読み込んでいます" : op == INSTALL_OP ? "登録しています" :
          op == REMOVE_OP ? "削除しています" : "アプリを起動しています");
  if (op != RUN_OP) {
    a->timeout = g_timeout_add_seconds(op == INSTALL_OP ? 300 : 75, timeout_cb, a);
    g_subprocess_communicate_utf8_async(a->child, NULL, NULL, communicated, a);
  } else {
    g_subprocess_wait_async(a->child, NULL, waited, a);
  }
}
static void refresh(App *a) { launch(a, LIST_OP, NULL, 0, NULL); }

static int draw(void *event, void *data)
{
  App *a = data; bWindow *w = a->window; cairo_t *cr = w->renderer; BuJ2meSuite *s = current(a);
  g_autofree char *warning = NULL;
  if (a->op == NONE && a->page == LIST && a->skipped_suites)
    warning = g_strdup_printf("読めないアプリ %u 件", a->skipped_suites);
  bu_ui_background(w, "Javaアプリ", a->op != NONE ? "処理中" :
                   warning ? warning : "アプリ一覧・起動・登録");
  if (a->op != NONE || a->page == CONFIG || a->page == ERROR) {
    cairo_set_source_rgb(cr, .8, .88, .95);
    bu_ui_text(cr, a->message ? a->message : "", 12, 104, w->width - 24, 12, FALSE, PANGO_ALIGN_CENTER, 6);
  } else if (a->page == INSTALL) {
    cairo_set_source_rgb(cr, 1, 1, 1);
    bu_ui_text(cr, "このJARを登録しますか？", 12, 94, w->width - 24, 16, TRUE, PANGO_ALIGN_CENTER, 2);
    g_autofree char *basename = g_path_get_basename(a->jar);
    bu_ui_text(cr, basename, 12, 142, w->width - 24, 10, FALSE, PANGO_ALIGN_CENTER, 2);
    bu_ui_text(cr, "登録済みのアプリは更新される場合があります", 12, 218, w->width - 24, 12, FALSE, PANGO_ALIGN_CENTER, 2);
  } else if (a->page == REMOVE) {
    cairo_set_source_rgb(cr, 1, 1, 1);
    bu_ui_text(cr, s ? s->name : "", 12, 105, w->width - 24, 16, TRUE, PANGO_ALIGN_CENTER, 2);
    cairo_set_source_rgb(cr, 1, .65, .55);
    bu_ui_text(cr, "アプリと保存データを削除します", 12, 165, w->width - 24, 12, FALSE, PANGO_ALIGN_CENTER, 3);
  } else if (a->page == ERROR) {
    cairo_set_source_rgb(cr, .8, .88, .95);
    bu_ui_text(cr, a->message ? a->message : "ファイルを開けません", 12, 104, w->width - 24, 12, FALSE, PANGO_ALIGN_CENTER, 6);
  } else if (a->page == DETAIL && s) {
    cairo_set_source_rgb(cr, .08, .17, .23); cairo_rectangle(cr, 8, 74, w->width - 16, 86); cairo_fill(cr);
    cairo_set_source_rgb(cr, 1, 1, 1); bu_ui_text(cr, s->name, 14, 82, w->width - 28, 16, TRUE, PANGO_ALIGN_LEFT, 2);
    bu_ui_text(cr, s->vendor, 14, 119, w->width - 28, 11, FALSE, PANGO_ALIGN_LEFT, 1);
    guint rows = MAX(1, (w->height - 198) / 42);
    guint start = (a->midlet / rows) * rows;
    for (guint i = start; i < MIN(s->midlets->len, start + rows); i++) {
      BuJ2meMidlet *m = g_ptr_array_index(s->midlets, i); double y = 170 + 42 * (i - start);
      cairo_set_source_rgb(cr, i == a->midlet ? .14 : .06, i == a->midlet ? .32 : .12, .2);
      cairo_rectangle(cr, 8, y, w->width - 16, 36); cairo_fill(cr);
      cairo_set_source_rgb(cr, 1, 1, 1); bu_ui_text(cr, m->name, 16, y + 8, w->width - 32, 12, FALSE, PANGO_ALIGN_LEFT, 1);
    }
  } else if (!a->suites || !a->suites->len) {
    cairo_set_source_rgb(cr, .75, .86, .94);
    bu_ui_text(cr, a->message ? a->message : "登録済みアプリはありません\n左キーからJARを登録", 12, 118, w->width - 24, 13, FALSE, PANGO_ALIGN_CENTER, 4);
  } else {
    guint shown = MAX(1, (w->height - 124) / 56), start = (a->selected / shown) * shown;
    for (guint i = start; i < MIN(a->suites->len, start + shown); i++) {
      BuJ2meSuite *it = g_ptr_array_index(a->suites, i); double y = 72 + (i - start) * 56;
      cairo_set_source_rgb(cr, i == a->selected ? .14 : .06, i == a->selected ? .32 : .12, .2);
      cairo_rectangle(cr, 8, y, w->width - 16, 51); cairo_fill(cr);
      cairo_set_source_rgb(cr, 1, 1, 1); bu_ui_text(cr, it->name, 15, y + 5, w->width - 30, 14, TRUE, PANGO_ALIGN_LEFT, 1);
      g_autofree char *sub = g_strdup_printf("%s · %u MIDlet", it->vendor, it->midlets->len);
      cairo_set_source_rgb(cr, .66, .81, .9); bu_ui_text(cr, sub, 15, y + 29, w->width - 30, 10, FALSE, PANGO_ALIGN_LEFT, 1);
    }
  }
  if (a->page == DETAIL) bu_ui_footer(w, "起動", "選択", "削除");
  else if (a->page == INSTALL) bu_ui_footer(w, "登録", "確認", "戻る");
  else if (a->page == REMOVE) bu_ui_footer(w, "削除", "確認", "戻る");
  else bu_ui_footer(w, "JAR登録", "詳細", "終了");
  return 1;
}

static void open_jar(App *a)
{
  if (!a->jar) { info(a, "JARファイルを指定してください"); return; }
  gsize n = strlen(a->jar);
  if (n < 4 || g_ascii_strcasecmp(a->jar + n - 4, ".jar") != 0) {
    a->page = ERROR; info(a, "JADは未対応です。ローカルJARを選択してください"); return;
  }
  a->page = INSTALL; g_clear_pointer(&a->message, g_free); bRequestFrame(a->window);
}
static int key(void *event, void *data)
{
  App *a = data; xkb_keysym_t k = ((bKeyEvent *)event)->sym;
  if (a->op != NONE) {
    if (k == BANANUI_KEY_EndCall) request_shutdown(a);
    return 0;
  }
  if (k == BANANUI_KEY_EndCall) g_application_quit(a->application);
  else if ((bu_ui_right(k) && a->page != DETAIL) || k == XKB_KEY_Escape || k == XKB_KEY_BackSpace) {
    if (a->page == REMOVE) a->page = DETAIL;
    else if (a->page == INSTALL) { g_clear_pointer(&a->jar, g_free); a->page = LIST; }
    else if (a->page == DETAIL || a->page == ERROR) { g_clear_pointer(&a->jar, g_free); a->page = LIST; }
    else g_application_quit(a->application);
  } else if (a->page == CONFIG && (k == XKB_KEY_F5 || k == BANANUI_KEY_Pound || k == XKB_KEY_numbersign)) {
    g_autoptr(GError) error = NULL;
    bu_j2me_config_clear(&a->config);
    if (bu_j2me_config_load(&a->config, &error)) {
      if (a->jar) open_jar(a);
      else refresh(a);
    } else info(a, error->message);
  } else if (a->page == LIST) {
    if (k == XKB_KEY_Up && a->selected) a->selected--;
    else if (k == XKB_KEY_Down && a->suites && a->selected + 1 < a->suites->len) a->selected++;
    else if (bu_ui_left(k)) { if (a->jar) open_jar(a); else open_picker(a); }
    else if (k == XKB_KEY_Return && current(a)) { a->page = DETAIL; a->midlet = 0; }
    else if (k == XKB_KEY_F5 || k == BANANUI_KEY_Pound || k == XKB_KEY_numbersign) refresh(a);
  } else if (a->page == INSTALL) {
    if (bu_ui_left(k) || k == XKB_KEY_Return) launch(a, INSTALL_OP, NULL, 0, a->jar);
  } else if (a->page == DETAIL) {
    BuJ2meSuite *s = current(a);
    if (k == XKB_KEY_Up && a->midlet) a->midlet--;
    else if (k == XKB_KEY_Down && s && a->midlet + 1 < s->midlets->len) a->midlet++;
    else if (bu_ui_left(k) || k == XKB_KEY_Return) { if (s && s->midlets->len) launch(a, RUN_OP, s->id, ((BuJ2meMidlet *)g_ptr_array_index(s->midlets, a->midlet))->ordinal, NULL); }
    else if (k == XKB_KEY_Delete || bu_ui_right(k)) a->page = REMOVE;
  } else if (a->page == REMOVE) {
    if (bu_ui_left(k) || k == XKB_KEY_Return) { BuJ2meSuite *s = current(a); if (s) launch(a, REMOVE_OP, s->id, 0, NULL); } else a->page = DETAIL;
  }
  bRequestFrame(a->window); return 0;
}

static void activate(GApplication *app, App *a)
{
  if (a->window) { bRequestFrame(a->window); return; }
  g_autoptr(GError) e = NULL; gboolean configured = bu_j2me_config_load(&a->config, &e);
  create_window(a);
  if (!a->window) return;
  g_application_hold(app);
  a->poll = g_timeout_add(100, window_tick, a);
  if (configured) {
    if (a->jar) open_jar(a);
    else refresh(a);
  }
  else {
    g_autofree char *config_path = g_build_filename(g_get_user_config_dir(), "bananui-utils", "runtime.ini", NULL);
    a->page = CONFIG; a->message = g_strdup_printf("設定が必要です\n%s\n%s", e->message, config_path);
  }
  bRequestFrame(a->window);
}
static void opened(GApplication *app, GFile **files, gint n, const char *hint, gpointer data)
{
  App *a = data;
  if (n != 1) { info(a, "一度に登録できるJARは1件です"); return; }
  if (!g_file_is_native(files[0])) { info(a, "ネットワーク上のファイルは登録できません"); return; }
  if (a->op != NONE) { info(a, "処理中です。終わってからJARを開き直してください"); return; }
  g_free(a->jar); a->jar = g_file_get_path(files[0]);
  if (!a->window) activate(app, a);
  else if (a->op != RUN_OP && !a->window->focused) {
    raise_window(a);
  }
  if (a->op == NONE) open_jar(a);
}
static gboolean stop(gpointer data) {
  App *a = data;
  guint id = g_source_get_id(g_main_current_source());
  if (id == a->term) a->term = 0;
  else if (id == a->interrupt) a->interrupt = 0;
  request_shutdown(a); return G_SOURCE_REMOVE;
}
int main(int argc, char **argv)
{
  App a = {0}; a.application = g_application_new("org.bananui.J2ME", G_APPLICATION_HANDLES_OPEN);
  g_signal_connect(a.application, "activate", G_CALLBACK(activate), &a);
  g_signal_connect(a.application, "open", G_CALLBACK(opened), &a);
  a.term = g_unix_signal_add(SIGTERM, stop, &a); a.interrupt = g_unix_signal_add(SIGINT, stop, &a);
  int status = g_application_run(a.application, argc, argv);
  if (a.timeout) g_source_remove(a.timeout);
  if (a.shutdown_timeout) g_source_remove(a.shutdown_timeout);
  if (a.poll) g_source_remove(a.poll);
  if (a.term) g_source_remove(a.term);
  if (a.interrupt) g_source_remove(a.interrupt);
  if (a.child) { g_subprocess_force_exit(a.child); g_object_unref(a.child); }
  if (a.window) bDestroyWindow(a.window);
  g_clear_pointer(&a.suites, g_ptr_array_unref);
  bu_j2me_config_clear(&a.config); g_free(a.jar); g_free(a.message); g_free(a.active_suite); g_object_unref(a.application);
  return a.failed ? 1 : status;
}
