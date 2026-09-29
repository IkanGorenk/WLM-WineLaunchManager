#include "actions.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <optional>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "config.h"
#include "dialogs.h"
#include "gog_ui.h"
#include "download.h"
#include "hud.h"
#include "live_log.h"
#include "prefix_manager.h"
#include "proton.h"
#include "runner_dialog.h"
#include "scripts.h"
#include "ui.h"
#include "util.h"
#include "window_mode.h"
#include "window_mode.h"

namespace actions {

namespace {

namespace fs = std::filesystem;

std::string sanitize_name(const std::string& input) {
    std::string out;
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-') {
            out.push_back(c);
        }
    }
    size_t begin = out.find_first_not_of(" \t");
    if (begin == std::string::npos) return "";
    size_t end = out.find_last_not_of(" \t");
    return out.substr(begin, end - begin + 1);
}

std::vector<std::string> read_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

std::string trim(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// Second line of a launch script is: cd "FOLDER"
std::string folder_from_script(const fs::path& script_path, bool require_cd_prefix) {
    std::vector<std::string> lines = read_lines(script_path);
    if (lines.size() < 2) return "";
    std::string line = trim(lines[1]);
    if (require_cd_prefix && line.rfind("cd ", 0) != 0) return "";

    std::string folder = line;
    size_t pos = folder.find("cd \"");
    if (pos != std::string::npos) folder.erase(pos, 4);
    folder.erase(std::remove(folder.begin(), folder.end(), '"'), folder.end());
    return trim(folder);
}

}  // namespace

void view_logs(GtkWidget*, gpointer) {
    std::optional<std::string> name = ui::get_selected_script_name();
    if (!name) {
        dialogs::show_info("Info", "Please select a game first");
        return;
    }
    live_log::open_log_window(*name);
}

void run_script(GtkWidget*, gpointer) {
    std::optional<std::string> selected = ui::get_selected_script_name();
    if (!selected) {
        dialogs::show_info("Info", "Please select a game first");
        return;
    }
    const std::string script_name_only = *selected;
    const std::string script_name = script_name_only + ".sh";
    fs::path script_path = cfg::bashlaunch_dir / script_name;
    const std::string launch_mode = ui::launch_mode_text();

    if (!fs::exists(script_path)) {
        ui::set_status("Error: Script file not found: " + script_name, "error");
        return;
    }

    // Pick the runner (Wine Vanilla / Proton GE / Proton-CachyOS) before launching.
    std::optional<scripts::RunnerChoice> choice =
        runner_dialog::ask_runner_choice(script_name_only, "play");
    if (!choice) {
        ui::set_status("Launch cancelled.", "secondary");
        return;
    }

    std::optional<std::string> exe_path = scripts::extract_exe_path_from_script(script_path);
    std::optional<std::string> folder_path = scripts::extract_folder_path_from_script(script_path);
    if (exe_path && folder_path) {
        try {
            std::string content = scripts::build_script_content(*folder_path, *exe_path, *choice);
            std::ofstream out(script_path);
            out << content;
        } catch (const std::exception& e) {
            ui::set_status(std::string("Error updating script for runner: ") + e.what(), "error");
            return;
        }
    } else {
        ui::set_status("Error: Could not read exe/folder path from script.", "error");
        return;
    }

    // Remember the runner choice for this game so it stays consistent.
    json runner_cfg = cfg::load_runner_config();
    runner_cfg[script_name_only] = scripts::choice_to_json(*choice);
    cfg::save_runner_config(runner_cfg);

    if (!util::is_executable(script_path)) {
        try {
            util::make_executable(script_path);
        } catch (const std::exception&) {
            ui::set_status("Error: Cannot set executable permission for " + script_name, "error");
            return;
        }
    }

    const std::string script_str = script_path.string();
    std::vector<std::string> command;
    if (launch_mode == "GalliumHUD") {
        command = {"bash", "-c", hud::gallium_env_prefix() + " \"" + script_str + "\""};
    } else if (launch_mode == "VulkanHUD") {
        command = {"bash", "-c", hud::vulkan_env_prefix() + " \"" + script_str + "\""};
    } else if (launch_mode == "MangoHud-GL") {
        command = {"bash", "-c", "mangohud --dlsym \"" + script_str + "\""};
    } else if (launch_mode == "Mangohud") {
        command = {"bash", "-c", "mangohud \"" + script_str + "\""};
    } else {
        command = {"bash", script_str};
    }

    int exec_err = 0;
    live_log::GamePtr entry = live_log::launch_tracked(script_name_only, command, &exec_err);
    if (!entry) {
        if (exec_err == ENOENT) {
            dialogs::show_error("Error",
                                "Launcher command not found. Do you have the necessary tools installed?");
            ui::set_status("Error: Launcher command not found.", "error");
        } else {
            ui::set_status(std::string("Error launching script: ") + std::strerror(exec_err),
                           "error");
        }
        return;
    }

    std::string runner_label;
    if (choice->runner == "protonge") {
        runner_label = "Proton GE (" + choice->prefix_code + ")";
    } else if (choice->runner == "protoncachyos") {
        runner_label = "Proton-CachyOS (" + choice->prefix_code + ")";
    } else if (!choice->prefix_code.empty()) {
        runner_label = "Wine (" + choice->prefix_code + ")";
    } else {
        runner_label = "Wine";
    }
    ui::set_status("Launching " + script_name_only + " via " + runner_label + " in " + launch_mode +
                       " mode...",
                   "success");
}

