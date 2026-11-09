/* SPDX-License-Identifier: MIT */
#include "note-store.h"
#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

static gboolean valid_text(const char *text, GError **error)
{
  if (!text || !g_utf8_validate(text, -1, NULL) || strlen(text) > BU_NOTE_MAX_BYTES) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "メモはUTF-8・16KiB以内で保存してください");
    return FALSE;
  }
  return TRUE;
}

static GFile *note_file(BuNoteStore *store, const char *id, GError **error)
{
  if (!id || !g_uuid_string_is_valid(id)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "メモの識別子が不正です");
    return NULL;
  }
  g_autofree char *name = g_strconcat(id, ".txt", NULL);
  g_autofree char *path = g_build_filename(store->directory, name, NULL);
  return g_file_new_for_path(path);
}

static char *revision(const char *text, const char *etag)
{
  g_autofree char *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
  return g_strconcat(digest, ":", etag ? etag : "", NULL);
}

static char *read_file(GFile *file, gsize limit, char **etag, GError **error)
{
  g_autoptr(GFileInfo) info = g_file_query_info(file,
    G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
  if (!info) return NULL;
  if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR || g_file_info_get_size(info) > (goffset)limit) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "対応する大きさの通常ファイルではありません");
    return NULL;
  }
  g_autoptr(GFileInputStream) stream = g_file_read(file, NULL, error);
  if (!stream) return NULL;
  /* Bound even a file that grows after the metadata check. */
  char *contents = g_malloc(limit + 2);
  gsize count = 0;
  if (!g_input_stream_read_all(G_INPUT_STREAM(stream), contents, limit + 1, &count, NULL, error)) {
    g_free(contents); return NULL;
  }
  if (count > limit || memchr(contents, 0, count) || !g_utf8_validate(contents, count, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "UTF-8のテキストとして読み込めません");
    g_free(contents); return NULL;
  }
  contents[count] = 0;
  if (etag) {
    g_autoptr(GFileInfo) stream_info = g_file_input_stream_query_info(stream, G_FILE_ATTRIBUTE_ETAG_VALUE, NULL, error);
    if (!stream_info) { g_free(contents); return NULL; }
    const char *file_etag = g_file_info_get_etag(stream_info);
    if (!file_etag) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "変更確認に対応していない保存先です");
      g_free(contents); return NULL;
    }
    *etag = revision(contents, file_etag);
  }
  return contents;
}

BuNoteStore *bu_note_store_new(const char *directory, GError **error)
{
  g_autofree char *path = directory ? g_canonicalize_filename(directory, NULL) :
    g_build_filename(g_get_user_data_dir(), "bananui-utils", "notes", NULL);
  if (g_mkdir_with_parents(path, 0700) < 0) {
    int saved_errno = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno), "保存先を作れません: %s", g_strerror(saved_errno));
    return NULL;
  }
  BuNoteStore *store = g_new0(BuNoteStore, 1);
  store->directory = g_steal_pointer(&path);
  return store;
}

void bu_note_store_free(BuNoteStore *store) { if (store) { g_free(store->directory); g_free(store); } }
void bu_note_summary_free(gpointer data)
{
  BuNoteSummary *summary = data;
  g_free(summary->id); g_free(summary->title); g_free(summary);
}

char *bu_note_store_read(BuNoteStore *store, const char *id, char **etag, GError **error)
{
  g_autoptr(GFile) file = note_file(store, id, error);
  return file ? read_file(file, BU_NOTE_MAX_BYTES, etag, error) : NULL;
}

static gint compare_notes(gconstpointer a, gconstpointer b)
{
  const BuNoteSummary *left = *(BuNoteSummary * const *)a, *right = *(BuNoteSummary * const *)b;
  if (left->modified != right->modified) return left->modified > right->modified ? -1 : 1;
  return g_strcmp0(left->id, right->id);
}

GPtrArray *bu_note_store_list(BuNoteStore *store, GError **error)
{
  g_autoptr(GFile) directory = g_file_new_for_path(store->directory);
  g_autoptr(GFileEnumerator) files = g_file_enumerate_children(directory,
    G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
  if (!files) return NULL;
  GPtrArray *notes = g_ptr_array_new_with_free_func(bu_note_summary_free);
  for (;;) {
    g_autoptr(GFileInfo) info = g_file_enumerator_next_file(files, NULL, error);
    if (!info) break;
    const char *name = g_file_info_get_name(info);
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR || strlen(name) != 40 || !g_str_has_suffix(name, ".txt")) continue;
    g_autofree char *id = g_strndup(name, 36);
    if (!g_uuid_string_is_valid(id)) continue;
    BuNoteSummary *entry = g_new0(BuNoteSummary, 1);
    entry->id = g_strdup(id);
    entry->modified = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    g_autoptr(GError) read_error = NULL;
    g_autofree char *text = bu_note_store_read(store, id, NULL, &read_error);
    entry->readable = text != NULL;
    entry->title = g_strdup(text ? "無題" : "読み込めないメモ");
    if (text) {
      g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
      for (guint i = 0; lines[i]; i++) {
        const char *line = g_strstrip(lines[i]);
        if (!line[0]) continue;
        g_free(entry->title);
        entry->title = g_utf8_substring(line, 0, MIN(48, g_utf8_strlen(line, -1)));
        break;
      }
    }
    g_ptr_array_add(notes, entry);
  }
  if (error && *error) { g_ptr_array_unref(notes); return NULL; }
  g_ptr_array_sort(notes, compare_notes);
  return notes;
}

