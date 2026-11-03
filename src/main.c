#include "gfm_file_ops.h"

#include <gtk/gtk.h>
#include <string.h>

#define APP_ID "org.gnuchan.GnuChanFileManager"

enum { COL_NAME, COL_TYPE, COL_SIZE, COL_FILE, N_COLUMNS };
enum {
    SIDE_NAME,
    SIDE_FILE,
    SIDE_VOLUME,
    SIDE_MOUNT,
    SIDE_LOADED,
    N_SIDE_COLUMNS
};

typedef struct {
    GFile *file;
    gboolean move;
} ClipboardFile;

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
    GtkWidget *sidebar;
    GtkTreeStore *sidebar_store;
    GVolumeMonitor *volume_monitor;
    GFile *location;
    GPtrArray *history;
    guint history_index;
    gboolean show_hidden;
    gchar *config_path;
    gchar *start_location;
    gulong volume_added_handler;
    gulong volume_removed_handler;
    gulong mount_added_handler;
    gulong mount_removed_handler;
} App;

static void on_back(GtkButton *button, gpointer data);
static void on_forward(GtkButton *button, gpointer data);
static void on_up(GtkButton *button, gpointer data);
static void on_volume_mounted(GObject *source, GAsyncResult *result,
                              gpointer data);

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

static void clipboard_file_free(gpointer data) {
    ClipboardFile *clipboard_file = data;
    g_clear_object(&clipboard_file->file);
    g_free(clipboard_file);
}

static void clipboard_get_data(GtkClipboard *clipboard,
                               GtkSelectionData *selection_data,
                               guint info, gpointer data) {
    (void)clipboard;
    ClipboardFile *clipboard_file = data;
    gchar *uri = g_file_get_uri(clipboard_file->file);
    gchar *contents = info == 0
                          ? g_strdup_printf("%s\n%s\n",
                                            clipboard_file->move ? "cut" :
                                                                   "copy",
                                            uri)
                          : g_strdup_printf("%s\r\n", uri);
    gtk_selection_data_set(selection_data,
                           gtk_selection_data_get_target(selection_data), 8,
                           (const guchar *)contents, (gint)strlen(contents));
    g_free(contents);
    g_free(uri);
}

static void clipboard_clear_data(GtkClipboard *clipboard, gpointer data) {
    (void)clipboard;
    clipboard_file_free(data);
}

static void set_file_clipboard(App *app, gboolean move) {
    GFile *file = selected_file(app);
    if (file == NULL) {
        return;
    }
    static GtkTargetEntry targets[] = {
        {(gchar *)"x-special/gnome-copied-files", 0, 0},
        {(gchar *)"text/uri-list", 0, 1}
    };
    ClipboardFile *clipboard_file = g_new0(ClipboardFile, 1);
    clipboard_file->file = file;
    clipboard_file->move = move;
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (!gtk_clipboard_set_with_data(clipboard, targets, G_N_ELEMENTS(targets),
                                     clipboard_get_data,
                                     clipboard_clear_data, clipboard_file)) {
        clipboard_file_free(clipboard_file);
    }
}

static gboolean paste_uri_list(App *app, const gchar *contents,
                               gboolean move, GFile *destination_directory) {
    GPtrArray *files = gfm_parse_uri_list(contents, &move);
    if (files == NULL) {
        return FALSE;
    }
    gboolean changed = FALSE;
    GError *error = NULL;
    for (guint i = 0; i < files->len; i++) {
        GFile *source = g_ptr_array_index(files, i);
        gboolean success = move
                               ? gfm_move_to_directory(source,
                                                       destination_directory,
                                                       &error)
                               : gfm_copy_to_directory(source,
                                                       destination_directory,
                                                       &error);
        if (!success) {
            break;
        }
        changed = TRUE;
    }
    g_ptr_array_unref(files);
    if (error != NULL) {
        show_error(app, "Could not paste the selected item.", error);
        g_clear_error(&error);
    } else if (changed) {
        refresh_listing(app);
    }
    return changed;
}

