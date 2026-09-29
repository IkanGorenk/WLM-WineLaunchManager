#pragma once

// Proton GE / Proton-CachyOS / Wine prefix management:
// finding builds, extracting archives, prefix numbering, registry and moving.

#include <gtk/gtk.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace proton {

namespace fs = std::filesystem;

// (build name, path to its "proton" binary)
using BuildList = std::vector<std::pair<std::string, fs::path>>;

BuildList find_proton_installations(const fs::path& base_dir);
BuildList find_protonge_installations();
BuildList find_protoncachyos_installations();

// Proton builds that live in a Steam installation rather than in ~/wlm: the
// compatibilitytools.d folders and steamapps/common. These are the same kind of
// build as the two above (a "proton" script next to a prefix), they are just
// installed by Steam instead of by this launcher, so they are not downloadable
// or removable from here - only usable.
BuildList find_steam_proton_installations();
fs::path find_steam_install_path();

void extract_proton_archive(const fs::path& target_dir, const std::string& build_label);
void extract_protonge_archive();
void extract_protoncachyos_archive();

// Asked before every entry (and every data block) while an archive is being
// unpacked; `entries` counts the entries already written. Return false to abort
// - that is how the "Cancel" button of a download task log stops an extraction
// that is already running. Called from a background thread only.
using ExtractControl = std::function<bool(long long entries)>;

// Extracts ONE Proton archive (.tar.gz/.tar.xz/.tgz/.zip) into target_dir,
// wrapping the result in a single folder when the archive has no single root.
// Shared by the manual "Extract ... Archive..." menu and the online download
// workers; safe to call from a background thread.
//
// Returns false when control() aborted the extraction (the folders this run
// created are removed again, so a cancelled download leaves nothing half
// extracted behind); a real failure throws std::runtime_error.
bool extract_archive_to_dir(const fs::path& archive_path, const fs::path& target_dir,
                            const ExtractControl& control = nullptr);

void open_proton_folder(const fs::path& target_dir, const std::string& build_label);
void open_protonge_folder();
void open_protoncachyos_folder();

// The next free "GAME###" code for a runner. It is taken from runner_config.json
// and the prefix folder, skipping any number whose folder already exists, so a
// manually created prefix never gets a code that is already on disk.
std::string generate_next_prefix_code(const std::string& runner_key);

// A user-typed prefix name made safe to use as a folder name: trimmed, with any
// path separator dropped. Returns "" for an empty / "." / ".." name, which the
// caller treats as "auto-generate the code instead".
std::string sanitize_prefix_name(const std::string& raw);

// Is this code already used for the runner (registered, or a folder on disk)?
// Compared case-sensitively, matching the filesystem and the registry keys.
bool prefix_code_taken(const std::string& runner_key, const std::string& prefix_code);

fs::path get_prefix_base_dir(const std::string& runner_key);
void set_prefix_base_dir(const std::string& runner_key, const fs::path& path);
void reset_prefix_base_dir(const std::string& runner_key);

void record_prefix_usage(const std::string& runner_key, const std::string& prefix_code,
                         const fs::path& prefix_path, const std::string& proton_name = "",
                         const std::string& proton_path = "");
void update_registry_prefix_path(const std::string& runner_key, const std::string& prefix_code,
                                 const fs::path& new_path);

// After a prefix has been moved, fix EVERY game that uses it: not only the
// prefix_path in runner_config.json, but the launch scripts themselves. A game
// installed inside the prefix physically moved with it, so its "cd" line and
// its wine/proton run line still point at the old (now gone) address; the
// relative paths are recomputed against the new prefix location. Games whose
// folder lives outside the prefix only get their WINEPREFIX /
// STEAM_COMPAT_DATA_PATH exports updated. Returns the names of the scripts
// that were rewritten.
std::vector<std::string> relink_scripts_after_prefix_move(const std::string& runner_key,
                                                          const std::string& prefix_code,
                                                          const fs::path& old_prefix_path,
                                                          const fs::path& new_prefix_path);

struct PrefixInfo {
    std::string runner;
    std::string prefix_code;
    std::string prefix_path;
    std::string proton_name;
    std::string proton_path;
};

// Is exe_path located inside one of the known prefixes?
std::optional<PrefixInfo> find_owning_prefix(const fs::path& exe_path);

std::optional<fs::path> browse_folder_with_create_option(const std::string& title,
                                                         const fs::path& initial_dir,
                                                         GtkWindow* parent = nullptr);

void move_prefix_folder_dialog(const fs::path& old_path, const std::string& prefix_code,
                               const std::string& runner_key,
                               std::function<void(const fs::path&)> on_done,
                               GtkWindow* parent = nullptr);

}  // namespace proton
