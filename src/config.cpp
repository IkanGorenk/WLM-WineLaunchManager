#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace cfg {

namespace fs = std::filesystem;

std::string home_dir() {
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home);
    return std::string(".");
}

const fs::path directory = fs::path(home_dir()) / "wlm";
const fs::path icon_dir = directory / "icons";
const fs::path bashlaunch_dir = directory / "bashlaunch";

const fs::path protonge_dir = directory / "protonge";
const fs::path protonge_prefix_root = directory / "protonprefixes";
const fs::path protoncachyos_dir = directory / "protoncachyos";
const fs::path protoncachyos_prefix_root = directory / "protoncachyosprefixes";
const fs::path steamproton_prefix_root = directory / "steamprotonprefixes";
const fs::path wine_prefix_root = directory / "wineprefixes";

const fs::path runner_config_file = directory / "runner_config.json";
const fs::path prefix_location_config_file = directory / "prefix_location_config.json";
const fs::path prefix_registry_file = directory / "prefix_registry.json";
const fs::path logs_dir = directory / "logs";
const fs::path env_config_file = directory / "env_config.yaml";
const fs::path hud_config_file = directory / "hud_config.json";
const fs::path gog_auth_file = directory / "gog_auth.json";
const fs::path gog_library_file = directory / "gog_library.json";
const fs::path gog_downloads_dir = directory / "gog_downloads";

void ensure_directories() {
    const fs::path dirs[] = {directory,     bashlaunch_dir,    icon_dir,
                             protonge_dir,  protonge_prefix_root,
                             protoncachyos_dir, protoncachyos_prefix_root,
                             steamproton_prefix_root, wine_prefix_root, logs_dir,
                             gog_downloads_dir};
    for (const auto& dir : dirs) {
        std::error_code ec;
        fs::create_directories(dir, ec);
    }
}

const std::map<std::string, fs::path>& default_prefix_roots() {
    static const std::map<std::string, fs::path> roots = {
        {"wine", wine_prefix_root},
        {"protonge", protonge_prefix_root},
        {"protoncachyos", protoncachyos_prefix_root},
        {"steamproton", steamproton_prefix_root},
    };
    return roots;
}

std::string runner_display_name(const std::string& runner_key) {
    if (runner_key == "wine") return "Wine";
    if (runner_key == "protonge") return "Proton GE";
    if (runner_key == "protoncachyos") return "Proton-CachyOS";
    if (runner_key == "steamproton") return "Proton (Steam)";
    return runner_key;
}

bool read_json(const fs::path& file, json& out) {
    out = json::object();
    std::error_code ec;
    if (!fs::exists(file, ec)) return false;

    std::ifstream in(file);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // allow_exceptions = false: a corrupt or hand-edited file returns a
    // discarded value instead of throwing, so the caller falls back to defaults
    // exactly as it always did.
    json value = json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_object()) return false;  // corrupt file -> defaults

    out = std::move(value);
    return true;
}

bool write_json(const fs::path& file, const json& value) {
    std::ofstream out(file);
    if (!out) return false;
    out << value.dump(4) << "\n";
    return static_cast<bool>(out);
}

json load_runner_config() {
    json v;
    read_json(runner_config_file, v);
    return v;
}

void save_runner_config(const json& cfg_value) {
    if (!write_json(runner_config_file, cfg_value)) {
        std::fprintf(stderr, "Error saving runner config\n");
    }
}

json load_prefix_location_config() {
    json v;
    read_json(prefix_location_config_file, v);
    return v;
}

void save_prefix_location_config(const json& cfg_value) {
    if (!write_json(prefix_location_config_file, cfg_value)) {
        std::fprintf(stderr, "Error saving prefix location config\n");
    }
}

json load_prefix_registry() {
    json v;
    read_json(prefix_registry_file, v);
    return v;
}

void save_prefix_registry(const json& reg) {
    if (!write_json(prefix_registry_file, reg)) {
        std::fprintf(stderr, "Error saving prefix registry\n");
    }
}

}  // namespace cfg