void add_script(GtkWidget*, gpointer) {
    std::optional<std::string> chosen = dialogs::choose_open_file(
        "Select Windows Executable (.exe)", "Executable Files", {"*.exe"});
    if (!chosen) return;

    fs::path exe_path_obj(*chosen);
    std::string exe_name = exe_path_obj.stem().string();

    // Keep asking for a name until it is valid and free (or the user gives up),
    // so a name clash can be replaced deliberately instead of silently.
    std::string suggested_name = exe_name;
    std::string safe_new_name;
    while (true) {
        std::optional<std::string> new_name =
            dialogs::ask_string("Rename Script", "Enter script name (for display):", suggested_name);
        if (!new_name || new_name->empty()) return;

        safe_new_name = sanitize_name(*new_name);
        if (safe_new_name.empty()) {
            dialogs::show_error("Error", "Invalid script name.");
            return;
        }

        fs::path script_path = cfg::bashlaunch_dir / (safe_new_name + ".sh");
        if (fs::exists(script_path)) {
            std::optional<bool> answer = dialogs::ask_yes_no_cancel(
                "Name Already Exists",
                "A game named '" + safe_new_name + "' already exists.\n\n"
                "Yes = Replace it (its old launch script & runner settings will be overwritten)\n"
                "No = Enter a different name instead\n"
                "Cancel = Don't add this game");
            if (!answer) return;          // cancelled: don't add this game
            if (!*answer) {               // No: enter a different name
                suggested_name = safe_new_name;
                continue;
            }
        }
        break;
    }

    fs::path script_path = cfg::bashlaunch_dir / (safe_new_name + ".sh");
    fs::path folder_resolved = util::resolve_path(exe_path_obj.parent_path());
    fs::path exe_resolved = util::resolve_path(exe_path_obj);

    try {
        {
            std::ofstream out(script_path);
            out << "#!/bin/bash\n"
                << "cd \"" << folder_resolved.string() << "\"\n"
                << "wine \"" << exe_resolved.string() << "\"\n";
        }
        util::make_executable(script_path);

        // If the exe already lives inside a known prefix (e.g. it was just
        // installed through APPS SETUP), link the game to that exact prefix
        // instead of letting the first PLAY create an empty new one.
        std::optional<proton::PrefixInfo> owning = proton::find_owning_prefix(exe_resolved);
        std::string status_extra;
        if (owning && (owning->runner == "wine" || !owning->proton_path.empty())) {
            scripts::RunnerChoice cfg_entry;
            cfg_entry.runner = owning->runner;
            cfg_entry.prefix_code = owning->prefix_code;
            cfg_entry.prefix_path = owning->prefix_path;
            cfg_entry.proton_name = owning->proton_name;
            cfg_entry.proton_path = owning->proton_path;

            std::string content =
                scripts::build_script_content(folder_resolved.string(), exe_resolved.string(),
                                              cfg_entry);
            {
                std::ofstream out(script_path);
                out << content;
            }
            util::make_executable(script_path);

            json runner_cfg = cfg::load_runner_config();
            runner_cfg[safe_new_name] = scripts::choice_to_json(cfg_entry);
            cfg::save_runner_config(runner_cfg);
            status_extra = " (linked to existing prefix " + owning->prefix_code + ")";
        } else {
            // The name may have been replaced: drop the stale runner settings
            // of the game it overwrote, so PLAY cannot pick up an old prefix.
            json runner_cfg = cfg::load_runner_config();
            if (runner_cfg.contains(safe_new_name)) {
                runner_cfg.erase(safe_new_name);
                cfg::save_runner_config(runner_cfg);
            }
        }

        ui::update_script_list();
        ui::set_status("Added: " + safe_new_name + status_extra, "success");
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error creating script: ") + e.what(), "error");
    }
}

void remove_script(GtkWidget*, gpointer) {
    std::optional<std::string> name = ui::get_selected_script_name();
    if (!name) {
        dialogs::show_info("Info", "Please select a game first");
        return;
    }
    if (!dialogs::ask_yes_no("Confirm", "Are you sure you want to remove '" + *name + "'?")) return;

    fs::path script_path = cfg::bashlaunch_dir / (*name + ".sh");
    fs::path icon_path = cfg::icon_dir / (*name + ".png");

    try {
        std::error_code ec;
        fs::remove(script_path, ec);
        fs::remove(icon_path, ec);

        // Drop the runner mapping (the Proton prefix itself is kept, so save
        // data of the game stays intact).
        json runner_cfg = cfg::load_runner_config();
        if (runner_cfg.contains(*name)) {
            runner_cfg.erase(*name);
            cfg::save_runner_config(runner_cfg);
        }

        ui::update_script_list();
        ui::reset_game_details();
        ui::set_status("Removed: " + *name, "warning");
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error removing files: ") + e.what(), "error");
    }
}

void rename_script(GtkWidget*, gpointer) {
    std::optional<std::string> old_name = ui::get_selected_script_name();
    if (!old_name) {
        dialogs::show_info("Info", "Please select a game first");
        return;
    }

    std::optional<std::string> input =
        dialogs::ask_string("Rename Script", "Enter new script name:", *old_name);
    if (!input || *input == *old_name) return;

    std::string new_name = sanitize_name(*input);
    if (new_name.empty()) {
        dialogs::show_error("Error", "Invalid new script name.");
        return;
    }

    fs::path old_path = cfg::bashlaunch_dir / (*old_name + ".sh");
    fs::path new_path = cfg::bashlaunch_dir / (new_name + ".sh");
    fs::path old_icon = cfg::icon_dir / (*old_name + ".png");
    fs::path new_icon = cfg::icon_dir / (new_name + ".png");

    try {
        if (fs::exists(new_path)) {
            dialogs::show_error("Error", "Script '" + new_name + "' already exists.");
            return;
        }

        fs::rename(old_path, new_path);
        if (fs::exists(old_icon)) fs::rename(old_icon, new_icon);

        // Move the runner mapping so the Proton prefix stays tied to the game.
        json runner_cfg = cfg::load_runner_config();
        if (runner_cfg.contains(*old_name)) {
            json entry = json_obj(runner_cfg, *old_name);
            runner_cfg.erase(*old_name);
            runner_cfg[new_name] = entry;
            cfg::save_runner_config(runner_cfg);
        }

        ui::update_script_list();
        ui::select_script(new_name);
        ui::set_status("Renamed to: " + new_name, "success");
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error renaming script: ") + e.what(), "error");
    }
}

