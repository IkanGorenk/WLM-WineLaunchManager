#pragma once

// Small process / string / filesystem helpers used all over the launcher.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <sys/types.h>  // pid_t

namespace util {

namespace fs = std::filesystem;

using EnvMap = std::map<std::string, std::string>;

// Copy of the current environment, safe to hand to external programs.
// When this launcher runs from inside an AppImage, LD_LIBRARY_PATH points at the
// AppImage's bundled libraries - child processes must not inherit it, otherwise
// things like "open folder" silently fail.
EnvMap clean_env();

// Owns a null-terminated char* array built from an EnvMap (for execvpe).
class EnvBlock {
public:
    explicit EnvBlock(const EnvMap& env);
    char** data() { return ptrs_.data(); }

private:
    std::vector<std::string> storage_;
    std::vector<char*> ptrs_;
};

// Fork + exec with stdout/stderr redirected to /dev/null (subprocess.Popen(...,
// stdout=DEVNULL) equivalent). Returns false when the command could not be
// executed; errno is reported through *err.
bool spawn_detached(const std::vector<std::string>& args, const EnvMap& extra_env, int* err = nullptr);

// fork + exec that WAITS for the child and collects its output - for the short
// helper programs the launcher runs inline (wineboot, ...) where both the exit
// code and the messages are needed.
//
// *exit_code is the child's exit status (128 + signal when it was killed).
// *output, when given, receives everything the child wrote to stdout/stderr.
// *err is set when the process could not be started at all. Returns false only
// in that case: `true` with a non-zero *exit_code means "it ran and failed".
bool run_captured(const std::vector<std::string>& args, const EnvMap& extra_env,
                  int* exit_code, std::string* output = nullptr, int* err = nullptr);

// Asks a running child to stop: SIGTERM, then SIGKILL if it is still there
// after grace_ms. Safe to call from another thread than run_captured().
void terminate_child(pid_t child, int grace_ms = 3000);

// xdg-open with the clean environment.
bool open_path(const std::string& path, int* err = nullptr);

// shlex.quote() / shlex.split() equivalents.
std::string shell_quote(const std::string& s);
std::vector<std::string> shlex_split(const std::string& text, bool* balanced = nullptr);

bool is_executable(const fs::path& p);
void make_executable(const fs::path& p);
fs::path resolve_path(const fs::path& p);
bool is_within(const fs::path& child, const fs::path& parent);
void move_path(const fs::path& from, const fs::path& to);

std::string lower(const std::string& s);
bool is_all_digits(const std::string& s);

// Python: human_size() - "482.3 MB" / "17 B", used by the backup/restore and
// "Download Online" progress reports.
std::string human_size(double num_bytes);

}  // namespace util
