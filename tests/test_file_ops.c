#include "gfm_file_ops.h"

#include <glib/gstdio.h>
#include <sys/stat.h>

static gchar *test_root;

static GFile *child(GFile *parent, const gchar *name) {
    GError *error = NULL;
    GFile *result = gfm_child_named(parent, name, &error);
    g_assert_no_error(error);
    g_assert_nonnull(result);
    return result;
}

static void test_safe_names(void) {
    g_assert_true(gfm_name_is_safe("ordinary name.txt"));
    g_assert_true(gfm_name_is_safe("dots...ok"));
    g_assert_false(gfm_name_is_safe(NULL));
    g_assert_false(gfm_name_is_safe(""));
    g_assert_false(gfm_name_is_safe("."));
    g_assert_false(gfm_name_is_safe(".."));
    g_assert_false(gfm_name_is_safe("../escape"));
    g_assert_false(gfm_name_is_safe("folder/name"));
    g_assert_false(gfm_name_is_safe("folder\\name"));
}

static void test_parse_uri_clipboard_data(void) {
    gchar *path = g_build_filename(test_root, "copied file.txt", NULL);
    GFile *expected = g_file_new_for_path(path);
    gchar *uri = g_file_get_uri(expected);
    gchar *clipboard = g_strdup_printf("cut\n%s\r\nnot a URI\n"
                                       "$(touch should-not-run)\n",
                                       uri);
    gboolean move = FALSE;
    GPtrArray *files = gfm_parse_uri_list(clipboard, &move);
    g_assert_true(move);
    g_assert_cmpuint(files->len, ==, 1);
    g_assert_true(g_file_equal(expected, g_ptr_array_index(files, 0)));

    GPtrArray *uri_list = gfm_parse_uri_list(uri, &move);
    g_assert_false(move);
    g_assert_cmpuint(uri_list->len, ==, 1);

    g_ptr_array_unref(uri_list);
    g_ptr_array_unref(files);
    g_free(clipboard);
    g_free(uri);
    g_object_unref(expected);
    g_free(path);
}

static void test_copy_move_rename_delete(void) {
    GError *error = NULL;
    gchar *from_path = g_build_filename(test_root, "from", NULL);
    gchar *to_path = g_build_filename(test_root, "to", NULL);
    g_assert_cmpint(g_mkdir(from_path, 0700), ==, 0);
    g_assert_cmpint(g_mkdir(to_path, 0700), ==, 0);
    gchar *source_path = g_build_filename(from_path, "sample.txt", NULL);
    g_assert_true(g_file_set_contents(source_path, "safe data", -1, &error));
    g_assert_no_error(error);

    GFile *source_dir = g_file_new_for_path(from_path);
    GFile *destination_dir = g_file_new_for_path(to_path);
    GFile *source = child(source_dir, "sample.txt");
    g_assert_true(gfm_copy_to_directory(source, destination_dir, &error));
    g_assert_no_error(error);
    GFile *copied = child(destination_dir, "sample.txt");
    g_assert_true(g_file_query_exists(copied, NULL));

    g_assert_false(gfm_move_to_directory(copied, source_dir, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_assert_true(gfm_delete(source, &error));
    g_assert_no_error(error);
    g_assert_true(gfm_move_to_directory(copied, source_dir, &error));
    g_assert_no_error(error);
    g_assert_false(g_file_query_exists(copied, NULL));
    GFile *moved = child(source_dir, "sample.txt");
    g_assert_true(gfm_rename(moved, "renamed.txt", &error));
    g_assert_no_error(error);
    GFile *renamed = child(source_dir, "renamed.txt");
    g_assert_true(g_file_query_exists(renamed, NULL));
    g_assert_false(gfm_rename(renamed, "../escape", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    g_assert_true(gfm_delete(source_dir, &error));
    g_assert_no_error(error);
    g_assert_false(g_file_query_exists(source_dir, NULL));
    g_assert_true(gfm_delete(destination_dir, &error));
    g_assert_no_error(error);

    g_object_unref(renamed);
    g_object_unref(moved);
    g_object_unref(copied);
    g_object_unref(source);
    g_object_unref(source_dir);
    g_object_unref(destination_dir);
    g_free(source_path);
    g_free(from_path);
    g_free(to_path);
}

static void test_refuse_copy_into_self(void) {
    GError *error = NULL;
    gchar *parent_path = g_build_filename(test_root, "tree", NULL);
    gchar *nested_path = g_build_filename(parent_path, "nested", NULL);
    g_assert_cmpint(g_mkdir(parent_path, 0700), ==, 0);
    g_assert_cmpint(g_mkdir(nested_path, 0700), ==, 0);
    GFile *parent = g_file_new_for_path(parent_path);
    GFile *nested = g_file_new_for_path(nested_path);
    g_assert_false(gfm_copy_to_directory(parent, nested, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_true(gfm_delete(parent, &error));
    g_assert_no_error(error);
    g_object_unref(nested);
    g_object_unref(parent);
    g_free(nested_path);
    g_free(parent_path);
}

static void test_failed_directory_copy_removes_partial_tree(void) {
    GError *error = NULL;
    gchar *source_path = g_build_filename(test_root, "source", NULL);
    gchar *special_path = g_build_filename(source_path, "special", NULL);
    g_assert_cmpint(g_mkdir(source_path, 0700), ==, 0);
    if (mkfifo(special_path, 0600) != 0) {
        g_test_skip("the test filesystem does not support FIFOs");
        GFile *source = g_file_new_for_path(source_path);
        g_assert_true(gfm_delete(source, &error));
        g_assert_no_error(error);
        g_object_unref(source);
        g_free(special_path);
        g_free(source_path);
        return;
    }

    GFile *source = g_file_new_for_path(source_path);
    GFile *destination_directory = g_file_new_for_path(test_root);
    GFile *destination = child(destination_directory, "source");
    g_assert_false(
        gfm_copy_to_directory(source, destination_directory, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(g_file_query_exists(destination, NULL));

    g_assert_true(gfm_delete(source, &error));
    g_assert_no_error(error);
    g_object_unref(destination);
    g_object_unref(destination_directory);
    g_object_unref(source);
    g_free(special_path);
    g_free(source_path);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    gchar *current_dir = g_get_current_dir();
    test_root = g_build_filename(current_dir, ".test-work", NULL);
    g_free(current_dir);
    if (g_file_test(test_root, G_FILE_TEST_EXISTS)) {
        GFile *old_test_root = g_file_new_for_path(test_root);
        GError *cleanup_error = NULL;
        g_assert_true(gfm_delete(old_test_root, &cleanup_error));
        g_assert_no_error(cleanup_error);
        g_object_unref(old_test_root);
    }
    g_assert_cmpint(g_mkdir_with_parents(test_root, 0700), ==, 0);
    g_test_add_func("/file-ops/safe-names", test_safe_names);
    g_test_add_func("/file-ops/parse-uri-clipboard-data",
                    test_parse_uri_clipboard_data);
    g_test_add_func("/file-ops/copy-move-rename-delete",
                    test_copy_move_rename_delete);
    g_test_add_func("/file-ops/refuse-copy-into-self",
                    test_refuse_copy_into_self);
    g_test_add_func("/file-ops/failed-copy-removes-partial-tree",
                    test_failed_directory_copy_removes_partial_tree);
    int result = g_test_run();
    g_rmdir(test_root);
    g_free(test_root);
    return result;
}
