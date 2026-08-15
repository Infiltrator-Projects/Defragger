// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"

#include <gtk/gtk.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LDTM_DEVICE_COL_PATH 0
#define LDTM_DEVICE_COL_DISPLAY 1
#define LDTM_DEVICE_COL_SAFE 2
#define LDTM_DEVICE_COL_SUMMARY 3
#define LDTM_DEVICE_N_COLUMNS 4

enum {
    LDTM_FS_COL_KEY = 0,
    LDTM_FS_COL_LABEL,
    LDTM_FS_COL_SIZE,
    LDTM_FS_COL_PAYLOAD,
    LDTM_FS_COL_CREATOR,
    LDTM_FS_COL_STATUS,
    LDTM_FS_N_COLUMNS
};

typedef struct {
    GtkWidget *window;
    GtkComboBox *device_combo;
    GtkListStore *device_store;
    GtkWidget *device_summary;
    GtkListStore *filesystem_store;
    GtkWidget *build_button;
    GtkWidget *verify_button;
    GtkWidget *progress;
    GtkTextBuffer *log_buffer;
    GPid child_pid;
    GIOChannel *stdout_channel;
    GIOChannel *stderr_channel;
    guint stdout_watch;
    guint stderr_watch;
    guint child_watch;
    guint pulse_timer;
    gboolean worker_running;
} LdtmApp;

static char *pair_value(const char *line, const char *key) {
    char *needle;
    const char *found;
    const char *cursor;
    GString *value;
    needle = g_strdup_printf("%s=\"", key);
    found = strstr(line, needle);
    g_free(needle);
    if (found == NULL) return g_strdup("");
    cursor = strchr(found, '"');
    if (cursor == NULL) return g_strdup("");
    ++cursor;
    value = g_string_new(NULL);
    while (*cursor != '\0' && *cursor != '"') {
        if (cursor[0] == '\\' && cursor[1] == 'x' &&
            g_ascii_isxdigit(cursor[2]) && g_ascii_isxdigit(cursor[3])) {
            char hex[3] = {cursor[2], cursor[3], '\0'};
            g_string_append_c(value, (char)strtoul(hex, NULL, 16));
            cursor += 4;
        } else if (cursor[0] == '\\' && cursor[1] != '\0') {
            g_string_append_c(value, cursor[1]);
            cursor += 2;
        } else {
            g_string_append_c(value, *cursor);
            ++cursor;
        }
    }
    return g_string_free(value, FALSE);
}

static void show_message(GtkWindow *parent, GtkMessageType type,
                         const char *primary, const char *secondary) {
    GtkWidget *dialog = gtk_message_dialog_new(parent,
                                                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                type, GTK_BUTTONS_CLOSE, "%s", primary);
    if (secondary != NULL && *secondary != '\0') {
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", secondary);
    }
    (void)gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void append_log(LdtmApp *app, const char *text) {
    GtkTextIter end;
    GtkTextMark *mark;
    GtkTextView *view = GTK_TEXT_VIEW(g_object_get_data(G_OBJECT(app->log_buffer), "view"));
    gtk_text_buffer_get_end_iter(app->log_buffer, &end);
    gtk_text_buffer_insert(app->log_buffer, &end, text, -1);
    gtk_text_buffer_get_end_iter(app->log_buffer, &end);
    mark = gtk_text_buffer_create_mark(app->log_buffer, NULL, &end, FALSE);
    if (view != NULL) gtk_text_view_scroll_mark_onscreen(view, mark);
    gtk_text_buffer_delete_mark(app->log_buffer, mark);
}

static void update_filesystem_status(LdtmApp *app, const char *key,
                                     const char *status, const char *detail) {
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->filesystem_store), &iter);
    while (valid) {
        char *row_key = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app->filesystem_store), &iter,
                           LDTM_FS_COL_KEY, &row_key, -1);
        if (g_strcmp0(row_key, key) == 0) {
            char *display = (detail != NULL && *detail != '\0')
                                ? g_strdup_printf("%s — %s", status, detail)
                                : g_strdup(status);
            gtk_list_store_set(app->filesystem_store, &iter,
                               LDTM_FS_COL_STATUS, display, -1);
            g_free(display);
            g_free(row_key);
            return;
        }
        g_free(row_key);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->filesystem_store), &iter);
    }
}

