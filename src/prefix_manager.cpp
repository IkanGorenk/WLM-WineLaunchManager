#include "prefix_manager.h"

#include "backup.h"
#include "config.h"
#include "dialogs.h"
#include "live_log.h"
#include "proton.h"
#include "scripts.h"
#include "task_log.h"
#include "ui.h"
#include "util.h"
#include "window_mode.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <thread>
#include <string>
#include <utility>
#include <vector>

namespace prefix {

namespace {

namespace fs = std::filesystem;

// Python: COMMON_WINETRICKS_VERBS (launcher.py:1578) - the (verb, description)
// pairs offered as checkboxes in the winetricks picker.
const std::pair<const char*, const char*> COMMON_WINETRICKS_VERBS[] = {
    {"corefonts", "Core Fonts (Arial, Times New Roman, etc.)"},
    {"cjkfonts", "CJK Fonts (China/Japan/Korea)"},
    {"vcrun2005", "Visual C++ 2005 Redist"},
    {"vcrun2008", "Visual C++ 2008 Redist"},
    {"vcrun2010", "Visual C++ 2010 Redist"},
    {"vcrun2012", "Visual C++ 2012 Redist"},
    {"vcrun2013", "Visual C++ 2013 Redist"},
    {"vcrun2019", "Visual C++ 2015-2019 Redist"},
    {"vcrun2022", "Visual C++ 2015-2022 Redist"},
    {"dotnet48", ".NET Framework 4.8"},
    {"dotnet6", ".NET 6 Runtime"},
    {"dotnetdesktop6", ".NET 6 Desktop Runtime"},
    {"d3dx9", "DirectX 9 (d3dx9)"},
    {"d3dx11_43", "DirectX 11 (d3dx11_43)"},
    {"d3dcompiler_47", "D3D Compiler 47"},
    {"xact", "XAudio (xact)"},
    {"physx", "PhysX"},
    {"dxvk", "DXVK (DirectX -> Vulkan)"},
    {"vkd3d", "VKD3D (Direct3D 12 -> Vulkan)"},
    {"faudio", "FAudio"},
};

constexpr size_t COMMON_VERB_COUNT =
    sizeof(COMMON_WINETRICKS_VERBS) / sizeof(COMMON_WINETRICKS_VERBS[0]);

// Response ids of the winetricks picker's custom buttons (GTK_RESPONSE_* are
// negative, so the positive ones cannot clash).
constexpr gint RESPONSE_WINETRICKS_RUN = 1;
constexpr gint RESPONSE_WINETRICKS_INTERACTIVE = 2;

// Model columns of the manager's tree view: the runner key is hidden (it is
// only used to map a clicked row back to its PrefixEntry), the other columns
// are shown in the order the view columns are created.
enum StoreColumn {
    COL_RUNNER_KEY = 0,
    COL_PREFIX_CODE,
    COL_RUNNER_NAME,
    COL_GAMES,
    COL_PROTON,
    COL_PATH,
    NUM_COLS
};

// State of one open "Prefix Configuration Manager" window. Freed when the
// window itself is destroyed, so it always outlives every widget using it.
struct ManagerCtx {
    GtkWidget* window = nullptr;
    GtkListStore* store = nullptr;
    GtkWidget* tree = nullptr;
    GtkWidget* empty_label = nullptr;
    std::vector<PrefixEntry> entries;
    // Cheap "did anything on disk move?" stamp, so the two-second poll does not
    // re-parse every config file and walk every prefix folder for nothing.
    std::string disk_stamp;

    // Auto-refresh: what the list looked like the last time it was built, and
    // the timer that watches for something different.
    std::string signature;
    guint auto_refresh_id = 0;
};

std::string trim(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// Flattened command line, only used to tell the user which command failed.
std::string join_command(const std::vector<std::string>& command) {
    std::string out;
    for (const std::string& part : command) {
        if (!out.empty()) out += " ";
        out += part;
    }
    return out;
}

// Python: shutil.which(name) - external tools are looked up on PATH the same
// way the launcher did, so a missing tool is reported before anything spawns.
bool has_command(const std::string& name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;

    const std::string path_list(path_env);
    size_t start = 0;
    while (start <= path_list.size()) {
        const size_t sep = path_list.find(':', start);
        const std::string dir =
            path_list.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        if (!dir.empty()) {
            const fs::path candidate = fs::path(dir) / name;
            std::error_code ec;
            if (util::is_executable(candidate) && !fs::is_directory(candidate, ec)) return true;
        }
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return false;
}

std::string tool_display_name(const std::string& tool) {
    if (tool == "winecfg") return "Wine Configuration";
    if (tool == "explorer") return "Wine Explorer";
    if (tool == "uninstaller") return "Uninstaller";
    if (tool == "winetricks") return "Winetricks";
    return tool;
}

// Python: find_proton_wine_binary() - locate the real 'wine' binary inside a
// Proton build (GE/CachyOS) starting from the path of its 'proton' script.
// Winetricks has to be pointed at THIS wine (env WINE) instead of the system
// one, otherwise its changes land in the wrong prefix.
// Returns "" when the build contains no wine binary at all.
std::string find_proton_wine_binary(const std::string& proton_script_path) {
    const fs::path proton_root = fs::path(proton_script_path).parent_path();

    // A build keeps its Wine in files/bin, or - in the architecture-specific
    // layouts Proton also ships - in files/bin-arm64 / files/bin-x86_64. The
    // arch-specific directories come after the plain one, so a build that has
    // both keeps the same Wine Proton itself would pick.
    std::vector<fs::path> bin_dirs = {proton_root / "files" / "bin",
                                       proton_root / "files" / "bin-x86_64",
                                       proton_root / "dist" / "bin"};
#if defined(__aarch64__)
    bin_dirs.insert(bin_dirs.begin(), proton_root / "files" / "bin-arm64");
#endif

    for (const fs::path& dir : bin_dirs) {
        for (const char* name : {"wine64", "wine"}) {
            const fs::path candidate = dir / name;
            std::error_code ec;
            if (fs::exists(candidate, ec)) return candidate.string();
        }
    }
    return "";
}

// Python: pick_proton_build_dialog() - choose one of the already extracted
// Proton builds for a prefix whose registry entry does not record a Proton
// version yet (an old prefix, or one found by scanning the prefix folder
// instead of the usual runner dialog). Stacked on top of `parent` so it can
// never end up behind the Prefix Configuration Manager that asked for it.
// Returns nullopt when cancelled or when no build exists at all.
std::optional<std::pair<std::string, fs::path>> pick_proton_build_dialog(
    const std::string& runner_key, GtkWindow* parent) {
    proton::BuildList installs = runner_key == "protonge"
                                     ? proton::find_protonge_installations()
                                     : proton::find_protoncachyos_installations();
    const std::string label = cfg::runner_display_name(runner_key);

    if (installs.empty()) {
        dialogs::show_error("Error",
                            "No " + label + " build found.\nExtract one first via Settings.",
                            parent);
        return std::nullopt;
    }

    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        ("Select " + label + " Build").c_str(), parent,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "_OK", GTK_RESPONSE_OK, "_Cancel", GTK_RESPONSE_CANCEL, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), winmode::px(15));
    gtk_box_set_spacing(GTK_BOX(content), winmode::px(10));

    GtkWidget* label_widget = gtk_label_new(
        ("This prefix has no " + label + " version recorded yet.\nSelect the build to use for it:")
            .c_str());
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0.0f);
    gtk_box_pack_start(GTK_BOX(content), label_widget, FALSE, FALSE, 0);

    GtkWidget* combo = gtk_combo_box_text_new();
    for (const auto& build : installs) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), build.first.c_str());
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    gtk_box_pack_start(GTK_BOX(content), combo, FALSE, FALSE, 0);

    std::optional<std::pair<std::string, fs::path>> result;
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const int index = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
        if (index >= 0 && index < static_cast<int>(installs.size())) {
            result = installs[static_cast<size_t>(index)];
        }
    }
    gtk_widget_destroy(dialog);
    return result;
}

// What the list currently on screen is made of: one "runner:code@path" per
// prefix, plus the games attached to it. Two lists with the same signature are
// the same list, so a rebuild that found nothing new is not worth doing.
std::string list_signature(const std::vector<PrefixEntry>& entries) {
    std::string out;
    for (const PrefixEntry& entry : entries) {
        out += entry.runner + ":" + entry.prefix_code + "@" + entry.prefix_path + "|";
        for (const std::string& game : entry.games) out += game + ",";
        out += "\n";
    }
    return out;
}

void refresh_manager(ManagerCtx* ctx);  // defined below; the auto-refresh uses it
void select_prefix(ManagerCtx* ctx, const std::string& runner_key,
                   const std::string& prefix_code);
const PrefixEntry* selected_entry(ManagerCtx* ctx);

// A cheap "has anything on disk moved?" test, built only from stat() results.
//
// list_all_known_prefixes() parses three JSON files and walks every prefix
// folder. Doing that every two seconds, for as long as this window stays open,
// is a lot of allocation churn for an answer that is almost always "no change" -
// and the churn is what makes the process creep up in memory. The config files
// and the prefix root directories get an mtime whenever their contents change,
// so comparing those turns the expensive scan into a handful of stat() calls and
// only does the real work when something actually moved.
std::string disk_stamp() {
    std::string stamp;
    std::error_code ec;

    auto add = [&](const fs::path& path) {
        // Seconds are plenty: this only has to notice a change between two
        // polls, and a sub-second difference would only cost one extra scan.
        const auto stamp_time = fs::last_write_time(path, ec);
        ec.clear();
        stamp += std::to_string(stamp_time.time_since_epoch().count());
        stamp += ':';
    };

    add(cfg::prefix_registry_file);
    add(cfg::runner_config_file);
    add(cfg::prefix_location_config_file);

    // The registry's own record of a custom prefix location, if there is one:
    // creating a prefix in a new place must count as a change.
    json loc_cfg;
    if (cfg::read_json(cfg::prefix_location_config_file, loc_cfg)) {
        for (const auto& kv : loc_cfg.items()) {
            if (kv.value().is_string()) add(fs::path(kv.value().get<std::string>()));
        }
    }

    for (const auto& root : cfg::default_prefix_roots()) add(root.second);
    return stamp;
}