static void paste_clipboard(App *app) {
    GFile *destination = NULL;
    GFile *selection = selected_file(app);
    if (selection != NULL &&
        g_file_query_file_type(selection, G_FILE_QUERY_INFO_NONE, NULL) ==
            G_FILE_TYPE_DIRECTORY) {
        destination = g_object_ref(selection);
    } else {
        destination = g_object_ref(app->location);
    }
    g_clear_object(&selection);

    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    GdkAtom gnome_target =
        gdk_atom_intern_static_string("x-special/gnome-copied-files");
    GtkSelectionData *selection_data =
        gtk_clipboard_wait_for_contents(clipboard, gnome_target);
    if (selection_data == NULL) {
        selection_data = gtk_clipboard_wait_for_contents(
            clipboard, gdk_atom_intern_static_string("text/uri-list"));
    }
    if (selection_data != NULL) {
        gint length = gtk_selection_data_get_length(selection_data);
        const guchar *bytes = gtk_selection_data_get_data(selection_data);
        if (length > 0 && bytes != NULL) {
            gchar *contents = g_strndup((const gchar *)bytes, (gsize)length);
            paste_uri_list(app, contents, FALSE, destination);
            g_free(contents);
        }
        gtk_selection_data_free(selection_data);
    }
    g_object_unref(destination);
}

static void open_selected(App *app) {
    GFile *file = selected_file(app);
    if (file == NULL) {
        return;
    }
    GError *error = NULL;
    if (g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL) ==
        G_FILE_TYPE_DIRECTORY) {
        go_to(app, file, TRUE);
    } else {
        gchar *uri = g_file_get_uri(file);
        g_app_info_launch_default_for_uri(uri, NULL, &error);
        g_free(uri);
    }
    if (error != NULL) {
        show_error(app, "Could not open the selected item.", error);
        g_clear_error(&error);
    }
    g_object_unref(file);
}

static void on_file_context_action(GtkMenuItem *item, gpointer data) {
    App *app = data;
    const gchar *action = g_object_get_data(G_OBJECT(item), "action");
    if (g_str_equal(action, "open")) {
        open_selected(app);
    } else if (g_str_equal(action, "copy-clipboard")) {
        set_file_clipboard(app, FALSE);
    } else if (g_str_equal(action, "cut")) {
        set_file_clipboard(app, TRUE);
    } else if (g_str_equal(action, "paste")) {
        paste_clipboard(app);
    } else if (g_str_equal(action, "copy") || g_str_equal(action, "move") ||
               g_str_equal(action, "rename") ||
               g_str_equal(action, "trash") ||
               g_str_equal(action, "delete")) {
        do_operation(app, action);
    }
}

static void append_context_item(GtkWidget *menu, const gchar *label,
                                const gchar *action, App *app) {
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_object_set_data(G_OBJECT(item), "action", (gpointer)action);
    g_signal_connect(item, "activate", G_CALLBACK(on_file_context_action),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static gboolean on_file_button_press(GtkWidget *widget, GdkEventButton *event,
                                     gpointer data) {
    if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_SECONDARY) {
        return FALSE;
    }
    App *app = data;
    GtkTreePath *path = NULL;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (gint)event->x,
                                      (gint)event->y, &path, NULL, NULL,
                                      NULL)) {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(
            GTK_TREE_VIEW(widget));
        gtk_tree_selection_select_path(selection, path);
        gtk_tree_path_free(path);
    } else {
        gtk_tree_selection_unselect_all(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)));
    }
    GtkWidget *menu = gtk_menu_new();
    GFile *selected = selected_file(app);
    gboolean has_selection = selected != NULL;
    g_clear_object(&selected);
    if (has_selection) {
        append_context_item(menu, "Open", "open", app);
        append_context_item(menu, "Copy", "copy-clipboard", app);
        append_context_item(menu, "Cut", "cut", app);
    }
    append_context_item(menu, "Paste", "paste", app);
    if (has_selection) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
        append_context_item(menu, "Copy to…", "copy", app);
        append_context_item(menu, "Move to…", "move", app);
        append_context_item(menu, "Rename…", "rename", app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
        append_context_item(menu, "Move to Trash", "trash", app);
        append_context_item(menu, "Delete Permanently…", "delete", app);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    return TRUE;
}