void change_icon(GtkWidget*, gpointer) {
    std::optional<std::string> name = ui::get_selected_script_name();
    if (!name) {
        dialogs::show_info("Info", "Please select a game first");
        return;
    }

    std::optional<std::string> icon_path = dialogs::choose_open_file(
        "Select Icon", "Image Files", {"*.png", "*.jpg", "*.jpeg", "*.ico", "*.bmp"});
    if (!icon_path) return;

    GError* error = nullptr;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(icon_path->c_str(), &error);
    if (!pixbuf) {
        std::string message = error ? error->message : "unknown error";
        if (error) g_error_free(error);
        dialogs::show_error("Error", "Failed to process image:\n" + message);
        ui::set_status("Error processing image: " + message, "error");
        return;
    }

    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    int new_size = std::min(width, height);
    int left = (width - new_size) / 2;
    int top = (height - new_size) / 2;

    GdkPixbuf* cropped = gdk_pixbuf_new_subpixbuf(pixbuf, left, top, new_size, new_size);
    GdkPixbuf* resized = gdk_pixbuf_scale_simple(cropped, cfg::ICON_SIZE, cfg::ICON_SIZE,
                                                 GDK_INTERP_BILINEAR);

    fs::path out_path = cfg::icon_dir / (*name + ".png");
    GError* save_error = nullptr;
    bool saved = gdk_pixbuf_save(resized, out_path.string().c_str(), "png", &save_error, NULL, NULL);

    g_object_unref(resized);
    g_object_unref(cropped);
    g_object_unref(pixbuf);

    if (!saved) {
        std::string message = save_error ? save_error->message : "unknown error";
        if (save_error) g_error_free(save_error);
        dialogs::show_error("Error", "Failed to process image:\n" + message);
        ui::set_status("Error processing image: " + message, "error");
        return;
    }

    ui::load_icon(*name);
    ui::set_status("Icon updated for " + *name, "success");
}

void open_file_manager(GtkWidget*, gpointer) {
    std::optional<std::string> name = ui::get_selected_script_name();
    if (!name) {
        dialogs::show_info("Info", "Please select a game first");
        return;
    }

    fs::path script_path = cfg::bashlaunch_dir / (*name + ".sh");
    if (!fs::exists(script_path)) {
        ui::set_status("Error: Script file not found: " + *name, "error");
        return;
    }

    try {
        std::string folder_path = folder_from_script(script_path, true);
        if (!folder_path.empty() && fs::is_directory(folder_path)) {
            int err = 0;
            if (util::open_path(folder_path, &err)) {
                ui::set_status("Opening folder for " + *name + "...", "secondary");
            } else {
                dialogs::show_error("Error",
                                    "Folder path not found or invalid in script for " + *name + ".");
                ui::set_status("Error: Invalid folder path in script.", "error");
            }
        } else {
            dialogs::show_error("Error",
                                "Folder path not found or invalid in script for " + *name + ".");
            ui::set_status("Error: Invalid folder path in script.", "error");
        }
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error opening file manager: ") + e.what(), "error");
    }
}

void open_wine_prefix_folder(GtkWidget*, gpointer) {
    const char* env_prefix = std::getenv("WINEPREFIX");
    fs::path wine_prefix = env_prefix && *env_prefix ? fs::path(env_prefix)
                                                     : fs::path(cfg::home_dir()) / ".wine";

    try {
        if (fs::is_directory(wine_prefix)) {
            int err = 0;
            if (util::open_path(wine_prefix.string(), &err)) {
                ui::set_status("Opening Wine Prefix Folder...", "secondary");
            } else {
                dialogs::show_error("Error", "Wine Prefix folder not found: " + wine_prefix.string());
                ui::set_status("Error: Wine Prefix folder not found.", "error");
            }
        } else {
            dialogs::show_error("Error", "Wine Prefix folder not found: " + wine_prefix.string());
            ui::set_status("Error: Wine Prefix folder not found.", "error");
        }
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error opening Wine Prefix: ") + e.what(), "error");
    }
}

void open_winecfg(GtkWidget*, gpointer) {
    int err = 0;
    if (util::spawn_detached({"winecfg"}, {}, &err)) {
        ui::set_status("Opening Wine Configuration...", "secondary");
    } else if (err == ENOENT) {
        dialogs::show_error("Error", "The 'wine' command was not found.");
        ui::set_status("Error: Wine command not found.", "error");
    } else {
        ui::set_status(std::string("Error opening winecfg: ") + std::strerror(err), "error");
    }
}

void run_wine_uninstaller(GtkWidget*, gpointer) {
    int err = 0;
    util::spawn_detached({"wine", "uninstaller"}, {}, &err);
}

void run_wine_explorer(GtkWidget*, gpointer) {
    int err = 0;
    util::spawn_detached({"wine", "explorer"}, {}, &err);
}

