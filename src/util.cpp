#include "util.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace util {


EnvMap clean_env() {
    EnvMap env;
    bool has_appimage = false;
    for (char** p = environ; p && *p; ++p) {
        std::string entry(*p);
        size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        std::string key = entry.substr(0, eq);
        if (key == "APPIMAGE") has_appimage = true;
        env[key] = entry.substr(eq + 1);
    }
    if (has_appimage) env.erase("LD_LIBRARY_PATH");
    return env;
}

EnvBlock::EnvBlock(const EnvMap& env) {
    storage_.reserve(env.size() + 1);
    for (const auto& kv : env) {
        storage_.push_back(kv.first + "=" + kv.second);
    }
    ptrs_.reserve(storage_.size() + 1);
    for (auto& s : storage_) {
        ptrs_.push_back(&s[0]);
    }
    ptrs_.push_back(nullptr);
}

bool spawn_detached(const std::vector<std::string>& args, const EnvMap& extra_env, int* err) {
    if (err) *err = 0;
    if (args.empty()) {
        if (err) *err = EINVAL;
        return false;
    }

    EnvMap env = clean_env();
    for (const auto& kv : extra_env) env[kv.first] = kv.second;
    EnvBlock block(env);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        if (err) *err = errno;
        return false;
    }
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    pid_t mid = fork();
    if (mid < 0) {
        int e = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        if (err) *err = e;
        return false;
    }

    if (mid == 0) {
        // Intermediate child: fork again so the real process is reparented to init
        // and never lingers as a zombie in this launcher.
        close(pipefd[0]);
        pid_t grandchild = fork();
        if (grandchild < 0) {
            int e = errno;
            ssize_t ignored = write(pipefd[1], &e, sizeof(e));
            (void)ignored;
            _exit(127);
        }
        if (grandchild > 0) _exit(0);

        setsid();
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        execvpe(argv[0], argv.data(), block.data());
        int e = errno;
        ssize_t ignored = write(pipefd[1], &e, sizeof(e));
        (void)ignored;
        _exit(127);
    }

    close(pipefd[1]);
    int child_errno = 0;
    ssize_t n = read(pipefd[0], &child_errno, sizeof(child_errno));
    close(pipefd[0]);
    int status = 0;
    while (waitpid(mid, &status, 0) < 0 && errno == EINTR) {
    }

    if (n > 0) {
        if (err) *err = child_errno;
        return false;
    }
    return true;
}

bool open_path(const std::string& path, int* err) {
    return spawn_detached({"xdg-open", path}, {}, err);
}

std::string shell_quote(const std::string& s) {
    static const char* safe =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_@%+=:,./-";
    if (!s.empty() && s.find_first_not_of(safe) == std::string::npos) {
        return s;
    }
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::vector<std::string> shlex_split(const std::string& text, bool* balanced) {
    std::vector<std::string> out;
    std::string current;
    bool in_single = false;
    bool in_double = false;
    bool pending = false;  // an (possibly empty) token was opened

    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (in_single) {
            if (c == '\'') {
                in_single = false;
            } else {
                current.push_back(c);
            }
            ++i;
            continue;
        }
        if (in_double) {
            if (c == '"') {
                in_double = false;
                ++i;
            } else if (c == '\\' && i + 1 < text.size()) {
                char next = text[i + 1];
                if (next == '"' || next == '\\' || next == '$' || next == '`') {
                    current.push_back(next);
                    i += 2;
                } else if (next == '\n') {
                    i += 2;  // line continuation
                } else {
                    current.push_back(c);
                    ++i;
                }
            } else {
                current.push_back(c);
                ++i;
            }
            continue;
        }
        if (c == '\'') {
            in_single = true;
            pending = true;
            ++i;
        } else if (c == '"') {
            in_double = true;
            pending = true;
            ++i;
        } else if (c == '\\' && i + 1 < text.size()) {
            current.push_back(text[i + 1]);
            pending = true;
            i += 2;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (pending || !current.empty()) {
                out.push_back(current);
                current.clear();
                pending = false;
            }
            ++i;
        } else {
            current.push_back(c);
            pending = true;
            ++i;
        }
    }
    if (pending || !current.empty()) out.push_back(current);

    if (balanced) *balanced = !(in_single || in_double);
    return out;
}

