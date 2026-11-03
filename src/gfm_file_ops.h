#ifndef GFM_FILE_OPS_H
#define GFM_FILE_OPS_H

#include <gio/gio.h>

gboolean gfm_name_is_safe(const gchar *name);
GFile *gfm_child_named(GFile *parent, const gchar *name, GError **error);
gboolean gfm_copy_to_directory(GFile *source, GFile *destination_directory,
                               GError **error);
gboolean gfm_move_to_directory(GFile *source, GFile *destination_directory,
                               GError **error);
gboolean gfm_rename(GFile *source, const gchar *new_name, GError **error);
gboolean gfm_trash(GFile *source, GError **error);
gboolean gfm_delete(GFile *source, GError **error);
GPtrArray *gfm_parse_uri_list(const gchar *contents, gboolean *move);

#endif
