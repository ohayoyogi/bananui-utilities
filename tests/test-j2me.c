/* SPDX-License-Identifier: MIT */
#include "j2me.h"
#include <string.h>
#include <glib/gstdio.h>

static const char *valid_output =
  "[ldev] hello protocol=1 pid=12\n"
  "[ldev] suite 42\tTest app\tVendor\t1.2\n"
  "[ldev] midlet 42\t1\tFirst\torg.example.First\n"
  "[ldev] midlet 42\t2\tSecond\torg.example.Second\n"
  "[ldev] midlets 42 2\n"
  "[ldev] suites 1\n"
  "[ldev] bye status=0\n";

static void test_parse_valid(void)
{
  g_autoptr(GError) error = NULL;
  guint skipped = 99;
  GPtrArray *list = bu_j2me_parse_list(valid_output, &skipped, &error);
  g_assert_cmpuint(skipped, ==, 0);
  g_assert_no_error(error); g_assert_nonnull(list); g_assert_cmpuint(list->len, ==, 1);
  BuJ2meSuite *s = g_ptr_array_index(list, 0);
  g_assert_cmpstr(s->id, ==, "42"); g_assert_cmpstr(s->name, ==, "Test app");
  g_assert_cmpuint(s->midlets->len, ==, 2);
  BuJ2meMidlet *m = g_ptr_array_index(s->midlets, 1);
  g_assert_cmpuint(m->ordinal, ==, 2); g_assert_cmpstr(m->klass, ==, "org.example.Second");
  g_ptr_array_unref(list);
}

static void test_parse_incomplete(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GString) incomplete = g_string_new(valid_output);
  g_assert_cmpuint(g_string_replace(incomplete, "[ldev] midlet 42\t2\tSecond\torg.example.Second\n", "", 1), ==, 1);
  GPtrArray *list = bu_j2me_parse_list(incomplete->str, NULL, &error);
  g_assert_null(list); g_assert_error(error, g_quark_from_static_string("bananui-j2me-error"), 1);
}

static void test_argv_boundaries(void)
{
  BuJ2meConfig config = { .executable = "/runtime path/micromidp", .lib_dir = "/data/lib",
                          .data_dir = "/data with spaces/suites", .cache_dir = "/tmp/cache" };
  g_autoptr(GError) error = NULL;
  GPtrArray *args = bu_j2me_build_argv(&config, "run", "123", 2, NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(args);
  gboolean saw_data = FALSE, saw_path_as_single_argument = FALSE, saw_ordinal = FALSE;
  for (guint i = 0; i + 1 < args->len; i++) {
    if (g_str_equal(g_ptr_array_index(args, i), "--data")) {
      saw_data = TRUE; saw_path_as_single_argument = g_str_equal(g_ptr_array_index(args, i + 1), config.data_dir);
    }
    if (g_str_equal(g_ptr_array_index(args, i), "--midlet"))
      saw_ordinal = g_str_equal(g_ptr_array_index(args, i + 1), "2");
  }
  g_assert_true(saw_data); g_assert_true(saw_path_as_single_argument); g_assert_true(saw_ordinal);
  g_ptr_array_unref(args);
}

static void assert_bad_catalog(const char *text)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) list = bu_j2me_parse_list(text, NULL, &error);
  g_assert_null(list);
  g_assert_error(error, g_quark_from_static_string("bananui-j2me-error"), 1);
}