// The manager is a view onto the disk, and a prefix can appear there from
// anywhere: a restore that just finished, PLAY in the main window, a folder
// created by hand, the Create Prefix dialog of a second manager window. Rather
// than make the user press Refresh, this polls every two seconds and only
// rebuilds when the result actually differs - so the list never flickers under
// the cursor and the selected row is never disturbed for nothing.
gboolean auto_refresh_cb(gpointer data) {
    auto* ctx = static_cast<ManagerCtx*>(data);
    try {
        if (!GTK_IS_WIDGET(ctx->window)) return G_SOURCE_REMOVE;

        // Cheap check first: nothing on disk moved, so there is nothing to do.
        const std::string stamp = disk_stamp();
        if (stamp == ctx->disk_stamp) return G_SOURCE_CONTINUE;
        ctx->disk_stamp = stamp;

        std::vector<PrefixEntry> current = list_all_known_prefixes();
        const std::string signature = list_signature(current);
        if (signature == ctx->signature) return G_SOURCE_CONTINUE;

        // The prefix that showed up, so it can be selected afterwards. Only
        // useful while nothing is selected: a row the user picked is never
        // taken away from them by a background change.
        const bool something_selected = selected_entry(ctx) != nullptr;
        std::string newcomer_runner;
        std::string newcomer_code;
        for (const PrefixEntry& entry : current) {
            if (signature.find(entry.runner + ":" + entry.prefix_code + "@" + entry.prefix_path) ==
                    std::string::npos ||
                ctx->signature.find(entry.runner + ":" + entry.prefix_code + "@" +
                                    entry.prefix_path) == std::string::npos) {
                newcomer_runner = entry.runner;
                newcomer_code = entry.prefix_code;
                break;
            }
        }

        ctx->signature = signature;
        refresh_manager(ctx);  // re-reads the list and keeps the selection

        // A prefix that just appeared (a finished restore, say) is selected so
        // its tools are usable straight away - but only if the user was not
        // working on another row.
        if (!something_selected && !newcomer_code.empty()) {
            select_prefix(ctx, newcomer_runner, newcomer_code);
        }
    } catch (const std::exception& e) {
        // A transient error (a folder removed mid-scan) must not kill the timer.
        std::fprintf(stderr, "[wlm] prefix auto-refresh: %s\n", e.what());
    }
    return G_SOURCE_CONTINUE;
}

// The window is going away: stop the timer before the context is freed with it.
void on_manager_destroy(GtkWidget*, gpointer data) {
    auto* ctx = static_cast<ManagerCtx*>(data);
    if (ctx->auto_refresh_id != 0) {
        g_source_remove(ctx->auto_refresh_id);
        ctx->auto_refresh_id = 0;
    }
}

// Reloads the manager's list from disk (runner_config.json +
// prefix_registry.json + a scan of the prefix folders). Called when the window
// first opens, after a create/remove, and by the auto-refresh whenever the set
// of prefixes changed.
void refresh_manager(ManagerCtx* ctx) {
    // Keep the user's place across the refresh (Python keeps the selection by
    // treeview iid).
    std::string selected_key;
    GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(ctx->tree));
    GtkTreeIter selected_iter;
    if (gtk_tree_selection_get_selected(selection, nullptr, &selected_iter)) {
        gchar* runner = nullptr;
        gchar* code = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), &selected_iter, COL_RUNNER_KEY, &runner,
                           COL_PREFIX_CODE, &code, -1);
        if (runner && code) selected_key = std::string(runner) + ":" + code;
        g_free(runner);
        g_free(code);
    }

    gtk_list_store_clear(ctx->store);
    ctx->entries = list_all_known_prefixes();

    GtkTreeIter restore_iter;
    bool restore_selection = false;
    for (const PrefixEntry& entry : ctx->entries) {
        GtkTreeIter row;
        gtk_list_store_append(ctx->store, &row);

        std::string games_display;
        for (const std::string& game : entry.games) {
            if (!games_display.empty()) games_display += ", ";
            games_display += game;
        }
        if (games_display.empty()) games_display = "-";
        const std::string runner_name = cfg::runner_display_name(entry.runner);
        const std::string proton_name = entry.proton_name.empty() ? "-" : entry.proton_name;

        gtk_list_store_set(ctx->store, &row, COL_RUNNER_KEY, entry.runner.c_str(),
                           COL_PREFIX_CODE, entry.prefix_code.c_str(), COL_RUNNER_NAME,
                           runner_name.c_str(), COL_GAMES, games_display.c_str(), COL_PROTON,
                           proton_name.c_str(), COL_PATH, entry.prefix_path.c_str(), -1);

        if (!selected_key.empty() && selected_key == entry.runner + ":" + entry.prefix_code) {
            restore_iter = row;
            restore_selection = true;
        }
    }

    if (restore_selection) gtk_tree_selection_select_iter(selection, &restore_iter);
    gtk_widget_set_visible(ctx->empty_label, ctx->entries.empty());

    // Remember what the list looks like now, so the auto-refresh can tell
    // whether anything actually happened.
    ctx->signature = list_signature(ctx->entries);
    ctx->disk_stamp = disk_stamp();
}

// The entry of the selected row, or nullptr when nothing is selected.
//
// gtk_tree_selection_get_selected() is not used on purpose: right after a
// refresh (which clears and refills the list) a row selected programmatically
// still counts as selected but fails get_selected()'s path revalidation, so a
// prefix created a second ago would answer "select a prefix from the list
// first". get_selected_rows() hands back the selection as it is stored.
const PrefixEntry* selected_entry(ManagerCtx* ctx) {
    GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(ctx->tree));
    GList* rows = gtk_tree_selection_get_selected_rows(selection, nullptr);
    if (rows == nullptr) return nullptr;

    const PrefixEntry* found = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(ctx->store), &iter,
                                static_cast<GtkTreePath*>(rows->data))) {
        gchar* runner = nullptr;
        gchar* code = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), &iter, COL_RUNNER_KEY, &runner,
                           COL_PREFIX_CODE, &code, -1);
        const std::string runner_key = runner ? runner : "";
        const std::string prefix_code = code ? code : "";
        g_free(runner);
        g_free(code);

        for (const PrefixEntry& entry : ctx->entries) {
            if (entry.runner == runner_key && entry.prefix_code == prefix_code) {
                found = &entry;
                break;
            }
        }
    }

    g_list_free_full(rows, reinterpret_cast<GDestroyNotify>(gtk_tree_path_free));
    return found;
}

// Python's get_selected_entry(): asks the user to select a row first instead
// of silently doing nothing.
const PrefixEntry* get_selected_entry(ManagerCtx* ctx) {
    const PrefixEntry* entry = selected_entry(ctx);
    if (entry == nullptr) {
        dialogs::show_info("Info", "Select a prefix from the list first.",
                           GTK_WINDOW(ctx->window));
    }
    return entry;
}

void delete_manager_ctx(gpointer data) { delete static_cast<ManagerCtx*>(data); }

// Uniform button row; the width is only a minimum, so longer labels still fit.
GtkWidget* make_button(const char* label, GCallback callback, gpointer data) {
    GtkWidget* button = gtk_button_new_with_label(label);
    gtk_widget_set_size_request(button, winmode::px(110), -1);
    g_signal_connect(button, "clicked", callback, data);
    return button;
}

void add_text_column(GtkTreeView* tree, const char* title, gint model_column, int min_width,
                     bool expand) {
    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* column =
        gtk_tree_view_column_new_with_attributes(title, renderer, "text", model_column, NULL);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_min_width(column, min_width);
    gtk_tree_view_column_set_expand(column, expand);
    gtk_tree_view_append_column(tree, column);
}

// No exception may ever escape a GTK callback - report it on the status bar
// and in an error dialog instead.
void report_callback_error(const std::string& what, const std::string& detail, GtkWindow* parent) {
    ui::set_status(what + ": " + detail, "error");
    dialogs::show_error("Error", what + "\n" + detail, parent);
}

// Python's prefix_task_is_busy(): only one backup/restore may run per session,
// and the second attempt has to say why it did nothing.
bool refuse_when_task_busy(GtkWindow* parent) {
    if (!tasklog::is_busy()) return false;
    dialogs::show_info("Backup/Restore Busy",
                       "A backup/restore is already running (" + tasklog::busy_label() + ").\n\n"
                       "Only one prefix/game backup or restore can run at a time. Use "
                       "\"Show Logs\" in the Prefix Configuration Manager to check its progress "
                       "or cancel it first.",
                       parent);
    return true;
}

void on_tool_clicked(GtkButton* button, gpointer data) {
    auto* ctx = static_cast<ManagerCtx*>(data);
    try {
        const char* tool =
            static_cast<const char*>(g_object_get_data(G_OBJECT(button), "wlm-tool"));
        if (!tool) return;
        const PrefixEntry* entry = get_selected_entry(ctx);
        if (!entry) return;

        if (std::string(tool) == "winetricks") {
            open_winetricks_dialog(*entry, GTK_WINDOW(ctx->window));
        } else {
            run_prefix_tool(*entry, tool, {}, GTK_WINDOW(ctx->window));
        }
    } catch (const std::exception& e) {
        report_callback_error("Error running prefix tool", e.what(), GTK_WINDOW(ctx->window));
    }
}

