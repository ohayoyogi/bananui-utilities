/* SPDX-License-Identifier: MIT */
#pragma once
#include <gio/gio.h>

typedef struct {
  char *id;
  char *name;
  char *vendor;
  char *version;
  GPtrArray *midlets;
} BuJ2meSuite;

typedef struct {
  guint ordinal;
  char *name;
  char *klass;
} BuJ2meMidlet;

typedef struct {
  char *executable, *lib_dir, *data_dir, *cache_dir, *keymap, *font;
} BuJ2meConfig;

GPtrArray *bu_j2me_parse_list(const char *output, guint *skipped_count, GError **error);
/* Runtime adapter boundary: UI code must not interpret native protocol records. */
gboolean bu_j2me_check_completion(const char *output, const char *operation,
                                 const char *suite_id, GError **error);
void bu_j2me_suite_free(gpointer suite);
void bu_j2me_config_clear(BuJ2meConfig *config);
gboolean bu_j2me_config_load(BuJ2meConfig *config, GError **error);
GPtrArray *bu_j2me_build_argv(const BuJ2meConfig *config, const char *operation,
                              const char *suite_id, guint ordinal,
                              const char *jar_path, GError **error);
