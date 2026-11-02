#include "gfm_file_ops.h"

#include <gtk/gtk.h>
#include <string.h>

#define APP_ID "org.gnuchan.GnuChanFileManager"

enum { COL_NAME, COL_TYPE, COL_SIZE, COL_FILE, N_COLUMNS };

typedef struct {
    GtkApplication *application;
    GtkWidget *window;
    GtkWidget *address;
    GtkWidget *search;
    GtkWidget *back_button;
    GtkWidget *forward_button;
    GtkWidget *up_button;
    GtkWidget *hidden_button;
    GtkWidget *view;
    GtkListStore *store;
    GFile *location;
    GPtrArray *history;
    guint history_index;
    gboolean show_hidden;
    gchar *config_path;
    gchar *start_location;
} App;

static void show_error(App *app, const gchar *summary, GError *error) {
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
        GTK_BUTTONS_CLOSE, "%s", summary);
    if (error != NULL) {
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                                  "%s", error->message);
    }
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static gchar *format_size(guint64 size, gboolean directory) {
    if (directory) {
        return g_strdup("—");
    }
    return g_format_size(size);
}

static GFile *file_from_address(const gchar *text, GFile *base) {
    gchar *scheme = g_uri_parse_scheme(text);
    GFile *file = NULL;
    if (scheme != NULL) {
        file = g_file_new_for_uri(text);
    } else {
        gchar *expanded = NULL;
        if (g_str_equal(text, "~")) {
            expanded = g_strdup(g_get_home_dir());
        } else if (g_str_has_prefix(text, "~/")) {
            expanded = g_build_filename(g_get_home_dir(), text + 2, NULL);
        } else {
            expanded = g_strdup(text);
        }
        if (g_path_is_absolute(expanded)) {
            file = g_file_new_for_path(expanded);
        } else if (base != NULL) {
            file = g_file_resolve_relative_path(base, expanded);
        } else {
            gchar *home_path = g_build_filename(g_get_home_dir(), expanded, NULL);
            file = g_file_new_for_path(home_path);
            g_free(home_path);
        }
        g_free(expanded);
    }
    g_free(scheme);
    return file;
}

static gboolean is_hidden_name(const gchar *name) {
    return name != NULL && name[0] == '.';
}

static void update_navigation(App *app) {
    gtk_widget_set_sensitive(app->back_button, app->history_index > 0);
    gtk_widget_set_sensitive(app->forward_button,
                             app->history_index + 1 < app->history->len);
    GFile *parent = g_file_get_parent(app->location);
    gtk_widget_set_sensitive(app->up_button, parent != NULL);
    g_clear_object(&parent);
}

static void save_config(App *app) {
    GKeyFile *key_file = g_key_file_new();
    g_key_file_set_boolean(key_file, "General", "show_hidden",
                           app->show_hidden);
    g_key_file_set_string(key_file, "General", "start_location",
                          app->start_location);
    gchar *data = g_key_file_to_data(key_file, NULL, NULL);
    gchar *directory = g_path_get_dirname(app->config_path);
    g_mkdir_with_parents(directory, 0700);
    GError *error = NULL;
    if (!g_file_set_contents(app->config_path, data, -1, &error)) {
        g_warning("Could not save configuration: %s", error->message);
        g_clear_error(&error);
    }
    g_free(directory);
    g_free(data);
    g_key_file_unref(key_file);
}

static void refresh_listing(App *app) {
    gtk_list_store_clear(app->store);
    GError *error = NULL;
    GFileEnumerator *enumerator = g_file_enumerate_children(
        app->location,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
        G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
    if (enumerator == NULL) {
        show_error(app, "Could not read this location.", error);
        g_clear_error(&error);
        return;
    }

    gchar *query = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(app->search)),
                                   -1);
    GFileInfo *info;
    while ((info = g_file_enumerator_next_file(enumerator, NULL, &error)) !=
           NULL) {
        const gchar *name = g_file_info_get_name(info);
        gboolean hidden = g_file_info_get_is_hidden(info) ||
                          is_hidden_name(name);
        if (hidden && !app->show_hidden) {
            g_object_unref(info);
            continue;
        }
        gchar *folded = g_utf8_casefold(name, -1);
        gboolean matches = query[0] == '\0' || strstr(folded, query) != NULL;
        g_free(folded);
        if (matches) {
            GFile *file = g_file_get_child(app->location, name);
            GFileType type = g_file_info_get_file_type(info);
            gboolean directory = type == G_FILE_TYPE_DIRECTORY;
            const gchar *type_name = directory ? "Folder" :
                                     type == G_FILE_TYPE_SYMBOLIC_LINK ? "Link" :
                                     type == G_FILE_TYPE_REGULAR ? "File" :
                                     "Other";
            gchar *size = format_size(g_file_info_get_size(info), directory);
            GtkTreeIter iter;
            gtk_list_store_append(app->store, &iter);
            gtk_list_store_set(app->store, &iter, COL_NAME, name, COL_TYPE,
                               type_name, COL_SIZE, size, COL_FILE, file, -1);
            g_free(size);
            g_object_unref(file);
        }
        g_object_unref(info);
    }
    if (error != NULL) {
        show_error(app, "An error occurred while reading this location.",
                   error);
        g_clear_error(&error);
    }
    g_free(query);
    g_object_unref(enumerator);
    update_navigation(app);
}