void on_open_folder_clicked(GtkButton*, gpointer data) {
    auto* ctx = static_cast<ManagerCtx*>(data);
    try {
        const PrefixEntry* entry = get_selected_entry(ctx);
        if (!entry) return;

        const fs::path folder(entry->prefix_path);
        std::error_code ec;
        if (!fs::is_directory(folder, ec)) {
            dialogs::show_error("Error", "Folder not found:\n" + folder.string(),
                                GTK_WINDOW(ctx->window));
            return;
        }

        int err = 0;
        if (util::open_path(folder.string(), &err)) {
            ui::set_status("Opening folder for prefix " + entry->prefix_code + "...", "secondary");
        } else {
            dialogs::show_error("Error", "Folder not found:\n" + folder.string(),
                                GTK_WINDOW(ctx->window));
        }
    } catch (const std::exception& e) {
        report_callback_error("Error opening folder", e.what(), GTK_WINDOW(ctx->window));
    }
}

void on_backup_clicked(GtkButton*, gpointer data) {
    auto* ctx = static_cast<ManagerCtx*>(data);
    try {
        const PrefixEntry* entry = get_selected_entry(ctx);
        if (!entry) return;
        if (refuse_when_task_busy(GTK_WINDOW(ctx->window))) return;
        backup::open_backup_prefix_dialog(*entry, GTK_WINDOW(ctx->window));
    } catch (const std::exception& e) {
        report_callback_error("Error starting backup", e.what(), GTK_WINDOW(ctx->window));
    }
}

void on_restore_clicked(GtkButton*, gpointer data) {
    auto* ctx = static_cast<ManagerCtx*>(data);
    try {
        if (refuse_when_task_busy(GTK_WINDOW(ctx->window))) return;
        backup::open_restore_backup_dialog(GTK_WINDOW(ctx->window));
    } catch (const std::exception& e) {
        report_callback_error("Error starting restore", e.what(), GTK_WINDOW(ctx->window));
    }
}

void on_remove_clicked(GtkButton*, gpointer data) {
    auto* ctx = static_cast<ManagerCtx*>(data);
    try {
        const PrefixEntry* entry = get_selected_entry(ctx);
        if (!entry) return;

        // Copied out: a successful removal rebuilds ctx->entries below and
        // would otherwise leave a dangling pointer behind.
        const PrefixEntry selected = *entry;
        if (remove_prefix_and_games(selected, GTK_WINDOW(ctx->window))) refresh_manager(ctx);
    } catch (const std::exception& e) {
        report_callback_error("Error removing prefix", e.what(), GTK_WINDOW(ctx->window));
    }
}

void on_show_logs_clicked(GtkButton*, gpointer data) {
    auto* ctx = static_cast<ManagerCtx*>(data);
    try {
        // Raise the backup/restore log window again (its Close button only
        // hides it while the task is still running); when nothing is running
        // say so, like the Python dialog does.
        if (tasklog::is_busy()) {
            tasklog::show_active();
        } else {
            dialogs::show_info("Show Logs", "No backup or restore process is currently running.",
                               GTK_WINDOW(ctx->window));
        }
    } catch (const std::exception& e) {
        report_callback_error("Error showing logs", e.what(), GTK_WINDOW(ctx->window));
    }
}

void on_close_clicked(GtkButton*, gpointer data) {
    auto* ctx = static_cast<ManagerCtx*>(data);
    gtk_window_close(GTK_WINDOW(ctx->window));
}

// ---------------------------------------------------------------------------
// "Create Prefix..."
// ---------------------------------------------------------------------------

// What a finished create dialog hands back, so the manager can refresh and then
// select the new row instead of leaving the list on the old selection.
struct CreatedPrefix {
    std::string runner;
    std::string prefix_code;
};

struct CreatedPrefix;

struct CreateCtx {
    GtkWidget* dialog = nullptr;
    // The window that opened this dialog (the manager). Safe to parent later
    // dialogs to: `dialog` itself is destroyed the moment the prefix exists,
    // while the initialisation it kicks off is still running.
    GtkWidget* owner = nullptr;
    GtkWidget* name_entry = nullptr;
    GtkWidget* runner_combo = nullptr;
    GtkWidget* build_label = nullptr;
    GtkWidget* build_combo = nullptr;
    GtkWidget* location_label = nullptr;
    GtkWidget* hint_label = nullptr;
    GtkWidget* init_checkbox = nullptr;

    // Base folder for the currently selected runner, mirroring the runner
    // dialog: picking a different one persists it as that runner's default.
    std::map<std::string, fs::path> base_dirs;

    // The builds currently listed in build_combo, so the selection can be
    // mapped back to a name + path.
    proton::BuildList builds;

    // Runs after the prefix exists - once straight away, or again when the
    // background wineboot finished.
    std::function<void(const CreatedPrefix&)> on_created;
};

std::string selected_runner_key(GtkComboBox* combo) {
    const int index = gtk_combo_box_get_active(combo);
    static const char* const keys[] = {"wine", "protonge", "protoncachyos", "steamproton"};
    if (index < 0 || index > 3) return "wine";
    return keys[index];
}

bool is_proton_runner(const std::string& runner_key) { return runner_key != "wine"; }

// Fills the build combo for the selected runner and refreshes the two labels
// underneath it. Shared by the runner-changed handler and the initial setup.
void refresh_create_dialog(CreateCtx* ctx) {
    const std::string runner_key = selected_runner_key(GTK_COMBO_BOX(ctx->runner_combo));
    const std::string label = cfg::runner_display_name(runner_key);

    const bool proton = is_proton_runner(runner_key);
    gtk_widget_set_visible(ctx->build_label, proton);
    gtk_widget_set_visible(ctx->build_combo, proton);

    ctx->builds = proton ? (runner_key == "protonge"
                                ? proton::find_protonge_installations()
                                : runner_key == "steamproton"
                                      ? proton::find_steam_proton_installations()
                                      : proton::find_protoncachyos_installations())
                          : proton::BuildList();

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ctx->build_combo));
    for (const auto& build : ctx->builds) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctx->build_combo), build.first.c_str());
    }
    if (!ctx->builds.empty()) gtk_combo_box_set_active(GTK_COMBO_BOX(ctx->build_combo), 0);
    gtk_widget_set_sensitive(ctx->build_combo, !ctx->builds.empty());

    // An empty prefix name means "auto", so the label has to show what the auto
    // code will look like right now.
    const fs::path base = ctx->base_dirs[runner_key];
    const std::string auto_code = proton::generate_next_prefix_code(runner_key);
    gtk_label_set_text(GTK_LABEL(ctx->location_label),
                       ("Folder: " + base.string() + "\nPrefix code: " + auto_code +
                        "  (or your own name above)")
                           .c_str());

    if (proton && ctx->builds.empty()) {
        const char* how = runner_key == "steamproton"
                              ? "Install one from Steam, or drop a build into Steam's "
                                "compatibilitytools.d folder."
                              : "Extract or download one first via the Settings menu.";
        gtk_label_set_text(GTK_LABEL(ctx->hint_label),
                           ("No " + label + " build found. " + how).c_str());
        gtk_style_context_add_class(gtk_widget_get_style_context(ctx->hint_label), "error");
    } else {
        gtk_label_set_text(GTK_LABEL(ctx->hint_label), "");
        gtk_style_context_remove_class(gtk_widget_get_style_context(ctx->hint_label), "error");
    }
}

void on_create_runner_changed(GtkComboBox*, gpointer data) {
    refresh_create_dialog(static_cast<CreateCtx*>(data));
}

void on_create_browse_clicked(GtkButton*, gpointer data) {
    auto* ctx = static_cast<CreateCtx*>(data);
    const std::string runner_key = selected_runner_key(GTK_COMBO_BOX(ctx->runner_combo));

    fs::path initial = ctx->base_dirs[runner_key];
    std::error_code ec;
    if (!fs::is_directory(initial, ec)) initial = cfg::home_dir();

    const std::optional<fs::path> chosen = proton::browse_folder_with_create_option(
        "Choose Location for New " + cfg::runner_display_name(runner_key) + " Prefixes", initial,
        GTK_WINDOW(ctx->dialog));
    if (!chosen) return;

    // Same as the runner dialog: the folder becomes that runner's default, so a
    // prefix created later lands in the same place.
    ctx->base_dirs[runner_key] = *chosen;
    proton::set_prefix_base_dir(runner_key, *chosen);
    refresh_create_dialog(ctx);
}

// A Proton build ships a ready-made prefix at files/share/default_pfx/ (or
// default_pfx_arm64/ on aarch64), and its own `proton` script bootstraps a new
// compat data path by copying that folder to <compat data>/pfx - see copy_pfx()
// in the proton script. This launcher does the same instead of hunting for a
// wineboot binary, because current Proton builds have none: the only wineboot
// they contain is wineboot.exe, a Windows program that needs a working prefix
// before it can run at all. Returns "" when this build ships no template.
std::string find_proton_default_prefix(const std::string& proton_script_path) {
    const fs::path root = fs::path(proton_script_path).parent_path();

    // Proton only uses the arm64 layout when it runs on aarch64 AND the build
    // really has it; the same rule is applied here.
    std::vector<fs::path> candidates;
#if defined(__aarch64__)
    candidates.push_back(root / "files" / "share" / "default_pfx_arm64");
#endif
    candidates.push_back(root / "files" / "share" / "default_pfx");

    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (fs::is_directory(candidate, ec)) return candidate.string();
    }
    return "";
}