gboolean bu_note_store_save(BuNoteStore *store, const char *id, const char *text,
                            const char *etag, char **new_etag, GError **error)
{
  if (!valid_text(text, error)) return FALSE;
  g_autoptr(GFile) file = note_file(store, id, error);
  if (!file) return FALSE;
  if (etag && etag[0] && !g_file_query_exists(file, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "元のメモが削除されています");
    return FALSE;
  }
  if (!etag || !etag[0]) {
    if (g_file_query_exists(file, NULL)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "同じ識別子のメモがあります");
      return FALSE;
    }
  } else {
    /* Local filesystem etags may have timestamp granularity coarser than two
     * quick saves. Include content, so different text never matches by time. */
    g_autofree char *current_etag = NULL;
    g_autofree char *current = read_file(file, BU_NOTE_MAX_BYTES, &current_etag, error);
    if (!current) return FALSE;
    if (g_strcmp0(etag, current_etag)) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG, "元のメモが変更されています");
      return FALSE;
    }
  }
  g_autofree char *file_etag = NULL;
  if (!g_file_replace_contents(file, text, strlen(text), etag && etag[0] ? etag + 65 : NULL,
      FALSE, G_FILE_CREATE_PRIVATE, &file_etag, NULL, error)) return FALSE;
  if (new_etag) *new_etag = revision(text, file_etag);
  return TRUE;
}

gboolean bu_note_store_trash(BuNoteStore *store, const char *id, GError **error)
{
  g_autoptr(GFile) file = note_file(store, id, error);
  if (!file) return FALSE;
  g_autofree char *directory = g_build_filename(store->directory, ".trash", NULL);
  if (g_mkdir_with_parents(directory, 0700) < 0) {
    int saved_errno = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno), "ゴミ箱を作れません: %s", g_strerror(saved_errno));
    return FALSE;
  }
  g_autofree char *suffix = g_uuid_string_random();
  g_autofree char *name = g_strdup_printf("%s-%s.txt", id, suffix);
  g_autofree char *path = g_build_filename(directory, name, NULL);
  g_autoptr(GFile) destination = g_file_new_for_path(path);
  return g_file_move(file, destination, G_FILE_COPY_NONE, NULL, NULL, NULL, error);
}

static GFile *draft_file(BuNoteStore *store)
{
  g_autofree char *path = g_build_filename(store->directory, ".draft.ini", NULL);
  return g_file_new_for_path(path);
}

gboolean bu_note_store_save_draft(BuNoteStore *store, const BuDraft *draft, GError **error)
{
  if (!draft || !draft->id || !g_uuid_string_is_valid(draft->id) || !valid_text(draft->text, error) || !valid_text(draft->original, error)) {
    if (error && !*error) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "下書きが不正です");
    return FALSE;
  }
  g_autoptr(GKeyFile) key = g_key_file_new();
  g_key_file_set_string(key, "draft", "id", draft->id);
  g_key_file_set_string(key, "draft", "text", draft->text);
  g_key_file_set_string(key, "draft", "original", draft->original);
  g_key_file_set_string(key, "draft", "etag", draft->etag ? draft->etag : "");
  gsize size;
  g_autofree char *contents = g_key_file_to_data(key, &size, NULL);
  g_autoptr(GFile) file = draft_file(store);
  return g_file_replace_contents(file, contents, size, NULL, FALSE, G_FILE_CREATE_PRIVATE, NULL, NULL, error);
}

BuDraft *bu_note_store_load_draft(BuNoteStore *store, GError **error)
{
  g_autoptr(GFile) file = draft_file(store);
  g_autoptr(GError) local_error = NULL;
  g_autofree char *contents = read_file(file, BU_NOTE_MAX_BYTES * 4 + 1024, NULL, &local_error);
  if (!contents) {
    if (!g_error_matches(local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) g_propagate_error(error, g_steal_pointer(&local_error));
    return NULL;
  }
  g_autoptr(GKeyFile) key = g_key_file_new();
  if (!g_key_file_load_from_data(key, contents, -1, G_KEY_FILE_NONE, error)) return NULL;
  g_autoptr(BuDraft) draft = g_new0(BuDraft, 1);
  draft->id = g_key_file_get_string(key, "draft", "id", NULL);
  draft->text = g_key_file_get_string(key, "draft", "text", NULL);
  draft->original = g_key_file_get_string(key, "draft", "original", NULL);
  draft->etag = g_key_file_get_string(key, "draft", "etag", NULL);
  if (!draft->id || !g_uuid_string_is_valid(draft->id) || !draft->etag ||
      !valid_text(draft->text, NULL) || !valid_text(draft->original, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "下書きの内容が不正です");
    return NULL;
  }
  return g_steal_pointer(&draft);
}

gboolean bu_note_store_clear_draft(BuNoteStore *store, GError **error)
{
  g_autoptr(GFile) file = draft_file(store);
  g_autoptr(GError) local_error = NULL;
  if (g_file_delete(file, NULL, &local_error) || g_error_matches(local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) return TRUE;
  g_propagate_error(error, g_steal_pointer(&local_error));
  return FALSE;
}

void bu_draft_free(BuDraft *draft)
{
  if (!draft) return;
  g_free(draft->id); g_free(draft->text); g_free(draft->original); g_free(draft->etag); g_free(draft);
}
