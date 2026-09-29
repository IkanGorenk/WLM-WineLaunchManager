#pragma once

// "Prefix Configuration Manager": list every known Wine / Proton GE /
// Proton-CachyOS prefix together with the games using it, and run
// winecfg / explorer / uninstaller / winetricks / backup / restore / remove
// for the selected one.
// C++ port of list_all_known_prefixes()/open_prefix_manager_dialog()/
// run_prefix_tool()/open_winetricks_dialog()/remove_prefix_and_games_dialog()
// in launcher.py.

#include <gtk/gtk.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tasklog {
class Task;
}

namespace prefix {

struct PrefixEntry {
    std::string runner;        // "wine" | "protonge" | "protoncachyos"
    std::string prefix_code;
    std::string prefix_path;
    std::string proton_name;   // may be empty (picked on first use)
    std::string proton_path;
    std::vector<std::string> games;
};

// Everything from prefix_registry.json plus a scan of the default/custom
// prefix folders, sorted by (runner, prefix_code), with the games that use
// each prefix attached.
std::vector<PrefixEntry> list_all_known_prefixes();

// The manager dialog itself (Settings -> "Prefix Configuration Manager...").
void open_prefix_manager_dialog();

// Runs one management tool against a single prefix. tool is one of
// "winecfg" | "explorer" | "uninstaller" | "winetricks"; extra_args are extra
// winetricks verbs. parent is the window currently calling (used to stack
// dialogs on top of it and to ask for the Proton build when the prefix has
// none recorded yet).
void run_prefix_tool(const PrefixEntry& entry, const std::string& tool,
                     const std::vector<std::string>& extra_args = {},
                     GtkWindow* parent = nullptr);

// Checkbox picker for the common winetricks verbs (+ free-form verbs).
void open_winetricks_dialog(const PrefixEntry& entry, GtkWindow* parent = nullptr);

// Makes `prefix_path` a prefix the runner can actually use, and reports into
// `task` while it works.
//
// For a Proton runner this copies the ready-made prefix that ships with the
// build (the same thing Proton's own `proton` script does), because current
// builds contain no wineboot binary - their only wineboot is wineboot.exe, which
// needs a working prefix before it can run. Older builds without that template
// fall back to running wineboot.
//
// Returns "" on success, or the reason it could not be initialised. It never
// throws and never fails the caller's work: a prefix that could not be
// initialised is still a valid folder, and the runner fills it in on next use.
std::string initialize_prefix(const std::string& runner_key,
                              const std::filesystem::path& prefix_path,
                              const std::string& proton_path,
                              const std::shared_ptr<tasklog::Task>& task);

// Deletes the prefix folder, its registry entry and every game using it.
// Returns true when the removal actually happened.
bool remove_prefix_and_games(const PrefixEntry& entry, GtkWindow* parent = nullptr);

}  // namespace prefix
