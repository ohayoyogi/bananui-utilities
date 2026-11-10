/* SPDX-License-Identifier: MIT */
#include "file-store.h"
#include "ui.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <string.h>

typedef enum { ROOT_LIST, FILE_LIST, ACTION_MENU, EDIT_NAME, DELETE_CONFIRM, TEXT_VIEW, IMAGE_VIEW, DETAILS } Screen;
typedef enum { NONE, OPEN, BACK, MENU, RENAME, COPY, CUT, PASTE, MKDIR, ASK_TRASH, ROOTS, CONFIRM, QUIT, INFO } Action;
typedef struct { char *label, *path; } Root;
typedef struct {
  GApplication *application; bWindow *window; bCard *editor; bInputWidget *input;
  GPtrArray *roots, *entries; BuFiles *files, *clipboard; char *clipboard_name;
  GSubprocess *handoff; GCancellable *handoff_cancel;
  char *root_option, *sd_option, *message, *text, *edit_name;
  Screen screen; Action action; guint root_index, selected, menu_index, deferred, poll, term, interrupt, handoff_timeout, handoff_close;
  gboolean move, rename, pick_j2me, closing; gint64 message_until; int text_offset, text_height;
  cairo_surface_t *image; guint32 *image_pixels;
} App;

static const char *menu_labels[] = { "開く", "名前変更", "コピー", "切り取り", "貼り付け", "新しいフォルダ", "ごみ箱へ移動", "詳細", "保存先を切替" };
static const Action menu_actions[] = { OPEN, RENAME, COPY, CUT, PASTE, MKDIR, ASK_TRASH, INFO, ROOTS };
static void root_free(gpointer data) { Root *root = data; g_free(root->label); g_free(root->path); g_free(root); }
static BuFileEntry *selected(App *app)
{ return app->entries && app->selected < app->entries->len ? g_ptr_array_index(app->entries, app->selected) : NULL; }
static void message(App *app, const char *text)
{ g_free(app->message); app->message = g_strdup(text); app->message_until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC; if (app->window) bRequestFrame(app->window); }
static void show(App *app, Screen screen)
{
  if (app->screen == EDIT_NAME) bUnfocusCard(app->window, app->editor);
  if (screen == EDIT_NAME || screen == DELETE_CONFIRM || screen == TEXT_VIEW || screen == IMAGE_VIEW || screen == DETAILS)
    g_clear_pointer(&app->message, g_free);
  app->screen = screen;
  if (screen == EDIT_NAME) { bFocusBox(app->window, app->editor, app->input->box); bFocusCard(app->window, app->editor); }
  bRequestFrame(app->window);
}
static void refresh(App *app)
{
  g_autoptr(GError) error = NULL;
  GPtrArray *entries = bu_files_list(app->files, &error);
  if (!entries) {
    /* Never keep actionable entries from a directory that disappeared. */
    if (app->entries) g_ptr_array_set_size(app->entries, 0);
    app->selected = 0; message(app, error->message); return;
  }
  g_clear_pointer(&app->entries, g_ptr_array_unref); app->entries = entries;
  app->selected = entries->len ? MIN(app->selected, entries->len - 1) : 0;
}
static void add_root(App *app, const char *label, const char *path)
{
  if (!path || !*path) return;
  g_autofree char *canonical = g_canonicalize_filename(path, NULL);
  for (guint i = 0; i < app->roots->len; i++)
    if (g_str_equal(((Root *)g_ptr_array_index(app->roots, i))->path, canonical)) return;
  Root *root = g_new0(Root, 1); root->label = g_strdup(label); root->path = g_steal_pointer(&canonical);
  g_ptr_array_add(app->roots, root);
}
static void load_roots(App *app)
{
  app->roots = g_ptr_array_new_with_free_func(root_free);
  g_autofree char *default_path = g_build_filename(g_get_user_data_dir(), "bananui-utils", "files", NULL);
  if (!app->root_option && g_mkdir_with_parents(default_path, 0700) < 0)
    message(app, "本体の保存先を作成できません");
  add_root(app, "本体", app->root_option ? app->root_option : default_path);
  if (app->sd_option) add_root(app, "SDカード", app->sd_option);
  g_autoptr(GVolumeMonitor) monitor = g_volume_monitor_get();
  GList *mounts = g_volume_monitor_get_mounts(monitor);
  for (GList *item = mounts; item; item = item->next) {
    GMount *mount = item->data;
    if (!g_mount_can_unmount(mount)) continue;
    g_autoptr(GFile) root = g_mount_get_root(mount);
    g_autofree char *path = g_file_get_path(root), *label = g_mount_get_name(mount);
    add_root(app, label, path);
  }
  g_list_free_full(mounts, g_object_unref);
}
static gboolean jar_file(const char *name)
{
  g_autofree char *lower = g_ascii_strdown(name, -1);
  return g_str_has_suffix(lower, ".jar") || g_str_has_suffix(lower, ".jad");
}
static gboolean close_picker(gpointer data)
{ App *app = data; app->handoff_close = 0; g_application_quit(app->application); return G_SOURCE_REMOVE; }
static gboolean handoff_timeout(gpointer data)
{ App *app = data; app->handoff_timeout = 0; g_cancellable_cancel(app->handoff_cancel); return G_SOURCE_REMOVE; }
static void handoff_finished(GObject *object, GAsyncResult *result, gpointer data)
{
  App *app = data; g_autoptr(GError) error = NULL;
  gboolean ok = g_subprocess_wait_finish(G_SUBPROCESS(object), result, &error) &&
    g_subprocess_get_if_exited(G_SUBPROCESS(object)) && g_subprocess_get_exit_status(G_SUBPROCESS(object)) == 0;
  if (app->handoff_timeout) { g_source_remove(app->handoff_timeout); app->handoff_timeout = 0; }
  g_clear_object(&app->handoff_cancel); g_clear_object(&app->handoff);
  if (app->closing) g_application_quit(app->application);
  else if (ok) {
    /* Let the receiving window finish mapping before closing this child window. */
    app->handoff_close = g_timeout_add(150, close_picker, app);
  } else message(app, "管理画面への受渡しを確認できませんでした");
}
static gboolean load_image(App *app, const char *path, guint64 bytes, GError **error)
{
  int width = 0, height = 0;
  if (bytes > 32 * 1024 * 1024 || !gdk_pixbuf_get_file_info(path, &width, &height) ||
      width <= 0 || height <= 0 || (guint64)width * height > 64000000) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "この画像はプレビューできません"); return FALSE;
  }
  g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_new_from_file_at_scale(path, 800, 800, TRUE, error);
  if (!pixbuf) return FALSE;
  if (app->image) cairo_surface_destroy(app->image);
  g_clear_pointer(&app->image_pixels, g_free);
  width = gdk_pixbuf_get_width(pixbuf); height = gdk_pixbuf_get_height(pixbuf);
  int channels = gdk_pixbuf_get_n_channels(pixbuf), stride = gdk_pixbuf_get_rowstride(pixbuf);
  const guchar *pixels = gdk_pixbuf_read_pixels(pixbuf);
  app->image_pixels = g_new0(guint32, width * height);
  for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
    const guchar *p = pixels + y * stride + x * channels;
    guint32 alpha = channels == 4 ? p[3] : 255;
    app->image_pixels[y * width + x] = (alpha << 24) | ((p[0] * alpha / 255) << 16) |
                                      ((p[1] * alpha / 255) << 8) | (p[2] * alpha / 255);
  }
  app->image = cairo_image_surface_create_for_data((unsigned char *)app->image_pixels, CAIRO_FORMAT_ARGB32, width, height, width * 4);
  return TRUE;
}
static void open_entry(App *app)
{
  if (app->screen == ROOT_LIST) {
    if (!app->roots->len) return;
    Root *root = g_ptr_array_index(app->roots, app->root_index);
    g_autoptr(GError) error = NULL;
    BuFiles *files = bu_files_new(root->path, &error);
    if (!files) { message(app, error->message); return; }
    bu_files_free(app->files); app->files = files; app->selected = 0;
    refresh(app); show(app, FILE_LIST); return;
  }
  BuFileEntry *entry = selected(app);
  if (!entry) { show(app, FILE_LIST); return; }
  g_autoptr(GError) error = NULL;
  if (entry->type == G_FILE_TYPE_DIRECTORY) {
    if (bu_files_enter(app->files, entry->name, &error)) { app->selected = 0; refresh(app); show(app, FILE_LIST); }
  } else if (entry->type != G_FILE_TYPE_REGULAR) {
    message(app, "リンクや特殊ファイルは開きません"); show(app, FILE_LIST);
  } else {
    g_autofree char *path = bu_files_path(app->files, entry->name, &error);
    if (!path) { message(app, error->message); return; }
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileInfo) info = g_file_query_info(file, "standard::type,standard::size",
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
    if (!info || g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR) {
      message(app, error ? error->message : "ファイルが変更されています。開き直してください"); refresh(app); return;
    }
    if (jar_file(entry->name)) {
      g_autoptr(GSubprocess) child = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error, "bananui-j2me", path, NULL);
      if (child) {
        message(app, "J2MEアプリ管理へ渡しました");
        if (app->pick_j2me) {
          app->handoff = g_object_ref(child); app->handoff_cancel = g_cancellable_new();
          app->handoff_timeout = g_timeout_add_seconds(5, handoff_timeout, app);
          g_subprocess_wait_async(app->handoff, app->handoff_cancel, handoff_finished, app);
        }
      }
      show(app, FILE_LIST);
    } else if (entry->content_type && g_str_has_prefix(entry->content_type, "image/")) {
      if (load_image(app, path, g_file_info_get_size(info), &error)) show(app, IMAGE_VIEW);
    } else {
      char *text = bu_files_read_text(app->files, entry->name, &error);
      if (text) { g_free(app->text); app->text = text; app->text_offset = 0; show(app, TEXT_VIEW); }
      else {
        g_clear_error(&error);
        g_autofree char *uri = g_filename_to_uri(path, NULL, &error);
        if (uri && g_app_info_launch_default_for_uri(uri, NULL, &error)) { message(app, "対応するアプリで開きました"); show(app, FILE_LIST); }
      }
    }
  }
  if (error) message(app, error->message);
}
static void details(App *app)
{
  BuFileEntry *entry = selected(app); if (!entry) { show(app, FILE_LIST); return; }
  g_autofree char *size = g_format_size(entry->size);
  g_autoptr(GDateTime) date = g_date_time_new_from_unix_local(entry->modified);
  g_autofree char *stamp = date ? g_date_time_format(date, "%Y/%m/%d %H:%M") : g_strdup("不明");
  g_free(app->text); app->text = g_strdup_printf("名前\n%s\n\n種類\n%s\n\n大きさ\n%s\n\n更新日時\n%s", entry->display_name,
    entry->type == G_FILE_TYPE_DIRECTORY ? "フォルダ" : entry->type == G_FILE_TYPE_SYMBOLIC_LINK ? "リンク（開きません）" : "ファイル", size, stamp);
  app->text_offset = 0; show(app, DETAILS);
}
static gboolean run_action(gpointer data)
{
  App *app = data; Action action = app->action; app->action = NONE; app->deferred = 0;
  BuFileEntry *entry = selected(app);
  g_autoptr(GError) error = NULL;
  if (action == QUIT) {
    if (app->handoff) { app->closing = TRUE; g_cancellable_cancel(app->handoff_cancel); }
    else g_application_quit(app->application);
    return G_SOURCE_REMOVE;
  }
  if (action == OPEN) open_entry(app);
  else if (action == ROOTS) show(app, ROOT_LIST);
  else if (action == BACK) {
    if (app->screen == EDIT_NAME || app->screen == ACTION_MENU || app->screen == DELETE_CONFIRM || app->screen == TEXT_VIEW || app->screen == IMAGE_VIEW || app->screen == DETAILS) show(app, FILE_LIST);
    else if (app->screen == ROOT_LIST) g_application_quit(app->application);
    else if (bu_files_parent(app->files)) { app->selected = 0; refresh(app); }
    else show(app, ROOT_LIST);
  } else if (action == MENU) { app->menu_index = 0; show(app, ACTION_MENU); }
  else if (action == INFO) details(app);
  else if (action == RENAME || action == MKDIR) {
    if (action == RENAME && !entry) { show(app, FILE_LIST); return G_SOURCE_REMOVE; }
    app->rename = action == RENAME;
    g_free(app->edit_name); app->edit_name = g_strdup(app->rename ? entry->name : "");
    bSetInputText(app->input, app->rename ? entry->display_name : "");
    show(app, EDIT_NAME);
  } else if (action == COPY || action == CUT) {
    if (entry && entry->type == G_FILE_TYPE_REGULAR) {
      bu_files_free(app->clipboard); app->clipboard = bu_files_clone(app->files);
      g_free(app->clipboard_name); app->clipboard_name = g_strdup(entry->name); app->move = action == CUT;
      message(app, action == CUT ? "切取りを準備しました。移動先で貼り付けてください" : "コピーを準備しました。保存先で貼り付けてください");
    } else message(app, "通常のファイルを選んでください");
    show(app, FILE_LIST);
  } else if (action == PASTE) {
    if (!app->clipboard) message(app, "先にコピーか切取りを選んでください");
    else if (bu_files_transfer(app->clipboard, app->clipboard_name, app->files, app->move, &error)) {
      if (app->move) { bu_files_free(app->clipboard); app->clipboard = NULL; g_clear_pointer(&app->clipboard_name, g_free); }
      refresh(app); message(app, "貼り付けました");
    }
    show(app, FILE_LIST);
  } else if (action == ASK_TRASH) {
    if (entry) show(app, DELETE_CONFIRM); else show(app, FILE_LIST);
  } else if (action == CONFIRM && app->screen == EDIT_NAME) {
    g_autofree char *text = bGetInputText(app->input);
    gboolean ok = app->rename ? bu_files_rename(app->files, app->edit_name, text, &error) : bu_files_mkdir(app->files, text, &error);
    if (ok) { refresh(app); show(app, FILE_LIST); message(app, app->rename ? "名前を変更しました" : "フォルダを作成しました"); }
  } else if (action == CONFIRM && app->screen == DELETE_CONFIRM && entry) {
    g_autofree char *trash = bu_files_trash(app->files, entry->name, &error);
    if (trash) { refresh(app); show(app, FILE_LIST); message(app, "ごみ箱へ移しました"); }
  }
  if (error) message(app, error->message);
  return G_SOURCE_REMOVE;
}
static void queue(App *app, Action action)
{
  if ((app->handoff || app->handoff_close) && action != QUIT) { message(app, "管理画面へ受け渡しています"); return; }
  if (!app->deferred) { app->action = action; app->deferred = g_idle_add(run_action, app); }
}
static int keydown(void *event, void *data)
{
  App *app = data; xkb_keysym_t key = ((bKeyEvent *)event)->sym;
  if (key == BANANUI_KEY_EndCall) { queue(app, QUIT); return 0; }
  if (app->screen == EDIT_NAME) {
    if (bu_ui_left(key) || key == XKB_KEY_Return) queue(app, CONFIRM);
    else if (bu_ui_right(key) || key == XKB_KEY_Escape) queue(app, BACK);
    else return 1;
  } else if (app->screen == TEXT_VIEW || app->screen == DETAILS) {
    if (key == XKB_KEY_Down) app->text_offset = MIN(app->text_offset + 40, MAX(0, app->text_height - (int)app->window->height + 122));
    else if (key == XKB_KEY_Up) app->text_offset = MAX(0, app->text_offset - 40);
    else if (key == XKB_KEY_Escape || key == XKB_KEY_BackSpace || bu_ui_right(key)) queue(app, BACK);
  } else if (app->screen == IMAGE_VIEW) {
    if (key == XKB_KEY_Escape || key == XKB_KEY_BackSpace || bu_ui_right(key) || key == XKB_KEY_Return) queue(app, BACK);
  } else if (app->screen == DELETE_CONFIRM) {
    if (bu_ui_left(key) || key == XKB_KEY_Return) queue(app, CONFIRM);
    else if (bu_ui_right(key) || key == XKB_KEY_BackSpace || key == XKB_KEY_Escape) queue(app, BACK);
  } else {
    guint *index = app->screen == ROOT_LIST ? &app->root_index : app->screen == ACTION_MENU ? &app->menu_index : &app->selected;
    guint count = app->screen == ROOT_LIST ? app->roots->len : app->screen == ACTION_MENU ? G_N_ELEMENTS(menu_labels) : app->entries ? app->entries->len : 0;
    if (key == XKB_KEY_Up && *index) (*index)--;
    else if (key == XKB_KEY_Down && *index + 1 < count) (*index)++;
    else if (key == XKB_KEY_Return || (app->screen != FILE_LIST && bu_ui_left(key))) queue(app, app->screen == ACTION_MENU ? menu_actions[app->menu_index] : OPEN);
    else if (bu_ui_left(key) && app->screen == FILE_LIST) queue(app, MENU);
    else if (bu_ui_right(key) || key == XKB_KEY_Escape || key == XKB_KEY_BackSpace) queue(app, BACK);
  }
  bRequestFrame(app->window); return 0;
}
static void draw_row(App *app, const char *label, const char *detail, int y, gboolean active)
{
  cairo_t *cr = app->window->renderer;
  cairo_set_source_rgb(cr, active ? .14 : .065, active ? .32 : .13, active ? .44 : .2);
  cairo_rectangle(cr, 8, y, app->window->width - 16, 53); cairo_fill(cr);
  cairo_set_source_rgb(cr, 1, 1, 1);
  bu_ui_text(cr, label, 15, y + 4, app->window->width - 30, 15, TRUE, PANGO_ALIGN_LEFT, 1);
  cairo_set_source_rgb(cr, .65, .8, .9);
  bu_ui_text(cr, detail, 15, y + 29, app->window->width - 30, 10, FALSE, PANGO_ALIGN_LEFT, 1);
}
static int render(void *event, void *data)
{
  App *app = data; bWindow *window = app->window; cairo_t *cr = window->renderer;
  Root *root = app->roots->len ? g_ptr_array_index(app->roots, app->root_index) : NULL;
  g_autofree char *relative = app->files ? g_filename_display_name(app->files->relative) : g_strdup("");
  g_autofree char *subtitle = app->screen == ROOT_LIST ? g_strdup("保存先を選んでください") :
    g_strdup_printf("%s%s%s", root ? root->label : "", *relative ? " / " : "", relative);
  bu_ui_background(window, "データフォルダ", subtitle);
  guint visible = MAX(1, (window->height - 130) / 57);
  if (app->screen == ROOT_LIST || app->screen == FILE_LIST || app->screen == ACTION_MENU) {
    guint current = app->screen == ROOT_LIST ? app->root_index : app->screen == ACTION_MENU ? app->menu_index : app->selected;
    guint count = app->screen == ROOT_LIST ? app->roots->len : app->screen == ACTION_MENU ? G_N_ELEMENTS(menu_labels) : app->entries ? app->entries->len : 0;
    guint start = current / visible * visible;
    for (guint i = start; i < MIN(count, start + visible); i++) {
      const char *label, *detail; g_autofree char *owned = NULL;
      if (app->screen == ROOT_LIST) { Root *item = g_ptr_array_index(app->roots, i); label = item->label; detail = "決定キーで開く"; }
      else if (app->screen == ACTION_MENU) { label = menu_labels[i]; detail = i == 2 || i == 3 ? "通常ファイルのみ" : ""; }
      else {
        BuFileEntry *item = g_ptr_array_index(app->entries, i); label = item->display_name;
        owned = item->type == G_FILE_TYPE_DIRECTORY ? g_strdup("フォルダ") : item->type == G_FILE_TYPE_SYMBOLIC_LINK ? g_strdup("リンク・開けません") : g_format_size(item->size);
        detail = owned;
      }
      draw_row(app, label, detail, 73 + (i - start) * 57, i == current);
    }
    if (!count) { cairo_set_source_rgb(cr, .7, .84, .94); bu_ui_text(cr, "ファイルはありません", 16, 120, window->width - 32, 15, FALSE, PANGO_ALIGN_CENTER, 2); }
    bu_ui_footer(window, app->screen == FILE_LIST ? "操作" : app->screen == ACTION_MENU ? "選択" : "開く",
      app->screen == ACTION_MENU ? "決定" : "開く", app->screen == ROOT_LIST ? "終了" : "戻る");
  } else if (app->screen == EDIT_NAME) {
    cairo_set_source_rgb(cr, 1, 1, 1);
    bu_ui_text(cr, app->rename ? "新しい名前を入力" : "フォルダ名を入力", 12, 76, window->width - 24, 16, TRUE, PANGO_ALIGN_LEFT, 2);
    bRenderCard(window, app->editor); bu_ui_footer(window, "確定", "確定", "取消");
  } else if (app->screen == DELETE_CONFIRM) {
    BuFileEntry *entry = selected(app); cairo_set_source_rgb(cr, 1, 1, 1);
    bu_ui_text(cr, "ごみ箱へ移しますか？", 12, 90, window->width - 24, 18, TRUE, PANGO_ALIGN_LEFT, 2);
    bu_ui_text(cr, entry ? entry->display_name : "", 12, 153, window->width - 24, 15, FALSE, PANGO_ALIGN_LEFT, 3);
    bu_ui_text(cr, "元の保存先のごみ箱に保管します。", 12, 241, window->width - 24, 12, FALSE, PANGO_ALIGN_LEFT, 3);
    bu_ui_footer(window, "移動", "移動", "取消");
  } else if (app->screen == TEXT_VIEW || app->screen == DETAILS) {
    g_autoptr(PangoLayout) layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_from_string("Noto Sans CJK JP 12");
    pango_layout_set_font_description(layout, font); pango_font_description_free(font);
    pango_layout_set_width(layout, MAX(1, window->width - 24) * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR); pango_layout_set_text(layout, app->text ? app->text : "", -1);
    pango_layout_get_pixel_size(layout, NULL, &app->text_height);
    cairo_save(cr); cairo_rectangle(cr, 12, 73, window->width - 24, MAX(1, window->height - 135)); cairo_clip(cr);
    cairo_set_source_rgb(cr, 1, 1, 1); cairo_move_to(cr, 12, 73 - app->text_offset); pango_cairo_show_layout(cr, layout); cairo_restore(cr);
    bu_ui_footer(window, "", "上下で移動", "戻る");
  } else if (app->screen == IMAGE_VIEW && app->image) {
    double width = cairo_image_surface_get_width(app->image), height = cairo_image_surface_get_height(app->image);
    double scale = MIN((window->width - 16) / width, MAX(1, window->height - 140) / height);
    cairo_save(cr); cairo_translate(cr, (window->width - width * scale) / 2, 73 + (window->height - 140 - height * scale) / 2);
    cairo_scale(cr, scale, scale); cairo_set_source_surface(cr, app->image, 0, 0); cairo_paint(cr); cairo_restore(cr);
    bu_ui_footer(window, "", "", "戻る");
  }
  if (app->message) { cairo_set_source_rgb(cr, 1, .82, .48); bu_ui_text(cr, app->message, 10, window->height - 64, window->width - 20, 11, FALSE, PANGO_ALIGN_CENTER, 2); }
  return 1;
}
static gboolean tick(gpointer data)
{
  App *app = data;
  if (app->window->closing) queue(app, QUIT);
  if (app->message && g_get_monotonic_time() >= app->message_until) { g_clear_pointer(&app->message, g_free); bRequestFrame(app->window); }
  return G_SOURCE_CONTINUE;
}
static gboolean terminate(gpointer data) { queue(data, QUIT); return G_SOURCE_CONTINUE; }
static void activate(GApplication *application, App *app)
{
  if (app->window) { bRequestFrame(app->window); return; }
  load_roots(app);
  bScript *theme = bLoadScript("themes/utilities.ipt");
  if (!theme) { g_application_quit(application); return; }
  bDefaultTheme(theme);
  /* A picker belongs to its requesting application, so closing it returns there. */
  app->window = bCreateWindow("データフォルダ", app->pick_j2me ? "org.bananui.J2ME" : "org.bananui.Files");
  if (!app->window) { g_application_quit(application); return; }
  g_application_hold(application);
  app->editor = bCreateCard("files editor");
  app->input = bCreateInputWidget("utility input", B_INPUT_MODE_ALPHA, B_INPUT_FLAG_NONE);
  bAddBox(app->editor->box, app->input->box);
  bRegisterEventHandler(&app->window->keydown, keydown, app);
  bRegisterEventHandler(&app->window->redraw, render, app);
  app->poll = g_timeout_add(100, tick, app);
  bRequestFrame(app->window);
}
static gint options(GApplication *application, GVariantDict *options, App *app)
{
  g_variant_dict_lookup(options, "root", "s", &app->root_option);
  g_variant_dict_lookup(options, "sd-root", "s", &app->sd_option);
  g_variant_dict_lookup(options, "pick-j2me", "b", &app->pick_j2me);
  /* A selection dialog may coexist with the user's normal Files window. */
  if (app->pick_j2me) g_application_set_flags(application, G_APPLICATION_NON_UNIQUE);
  return -1;
}
int main(int argc, char **argv)
{
  App app = {0}; app.screen = ROOT_LIST;
  app.application = g_application_new("org.bananui.Files", G_APPLICATION_DEFAULT_FLAGS);
  g_application_add_main_option(app.application, "root", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "本体として表示するフォルダ", "PATH");
  g_application_add_main_option(app.application, "sd-root", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "SDとして表示するフォルダ", "PATH");
  g_application_add_main_option(app.application, "pick-j2me", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, "J2ME登録用のファイル選択", NULL);
  g_signal_connect(app.application, "handle-local-options", G_CALLBACK(options), &app);
  g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
  app.term = g_unix_signal_add(SIGTERM, terminate, &app); app.interrupt = g_unix_signal_add(SIGINT, terminate, &app);
  int result = g_application_run(app.application, argc, argv);
  if (app.deferred) g_source_remove(app.deferred);
  if (app.poll) g_source_remove(app.poll);
  if (app.handoff_timeout) g_source_remove(app.handoff_timeout);
  if (app.handoff_close) g_source_remove(app.handoff_close);
  g_source_remove(app.term); g_source_remove(app.interrupt);
  if (app.editor) {
    if (app.editor->has_focus) bUnfocusCard(app.window, app.editor);
    bDestroyCard(app.editor);
  }
  if (app.window) bDestroyWindow(app.window);
  if (app.image) cairo_surface_destroy(app.image);
  g_free(app.image_pixels); g_free(app.message); g_free(app.text); g_free(app.edit_name);
  g_free(app.root_option); g_free(app.sd_option); g_free(app.clipboard_name);
  g_clear_object(&app.handoff_cancel); g_clear_object(&app.handoff);
  bu_files_free(app.files); bu_files_free(app.clipboard);
  g_clear_pointer(&app.roots, g_ptr_array_unref); g_clear_pointer(&app.entries, g_ptr_array_unref);
  g_object_unref(app.application); return result;
}
