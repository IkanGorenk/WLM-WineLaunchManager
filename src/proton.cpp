#include "proton.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>

#include "config.h"
#include "dialogs.h"
#include "scripts.h"
#include "ui.h"
#include "util.h"

namespace proton {

namespace {

// First path component of an archive entry, ignoring leading '/' (matches the
// Python Path(name).parts[0] logic used to detect single-root archives).
std::string first_component(const fs::path& p) {
    for (const auto& part : p) {
        std::string s = part.string();
        if (s.empty() || s == "/" || s == "." || s == "..") continue;
        return s;
    }
    return "";
}

// Archive entry path made safe relative to target_dir (no absolute paths, no "..").
fs::path sanitize_entry_path(const std::string& raw) {
    fs::path out;
    fs::path raw_path(raw);
    for (const auto& part : raw_path) {
        if (part.is_absolute()) continue;
        std::string s = part.string();
        if (s.empty() || s == "." || s == "..") continue;
        out /= s;
    }
    return out;
}

std::string archive_error(struct archive* a, const char* fallback) {
    const char* msg = archive_error_string(a);
    return msg ? std::string(msg) : std::string(fallback);
}

bool extract_archive(const fs::path& archive_path, const fs::path& target_dir,
                     const ExtractControl& control) {
    std::error_code ec;
    fs::create_directories(target_dir, ec);

    std::set<std::string> before_entries;
    if (fs::is_directory(target_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(target_dir, ec)) {
            if (entry.is_directory()) before_entries.insert(entry.path().filename().string());
        }
    }

    std::set<std::string> top_level_names;

    struct archive* reader = archive_read_new();
    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);
    if (archive_read_open_filename(reader, archive_path.c_str(), 10240) != ARCHIVE_OK) {
        std::string msg = archive_error(reader, "cannot open archive");
        archive_read_free(reader);
        throw std::runtime_error(msg);
    }

    struct archive* writer = archive_write_disk_new();
    archive_write_disk_set_options(writer, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                               ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS |
                                               ARCHIVE_EXTRACT_SECURE_SYMLINKS |
                                               ARCHIVE_EXTRACT_SECURE_NODOTDOT);

    struct archive_entry* entry = nullptr;
    bool failed = false;
    bool cancelled = false;
    long long written_entries = 0;
    std::string failure;

    // The control callback (the download task's Cancel button) is consulted
    // between entries and between data blocks, so a long extraction can be
    // stopped as well - not just the download before it.
    auto aborted = [&]() { return control && !control(written_entries); };

    while (!failed && !cancelled && archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
        fs::path relative = sanitize_entry_path(archive_entry_pathname(entry));
        std::string top = first_component(relative);
        if (top.empty()) continue;
        top_level_names.insert(top);

        fs::path destination = target_dir / relative;
        archive_entry_set_pathname(entry, destination.c_str());

        if (archive_write_header(writer, entry) != ARCHIVE_OK) {
            failed = true;
            failure = archive_error(writer, "failed to write entry");
            break;
        }

        const void* buffer = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        int rc;
        while ((rc = archive_read_data_block(reader, &buffer, &size, &offset)) == ARCHIVE_OK) {
            if (aborted()) {
                cancelled = true;
                break;
            }
            if (size > 0 && archive_write_data(writer, buffer, size) < 0) {
                failed = true;
                failure = archive_error(writer, "failed to write entry data");
                break;
            }
        }
        if (failed || cancelled) break;
        ++written_entries;
        if (rc != ARCHIVE_EOF) {
            failed = true;
            failure = archive_error(reader, "failed to read entry data");
            break;
        }
        archive_write_finish_entry(writer);
    }

    archive_write_free(writer);
    archive_read_close(reader);
    archive_read_free(reader);

    if (failed) throw std::runtime_error(failure);

    std::set<std::string> new_top_names;
    for (const auto& name : top_level_names) {
        if (before_entries.count(name) == 0) new_top_names.insert(name);
    }

    // Cancelled: throw away everything this run created (only the folders that
    // did not exist before the extraction started) so the runner directory is
    // never left with a half installed build that Proton discovery would pick up.
    if (cancelled) {
        for (const auto& name : new_top_names) {
            std::error_code remove_ec;
            fs::remove_all(target_dir / name, remove_ec);  // best effort
        }
        return false;
    }