static void parse_worker_status(LdtmApp *app, const char *line) {
    if (g_str_has_prefix(line, "LDTM_STATUS\t")) {
        gchar **fields = g_strsplit(line, "\t", 4);
        if (g_strv_length(fields) >= 4U) {
            g_strchomp(fields[3]);
            update_filesystem_status(app, fields[1], fields[2], fields[3]);
        }
        g_strfreev(fields);
    }
}

static gboolean channel_watch(GIOChannel *channel, GIOCondition condition, gpointer user_data) {
    LdtmApp *app = (LdtmApp *)user_data;
    if ((condition & (G_IO_IN | G_IO_HUP)) != 0) {
        for (;;) {
            gchar *line = NULL;
            gsize length = 0U;
            GIOStatus status = g_io_channel_read_line(channel, &line, &length, NULL, NULL);
            if (status == G_IO_STATUS_NORMAL && line != NULL) {
                (void)length;
                append_log(app, line);
                parse_worker_status(app, line);
                g_free(line);
                continue;
            }
            g_free(line);
            if (status == G_IO_STATUS_AGAIN) return TRUE;
            break;
        }
    }
    return (condition & (G_IO_ERR | G_IO_NVAL | G_IO_HUP)) == 0;
}

static gboolean pulse_progress(gpointer user_data) {
    LdtmApp *app = (LdtmApp *)user_data;
    if (!app->worker_running) return G_SOURCE_REMOVE;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(app->progress));
    return G_SOURCE_CONTINUE;
}

static void set_worker_controls(LdtmApp *app, gboolean running) {
    GtkTreeIter iter;
    gboolean safe = FALSE;
    app->worker_running = running;
    if (gtk_combo_box_get_active_iter(app->device_combo, &iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->device_store), &iter,
                           LDTM_DEVICE_COL_SAFE, &safe, -1);
    }
    gtk_widget_set_sensitive(app->build_button, !running && safe);
    gtk_widget_set_sensitive(app->verify_button, !running && safe);
    gtk_widget_set_sensitive(GTK_WIDGET(app->device_combo), !running);
    if (running) {
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), "Working…");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);
        if (app->pulse_timer == 0U) app->pulse_timer = g_timeout_add(120U, pulse_progress, app);
    } else {
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app->progress), "Idle");
        if (app->pulse_timer != 0U) {
            g_source_remove(app->pulse_timer);
            app->pulse_timer = 0U;
        }
    }
}

static void cleanup_channels(LdtmApp *app) {
    if (app->stdout_watch != 0U) {
        g_source_remove(app->stdout_watch);
        app->stdout_watch = 0U;
    }
    if (app->stderr_watch != 0U) {
        g_source_remove(app->stderr_watch);
        app->stderr_watch = 0U;
    }
    if (app->stdout_channel != NULL) {
        g_io_channel_unref(app->stdout_channel);
        app->stdout_channel = NULL;
    }
    if (app->stderr_channel != NULL) {
        g_io_channel_unref(app->stderr_channel);
        app->stderr_channel = NULL;
    }
}

static void worker_finished(GPid pid, gint status, gpointer user_data) {
    LdtmApp *app = (LdtmApp *)user_data;
    char message[128];
    cleanup_channels(app);
    app->child_watch = 0U;
    app->child_pid = 0;
    g_spawn_close_pid(pid);
    set_worker_controls(app, FALSE);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 1.0);
        (void)snprintf(message, sizeof(message), "\nWorker completed successfully.\n");
        append_log(app, message);
    } else {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress), 0.0);
        (void)snprintf(message, sizeof(message), "\nWorker failed (status %d).\n", status);
        append_log(app, message);
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR,
                     "Test-media operation failed",
                     "See the live log for the command that failed.");
    }
}

static char *selected_device(LdtmApp *app) {
    GtkTreeIter iter;
    char *path = NULL;
    if (!gtk_combo_box_get_active_iter(app->device_combo, &iter)) return NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->device_store), &iter,
                       LDTM_DEVICE_COL_PATH, &path, -1);
    return path;
}

