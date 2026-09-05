#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>

typedef struct {
    GtkWidget *window;

    GtkWidget *iso_entry;
    GtkWidget *nand_entry;
    GtkWidget *keys_entry;

    GtkWidget *install_button;
    GtkWidget *play_button;
    GtkWidget *progress_bar;
    GtkWidget *status_label;

    GtkWidget *terminal_toggle;
    GtkWidget *terminal_revealer;
    GtkWidget *terminal_view;

    /* Set by on_install_clicked() after the user confirms the overwrite
     * dialog; read by install_worker() to decide whether to wipe the
     * existing install before extracting again. */
    gboolean wipe_before_install;
} AppData;

/* ---------- Path helpers ---------- */

/* Resolves the directory the *executable itself* lives in, as opposed to the
 * process's current working directory (which is wherever the user happened
 * to be when they launched it). This matters for AppImage: the binary runs
 * from a temporary mount point, not from the folder the user double-clicked
 * or ran the AppImage from, so "wit", "bootmii_nand_import" and the .zip
 * must be located relative to /proc/self/exe, not the CWD. */
static gchar *get_binary_dir(void) {
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        /* Fallback for non-Linux or if /proc is unavailable */
        return g_get_current_dir();
    }
    exe_path[len] = '\0';
    return g_path_get_dirname(exe_path);
}

/* When running as an AppImage, get_binary_dir() resolves to somewhere under
 * /tmp/.mount_XXXXXX/usr/bin — the squashfs mount point, which is READ-ONLY.
 * It's fine for locating bundled binaries/assets to read (wit,
 * bootmii_nand_import, WiiCompiled_dist.zip), but any directory we need to
 * write into must live somewhere real and writable instead. */
static gchar *get_writable_temp_dir(const gchar *name) {
    gchar *cache_root = g_build_filename(g_get_user_cache_dir(), "WiiCompiled", NULL);
    g_mkdir_with_parents(cache_root, 0755);
    gchar *dir = g_build_filename(cache_root, name, NULL);
    g_free(cache_root);
    return dir;
}

/* Where the game gets installed to. */
static gchar *get_wiicompiled_target_dir(void) {
    return g_build_filename(g_get_home_dir(), ".local", "share", "WiiCompiled", NULL);
}

/* The actual game executable inside the installed tree — used both to
 * detect an existing install (Play button) and to launch it / point the
 * .desktop file at it. */
static gchar *get_install_check_path(void) {
    gchar *target_dir = get_wiicompiled_target_dir();
    gchar *check_path = g_build_filename(target_dir, "Install", "Base", "WiiCompiled", NULL);
    g_free(target_dir);
    return check_path;
}

/* ---------- Progress bar / status label updates (must run on the main thread) ---------- */

typedef struct {
    AppData *app;
    double fraction;
    char *message;
} ProgressUpdate;

gboolean update_ui(gpointer data) {
    ProgressUpdate *update = (ProgressUpdate *)data;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(update->app->progress_bar), update->fraction);
    gtk_label_set_text(GTK_LABEL(update->app->status_label), update->message);

    if (update->fraction >= 1.0 || update->fraction < 0.0) {
        gtk_widget_set_sensitive(update->app->install_button, TRUE);
        gtk_widget_set_sensitive(update->app->iso_entry, TRUE);
        gtk_widget_set_sensitive(update->app->nand_entry, TRUE);
        gtk_widget_set_sensitive(update->app->keys_entry, TRUE);
    }

    g_free(update->message);
    g_free(update);
    return G_SOURCE_REMOVE;
}

void set_progress(AppData *app, double fraction, const char *message) {
    ProgressUpdate *update = g_new(ProgressUpdate, 1);
    update->app = app;
    update->fraction = fraction;
    update->message = g_strdup(message);
    g_idle_add(update_ui, update);
}

/* ---------- Mini terminal log (also runs on the main thread) ---------- */

typedef struct {
    AppData *app;
    char *text;
} TerminalLine;

gboolean append_terminal_text(gpointer data) {
    TerminalLine *tl = (TerminalLine *)data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tl->app->terminal_view));

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, tl->text, -1);

    GtkTextMark *mark = gtk_text_buffer_get_insert(buffer);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(tl->app->terminal_view), mark);

    g_free(tl->text);
    g_free(tl);
    return G_SOURCE_REMOVE;
}