    // If the archive has no single parent folder, wrap the extracted files in
    // a folder named after the archive itself.
    if (new_top_names.size() != 1) {
        std::string wrapper_name = archive_path.filename().string();
        for (const char* suffix : {".tar.gz", ".tar.xz", ".tgz", ".zip"}) {
            size_t len = std::strlen(suffix);
            if (wrapper_name.size() > len &&
                wrapper_name.compare(wrapper_name.size() - len, len, suffix) == 0) {
                wrapper_name.erase(wrapper_name.size() - len);
                break;
            }
        }
        fs::path wrapper_dir = target_dir / wrapper_name;
        fs::create_directories(wrapper_dir, ec);
        for (const auto& name : new_top_names) {
            fs::path src = target_dir / name;
            if (src != wrapper_dir && fs::exists(src)) {
                util::move_path(src, wrapper_dir / name);
            }
        }
    }
    return true;
}

}  // namespace

bool extract_archive_to_dir(const fs::path& archive_path, const fs::path& target_dir,
                            const ExtractControl& control) {
    return extract_archive(archive_path, target_dir, control);
}

BuildList find_proton_installations(const fs::path& base_dir) {
    BuildList found;
    std::error_code ec;
    if (!fs::is_directory(base_dir, ec)) return found;

    std::vector<fs::path> dirs;
    for (const auto& entry : fs::directory_iterator(base_dir, ec)) {
        if (entry.is_directory()) dirs.push_back(entry.path());
    }
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() > b.filename().string();
    });

    for (const auto& dir : dirs) {
        fs::path proton_bin = dir / "proton";
        if (fs::exists(proton_bin)) {
            found.emplace_back(dir.filename().string(), proton_bin);
        }
    }
    return found;
}

BuildList find_protonge_installations() {
    return find_proton_installations(cfg::protonge_dir);
}

BuildList find_protoncachyos_installations() {
    return find_proton_installations(cfg::protoncachyos_dir);
}

BuildList find_steam_proton_installations() {
    BuildList found;
    const fs::path home = cfg::home_dir();

    // Where Steam keeps Proton builds: the two compatibilitytools.d locations
    // (where Proton-GE and friends are dropped by hand or by an updater tool)
    // and steamapps/common (where Valve's own builds live, e.g. "Proton 9.0"
    // or "Proton - Experimental").
    const fs::path roots[] = {
        home / ".steam" / "root" / "compatibilitytools.d",
        home / ".local" / "share" / "Steam" / "compatibilitytools.d",
        home / ".var" / "app" / "com.valvesoftware.Steam" / "data" / "Steam" /
            "compatibilitytools.d",
        home / ".local" / "share" / "Steam" / "steamapps" / "common",
        home / ".steam" / "steam" / "steamapps" / "common",
    };

    // A build already listed is not listed twice; the order of `roots` decides
    // which copy wins.
    for (const fs::path& root : roots) {
        for (const auto& build : find_proton_installations(root)) {
            const bool already =
                std::any_of(found.begin(), found.end(),
                            [&](const std::pair<std::string, fs::path>& f) {
                                return f.first == build.first;
                            });
            if (!already) found.push_back(build);
        }
    }

    // Alphabetical, the same order the other two families are listed in.
    std::sort(found.begin(), found.end(),
              [](const std::pair<std::string, fs::path>& a,
                 const std::pair<std::string, fs::path>& b) { return a.first < b.first; });
    return found;
}

fs::path find_steam_install_path() {
    const fs::path home = cfg::home_dir();
    const fs::path candidates[] = {
        home / ".steam" / "steam",
        home / ".local" / "share" / "Steam",
        home / ".var" / "app" / "com.valvesoftware.Steam" / "data" / "Steam",
    };
    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (fs::is_directory(candidate, ec)) return candidate;
    }
    fs::path fallback = cfg::directory / "steamclient";
    fs::create_directories(fallback, ec);
    return fallback;
}

