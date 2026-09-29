#pragma once

// Reading / writing the generated bash launch scripts (~/wlm/bashlaunch/*.sh)
// and Steam-style launch option parsing.

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// Same alias as in config.h (ordered_json) - a repeated identical alias is fine.
using json = nlohmann::ordered_json;

namespace scripts {

// One "runner choice" dialog result (also what gets stored in runner_config.json).
struct RunnerChoice {
    std::string runner;  // "wine" | "protonge" | "protoncachyos"
    std::string proton_name;
    std::string proton_path;
    std::string prefix_code;
    std::string prefix_path;
    std::string launch_options;
    std::string comment;
    bool use_prefix = false;  // wine: "use an isolated prefix for this game"
};

struct LaunchOptions {
    std::vector<std::pair<std::string, std::string>> env;  // KEY=VALUE
    std::vector<std::string> args;                         // arguments after %command%
};

LaunchOptions parse_launch_options(const std::string& text);

std::optional<std::string> extract_exe_path_from_script(const std::filesystem::path& script_path);
std::optional<std::string> extract_folder_path_from_script(const std::filesystem::path& script_path);

// Rebuilds the .sh content for the selected runner / comment / launch options.
// The 'cd "..."' line always stays on line 2 so game-info parsing keeps working.
std::string build_script_content(const std::string& folder_path, const std::string& exe_path,
                                 const RunnerChoice& choice);

json choice_to_json(const RunnerChoice& choice);
RunnerChoice choice_from_json(const json& value);

}  // namespace scripts
