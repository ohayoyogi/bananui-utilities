/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "note-store.h"
#include "calculator.h"
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <string.h>

static char *fixture;
static void remove_fixture(const char *path)
{
  GStatBuf file_status;
  if (g_lstat(path, &file_status) < 0) return;
  if (!S_ISDIR(file_status.st_mode)) { g_unlink(path); return; }
  g_autoptr(GDir) directory = g_dir_open(path, 0, NULL);
  const char *name;
  while ((name = g_dir_read_name(directory))) {
    g_autofree char *child = g_build_filename(path, name, NULL);
    remove_fixture(child);
  }
  g_rmdir(path);
}

static BuNoteStore *store_for(const char *name)
{
  g_autofree char *directory = g_build_filename(fixture, name, NULL);
  g_autoptr(GError) error = NULL;
  BuNoteStore *store = bu_note_store_new(directory, &error);
  g_assert_no_error(error); g_assert_nonnull(store);
  return store;
}

static void test_roundtrip(void)
{
  g_autoptr(BuNoteStore) store = store_for("roundtrip");
  g_autofree char *id = g_uuid_string_random(), *etag = NULL;
  const char *body = "買い物\n牛乳・パン\n絵文字 🐈 と backslash \\";
  g_autoptr(GError) error = NULL;
  g_assert_true(bu_note_store_save(store, id, body, NULL, &etag, &error));
  g_assert_no_error(error); g_assert_nonnull(etag);
  g_autoptr(BuNoteStore) reopened = bu_note_store_new(store->directory, &error);
  g_autofree char *loaded_etag = NULL;
  g_autofree char *text = bu_note_store_read(reopened, id, &loaded_etag, &error);
  g_assert_no_error(error); g_assert_cmpstr(text, ==, body);
  g_assert_cmpstr(etag, ==, loaded_etag);
  g_autoptr(GPtrArray) notes = bu_note_store_list(reopened, &error);
  g_assert_no_error(error); g_assert_cmpuint(notes->len, ==, 1);
  g_assert_cmpstr(((BuNoteSummary *)g_ptr_array_index(notes, 0))->title, ==, "買い物");
  g_autofree char *filename = g_strconcat(id, ".txt", NULL);
  g_autofree char *path = g_build_filename(store->directory, filename, NULL);
  GStatBuf file_status;
  g_assert_cmpint(g_stat(path, &file_status), ==, 0);
  g_assert_cmpint(file_status.st_mode & 0777, ==, 0600);
}