// Does this DIRECTORY name end with one of Wine's built-in library folders?
//
// The template stores its Wine libraries as RELATIVE links
// ("../../../../../lib/wine/x86_64-windows/foo.dll"), which only resolve from
// inside the build. Copied into a prefix that lives somewhere else they dangle,
// so they are rewritten to absolute paths - which is exactly what Proton's own
// pfx_copy() does, testing os.path.dirname() of the link for the same reason.
bool is_wine_library_dir(const std::string& dir) {
    static const char* const suffixes[] = {
        "/lib/wine/i386-unix",     "/lib/wine/i386-windows",
        "/lib/wine/x86_64-unix",   "/lib/wine/x86_64-windows",
        "/lib/wine/aarch64-unix",  "/lib/wine/aarch64-windows",
        // older layouts some builds still ship:
        "/lib64/wine/x86_64-unix", "/lib64/wine/x86_64-windows",
    };
    for (const char* suffix : suffixes) {
        const size_t n = std::strlen(suffix);
        if (dir.size() >= n && dir.compare(dir.size() - n, n, suffix) == 0) return true;
    }
    return false;
}

// Copies a prefix template while preserving SYMLINKS. That matters: default_pfx
// fills drive_c/Program Files with links into files/lib/wine/*-windows/, and
// following them would turn the 41 MB template into hundreds of megabytes of
// duplicated DLLs.
//
// Returns false and fills *error on the first thing it could not copy.
bool copy_prefix_template(const fs::path& from, const fs::path& to, std::string* error,
                          long long* bytes_copied) {
    std::error_code ec;

    fs::create_directories(to, ec);
    if (ec) {
        if (error) *error = "could not create " + to.string() + ": " + ec.message();
        return false;
    }

    // The "!ec" matters: a CLEAR error_code is falsy, so the condition reads
    // "no error so far", not "there is an error".
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path src = it->path();
        const fs::path dst = to / src.filename();

        if (it->is_symlink(ec)) {
            fs::path target = fs::read_symlink(src, ec);
            if (ec) {
                if (error) *error = "could not read the link " + src.string();
                return false;
            }
            // A relative link into the Proton build only resolves from there,
            // so anchor it to the build instead of leaving it to dangle. The
            // test is on the link's DIRECTORY, because that is the part naming
            // the Wine library.
            if (is_wine_library_dir(target.parent_path().string())) {
                target = (src.parent_path() / target).lexically_normal();
            }
            fs::remove(dst, ec);  // a leftover link would make create_symlink fail
            ec.clear();
            fs::create_symlink(target, dst, ec);
            if (ec) {
                if (error) *error = "could not link " + dst.string() + ": " + ec.message();
                return false;
            }
            continue;
        }

        if (it->is_directory(ec)) {
            if (!copy_prefix_template(src, dst, error, bytes_copied)) return false;
            continue;
        }

        if (!fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec)) {
            if (error) *error = "could not copy " + src.string() + ": " + ec.message();
            return false;
        }
        // Registry files are read-only in some builds; keep what the template
        // says so Wine does not rewrite them on the first boot.
        const fs::perms perms = fs::status(src, ec).permissions();
        ec.clear();
        fs::permissions(dst, perms, ec);
        ec.clear();
        if (bytes_copied) *bytes_copied += static_cast<long long>(fs::file_size(src, ec));
        ec.clear();
    }
    if (ec) {
        if (error) *error = "could not read " + from.string() + ": " + ec.message();
        return false;
    }
    return true;
}

// The `wineboot` that belongs to a Proton build, i.e. the Wine the games will
// actually run under. Proton ships its own, and using the system one would
// build a prefix the Proton build then has to migrate. Mirrors the lookup the
// Winetricks branch does, so both Proton layouts (files/bin and dist/bin) work.
//
// This is only a FALLBACK now: current Proton builds contain no wineboot binary
// at all, so find_proton_default_prefix() normally does the work. Older builds,
// and variants that still ship one, are covered here.
std::string find_proton_wineboot(const std::string& proton_script_path) {
    const fs::path root = fs::path(proton_script_path).parent_path();
    const fs::path candidates[] = {
        root / "files" / "bin" / "wineboot",
#if defined(__aarch64__)
        root / "files" / "bin-arm64" / "wineboot",
#endif
        root / "files" / "bin-x86_64" / "wineboot",
        root / "dist" / "bin" / "wineboot",
    };
    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (fs::exists(candidate, ec)) return candidate.string();
    }
    return "";
}

}  // namespace

// A new prefix is only an empty folder until Wine has booted into it once -
// that is what creates drive_c, system.reg/user.reg and the dosdevices links.
// Without this the row shows up in the list but every tool (winecfg,
// Winetricks, PLAY) has to build the whole thing first.
//
// Runs on the caller's (background) thread, reports into `task`, and returns ""
// on success or the reason it could not be initialised. A prefix that cannot be
// initialised is still created and registered - the folder is valid and Wine (or
// the Proton build) fills it on the next run - so this never fails a creation.
std::string initialize_prefix(const std::string& runner_key, const fs::path& prefix_path,
                              const std::string& proton_path, const tasklog::TaskPtr& task) {
    const std::string label = cfg::runner_display_name(runner_key);

    // Vanilla Wine uses the prefix folder itself. Proton keeps its Wine prefix
    // in "pfx" inside the compat data path - the same place the proton script
    // and the Winetricks branch of run_prefix_tool() use.
    const fs::path wine_prefix = (runner_key == "wine") ? prefix_path : prefix_path / "pfx";

    std::string wineboot = "wineboot";
    if (runner_key == "wine") {
        if (!has_command("wineboot")) {
            return "The 'wineboot' command was not found.\n\n"
                   "Install Wine first, e.g.:\n"
                   "  sudo apt install wine wine64\n"
                   "  sudo dnf install wine\n"
                   "  sudo pacman -S wine";
        }
    } else {
        // Proton's own way of making a prefix: copy the ready-made one it ships.
        // This is tried FIRST because current builds have no wineboot binary to
        // run - the only wineboot they contain is wineboot.exe, which needs a
        // prefix that already exists.
        const std::string template_dir = find_proton_default_prefix(proton_path);
        if (!template_dir.empty()) {
            task->append_line("Initialising the " + label + " prefix...");
            task->append_line("Wine prefix: " + wine_prefix.string());
            task->append_line("Copying the prefix that ships with " + label + "...");

            std::error_code ec;
            fs::create_directories(wine_prefix.parent_path(), ec);
            // A half-copied prefix would be worse than none, so start clean.
            fs::remove_all(wine_prefix, ec);
            ec.clear();

            std::string copy_error;
            long long copied = 0;
            if (!copy_prefix_template(template_dir, wine_prefix, &copy_error, &copied)) {
                return "Could not prepare the " + label + " prefix:\n" + copy_error +
                       "\n\nThe folder was created but stays empty - PLAY the game once and the "
                       "build will initialise it itself.";
            }

            // Wine recreates these on its first boot, but a prefix that already
            // looks complete does not have to, and the launcher's own tools
            // expect them to be there.
            const fs::path dosdevices = wine_prefix / "dosdevices";
            fs::create_directories(dosdevices, ec);
            ec.clear();
            if (!fs::exists(dosdevices / "c:", ec)) {
                fs::create_symlink(fs::path("..") / "drive_c", dosdevices / "c:", ec);
                ec.clear();
            }
            if (!fs::exists(dosdevices / "z:", ec)) {
                fs::create_symlink("/", dosdevices / "z:", ec);
                ec.clear();
            }

            task->append_line("The prefix is initialised and ready to configure (" +
                              util::human_size(static_cast<double>(copied)) + ").");
            return "";
        }

        // No template: fall back to booting a wineboot, for older builds.
        wineboot = find_proton_wineboot(proton_path);
        if (wineboot.empty()) {
            return "This " + label +
                   " build contains neither a ready-made prefix to copy nor a 'wineboot' "
                   "binary:\n" +
                   proton_path +
                   "\n\nThe folder was created but stays empty - PLAY the game once and the "
                   "build will initialise it itself.";
        }
    }

    task->append_line("Initialising the " + label + " prefix...");
    task->append_line("Wine prefix: " + wine_prefix.string());

    util::EnvMap env;
    env["WINEPREFIX"] = wine_prefix.string();
    // A fresh prefix must not stop for the Gecko/Mono installer dialogs, and
    // the debugger noise has no place in this log.
    const char* inherited = std::getenv("WINEDLLOVERRIDES");
    env["WINEDLLOVERRIDES"] =
        inherited != nullptr ? std::string("mscoree,mshtml=") + inherited : "mscoree,mshtml=";
    env["WINEDEBUG"] = "-all";
    if (runner_key != "wine") {
        // Proton's Wine still wants to know where Steam is; the compat data path
        // is the prefix folder itself (same as the launch scripts).
        env["STEAM_COMPAT_DATA_PATH"] = prefix_path.string();
        env["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = proton::find_steam_install_path().string();
    }

    std::error_code ec;
    fs::create_directories(wine_prefix, ec);

    int exit_code = -1;
    int exec_err = 0;
    std::string output;
    if (!util::run_captured({wineboot, "--init"}, env, &exit_code, &output, &exec_err)) {
        return "Could not run " + wineboot + ":\n" + std::strerror(exec_err) +
               "\n\nThe folder was created but stays empty.";
    }

    // wineboot is chatty even with WINEDEBUG=-all, so only its errors are worth
    // showing - the prefix either booted or it did not.
    std::string errors;
    {
        std::istringstream lines(output);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.find("err:") == std::string::npos && line.find("error") == std::string::npos &&
                line.find("failed") == std::string::npos) {
                continue;
            }
            if (!errors.empty()) errors += "\n";
            errors += line;
        }
    }

    if (exit_code != 0) {
        task->append_line("ERROR: wineboot exited with code " + std::to_string(exit_code) + ".");
        if (!errors.empty()) task->append_line(errors);
        return "wineboot exited with code " + std::to_string(exit_code) + "." +
               (errors.empty() ? "" : "\n\n" + errors);
    }

    task->append_line("The prefix is initialised and ready to configure.");
    return "";
}