void run_exe_setup(GtkWidget*, gpointer) {
    std::optional<std::string> chosen = dialogs::choose_open_file(
        "Select Setup Executable (.exe)", "Executable Files", {"*.exe"});
    if (!chosen) return;

    // Pick the runner before running the installer; with Proton GE a new prefix
    // is created sequentially inside ~/wlm/protonprefixes/GAMEXXX.
    std::optional<scripts::RunnerChoice> choice = runner_dialog::ask_runner_choice("", "setup");
    if (!choice) {
        ui::set_status("Setup cancelled.", "secondary");
        return;
    }

    try {
        scripts::LaunchOptions parsed = scripts::parse_launch_options(choice->launch_options);
        util::EnvMap extra_env;
        for (const auto& kv : parsed.env) extra_env[kv.first] = kv.second;

        std::vector<std::string> command;
        if (choice->runner != "wine" && !choice->runner.empty()) {
            extra_env["STEAM_COMPAT_DATA_PATH"] = choice->prefix_path;
            extra_env["STEAM_COMPAT_CLIENT_INSTALL_PATH"] =
                proton::find_steam_install_path().string();
            command = {choice->proton_path, "run", *chosen};
            command.insert(command.end(), parsed.args.begin(), parsed.args.end());

            int err = 0;
            if (!util::spawn_detached(command, extra_env, &err)) throw err;

            std::string runner_label =
                choice->runner == "protonge" ? "Proton GE" : "Proton-CachyOS";
            ui::set_status("Running setup for " + fs::path(*chosen).filename().string() + " via " +
                               runner_label + " (prefix: " + choice->prefix_code + ")...",
                           "secondary");
        } else {
            if (!choice->prefix_path.empty()) extra_env["WINEPREFIX"] = choice->prefix_path;
            command = {"wine", *chosen};
            command.insert(command.end(), parsed.args.begin(), parsed.args.end());

            int err = 0;
            if (!util::spawn_detached(command, extra_env, &err)) throw err;

            if (!choice->prefix_code.empty()) {
                ui::set_status("Running setup for " + fs::path(*chosen).filename().string() +
                                   " via Wine (prefix: " + choice->prefix_code + ")...",
                               "secondary");
            } else {
                ui::set_status("Running setup for " + fs::path(*chosen).filename().string() +
                                   " via Wine...",
                               "secondary");
            }
        }
    } catch (int err) {
        if (err == ENOENT) {
            dialogs::show_error("Error", "Runner command was not found.");
            ui::set_status("Error: Runner command not found.", "error");
        } else {
            ui::set_status(std::string("Error running setup: ") + std::strerror(err), "error");
        }
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error running setup: ") + e.what(), "error");
    }
}

// The two Proton flavours behave the same for extract / download / open /
// remove, so the Settings menu has ONE entry for each of those and this popup
// asks which runner it is for. Each row also shows how many builds are
// installed, so it is obvious which one is actually in use.
std::optional<std::string> choose_proton_runner(const std::string& title,
                                                const std::string& message) {
    struct Row {
        const char* key;
        const char* label;
    };
    static const Row rows[] = {
        {"protonge", "Proton GE"},
        {"protoncachyos", "Proton-CachyOS"},
    };

    const proton::BuildList ge = proton::find_protonge_installations();
    const proton::BuildList cachyos = proton::find_protoncachyos_installations();
    (void)ge;
    (void)cachyos;

    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        title.c_str(), ui::window,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "_OK",
        GTK_RESPONSE_OK, "_Cancel", GTK_RESPONSE_CANCEL, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), winmode::px(15));
    gtk_box_set_spacing(GTK_BOX(content), winmode::px(8));

    GtkWidget* label_widget = gtk_label_new(message.c_str());
    gtk_label_set_xalign(GTK_LABEL(label_widget), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(label_widget), TRUE);
    gtk_box_pack_start(GTK_BOX(content), label_widget, FALSE, FALSE, 0);

    GtkWidget* combo = gtk_combo_box_text_new();
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        const proton::BuildList& installed = i == 0 ? ge : cachyos;
        std::string text = rows[i].label;
        if (installed.empty()) {
            text += "  (none installed)";
        } else if (installed.size() == 1) {
            text += "  (" + installed.front().first + ")";
        } else {
            text += "  (" + std::to_string(installed.size()) + " builds installed)";
        }
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), text.c_str());
    }
    // Default to whichever one actually has something installed.
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), (ge.empty() && !cachyos.empty()) ? 1 : 0);
    gtk_box_pack_start(GTK_BOX(content), combo, FALSE, FALSE, 0);

    std::optional<std::string> chosen;
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const int index = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
        if (index >= 0 && index < 2) chosen = rows[index].key;
    }
    gtk_widget_destroy(dialog);
    return chosen;
}

// ---- the two runners, in one place -----------------------------------------
//
// Downloads, extraction, the folder and removal all behave identically for both
// flavours, so they are described once here instead of being spelled out twice
// per action.

void download_proton_ge(GtkWidget*, gpointer) {
    try {
        dload::open_protonge_download_dialog();
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error opening the download window: ") + e.what(), "error");
        dialogs::show_error("Error", std::string("Could not open the download window:\n") + e.what());
    }
}

void download_proton_cachyos(GtkWidget*, gpointer) {
    try {
        dload::open_protoncachyos_download_dialog();
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error opening the download window: ") + e.what(), "error");
        dialogs::show_error("Error", std::string("Could not open the download window:\n") + e.what());
    }
}

// The two runner families this launcher installs and manages itself.
struct ProtonRunner {
    const char* key;
    const char* title;
    void (*open_download)(GtkWidget*, gpointer);
    void (*open_folder)();
    void (*extract_archive)();
};

const ProtonRunner PROTON_RUNNERS[] = {
    {"protonge", "Proton GE", &download_proton_ge, &proton::open_protonge_folder,
     &proton::extract_protonge_archive},
    {"protoncachyos", "Proton-CachyOS", &download_proton_cachyos,
     &proton::open_protoncachyos_folder, &proton::extract_protoncachyos_archive},
};

constexpr size_t PROTON_RUNNER_COUNT = sizeof(PROTON_RUNNERS) / sizeof(PROTON_RUNNERS[0]);

// Proton builds that Steam installed. They are listed here and can be used like
// any other runner, but they are not ours to download into, unpack an archive
// into, or delete - Steam owns those folders. Only "Open Folder" applies to
// them, and the two destructive actions refuse with an explanation.
constexpr const char* kSteamProtonKey = "steamproton";
constexpr const char* kSteamProtonTitle = "Proton (Steam)";

bool is_steam_proton(const std::string& key) { return key == kSteamProtonKey; }

