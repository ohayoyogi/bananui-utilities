/* SPDX-License-Identifier: MIT */
#include "j2me.h"
#include <string.h>

static GQuark j2me_error_quark(void) { return g_quark_from_static_string("bananui-j2me-error"); }
enum { J2ME_ERROR_PROTOCOL = 1, J2ME_ERROR_CONFIG };

static BuJ2meSuite *find_suite(GPtrArray *suites, const char *id)
{
  for (guint i = 0; i < suites->len; i++) {
    BuJ2meSuite *suite = g_ptr_array_index(suites, i);
    if (g_str_equal(suite->id, id)) return suite;
  }
  return NULL;
}

void bu_j2me_suite_free(gpointer value)
{
  BuJ2meSuite *suite = value;
  if (!suite) return;
  g_free(suite->id); g_free(suite->name); g_free(suite->vendor); g_free(suite->version);
  g_ptr_array_unref(suite->midlets); g_free(suite);
}

static void midlet_free(gpointer value)
{
  BuJ2meMidlet *midlet = value;
  g_free(midlet->name); g_free(midlet->klass); g_free(midlet);
}

static gboolean decimal(const char *text, guint *value)
{
  guint parsed = 0;
  if (!text || !*text || (text[0] == '0' && text[1])) return FALSE;
  for (const char *p = text; *p; p++) {
    if (!g_ascii_isdigit(*p) || parsed > (G_MAXINT32 - (guint)(*p - '0')) / 10)
      return FALSE;
    parsed = parsed * 10 + (guint)(*p - '0');
  }
  *value = parsed;
  return TRUE;
}

GPtrArray *bu_j2me_parse_list(const char *output, guint *skipped_count, GError **error)
{
  GPtrArray *suites = g_ptr_array_new_with_free_func(bu_j2me_suite_free);
  enum { HELLO, SUITE, MIDLET, BYE, DONE } state = HELLO;
  BuJ2meSuite *current = NULL;
  guint skipped = 0;
  gboolean failed = FALSE;
  if (!output || strlen(output) > 2 * 1024 * 1024 || !g_utf8_validate(output, -1, NULL)) {
    g_set_error(error, j2me_error_quark(), J2ME_ERROR_PROTOCOL,
                "ランタイムの一覧応答がUTF-8形式でないか、サイズ上限を超えました");
    g_ptr_array_unref(suites); return NULL;
  }
  g_auto(GStrv) lines = g_strsplit(output ? output : "", "\n", -1);

  for (guint i = 0; lines[i]; i++) {
    char *line = lines[i];
    gsize length = strlen(line);
    /* Remove only CRLF framing; spaces belong to the TSV field values. */
    if (length && line[length - 1] == '\r') line[length - 1] = '\0';
    if (!g_str_has_prefix(line, "[ldev] ")) continue;
    line += 7;
    if (g_str_has_prefix(line, "hello protocol=")) {
      if (state != HELLO || !(g_str_equal(line, "hello protocol=1") ||
                             g_str_has_prefix(line, "hello protocol=1 "))) failed = TRUE;
      else state = SUITE;
    } else if (g_str_has_prefix(line, "error suite ")) {
      const char *start = line + strlen("error suite ");
      const char *separator = strstr(start, ": ");
      g_autofree char *id_text = separator ? g_strndup(start, separator - start) : NULL;
      guint id;
      if (state != SUITE || !separator || !separator[2] || !decimal(id_text, &id) || !id)
        failed = TRUE;
      else
        skipped++;
    } else if (g_str_has_prefix(line, "error ")) {
      failed = TRUE;
    } else if (g_str_has_prefix(line, "suite ")) {
      char **fields = g_strsplit(line + 6, "\t", -1);
      guint id;
      if (state != SUITE || g_strv_length(fields) != 4 || !decimal(fields[0], &id) || id == 0 || find_suite(suites, fields[0])) {
        failed = TRUE;
      } else {
        BuJ2meSuite *suite = g_new0(BuJ2meSuite, 1);
        suite->id = g_strdup(fields[0]); suite->name = g_strdup(fields[1]);
        suite->vendor = g_strdup(fields[2]); suite->version = g_strdup(fields[3]);
        suite->midlets = g_ptr_array_new_with_free_func(midlet_free);
        g_ptr_array_add(suites, suite);
        current = suite;
        state = MIDLET;
      }
      g_strfreev(fields);
    } else if (g_str_has_prefix(line, "midlet ")) {
      char **fields = g_strsplit(line + 7, "\t", -1);
      guint ordinal;
      if (state != MIDLET || g_strv_length(fields) != 4 || !current ||
          !g_str_equal(fields[0], current->id) || !decimal(fields[1], &ordinal) ||
          ordinal != current->midlets->len + 1 || ordinal > 50 ||
          !fields[2][0] || !fields[3][0]) {
        failed = TRUE;
      } else {
        BuJ2meMidlet *midlet = g_new0(BuJ2meMidlet, 1);
        midlet->ordinal = ordinal; midlet->name = g_strdup(fields[2]); midlet->klass = g_strdup(fields[3]);
        g_ptr_array_add(current->midlets, midlet);
      }
      g_strfreev(fields);
    } else if (g_str_has_prefix(line, "midlets ")) {
      char **fields = g_strsplit(line + 8, " ", 2);
      guint count;
      if (state != MIDLET || g_strv_length(fields) != 2 || !current ||
          !g_str_equal(fields[0], current->id) || !decimal(fields[1], &count) ||
          count == 0 || count > 50 || count != current->midlets->len) failed = TRUE;
      else { current = NULL; state = SUITE; }
      g_strfreev(fields);
    } else if (g_str_has_prefix(line, "suites ")) {
      guint count;
      if (state != SUITE || !decimal(line + 7, &count) || count != suites->len) failed = TRUE;
      else state = BYE;
    } else if (g_str_has_prefix(line, "bye status=")) {
      guint status;
      if (state != BYE || !decimal(line + 11, &status) || status != 0) failed = TRUE;
      else state = DONE;
    } else failed = TRUE;
    if (failed) break;
  }
  if (state != DONE || failed) {
    g_set_error(error, j2me_error_quark(), J2ME_ERROR_PROTOCOL,
                "ランタイムの一覧応答が不完全か、失敗しました");
    g_ptr_array_unref(suites); return NULL;
  }
  if (skipped_count)
    *skipped_count = skipped;
  return suites;
}