static gboolean spawn_worker(LdtmApp *app, const char *operation,
                             const char *device, gboolean confirmed) {
    gchar *self = g_file_read_link("/proc/self/exe", NULL);
    gchar *argv[8];
    gint stdout_fd = -1;
    gint stderr_fd = -1;
    GError *error = NULL;
    guint arg = 0U;
    gboolean started;
    if (self == NULL) self = g_strdup("linux-defragger-test-media");
    argv[arg++] = g_strdup("pkexec");
    argv[arg++] = self;
    argv[arg++] = g_strdup("--worker");
    argv[arg++] = g_strdup(operation);
    argv[arg++] = g_strdup(device);
    if (confirmed) {
        argv[arg++] = g_strdup("--confirmed");
        argv[arg++] = g_strdup(device);
    }
    argv[arg] = NULL;
    started = g_spawn_async_with_pipes(NULL, argv, NULL,
                                       G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
                                       NULL, NULL, &app->child_pid,
                                       NULL, &stdout_fd, &stderr_fd, &error);
    for (guint index = 0U; index < arg; ++index) g_free(argv[index]);
    if (!started) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR,
                     "Could not start privileged worker",
                     error != NULL ? error->message : "Unknown process-launch error");
        g_clear_error(&error);
        return FALSE;
    }
    app->stdout_channel = g_io_channel_unix_new(stdout_fd);
    app->stderr_channel = g_io_channel_unix_new(stderr_fd);
    g_io_channel_set_close_on_unref(app->stdout_channel, TRUE);
    g_io_channel_set_close_on_unref(app->stderr_channel, TRUE);
    (void)g_io_channel_set_encoding(app->stdout_channel, NULL, NULL);
    (void)g_io_channel_set_encoding(app->stderr_channel, NULL, NULL);
    (void)g_io_channel_set_flags(app->stdout_channel,
                                 g_io_channel_get_flags(app->stdout_channel) | G_IO_FLAG_NONBLOCK, NULL);
    (void)g_io_channel_set_flags(app->stderr_channel,
                                 g_io_channel_get_flags(app->stderr_channel) | G_IO_FLAG_NONBLOCK, NULL);
    app->stdout_watch = g_io_add_watch(app->stdout_channel,
                                      G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                      channel_watch, app);
    app->stderr_watch = g_io_add_watch(app->stderr_channel,
                                      G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                                      channel_watch, app);
    app->child_watch = g_child_watch_add(app->child_pid, worker_finished, app);
    set_worker_controls(app, TRUE);
    return TRUE;
}

static gboolean confirmation_dialog(LdtmApp *app, const char *device) {
    GtkWidget *dialog;
    GtkWidget *content;
    GtkWidget *label;
    GtkWidget *entry;
    char *expected;
    gboolean accepted = FALSE;
    gint response;
    expected = g_strdup_printf("DESTROY %s", device);
    dialog = gtk_dialog_new_with_buttons("Destroy and build test disk",
                                         GTK_WINDOW(app->window),
                                         GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                         "_Cancel", GTK_RESPONSE_CANCEL,
                                         "_Destroy and Build", GTK_RESPONSE_ACCEPT,
                                         NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 560, -1);
    content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    label = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    {
        char *markup = g_markup_printf_escaped(
            "<b>This will erase the entire disk %s.</b>\n\n"
            "All existing partitions and files on that device will be destroyed. "
            "Type the exact confirmation below to continue:\n\n<b>%s</b>",
            device, expected);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
    }
    entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 10U);
    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 10U);
    gtk_widget_show_all(dialog);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_ACCEPT &&
        g_strcmp0(gtk_entry_get_text(GTK_ENTRY(entry)), expected) == 0) {
        accepted = TRUE;
    } else if (response == GTK_RESPONSE_ACCEPT) {
        show_message(GTK_WINDOW(dialog), GTK_MESSAGE_ERROR,
                     "Confirmation did not match",
                     "No disk changes were made.");
    }
    gtk_widget_destroy(dialog);
    g_free(expected);
    return accepted;
}