namespace {

// What the initialisation thread hands back to the main thread, so the callback
// does not have to reach into a dialog that is about to be destroyed.
struct InitWork {
    std::string problem;  // empty = the prefix booted
    std::string prefix_code;
    tasklog::TaskPtr task;
    GtkWindow* owner;
    std::function<void(const CreatedPrefix&)> created_cb;
    CreatedPrefix created;
};

// The "_Create" click. Returns what was created, or nullopt after showing why
// not - the caller keeps the dialog open in that case.
std::optional<CreatedPrefix> create_prefix_now(CreateCtx* ctx) {
    const std::string runner_key = selected_runner_key(GTK_COMBO_BOX(ctx->runner_combo));
    const std::string label = cfg::runner_display_name(runner_key);

    std::string proton_name;
    std::string proton_path;
    if (is_proton_runner(runner_key)) {
        if (ctx->builds.empty()) {
            dialogs::show_error("Error",
                                "No " + label +
                                    " build found.\nExtract or download one first via the "
                                    "Settings menu.",
                                GTK_WINDOW(ctx->dialog));
            return std::nullopt;
        }
        const int index = gtk_combo_box_get_active(GTK_COMBO_BOX(ctx->build_combo));
        if (index < 0 || index >= static_cast<int>(ctx->builds.size())) {
            dialogs::show_error("Error", "Select a " + label + " build first.",
                                GTK_WINDOW(ctx->dialog));
            return std::nullopt;
        }
        proton_name = ctx->builds[static_cast<size_t>(index)].first;
        proton_path = ctx->builds[static_cast<size_t>(index)].second.string();
    }

    // Empty name -> the next free GAME### code, exactly like running a game does.
    const std::string typed = trim(gtk_entry_get_text(GTK_ENTRY(ctx->name_entry)));
    std::string prefix_code;
    if (typed.empty()) {
        prefix_code = proton::generate_next_prefix_code(runner_key);
    } else {
        prefix_code = proton::sanitize_prefix_name(typed);
        if (prefix_code.empty()) {
            dialogs::show_error("Error",
                                "\"" + typed +
                                    "\" is not a usable prefix name.\n"
                                    "Use letters, digits, spaces, dots or dashes.",
                                GTK_WINDOW(ctx->dialog));
            return std::nullopt;
        }
        if (proton::prefix_code_taken(runner_key, prefix_code)) {
            dialogs::show_error("Error",
                                "A " + label + " prefix named \"" + prefix_code +
                                    "\" already exists.\nPick another name.",
                                GTK_WINDOW(ctx->dialog));
            return std::nullopt;
        }
    }

    const fs::path prefix_path = ctx->base_dirs[runner_key] / prefix_code;

    std::error_code ec;
    fs::create_directories(prefix_path, ec);
    if (ec) {
        dialogs::show_error("Error",
                            "Could not create the prefix folder:\n" + prefix_path.string() +
                                "\n\n" + ec.message(),
                            GTK_WINDOW(ctx->dialog));
        return std::nullopt;
    }

    // Exactly the registration a prefix created by running a game gets, so the
    // manager, the backup tools and the runner dialog all treat it identically.
    proton::record_prefix_usage(runner_key, prefix_code, prefix_path, proton_name, proton_path);

    // Registered first, so the list shows the prefix right away and the task log
    // window is anchored to something real; only then is Wine booted into it.
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctx->init_checkbox))) {
        tasklog::TaskPtr init_task = tasklog::open(
            "Create Prefix - " + prefix_code, GTK_WINDOW(ctx->owner), /*cancellable=*/false,
            "Hide this window. Creating the prefix keeps running.");

        // wineboot takes seconds and must not block the GTK thread, so it runs
        // on its own thread and only the outcome is bounced back to the main one.
        std::thread([runner = runner_key, prefix_code, folder = prefix_path,
                     build = proton_path, init_task, owner = ctx->owner,
                     created_cb = ctx->on_created]() {
            const std::string problem = initialize_prefix(runner, folder, build, init_task);

            // The callback owns the payload (unique_ptr); no free-notify, which
            // would delete it a second time.
            g_idle_add(
                [](gpointer data) -> gboolean {
                    std::unique_ptr<InitWork> work(static_cast<InitWork*>(data));
                    if (work->problem.empty()) {
                        ui::set_status("Created and initialised prefix \'" +
                                           work->prefix_code + "\'.",
                                       "success");
                    } else {
                        ui::set_status("Created prefix \'" + work->prefix_code +
                                           "\' but could not initialise it.",
                                       "warning");
                        dialogs::show_error("Prefix Not Initialised", work->problem, work->owner);
                    }
                    work->task->mark_finished();
                    if (work->created_cb) work->created_cb(work->created);
                    return G_SOURCE_REMOVE;
                },
                new InitWork{problem, prefix_code, init_task, GTK_WINDOW(owner), created_cb,
                             CreatedPrefix{runner, prefix_code}});
        }).detach();
    } else if (ctx->on_created) {
        ctx->on_created(CreatedPrefix{runner_key, prefix_code});
    }

    return CreatedPrefix{runner_key, prefix_code};
}

// "Create Prefix..." from the manager's top bar. The dialog only collects the
// details; everything it writes goes through the same proton:: calls a game run
// uses, and the existing "a prefix is created when you PLAY" path is untouched.
void open_create_prefix_dialog(GtkWindow* parent,
                               const std::function<void(const CreatedPrefix&)>& on_created) {
    GtkWidget* dialog = nullptr;
    try {
        auto* ctx = new CreateCtx();

        dialog = gtk_dialog_new_with_buttons(
            "Create New Prefix", parent,
            static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
            "_Create", GTK_RESPONSE_OK, "_Cancel", GTK_RESPONSE_CANCEL, NULL);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
        ctx->dialog = dialog;
        gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
        g_object_set_data_full(G_OBJECT(dialog), "create-prefix-ctx", ctx,
                               [](gpointer p) { delete static_cast<CreateCtx*>(p); });
        ctx->on_created = on_created;
        ctx->owner = GTK_WIDGET(parent != nullptr ? parent : ui::window);

        for (const auto& runner_root : cfg::default_prefix_roots()) {
            ctx->base_dirs[runner_root.first] = proton::get_prefix_base_dir(runner_root.first);
        }

        GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        gtk_container_set_border_width(GTK_CONTAINER(content), winmode::px(15));
        gtk_box_set_spacing(GTK_BOX(content), winmode::px(8));

        GtkWidget* intro = gtk_label_new(
            "Creates a prefix you can set up before any game uses it.\n"
            "Running a game still creates its own prefix automatically - this is\n"
            "just for preparing one ahead of time (winecfg, fonts, winetricks).");
        gtk_label_set_xalign(GTK_LABEL(intro), 0.0f);
        gtk_box_pack_start(GTK_BOX(content), intro, FALSE, FALSE, 0);

        // ---- prefix name (optional) ----
        GtkWidget* name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(8));
        GtkWidget* name_caption = gtk_label_new("Prefix name:");
        gtk_label_set_xalign(GTK_LABEL(name_caption), 0.0f);
        gtk_widget_set_size_request(name_caption, winmode::px(110), -1);
        gtk_box_pack_start(GTK_BOX(name_row), name_caption, FALSE, FALSE, 0);

        ctx->name_entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->name_entry), "leave empty for GAMEXXX");
        gtk_entry_set_width_chars(GTK_ENTRY(ctx->name_entry), 28);
        gtk_entry_set_activates_default(GTK_ENTRY(ctx->name_entry), TRUE);
        gtk_box_pack_start(GTK_BOX(name_row), ctx->name_entry, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(content), name_row, FALSE, FALSE, 0);

        // ---- runner ----
        GtkWidget* runner_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(8));
        GtkWidget* runner_caption = gtk_label_new("Runner:");
        gtk_label_set_xalign(GTK_LABEL(runner_caption), 0.0f);
        gtk_widget_set_size_request(runner_caption, winmode::px(110), -1);
        gtk_box_pack_start(GTK_BOX(runner_row), runner_caption, FALSE, FALSE, 0);

        ctx->runner_combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctx->runner_combo), "Wine");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctx->runner_combo), "Proton GE");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctx->runner_combo), "Proton-CachyOS");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctx->runner_combo),
                                       "Proton (Steam)");
        gtk_combo_box_set_active(GTK_COMBO_BOX(ctx->runner_combo), 0);
        g_signal_connect(ctx->runner_combo, "changed", G_CALLBACK(on_create_runner_changed), ctx);
        gtk_box_pack_start(GTK_BOX(runner_row), ctx->runner_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(content), runner_row, FALSE, FALSE, 0);

        // ---- Proton build (only for the two Proton runners) ----
        GtkWidget* build_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(8));
        ctx->build_label = gtk_label_new("Proton build:");
        gtk_label_set_xalign(GTK_LABEL(ctx->build_label), 0.0f);
        gtk_widget_set_size_request(ctx->build_label, winmode::px(110), -1);
        gtk_box_pack_start(GTK_BOX(build_row), ctx->build_label, FALSE, FALSE, 0);

        ctx->build_combo = gtk_combo_box_text_new();
        gtk_box_pack_start(GTK_BOX(build_row), ctx->build_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(content), build_row, FALSE, FALSE, 0);

        // ---- location ----
        ctx->location_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ctx->location_label), 0.0f);
        gtk_label_set_selectable(GTK_LABEL(ctx->location_label), TRUE);
        gtk_box_pack_start(GTK_BOX(content), ctx->location_label, FALSE, FALSE, 0);

        GtkWidget* browse_btn = gtk_button_new_with_label("Change Folder...");
        g_signal_connect(browse_btn, "clicked", G_CALLBACK(on_create_browse_clicked), ctx);
        gtk_widget_set_halign(browse_btn, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(content), browse_btn, FALSE, FALSE, 0);

        ctx->hint_label = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(ctx->hint_label), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(ctx->hint_label), TRUE);
        gtk_box_pack_start(GTK_BOX(content), ctx->hint_label, FALSE, FALSE, 0);

        // A folder on its own is not a prefix yet: Wine still has to boot into
        // it once to create drive_c, the registry files and the dosdevices
        // links. On by default, because that is what makes the new prefix
        // immediately usable with winecfg / Winetricks / PLAY.
        ctx->init_checkbox = gtk_check_button_new_with_label(
            "Set it up now (create a real working prefix)");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctx->init_checkbox), TRUE);
        gtk_widget_set_tooltip_text(
            ctx->init_checkbox,
            "Runs wineboot with this runner's own Wine, so the prefix is a working one "
            "instead of an empty folder. Takes a few seconds.");
        gtk_box_pack_start(GTK_BOX(content), ctx->init_checkbox, FALSE, FALSE, 0);

        // Populates the build combo, the folder/auto-code label and the hint.
        refresh_create_dialog(ctx);
        gtk_widget_show_all(dialog);
        // Wine has no build to pick: show_all() just revealed those two.
        if (!is_proton_runner(selected_runner_key(GTK_COMBO_BOX(ctx->runner_combo)))) {
            gtk_widget_hide(ctx->build_label);
            gtk_widget_hide(ctx->build_combo);
        }

        // Nested loop (like every other dialog here): a refused Create - bad
        // name, name already taken - keeps the dialog open with the reason
        // instead of closing on the user.
        std::optional<CreatedPrefix> created;
        while (true) {
            const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
            if (response != GTK_RESPONSE_OK) break;
            created = create_prefix_now(ctx);
            if (created) break;
        }

        if (created) {
            const std::string display = cfg::runner_display_name(created->runner);
            ui::set_status("Created " + display + " prefix '" + created->prefix_code + "'.",
                           "success");
            if (on_created) on_created(*created);
            dialogs::show_info("Prefix Created",
                               "Created " + display + " prefix:\n\n  " + created->prefix_code +
                                   "\n\nIt is listed in the Prefix Configuration Manager, where "
                                   "you can run winecfg, Explorer or Winetricks on it.",
                               parent);
        }

        // The context is freed together with the dialog object, so no widget may
        // be touched after this.
        gtk_widget_destroy(dialog);
    } catch (const std::exception& e) {
        if (dialog != nullptr && GTK_IS_WIDGET(dialog)) gtk_widget_destroy(dialog);
        report_callback_error("Error creating prefix", e.what(), parent);
    }
}