void bu_j2me_config_clear(BuJ2meConfig *config)
{
  g_free(config->executable); g_free(config->lib_dir); g_free(config->data_dir);
  g_free(config->cache_dir); g_free(config->keymap); g_free(config->font); memset(config, 0, sizeof(*config));
}

gboolean bu_j2me_check_completion(const char *output, const char *operation,
                                 const char *suite_id, GError **error)
{
  gboolean install = g_strcmp0(operation, "install") == 0;
  guint expected = 0;
  g_auto(GStrv) lines = NULL;
  if ((!install && (g_strcmp0(operation, "remove") != 0 ||
                    !decimal(suite_id, &expected) || !expected)) ||
      (install && suite_id) || !output || strlen(output) > 2 * 1024 * 1024 ||
      !g_utf8_validate(output, -1, NULL)) goto invalid;
  gboolean hello = FALSE, complete = FALSE, bye = FALSE;
  lines = g_strsplit(output, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    char *line = lines[i];
    gsize length = strlen(line);
    if (length && line[length - 1] == '\r') line[length - 1] = '\0';
    if (!g_str_has_prefix(line, "[ldev] ")) continue;
    line += 7;
    if (bye) goto invalid;
    if (g_str_has_prefix(line, "hello protocol=")) {
      if (hello || !(g_str_equal(line, "hello protocol=1") ||
                     g_str_has_prefix(line, "hello protocol=1 "))) goto invalid;
      hello = TRUE;
    } else if (!hello || g_str_has_prefix(line, "error ")) goto invalid;
    else if (g_str_has_prefix(line, "installed suite ") || g_str_has_prefix(line, "removed suite ")) {
      guint id;
      const char *prefix = install ? "installed suite " : "removed suite ";
      if (complete || !g_str_has_prefix(line, prefix) ||
          !decimal(line + strlen(prefix), &id) || !id ||
          (!install && id != expected)) goto invalid;
      complete = TRUE;
    } else if (g_str_has_prefix(line, "bye status=")) {
      if (!complete || !g_str_equal(line, "bye status=0")) goto invalid;
      bye = TRUE;
    }
    /* Native lifecycle/progress records do not alter the operation result. */
  }
  if (hello && complete && bye) return TRUE;
invalid:
  g_set_error(error, j2me_error_quark(), J2ME_ERROR_PROTOCOL,
              "ランタイムの処理完了を確認できませんでした");
  return FALSE;
}