static gboolean on_window_key_press(GtkWidget *widget, GdkEventKey *event,
                                    gpointer data) {
    (void)widget;
    App *app = data;
    GdkModifierType modifiers = event->state & gtk_accelerator_get_default_mod_mask();
    gboolean control = (modifiers & GDK_CONTROL_MASK) != 0;
    gboolean shift = (modifiers & GDK_SHIFT_MASK) != 0;
    gboolean alt = (modifiers & GDK_MOD1_MASK) != 0;
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(app->window));
    gboolean list_focused = focus == app->view;

    if (control && event->keyval == GDK_KEY_l) {
        gtk_widget_grab_focus(app->address);
        gtk_editable_select_region(GTK_EDITABLE(app->address), 0, -1);
        return TRUE;
    }
    if (control && event->keyval == GDK_KEY_h) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->hidden_button),
                                     !app->show_hidden);
        return TRUE;
    }
    if (alt && event->keyval == GDK_KEY_Left) {
        on_back(NULL, app);
        return TRUE;
    }
    if (alt && event->keyval == GDK_KEY_Right) {
        on_forward(NULL, app);
        return TRUE;
    }
    if (alt && event->keyval == GDK_KEY_Up) {
        on_up(NULL, app);
        return TRUE;
    }
    if (!list_focused) {
        return FALSE;
    }
    if (control && event->keyval == GDK_KEY_c) {
        set_file_clipboard(app, FALSE);
        return TRUE;
    }
    if (control && event->keyval == GDK_KEY_x) {
        set_file_clipboard(app, TRUE);
        return TRUE;
    }
    if (control && event->keyval == GDK_KEY_v) {
        paste_clipboard(app);
        return TRUE;
    }
    if (event->keyval == GDK_KEY_F2) {
        do_operation(app, "rename");
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Delete) {
        do_operation(app, shift ? "delete" : "trash");
        return TRUE;
    }
    return FALSE;
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

static void sidebar_add_placeholder(App *app, GtkTreeIter *parent) {
    GtkTreeIter child;
    gtk_tree_store_append(app->sidebar_store, &child, parent);
    gtk_tree_store_set(app->sidebar_store, &child, SIDE_NAME, "", SIDE_LOADED,
                       TRUE, -1);
}

static void sidebar_add_root(App *app, const gchar *name, GFile *file,
                             GVolume *volume, GMount *mount) {
    GtkTreeIter iter;
    gtk_tree_store_append(app->sidebar_store, &iter, NULL);
    gtk_tree_store_set(app->sidebar_store, &iter, SIDE_NAME, name, SIDE_FILE,
                       file, SIDE_VOLUME, volume, SIDE_MOUNT, mount,
                       SIDE_LOADED, FALSE, -1);
    GMount *volume_mount = volume != NULL ? g_volume_get_mount(volume) : NULL;
    if (file != NULL ||
        (volume != NULL && (g_volume_can_mount(volume) || volume_mount != NULL))) {
        sidebar_add_placeholder(app, &iter);
    }
    g_clear_object(&volume_mount);
}

static void refresh_sidebar(App *app) {
    if (app->sidebar_store == NULL) {
        return;
    }
    gtk_tree_store_clear(app->sidebar_store);
    GFile *home = g_file_new_for_path(g_get_home_dir());
    GFile *root = g_file_new_for_path(G_DIR_SEPARATOR_S);
    sidebar_add_root(app, "Home", home, NULL, NULL);
    sidebar_add_root(app, "File System", root, NULL, NULL);
    g_object_unref(home);
    g_object_unref(root);

    GList *volumes = g_volume_monitor_get_volumes(app->volume_monitor);
    for (GList *item = volumes; item != NULL; item = item->next) {
        GVolume *volume = item->data;
        GMount *mount = g_volume_get_mount(volume);
        if (mount != NULL || g_volume_can_mount(volume)) {
            gchar *name = g_volume_get_name(volume);
            GFile *mount_root = mount != NULL ? g_mount_get_root(mount) : NULL;
            sidebar_add_root(app, name, mount_root, volume, mount);
            g_free(name);
            g_clear_object(&mount_root);
        }
        g_clear_object(&mount);
    }
    g_list_free_full(volumes, g_object_unref);

    GList *mounts = g_volume_monitor_get_mounts(app->volume_monitor);
    for (GList *item = mounts; item != NULL; item = item->next) {
        GMount *mount = item->data;
        GVolume *volume = g_mount_get_volume(mount);
        if (volume == NULL &&
            (g_mount_can_unmount(mount) || g_mount_can_eject(mount))) {
            gchar *name = g_mount_get_name(mount);
            gchar *label = g_strdup_printf("Mounted: %s", name);
            GFile *mount_root = g_mount_get_root(mount);
            sidebar_add_root(app, label, mount_root, NULL, mount);
            g_object_unref(mount_root);
            g_free(label);
            g_free(name);
        }
        g_clear_object(&volume);
    }
    g_list_free_full(mounts, g_object_unref);
}