static void reset_filesystem_rows(LdtmApp *app) {
    size_t index;
    gtk_list_store_clear(app->filesystem_store);
    for (index = 0U; index < ldtm_spec_count(); ++index) {
        const LdtmFilesystemSpec *spec = &ldtm_specs()[index];
        GtkTreeIter iter;
        char size[32];
        char payload[32];
        char creator[256];
        const gboolean available = ldtm_spec_creator_available(spec, creator, sizeof(creator)) != 0;
        const char *status;
        (void)snprintf(size, sizeof(size), "%u MiB", spec->size_mib);
        if (spec->payload_mib == 0U) (void)snprintf(payload, sizeof(payload), "—");
        else (void)snprintf(payload, sizeof(payload), "%u MiB", spec->payload_mib);
        if (spec->creator == LDTM_CREATOR_MANUAL) status = "Reserved / manual";
        else status = available ? "Ready" : "Creator missing";
        gtk_list_store_append(app->filesystem_store, &iter);
        gtk_list_store_set(app->filesystem_store, &iter,
                           LDTM_FS_COL_KEY, spec->key,
                           LDTM_FS_COL_LABEL, spec->label,
                           LDTM_FS_COL_SIZE, size,
                           LDTM_FS_COL_PAYLOAD, payload,
                           LDTM_FS_COL_CREATOR, creator,
                           LDTM_FS_COL_STATUS, status,
                           -1);
    }
}

static void device_changed(GtkComboBox *combo, gpointer user_data) {
    LdtmApp *app = (LdtmApp *)user_data;
    GtkTreeIter iter;
    char *summary = NULL;
    gboolean safe = FALSE;
    (void)combo;
    if (gtk_combo_box_get_active_iter(app->device_combo, &iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->device_store), &iter,
                           LDTM_DEVICE_COL_SUMMARY, &summary,
                           LDTM_DEVICE_COL_SAFE, &safe, -1);
    }
    gtk_label_set_text(GTK_LABEL(app->device_summary), summary != NULL ? summary : "No disk selected.");
    g_free(summary);
    if (!app->worker_running) {
        gtk_widget_set_sensitive(app->build_button, safe);
        gtk_widget_set_sensitive(app->verify_button, safe);
    }
    reset_filesystem_rows(app);
}

static void refresh_devices(LdtmApp *app) {
    gchar *stdout_text = NULL;
    gchar *stderr_text = NULL;
    gint exit_status = 0;
    GError *error = NULL;
    gchar *argv[] = {
        (gchar *)"lsblk", (gchar *)"-d", (gchar *)"-b", (gchar *)"-n", (gchar *)"-P",
        (gchar *)"-o", (gchar *)"PATH,SIZE,MODEL,SERIAL,TRAN,RM,RO", NULL
    };
    gchar **lines;
    gint first_safe = -1;
    gint row = 0;
    gtk_list_store_clear(app->device_store);
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                      &stdout_text, &stderr_text, &exit_status, &error) ||
        !g_spawn_check_wait_status(exit_status, &error)) {
        show_message(GTK_WINDOW(app->window), GTK_MESSAGE_ERROR,
                     "Could not enumerate physical disks",
                     error != NULL ? error->message : stderr_text);
        g_clear_error(&error);
        g_free(stdout_text);
        g_free(stderr_text);
        return;
    }
    lines = g_strsplit(stdout_text, "\n", -1);
    for (guint index = 0U; lines[index] != NULL; ++index) {
        char *path;
        char *size_text;
        char *model;
        char *serial;
        char *transport;
        char *rm_text;
        char *ro_text;
        unsigned long long bytes;
        int removable;
        int readonly;
        gboolean system_disk;
        gboolean field_media;
        gboolean enough;
        gboolean safe;
        char *display;
        char *summary;
        GtkTreeIter iter;
        if (*lines[index] == '\0') continue;
        path = pair_value(lines[index], "PATH");
        size_text = pair_value(lines[index], "SIZE");
        model = pair_value(lines[index], "MODEL");
        serial = pair_value(lines[index], "SERIAL");
        transport = pair_value(lines[index], "TRAN");
        rm_text = pair_value(lines[index], "RM");
        ro_text = pair_value(lines[index], "RO");
        bytes = strtoull(size_text, NULL, 10);
        removable = atoi(rm_text);
        readonly = atoi(ro_text);
        system_disk = ldtm_is_system_disk(path) != 0;
        field_media = ldtm_transport_is_field_media(removable, transport) != 0;
        enough = bytes >= ldtm_required_capacity_bytes();
        safe = !system_disk && readonly == 0 && field_media && enough;
        display = g_strdup_printf("%s — %.1f GiB — %s%s",
                                  path, (double)bytes / (double)LDTM_GIB,
                                  (*model != '\0') ? model : "unknown model",
                                  system_disk ? " — PROTECTED SYSTEM DISK" :
                                  (safe ? " — field-media candidate" : ""));
        summary = g_strdup_printf(
            "Device: %s\nModel: %s\nSerial: %s\nSize: %.1f GiB\nTransport: %s  RM=%d  RO=%d\n%s",
            path, *model != '\0' ? model : "unknown", *serial != '\0' ? serial : "unknown",
            (double)bytes / (double)LDTM_GIB, *transport != '\0' ? transport : "unknown",
            removable, readonly,
            system_disk ? "Protected: this disk contains /, /boot or /boot/efi." :
            (!field_media ? "Not accepted as removable/USB/MMC field media." :
             (!enough ? "Too small for the full 18-partition test layout." :
              "Safety pre-check passed. Destructive worker will verify it again.")));
        gtk_list_store_append(app->device_store, &iter);
        gtk_list_store_set(app->device_store, &iter,
                           LDTM_DEVICE_COL_PATH, path,
                           LDTM_DEVICE_COL_DISPLAY, display,
                           LDTM_DEVICE_COL_SAFE, safe,
                           LDTM_DEVICE_COL_SUMMARY, summary,
                           -1);
        if (safe && first_safe < 0) first_safe = row;
        ++row;
        g_free(summary);
        g_free(display);
        g_free(ro_text);
        g_free(rm_text);
        g_free(transport);
        g_free(serial);
        g_free(model);
        g_free(size_text);
        g_free(path);
    }
    g_strfreev(lines);
    g_free(stdout_text);
    g_free(stderr_text);
    if (first_safe >= 0) gtk_combo_box_set_active(app->device_combo, first_safe);
    else if (row > 0) gtk_combo_box_set_active(app->device_combo, 0);
}