gboolean bu_j2me_config_load(BuJ2meConfig *config, GError **error)
{
  g_autofree char *path = g_build_filename(g_get_user_config_dir(), "bananui-utils", "runtime.ini", NULL);
  g_autoptr(GKeyFile) file = g_key_file_new();
  g_autofree char *data_default = g_build_filename(g_get_user_data_dir(), "bananui-utils", "j2me", NULL);
  g_autofree char *cache_default = g_build_filename(g_get_user_cache_dir(), "bananui-utils", "j2me", NULL);
  if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, error)) return FALSE;
  config->executable = g_key_file_get_string(file, "runtime", "executable", NULL);
  config->lib_dir = g_key_file_get_string(file, "runtime", "lib-dir", NULL);
  config->data_dir = g_key_file_get_string(file, "runtime", "data-dir", NULL);
  config->cache_dir = g_key_file_get_string(file, "runtime", "cache-dir", NULL);
  config->keymap = g_key_file_get_string(file, "runtime", "keymap", NULL);
  config->font = g_key_file_get_string(file, "runtime", "font", NULL);
  if (!config->data_dir) config->data_dir = g_strdup(data_default);
  if (!config->cache_dir) config->cache_dir = g_strdup(cache_default);
  if (!config->executable || !g_path_is_absolute(config->executable) || !g_file_test(config->executable, G_FILE_TEST_IS_EXECUTABLE) ||
      !config->lib_dir || !g_path_is_absolute(config->lib_dir) || !g_file_test(config->lib_dir, G_FILE_TEST_IS_DIR)) {
    g_set_error(error, j2me_error_quark(), J2ME_ERROR_CONFIG,
                "runtime.ini に実行ファイルと lib-dir の絶対パスを設定してください");
    bu_j2me_config_clear(config); return FALSE;
  }
  if (!g_path_is_absolute(config->data_dir) || !g_path_is_absolute(config->cache_dir)) {
    g_set_error(error, j2me_error_quark(), J2ME_ERROR_CONFIG, "data-dir と cache-dir は絶対パスが必要です");
    bu_j2me_config_clear(config); return FALSE;
  }
  if (config->keymap && (!g_path_is_absolute(config->keymap) || !g_file_test(config->keymap, G_FILE_TEST_IS_REGULAR))) {
    g_set_error(error, j2me_error_quark(), J2ME_ERROR_CONFIG, "keymap が見つかりません");
    bu_j2me_config_clear(config); return FALSE;
  }
  if (config->font && (!g_path_is_absolute(config->font) || !g_file_test(config->font, G_FILE_TEST_IS_REGULAR))) {
    g_set_error(error, j2me_error_quark(), J2ME_ERROR_CONFIG, "font が見つかりません");
    bu_j2me_config_clear(config); return FALSE;
  }
  return TRUE;
}

GPtrArray *bu_j2me_build_argv(const BuJ2meConfig *c, const char *operation,
                              const char *suite_id, guint ordinal,
                              const char *jar_path, GError **error)
{
  guint id;
  if (!c || !c->executable || !g_path_is_absolute(c->executable) ||
      !c->lib_dir || !g_path_is_absolute(c->lib_dir) ||
      !c->data_dir || !g_path_is_absolute(c->data_dir) ||
      !c->cache_dir || !g_path_is_absolute(c->cache_dir) ||
      (c->keymap && !g_path_is_absolute(c->keymap)) ||
      (c->font && !g_path_is_absolute(c->font))) {
    g_set_error(error, j2me_error_quark(), J2ME_ERROR_CONFIG,
                "ランタイムの実行ファイルと保存先の絶対パスを設定してください");
    return NULL;
  }
  gboolean list = g_strcmp0(operation, "list") == 0;
  gboolean install = g_strcmp0(operation, "install") == 0;
  gboolean remove = g_strcmp0(operation, "remove") == 0;
  gboolean run = g_strcmp0(operation, "run") == 0;
  gboolean valid = (list && !suite_id && !ordinal && !jar_path) ||
    (install && !suite_id && !ordinal && jar_path && g_path_is_absolute(jar_path) &&
     strlen(jar_path) >= 4 && g_ascii_strcasecmp(jar_path + strlen(jar_path) - 4, ".jar") == 0 &&
     g_file_test(jar_path, G_FILE_TEST_IS_REGULAR)) ||
    ((remove || run) && decimal(suite_id, &id) && id > 0 && !jar_path &&
     ((remove && !ordinal) || (run && ordinal >= 1 && ordinal <= 50)));
  if (!valid) {
    g_set_error(error, j2me_error_quark(), J2ME_ERROR_CONFIG,
                "操作の引数が不正です。登録にはローカルのJARファイルが必要です");
    return NULL;
  }
  GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
  #define ADD(v) g_ptr_array_add(argv, g_strdup(v))
  ADD(c->executable); ADD("--lib"); ADD(c->lib_dir); ADD("--data"); ADD(c->data_dir);
  ADD("--cache"); ADD(c->cache_dir); ADD("--display"); ADD(run ? "wayland" : "sim");
  ADD("--input"); ADD("cmd:-"); ADD("--on-exit"); ADD("quit");
  if (c->keymap) { ADD("--keymap"); ADD(c->keymap); }
  if (c->font) { ADD("--font"); ADD(c->font); }
  if (list) ADD("--list-suites");
  else if (install) { ADD("--install-only"); ADD(jar_path); }
  else if (remove) { ADD("--remove-suite"); ADD(suite_id); }
  else {
    ADD("--app-id"); ADD("org.bananui.J2ME");
    ADD("--suite"); ADD(suite_id);
    ADD("--midlet"); g_ptr_array_add(argv, g_strdup_printf("%u", ordinal));
  }
  g_ptr_array_add(argv, NULL);
  #undef ADD
  return argv;
}