typedef struct { const char *name, *needle, *replacement; } Mutation;
static const Mutation mutations[] = {
  { "missing-hello", "[ldev] hello protocol=1 pid=12\n", "" },
  { "wrong-protocol", "protocol=1", "protocol=2" },
  { "duplicate-hello", "[ldev] suite", "[ldev] hello protocol=1\n[ldev] suite" },
  { "zero-id", "suite 42", "suite 0" },
  { "id-alias", "suite 42", "suite 042" },
  { "negative-id", "suite 42", "suite -1" },
  { "overflow-id", "suite 42", "suite 2147483648" },
  { "huge-id", "suite 42", "suite 184467440737095516160000000" },
  { "invalid-id", "suite 42", "suite 42x" },
  { "duplicate-suite", "[ldev] suites 1", "[ldev] suite 42\tDuplicate\tVendor\t1\n[ldev] suites 1" },
  { "alias-suite", "[ldev] suites 1", "[ldev] suite 042\tDuplicate\tVendor\t1\n[ldev] suites 1" },
  { "missing-suite-trailer", "[ldev] midlets 42 2\n", "" },
  { "missing-total", "[ldev] suites 1\n", "" },
  { "missing-bye", "[ldev] bye status=0\n", "" },
  { "duplicate-trailer", "[ldev] midlets 42 2", "[ldev] midlets 42 2\n[ldev] midlets 42 2" },
  { "wrong-count", "midlets 42 2", "midlets 42 1" },
  { "count-overflow", "midlets 42 2", "midlets 42 4294967295" },
  { "empty-midlets", "midlets 42 2", "midlets 42 0" },
  { "too-many-midlets", "midlets 42 2", "midlets 42 51" },
  { "total-overflow", "suites 1", "suites 4294967296" },
  { "total-mismatch", "suites 1", "suites 2" },
  { "duplicate-ordinal", "midlet 42\t2", "midlet 42\t1" },
  { "ordinal-gap", "midlet 42\t2", "midlet 42\t3" },
  { "zero-ordinal", "midlet 42\t1", "midlet 42\t0" },
  { "ordinal-alias", "midlet 42\t1", "midlet 42\t01" },
  { "wrong-midlet-owner", "midlet 42\t1", "midlet 43\t1" },
  { "wrong-trailer-owner", "midlets 42 2", "midlets 43 2" },
  { "extra-field", "\tFirst\t", "\tFirst\textra\t" },
  { "empty-class", "org.example.First", "" },
  { "invalid-utf8", "First\t", "\xff\t" },
  { "premature-bye", "[ldev] suites 1", "[ldev] bye status=0\n[ldev] suites 1" },
  { "error-record", "[ldev] suites 1", "[ldev] error store unreadable\n[ldev] suites 1" },
  { "malformed-skip", "[ldev] suites 1", "[ldev] error suite 7: \n[ldev] suites 1" },
  { "skip-during-suite", "[ldev] midlets 42 2", "[ldev] error suite 7: properties unreadable\n[ldev] midlets 42 2" },
  { "failed-exit", "bye status=0", "bye status=3" },
  { "post-trailer-midlet", "[ldev] suites 1", "[ldev] midlet 42\t3\tLate\torg.Late\n[ldev] suites 1" },
  { "trailing-suite", "bye status=0\n", "bye status=0\n[ldev] suite 50\tLate\tV\t1\n" },
  { "trailing-total", "bye status=0\n", "bye status=0\n[ldev] suites 1\n" },
  { "duplicate-bye", "bye status=0\n", "bye status=0\n[ldev] bye status=0\n" },
};

static void test_mutation(gconstpointer data)
{
  const Mutation *m = data;
  const char *at = strstr(valid_output, m->needle);
  g_assert_nonnull(at);
  g_autofree char *prefix = g_strndup(valid_output, at - valid_output);
  g_autofree char *changed = g_strconcat(prefix, m->replacement, at + strlen(m->needle), NULL);
  assert_bad_catalog(changed);
}