void extract_proton_archive(const fs::path& target_dir, const std::string& build_label) {
    std::optional<std::string> chosen = dialogs::choose_open_file(
        "Select " + build_label + " Archive", build_label + " Archive",
        {"*.tar.gz", "*.tar.xz", "*.tgz", "*.zip"});
    if (!chosen) return;

    fs::path archive_path(*chosen);
    ui::set_status("Extracting " + archive_path.filename().string() + " to WLM directory...", "secondary");

    auto error_holder = std::make_shared<std::string>();

    dialogs::run_with_progress(
        "Extracting " + build_label, "Extracting " + archive_path.filename().string() + "...",
        [archive_path, target_dir, error_holder]() {
            try {
                extract_archive(archive_path, target_dir, nullptr);
            } catch (const std::exception& e) {
                *error_holder = e.what();
            }
        },
        [build_label, target_dir, error_holder]() {
            if (error_holder->empty()) {
                ui::set_status(build_label + " successfully extracted to " + target_dir.string(), "success");
                dialogs::show_info("Done", build_label +
                                               " successfully extracted to the WLM directory:\n" +
                                               target_dir.string());
            } else {
                ui::set_status("Error extracting " + build_label + ": " + *error_holder, "error");
                dialogs::show_error("Error", "Failed to extract archive:\n" + *error_holder);
            }
        });
}

void extract_protonge_archive() {
    extract_proton_archive(cfg::protonge_dir, "Proton GE");
}

void extract_protoncachyos_archive() {
    extract_proton_archive(cfg::protoncachyos_dir, "Proton-CachyOS");
}

void open_proton_folder(const fs::path& target_dir, const std::string& build_label) {
    std::error_code ec;
    fs::create_directories(target_dir, ec);
    int err = 0;
    if (util::open_path(target_dir.string(), &err)) {
        ui::set_status("Opening " + build_label + " Folder...", "secondary");
    } else {
        ui::set_status("Error opening " + build_label + " folder: " + std::strerror(err), "error");
    }
}

void open_protonge_folder() {
    open_proton_folder(cfg::protonge_dir, "Proton GE");
}

void open_protoncachyos_folder() {
    open_proton_folder(cfg::protoncachyos_dir, "Proton-CachyOS");
}

// A user-typed prefix name may not become a path or a registry key as it is:
// trim it, refuse anything with a path separator or a dot-only name, and make
// sure a different case cannot collide with an existing prefix.
std::string sanitize_prefix_name(const std::string& raw) {
    std::string name;
    for (char c : raw) {
        if (c == '/' || c == '\\') continue;
        name += c;
    }

    const size_t begin = name.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const size_t end = name.find_last_not_of(" \t\r\n");
    name = name.substr(begin, end - begin + 1);

    // "." and ".." would resolve to the parent / the base dir itself.
    if (name == "." || name == "..") return "";
    return name;
}

// Is prefix_code already taken for this runner - as a registered prefix or as
// a folder that is already on disk? Compared exactly, not case-insensitively:
// on Linux "GAME001" and "game001" really are two different folders with two
// different registry keys, so they may both exist.
bool prefix_code_taken(const std::string& runner_key, const std::string& prefix_code) {
    const json registry = cfg::load_prefix_registry();
    if (registry.is_object()) {
        for (const auto& kv : registry.items()) {
            const json& entry = kv.value();
            if (json_str(entry, "runner") != runner_key) continue;
            if (json_str(entry, "prefix_code") == prefix_code) return true;
        }
    }

    std::error_code ec;
    const fs::path root = get_prefix_base_dir(runner_key);
    if (fs::exists(root / prefix_code, ec)) return true;

    // A different base directory may already hold a folder with that name.
    const auto& default_root = cfg::default_prefix_roots();
    const auto it = default_root.find(runner_key);
    if (it != default_root.end() && it->second != root) {
        if (fs::exists(it->second / prefix_code, ec)) return true;
    }
    return false;
}

