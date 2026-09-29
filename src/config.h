#pragma once

// Configuration paths + JSON config file access.
// Mirrors the Python launcher's ~/wlm layout exactly.

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <string>

// Every config file and the GOG API are handled with nlohmann/json.
//
// ordered_json rather than plain json: it keeps object keys in insertion order,
// so the launcher's config files are written back in the same shape they were
// read in instead of being silently reshuffled alphabetically on every save.
using json = nlohmann::ordered_json;

// ---- null-safe readers ------------------------------------------------------
//
// nlohmann's own value() only falls back to the default when a key is ABSENT; it
// throws a type_error when the key is present but holds null. Null is not a rare
// edge case here: prefix_registry.json stores "proton_name": null for every wine
// prefix (those are not tied to a Proton build), so a plain value() call on such
// a field would throw while merely listing prefixes.
//
// These readers return the fallback for missing, null AND wrong-typed values,
// which is how the launcher's own JSON layer always behaved.

inline std::string json_str(const json& j, const std::string& key) {
    if (!j.is_object()) return {};
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

inline bool json_bool(const json& j, const std::string& key) {
    if (!j.is_object()) return false;
    const auto it = j.find(key);
    if (it == j.end()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;
    if (it->is_string()) {
        const std::string s = it->get<std::string>();
        return s == "1" || s == "true" || s == "yes";
    }
    return false;
}

inline long long json_int(const json& j, const std::string& key) {
    if (!j.is_object()) return 0;
    const auto it = j.find(key);
    if (it == j.end()) return 0;
    if (it->is_number_integer()) return it->get<long long>();
    if (it->is_number_float()) return static_cast<long long>(it->get<double>());
    if (it->is_string()) {
        try {
            return std::stoll(it->get<std::string>());
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

// A sub-object that is guaranteed to be an object, so the caller can index into
// it without checking (indexing a non-object throws in nlohmann).
inline json json_obj(const json& j, const std::string& key) {
    if (!j.is_object()) return json::object();
    const auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return json::object();
    return *it;
}

// A sub-array that is guaranteed to be an array, safe to range over.
inline json json_arr(const json& j, const std::string& key) {
    if (!j.is_object()) return json::array();
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return json::array();
    return *it;
}

namespace cfg {

namespace fs = std::filesystem;

// ---- configuration paths ----
extern const fs::path directory;                   // ~/wlm
extern const fs::path icon_dir;                    // ~/wlm/icons
extern const fs::path bashlaunch_dir;              // ~/wlm/bashlaunch

extern const fs::path protonge_dir;                // tempat ekstrak binary Proton GE
extern const fs::path protonge_prefix_root;        // prefix ProtonGE dibuat disini
extern const fs::path protoncachyos_dir;           // tempat ekstrak binary Proton-CachyOS
extern const fs::path protoncachyos_prefix_root;   // prefix Proton-CachyOS dibuat disini
extern const fs::path steamproton_prefix_root;    // prefix Proton dari Steam dibuat disini
extern const fs::path wine_prefix_root;            // prefix Wine (vanilla)

extern const fs::path runner_config_file;          // pilihan runner per game
extern const fs::path prefix_location_config_file;// lokasi default kustom prefix baru
extern const fs::path prefix_registry_file;        // catatan semua prefix yang pernah dibuat
extern const fs::path logs_dir;                    // output stdout/stderr per game
extern const fs::path env_config_file;             // URL "Download Online" (env_config.yaml)
extern const fs::path hud_config_file;             // pengaturan HUD (hud_config.json)
extern const fs::path gog_auth_file;               // ~/wlm/gog_auth.json (GOG OAuth token)
extern const fs::path gog_library_file;            // ~/wlm/gog_library.json (cached library)
extern const fs::path gog_downloads_dir;           // ~/wlm/gog_downloads (installer files)

constexpr int ICON_SIZE = 250;
constexpr int ICON_WIDTH = ICON_SIZE;
constexpr int ICON_HEIGHT = ICON_SIZE;
constexpr int MAX_LOG_BUFFER_LINES = 5000;

// Python: WLM_VERSION / WLM_DEVELOPER / WLM_MAINTAINER (--version)
constexpr const char* WLM_VERSION = "0.4.0~beta";
constexpr const char* WLM_DEVELOPER = "Opensource OS Gathering Republic (OOGR)";
constexpr const char* WLM_MAINTAINER = "Didi Sloth Stanca & Ikan Goreng";

std::string home_dir();
void ensure_directories();

// Lokasi default (bawaan) tempat prefix BARU dibuat untuk masing-masing runner.
const std::map<std::string, fs::path>& default_prefix_roots();

// Python: RUNNER_DISPLAY_NAMES - "wine" -> "Wine", "protonge" -> "Proton GE", ...
std::string runner_display_name(const std::string& runner_key);

// ---- JSON helpers ----
bool read_json(const fs::path& file, json& out);
bool write_json(const fs::path& file, const json& value);

// NOTE: there is no window size/position config any more. The launcher always
// opens at the standard windowed size (see winmode::standard_size), which is
// what makes maximize/unmaximize behave - a remembered geometry could be larger
// than the display can show, and the window manager then refuses to maximize.

json load_runner_config();
void save_runner_config(const json& cfg_value);
json load_prefix_location_config();
void save_prefix_location_config(const json& cfg_value);
json load_prefix_registry();
void save_prefix_registry(const json& reg);

}  // namespace cfg