const ProtonRunner* runner_by_key(const std::string& key) {
    for (size_t i = 0; i < PROTON_RUNNER_COUNT; ++i) {
        if (key == PROTON_RUNNERS[i].key) return &PROTON_RUNNERS[i];
    }
    return nullptr;
}

fs::path runner_dir_of(const std::string& key) {
    return key == "protonge" ? cfg::protonge_dir : cfg::protoncachyos_dir;
}

proton::BuildList builds_of(const std::string& key) {
    return key == "protonge" ? proton::find_protonge_installations()
                             : proton::find_protoncachyos_installations();
}

void extract_archive_into_runner(const std::string& runner_key, GtkWindow* parent) {
    const ProtonRunner* runner = runner_by_key(runner_key);
    if (runner == nullptr || runner->extract_archive == nullptr) return;
    runner->extract_archive();
    (void)parent;  // the extraction reports its own progress window
}

// One row of the installed-build list: a runner, and one build inside it.
struct InstalledBuild {
    std::string runner_key;
    std::string runner_label;
    std::string name;
    fs::path path;
};

std::vector<InstalledBuild> list_installed_proton_builds() {
    std::vector<InstalledBuild> out;
    for (const ProtonRunner& runner : PROTON_RUNNERS) {
        for (const auto& build : builds_of(runner.key)) {
            out.push_back({runner.key, runner.title, build.first, build.second});
        }
    }
    // Steam's own Proton builds are listed too, so a game can be pointed at
    // whichever Proton the user actually has.
    for (const auto& build : proton::find_steam_proton_installations()) {
        out.push_back({kSteamProtonKey, kSteamProtonTitle, build.first, build.second});
    }
    return out;
}

// ---- the download window: one button per runner ----------------------------

struct DownloadButtonCtx {
    void (*open_download)(GtkWidget*, gpointer);
};

// Closes the window a widget belongs to. This cannot be gtk_widget_destroy
// connected straight to "clicked": that signal hands the handler the BUTTON, so
// gtk_widget_destroy would destroy the button and leave the window sitting
// there with a gap where the button was. Every "Close" button in the launcher
// uses this (see ui::close_window_from_button) rather than repeating the
// connect-and-forget mistake.
void close_window_from_button(GtkWidget* widget, gpointer) {
    GtkWidget* toplevel = gtk_widget_get_toplevel(widget);
    if (toplevel != nullptr) gtk_widget_destroy(toplevel);
}

void on_download_button_clicked(GtkButton* button, gpointer data) {
    auto* ctx = static_cast<DownloadButtonCtx*>(data);
    if (ctx == nullptr) return;
    if (ctx->open_download != nullptr) ctx->open_download(GTK_WIDGET(button), nullptr);
}

void free_download_button_ctx(gpointer data, GClosure*) {
    delete static_cast<DownloadButtonCtx*>(data);
}

// A window with a "Download from GitHub" button per runner. It is the fallback
// behind the Runner Options window's "Download from Online" button, so the
// flavour is picked with one click instead of a submenu that hides one of them.
void open_proton_download_window(GtkWidget*, gpointer) {
    try {
        GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window), "Download Proton from GitHub");
        gtk_window_set_default_size(GTK_WINDOW(window), winmode::px(560), winmode::px(210));
        gtk_widget_set_size_request(window, winmode::px(420), winmode::px(180));
        gtk_window_set_transient_for(GTK_WINDOW(window), ui::window);
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);

        GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, winmode::px(10));
        gtk_container_set_border_width(GTK_CONTAINER(outer), winmode::px(15));
        gtk_container_add(GTK_CONTAINER(window), outer);

        GtkWidget* intro = gtk_label_new(
            "Pick a Proton to download a new build for.\n"
            "Nothing is downloaded until you choose a release.");
        gtk_label_set_xalign(GTK_LABEL(intro), 0.0f);
        gtk_box_pack_start(GTK_BOX(outer), intro, FALSE, FALSE, 0);

        for (const ProtonRunner& runner : PROTON_RUNNERS) {
            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(10));
            gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);

            GtkWidget* name = gtk_label_new(runner.title);
            gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
            gtk_widget_set_valign(name, GTK_ALIGN_CENTER);
            gtk_box_pack_start(GTK_BOX(row), name, TRUE, TRUE, 0);

            auto* ctx = new DownloadButtonCtx{runner.open_download};
            GtkWidget* button = gtk_button_new_with_label("Download from GitHub");
            gtk_widget_set_size_request(button, winmode::px(190), -1);
            g_signal_connect_data(button, "clicked", G_CALLBACK(on_download_button_clicked), ctx,
                                  free_download_button_ctx, G_CONNECT_DEFAULT);
            gtk_box_pack_start(GTK_BOX(row), button, FALSE, FALSE, 0);
        }

        GtkWidget* close_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(6));
        gtk_widget_set_halign(close_row, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(close_row, winmode::px(6));
        gtk_box_pack_start(GTK_BOX(outer), close_row, FALSE, FALSE, 0);
        GtkWidget* close_btn = gtk_button_new_with_label("Close");
        gtk_widget_set_size_request(close_btn, winmode::px(110), -1);
        g_signal_connect(close_btn, "clicked", G_CALLBACK(close_window_from_button), nullptr);
        gtk_box_pack_start(GTK_BOX(close_row), close_btn, FALSE, FALSE, 0);

        gtk_widget_show_all(window);
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error opening the download window: ") + e.what(), "error");
        dialogs::show_error("Error", std::string("Could not open the download window:\n") + e.what());
    }
}

// ---- removing a runner ------------------------------------------------------
//
// Deleting a runner removes its extracted builds. Prefixes that were using one
// of them are kept - they just have no Proton version recorded any more, so the
// next winecfg / Winetricks / PLAY asks which build to use again.