std::string generate_next_prefix_code(const std::string& runner_key) {
    std::vector<int> numbers;

    json runner_cfg = cfg::load_runner_config();
    if (runner_cfg.is_object()) {
        for (const auto& kv : runner_cfg.items()) {
            const json& entry = kv.value();
            if (json_str(entry, "runner") != runner_key) continue;
            std::string code = json_str(entry, "prefix_code");
            if (code.compare(0, 4, "GAME") == 0 && util::is_all_digits(code.substr(4))) {
                numbers.push_back(std::atoi(code.substr(4).c_str()));
            }
        }
    }

    auto root_it = cfg::default_prefix_roots().find(runner_key);
    if (root_it != cfg::default_prefix_roots().end()) {
        std::error_code ec;
        if (fs::is_directory(root_it->second, ec)) {
            for (const auto& entry : fs::directory_iterator(root_it->second, ec)) {
                if (!entry.is_directory()) continue;
                std::string name = entry.path().filename().string();
                if (name.compare(0, 4, "GAME") == 0 && util::is_all_digits(name.substr(4))) {
                    numbers.push_back(std::atoi(name.substr(4).c_str()));
                }
            }
        }
    }

    int next_num = 1;
    if (!numbers.empty()) next_num = *std::max_element(numbers.begin(), numbers.end()) + 1;

    // A code can be free in runner_config.json while its folder is already on
    // disk (a prefix restored from a backup, for instance), so keep counting
    // until both the number and the folder are unused.
    char buffer[16];
    while (true) {
        std::snprintf(buffer, sizeof(buffer), "GAME%03d", next_num);
        if (!prefix_code_taken(runner_key, buffer)) break;
        ++next_num;
    }
    return std::string(buffer);
}

fs::path get_prefix_base_dir(const std::string& runner_key) {
    json loc_cfg;
    cfg::read_json(cfg::prefix_location_config_file, loc_cfg);

    fs::path default_root = cfg::default_prefix_roots().at(runner_key);
    std::string custom = json_str(loc_cfg, runner_key);
    if (!custom.empty()) {
        std::error_code ec;
        fs::create_directories(custom, ec);
        if (!ec) return fs::path(custom);
        return default_root;
    }
    return default_root;
}

void set_prefix_base_dir(const std::string& runner_key, const fs::path& path) {
    json loc_cfg;
    cfg::read_json(cfg::prefix_location_config_file, loc_cfg);
    loc_cfg[runner_key] = path.string();
    cfg::save_prefix_location_config(loc_cfg);
}

void reset_prefix_base_dir(const std::string& runner_key) {
    json loc_cfg;
    cfg::read_json(cfg::prefix_location_config_file, loc_cfg);
    if (loc_cfg.contains(runner_key)) {
        loc_cfg.erase(runner_key);
        cfg::save_prefix_location_config(loc_cfg);
    }
}

void record_prefix_usage(const std::string& runner_key, const std::string& prefix_code,
                         const fs::path& prefix_path, const std::string& proton_name,
                         const std::string& proton_path) {
    json reg = cfg::load_prefix_registry();
    std::string key = runner_key + ":" + prefix_code;
    json entry = json::object();
    entry["runner"] = runner_key;
    entry["prefix_code"] = prefix_code;
    entry["prefix_path"] = prefix_path.string();
    if (runner_key == "wine") {
        // Vanilla Wine prefixes are not tied to a Proton build.
        entry["proton_name"] = nullptr;
        entry["proton_path"] = nullptr;
    } else {
        entry["proton_name"] = proton_name;
        entry["proton_path"] = proton_path;
    }
    reg[key] = entry;
    cfg::save_prefix_registry(reg);
}

void update_registry_prefix_path(const std::string& runner_key, const std::string& prefix_code,
                                 const fs::path& new_path) {
    json reg = cfg::load_prefix_registry();
    std::string key = runner_key + ":" + prefix_code;
    if (reg.contains(key)) {
        reg[key]["prefix_path"] = new_path.string();
    } else {
        json entry = json::object();
        entry["runner"] = runner_key;
        entry["prefix_code"] = prefix_code;
        entry["prefix_path"] = new_path.string();
        entry["proton_name"] = nullptr;
        entry["proton_path"] = nullptr;
        reg[key] = entry;
    }
    cfg::save_prefix_registry(reg);
}