static void test_catalog_boundaries(void)
{
  assert_bad_catalog(NULL);
  assert_bad_catalog("");
  g_autofree char *large = g_strnfill(2 * 1024 * 1024 + 1, 'x');
  assert_bad_catalog(large);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) empty = bu_j2me_parse_list(
    "diagnostic log\n[ldev] hello protocol=1\r\n[ldev] suites 0\r\n[ldev] bye status=0\r\n", NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(empty); g_assert_cmpuint(empty->len, ==, 0);

  g_autoptr(GString) text = g_string_new("[ldev] hello protocol=1\n");
  for (guint id = 1; id <= 2; id++) {
    g_string_append_printf(text, "[ldev] suite %u\t  日本語  \t Vendor \t 1.0 \n", id);
    for (guint n = 1; n <= 50; n++)
      g_string_append_printf(text, "[ldev] midlet %u\t%u\t 表示名 %u \torg.App%u\n", id, n, n, n);
    g_string_append_printf(text, "[ldev] midlets %u 50\n", id);
  }
  g_string_append(text, "[ldev] suites 2\n[ldev] bye status=0\n");
  g_autoptr(GPtrArray) list = bu_j2me_parse_list(text->str, NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(list); g_assert_cmpuint(list->len, ==, 2);
  BuJ2meSuite *suite = g_ptr_array_index(list, 0);
  g_assert_cmpstr(suite->name, ==, "  日本語  ");
  g_assert_cmpstr(suite->vendor, ==, " Vendor ");
  g_assert_cmpstr(suite->version, ==, " 1.0 ");
  BuJ2meMidlet *midlet = g_ptr_array_index(suite->midlets, 49);
  g_assert_cmpstr(midlet->name, ==, " 表示名 50 ");
}

static BuJ2meConfig fixture_config(void)
{
  return (BuJ2meConfig) { .executable = "/runtime path/$(fake);micromidp",
    .lib_dir = "/data/lib", .data_dir = "/data with spaces/suites", .cache_dir = "/tmp/cache" };
}

static void assert_bad_argv(const BuJ2meConfig *config, const char *op, const char *id, guint ordinal, const char *jar)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) args = bu_j2me_build_argv(config, op, id, ordinal, jar, &error);
  g_assert_null(args);
  g_assert_error(error, g_quark_from_static_string("bananui-j2me-error"), 2);
}

static const char *option(GPtrArray *args, const char *name)
{
  for (guint i = 0; i + 1 < args->len; i++)
    if (g_str_equal(g_ptr_array_index(args, i), name)) return g_ptr_array_index(args, i + 1);
  return NULL;
}

static void test_invalid_args(void)
{
  BuJ2meConfig c = fixture_config(), empty = {0};
  assert_bad_argv(NULL, "list", NULL, 0, NULL);
  assert_bad_argv(&empty, "install", NULL, 0, "https://example.test/app.jar");
  char **required[] = { &c.executable, &c.lib_dir, &c.data_dir, &c.cache_dir, &c.keymap, &c.font };
  for (guint i = 0; i < G_N_ELEMENTS(required); i++) {
    char *saved = *required[i];
    if (i < 4) { *required[i] = NULL; assert_bad_argv(&c, "list", NULL, 0, NULL); }
    *required[i] = ""; assert_bad_argv(&c, "list", NULL, 0, NULL);
    *required[i] = "relative/path"; assert_bad_argv(&c, "list", NULL, 0, NULL);
    *required[i] = saved;
  }
  const char *bad_ids[] = { NULL, "", "0", "01", "-1", "+1", "1x", " 1", "1 ", "2147483648", "184467440737095516160" };
  for (guint i = 0; i < G_N_ELEMENTS(bad_ids); i++) {
    assert_bad_argv(&c, "remove", bad_ids[i], 0, NULL);
    assert_bad_argv(&c, "run", bad_ids[i], 1, NULL);
  }
  assert_bad_argv(&c, NULL, NULL, 0, NULL);
  assert_bad_argv(&c, "unknown", NULL, 0, NULL);
  assert_bad_argv(&c, "list", "1", 0, NULL);
  assert_bad_argv(&c, "list", NULL, 1, NULL);
  assert_bad_argv(&c, "list", NULL, 0, "/tmp/a.jar");
  assert_bad_argv(&c, "remove", "1", 1, NULL);
  assert_bad_argv(&c, "remove", "1", 0, "/tmp/a.jar");
  assert_bad_argv(&c, "run", "1", 0, NULL);
  assert_bad_argv(&c, "run", "1", 51, NULL);
  assert_bad_argv(&c, "run", "1", G_MAXUINT, NULL);
  assert_bad_argv(&c, "run", "1", 1, "/tmp/a.jar");
  assert_bad_argv(&c, "install", NULL, 0, NULL);
  assert_bad_argv(&c, "install", NULL, 0, "https://example.test/app.jar");
  assert_bad_argv(&c, "install", NULL, 0, "file:///tmp/app.jar");
  assert_bad_argv(&c, "install", NULL, 0, "app.jar");
}

