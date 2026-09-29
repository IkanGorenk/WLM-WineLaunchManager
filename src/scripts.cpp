#include "scripts.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <regex>

#include "config.h"
#include "proton.h"
#include "util.h"

namespace scripts {

namespace {

namespace fs = std::filesystem;

std::string read_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> read_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    if (!in) return lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

bool blank(const std::string& s) {
    for (char c : s) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    }
    return true;
}

}  // namespace

LaunchOptions parse_launch_options(const std::string& text) {
    LaunchOptions result;
    if (blank(text)) return result;

    bool balanced = true;
    std::vector<std::string> tokens = util::shlex_split(text, &balanced);
    if (!balanced) {
        tokens.clear();
        std::string current;
        for (char c : text) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!current.empty()) {
                    tokens.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) tokens.push_back(current);
    }

    auto is_env_token = [](const std::string& t) {
        return t.find('=') != std::string::npos && t[0] != '-';
    };
    auto add_env = [&result](const std::string& t) {
        size_t eq = t.find('=');
        result.env.emplace_back(t.substr(0, eq), t.substr(eq + 1));
    };

    auto command_it = std::find(tokens.begin(), tokens.end(), "%command%");
    if (command_it != tokens.end()) {
        for (auto it = tokens.begin(); it != command_it; ++it) {
            if (is_env_token(*it)) add_env(*it);
        }
        for (auto it = std::next(command_it); it != tokens.end(); ++it) {
            result.args.push_back(*it);
        }
    } else {
        for (const auto& token : tokens) {
            if (is_env_token(token)) {
                add_env(token);
            } else {
                result.args.push_back(token);
            }
        }
    }
    return result;
}

std::optional<std::string> extract_exe_path_from_script(const fs::path& script_path) {
    std::string content = read_file(script_path);
    if (content.empty() && !fs::exists(script_path)) return std::nullopt;

    static const std::regex wine_re(R"re(wine\s+"([^"]+)")re");
    // The proton pattern deliberately does not require the word "proton" inside
    // the quotes: a script written by "Restore Backup" keeps an empty proton
    // path ("" run "exe.exe") until the user picks a Proton build, and the exe
    // still has to be readable for the first PLAY afterwards.
    static const std::regex proton_re(R"re("[^"]*"\s+run\s+"([^"]+)")re");

    std::smatch match;
    if (std::regex_search(content, match, wine_re)) return match[1].str();
    if (std::regex_search(content, match, proton_re)) return match[1].str();
    return std::nullopt;
}

std::optional<std::string> extract_folder_path_from_script(const fs::path& script_path) {
    std::vector<std::string> lines = read_lines(script_path);
    if (lines.size() < 2) return std::nullopt;

    static const std::regex cd_re(R"re(cd\s+"([^"]+)")re");
    std::smatch match;
    std::string second = lines[1];
    if (std::regex_search(second, match, cd_re)) return match[1].str();
    return std::nullopt;
}

std::string build_script_content(const std::string& folder_path, const std::string& exe_path,
                                 const RunnerChoice& choice) {
    std::vector<std::string> lines;
    lines.push_back("#!/bin/bash");
    lines.push_back("cd \"" + folder_path + "\"");

    std::string comment = choice.comment;
    // trim
    while (!comment.empty() && (comment.back() == ' ' || comment.back() == '\n' ||
                                comment.back() == '\r' || comment.back() == '\t')) {
        comment.pop_back();
    }
    size_t first = comment.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) comment.clear();
    else comment = comment.substr(first);

    if (!comment.empty()) {
        size_t start = 0;
        while (start <= comment.size()) {
            size_t nl = comment.find('\n', start);
            std::string part = comment.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            lines.push_back("# " + part);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }

    std::string launch_options = choice.launch_options;
    while (!launch_options.empty() && (launch_options.back() == ' ' || launch_options.back() == '\n' ||
                                       launch_options.back() == '\r' || launch_options.back() == '\t')) {
        launch_options.pop_back();
    }
    size_t lo_first = launch_options.find_first_not_of(" \t\r\n");
    if (lo_first == std::string::npos) launch_options.clear();
    else launch_options = launch_options.substr(lo_first);

    if (!launch_options.empty()) {
        lines.push_back("# Launch Options: " + launch_options);
    }

    LaunchOptions parsed = parse_launch_options(launch_options);
    for (const auto& kv : parsed.env) {
        lines.push_back("export " + kv.first + "=" + util::shell_quote(kv.second));
    }

    std::string extra_args;
    for (const auto& arg : parsed.args) {
        extra_args += " " + util::shell_quote(arg);
    }

    if (choice.runner != "wine" && !choice.runner.empty()) {
        lines.push_back("export STEAM_COMPAT_DATA_PATH=\"" + choice.prefix_path + "\"");
        lines.push_back("export STEAM_COMPAT_CLIENT_INSTALL_PATH=\"" +
                        proton::find_steam_install_path().string() + "\"");
        lines.push_back("\"" + choice.proton_path + "\" run \"" + exe_path + "\"" + extra_args);
    } else {
        if (!choice.prefix_path.empty()) {
            lines.push_back("export WINEPREFIX=\"" + choice.prefix_path + "\"");
        }
        lines.push_back("wine \"" + exe_path + "\"" + extra_args);
    }

    std::string content;
    for (const auto& line : lines) {
        content += line;
        content += "\n";
    }
    return content;
}

json choice_to_json(const RunnerChoice& choice) {
    json v = json::object();
    v["runner"] = choice.runner;
    if (choice.runner != "wine") {
        v["proton_name"] = choice.proton_name;
        v["proton_path"] = choice.proton_path;
    }
    if (!choice.prefix_code.empty()) {
        v["prefix_code"] = choice.prefix_code;
    }
    if (!choice.prefix_path.empty()) {
        v["prefix_path"] = choice.prefix_path;
    }
    v["launch_options"] = choice.launch_options;
    v["comment"] = choice.comment;
    return v;
}

RunnerChoice choice_from_json(const json& value) {
    RunnerChoice choice;
    choice.runner = json_str(value, "runner");
    choice.proton_name = json_str(value, "proton_name");
    choice.proton_path = json_str(value, "proton_path");
    choice.prefix_code = json_str(value, "prefix_code");
    choice.prefix_path = json_str(value, "prefix_path");
    choice.launch_options = json_str(value, "launch_options");
    choice.comment = json_str(value, "comment");
    choice.use_prefix = !choice.prefix_path.empty();
    return choice;
}

}  // namespace scripts