void log_terminal(AppData *app, const char *text) {
    TerminalLine *tl = g_new(TerminalLine, 1);
    tl->app = app;
    tl->text = g_strdup(text);
    g_idle_add(append_terminal_text, tl);
}

/* Runs a shell command, streams its output into the mini terminal, and
 * returns its exit status (0 on success). */
int run_command(AppData *app, const char *cmd) {
    gchar *prompt = g_strdup_printf("$ %s\n", cmd);
    log_terminal(app, prompt);
    g_free(prompt);

    gchar *full_cmd = g_strdup_printf("%s 2>&1", cmd);
    FILE *fp = popen(full_cmd, "r");
    g_free(full_cmd);

    if (!fp) {
        log_terminal(app, "(failed to start command)\n");
        return -1;
    }

    char buf[512];
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        log_terminal(app, buf);
    }

    int status = pclose(fp);
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return status;
}

/* ---------- Play button visibility ---------- */

static void update_play_button_visibility(AppData *app) {
    gchar *check_path = get_install_check_path();
    gboolean exists = g_file_test(check_path, G_FILE_TEST_EXISTS);
    gtk_widget_set_visible(app->play_button, exists);
    g_free(check_path);
}

/* Must be called via g_idle_add() from worker threads, since it touches
 * widgets. */
gboolean refresh_play_button_idle(gpointer data) {
    AppData *app = (AppData *)data;
    update_play_button_visibility(app);
    return G_SOURCE_REMOVE;
}

/* ---------- .desktop entry creation ---------- */

static void create_desktop_entry(const gchar *exe_path) {
    gchar *apps_dir = g_build_filename(g_get_home_dir(), ".local", "share", "applications", NULL);
    g_mkdir_with_parents(apps_dir, 0755);

    gchar *desktop_path = g_build_filename(apps_dir, "wiicompiled.desktop", NULL);

    gchar *contents = g_strdup_printf(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=WiiCompiled\n"
        "Comment=Launch WiiCompiled\n"
        "Exec=\"%s\"\n"
        "Icon=preferences-desktop-gaming\n"
        "Terminal=false\n"
        "Categories=Game;\n"
        "StartupNotify=true\n",
        exe_path);

    g_file_set_contents(desktop_path, contents, -1, NULL);
    chmod(desktop_path, 0755);

    /* Best-effort; harmless if the tool isn't installed. */
    run_command_silent:
    system("update-desktop-database ~/.local/share/applications >/dev/null 2>&1");

    g_free(contents);
    g_free(desktop_path);
    g_free(apps_dir);
}

/* ---------- Worker thread: does the actual installation ---------- */

