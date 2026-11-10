/* SPDX-License-Identifier: MIT */
#pragma once
#include <gio/gio.h>

#define BU_FILES_TRASH ".bananui-trash"
#define BU_FILES_TEXT_LIMIT (128 * 1024)
typedef struct { char *root, *relative; } BuFiles;
typedef struct {
  char *name, *display_name, *content_type;
  GFileType type;
  guint64 size, modified;
} BuFileEntry;

BuFiles *bu_files_new(const char *root, GError **error);
BuFiles *bu_files_clone(const BuFiles *files);
void bu_files_free(BuFiles *files);
void bu_file_entry_free(gpointer entry);
GPtrArray *bu_files_list(BuFiles *files, GError **error);
gboolean bu_files_enter(BuFiles *files, const char *name, GError **error);
gboolean bu_files_parent(BuFiles *files);
char *bu_files_path(BuFiles *files, const char *name, GError **error);
gboolean bu_files_mkdir(BuFiles *files, const char *name, GError **error);
gboolean bu_files_rename(BuFiles *files, const char *name, const char *new_name, GError **error);
gboolean bu_files_transfer(BuFiles *source, const char *name, BuFiles *destination,
                           gboolean move, GError **error);
char *bu_files_trash(BuFiles *files, const char *name, GError **error);
char *bu_files_read_text(BuFiles *files, const char *name, GError **error);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(BuFiles, bu_files_free)