static void on_sidebar_row_expanded(GtkTreeView *tree_view, GtkTreeIter *iter,
                                    GtkTreePath *path, gpointer data) {
    (void)tree_view;
    (void)path;
    App *app = data;
    gboolean loaded = FALSE;
    GFile *directory = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->sidebar_store), iter,
                       SIDE_LOADED, &loaded, SIDE_FILE, &directory, -1);
    if (loaded || directory == NULL) {
        g_clear_object(&directory);
        return;
    }
    GtkTreeIter child;
    while (gtk_tree_model_iter_children(GTK_TREE_MODEL(app->sidebar_store),
                                        &child, iter)) {
        gtk_tree_store_remove(app->sidebar_store, &child);
    }
    gtk_tree_store_set(app->sidebar_store, iter, SIDE_LOADED, TRUE, -1);

    GError *error = NULL;
    GFileEnumerator *enumerator = g_file_enumerate_children(
        directory, G_FILE_ATTRIBUTE_STANDARD_NAME ","
                   G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
    if (enumerator != NULL) {
        GFileInfo *info;
        while ((info = g_file_enumerator_next_file(enumerator, NULL, &error)) !=
               NULL) {
            if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
                const gchar *name = g_file_info_get_name(info);
                GFile *file = g_file_get_child(directory, name);
                GtkTreeIter subdirectory;
                gtk_tree_store_append(app->sidebar_store, &subdirectory, iter);
                gtk_tree_store_set(app->sidebar_store, &subdirectory,
                                   SIDE_NAME, name, SIDE_FILE, file,
                                   SIDE_LOADED, FALSE, -1);
                sidebar_add_placeholder(app, &subdirectory);
                g_object_unref(file);
            }
            g_object_unref(info);
        }
        if (error != NULL) {
            g_clear_error(&error);
        }
        g_object_unref(enumerator);
    }
    g_clear_object(&directory);
}

static void on_sidebar_row_activated(GtkTreeView *tree_view, GtkTreePath *path,
                                    GtkTreeViewColumn *column, gpointer data) {
    (void)column;
    App *app = data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->sidebar_store), &iter,
                                 path)) {
        return;
    }
    GFile *file = NULL;
    GVolume *volume = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->sidebar_store), &iter,
                       SIDE_FILE, &file, SIDE_VOLUME, &volume, -1);
    if (file != NULL) {
        go_to(app, file, TRUE);
    } else if (volume != NULL && g_volume_can_mount(volume)) {
        g_volume_mount(volume, G_MOUNT_MOUNT_NONE, NULL, NULL,
                       on_volume_mounted, app);
    }
    g_clear_object(&file);
    g_clear_object(&volume);
    (void)tree_view;
}

static void on_volume_mounted(GObject *source, GAsyncResult *result,
                              gpointer data) {
    App *app = data;
    GError *error = NULL;
    if (!g_volume_mount_finish(G_VOLUME(source), result, &error)) {
        show_error(app, "Could not mount this device.", error);
        g_clear_error(&error);
        return;
    }
    GMount *mount = g_volume_get_mount(G_VOLUME(source));
    if (mount != NULL) {
        GFile *root = g_mount_get_root(mount);
        go_to(app, root, TRUE);
        g_object_unref(root);
        g_object_unref(mount);
    }
    refresh_sidebar(app);
}

