/* SPDX-License-Identifier: MIT */
#include "file-store.h"
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

static char *fixture;
static void clean(const char *path)
{
  GStatBuf status;
  if (g_lstat(path, &status) < 0) return;
  if (!S_ISDIR(status.st_mode)) { g_unlink(path); return; }
  g_autoptr(GDir) directory = g_dir_open(path, 0, NULL);
  const char *name;
  while ((name = g_dir_read_name(directory))) {
    g_autofree char *child = g_build_filename(path, name, NULL); clean(child);
  }
  g_rmdir(path);
}
static BuFiles *store(const char *name)
{
  g_autofree char *path = g_build_filename(fixture, name, NULL);
  g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
  g_autoptr(GError) error = NULL;
  BuFiles *files = bu_files_new(path, &error);
  g_assert_no_error(error); g_assert_nonnull(files); return files;
}
static void put(BuFiles *files, const char *name, const char *text)
{
  g_autofree char *path = bu_files_path(files, name, NULL);
  g_assert_true(g_file_set_contents(path, text, -1, NULL));
}
static void navigation(void)
{
  g_autoptr(BuFiles) files = store("navigation");
  g_autoptr(GError) error = NULL;
  put(files, "z.txt", "日本語\n本文");
  g_assert_true(bu_files_mkdir(files, "フォルダ", &error));
  g_assert_no_error(error);
  g_autoptr(GPtrArray) entries = bu_files_list(files, &error);
  g_assert_no_error(error); g_assert_cmpuint(entries->len, ==, 2);
  BuFileEntry *first = g_ptr_array_index(entries, 0);
  g_assert_cmpint(first->type, ==, G_FILE_TYPE_DIRECTORY);
  g_assert_cmpstr(first->display_name, ==, "フォルダ");
  g_assert_true(bu_files_enter(files, first->name, &error));
  put(files, "内側.txt", "テスト");
  g_autoptr(BuFiles) copy = bu_files_clone(files);
  g_assert_cmpstr(copy->relative, ==, "フォルダ");
  g_assert_true(bu_files_parent(files));
  g_assert_false(bu_files_parent(files));
  g_autofree char *text = bu_files_read_text(files, "z.txt", &error);
  g_assert_no_error(error); g_assert_cmpstr(text, ==, "日本語\n本文");
}
static void names_and_links(void)
{
  g_autoptr(BuFiles) files = store("links");
  g_autoptr(GError) error = NULL;
  const char *invalid[] = { "", ".", "..", "../outside", "a/b", BU_FILES_TRASH, "bad\nname", "\xff" };
  for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
    g_assert_false(bu_files_mkdir(files, invalid[i], &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME); g_clear_error(&error);
  }
  g_autofree char *outside = g_build_filename(fixture, "outside", NULL);
  g_assert_cmpint(g_mkdir(outside, 0700), ==, 0);
  g_autofree char *link = g_build_filename(files->root, "link", NULL);
  g_assert_cmpint(symlink(outside, link), ==, 0);
  g_assert_false(bu_files_enter(files, "link", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY); g_clear_error(&error);
  g_assert_null(bu_files_read_text(files, "link", &error)); g_clear_error(&error);
  g_autoptr(BuFiles) alias = bu_files_new(link, &error);
  g_assert_null(alias); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY);
}
static void replaced_parent(void)
{
  g_autoptr(BuFiles) files = store("replaced");
  g_autoptr(GError) error = NULL;
  g_assert_true(bu_files_mkdir(files, "sub", &error));
  g_assert_true(bu_files_enter(files, "sub", &error));
  g_autofree char *sub = g_build_filename(files->root, "sub", NULL);
  g_autofree char *outside = g_build_filename(fixture, "replacement-target", NULL);
  g_assert_cmpint(g_mkdir(outside, 0700), ==, 0);
  g_assert_cmpint(g_rmdir(sub), ==, 0); g_assert_cmpint(symlink(outside, sub), ==, 0);
  g_assert_false(bu_files_mkdir(files, "must-not-exist", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY);
  g_autofree char *unwanted = g_build_filename(outside, "must-not-exist", NULL);
  g_assert_false(g_file_test(unwanted, G_FILE_TEST_EXISTS));
}
static void transfers(void)
{
  g_autoptr(BuFiles) from = store("transfer-from"), to = store("transfer-to");
  g_autoptr(GError) error = NULL;
  put(from, "a ; $(never).txt", "original");
  g_assert_true(bu_files_transfer(from, "a ; $(never).txt", to, FALSE, &error));
  g_assert_no_error(error);
  g_assert_false(bu_files_transfer(from, "a ; $(never).txt", to, TRUE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS); g_clear_error(&error);
  g_autofree char *text = bu_files_read_text(from, "a ; $(never).txt", &error);
  g_assert_no_error(error); g_assert_cmpstr(text, ==, "original");
  g_assert_true(bu_files_rename(to, "a ; $(never).txt", "別名.txt", &error));
  g_assert_true(bu_files_transfer(from, "a ; $(never).txt", to, TRUE, &error));
  g_autofree char *source = bu_files_path(from, "a ; $(never).txt", &error);
  g_assert_false(g_file_test(source, G_FILE_TEST_EXISTS));
  g_assert_false(bu_files_rename(to, "別名.txt", "a ; $(never).txt", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS); g_clear_error(&error);
  g_assert_true(bu_files_mkdir(from, "directory", &error));
  g_assert_false(bu_files_transfer(from, "directory", to, FALSE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}
static void trash(void)
{
  g_autoptr(BuFiles) files = store("trash");
  g_autoptr(GError) error = NULL;
  put(files, "保存.txt", "keep");
  g_autofree char *first = bu_files_trash(files, "保存.txt", &error);
  g_assert_no_error(error); g_assert_nonnull(first);
  put(files, "保存.txt", "second");
  g_autofree char *second = bu_files_trash(files, "保存.txt", &error);
  g_assert_no_error(error); g_assert_cmpstr(first, !=, second);
  g_autofree char *text = NULL;
  g_assert_true(g_file_get_contents(first, &text, NULL, &error));
  g_assert_cmpstr(text, ==, "keep");
  g_autoptr(GPtrArray) entries = bu_files_list(files, &error);
  g_assert_cmpuint(entries->len, ==, 0);
  g_assert_true(bu_files_mkdir(files, "parent", &error));
  g_assert_true(bu_files_enter(files, "parent", &error));
  put(files, "inside.txt", "child"); bu_files_parent(files);
  g_autofree char *directory = bu_files_trash(files, "parent", &error);
  g_autofree char *child = g_build_filename(directory, "inside.txt", NULL);
  g_assert_no_error(error); g_assert_true(g_file_test(child, G_FILE_TEST_IS_REGULAR));
}
static void trash_link(void)
{
  g_autoptr(BuFiles) files = store("trash-link");
  g_autoptr(GError) error = NULL;
  g_autofree char *outside = g_build_filename(fixture, "trash-outside", NULL);
  g_assert_cmpint(g_mkdir(outside, 0700), ==, 0);
  g_autofree char *trash_path = g_build_filename(files->root, BU_FILES_TRASH, NULL);
  g_assert_cmpint(symlink(outside, trash_path), ==, 0);
  put(files, "a.txt", "still here");
  g_assert_null(bu_files_trash(files, "a.txt", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY); g_clear_error(&error);
  g_autofree char *text = bu_files_read_text(files, "a.txt", &error);
  g_assert_cmpstr(text, ==, "still here");
}
static void text_limits(void)
{
  g_autoptr(BuFiles) files = store("text");
  g_autoptr(GError) error = NULL;
  put(files, "invalid", "\xff");
  g_assert_null(bu_files_read_text(files, "invalid", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
  g_autofree char *large = g_strnfill(BU_FILES_TEXT_LIMIT + 1, 'a');
  put(files, "large", large);
  g_assert_null(bu_files_read_text(files, "large", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED); g_clear_error(&error);
  g_autofree char *path = bu_files_path(files, "nul", &error);
  g_assert_true(g_file_set_contents(path, "a\0b", 3, &error));
  g_assert_null(bu_files_read_text(files, "nul", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}
int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  fixture = g_dir_make_tmp("bananui-files-test-XXXXXX", NULL);
  g_assert_nonnull(fixture);
  g_test_add_func("/files/navigation-utf8", navigation);
  g_test_add_func("/files/names-and-links", names_and_links);
  g_test_add_func("/files/replaced-parent", replaced_parent);
  g_test_add_func("/files/transfer-no-overwrite", transfers);
  g_test_add_func("/files/trash-preserves-data", trash);
  g_test_add_func("/files/trash-link", trash_link);
  g_test_add_func("/files/text-limits", text_limits);
  int result = g_test_run(); clean(fixture); g_free(fixture); return result;
}