// Puts the cursor on one row, so a prefix that was just created is selected
// right away and its tools can be used without a second click.
void select_prefix(ManagerCtx* ctx, const std::string& runner_key,
                   const std::string& prefix_code) {
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ctx->store), &iter)) return;
    do {
        gchar* runner = nullptr;
        gchar* code = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), &iter, COL_RUNNER_KEY, &runner,
                           COL_PREFIX_CODE, &code, -1);
        const bool match =
            runner != nullptr && code != nullptr && runner_key == runner && prefix_code == code;
        g_free(runner);
        g_free(code);
        if (match) {
            // Just select it: the list is short enough that the row is always
            // visible, and scrolling it into view can drop the selection again.
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(ctx->tree)),
                                           &iter);
            return;
        }
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(ctx->store), &iter));
}

void on_create_prefix_clicked(GtkButton*, gpointer data) {
    auto* ctx = static_cast<ManagerCtx*>(data);
    open_create_prefix_dialog(GTK_WINDOW(ctx->window), [ctx](const CreatedPrefix& created) {
        refresh_manager(ctx);
        select_prefix(ctx, created.runner, created.prefix_code);
    });
}

}  // namespace

std::vector<PrefixEntry> list_all_known_prefixes() {
    // Every prefix that was ever registered, keyed "runner:prefix_code" so the
    // later scan can never overwrite a registry entry (Python uses a dict the
    // same way). One prefix may be used by several games, so the games are
    // attached afterwards.
    std::map<std::string, PrefixEntry> by_key;

    const json registry = cfg::load_prefix_registry();
    for (const auto& kv : registry.items()) {
        const json& entry_json = kv.value();
        const std::string runner = json_str(entry_json, "runner");
        const std::string code = json_str(entry_json, "prefix_code");
        const std::string path = json_str(entry_json, "prefix_path");
        if (runner.empty() || code.empty() || path.empty()) continue;

        PrefixEntry entry;
        entry.runner = runner;
        entry.prefix_code = code;
        entry.prefix_path = path;
        entry.proton_name = json_str(entry_json, "proton_name");
        entry.proton_path = json_str(entry_json, "proton_path");
        by_key[runner + ":" + code] = std::move(entry);
    }

    // Scan the default prefix folders plus every custom location saved in
    // prefix_location_config.json - prefixes missing from the registry (found
    // by hand, or left over from an older version) still belong in the list.
    std::vector<std::pair<std::string, fs::path>> scan_roots;
    for (const auto& kv : cfg::default_prefix_roots()) {
        scan_roots.emplace_back(kv.first, kv.second);
    }
    const json loc_cfg = cfg::load_prefix_location_config();
    for (const auto& kv : loc_cfg.items()) {
        const std::string custom = kv.value().is_string() ? kv.value().get<std::string>() : "";
        if (!custom.empty()) scan_roots.emplace_back(kv.key(), fs::path(custom));
    }

    for (const auto& root : scan_roots) {
        std::error_code ec;
        if (!fs::is_directory(root.second, ec)) continue;

        fs::directory_iterator it(root.second, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code type_ec;
            if (!it->is_directory(type_ec) || type_ec) continue;

            const std::string code = it->path().filename().string();
            const std::string key = root.first + ":" + code;
            if (by_key.count(key) != 0) continue;

            PrefixEntry entry;
            entry.runner = root.first;
            entry.prefix_code = code;
            entry.prefix_path = it->path().string();
            by_key[key] = std::move(entry);
        }
    }

    // Attach the games of runner_config.json to their prefix (a prefix can be
    // shared by several games) and sort them by name.
    std::map<std::string, std::vector<std::string>> games_by_key;
    const json runner_cfg = cfg::load_runner_config();
    for (const auto& kv : runner_cfg.items()) {
        const json& entry = kv.value();
        const std::string runner = json_str(entry, "runner");
        const std::string code = json_str(entry, "prefix_code");
        if (runner.empty() || code.empty()) continue;
        games_by_key[runner + ":" + code].push_back(kv.key());
    }

    std::vector<PrefixEntry> result;
    result.reserve(by_key.size());
    for (auto& kv : by_key) {
        auto games = games_by_key.find(kv.first);
        if (games != games_by_key.end()) {
            std::sort(games->second.begin(), games->second.end());
            kv.second.games = std::move(games->second);
        }
        result.push_back(std::move(kv.second));
    }

    std::sort(result.begin(), result.end(), [](const PrefixEntry& a, const PrefixEntry& b) {
        if (a.runner != b.runner) return a.runner < b.runner;
        return a.prefix_code < b.prefix_code;
    });
    return result;
}