static void on_mount_unmounted(GObject *source, GAsyncResult *result,
                               gpointer data) {
    App *app = data;
    GError *error = NULL;
    if (!g_mount_unmount_with_operation_finish(G_MOUNT(source), result,
                                               &error)) {
        show_error(app, "Could not unmount this device.", error);
        g_clear_error(&error);
        return;
    }
    refresh_sidebar(app);
}

static void on_mount_ejected(GObject *source, GAsyncResult *result,
                             gpointer data) {
    App *app = data;
    GError *error = NULL;
    if (!g_mount_eject_with_operation_finish(G_MOUNT(source), result,
                                             &error)) {
        show_error(app, "Could not eject this device.", error);
        g_clear_error(&error);
        return;
    }
    refresh_sidebar(app);
}

static void on_device_menu_action(GtkMenuItem *item, gpointer data) {
    App *app = data;
    const gchar *action = g_object_get_data(G_OBJECT(item), "action");
    GVolume *volume = g_object_get_data(G_OBJECT(item), "volume");
    GMount *mount = g_object_get_data(G_OBJECT(item), "mount");
    if (g_str_equal(action, "mount") && volume != NULL) {
        g_volume_mount(volume, G_MOUNT_MOUNT_NONE, NULL, NULL,
                       on_volume_mounted, app);
    } else if (g_str_equal(action, "unmount")) {
        GMount *target = mount != NULL ? g_object_ref(mount) :
                         volume != NULL ? g_volume_get_mount(volume) : NULL;
        if (target != NULL) {
            g_mount_unmount_with_operation(target, G_MOUNT_UNMOUNT_NONE, NULL,
                                           NULL, on_mount_unmounted, app);
            g_object_unref(target);
        }
    } else if (g_str_equal(action, "eject") && mount != NULL) {
        g_mount_eject_with_operation(mount, G_MOUNT_UNMOUNT_NONE, NULL, NULL,
                                     on_mount_ejected, app);
    }
}

static void append_device_menu_item(GtkWidget *menu, const gchar *label,
                                    const gchar *action, GVolume *volume,
                                    GMount *mount, App *app) {
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_object_set_data(G_OBJECT(item), "action", (gpointer)action);
    if (volume != NULL) {
        g_object_set_data_full(G_OBJECT(item), "volume",
                               g_object_ref(volume), g_object_unref);
    }
    if (mount != NULL) {
        g_object_set_data_full(G_OBJECT(item), "mount",
                               g_object_ref(mount), g_object_unref);
    }
    g_signal_connect(item, "activate", G_CALLBACK(on_device_menu_action), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static gboolean on_sidebar_button_press(GtkWidget *widget,
                                        GdkEventButton *event,
                                        gpointer data) {
    if (event->type != GDK_BUTTON_PRESS ||
        event->button != GDK_BUTTON_SECONDARY) {
        return FALSE;
    }
    App *app = data;
    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (gint)event->x,
                                       (gint)event->y, &path, NULL, NULL,
                                       NULL)) {
        return FALSE;
    }
    GtkTreeIter iter;
    GVolume *volume = NULL;
    GMount *mount = NULL;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->sidebar_store), &iter,
                                 path)) {
        gtk_tree_path_free(path);
        return FALSE;
    }
    gtk_tree_model_get(GTK_TREE_MODEL(app->sidebar_store), &iter,
                       SIDE_VOLUME, &volume, SIDE_MOUNT, &mount, -1);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(widget), path, NULL, FALSE);
    gtk_tree_path_free(path);
    if (volume == NULL && mount == NULL) {
        return FALSE;
    }
    GtkWidget *menu = gtk_menu_new();
    GMount *actual_mount = mount != NULL ? g_object_ref(mount) :
                           volume != NULL ? g_volume_get_mount(volume) : NULL;
    if (actual_mount != NULL) {
        if (g_mount_can_unmount(actual_mount)) {
            append_device_menu_item(menu, "Unmount", "unmount", volume,
                                    actual_mount, app);
        }
        if (g_mount_can_eject(actual_mount)) {
            append_device_menu_item(menu, "Eject", "eject", volume,
                                    actual_mount, app);
        }
        g_object_unref(actual_mount);
    } else if (volume != NULL && g_volume_can_mount(volume)) {
        append_device_menu_item(menu, "Mount", "mount", volume, NULL, app);
    }
    GList *items = gtk_container_get_children(GTK_CONTAINER(menu));
    gboolean has_actions = items != NULL;
    g_list_free(items);
    if (!has_actions) {
        gtk_widget_destroy(menu);
        g_clear_object(&mount);
        g_clear_object(&volume);
        return FALSE;
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    g_clear_object(&mount);
    g_clear_object(&volume);
    return TRUE;
}

