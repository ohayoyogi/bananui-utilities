/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>
#define BU_NOTE_MAX_BYTES (16 * 1024)
typedef struct { char *directory; } BuNoteStore;
typedef struct { char *id, *title; gint64 modified; gboolean readable; } BuNoteSummary;
typedef struct { char *id, *text, *original, *etag; } BuDraft;
BuNoteStore *bu_note_store_new(const char *directory, GError **error);
void bu_note_store_free(BuNoteStore *store);
void bu_note_summary_free(gpointer summary);
GPtrArray *bu_note_store_list(BuNoteStore *store, GError **error);
char *bu_note_store_read(BuNoteStore *store, const char *id, char **etag, GError **error);
gboolean bu_note_store_save(BuNoteStore *store, const char *id, const char *text,
                            const char *etag, char **new_etag, GError **error);
gboolean bu_note_store_trash(BuNoteStore *store, const char *id, GError **error);
gboolean bu_note_store_save_draft(BuNoteStore *store, const BuDraft *draft, GError **error);
BuDraft *bu_note_store_load_draft(BuNoteStore *store, GError **error);
gboolean bu_note_store_clear_draft(BuNoteStore *store, GError **error);
void bu_draft_free(BuDraft *draft);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(BuNoteStore, bu_note_store_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(BuDraft, bu_draft_free)