static gboolean go_to(App *app, GFile *file, gboolean add_history) {
    GError *error = NULL;
    GFileInfo *info = g_file_query_info(
        file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, NULL,
        &error);
    if (info == NULL || g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) {
        if (info != NULL) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                                "The location is not a directory.");
            g_object_unref(info);
        }
        show_error(app, "Cannot open location.", error);
        g_clear_error(&error);
        return FALSE;
    }
    g_object_unref(info);

    g_set_object(&app->location, file);
    if (add_history) {
        while (app->history->len > app->history_index + 1) {
            g_ptr_array_remove_index(app->history, app->history->len - 1);
        }
        g_ptr_array_add(app->history, g_object_ref(file));
        app->history_index = app->history->len - 1;
    }
    gchar *path = g_file_get_path(file);
    if (path == NULL) {
        path = g_file_get_uri(file);
    }
    gtk_entry_set_text(GTK_ENTRY(app->address), path);
    g_free(path);
    refresh_listing(app);
    return TRUE;
}

static void on_row_activated(GtkTreeView *view, GtkTreePath *path,
                             GtkTreeViewColumn *column, gpointer data) {
    (void)view;
    (void)column;
    App *app = data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->store), &iter, path)) {
        return;
    }
    GFile *file = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->store), &iter, COL_FILE, &file, -1);
    GError *error = NULL;
    GFileInfo *info = g_file_query_info(
        file, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, NULL,
        &error);
    if (info != NULL && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        go_to(app, file, TRUE);
    } else {
        gchar *uri = g_file_get_uri(file);
        g_app_info_launch_default_for_uri(uri, NULL, &error);
        g_free(uri);
    }
    if (error != NULL) {
        show_error(app, "Could not open this item.", error);
        g_clear_error(&error);
    }
    g_clear_object(&info);
    g_object_unref(file);
}

static GFile *selected_file(App *app) {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(
        GTK_TREE_VIEW(app->view));
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) {
        return NULL;
    }
    GFile *file = NULL;
    gtk_tree_model_get(model, &iter, COL_FILE, &file, -1);
    return file;
}

static gint ask_confirm(App *app, const gchar *message, const gchar *detail) {
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_CANCEL, "%s", message);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
                                             detail);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Continue", GTK_RESPONSE_ACCEPT);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return response;
}

static gchar *ask_text(App *app, const gchar *title, const gchar *prompt,
                       const gchar *initial) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        title, GTK_WINDOW(app->window), GTK_DIALOG_MODAL, "_Cancel",
        GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *label = gtk_label_new(prompt);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_text(GTK_ENTRY(entry), initial != NULL ? initial : "");
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 6);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_widget_show_all(dialog);
    gchar *result = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        result = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
    }
    gtk_widget_destroy(dialog);
    return result;
}

static void do_operation(App *app, const gchar *operation) {
    GFile *source = selected_file(app);
    if (source == NULL) {
        show_error(app, "Select an item first.", NULL);
        return;
    }
    GError *error = NULL;
    gboolean success = FALSE;
    if (g_str_equal(operation, "rename")) {
        gchar *old_name = g_file_get_basename(source);
        gchar *new_name = ask_text(app, "Rename", "New name:", old_name);
        if (new_name != NULL && gfm_name_is_safe(new_name)) {
            if (ask_confirm(app, "Rename selected item?",
                            "This changes the item's name.") ==
                GTK_RESPONSE_ACCEPT) {
                success = gfm_rename(source, new_name, &error);
            }
        } else if (new_name != NULL) {
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "Enter a single file name without path separators.");
        }
        g_free(new_name);
        g_free(old_name);
    } else if (g_str_equal(operation, "copy") ||
               g_str_equal(operation, "move")) {
        gchar *path = ask_text(app, g_str_equal(operation, "copy") ? "Copy" : "Move",
                               "Destination directory:", NULL);
        if (path != NULL && path[0] != '\0') {
            GFile *destination = file_from_address(path, app->location);
            if (ask_confirm(app, g_str_equal(operation, "copy") ?
                                "Copy selected item?" : "Move selected item?",
                            path) == GTK_RESPONSE_ACCEPT) {
                success = g_str_equal(operation, "copy")
                              ? gfm_copy_to_directory(source, destination, &error)
                              : gfm_move_to_directory(source, destination, &error);
            }
            g_object_unref(destination);
        }
        g_free(path);
    } else if (g_str_equal(operation, "trash")) {
        if (ask_confirm(app, "Move selected item to Trash?",
                        "The item can usually be restored from Trash.") ==
            GTK_RESPONSE_ACCEPT) {
            success = gfm_trash(source, &error);
        }
    } else if (g_str_equal(operation, "delete")) {
        if (ask_confirm(app, "Permanently delete selected item?",
                        "This cannot be undone.") == GTK_RESPONSE_ACCEPT) {
            success = gfm_delete(source, &error);
        }
    }
    if (error != NULL) {
        show_error(app, "File operation failed.", error);
        g_clear_error(&error);
    } else if (success) {
        refresh_listing(app);
    }
    g_object_unref(source);
}