gpointer install_worker(gpointer data) {
    AppData *app = (AppData *)data;
    const gchar *iso_path  = gtk_entry_get_text(GTK_ENTRY(app->iso_entry));
    const gchar *nand_path = gtk_entry_get_text(GTK_ENTRY(app->nand_entry));
    const gchar *keys_path = gtk_entry_get_text(GTK_ENTRY(app->keys_entry));

    gboolean has_nand = (nand_path != NULL && strlen(nand_path) > 0);
    gboolean has_keys = (keys_path != NULL && strlen(keys_path) > 0);

    if (strlen(iso_path) == 0 || !g_file_test(iso_path, G_FILE_TEST_EXISTS)) {
        set_progress(app, -1.0, "Error: no ISO selected, or the file was not found!");
        return NULL;
    }

    if (has_nand != has_keys) {
        set_progress(app, -1.0, "Error: please provide both nand.bin and keys.bin, or leave both blank.");
        return NULL;
    }

    gchar *target_dir = get_wiicompiled_target_dir();

    /* If the user confirmed the overwrite dialog, wipe the previous
     * install before doing anything else. */
    if (app->wipe_before_install) {
        set_progress(app, 0.02, "Removing previous installation...");
        gchar *rm_target_cmd = g_strdup_printf("rm -rf \"%s\"", target_dir);
        run_command(app, rm_target_cmd);
        g_free(rm_target_cmd);
    }

    gchar *binary_dir = get_binary_dir();
    gchar *zip_path = g_build_filename(binary_dir, "WiiCompiled_dist.zip", NULL);

    /* 1. Extract the main package (native code / runtime files) */
    set_progress(app, 0.1, "Extracting native code...");
    if (!g_file_test(zip_path, G_FILE_TEST_EXISTS)) {
        g_free(zip_path);
        zip_path = g_build_filename(target_dir, "WiiCompiled_dist.zip", NULL);
    }

    if (!g_file_test(zip_path, G_FILE_TEST_EXISTS)) {
        set_progress(app, -1.0, "Error: WiiCompiled_dist.zip was not found.");
        g_free(binary_dir); g_free(target_dir); g_free(zip_path);
        return NULL;
    }

    g_mkdir_with_parents(target_dir, 0755);

    gchar *unzip_cmd = g_strdup_printf("unzip -o -q \"%s\" -d \"%s\"", zip_path, target_dir);
    run_command(app, unzip_cmd);
    g_free(unzip_cmd);

    /* 2. Extract the game dump assets from the ISO using WIT */
    gchar *wit_binary = g_build_filename(binary_dir, "wit", NULL);
    gchar *temp_extract_dir = get_writable_temp_dir("AssetsTemp");
    gchar *assets_dir = g_build_filename(target_dir, "workspace", "Assets", NULL);

    if (!g_file_test(wit_binary, G_FILE_TEST_EXISTS)) {
        set_progress(app, -1.0, "Error: the 'wit' binary was not found next to the executable.");
        g_free(binary_dir); g_free(target_dir); g_free(zip_path);
        g_free(wit_binary); g_free(temp_extract_dir); g_free(assets_dir);
        return NULL;
    }
    chmod(wit_binary, 0755);

    gchar *rm_temp_cmd = g_strdup_printf("rm -rf \"%s\"", temp_extract_dir);
    run_command(app, rm_temp_cmd);

    set_progress(app, 0.35, "Extracting game dump assets (ISO)...");
    gchar *wit_cmd = g_strdup_printf("\"%s\" extract \"%s\" \"%s\"", wit_binary, iso_path, temp_extract_dir);
    int wit_status = run_command(app, wit_cmd);
    g_free(wit_cmd);

    if (wit_status != 0) {
        set_progress(app, -1.0, "Error while extracting the ISO.");
        g_free(rm_temp_cmd);
        g_free(binary_dir); g_free(target_dir); g_free(zip_path);
        g_free(wit_binary); g_free(temp_extract_dir); g_free(assets_dir);
        return NULL;
    }

    /* 3. Locate the DATA partition and move it into place */
    set_progress(app, 0.6, "Extracting game dump assets (ISO)...");
    gchar *source_data_dir = NULL;
    GDir *dir = g_dir_open(temp_extract_dir, 0, NULL);

    if (dir) {
        const gchar *name;
        while ((name = g_dir_read_name(dir)) != NULL) {
            gchar *sub_dir = g_build_filename(temp_extract_dir, name, NULL);
            gchar *sys_test = g_build_filename(sub_dir, "sys", NULL);
            gchar *files_test = g_build_filename(sub_dir, "files", NULL);

            if (g_file_test(sys_test, G_FILE_TEST_IS_DIR) && g_file_test(files_test, G_FILE_TEST_IS_DIR)) {
                source_data_dir = g_strdup(sub_dir);
            }
            g_free(sys_test);
            g_free(files_test);
            g_free(sub_dir);
            if (source_data_dir) break;
        }
        g_dir_close(dir);
    }

    if (!source_data_dir) {
        gchar *sys_test = g_build_filename(temp_extract_dir, "sys", NULL);
        if (g_file_test(sys_test, G_FILE_TEST_IS_DIR)) {
            source_data_dir = g_strdup(temp_extract_dir);
        }
        g_free(sys_test);
    }

    if (source_data_dir) {
        gchar *final_data_dir = g_build_filename(assets_dir, "DATA", NULL);
        g_mkdir_with_parents(assets_dir, 0755);

        gchar *rm_data_cmd = g_strdup_printf("rm -rf \"%s\"", final_data_dir);
        run_command(app, rm_data_cmd);
        g_free(rm_data_cmd);

        gchar *mv_cmd = g_strdup_printf("mv \"%s\" \"%s\"", source_data_dir, final_data_dir);
        run_command(app, mv_cmd);
        g_free(mv_cmd);

        gchar *cp_cmd = g_strdup_printf("cp \"%s/files/rel/StaticR.rel\" \"%s/sys/main.dol\" \"%s/\" 2>/dev/null", final_data_dir, final_data_dir, assets_dir);
        run_command(app, cp_cmd);
        g_free(cp_cmd);

        g_free(final_data_dir);
    } else {
        set_progress(app, -1.0, "Error: could not find the data partition inside the ISO.");
        run_command(app, rm_temp_cmd);
        g_free(rm_temp_cmd);
        g_free(binary_dir); g_free(target_dir); g_free(zip_path);
        g_free(wit_binary); g_free(temp_extract_dir); g_free(assets_dir);
        return NULL;
    }

    /* 4. NAND: only runs if both nand.bin and keys.bin were provided.
     *    Otherwise, whatever NAND is already installed is left untouched. */
    gchar *nand_final_dir = g_build_filename(target_dir, "NAND", NULL);

    if (has_nand && has_keys) {
        if (!g_file_test(nand_path, G_FILE_TEST_EXISTS)) {
            set_progress(app, -1.0, "Error: nand.bin file was not found!");
            g_free(rm_temp_cmd);
            g_free(binary_dir); g_free(target_dir); g_free(zip_path);
            g_free(wit_binary); g_free(temp_extract_dir); g_free(assets_dir);
            g_free(source_data_dir); g_free(nand_final_dir);
            return NULL;
        }
        if (!g_file_test(keys_path, G_FILE_TEST_EXISTS)) {
            set_progress(app, -1.0, "Error: keys.bin file was not found!");
            g_free(rm_temp_cmd);
            g_free(binary_dir); g_free(target_dir); g_free(zip_path);
            g_free(wit_binary); g_free(temp_extract_dir); g_free(assets_dir);
            g_free(source_data_dir); g_free(nand_final_dir);
            return NULL;
        }

        gchar *nand_import_binary = g_build_filename(binary_dir, "bootmii_nand_import", NULL);
        if (!g_file_test(nand_import_binary, G_FILE_TEST_EXISTS)) {
            set_progress(app, -1.0, "Error: the 'bootmii_nand_import' binary was not found next to the executable.");
            g_free(nand_import_binary);
            g_free(rm_temp_cmd);
            g_free(binary_dir); g_free(target_dir); g_free(zip_path);
            g_free(wit_binary); g_free(temp_extract_dir); g_free(assets_dir);
            g_free(source_data_dir); g_free(nand_final_dir);
            return NULL;
        }
        chmod(nand_import_binary, 0755);

        set_progress(app, 0.75, "Extracting NAND...");

        gchar *nand_temp_dir = get_writable_temp_dir("nand");
        gchar *rm_nand_temp_cmd = g_strdup_printf("rm -rf \"%s\"", nand_temp_dir);
        run_command(app, rm_nand_temp_cmd);
        g_free(rm_nand_temp_cmd);
        g_mkdir_with_parents(nand_temp_dir, 0755);

        gchar *nand_cmd = g_strdup_printf("\"%s\" \"%s\" \"%s\" \"%s\"",
                                          nand_import_binary, nand_path, nand_temp_dir, keys_path);
        int nand_status = run_command(app, nand_cmd);
        g_free(nand_cmd);

        if (nand_status != 0) {
            set_progress(app, -1.0, "Error while extracting the NAND.");
            g_free(nand_import_binary); g_free(nand_temp_dir);
            g_free(rm_temp_cmd);
            g_free(binary_dir); g_free(target_dir); g_free(zip_path);
            g_free(wit_binary); g_free(temp_extract_dir); g_free(assets_dir);
            g_free(source_data_dir); g_free(nand_final_dir);
            return NULL;
        }

        set_progress(app, 0.9, "Extracting NAND...");

        gchar *clean_cmd = g_strdup_printf(
            "mkdir -p \"%s\" && find \"%s\" -mindepth 1 -delete", nand_final_dir, nand_final_dir);
        run_command(app, clean_cmd);
        g_free(clean_cmd);

        gchar *copy_cmd = g_strdup_printf("cp -a \"%s\"/. \"%s\"/", nand_temp_dir, nand_final_dir);
        run_command(app, copy_cmd);
        g_free(copy_cmd);

        gchar *rm_nand_temp_cmd2 = g_strdup_printf("rm -rf \"%s\"", nand_temp_dir);
        run_command(app, rm_nand_temp_cmd2);
        g_free(rm_nand_temp_cmd2);

        g_free(nand_import_binary);
        g_free(nand_temp_dir);
    }
    g_free(nand_final_dir);

    /* 5. Cleanup */
    set_progress(app, 0.97, "Cleaning up temporary files...");
    run_command(app, rm_temp_cmd);
    g_free(rm_temp_cmd);

    /* 6. Create/refresh the .desktop launcher and reveal the Play button */
    gchar *exe_check_path = get_install_check_path();
    create_desktop_entry(exe_check_path);
    g_free(exe_check_path);

    g_idle_add(refresh_play_button_idle, app);

    set_progress(app, 1.0, "Installation completed successfully!");

    g_free(binary_dir);
    g_free(target_dir);
    g_free(zip_path);
    g_free(wit_binary);
    g_free(temp_extract_dir);
    g_free(assets_dir);
    g_free(source_data_dir);

    return NULL;
}