static void test_operation_args(void)
{
  BuJ2meConfig c = fixture_config();
  g_autoptr(GError) error = NULL;
  g_autofree char *dir = g_dir_make_tmp("j2me-test-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree char *jar = g_build_filename(dir, "app ' $(touch SHOULD_NOT_EXIST); 日本語.JAR", NULL);
  g_assert_true(g_file_set_contents(jar, "test fixture", -1, &error));
  g_assert_no_error(error);
  g_autoptr(GPtrArray) install = bu_j2me_build_argv(&c, "install", NULL, 0, jar, &error);
  g_assert_no_error(error); g_assert_nonnull(install);
  g_assert_cmpstr(option(install, "--display"), ==, "sim");
  g_assert_null(option(install, "--app-id"));
  g_assert_cmpstr(option(install, "--install-only"), ==, jar);
  g_assert_null(g_ptr_array_index(install, install->len - 1));
  assert_bad_argv(&c, "install", "1", 0, jar);
  assert_bad_argv(&c, "install", NULL, 1, jar);
  for (guint ordinal = 1; ordinal <= 50; ordinal += 49) {
    g_autoptr(GPtrArray) run = bu_j2me_build_argv(&c, "run", "2147483647", ordinal, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(run);
    g_assert_cmpstr(g_ptr_array_index(run, 0), ==, c.executable);
    g_assert_cmpstr(option(run, "--display"), ==, "wayland");
    g_assert_cmpstr(option(run, "--app-id"), ==, "org.bananui.J2ME");
    g_assert_cmpstr(option(run, "--suite"), ==, "2147483647");
  }
  g_autoptr(GPtrArray) list = bu_j2me_build_argv(&c, "list", NULL, 0, NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(list);
  g_assert_cmpstr(option(list, "--display"), ==, "sim"); g_assert_null(option(list, "--app-id"));
  g_autoptr(GPtrArray) removal = bu_j2me_build_argv(&c, "remove", "42", 0, NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(removal);
  g_assert_cmpstr(option(removal, "--remove-suite"), ==, "42"); g_assert_null(option(removal, "--app-id"));
  c.font = "/font path/日本語.bdf";
  g_autoptr(GPtrArray) with_font = bu_j2me_build_argv(&c, "run", "42", 1, NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(with_font);
  g_assert_cmpstr(option(with_font, "--font"), ==, c.font);
  g_assert_cmpint(g_remove(jar), ==, 0);
  assert_bad_argv(&c, "install", NULL, 0, jar);
  g_assert_cmpint(g_mkdir(jar, 0700), ==, 0);
  assert_bad_argv(&c, "install", NULL, 0, jar);
  g_assert_cmpint(g_rmdir(jar), ==, 0);
  g_autofree char *jad = g_build_filename(dir, "app.jad", NULL);
  g_assert_true(g_file_set_contents(jad, "not supported", -1, &error));
  assert_bad_argv(&c, "install", NULL, 0, jad);
  g_assert_cmpint(g_remove(jad), ==, 0); g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void test_completion(void)
{
  const char *valid[] = {
    "log\n[ldev] hello protocol=1 pid=9\n[ldev] state INSTALLING\n[ldev] installed suite 42\n[ldev] state QUIT\n[ldev] bye status=0\n",
    "[ldev] hello protocol=1\r\n[ldev] removed suite 42\r\n[ldev] bye status=0\r\n"
  };
  for (guint i = 0; i < G_N_ELEMENTS(valid); i++) {
    g_autoptr(GError) error = NULL;
    g_assert_true(bu_j2me_check_completion(valid[i], i ? "remove" : "install", i ? "42" : NULL, &error));
    g_assert_no_error(error);
  }
  const char *invalid[] = {
    NULL, "", "[ldev] installed suite 42\n[ldev] bye status=0\n",
    "[ldev] hello protocol=2\n[ldev] installed suite 42\n[ldev] bye status=0\n",
    "[ldev] hello protocol=1\n[ldev] installed suite 42\n",
    "[ldev] hello protocol=1\n[ldev] installed suite 0\n[ldev] bye status=0\n",
    "[ldev] hello protocol=1\n[ldev] installed suite 2147483648\n[ldev] bye status=0\n",
    "[ldev] hello protocol=1\n[ldev] installed suite 42extra\n[ldev] bye status=0\n",
    "[ldev] hello protocol=1\n[ldev] installed suite 42\n[ldev] installed suite 43\n[ldev] bye status=0\n",
    "[ldev] hello protocol=1\n[ldev] removed suite 42\n[ldev] bye status=0\n",
    "[ldev] hello protocol=1\n[ldev] installed suite 42\n[ldev] error install failed\n[ldev] bye status=0\n",
    "[ldev] hello protocol=1\n[ldev] installed suite 42\n[ldev] bye status=1\n",
    "[ldev] hello protocol=1\n[ldev] bye status=0\n[ldev] installed suite 42\n",
    "[ldev] hello protocol=1\n[ldev] installed suite 42\n[ldev] bye status=0\n[ldev] bye status=0\n"
  };
  for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
    g_autoptr(GError) error = NULL;
    g_assert_false(bu_j2me_check_completion(invalid[i], "install", NULL, &error));
    g_assert_nonnull(error);
  }
  const char *wrong_ids[] = { "4", "421", "01", NULL };
  for (guint i = 0; i < G_N_ELEMENTS(wrong_ids); i++) {
    g_autoptr(GError) error = NULL;
    g_assert_false(bu_j2me_check_completion(valid[1], "remove", wrong_ids[i], &error));
    g_assert_nonnull(error);
  }
  g_autoptr(GError) error = NULL;
  g_assert_false(bu_j2me_check_completion(valid[0], "unknown", NULL, &error));
  g_assert_nonnull(error);
}

static void test_catalog_skipped_suite(void)
{
  g_autoptr(GString) text = g_string_new(valid_output);
  g_string_replace(text, "[ldev] suite 42",
                   "[ldev] error suite 7: properties unreadable\n[ldev] suite 42", 1);
  g_string_replace(text, "[ldev] suites 1",
                   "[ldev] error suite 8: MIDlet properties unreadable\n[ldev] suites 1", 1);
  guint skipped = 0;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) list = bu_j2me_parse_list(text->str, &skipped, &error);
  g_assert_no_error(error);
  g_assert_nonnull(list);
  g_assert_cmpuint(list->len, ==, 1);
  g_assert_cmpstr(((BuJ2meSuite *)g_ptr_array_index(list, 0))->id, ==, "42");
  g_assert_cmpuint(skipped, ==, 2);
  g_string_replace(text, "status=0", "status=3", 1);
  assert_bad_catalog(text->str);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/j2me/list/valid", test_parse_valid);
  g_test_add_func("/j2me/list/incomplete", test_parse_incomplete);
  g_test_add_func("/j2me/process/argv-boundaries", test_argv_boundaries);
  g_test_add_func("/j2me/list/boundaries", test_catalog_boundaries);
  g_test_add_func("/j2me/list/skipped-suite", test_catalog_skipped_suite);
  g_test_add_func("/j2me/process/invalid-args", test_invalid_args);
  g_test_add_func("/j2me/process/operation-args", test_operation_args);
  g_test_add_func("/j2me/process/completion", test_completion);
  for (guint i = 0; i < G_N_ELEMENTS(mutations); i++) {
    g_autofree char *path = g_strconcat("/j2me/list/reject/", mutations[i].name, NULL);
    g_test_add_data_func(path, &mutations[i], test_mutation);
  }
  return g_test_run();
}