static void on_operation_clicked(GtkButton *button, gpointer data) {
    do_operation(data, g_object_get_data(G_OBJECT(button), "operation"));
}

static void on_back(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->history_index > 0) {
        app->history_index--;
        go_to(app, g_ptr_array_index(app->history, app->history_index), FALSE);
    }
}

static void on_forward(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->history_index + 1 < app->history->len) {
        app->history_index++;
        go_to(app, g_ptr_array_index(app->history, app->history_index), FALSE);
    }
}

static void on_up(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    GFile *parent = g_file_get_parent(app->location);
    if (parent != NULL) {
        go_to(app, parent, TRUE);
        g_object_unref(parent);
    }
}

static void on_address_activate(GtkEntry *entry, gpointer data) {
    App *app = data;
    const gchar *text = gtk_entry_get_text(entry);
    GFile *file = file_from_address(text, app->location);
    go_to(app, file, TRUE);
    g_object_unref(file);
}

static void on_go_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    on_address_activate(GTK_ENTRY(app->address), app);
}

static void on_search_changed(GtkEditable *editable, gpointer data) {
    (void)editable;
    refresh_listing(data);
}

static void on_hidden_toggled(GtkToggleButton *button, gpointer data) {
    App *app = data;
    app->show_hidden = gtk_toggle_button_get_active(button);
    save_config(app);
    refresh_listing(app);
}

static GtkWidget *add_button(GtkWidget *box, const gchar *label,
                             const gchar *operation, App *app) {
    GtkWidget *button = gtk_button_new_with_label(label);
    if (operation != NULL) {
        g_object_set_data(G_OBJECT(button), "operation", (gpointer)operation);
        g_signal_connect(button, "clicked", G_CALLBACK(on_operation_clicked),
                         app);
    }
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 2);
    return button;
}

static void add_text_column(GtkTreeView *view, const gchar *title,
                            gint model_column, gboolean expand) {
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
        title, renderer, "text", model_column, NULL);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_expand(column, expand);
    gtk_tree_view_append_column(view, column);
}