void run_prefix_tool(const PrefixEntry& entry, const std::string& tool,
                     const std::vector<std::string>& extra_args, GtkWindow* parent) {
    // Every branch below can raise a dialog (missing prefix folder, missing
    // winetricks, missing Proton build, failed exec), and this is called from
    // a GTK callback - nothing may escape it.
    try {
        const fs::path prefix_path(entry.prefix_path);
        if (!fs::is_directory(prefix_path)) {
            dialogs::show_error("Error", "Prefix folder not found on disk:\n" +
                                             prefix_path.string(),
                                parent);
            return;
        }

        if (tool == "winetricks" && !has_command("winetricks")) {
            dialogs::show_error("Error",
                                "The 'winetricks' command was not found.\n"
                                "Install it first, e.g.:\n"
                                "  sudo apt install winetricks\n"
                                "  sudo dnf install winetricks\n"
                                "  sudo pacman -S winetricks",
                                parent);
            return;
        }

        // Env is only handed to this child process, the launcher's own
        // environment stays untouched.
        util::EnvMap env;
        std::vector<std::string> command;
        const std::string runner_label = cfg::runner_display_name(entry.runner);

        if (entry.runner == "wine") {
            // Vanilla Wine: WINEPREFIX is enough, the system wine/winecfg are
            // used as they are.
            env["WINEPREFIX"] = entry.prefix_path;
            if (tool == "winecfg") {
                command = {"winecfg"};
            } else if (tool == "winetricks") {
                command = {"winetricks"};
                command.insert(command.end(), extra_args.begin(), extra_args.end());
            } else {
                command = {"wine", tool};
            }
        } else {
            // A Proton prefix must know which build it belongs to; when the
            // registry has no (or a stale) record, ask once and remember the
            // answer so the same prefix never asks again.
            std::string proton_path = entry.proton_path;
            if (proton_path.empty() || !fs::exists(proton_path)) {
                std::optional<std::pair<std::string, fs::path>> picked =
                    pick_proton_build_dialog(entry.runner, parent);
                if (!picked) return;  // cancelled: do nothing, like the Python
                proton::record_prefix_usage(entry.runner, entry.prefix_code, prefix_path,
                                            picked->first, picked->second.string());
                proton_path = picked->second.string();
            }

            if (tool == "winetricks") {
                const fs::path pfx_path = prefix_path / "pfx";
                if (!fs::is_directory(pfx_path)) {
                    dialogs::show_error(
                        "Error",
                        "This prefix doesn't have a Wine prefix yet ('pfx' folder missing):\n" +
                            pfx_path.string() +
                            "\n\nRun this game/app via PLAY or APPS SETUP at least once first, "
                            "so Proton\n can create it, then try Winetricks again.",
                        parent);
                    return;
                }

                const std::string wine_bin = find_proton_wine_binary(proton_path);
                if (wine_bin.empty()) {
                    dialogs::show_error(
                        "Error",
                        "Could not find the 'wine' binary inside this Proton build:\n" +
                            proton_path,
                        parent);
                    return;
                }

                // Point winetricks at THIS Proton's wine (and wineserver), so
                // its changes really land inside the selected prefix.
                env["WINE"] = wine_bin;
                const fs::path wineserver = fs::path(wine_bin).parent_path() / "wineserver";
                if (fs::exists(wineserver)) env["WINESERVER"] = wineserver.string();
                env["WINEPREFIX"] = pfx_path.string();
                command = {"winetricks"};
                command.insert(command.end(), extra_args.begin(), extra_args.end());
            } else {
                env["STEAM_COMPAT_DATA_PATH"] = entry.prefix_path;
                env["STEAM_COMPAT_CLIENT_INSTALL_PATH"] =
                    proton::find_steam_install_path().string();
                command = {proton_path, "run", tool};
            }
        }

        const std::string tool_label = tool_display_name(tool);

        if (tool == "winetricks") {
            // Winetricks runs interactively, so its output must be streamed
            // into the Logs window instead of being thrown away like the
            // silent tools (the Python version opens a pty for the same reason).
            const std::string task_key = "winetricks-" + entry.runner + "-" + entry.prefix_code;
            int exec_err = 0;
            live_log::GamePtr game = live_log::launch_tracked(task_key, command, &exec_err, env);
            if (!game) {
                if (exec_err == ENOENT) {
                    dialogs::show_error("Error", "Command not found: " + join_command(command),
                                        parent);
                    ui::set_status("Error: winetricks command not found.", "error");
                } else {
                    const std::string reason = std::strerror(exec_err);
                    dialogs::show_error("Error", "Failed to launch Winetricks:\n" + reason, parent);
                    ui::set_status("Error launching Winetricks: " + reason, "error");
                }
                return;
            }
            live_log::open_log_window(task_key);
            ui::set_status("Running Winetricks for prefix " + entry.prefix_code + " (" +
                               runner_label + ")...",
                           "secondary");
            return;
        }

        // winecfg / explorer / uninstaller: fire and forget, stdout to
        // /dev/null like subprocess.Popen(..., stdout=DEVNULL).
        int err = 0;
        if (!util::spawn_detached(command, env, &err)) {
            if (err == ENOENT) {
                dialogs::show_error("Error", "Command not found: " + join_command(command), parent);
                ui::set_status("Error: runner command not found.", "error");
            } else {
                const std::string reason = std::strerror(err);
                dialogs::show_error("Error",
                                    "Failed to launch " + tool_label + ":\n" + reason, parent);
                ui::set_status("Error launching " + tool_label + ": " + reason, "error");
            }
            return;
        }
        ui::set_status("Opening " + tool_label + " for prefix " + entry.prefix_code + " (" +
                           runner_label + ")...",
                       "secondary");
    } catch (const std::exception& e) {
        report_callback_error("Error running " + tool, e.what(), parent);
    }
}

void open_winetricks_dialog(const PrefixEntry& entry, GtkWindow* parent) {
    // Transient for the caller (the Prefix Configuration Manager) so the picker
    // always stacks ON TOP of it instead of ending up behind it - the Tk
    // version had to release and re-grab the parent's modal grab by hand for
    // this, because a dialog hidden behind its parent made the calling buttons
    // look stuck.
    GtkWindow* parent_win = parent ? parent : ui::window;
    GtkWidget* dialog = nullptr;

    try {
        dialog = gtk_dialog_new_with_buttons(
            ("Winetricks - " + entry.prefix_code).c_str(), parent_win,
            static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
            "_Cancel", GTK_RESPONSE_CANCEL, "Open _Interactive Menu",
            RESPONSE_WINETRICKS_INTERACTIVE, "Run _Winetricks", RESPONSE_WINETRICKS_RUN, NULL);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), RESPONSE_WINETRICKS_RUN);
        gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

        GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        gtk_container_set_border_width(GTK_CONTAINER(content), winmode::px(15));
        gtk_box_set_spacing(GTK_BOX(content), winmode::px(8));

        GtkWidget* header = gtk_label_new(
            ("Select components to install into prefix '" + entry.prefix_code + "':").c_str());
        gtk_label_set_xalign(GTK_LABEL(header), 0.0f);
        gtk_box_pack_start(GTK_BOX(content), header, FALSE, FALSE, 0);

        // Two columns of checkboxes, one per common verb (the Python picker
        // grids them the same way).
        GtkWidget* grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), winmode::px(20));
        gtk_grid_set_row_spacing(GTK_GRID(grid), winmode::px(2));
        gtk_widget_set_margin_top(grid, winmode::px(6));
        gtk_widget_set_margin_bottom(grid, winmode::px(6));

        std::vector<GtkWidget*> checks;
        checks.reserve(COMMON_VERB_COUNT);
        for (size_t i = 0; i < COMMON_VERB_COUNT; ++i) {
            const std::string text = std::string(COMMON_WINETRICKS_VERBS[i].first) + " - " +
                                     COMMON_WINETRICKS_VERBS[i].second;
            GtkWidget* check = gtk_check_button_new_with_label(text.c_str());
            gtk_widget_set_halign(check, GTK_ALIGN_START);
            gtk_grid_attach(GTK_GRID(grid), check, static_cast<int>(i % 2),
                            static_cast<int>(i / 2), 1, 1);
            checks.push_back(check);
        }
        gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 0);

        GtkWidget* extra_label = gtk_label_new("Additional/custom verb(s) (space separated):");
        gtk_label_set_xalign(GTK_LABEL(extra_label), 0.0f);
        gtk_style_context_add_class(gtk_widget_get_style_context(extra_label), "dim-label");
        gtk_box_pack_start(GTK_BOX(content), extra_label, FALSE, FALSE, 0);

        GtkWidget* extra_entry = gtk_entry_new();
        gtk_entry_set_width_chars(GTK_ENTRY(extra_entry), 64);
        gtk_box_pack_start(GTK_BOX(content), extra_entry, FALSE, FALSE, 0);

        gtk_widget_show_all(dialog);

        while (true) {
            const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
            if (response != RESPONSE_WINETRICKS_RUN &&
                response != RESPONSE_WINETRICKS_INTERACTIVE) {
                break;  // Cancel or the window close button
            }

            std::vector<std::string> verbs;
            if (response == RESPONSE_WINETRICKS_RUN) {
                for (size_t i = 0; i < COMMON_VERB_COUNT; ++i) {
                    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checks[i]))) {
                        verbs.push_back(COMMON_WINETRICKS_VERBS[i].first);
                    }
                }

                const std::string extra = trim(gtk_entry_get_text(GTK_ENTRY(extra_entry)));
                if (!extra.empty()) {
                    const std::vector<std::string> custom = util::shlex_split(extra);
                    verbs.insert(verbs.end(), custom.begin(), custom.end());
                }

                if (verbs.empty()) {
                    dialogs::show_info("Info",
                                       "Select at least one component, or type a custom verb.",
                                       GTK_WINDOW(dialog));
                    continue;
                }
            }

            // Closed first, exactly like the Python dialog: the tool (and its
            // possible Proton build picker) is stacked on the caller instead.
            gtk_widget_destroy(dialog);
            dialog = nullptr;
            run_prefix_tool(entry, "winetricks", verbs, parent_win);
            return;
        }
        gtk_widget_destroy(dialog);
        dialog = nullptr;
    } catch (const std::exception& e) {
        if (dialog) gtk_widget_destroy(dialog);
        report_callback_error("Error opening the Winetricks dialog", e.what(), parent_win);
    }
}