static void refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    refresh_devices((LdtmApp *)user_data);
}

static void build_clicked(GtkButton *button, gpointer user_data) {
    LdtmApp *app = (LdtmApp *)user_data;
    char *device = selected_device(app);
    (void)button;
    if (device == NULL) return;
    if (confirmation_dialog(app, device)) {
        gtk_text_buffer_set_text(app->log_buffer, "", -1);
        append_log(app, "Preparing destructive filesystem test media...\n");
        (void)spawn_worker(app, "prepare", device, TRUE);
    }
    g_free(device);
}

static void verify_clicked(GtkButton *button, gpointer user_data) {
    LdtmApp *app = (LdtmApp *)user_data;
    char *device = selected_device(app);
    (void)button;
    if (device == NULL) return;
    gtk_text_buffer_set_text(app->log_buffer, "", -1);
    append_log(app, "Verifying retained test payloads against the C-generated manifest...\n");
    (void)spawn_worker(app, "verify", device, FALSE);
    g_free(device);
}

static GtkWidget *make_tree_view(LdtmApp *app) {
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->filesystem_store));
    static const struct {
        const char *title;
        int column;
    } columns[] = {
        {"Filesystem", LDTM_FS_COL_KEY}, {"Partition label", LDTM_FS_COL_LABEL},
        {"Size", LDTM_FS_COL_SIZE}, {"Payload", LDTM_FS_COL_PAYLOAD},
        {"Creator", LDTM_FS_COL_CREATOR}, {"Status", LDTM_FS_COL_STATUS}
    };
    for (size_t index = 0U; index < sizeof(columns) / sizeof(columns[0]); ++index) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
            columns[index].title, renderer, "text", columns[index].column, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
    }
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), TRUE);
    return view;
}

static GtkWidget *make_device_combo(LdtmApp *app) {
    GtkWidget *combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(app->device_store));
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), renderer, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo), renderer,
                                   "text", LDTM_DEVICE_COL_DISPLAY, NULL);
    return combo;
}

static void window_destroyed(GtkWidget *widget, gpointer user_data) {
    LdtmApp *app = (LdtmApp *)user_data;
    (void)widget;
    if (app->child_pid != 0) (void)kill(app->child_pid, SIGTERM);
    gtk_main_quit();
}