std::optional<PrefixInfo> find_owning_prefix(const fs::path& exe_path) {
    fs::path exe_resolved = util::resolve_path(exe_path);

    json registry = cfg::load_prefix_registry();

    struct Candidate {
        std::string runner;
        std::string code;
        fs::path path;
    };
    std::vector<Candidate> candidates;

    if (registry.is_object()) {
        for (const auto& kv : registry.items()) {
            const json& entry = kv.value();
            std::string path = json_str(entry, "prefix_path");
            std::string runner = json_str(entry, "runner");
            std::string code = json_str(entry, "prefix_code");
            if (!path.empty() && !runner.empty() && !code.empty()) {
                candidates.push_back({runner, code, fs::path(path)});
            }
        }
    }

    // Fallback: also scan the default and currently active custom folders, for
    // prefixes that were never recorded in the registry.
    json loc_cfg;
    cfg::read_json(cfg::prefix_location_config_file, loc_cfg);

    std::vector<std::pair<std::string, fs::path>> scan_roots = {
        {"wine", cfg::wine_prefix_root},
        {"protonge", cfg::protonge_prefix_root},
        {"protoncachyos", cfg::protoncachyos_prefix_root},
        {"steamproton", cfg::steamproton_prefix_root},
    };
    const size_t base_root_count = scan_roots.size();
    for (size_t i = 0; i < base_root_count; ++i) {
        std::string custom = json_str(loc_cfg, scan_roots[i].first);
        if (!custom.empty()) {
            scan_roots.emplace_back(scan_roots[i].first, fs::path(custom));
        }
    }

    std::set<std::string> known_paths;
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate.path)) {
            known_paths.insert(util::resolve_path(candidate.path).string());
        }
    }

    for (const auto& root : scan_roots) {
        std::error_code ec;
        if (!fs::is_directory(root.second, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root.second, ec)) {
            if (!entry.is_directory()) continue;
            std::error_code resolve_ec;
            fs::path resolved = fs::weakly_canonical(entry.path(), resolve_ec);
            if (resolve_ec) continue;
            if (known_paths.count(resolved.string()) != 0) continue;
            candidates.push_back({root.first, entry.path().filename().string(), entry.path()});
        }
    }

    for (const auto& candidate : candidates) {
        fs::path prefix_resolved = util::resolve_path(candidate.path);
        if (prefix_resolved == exe_resolved || util::is_within(exe_resolved, prefix_resolved)) {
            json reg_entry = json_obj(registry, candidate.runner + ":" + candidate.code);
            PrefixInfo info;
            info.runner = candidate.runner;
            info.prefix_code = candidate.code;
            info.prefix_path = prefix_resolved.string();
            info.proton_name = json_str(reg_entry, "proton_name");
            info.proton_path = json_str(reg_entry, "proton_path");
            return info;
        }
    }
    return std::nullopt;
}

std::optional<fs::path> browse_folder_with_create_option(const std::string& title,
                                                         const fs::path& initial_dir,
                                                         GtkWindow* parent) {
    std::optional<std::string> chosen =
        dialogs::choose_folder(title, initial_dir.string(), parent);
    if (!chosen) return std::nullopt;

    fs::path chosen_path(*chosen);
    bool make_new = dialogs::ask_yes_no(
        "Create New Folder?",
        "Selected folder:\n" + chosen_path.string() + "\n\n" +
            "Create a new folder inside it for the prefix(es)?\n"
            "(Recommended so prefixes stay organized and don't mix with other files.)",
        parent);
    if (!make_new) return chosen_path;

    std::optional<std::string> folder_name =
        dialogs::ask_string("New Folder", "New folder name:", "", parent);
    if (!folder_name || folder_name->empty()) return chosen_path;

    fs::path new_dir = chosen_path / *folder_name;
    std::error_code ec;
    fs::create_directories(new_dir, ec);
    if (ec) {
        dialogs::show_error("Error", "Failed to create folder:\n" + ec.message(), parent);
        return chosen_path;
    }
    return new_dir;
}