bool is_executable(const fs::path& p) {
    return ::access(p.c_str(), X_OK) == 0;
}

void make_executable(const fs::path& p) {
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                           fs::perms::others_read | fs::perms::others_exec,
                    ec);
    if (ec) throw fs::filesystem_error("chmod", p, ec);
}

fs::path resolve_path(const fs::path& p) {
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(p, ec);
    if (ec) return p;
    return resolved;
}

bool is_within(const fs::path& child, const fs::path& parent) {
    fs::path::const_iterator c = child.begin();
    for (fs::path::const_iterator par = parent.begin(); par != parent.end(); ++par) {
        if (c == child.end() || *c != *par) return false;
        ++c;
    }
    return true;
}

void move_path(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return;

    // Different filesystem: copy then remove (shutil.move behaviour).
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) throw fs::filesystem_error("move (copy)", from, to, ec);
    fs::remove_all(from, ec);
    if (ec) throw fs::filesystem_error("move (cleanup)", from, ec);
}

std::string lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool is_all_digits(const std::string& s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string human_size(double num_bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double n = num_bytes;
    for (int i = 0; i < 5; ++i) {
        if (n < 1024.0 || i == 4) {
            char buffer[64];
            if (i == 0) {
                std::snprintf(buffer, sizeof(buffer), "%lld %s",
                              static_cast<long long>(n), units[i]);
            } else {
                std::snprintf(buffer, sizeof(buffer), "%.1f %s", n, units[i]);
            }
            return std::string(buffer);
        }
        n /= 1024.0;
    }
    return std::to_string(static_cast<long long>(num_bytes)) + " B";
}


// ---- waiting for a helper program (wineboot and friends) ----

void terminate_child(pid_t child, int grace_ms) {
    if (child <= 0) return;
    kill(child, SIGTERM);
    for (int waited = 0; waited < grace_ms; waited += 50) {
        if (kill(child, 0) != 0) return;  // gone
        usleep(50 * 1000);
    }
    kill(child, SIGKILL);
}

bool run_captured(const std::vector<std::string>& args, const EnvMap& extra_env,
                  int* exit_code, std::string* output, int* err) {
    if (err) *err = 0;
    if (exit_code) *exit_code = -1;
    if (args.empty()) {
        if (err) *err = EINVAL;
        return false;
    }

    EnvMap env = clean_env();
    for (const auto& kv : extra_env) env[kv.first] = kv.second;
    EnvBlock block(env);

    // stdout and stderr share one pipe: the caller gets the messages in the
    // order they were written, and the pipe is drained here so a chatty child
    // can never block on a full buffer.
    int fds[2];
    if (pipe(fds) != 0) {
        if (err) *err = errno;
        return false;
    }

    const pid_t child = fork();
    if (child < 0) {
        const int e = errno;
        close(fds[0]);
        close(fds[1]);
        if (err) *err = e;
        return false;
    }

    if (child == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        if (fds[1] > STDERR_FILENO) close(fds[1]);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvpe(argv[0], argv.data(), block.data());
        _exit(127);  // could not exec: 127 is the shell's "command not found"
    }

    close(fds[1]);
    std::string collected;
    char buffer[4096];
    ssize_t got = 0;
    while ((got = read(fds[0], buffer, sizeof(buffer))) > 0) {
        collected.append(buffer, static_cast<size_t>(got));
    }
    close(fds[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (output) *output = collected;

    if (exit_code) {
        if (WIFEXITED(status)) {
            *exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            *exit_code = 128 + WTERMSIG(status);
        } else {
            *exit_code = -1;
        }
    }
    return true;
}

}  // namespace util