static void on_volume_monitor_changed(GVolumeMonitor *monitor, GObject *object,
                                      gpointer data) {
    (void)monitor;
    (void)object;
    refresh_sidebar(data);
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

    app->sidebar_store = gtk_tree_store_new(
        N_SIDE_COLUMNS, G_TYPE_STRING, G_TYPE_FILE, G_TYPE_VOLUME, G_TYPE_MOUNT,
        G_TYPE_BOOLEAN);
    app->sidebar = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(app->sidebar_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app->sidebar), FALSE);
    add_text_column(GTK_TREE_VIEW(app->sidebar), "Places", SIDE_NAME, TRUE);
    g_signal_connect(app->sidebar, "row-expanded",
                     G_CALLBACK(on_sidebar_row_expanded), app);
    g_signal_connect(app->sidebar, "row-activated",
                     G_CALLBACK(on_sidebar_row_activated), app);
    g_signal_connect(app->sidebar, "button-press-event",
                     G_CALLBACK(on_sidebar_button_press), app);

    GtkWidget *sidebar_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sidebar_scroll), app->sidebar);
    gtk_widget_set_size_request(sidebar_scroll, 190, -1);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(outer), paned, TRUE, TRUE, 0);
    gtk_paned_pack1(GTK_PANED(paned), sidebar_scroll, FALSE, FALSE);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), app->view);
    gtk_paned_pack2(GTK_PANED(paned), scroll, TRUE, FALSE);
    g_signal_connect(app->view, "button-press-event",
                     G_CALLBACK(on_file_button_press), app);
    g_signal_connect(app->window, "key-press-event",
                     G_CALLBACK(on_window_key_press), app);

    app->volume_monitor = g_volume_monitor_get();
    app->volume_added_handler = g_signal_connect(
        app->volume_monitor, "volume-added",
        G_CALLBACK(on_volume_monitor_changed), app);
    app->volume_removed_handler = g_signal_connect(
        app->volume_monitor, "volume-removed",
        G_CALLBACK(on_volume_monitor_changed), app);
    app->mount_added_handler = g_signal_connect(
        app->volume_monitor, "mount-added",
        G_CALLBACK(on_volume_monitor_changed), app);
    app->mount_removed_handler = g_signal_connect(
        app->volume_monitor, "mount-removed",
        G_CALLBACK(on_volume_monitor_changed), app);
    refresh_sidebar(app);
    gtk_widget_show_all(app->window);
    GtkTreePath *home_path = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(app->sidebar), home_path, FALSE);
    gtk_tree_path_free(home_path);

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
    if (app->volume_monitor != NULL) {
        if (app->volume_added_handler != 0) {
            g_signal_handler_disconnect(app->volume_monitor,
                                        app->volume_added_handler);
        }
        if (app->volume_removed_handler != 0) {
            g_signal_handler_disconnect(app->volume_monitor,
                                        app->volume_removed_handler);
        }
        if (app->mount_added_handler != 0) {
            g_signal_handler_disconnect(app->volume_monitor,
                                        app->mount_added_handler);
        }
        if (app->mount_removed_handler != 0) {
            g_signal_handler_disconnect(app->volume_monitor,
                                        app->mount_removed_handler);
        }
    }
    g_clear_object(&app->location);
    g_clear_object(&app->store);
    g_clear_object(&app->sidebar_store);
    g_clear_object(&app->volume_monitor);
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