void move_prefix_folder_dialog(const fs::path& old_path, const std::string& prefix_code,
                               const std::string& /*runner_key*/,
                               std::function<void(const fs::path&)> on_done,
                               GtkWindow* parent) {
    fs::path initial_dir = old_path.parent_path();
    std::error_code ec;
    if (!fs::is_directory(initial_dir, ec)) initial_dir = cfg::home_dir();

    std::optional<fs::path> new_base_path = browse_folder_with_create_option(
        "Choose New Location for Prefix " + prefix_code, initial_dir, parent);
    if (!new_base_path) return;

    fs::path new_path = *new_base_path / prefix_code;

    bool same_location = false;
    try {
        same_location = util::resolve_path(new_path) == util::resolve_path(old_path);
    } catch (...) {
        same_location = false;
    }
    if (same_location) {
        dialogs::show_info("Info", "The prefix is already in this location.", parent);
        return;
    }
    if (fs::exists(new_path)) {
        dialogs::show_error("Error",
                            "A folder named '" + prefix_code + "' already exists in that location.",
                            parent);
        return;
    }
    if (!fs::exists(old_path)) {
        dialogs::show_error("Error", "Prefix folder not found on disk:\n" + old_path.string(), parent);
        return;
    }
    if (!dialogs::ask_yes_no(
            "Confirm Move",
            "Move prefix '" + prefix_code + "' to:\n" + new_path.string() + "\n\n" +
                "Make sure the game/app using this prefix is not currently running.\n"
                "This may take a while for large prefixes. Continue?",
            parent)) {
        return;
    }

    auto error_holder = std::make_shared<std::string>();

    dialogs::run_with_progress(
        "Moving Prefix", "Moving prefix '" + prefix_code + "'...",
        [old_path, new_path, new_base_path, error_holder]() {
            try {
                std::error_code mkdir_ec;
                fs::create_directories(*new_base_path, mkdir_ec);
                util::move_path(old_path, new_path);
            } catch (const std::exception& e) {
                *error_holder = e.what();
            }
        },
        [prefix_code, new_path, error_holder, on_done, parent]() {
            if (error_holder->empty()) {
                ui::set_status("Prefix '" + prefix_code + "' moved to " + new_path.string(), "success");
                if (on_done) on_done(new_path);
            } else {
                ui::set_status("Error moving prefix: " + *error_holder, "error");
                dialogs::show_error("Error", "Failed to move prefix:\n" + *error_holder, parent);
            }
        });
}

std::vector<std::string> relink_scripts_after_prefix_move(const std::string& runner_key,
                                                          const std::string& prefix_code,
                                                          const fs::path& old_prefix_path,
                                                          const fs::path& new_prefix_path) {
    fs::path old_resolved = util::resolve_path(old_prefix_path);
    fs::path new_resolved = new_prefix_path;

    json runner_cfg = cfg::load_runner_config();
    std::vector<std::string> updated_scripts;
    if (!runner_cfg.is_object()) return updated_scripts;

    // A path that lived inside the old prefix is rebased onto the new one;
    // anything else (a game stored elsewhere) keeps its own location.
    auto rebase = [&](const fs::path& path) {
        fs::path resolved = util::resolve_path(path);
        std::error_code ec;
        if (util::is_within(resolved, old_resolved)) {
            fs::path rel = fs::relative(resolved, old_resolved, ec);
            if (!ec && !rel.empty()) return new_resolved / rel;
        }
        return resolved;
    };

    bool changed = false;
    std::vector<std::string> script_names;
    script_names.reserve(runner_cfg.size());
    for (const auto& kv : runner_cfg.items()) script_names.push_back(kv.key());

    for (const std::string& script_name : script_names) {
        json entry = json_obj(runner_cfg, script_name);
        if (json_str(entry, "runner") != runner_key) continue;
        if (json_str(entry, "prefix_code") != prefix_code) continue;

        entry["prefix_path"] = new_resolved.string();
        runner_cfg[script_name] = entry;
        changed = true;

        fs::path script_path = cfg::bashlaunch_dir / (script_name + ".sh");
        if (!fs::exists(script_path)) continue;

        std::optional<std::string> folder = scripts::extract_folder_path_from_script(script_path);
        std::optional<std::string> exe = scripts::extract_exe_path_from_script(script_path);
        if (!folder || !exe) continue;

        fs::path new_folder = rebase(*folder);
        fs::path new_exe = rebase(*exe);

        try {
            scripts::RunnerChoice choice = scripts::choice_from_json(entry);
            std::string content =
                scripts::build_script_content(new_folder.string(), new_exe.string(), choice);
            {
                std::ofstream out(script_path);
                if (!out) throw std::runtime_error("cannot write " + script_path.string());
                out << content;
            }
            util::make_executable(script_path);
            updated_scripts.push_back(script_name);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Error relinking script '%s' after prefix move: %s\n",
                         script_name.c_str(), e.what());
        }
    }

    if (changed) cfg::save_runner_config(runner_cfg);
    return updated_scripts;
}

}  // namespace proton
