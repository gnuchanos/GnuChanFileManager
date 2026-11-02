#include "gfm_file_ops.h"

#include <string.h>

gboolean gfm_name_is_safe(const gchar *name) {
    return name != NULL && name[0] != '\0' &&
           strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
           strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

GFile *gfm_child_named(GFile *parent, const gchar *name, GError **error) {
    g_return_val_if_fail(G_IS_FILE(parent), NULL);
    if (!gfm_name_is_safe(name)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Use a single file name, not a path.");
        return NULL;
    }
    return g_file_get_child(parent, name);
}

static gboolean delete_node(GFile *file, GError **error);

static gboolean copy_node(GFile *source, GFile *destination, GError **error) {
    GFileInfo *info = g_file_query_info(
        source,
        G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK ","
        G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
    if (info == NULL) {
        return FALSE;
    }

    gboolean success = FALSE;
    if (g_file_info_get_is_symlink(info)) {
        const gchar *target = g_file_info_get_symlink_target(info);
        if (target == NULL || target[0] == '\0') {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "The symbolic link has no target.");
        } else {
            success = g_file_make_symbolic_link(destination, target, NULL,
                                                 error);
        }
    } else if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        if (g_file_make_directory(destination, NULL, error)) {
            GFileEnumerator *enumerator = g_file_enumerate_children(
                source, G_FILE_ATTRIBUTE_STANDARD_NAME,
                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
            if (enumerator != NULL) {
                success = TRUE;
                GFileInfo *child_info;
                while ((child_info = g_file_enumerator_next_file(
                            enumerator, NULL, error)) != NULL) {
                    const gchar *child_name =
                        g_file_info_get_name(child_info);
                    GFile *source_child =
                        gfm_child_named(source, child_name, error);
                    GFile *destination_child =
                        source_child != NULL
                            ? gfm_child_named(destination, child_name, error)
                            : NULL;
                    if (source_child == NULL || destination_child == NULL ||
                        !copy_node(source_child, destination_child, error)) {
                        success = FALSE;
                        g_clear_object(&source_child);
                        g_clear_object(&destination_child);
                        g_object_unref(child_info);
                        break;
                    }
                    g_object_unref(source_child);
                    g_object_unref(destination_child);
                    g_object_unref(child_info);
                }
                if (success && error != NULL && *error != NULL) {
                    success = FALSE;
                }
                g_object_unref(enumerator);
            }
            if (!success) {
                GError *cleanup_error = NULL;
                delete_node(destination, &cleanup_error);
                g_clear_error(&cleanup_error);
            }
        }
    } else {
        success = g_file_copy(source, destination, G_FILE_COPY_NONE, NULL,
                              NULL, NULL, error);
    }

    g_object_unref(info);
    return success;
}

static GFile *destination_for_source(GFile *source,
                                     GFile *destination_directory,
                                     GError **error) {
    GFileInfo *destination_info = g_file_query_info(
        destination_directory, G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
    if (destination_info == NULL) {
        return NULL;
    }
    if (g_file_info_get_file_type(destination_info) != G_FILE_TYPE_DIRECTORY) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                            "The destination is not a directory.");
        g_object_unref(destination_info);
        return NULL;
    }
    g_object_unref(destination_info);

    GFileInfo *source_info = g_file_query_info(
        source, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
    if (source_info == NULL) {
        return NULL;
    }
    gboolean source_is_directory =
        !g_file_info_get_is_symlink(source_info) &&
        g_file_info_get_file_type(source_info) == G_FILE_TYPE_DIRECTORY;
    g_object_unref(source_info);
    if (source_is_directory) {
        gchar *relative = g_file_get_relative_path(source, destination_directory);
        if (relative != NULL) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "A directory cannot be copied or moved into itself.");
            g_free(relative);
            return NULL;
        }
    }

    gchar *basename = g_file_get_basename(source);
    GFile *destination = gfm_child_named(destination_directory, basename,
                                         error);
    g_free(basename);
    if (destination != NULL && g_file_equal(source, destination)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "The source and destination are the same.");
        g_clear_object(&destination);
    }
    return destination;
}

gboolean gfm_copy_to_directory(GFile *source, GFile *destination_directory,
                               GError **error) {
    g_return_val_if_fail(G_IS_FILE(source), FALSE);
    g_return_val_if_fail(G_IS_FILE(destination_directory), FALSE);
    GFile *destination =
        destination_for_source(source, destination_directory, error);
    if (destination == NULL) {
        return FALSE;
    }
    gboolean success = copy_node(source, destination, error);
    g_object_unref(destination);
    return success;
}

gboolean gfm_move_to_directory(GFile *source, GFile *destination_directory,
                               GError **error) {
    g_return_val_if_fail(G_IS_FILE(source), FALSE);
    g_return_val_if_fail(G_IS_FILE(destination_directory), FALSE);
    GFile *destination =
        destination_for_source(source, destination_directory, error);
    if (destination == NULL) {
        return FALSE;
    }
    gboolean success =
        g_file_move(source, destination, G_FILE_COPY_NONE, NULL, NULL, NULL,
                    error);
    g_object_unref(destination);
    return success;
}

gboolean gfm_rename(GFile *source, const gchar *new_name, GError **error) {
    g_return_val_if_fail(G_IS_FILE(source), FALSE);
    GFile *parent = g_file_get_parent(source);
    if (parent == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The filesystem root cannot be renamed.");
        return FALSE;
    }
    GFile *destination = gfm_child_named(parent, new_name, error);
    g_object_unref(parent);
    if (destination == NULL) {
        return FALSE;
    }
    if (g_file_equal(source, destination)) {
        g_object_unref(destination);
        return TRUE;
    }
    gboolean success = g_file_move(source, destination, G_FILE_COPY_NONE,
                                   NULL, NULL, NULL, error);
    g_object_unref(destination);
    return success;
}

gboolean gfm_trash(GFile *source, GError **error) {
    g_return_val_if_fail(G_IS_FILE(source), FALSE);
    return g_file_trash(source, NULL, error);
}

static gboolean delete_node(GFile *file, GError **error) {
    GFileInfo *info = g_file_query_info(
        file, G_FILE_ATTRIBUTE_STANDARD_TYPE ","
              G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
    if (info == NULL) {
        return FALSE;
    }
    gboolean is_directory =
        !g_file_info_get_is_symlink(info) &&
        g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    g_object_unref(info);

    if (is_directory) {
        GFileEnumerator *enumerator = g_file_enumerate_children(
            file, G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
        if (enumerator == NULL) {
            return FALSE;
        }
        GFileInfo *child_info;
        while ((child_info =
                    g_file_enumerator_next_file(enumerator, NULL, error)) !=
               NULL) {
            GFile *child = gfm_child_named(
                file, g_file_info_get_name(child_info), error);
            gboolean success =
                child != NULL && delete_node(child, error);
            g_clear_object(&child);
            g_object_unref(child_info);
            if (!success) {
                g_object_unref(enumerator);
                return FALSE;
            }
        }
        if (error != NULL && *error != NULL) {
            g_object_unref(enumerator);
            return FALSE;
        }
        g_object_unref(enumerator);
    }
    return g_file_delete(file, NULL, error);
}

gboolean gfm_delete(GFile *source, GError **error) {
    g_return_val_if_fail(G_IS_FILE(source), FALSE);
    return delete_node(source, error);
}