int ldtm_gui_main(int argc, char **argv) {
    LdtmApp app;
    GtkWidget *outer;
    GtkWidget *warning;
    GtkWidget *device_row;
    GtkWidget *refresh_button;
    GtkWidget *fs_scroll;
    GtkWidget *tree;
    GtkWidget *action_row;
    GtkWidget *log_scroll;
    GtkWidget *log_view;
    memset(&app, 0, sizeof(app));
    gtk_init(&argc, &argv);

    app.device_store = gtk_list_store_new(LDTM_DEVICE_N_COLUMNS,
                                          G_TYPE_STRING, G_TYPE_STRING,
                                          G_TYPE_BOOLEAN, G_TYPE_STRING);
    app.filesystem_store = gtk_list_store_new(LDTM_FS_N_COLUMNS,
                                              G_TYPE_STRING, G_TYPE_STRING,
                                              G_TYPE_STRING, G_TYPE_STRING,
                                              G_TYPE_STRING, G_TYPE_STRING);
    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Linux Defragger Test Media");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 1050, 760);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    g_signal_connect(app.window, "destroy", G_CALLBACK(window_destroyed), &app);

    outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12U);
    gtk_container_add(GTK_CONTAINER(app.window), outer);

    warning = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(warning),
        "<b>Dedicated destructive test-media builder</b> — separate from Linux Defragger. "
        "It creates sacrificial filesystems and fragmented deterministic payloads for field testing.");
    gtk_label_set_line_wrap(GTK_LABEL(warning), TRUE);
    gtk_label_set_xalign(GTK_LABEL(warning), 0.0F);
    gtk_box_pack_start(GTK_BOX(outer), warning, FALSE, FALSE, 0U);

    device_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(outer), device_row, FALSE, FALSE, 0U);
    gtk_box_pack_start(GTK_BOX(device_row), gtk_label_new("Physical test disk:"), FALSE, FALSE, 0U);
    app.device_combo = GTK_COMBO_BOX(make_device_combo(&app));
    gtk_box_pack_start(GTK_BOX(device_row), GTK_WIDGET(app.device_combo), TRUE, TRUE, 0U);
    refresh_button = gtk_button_new_with_label("Refresh Disks");
    gtk_box_pack_start(GTK_BOX(device_row), refresh_button, FALSE, FALSE, 0U);
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(refresh_clicked), &app);
    g_signal_connect(app.device_combo, "changed", G_CALLBACK(device_changed), &app);

    app.device_summary = gtk_label_new("No disk selected.");
    gtk_label_set_xalign(GTK_LABEL(app.device_summary), 0.0F);
    gtk_label_set_selectable(GTK_LABEL(app.device_summary), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), app.device_summary, FALSE, FALSE, 0U);

    reset_filesystem_rows(&app);
    tree = make_tree_view(&app);
    fs_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fs_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(fs_scroll, -1, 300);
    gtk_container_add(GTK_CONTAINER(fs_scroll), tree);
    gtk_box_pack_start(GTK_BOX(outer), fs_scroll, TRUE, TRUE, 0U);

    action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.build_button = gtk_button_new_with_label("Build Test Disk");
    app.verify_button = gtk_button_new_with_label("Verify After Defrag");
    gtk_widget_set_sensitive(app.build_button, FALSE);
    gtk_widget_set_sensitive(app.verify_button, FALSE);
    gtk_box_pack_start(GTK_BOX(action_row), app.build_button, FALSE, FALSE, 0U);
    gtk_box_pack_start(GTK_BOX(action_row), app.verify_button, FALSE, FALSE, 0U);
    gtk_box_pack_start(GTK_BOX(outer), action_row, FALSE, FALSE, 0U);
    g_signal_connect(app.build_button, "clicked", G_CALLBACK(build_clicked), &app);
    g_signal_connect(app.verify_button, "clicked", G_CALLBACK(verify_clicked), &app);

    app.progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app.progress), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app.progress), "Idle");
    gtk_box_pack_start(GTK_BOX(outer), app.progress, FALSE, FALSE, 0U);

    log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_view), GTK_WRAP_WORD_CHAR);
    app.log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));
    g_object_set_data(G_OBJECT(app.log_buffer), "view", log_view);
    log_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(log_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(log_scroll, -1, 190);
    gtk_container_add(GTK_CONTAINER(log_scroll), log_view);
    gtk_box_pack_start(GTK_BOX(outer), log_scroll, TRUE, TRUE, 0U);

    refresh_devices(&app);
    gtk_widget_show_all(app.window);
    gtk_main();

    cleanup_channels(&app);
    if (app.pulse_timer != 0U) g_source_remove(app.pulse_timer);
    g_object_unref(app.filesystem_store);
    g_object_unref(app.device_store);
    return 0;
}