void remove_runner_by_key(const std::string& runner_key, GtkWindow* parent) {
    const std::string label = cfg::runner_display_name(runner_key);
    const proton::BuildList installed = builds_of(runner_key);
    if (installed.empty()) {
        dialogs::show_info("Nothing to remove", "No " + label + " build is installed.", parent);
        return;
    }

    const fs::path runner_dir = runner_dir_of(runner_key);

    // Prefixes that would be left without a build.
    std::vector<prefix::PrefixEntry> affected;
    for (const prefix::PrefixEntry& entry : prefix::list_all_known_prefixes()) {
        if (entry.runner == runner_key) affected.push_back(entry);
    }

    std::string message = "This will delete " + label + " and all of its builds:\n  " +
                          runner_dir.string() + "\n\n";
    for (const auto& build : installed) message += "  - " + build.first + "\n";
    if (affected.empty()) {
        message += "\nNo prefix uses it.";
    } else {
        message += "\nThese prefixes use it and will have to pick a new build next time:\n";
        for (const prefix::PrefixEntry& entry : affected) {
            message += "  - " + entry.prefix_code + "\n";
        }
    }
    message += "\n\nDelete it?";
    if (!dialogs::ask_yes_no("Remove " + label + "?", message, parent)) return;

    std::error_code ec;
    fs::remove_all(runner_dir, ec);
    if (ec) {
        dialogs::show_error("Error",
                            "Could not delete " + runner_dir.string() + ":\n" + ec.message(),
                            parent);
        ui::set_status("Failed to remove " + label, "error");
        return;
    }

    // Forget the build those prefixes pointed at, so nothing tries to run a
    // path that is gone (run_prefix_tool re-asks when this is empty).
    for (const prefix::PrefixEntry& entry : affected) {
        proton::record_prefix_usage(entry.runner, entry.prefix_code, entry.prefix_path, "", "");
    }

    ui::set_status("Removed " + label +
                       (affected.empty() ? "" : " (" + std::to_string(affected.size()) +
                                                   " prefix(es) now need a new build)"),
                   "warning");
    dialogs::show_info("Runner Removed",
                       label + " was removed.\n\n" +
                           (affected.empty()
                                ? std::string("No prefix was using it.")
                                : std::to_string(affected.size()) +
                                      " prefix(es) will ask for a build again the next time you "
                                      "use them."),
                       parent);
}

void remove_runner(GtkWidget*, gpointer) {
    try {
        const std::optional<std::string> runner =
            choose_proton_runner("Remove Runner", "Choose which runner to remove.");
        if (!runner) return;
        remove_runner_by_key(*runner, ui::window);
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error removing runner: ") + e.what(), "error");
        dialogs::show_error("Error", std::string("Could not remove the runner:\n") + e.what(),
                            ui::window);
    }
}

void open_runner_folder(GtkWidget*, gpointer) {
    try {
        const std::optional<std::string> runner =
            choose_proton_runner("Open Runner Folder", "Choose which runner's folder to open.");
        if (!runner) return;
        const ProtonRunner* entry = runner_by_key(*runner);
        if (entry != nullptr && entry->open_folder != nullptr) entry->open_folder();
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error opening folder: ") + e.what(), "error");
        dialogs::show_error("Error", std::string("Could not open the folder:\n") + e.what());
    }
}

void extract_proton_archive(GtkWidget*, gpointer) {
    try {
        const std::optional<std::string> runner = choose_proton_runner(
            "Extract Proton Archive",
            "Choose which Proton to extract the archive into.\n"
            "It is unpacked into that runner's folder; the runner does not have to be installed yet.");
        if (!runner) return;
        extract_archive_into_runner(*runner, ui::window);
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error extracting archive: ") + e.what(), "error");
        dialogs::show_error("Error", std::string("Could not extract the archive:\n") + e.what());
    }
}

// ---- the Runner Options window ---------------------------------------------
//
// The Proton builds that are actually installed, with the three actions that
// work on a runner: download a new one, unpack an archive into it, or remove
// it. This is what the "Runner Options" submenu used to do, in one place.

enum { COL_RO_KEY = 0, COL_RO_RUNNER, COL_RO_BUILD, COL_RO_PATH, COL_RO_COUNT };

struct RunnerOptionsCtx {
    GtkWidget* window = nullptr;
    GtkListStore* store = nullptr;
    GtkWidget* tree = nullptr;
    GtkWidget* empty_label = nullptr;
    std::vector<InstalledBuild> builds;

    // Several of the actions run a MODAL dialog (the runner chooser, the delete
    // confirmation), which spins its own nested main loop. The user can close
    // this window while that loop is running, and the window's destroy notify
    // would then free this struct while the button handler is still using it.
    // So the window holds one reference and each handler takes another for as
    // long as it runs.
    int refs = 1;
    // Set by the destroy notify, so a handler that comes back from a nested
    // main loop knows the widgets it was holding are gone.
    bool window_destroyed = false;
};

void retain_runner_options(RunnerOptionsCtx* ctx) {
    ++ctx->refs;
}

void release_runner_options(RunnerOptionsCtx* ctx) {
    if (--ctx->refs == 0) delete ctx;
}

// RAII for a button handler: keeps the ctx alive across a nested main loop.
struct RunnerOptionsRef {
    explicit RunnerOptionsRef(RunnerOptionsCtx* c) : ctx(c) { retain_runner_options(c); }
    ~RunnerOptionsRef() { release_runner_options(ctx); }
    RunnerOptionsCtx* ctx;
};

void delete_runner_options_ctx(gpointer data) {
    auto* ctx = static_cast<RunnerOptionsCtx*>(data);
    if (ctx != nullptr) ctx->window_destroyed = true;
    release_runner_options(ctx);
}

