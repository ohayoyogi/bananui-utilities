/* SPDX-License-Identifier: MIT */
#include "file-store.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

static gboolean failure(GError **error, GIOErrorEnum code, const char *message)
{
  g_set_error_literal(error, G_IO_ERROR, code, message); return FALSE;
}

static gboolean basename_ok(const char *name, gboolean new_name, GError **error)
{
  if (!name || !*name || strchr(name, '/') || g_str_equal(name, ".") ||
      g_str_equal(name, "..") || g_str_equal(name, BU_FILES_TRASH))
    return failure(error, G_IO_ERROR_INVALID_FILENAME, "使用できないファイル名です");
  if (new_name) {
    if (!g_utf8_validate(name, -1, NULL))
      return failure(error, G_IO_ERROR_INVALID_FILENAME, "名前はUTF-8で入力してください");
    for (const char *p = name; *p; p = g_utf8_next_char(p))
      if (g_unichar_iscntrl(g_utf8_get_char(p)))
        return failure(error, G_IO_ERROR_INVALID_FILENAME, "名前に制御文字は使えません");
  }
  return TRUE;
}

/* Recheck every directory on each operation: do not follow a replaced link. */
static char *current_path(BuFiles *files, GError **error)
{
  char *path = g_strdup(files->root);
  g_auto(GStrv) parts = g_strsplit(files->relative, "/", -1);
  for (guint i = 0; ; i++) {
    GStatBuf status;
    if (g_lstat(path, &status) < 0) {
      int saved = errno;
      g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved), "保存先を確認できません: %s", g_strerror(saved));
      g_free(path); return NULL;
    }
    if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode)) {
      failure(error, G_IO_ERROR_NOT_DIRECTORY, "保存先がフォルダではないか、リンクへ変更されています");
      g_free(path); return NULL;
    }
    while (parts[i] && !*parts[i]) i++;
    if (!parts[i]) return path;
    if (!basename_ok(parts[i], FALSE, error)) { g_free(path); return NULL; }
    char *next = g_build_filename(path, parts[i], NULL);
    g_free(path); path = next;
  }
}

BuFiles *bu_files_new(const char *root, GError **error)
{
  if (!root || !*root) { failure(error, G_IO_ERROR_INVALID_ARGUMENT, "保存先を指定してください"); return NULL; }
  BuFiles *files = g_new0(BuFiles, 1);
  files->root = g_canonicalize_filename(root, NULL);
  files->relative = g_strdup("");
  g_autofree char *path = current_path(files, error);
  if (!path) { bu_files_free(files); return NULL; }
  return files;
}

BuFiles *bu_files_clone(const BuFiles *files)
{
  BuFiles *copy = g_new0(BuFiles, 1);
  copy->root = g_strdup(files->root); copy->relative = g_strdup(files->relative); return copy;
}
void bu_files_free(BuFiles *files)
{ if (files) { g_free(files->root); g_free(files->relative); g_free(files); } }
void bu_file_entry_free(gpointer data)
{
  BuFileEntry *entry = data;
  g_free(entry->name); g_free(entry->display_name); g_free(entry->content_type); g_free(entry);
}

char *bu_files_path(BuFiles *files, const char *name, GError **error)
{
  if (!basename_ok(name, FALSE, error)) return NULL;
  g_autofree char *directory = current_path(files, error);
  return directory ? g_build_filename(directory, name, NULL) : NULL;
}

static gint compare_entries(gconstpointer a, gconstpointer b)
{
  const BuFileEntry *left = *(BuFileEntry *const *)a, *right = *(BuFileEntry *const *)b;
  if ((left->type == G_FILE_TYPE_DIRECTORY) != (right->type == G_FILE_TYPE_DIRECTORY))
    return left->type == G_FILE_TYPE_DIRECTORY ? -1 : 1;
  return g_utf8_collate(left->display_name, right->display_name);
}

GPtrArray *bu_files_list(BuFiles *files, GError **error)
{
  g_autofree char *path = current_path(files, error);
  if (!path) return NULL;
  g_autoptr(GFile) directory = g_file_new_for_path(path);
  g_autoptr(GFileEnumerator) enumerator = g_file_enumerate_children(directory,
    "standard::name,standard::type,standard::content-type,standard::size,time::modified",
    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
  if (!enumerator) return NULL;
  GPtrArray *entries = g_ptr_array_new_with_free_func(bu_file_entry_free);
  for (;;) {
    g_autoptr(GError) local_error = NULL;
    g_autoptr(GFileInfo) info = g_file_enumerator_next_file(enumerator, NULL, &local_error);
    if (local_error) { g_propagate_error(error, g_steal_pointer(&local_error)); g_ptr_array_unref(entries); return NULL; }
    if (!info) break;
    const char *name = g_file_info_get_name(info);
    if (g_str_equal(name, BU_FILES_TRASH)) continue;
    if (entries->len >= 10000) {
      failure(error, G_IO_ERROR_TOO_MANY_OPEN_FILES, "一つのフォルダは10,000件以内にしてください");
      g_ptr_array_unref(entries); return NULL;
    }
    BuFileEntry *entry = g_new0(BuFileEntry, 1);
    entry->name = g_strdup(name); entry->display_name = g_filename_display_name(name);
    entry->type = g_file_info_get_file_type(info);
    entry->content_type = g_strdup(g_file_info_get_content_type(info));
    entry->size = g_file_info_get_size(info);
    entry->modified = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    g_ptr_array_add(entries, entry);
  }
  g_ptr_array_sort(entries, compare_entries); return entries;
}

static GFileInfo *entry_info(BuFiles *files, const char *name, char **path, GError **error)
{
  *path = bu_files_path(files, name, error);
  if (!*path) return NULL;
  g_autoptr(GFile) file = g_file_new_for_path(*path);
  return g_file_query_info(file, "standard::type,standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
}

gboolean bu_files_enter(BuFiles *files, const char *name, GError **error)
{
  g_autofree char *path = NULL;
  g_autoptr(GFileInfo) info = entry_info(files, name, &path, error);
  if (!info) return FALSE;
  if (g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY)
    return failure(error, G_IO_ERROR_NOT_DIRECTORY, "フォルダ以外やリンクには移動できません");
  char *relative = *files->relative ? g_build_filename(files->relative, name, NULL) : g_strdup(name);
  g_free(files->relative); files->relative = relative; return TRUE;
}

gboolean bu_files_parent(BuFiles *files)
{
  if (!*files->relative) return FALSE;
  char *parent = g_path_get_dirname(files->relative);
  g_free(files->relative); files->relative = g_str_equal(parent, ".") ? g_strdup("") : g_strdup(parent);
  g_free(parent); return TRUE;
}

gboolean bu_files_mkdir(BuFiles *files, const char *name, GError **error)
{
  if (!basename_ok(name, TRUE, error)) return FALSE;
  g_autofree char *path = bu_files_path(files, name, error);
  if (!path) return FALSE;
  if (g_mkdir(path, 0700) == 0) return TRUE;
  int saved = errno;
  g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved), "フォルダを作れません: %s", g_strerror(saved)); return FALSE;
}