/* ---------- Callbacks ---------- */

void on_install_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app = (AppData *)data;

    gchar *target_dir = get_wiicompiled_target_dir();
    gboolean existing_install = g_file_test(target_dir, G_FILE_TEST_IS_DIR);
    g_free(target_dir);

    if (existing_install) {
        GtkWidget *confirm = gtk_message_dialog_new(GTK_WINDOW(app->window),
                                                    GTK_DIALOG_MODAL,
                                                    GTK_MESSAGE_QUESTION,
                                                    GTK_BUTTONS_YES_NO,
                                                    "An existing WiiCompiled installation was found.\n"
                                                    "Do you want to delete it and reinstall from scratch?");
        gtk_window_set_title(GTK_WINDOW(confirm), "Overwrite existing installation?");

        gint response = gtk_dialog_run(GTK_DIALOG(confirm));
        gtk_widget_destroy(confirm);

        if (response != GTK_RESPONSE_YES) {
            return; /* user backed out, don't touch anything */
        }
        app->wipe_before_install = TRUE;
    } else {
        app->wipe_before_install = FALSE;
    }

    gtk_widget_set_sensitive(app->install_button, FALSE);
    gtk_widget_set_sensitive(app->iso_entry, FALSE);
    gtk_widget_set_sensitive(app->nand_entry, FALSE);
    gtk_widget_set_sensitive(app->keys_entry, FALSE);

    g_thread_new("InstallThread", install_worker, app);
}