// The row the user picked, or nullptr when nothing is selected. The rows are
// looked up by value rather than by index so a reload cannot hand back a pointer
// into a vector that has since been replaced.
const InstalledBuild* selected_build(RunnerOptionsCtx* ctx) {
    if (ctx == nullptr || ctx->window_destroyed || ctx->tree == nullptr) return nullptr;
    GList* rows =
        gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(ctx->tree)),
                                             nullptr);
    if (rows == nullptr) return nullptr;

    const InstalledBuild* found = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(ctx->store), &iter,
                                static_cast<GtkTreePath*>(rows->data))) {
        gchar* runner_key = nullptr;
        gchar* build = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), &iter, COL_RO_KEY, &runner_key, COL_RO_BUILD,
                           &build, -1);
        for (const InstalledBuild& b : ctx->builds) {
            if (runner_key != nullptr && build != nullptr && b.runner_key == runner_key &&
                b.name == build) {
                found = &b;
                break;
            }
        }
        g_free(runner_key);
        g_free(build);
    }
    g_list_free_full(rows, reinterpret_cast<GDestroyNotify>(gtk_tree_path_free));
    return found;
}

void reload_runner_options(RunnerOptionsCtx* ctx) {
    if (ctx == nullptr || ctx->window_destroyed || ctx->store == nullptr) return;
    gtk_list_store_clear(ctx->store);
    ctx->builds = list_installed_proton_builds();
    for (const InstalledBuild& b : ctx->builds) {
        GtkTreeIter row;
        gtk_list_store_append(ctx->store, &row);
        gtk_list_store_set(ctx->store, &row, COL_RO_KEY, b.runner_key.c_str(), COL_RO_RUNNER,
                           b.runner_label.c_str(), COL_RO_BUILD, b.name.c_str(), COL_RO_PATH,
                           b.path.string().c_str(), -1);
    }
    gtk_widget_set_visible(ctx->empty_label, ctx->builds.empty());
}

// Picks the runner of the selected build, or asks when the list is empty - the
// runner folder exists whether or not a build has been put in it yet.
std::optional<std::string> runner_of_selection(RunnerOptionsCtx* ctx) {
    const InstalledBuild* build = selected_build(ctx);
    if (build != nullptr) return build->runner_key;
    return choose_proton_runner(
        "Choose a Proton",
        "Nothing is selected.\nChoose which Proton to use - the runner does not have to be "
        "installed yet.");
}

void on_runner_options_download(GtkButton*, gpointer data) {
    RunnerOptionsRef ref(static_cast<RunnerOptionsCtx*>(data));
    RunnerOptionsCtx* ctx = ref.ctx;
    try {
        open_proton_download_window(nullptr, nullptr);
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error opening the download window: ") + e.what(), "error");
        dialogs::show_error("Error", std::string("Could not open the download window:\n") + e.what(),
                            GTK_WINDOW(ctx->window));
    }
}

void on_runner_options_extract(GtkButton*, gpointer data) {
    RunnerOptionsRef ref(static_cast<RunnerOptionsCtx*>(data));
    RunnerOptionsCtx* ctx = ref.ctx;
    try {
        const std::optional<std::string> runner_key = runner_of_selection(ctx);
        if (!runner_key) return;
        if (is_steam_proton(*runner_key)) {
            dialogs::show_info(
                "Steam's own Proton",
                "This Proton was installed by Steam, so its folder belongs to Steam.\n\n"
                "It can be used for a game (pick it as the runner), but the launcher does not "
                "unpack archives into it.\n\n"
                "To change its version, do it in Steam's own settings.",
                GTK_WINDOW(ctx->window));
            return;
        }
        // This opens a file chooser, i.e. a nested main loop the user can close
        // the window from.
        extract_archive_into_runner(*runner_key, GTK_WINDOW(ctx->window));
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error extracting archive: ") + e.what(), "error");
        dialogs::show_error("Error", std::string("Could not extract the archive:\n") + e.what(),
                            GTK_WINDOW(ctx->window));
    }
}

void on_runner_options_open_folder(GtkButton*, gpointer data) {
    RunnerOptionsRef ref(static_cast<RunnerOptionsCtx*>(data));
    RunnerOptionsCtx* ctx = ref.ctx;
    try {
        const std::optional<std::string> runner_key = runner_of_selection(ctx);
        if (!runner_key) return;
        const ProtonRunner* runner = runner_by_key(*runner_key);
        if (runner != nullptr && runner->open_folder != nullptr) runner->open_folder();
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error opening folder: ") + e.what(), "error");
        dialogs::show_error("Error", std::string("Could not open the folder:\n") + e.what(),
                            GTK_WINDOW(ctx->window));
    }
}

void on_runner_options_remove(GtkButton*, gpointer data) {
    RunnerOptionsRef ref(static_cast<RunnerOptionsCtx*>(data));
    RunnerOptionsCtx* ctx = ref.ctx;
    try {
        const std::optional<std::string> runner_key = runner_of_selection(ctx);
        if (!runner_key) return;
        if (is_steam_proton(*runner_key)) {
            dialogs::show_info(
                "Steam's own Proton",
                "This Proton belongs to Steam, so the launcher will not delete it.\n\n"
                "Uninstall or change it through Steam itself, then press Refresh.",
                GTK_WINDOW(ctx->window));
            return;
        }
        // The confirmation is modal, so the window can be closed underneath it.
        remove_runner_by_key(*runner_key, GTK_WINDOW(ctx->window));
        if (ctx->window_destroyed) return;  // nothing left to refresh
        // The delete may have removed every build, so the list is stale.
        reload_runner_options(ctx);
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error removing runner: ") + e.what(), "error");
        dialogs::show_error("Error", std::string("Could not remove the runner:\n") + e.what(),
                            GTK_WINDOW(ctx->window));
    }
}