bool remove_prefix_and_games(const PrefixEntry& entry, GtkWindow* parent) {
    GtkWindow* parent_win = parent ? parent : ui::window;
    try {
        const fs::path prefix_path(entry.prefix_path);
        const fs::path prefix_resolved = util::resolve_path(prefix_path);

        // Split the affected games into "installed inside the prefix" (their
        // files go away with the prefix folder) and "installed elsewhere"
        // (only the launcher entry is removed) so the user is told exactly
        // what happens BEFORE anything is deleted.
        std::vector<std::string> inside_names;
        std::vector<std::string> outside_names;
        for (const std::string& script_name : entry.games) {
            const fs::path script_path = cfg::bashlaunch_dir / (script_name + ".sh");
            const std::optional<std::string> folder =
                scripts::extract_folder_path_from_script(script_path);
            bool is_inside = false;
            if (folder) {
                is_inside = util::is_within(util::resolve_path(fs::path(*folder)), prefix_resolved);
            }
            (is_inside ? inside_names : outside_names).push_back(script_name);
        }

        std::string message = "Prefix: " + entry.prefix_code + " (" +
                              cfg::runner_display_name(entry.runner) + ")\n";
        message += "Location: " + entry.prefix_path + "\n\n";
        if (!entry.games.empty()) {
            message +=
                "The following game(s)/app(s) using this prefix will be removed from the "
                "launcher:\n";
            for (const std::string& script_name : entry.games) {
                const bool inside =
                    std::find(inside_names.begin(), inside_names.end(), script_name) !=
                    inside_names.end();
                message += "  - " + script_name;
                message += inside
                               ? "  (installed inside this prefix - its files will also be DELETED)"
                               : "  (files elsewhere are kept - only removed from the launcher)";
                message += "\n";
            }
            message += "\n";
        } else {
            message += "No game/app is currently linked to this prefix.\n\n";
        }
        message +=
            "This will permanently delete the prefix folder from disk. This cannot be "
            "undone.\n\nContinue?";

        if (!dialogs::ask_yes_no("Remove Prefix & Game(s)?", message, parent_win)) return false;

        if (!inside_names.empty()) {
            std::string confirm = "This will PERMANENTLY DELETE the installed files for:\n\n";
            for (const std::string& script_name : inside_names) {
                confirm += "  - " + script_name + "\n";
            }
            confirm +=
                "\n(they are installed inside this prefix's folder, including any save data kept "
                "there). Are you absolutely sure?";
            if (!dialogs::ask_yes_no("Confirm File Deletion", confirm, parent_win)) return false;
        }

        try {
            std::error_code ec;
            if (fs::exists(prefix_path, ec)) fs::remove_all(prefix_path, ec);
            if (ec) {
                dialogs::show_error("Error",
                                    "Failed to delete prefix folder:\n" + ec.message(), parent_win);
                return false;
            }
        } catch (const std::exception& e) {
            dialogs::show_error("Error",
                                std::string("Failed to delete prefix folder:\n") + e.what(),
                                parent_win);
            return false;
        }

        // Drop the launcher side of every game that used the prefix: its
        // script, its icon and its runner_config.json entry. Games whose files
        // live outside the prefix keep those files, exactly like a plain
        // "Remove" would do.
        json runner_cfg = cfg::load_runner_config();
        for (const std::string& script_name : entry.games) {
            std::error_code ec;
            fs::remove(cfg::bashlaunch_dir / (script_name + ".sh"), ec);
            fs::remove(cfg::icon_dir / (script_name + ".png"), ec);
            runner_cfg.erase(script_name);
        }
        cfg::save_runner_config(runner_cfg);

        json registry = cfg::load_prefix_registry();
        registry.erase(entry.runner + ":" + entry.prefix_code);
        cfg::save_prefix_registry(registry);

        ui::update_script_list();
        ui::set_status("Removed prefix '" + entry.prefix_code + "' and " +
                           std::to_string(entry.games.size()) + " game(s)/app(s).",
                       "warning");
        return true;
    } catch (const std::exception& e) {
        report_callback_error("Error removing prefix", e.what(), parent_win);
        return false;
    }
}

void open_prefix_manager_dialog() {
    // The window is deliberately NOT modal: the Python dialog is a plain
    // Toplevel that only stays above its parent, so winecfg/Winetricks can be
    // opened from it and stacked in turn.
    GtkWidget* window = nullptr;
    try {
        auto* ctx = new ManagerCtx();

        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        ctx->window = window;
        gtk_window_set_title(GTK_WINDOW(window), "Prefix Configuration Manager");
        gtk_window_set_default_size(GTK_WINDOW(window), winmode::px(1000), winmode::px(520));
        gtk_widget_set_size_request(window, winmode::px(640), winmode::px(360));
        gtk_window_set_transient_for(GTK_WINDOW(window), ui::window);
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);

        // The context lives exactly as long as the window itself.
        g_object_set_data_full(G_OBJECT(window), "prefix-manager-ctx", ctx, delete_manager_ctx);

        GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, winmode::px(8));
        gtk_container_set_border_width(GTK_CONTAINER(outer), winmode::px(15));
        gtk_container_add(GTK_CONTAINER(window), outer);

        // ---- top bar: hint + Create Prefix + Show Logs ----
        GtkWidget* top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(6));
        gtk_box_pack_start(GTK_BOX(outer), top_bar, FALSE, FALSE, 0);

        // Says it updates itself, because there is no Refresh button any more.
        GtkWidget* hint = gtk_label_new(
            "Select a prefix to configure (winecfg / explorer / uninstaller / winetricks) - "
            "the list updates itself:");
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
        gtk_box_pack_start(GTK_BOX(top_bar), hint, TRUE, TRUE, 0);

        gtk_box_pack_start(GTK_BOX(top_bar),
                           make_button("Create Prefix...", G_CALLBACK(on_create_prefix_clicked),
                                       ctx),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(top_bar),
                           make_button("Show Logs", G_CALLBACK(on_show_logs_clicked), ctx), FALSE,
                           FALSE, 0);
        // No Refresh button: the auto-refresh below notices a new or vanished
        // prefix on its own within a couple of seconds.

        // ---- prefix list ----
        ctx->store = gtk_list_store_new(NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
        ctx->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ctx->store));
        g_object_unref(ctx->store);  // from here on the view owns the model

        add_text_column(GTK_TREE_VIEW(ctx->tree), "Runner", COL_RUNNER_NAME, winmode::px(100),
                        false);
        add_text_column(GTK_TREE_VIEW(ctx->tree), "Prefix Code", COL_PREFIX_CODE,
                        winmode::px(100), false);
        add_text_column(GTK_TREE_VIEW(ctx->tree), "Game(s)", COL_GAMES, winmode::px(220), false);
        add_text_column(GTK_TREE_VIEW(ctx->tree), "Proton Version", COL_PROTON, winmode::px(170),
                        false);
        add_text_column(GTK_TREE_VIEW(ctx->tree), "Location", COL_PATH, winmode::px(320), true);

        gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(ctx->tree)),
                                    GTK_SELECTION_SINGLE);

        GtkWidget* tree_scrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree_scrolled), GTK_POLICY_AUTOMATIC,
                                       GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(tree_scrolled), ctx->tree);
        gtk_box_pack_start(GTK_BOX(outer), tree_scrolled, TRUE, TRUE, 0);

        ctx->empty_label = gtk_label_new(
            "No prefixes found yet. A prefix is created the first time\n"
            "you PLAY or run APPS SETUP for a game via Wine/Proton,\n"
            "or you can make one up front with \"Create Prefix...\".");
        gtk_label_set_xalign(GTK_LABEL(ctx->empty_label), 0.0f);
        gtk_label_set_justify(GTK_LABEL(ctx->empty_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(outer), ctx->empty_label, FALSE, FALSE, 0);

        // ---- first button row: the management tools for the selected prefix ----
        GtkWidget* row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(3));
        gtk_widget_set_halign(row1, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(row1, winmode::px(10));
        gtk_box_pack_start(GTK_BOX(outer), row1, FALSE, FALSE, 0);

        const std::pair<const char*, const char*> tools[] = {
            {"Winecfg", "winecfg"},
            {"Explorer", "explorer"},
            {"Uninstaller", "uninstaller"},
            {"Winetricks...", "winetricks"},
        };
        for (const auto& tool : tools) {
            GtkWidget* button = make_button(tool.first, G_CALLBACK(on_tool_clicked), ctx);
            g_object_set_data_full(G_OBJECT(button), "wlm-tool", g_strdup(tool.second), g_free);
            gtk_box_pack_start(GTK_BOX(row1), button, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(row1),
                           make_button("Open Folder", G_CALLBACK(on_open_folder_clicked), ctx),
                           FALSE, FALSE, 0);

        // ---- second button row: backup / restore / remove / close ----
        GtkWidget* row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(3));
        gtk_widget_set_halign(row2, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(row2, winmode::px(8));
        gtk_box_pack_start(GTK_BOX(outer), row2, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(row2),
                           make_button("Backup Apps", G_CALLBACK(on_backup_clicked), ctx), FALSE,
                           FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row2),
                           make_button("Restore Apps", G_CALLBACK(on_restore_clicked), ctx), FALSE,
                           FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row2),
                           make_button("Remove Apps", G_CALLBACK(on_remove_clicked), ctx), FALSE,
                           FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row2),
                           make_button("Close", G_CALLBACK(on_close_clicked), ctx), FALSE, FALSE,
                           0);

        gtk_widget_show_all(window);
        refresh_manager(ctx);

        // Watch for prefixes appearing on their own: a restore that finished, a
        // game launched from the main window, a folder added by hand, or the
        // Create Prefix dialog of another manager window. The timer is removed
        // by the destroy handler, which runs before the context is freed.
        g_signal_connect(window, "destroy", G_CALLBACK(on_manager_destroy), ctx);
        ctx->auto_refresh_id = g_timeout_add(2000, auto_refresh_cb, ctx);

        gtk_window_present(GTK_WINDOW(window));
    } catch (const std::exception& e) {
        if (window) gtk_widget_destroy(window);
        report_callback_error("Error opening the Prefix Configuration Manager", e.what(),
                              ui::window);
    }
}

}  // namespace prefix