static void app_activate(GtkApplication *application, gpointer user_data) {
    App *app = user_data;
    if (app->window != NULL) {
        gtk_window_present(GTK_WINDOW(app->window));
        return;
    }
    app->application = application;
    app->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app->window), "GnuChan File Manager");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 900, 600);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
    gtk_container_add(GTK_CONTAINER(app->window), outer);
    GtkWidget *navigation = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(outer), navigation, FALSE, FALSE, 0);
    app->back_button = add_button(navigation, "←", NULL, app);
    app->forward_button = add_button(navigation, "→", NULL, app);
    app->up_button = add_button(navigation, "↑", NULL, app);
    g_signal_connect(app->back_button, "clicked", G_CALLBACK(on_back), app);
    g_signal_connect(app->forward_button, "clicked", G_CALLBACK(on_forward), app);
    g_signal_connect(app->up_button, "clicked", G_CALLBACK(on_up), app);
    app->address = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->address), "Location");
    gtk_box_pack_start(GTK_BOX(navigation), app->address, TRUE, TRUE, 0);
    g_signal_connect(app->address, "activate",
                     G_CALLBACK(on_address_activate), app);
    GtkWidget *go_button = add_button(navigation, "Go", NULL, app);
    g_signal_connect(go_button, "clicked", G_CALLBACK(on_go_clicked), app);

    GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(outer), tools, FALSE, FALSE, 0);
    app->search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->search), "Search this folder");
    gtk_box_pack_start(GTK_BOX(tools), app->search, TRUE, TRUE, 0);
    g_signal_connect(app->search, "search-changed",
                     G_CALLBACK(on_search_changed), app);
    app->hidden_button = gtk_check_button_new_with_label("Show hidden files");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->hidden_button),
                                 app->show_hidden);
    gtk_box_pack_start(GTK_BOX(tools), app->hidden_button, FALSE, FALSE, 0);
    g_signal_connect(app->hidden_button, "toggled",
                     G_CALLBACK(on_hidden_toggled), app);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(outer), actions, FALSE, FALSE, 0);
    add_button(actions, "Copy", "copy", app);
    add_button(actions, "Move", "move", app);
    add_button(actions, "Rename", "rename", app);
    add_button(actions, "Trash", "trash", app);
    add_button(actions, "Delete", "delete", app);

    app->store = gtk_list_store_new(N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING,
                                    G_TYPE_STRING, G_TYPE_FILE);
    app->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->store));
    add_text_column(GTK_TREE_VIEW(app->view), "Name", COL_NAME, TRUE);
    add_text_column(GTK_TREE_VIEW(app->view), "Type", COL_TYPE, FALSE);
    add_text_column(GTK_TREE_VIEW(app->view), "Size", COL_SIZE, FALSE);
    gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(app->view), TRUE);
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(app->view), FALSE);
    GtkTreeViewColumn *name_column =
        gtk_tree_view_get_column(GTK_TREE_VIEW(app->view), 0);
    gtk_tree_view_column_set_sort_column_id(name_column, COL_NAME);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->store),
                                         COL_NAME, GTK_SORT_ASCENDING);
    g_signal_connect(app->view, "row-activated",
                     G_CALLBACK(on_row_activated), app);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), app->view);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(app->window);

    GFile *start = app->history->len != 0
                       ? g_object_ref(g_ptr_array_index(app->history, 0))
                       : file_from_address(app->start_location, NULL);
    if (app->history->len == 0) {
        g_ptr_array_add(app->history, g_object_ref(start));
    }
    if (!go_to(app, start, FALSE) && app->history->len == 1) {
        g_ptr_array_set_size(app->history, 0);
        g_clear_object(&start);
        start = g_file_new_for_path(g_get_home_dir());
        g_ptr_array_add(app->history, g_object_ref(start));
        go_to(app, start, FALSE);
    }
    g_object_unref(start);
}

static void app_free(App *app) {
    g_clear_object(&app->location);
    g_clear_object(&app->store);
    if (app->history != NULL) {
        g_ptr_array_unref(app->history);
    }
    g_free(app->config_path);
    g_free(app->start_location);
}

static void app_open(GApplication *application, GFile **files, gint count,
                     const gchar *hint, gpointer user_data) {
    (void)hint;
    App *app = user_data;
    if (app->window == NULL) {
        app_activate(GTK_APPLICATION(application), app);
    }
    if (count > 0) {
        GError *error = NULL;
        GFileInfo *info = g_file_query_info(
            files[0], G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE,
            NULL, &error);
        if (info != NULL &&
            g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
            go_to(app, files[0], TRUE);
        } else if (info != NULL) {
            gchar *uri = g_file_get_uri(files[0]);
            g_app_info_launch_default_for_uri(uri, NULL, &error);
            g_free(uri);
        }
        if (error != NULL) {
            show_error(app, "Could not open the requested item.", error);
            g_clear_error(&error);
        }
        g_clear_object(&info);
    }
}

int main(int argc, char **argv) {
    App app = {0};
    app.history = g_ptr_array_new_with_free_func(g_object_unref);
    gchar *config_dir = g_build_filename(g_get_user_config_dir(),
                                         "GnuChanFileManager", NULL);
    app.config_path = g_build_filename(config_dir, "config.conf", NULL);
    g_free(config_dir);
    GKeyFile *key_file = g_key_file_new();
    if (g_key_file_load_from_file(key_file, app.config_path,
                                  G_KEY_FILE_NONE, NULL)) {
        app.show_hidden = g_key_file_get_boolean(key_file, "General",
                                                 "show_hidden", NULL);
        app.start_location = g_key_file_get_string(
            key_file, "General", "start_location", NULL);
    }
    g_key_file_unref(key_file);
    if (app.start_location == NULL) {
        app.start_location = g_strdup("~");
    }

    if (argc == 2 && g_str_equal(argv[1], "--version")) {
        g_print("GnuChan File Manager %s\n", APP_VERSION);
        app_free(&app);
        return 0;
    }
    GtkApplication *application = gtk_application_new(
        APP_ID, G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(application, "activate", G_CALLBACK(app_activate), &app);
    g_signal_connect(application, "open", G_CALLBACK(app_open), &app);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    app_free(&app);
    return status;
}