void on_play_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    (void)data;

    gchar *exe_path = get_install_check_path();

    if (!g_file_test(exe_path, G_FILE_TEST_EXISTS)) {
        g_free(exe_path);
        return;
    }
    chmod(exe_path, 0755);

    GError *error = NULL;
    gchar *argv[] = { exe_path, NULL };
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &error)) {
        g_warning("Failed to launch WiiCompiled: %s", error ? error->message : "unknown error");
        if (error) g_error_free(error);
    }

    g_free(exe_path);
}

void on_terminal_toggle_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    AppData *app = (AppData *)data;
    gboolean visible = gtk_revealer_get_reveal_child(GTK_REVEALER(app->terminal_revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->terminal_revealer), !visible);
}

static void browse_for_entry(AppData *app, GtkEntry *entry, const gchar *title,
                             const gchar *filter_name, const gchar *pattern) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new(title,
                                                    GTK_WINDOW(app->window),
                                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Open", GTK_RESPONSE_ACCEPT,
                                                    NULL);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, filter_name);
    gtk_file_filter_add_pattern(filter, pattern);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        gtk_entry_set_text(entry, filename);
        g_free(filename);
    }

    gtk_widget_destroy(dialog);
                             }

                             void on_browse_iso_clicked(GtkWidget *widget, gpointer data) {
                                 (void)widget;
                                 AppData *app = (AppData *)data;
                                 browse_for_entry(app, GTK_ENTRY(app->iso_entry), "Select Game ISO", "ISO images (*.iso)", "*.iso");
                             }

                             void on_browse_nand_clicked(GtkWidget *widget, gpointer data) {
                                 (void)widget;
                                 AppData *app = (AppData *)data;
                                 browse_for_entry(app, GTK_ENTRY(app->nand_entry), "Select nand.bin (optional)", "NAND file (*.bin)", "*.bin");
                             }

                             void on_browse_keys_clicked(GtkWidget *widget, gpointer data) {
                                 (void)widget;
                                 AppData *app = (AppData *)data;
                                 browse_for_entry(app, GTK_ENTRY(app->keys_entry), "Select keys.bin (optional)", "Keys file (*.bin)", "*.bin");
                             }

                             /* ---------- Helper to build a "label + entry + browse button" row ---------- */

                             static void build_file_row(GtkWidget *grid, int row, const gchar *row_label,
                                                        const gchar *placeholder, GtkWidget **entry_out,
                                                        GCallback browse_cb, AppData *app) {
                                 GtkWidget *label = gtk_label_new(row_label);
                                 gtk_widget_set_halign(label, GTK_ALIGN_START);
                                 gtk_grid_attach(GTK_GRID(grid), label, 0, row, 2, 1);

                                 GtkWidget *entry = gtk_entry_new();
                                 gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
                                 gtk_widget_set_hexpand(entry, TRUE);
                                 gtk_grid_attach(GTK_GRID(grid), entry, 0, row + 1, 1, 1);

                                 GtkWidget *button = gtk_button_new_with_label("Browse...");
                                 g_signal_connect(button, "clicked", browse_cb, app);
                                 gtk_grid_attach(GTK_GRID(grid), button, 1, row + 1, 1, 1);

                                 *entry_out = entry;
                                                        }

                                                        /* Gives the Play button its blue "call to action" look regardless of the
                                                         * user's GTK theme. */
                                                        static void apply_blue_style(GtkWidget *widget) {
                                                            GtkCssProvider *provider = gtk_css_provider_new();
                                                            const gchar *css =
                                                            "button.wiicompiled-play {"
                                                            "  background-image: none;"
                                                            "  background-color: #1a73e8;"
                                                            "  color: #ffffff;"
                                                            "  font-weight: bold;"
                                                            "  border-color: #1256b8;"
                                                            "}"
                                                            "button.wiicompiled-play:hover {"
                                                            "  background-color: #1669d6;"
                                                            "}"
                                                            "button.wiicompiled-play:active {"
                                                            "  background-color: #0f52ad;"
                                                            "}";
                                 gtk_css_provider_load_from_data(provider, css, -1, NULL);

                                 GtkStyleContext *context = gtk_widget_get_style_context(widget);
                                 gtk_style_context_add_class(context, "wiicompiled-play");
                                 gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

                                 g_object_unref(provider);
                                                        }

                                                        int main(int argc, char *argv[]) {
                                                            gtk_init(&argc, &argv);

                                                            AppData *app = g_new(AppData, 1);
                                                            app->wipe_before_install = FALSE;

                                                            app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
                                                            gtk_window_set_title(GTK_WINDOW(app->window), "WiiCompiled Installer");
                                                            gtk_window_set_default_size(GTK_WINDOW(app->window), 520, 320);
                                                            gtk_container_set_border_width(GTK_CONTAINER(app->window), 16);
                                                            g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

                                                            GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
                                                            gtk_container_add(GTK_CONTAINER(app->window), vbox);

                                                            GtkWidget *label = gtk_label_new("Select your game ISO file below to begin installation.\nnand.bin and keys.bin are optional and only needed to update your NAND.");
                                                            gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
                                                            gtk_widget_set_halign(label, GTK_ALIGN_START);
                                                            gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

                                                            GtkWidget *grid = gtk_grid_new();
                                                            gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
                                                            gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
                                                            gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 4);

                                                            build_file_row(grid, 0, "Game ISO (required)", "Path to the game ISO...",
                                                                           &app->iso_entry, G_CALLBACK(on_browse_iso_clicked), app);

                                                            build_file_row(grid, 3, "nand.bin (optional)", "Path to nand.bin...",
                                                                           &app->nand_entry, G_CALLBACK(on_browse_nand_clicked), app);

                                                            build_file_row(grid, 6, "keys.bin (optional)", "Path to keys.bin...",
                                                                           &app->keys_entry, G_CALLBACK(on_browse_keys_clicked), app);

                                                            GtkWidget *nand_note = gtk_label_new("A NAND is required to play online.");
                                                            gtk_widget_set_halign(nand_note, GTK_ALIGN_START);
                                                            gtk_box_pack_start(GTK_BOX(vbox), nand_note, FALSE, FALSE, 0);

                                                            /* Install button + (conditionally hidden) Play button side by side */
                                                            GtkWidget *action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                                                            gtk_box_pack_start(GTK_BOX(vbox), action_row, FALSE, FALSE, 10);

                                                            app->install_button = gtk_button_new_with_label("Start Installation");
                                                            gtk_widget_set_size_request(app->install_button, -1, 40);
                                                            g_signal_connect(app->install_button, "clicked", G_CALLBACK(on_install_clicked), app);
                                                            gtk_box_pack_start(GTK_BOX(action_row), app->install_button, TRUE, TRUE, 0);

                                                            app->play_button = gtk_button_new_with_label("Play");
                                                            gtk_widget_set_size_request(app->play_button, 100, 40);
                                                            apply_blue_style(app->play_button);
                                                            g_signal_connect(app->play_button, "clicked", G_CALLBACK(on_play_clicked), app);
                                                            gtk_box_pack_start(GTK_BOX(action_row), app->play_button, FALSE, FALSE, 0);
                                                            /* Stay hidden through gtk_widget_show_all() until we explicitly decide
                                                             * (via update_play_button_visibility) that it should be shown. */
                                                            gtk_widget_set_no_show_all(app->play_button, TRUE);

                                                            app->progress_bar = gtk_progress_bar_new();
                                                            gtk_box_pack_start(GTK_BOX(vbox), app->progress_bar, FALSE, FALSE, 0);

                                                            /* Status row: status label + terminal toggle button */
                                                            GtkWidget *status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                                                            gtk_box_pack_start(GTK_BOX(vbox), status_row, FALSE, FALSE, 0);

                                                            app->status_label = gtk_label_new("Ready to install.");
                                                            gtk_widget_set_halign(app->status_label, GTK_ALIGN_START);
                                                            gtk_widget_set_hexpand(app->status_label, TRUE);
                                                            gtk_box_pack_start(GTK_BOX(status_row), app->status_label, TRUE, TRUE, 0);

                                                            app->terminal_toggle = gtk_button_new_from_icon_name("utilities-terminal-symbolic", GTK_ICON_SIZE_BUTTON);
                                                            gtk_widget_set_tooltip_text(app->terminal_toggle, "Show/hide terminal output");
                                                            g_signal_connect(app->terminal_toggle, "clicked", G_CALLBACK(on_terminal_toggle_clicked), app);
                                                            gtk_box_pack_start(GTK_BOX(status_row), app->terminal_toggle, FALSE, FALSE, 0);

                                                            /* Collapsible mini terminal with the raw command output */
                                                            app->terminal_revealer = gtk_revealer_new();
                                                            gtk_revealer_set_transition_type(GTK_REVEALER(app->terminal_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
                                                            gtk_revealer_set_reveal_child(GTK_REVEALER(app->terminal_revealer), FALSE);
                                                            gtk_box_pack_start(GTK_BOX(vbox), app->terminal_revealer, FALSE, FALSE, 0);

                                                            GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
                                                            gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
                                                            gtk_widget_set_size_request(scroll, -1, 150);
                                                            gtk_container_add(GTK_CONTAINER(app->terminal_revealer), scroll);

                                                            app->terminal_view = gtk_text_view_new();
                                                            gtk_text_view_set_editable(GTK_TEXT_VIEW(app->terminal_view), FALSE);
                                                            gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app->terminal_view), FALSE);
                                                            gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->terminal_view), TRUE);
                                                            gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->terminal_view), GTK_WRAP_CHAR);
                                                            gtk_container_add(GTK_CONTAINER(scroll), app->terminal_view);

                                                            gtk_widget_show_all(app->window);

                                                            /* Decide, at startup, whether the Play button should be visible. */
                                                            update_play_button_visibility(app);

                                                            gtk_main();

                                                            g_free(app);
                                                            return 0;
                                                        }