gboolean bu_files_rename(BuFiles *files, const char *name, const char *new_name, GError **error)
{
  if (!basename_ok(new_name, TRUE, error)) return FALSE;
  g_autofree char *path = bu_files_path(files, name, error), *new_path = NULL;
  if (!path) return FALSE;
  new_path = bu_files_path(files, new_name, error);
  if (!new_path) return FALSE;
  if (g_str_equal(path, new_path)) return TRUE;
  g_autoptr(GFile) source = g_file_new_for_path(path), destination = g_file_new_for_path(new_path);
  return g_file_move(source, destination, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE,
                     NULL, NULL, NULL, error);
}

gboolean bu_files_transfer(BuFiles *source, const char *name, BuFiles *destination, gboolean move, GError **error)
{
  g_autofree char *path = NULL, *new_path = NULL;
  g_autoptr(GFileInfo) info = entry_info(source, name, &path, error);
  if (!info) return FALSE;
  if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR)
    return failure(error, G_IO_ERROR_NOT_SUPPORTED, "コピー・切取りは通常のファイルを選んでください");
  new_path = bu_files_path(destination, name, error);
  if (!new_path) return FALSE;
  if (g_str_equal(path, new_path)) return failure(error, G_IO_ERROR_EXISTS, "同じ場所へは貼り付けられません");
  g_autoptr(GFile) from = g_file_new_for_path(path), to = g_file_new_for_path(new_path);
  if (move) return g_file_move(from, to, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, error);
  return g_file_copy(from, to, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, error);
}

char *bu_files_trash(BuFiles *files, const char *name, GError **error)
{
  g_autofree char *source_path = bu_files_path(files, name, error);
  if (!source_path) return NULL;
  /* The trash belongs to this root, so SD files never move to another filesystem. */
  g_autofree char *directory = g_build_filename(files->root, BU_FILES_TRASH, NULL);
  if (g_mkdir(directory, 0700) < 0 && errno != EEXIST) {
    int saved = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved), "ごみ箱を作れません: %s", g_strerror(saved)); return NULL;
  }
  GStatBuf status;
  if (g_lstat(directory, &status) < 0 || !S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode)) {
    failure(error, G_IO_ERROR_NOT_DIRECTORY, "ごみ箱が通常のフォルダではありません"); return NULL;
  }
  g_autofree char *id = g_uuid_string_random();
  g_autofree char *container = g_build_filename(directory, id, NULL);
  if (g_mkdir(container, 0700) < 0) {
    int saved = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved), "ごみ箱を用意できません: %s", g_strerror(saved)); return NULL;
  }
  char *destination_path = g_build_filename(container, name, NULL);
  g_autoptr(GFile) source = g_file_new_for_path(source_path), destination = g_file_new_for_path(destination_path);
  if (!g_file_move(source, destination, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE, NULL, NULL, NULL, error)) {
    g_rmdir(container); g_free(destination_path); return NULL;
  }
  return destination_path;
}

char *bu_files_read_text(BuFiles *files, const char *name, GError **error)
{
  g_autofree char *path = NULL;
  g_autoptr(GFileInfo) info = entry_info(files, name, &path, error);
  if (!info) return NULL;
  if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR || g_file_info_get_size(info) > BU_FILES_TEXT_LIMIT) {
    failure(error, G_IO_ERROR_NOT_SUPPORTED, "テキスト表示は128KiB以内の通常ファイルに対応します"); return NULL;
  }
  g_autoptr(GFile) file = g_file_new_for_path(path);
  g_autoptr(GFileInputStream) stream = g_file_read(file, NULL, error);
  if (!stream) return NULL;
  char *text = g_malloc(BU_FILES_TEXT_LIMIT + 2);
  gsize size = 0;
  if (!g_input_stream_read_all(G_INPUT_STREAM(stream), text, BU_FILES_TEXT_LIMIT + 1, &size, NULL, error)) {
    g_free(text); return NULL;
  }
  if (size > BU_FILES_TEXT_LIMIT || memchr(text, 0, size) || !g_utf8_validate(text, size, NULL)) {
    g_free(text); failure(error, G_IO_ERROR_INVALID_DATA, "UTF-8のテキストとして表示できません"); return NULL;
  }
  text[size] = '\0'; return text;
}