void open_runner_options_window(GtkWidget*, gpointer) {
    GtkWidget* window = nullptr;
    RunnerOptionsCtx* ctx = nullptr;
    try {
        ctx = new RunnerOptionsCtx();

        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        ctx->window = window;
        gtk_window_set_title(GTK_WINDOW(window), "Runner Options");
        gtk_window_set_default_size(GTK_WINDOW(window), winmode::px(820), winmode::px(420));
        gtk_widget_set_size_request(window, winmode::px(560), winmode::px(340));
        gtk_window_set_transient_for(GTK_WINDOW(window), ui::window);
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
        g_object_set_data_full(G_OBJECT(window), "runner-options-ctx", ctx,
                               delete_runner_options_ctx);

        GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, winmode::px(8));
        gtk_container_set_border_width(GTK_CONTAINER(outer), winmode::px(15));
        gtk_box_set_spacing(GTK_BOX(outer), winmode::px(8));
        gtk_container_add(GTK_CONTAINER(window), outer);

        GtkWidget* intro = gtk_label_new(
            "The Proton builds that are installed. Select one, then download a new build for it, "
            "unpack an\narchive into it, open its folder, or remove it.");
        gtk_label_set_xalign(GTK_LABEL(intro), 0.0f);
        gtk_box_pack_start(GTK_BOX(outer), intro, FALSE, FALSE, 0);

        // ---- the installed builds ----
        ctx->store = gtk_list_store_new(COL_RO_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                        G_TYPE_STRING);
        ctx->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ctx->store));
        g_object_unref(ctx->store);  // the view owns the model now

        struct Column {
            const char* title;
            int model_col;
            int min_width;
            bool expand;
        };
        static const Column columns[] = {
            {"Runner", COL_RO_RUNNER, 110, false},
            {"Build", COL_RO_BUILD, 200, true},
            {"Location", COL_RO_PATH, 260, false},
        };
        for (const Column& column : columns) {
            GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
            GtkTreeViewColumn* view_column = gtk_tree_view_column_new_with_attributes(
                column.title, renderer, "text", column.model_col, NULL);
            gtk_tree_view_column_set_resizable(view_column, TRUE);
            gtk_tree_view_column_set_min_width(view_column, winmode::px(column.min_width));
            gtk_tree_view_column_set_expand(view_column, column.expand);
            gtk_tree_view_append_column(GTK_TREE_VIEW(ctx->tree), view_column);
        }
        gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(ctx->tree)),
                                    GTK_SELECTION_SINGLE);

        GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                       GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scroll), ctx->tree);
        gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

        ctx->empty_label = gtk_label_new(
            "No Proton build is installed yet.\n"
            "Use \"Download from Online\" to fetch one, or \"Extract from Archive\" to unpack one "
            "you\nalready downloaded.");
        gtk_label_set_xalign(GTK_LABEL(ctx->empty_label), 0.0f);
        gtk_box_pack_start(GTK_BOX(outer), ctx->empty_label, FALSE, FALSE, 0);

        // ---- the actions ----
        GtkWidget* btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, winmode::px(6));
        gtk_widget_set_halign(btn_row, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(btn_row, winmode::px(8));
        gtk_box_pack_start(GTK_BOX(outer), btn_row, FALSE, FALSE, 0);

        struct Action {
            const char* label;
            GCallback callback;
        };
        static const Action actions[] = {
            {"Download from Online...", G_CALLBACK(on_runner_options_download)},
            {"Extract from Archive...", G_CALLBACK(on_runner_options_extract)},
            {"Open Folder", G_CALLBACK(on_runner_options_open_folder)},
            {"Remove Runner...", G_CALLBACK(on_runner_options_remove)},
        };
        for (const Action& action : actions) {
            GtkWidget* button = gtk_button_new_with_label(action.label);
            gtk_widget_set_size_request(button, winmode::px(165), -1);
            g_signal_connect(button, "clicked", action.callback, ctx);
            gtk_box_pack_start(GTK_BOX(btn_row), button, FALSE, FALSE, 0);
        }

        GtkWidget* close_btn = gtk_button_new_with_label("Close");
        gtk_widget_set_size_request(close_btn, winmode::px(100), -1);
        g_signal_connect(close_btn, "clicked", G_CALLBACK(close_window_from_button), nullptr);
        gtk_box_pack_start(GTK_BOX(btn_row), close_btn, FALSE, FALSE, 0);

        reload_runner_options(ctx);
        gtk_widget_show_all(window);
        // set_visible() in reload_runner_options() ran before the window was
        // shown, so put the empty-list hint back the way it belongs.
        gtk_widget_set_visible(ctx->empty_label, ctx->builds.empty());
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error opening Runner Options: ") + e.what(), "error");
        dialogs::show_error("Error", std::string("Could not open Runner Options:\n") + e.what(),
                            ui::window);

        // The ctx belongs to the window from the moment g_object_set_data_full
        // ran, and that destroy notify drops the window's reference. So when the
        // window exists, let destroying it do the releasing; only when it does
        // not exist (the throw came before the window was created) is the ctx
        // ours to release. Doing both would free it twice.
        if (window != nullptr && GTK_IS_WIDGET(window)) {
            gtk_widget_destroy(window);
        } else {
            release_runner_options(ctx);
        }
    }
}

void open_gog_library(GtkWidget*, gpointer) {
    try {
        gogui::open_window();
    } catch (const std::exception& e) {
        ui::set_status(std::string("Error opening the GOG library: ") + e.what(), "error");
        dialogs::show_error("Error",
                            std::string("Could not open the GOG library:\n") + e.what());
    }
}

void open_prefix_manager(GtkWidget*, gpointer) {
    prefix::open_prefix_manager_dialog();
}

void config_hud(GtkWidget*, gpointer) {
    hud::open_config_dialog();
}

void refresh_list(GtkWidget*, gpointer) {
    ui::update_script_list();
}

void sort_by_selected(GtkComboBox*, gpointer) {
    gchar* text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(ui::sort_combo));
    std::string value = text ? text : "";
    g_free(text);

    if (value == "A-Z") {
        ui::update_script_list("ascending");
    } else if (value == "Z-A") {
        ui::update_script_list("descending");
    }
}

}  // namespace actions