static void test_conflict(void)
{
  g_autoptr(BuNoteStore) store = store_for("conflict");
  g_autofree char *id = g_uuid_string_random(), *first = NULL, *second = NULL;
  g_autoptr(GError) error = NULL;
  g_assert_true(bu_note_store_save(store, id, "最初", NULL, &first, &error));
  g_assert_true(bu_note_store_save(store, id, "別の編集者から更新しました", first, &second, &error));
  g_assert_false(bu_note_store_save(store, id, "古い内容", first, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG); g_clear_error(&error);
  g_autofree char *text = bu_note_store_read(store, id, NULL, &error);
  g_assert_no_error(error); g_assert_cmpstr(text, ==, "別の編集者から更新しました");
  g_assert_false(bu_note_store_save(store, id, "新規のふりをして上書き", NULL, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS); g_clear_error(&error);
  g_assert_true(bu_note_store_trash(store, id, &error));
  g_assert_false(bu_note_store_save(store, id, "削除された後の保存", second, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

static void test_validation(void)
{
  g_autoptr(BuNoteStore) store = store_for("validation");
  g_autofree char *id = g_uuid_string_random();
  g_autoptr(GError) error = NULL;
  g_assert_false(bu_note_store_save(store, "../outside", "text", NULL, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
  g_autofree char *large = g_strnfill(BU_NOTE_MAX_BYTES + 1, 'a');
  g_assert_false(bu_note_store_save(store, id, large, NULL, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
  g_assert_false(bu_note_store_save(store, id, "\xff", NULL, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
  g_assert_true(bu_note_store_save(store, id, "", NULL, NULL, &error));
  g_assert_no_error(error);
  g_autoptr(GPtrArray) entries = bu_note_store_list(store, &error);
  g_assert_cmpstr(((BuNoteSummary *)g_ptr_array_index(entries, 0))->title, ==, "無題");
}

static void test_trash_and_draft(void)
{
  g_autoptr(BuNoteStore) store = store_for("draft");
  g_autofree char *id = g_uuid_string_random(), *etag = NULL;
  g_autoptr(GError) error = NULL;
  g_assert_true(bu_note_store_save(store, id, "本文", NULL, &etag, &error));
  BuDraft draft = { id, "編集中\n=次の行\\ひらがな🐈", "本文", etag };
  g_assert_true(bu_note_store_save_draft(store, &draft, &error));
  g_autoptr(BuDraft) loaded = bu_note_store_load_draft(store, &error);
  g_assert_no_error(error); g_assert_nonnull(loaded);
  g_assert_cmpstr(loaded->text, ==, draft.text);
  g_assert_cmpstr(loaded->id, ==, id);
  g_assert_cmpstr(loaded->etag, ==, etag);
  g_assert_cmpstr(loaded->original, ==, "本文");
  g_assert_true(bu_note_store_clear_draft(store, &error));
  g_assert_true(bu_note_store_clear_draft(store, &error));
  g_assert_null(bu_note_store_load_draft(store, &error)); g_assert_no_error(error);
  g_assert_true(bu_note_store_trash(store, id, &error));
  g_autoptr(GPtrArray) entries = bu_note_store_list(store, &error);
  g_assert_cmpuint(entries->len, ==, 0);
  g_autofree char *trash = g_build_filename(store->directory, ".trash", NULL);
  g_autoptr(GDir) directory = g_dir_open(trash, 0, &error);
  const char *name = g_dir_read_name(directory);
  g_assert_nonnull(name);
  g_autofree char *path = g_build_filename(trash, name, NULL), *text = NULL;
  g_assert_true(g_file_get_contents(path, &text, NULL, &error));
  g_assert_cmpstr(text, ==, "本文");
}

static void test_bad_files(void)
{
  g_autoptr(BuNoteStore) store = store_for("bad-files");
  g_autofree char *id = g_uuid_string_random();
  g_autofree char *name = g_strconcat(id, ".txt", NULL);
  g_autofree char *path = g_build_filename(store->directory, name, NULL);
  const char bad[] = { 'a', 0, 'b' };
  g_assert_true(g_file_set_contents(path, bad, sizeof bad, NULL));
  g_autoptr(GError) error = NULL;
  g_assert_null(bu_note_store_read(store, id, NULL, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
  g_autoptr(GPtrArray) entries = bu_note_store_list(store, &error);
  g_assert_cmpuint(entries->len, ==, 1);
  g_assert_false(((BuNoteSummary *)g_ptr_array_index(entries, 0))->readable);
  g_autofree char *draft = g_build_filename(store->directory, ".draft.ini", NULL);
  g_assert_true(g_file_set_contents(draft, "[draft]\nid=../escape\n", -1, NULL));
  g_assert_null(bu_note_store_load_draft(store, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void calculate(BuCalculator *calc, const char *sequence)
{
  bu_calculator_clear(calc);
  for (const char *p = sequence; *p; p++) g_assert_true(bu_calculator_key(calc, *p));
}

static void test_arithmetic(void)
{
  const char *cases[][2] = {
    {"2+3*4=", "20"}, {"2+*3=", "6"}, {"2+3===", "11"},
    {"0.1+0.2=", "0.3"}, {"2S*3=", "-6"}, {"8/4=+3=", "5"},
    {"12\b3+2=", "15"}, {"5=S+3=", "-2"}, {"2+=", "4"},
    {"9999999999999", "999999999999"}, {"9=2+3=", "5"},
  };
  BuCalculator calc;
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    calculate(&calc, cases[i][0]);
    g_assert_false(calc.error);
    g_assert_cmpstr(bu_calculator_display(&calc), ==, cases[i][1]);
  }
  calculate(&calc, "4*S2="); g_assert_cmpstr(calc.entry, ==, "-8");
  calculate(&calc, "3/0="); g_assert_true(calc.error);
  g_assert_true(bu_calculator_key(&calc, '7')); g_assert_false(calc.error);
  g_assert_cmpstr(calc.entry, ==, "7");
}

static void test_calculator_limits(void)
{
  BuCalculator calc;
  calculate(&calc, "9999999999999");
  g_assert_true(calc.digit_limit); g_assert_cmpuint(strlen(calc.entry), ==, 12);
  calculate(&calc, "999999999999*999999999999=");
  g_assert_nonnull(strchr(calc.entry, 'e'));
  bu_calculator_key(&calc, '\b'); g_assert_cmpstr(calc.entry, ==, "0");
  calculate(&calc, "999999999999*999999999999=");
  for (guint i = 0; i < 40 && !calc.error; i++) bu_calculator_key(&calc, '=');
  g_assert_true(calc.error);
  bu_calculator_key(&calc, 'C'); g_assert_false(calc.error); g_assert_cmpstr(calc.entry, ==, "0");
  g_assert_false(bu_calculator_key(&calc, 'x'));
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  fixture = g_dir_make_tmp("bananui-utilities-test-XXXXXX", NULL);
  g_assert_nonnull(fixture);
  g_test_add_func("/notes/roundtrip-private-files", test_roundtrip);
  g_test_add_func("/notes/external-change-conflict", test_conflict);
  g_test_add_func("/notes/validation", test_validation);
  g_test_add_func("/notes/trash-draft-recovery", test_trash_and_draft);
  g_test_add_func("/notes/bad-files", test_bad_files);
  g_test_add_func("/calculator/arithmetic", test_arithmetic);
  g_test_add_func("/calculator/limits", test_calculator_limits);
  int result = g_test_run();
  remove_fixture(fixture); g_free(fixture);
  return result;
}
